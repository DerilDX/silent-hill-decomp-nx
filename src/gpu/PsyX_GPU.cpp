#include "PsyX_GPU.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"

#include "../PsyX_main.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <SDL_timer.h>
#include <algorithm>

#include "psx/gtereg.h"

extern "C" int GR_NeedViewSpaceData(void);
/* 1 = this frame draws the 3D world in perspective; see PsyX_render.cpp. */
extern "C" int g_PsxFrame3dClass;

#define GET_TPAGE_FORMAT(tpage) ((TexFormat)((tpage >> 7) & 0x3))
#define GET_TPAGE_BLEND(tpage)  ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage) ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)        ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)        (clut >> 6)

OT_TAG prim_terminator = { (uintptr_t)-1, 0 }; // P_TAG with zero primLength

extern "C" void GR_CaptureFrameToVramRect(int x, int y, int w, int h);
int g_currentOTBucketCount = 0;
float g_otBucketDepth = 0.0f;

/* The OT bucket the game fills with 2D screen-space strings, or -1.
 *
 * The frame-class 3D marker below assumes a POLY on a 3D frame is world
 * geometry, because the 2D content the game orders is SPRT and TILE. Scaled
 * text breaks that assumption on purpose: a SPRT draws texels 1:1 and cannot be
 * resized, so enlarged glyphs are emitted as POLY_FT4 instead -- and were then
 * filtered as if they were a wall, which is what made big subtitles look soft
 * and swollen in the world while the same text stayed crisp on a menu frame.
 * The port names that bucket while the text is scaled; -1 means business as
 * usual. Absolute index, i.e. the port's own OT tag index. */
extern "C" { int g_PsxUi2dBucket = -1; }

/* Absolute index of the bucket being walked (the walk runs far->near). */
static int s_otBucketAbs = -1;

static int OtBucketIsUi2d(void)
{
	return g_PsxUi2dBucket >= 0 && s_otBucketAbs == g_PsxUi2dBucket;
}

/* ----------------------------------------------------------------------------
 * PGXP (perspective-correct rendering) — shadow-memory model, DuckStation-faithful.
 *
 * One shadow table parallels PSX memory: each entry is keyed by the NATIVE
 * ADDRESS of a vertex word (the packed s16 x | s16 y<<16 the GPU reads) and
 * holds the precise float screen X/Y + view W the GTE produced for that word,
 * the integer `value` it shadows (validation, never the key) and the frame
 * generation. Coverage is built by propagation along the data path, never by
 * heuristics:
 *   - GTE store (gte_stsxy*)               -> Shadow_Store(destAddr, fx,fy,w, value)
 *   - drawer copy (poly->xN = screenXy[f]) -> Shadow_Copy(&poly->xN, &screenXy[f])
 *   - GPU draw   (MakeVertex)              -> GetPreciseVertex(primFieldAddr, value, ...)
 * A vertex is either propagated (precise) or absent (clean affine, ppw=0). No
 * ring, no parked set, no nearest-match, no weld — those collide and oscillate.
 * Seams vanish for free: both bone-joint verts are tracked end-to-end and project
 * to the same precise value, so they coincide. All work is gated by g_PsxUsePgxp;
 * the off path is byte-identical to the legacy affine path.
 * -------------------------------------------------------------------------- */

/* Frame generation: bumped once per frame (PGXP_CoverageTick) so a shadow entry
 * left at a packet address reused by a later frame is rejected on lookup. */
static unsigned s_pgxpGen = 1;
extern "C" void PGXP_BumpGen(void) { s_pgxpGen++; }
/* The exact-transform twin tables (PsyX_GTE.cpp) gen-stamp against this same
 * per-frame counter so they share one "clear" and never allocate. */
extern "C" unsigned PGXP_CurGen(void) { return s_pgxpGen; }

/* Shadow entry: precise projection of the word at `key`. value = the packed
 * integer (s16 x | s16 y<<16) that lives at key; a draw that reused the address
 * with a different value falls to affine. */
struct ShadowEntry { uintptr_t key; unsigned gen; unsigned value; float x, y, w; };
/* Must hold every projected vertex word AND every copied prim-field word for one
 * frame (~230k verts -> up to ~1M words). 2^21 open-addressed, 16-probe. */
#define SHADOW_BITS 21
#define SHADOW_SIZE (1u << SHADOW_BITS)
#define SHADOW_MASK (SHADOW_SIZE - 1u)
static ShadowEntry s_shadow[SHADOW_SIZE];

/* Blocked scatter. A fully address-ordered slot (k >> 2) streamed through
 * cache, but the scratch area and the packet arenas alias modulo the table
 * span: whole runs of consecutive words piled onto the same slots, the 16-step
 * linear probe overflowed, live entries were evicted, and those vertices fell
 * to integer coordinates (sidewalk and wall strips splitting while the camera
 * moved). A plain per-word scatter is correct but puts every access on a
 * random page, which made the whole-map OT parse CPU-bound.
 *
 * So: each 64-byte block of addresses (16 words) owns one 16-slot bucket at a
 * scattered position, and its words sit in that bucket in address order. A
 * probe steps a whole bucket, so two blocks either share a bucket or never
 * touch -- runs cannot overlap part-way. Neighbouring words stay on one or two
 * cache lines; distinct blocks still scatter like the per-word hash. */
#define SHADOW_BLOCK_SHIFT 4
#define SHADOW_BUCKETS_BITS (SHADOW_BITS - SHADOW_BLOCK_SHIFT)
static inline unsigned ShadowBucket(uintptr_t k) {
	unsigned b = (unsigned)((k >> 6) ^ ((uint64_t)k >> 38));
	return ((b * 2654435761u) >> (32 - SHADOW_BUCKETS_BITS)) << SHADOW_BLOCK_SHIFT;
}
static inline unsigned ShadowSlot(uintptr_t k, unsigned bucket, int probe) {
	return (bucket + ((unsigned)probe << SHADOW_BLOCK_SHIFT) + (unsigned)((k >> 2) & 15u)) & SHADOW_MASK;
}

/* One-shot: a full probe means an entry was evicted, i.e. a vertex lost its
 * precise position. Should never print; if it does, seams are back. */
static int s_shadowOverflowWarned = 0;
static void Shadow_WarnOverflow(const char* table) {
	if (!s_shadowOverflowWarned) {
		s_shadowOverflowWarned = 1;
		eprintwarn("[PGXP] %s shadow table probe exhausted; precise vertices are being evicted\n", table);
	}
}

static void Shadow_Put(void* addr, float x, float y, float w, unsigned value) {
	uintptr_t k = (uintptr_t)addr;
	unsigned b = ShadowBucket(k);
	for (int i = 0; i < 16; i++) {
		ShadowEntry* e = &s_shadow[ShadowSlot(k, b, i)];
		if (e->key == k || e->key == 0 || e->gen != s_pgxpGen) {
			e->key = k; e->gen = s_pgxpGen; e->value = value;
			e->x = x; e->y = y; e->w = w; return;
		}
	}
	Shadow_WarnOverflow("position");
	ShadowEntry* e = &s_shadow[ShadowSlot(k, b, 0)];   /* probe exhausted: overwrite base */
	e->key = k; e->gen = s_pgxpGen; e->value = value;
	e->x = x; e->y = y; e->w = w;
}

/* A miss may stop at the first stale-gen slot, not only an empty one: Put takes
 * the first slot that is empty, ours or stale, and a slot stamped this gen never
 * goes stale within it, so every slot ahead of a live entry is live. The tables
 * are never cleared, so stopping only at key 0 walked all 16 slots per miss. */
static const ShadowEntry* Shadow_Get(const void* addr) {
	uintptr_t k = (uintptr_t)addr;
	unsigned b = ShadowBucket(k);
	for (int i = 0; i < 16; i++) {
		const ShadowEntry* e = &s_shadow[ShadowSlot(k, b, i)];
		if (e->key == k) return (e->gen == s_pgxpGen) ? e : nullptr;
		if (e->key == 0 || e->gen != s_pgxpGen) return nullptr;
	}
	return nullptr;
}

/* GTE store hook (DuckStation SWC2, done at source level): record the precise
 * projection of the word just written to `addr`. Called from the gte_stsxy*
 * macros via PGXP_StoreAddr, which reads the integer value back from addr. */
extern "C" void Shadow_Store(void* addr, float x, float y, float w, unsigned value) {
	Shadow_Put(addr, x, y, w, value);
}

/* ---- View-space shadow for the per-pixel flashlight -------------------------
 * A second table parallel to s_shadow, keyed the same way (vertex-word native
 * address) and gen-stamped with the same s_pgxpGen (bumped every frame in
 * PGXP_CoverageTick regardless of flags). It holds the GTE RTPS camera-space
 * position of each projected vertex and is propagated along the SAME copy path
 * as PGXP (Shadow_Copy below). Entirely gated by g_PsyX_UsePerPixelFlashlight;
 * the off path never reads or writes it. */
/* Per-vertex "does not cast a flashlight shadow" flag (see g_PsyX_UseFlashlightShadows).
 * Set by game code (world_draw.c) around Harry's skeleton draw so the player never
 * shadows the scene he's standing in; rides the same address-keyed view-space FIFO as
 * the position and lands in GrVertex.nx, read by the shadow depth shader. Default 0:
 * everything casts (props included); only Harry is suppressed. (The prop shadow
 * projects from the hand-height flashlight so it can look like a silhouette growing
 * off the object up close in first person — acceptable; shadows have their own
 * on/off, and it isn't noticeable at third-person camera distances.) */
extern "C" int g_PsyX_NoShadowCast = 0;
extern "C" float g_PsyX_CharaFade = 0.0f;

/* Like the PGXP ShadowEntry, each entry records the packed integer `value` of the
 * vertex word it shadows. A lookup whose current word differs falls to "untracked":
 * without this, a prim whose XY was written by the CPU (muzzle-flash quads etc.)
 * into a packet-buffer slot that a GTE-projected vertex used earlier in the same
 * frame inherited THAT vertex's view-space position — the shadow depth pass then
 * drew the quad at the stale position (the glitchy arm/gun shadow while firing). */
/* ofx/ofy/h are the GTE projection registers ACTIVE WHEN THIS VERTEX WAS
 * TRANSFORMED. They must ride the shadow rather than sit in a frame-global: the
 * near clipper consumes them at DrawOTag time, long after every RTPS of the
 * frame has run, and SH1 does not hold them constant across a frame — e.g.
 * bodyprog_80055028.c:1039 sets SetGeomOffset(-1024,-1024)/SetGeomScreen(16) for
 * a lighting helper and restores only the GTE registers, and the various drawers
 * each push their own SetGeomScreen. A frame-global therefore reprojects clip
 * vertices with whichever values the LAST RTPS of the frame happened to leave. */
struct VsEntry { uintptr_t key; unsigned gen; unsigned value; float vx, vy, vz; float nocast; float fade; float ofx, ofy, h; };
static VsEntry s_vshadow[SHADOW_SIZE];
/* With PGXP and the flashlight off nothing stores view-space data, yet texture
 * filtering alone keeps GR_NeedViewSpaceData() true, so every vertex copy
 * probed this 96 MB table for a hit that cannot exist this gen. */
static unsigned s_vsPutGen = 0;

static void Vs_Put(void* addr, float vx, float vy, float vz, float nocast, unsigned value,
                   float ofx, float ofy, float h, float fade = 0.0f) {
	uintptr_t k = (uintptr_t)addr;
	unsigned b = ShadowBucket(k);
	s_vsPutGen = s_pgxpGen;
	for (int i = 0; i < 16; i++) {
		VsEntry* e = &s_vshadow[ShadowSlot(k, b, i)];
		if (e->key == k || e->key == 0 || e->gen != s_pgxpGen) {
			e->key = k; e->gen = s_pgxpGen; e->value = value; e->vx = vx; e->vy = vy; e->vz = vz; e->nocast = nocast;
			e->fade = fade; e->ofx = ofx; e->ofy = ofy; e->h = h; return;
		}
	}
	Shadow_WarnOverflow("view-space");
	VsEntry* e = &s_vshadow[ShadowSlot(k, b, 0)];
	e->key = k; e->gen = s_pgxpGen; e->value = value; e->vx = vx; e->vy = vy; e->vz = vz; e->nocast = nocast;
	e->fade = fade; e->ofx = ofx; e->ofy = ofy; e->h = h;
}

static const VsEntry* Vs_Get(const void* addr, unsigned value) {
	uintptr_t k = (uintptr_t)addr;
	unsigned b = ShadowBucket(k);
	if (s_vsPutGen != s_pgxpGen)
		return nullptr;
	for (int i = 0; i < 16; i++) {
		const VsEntry* e = &s_vshadow[ShadowSlot(k, b, i)];
		if (e->key == k) return (e->gen == s_pgxpGen && e->value == value) ? e : nullptr;
		if (e->key == 0 || e->gen != s_pgxpGen) return nullptr;
	}
	return nullptr;
}

/* GTE store hook for the flashlight view-space FIFO (PsyX_GTE.cpp PGXP_StoreAddr,
 * fired when g_PsyX_UsePerPixelFlashlight). The packed vertex word is already at
 * addr when the hook fires (same contract as PGXP's Shadow_Store). */
extern "C" void VShadow_Store(void* addr, float vx, float vy, float vz, float ofx, float ofy, float h) {
	Vs_Put(addr, vx, vy, vz, g_PsyX_NoShadowCast ? 1.0f : 0.0f, *(const unsigned*)addr, ofx, ofy, h,
	       g_PsyX_CharaFade);
}

/* Drawer copy hook (DuckStation CPU MOVE/SW): the game just did *dst = *src (a
 * vertex word moving from a GTE scratch slot into a prim field). Propagate the
 * shadow along the same path so the GPU resolves the prim-field address. If src
 * isn't tracked, leave dst absent -> clean affine. The PGXP and flashlight
 * shadows are independently gated, so each is byte-identical when its flag is
 * off and both off => an immediate return (legacy path). */
extern "C" void Shadow_Copy(void* dst, const void* src) {
	if (g_PsxUsePgxp) {
		const ShadowEntry* e = Shadow_Get(src);
		if (e) Shadow_Put(dst, e->x, e->y, e->w, *(const unsigned*)dst);
	}
	/* View-space propagates whenever PGXP is on too — the near-plane clipper
	 * needs it (gate matches the vs FIFO / VShadow_Store in PsyX_GTE.cpp). */
	if (GR_NeedViewSpaceData()) {
		const VsEntry* ve = Vs_Get(src, *(const unsigned*)src);
		if (ve) Vs_Put(dst, ve->vx, ve->vy, ve->vz, ve->nocast, *(const unsigned*)dst,
		               ve->ofx, ve->ofy, ve->h, ve->fade);
	}
}

/* Max |precise screen coord| (PSX units) PGXP will use before clamping the POSITION
 * for GPU guard-band safety (W is kept either way, so the vertex stays perspective).
 * Higher = less texture compression at the extreme screen edge (more visible in 16:9
 * Hor+), at the cost of larger off-screen NDC. Live-tunable via console `pgxpedge`. */
extern "C" { float g_PgxpEdgeMax = 8192.0f; }

/* [PGXP-SPIKE] diagnostic: counts per-frame how many precise vertices project
 * PAST the guard band (|screen coord| > g_PgxpEdgeMax) and get position-clamped.
 * A clamped precise vertex is a bounded-but-visible thin spike (the clamp is a
 * guard band, not a true clip; the ortho shader path makes screen pos = ppx/ppy
 * with W cancelling, so a large ppx IS a spike). Reported on the [PGXP] cov line.
 * If the Nowhere blue-spike/black-wedge frames show clamp>0 while clean frames
 * show 0, the near-camera precise-projection is the source. Pure observation. */
static unsigned int s_pgxpClamp = 0;

/* PGXP perspective-W precision. 1 (default) = use the unquantized GTE accumulator
 * (gte_shift(m_mac3,1)) for the shader's per-vertex W instead of the clamped 16-bit
 * SZ3 register; coincident edges then get a matching 1/W so seams stop widening with
 * distance. 0 = original quantized SZ3 (for A/B). Live-tunable via console `pgxpdepth`. */
extern "C" { int g_PgxpUseUnquantizedDepth = 1; }

/* Whole-town render mode flag (docs/WholeMap_Far_Projection_Task.md). Set by the
 * game to Pc_WholeMapDrawActive() around the world-chunk draw so GTE_RotTransPers
 * re-projects far vertices from unclamped view coords. 0 = legacy path untouched. */
extern "C" { int g_PsxWholeMapFar = 0; }
/* True (unclamped) SZ of the newest RTPS while the whole-map flag is set; the
 * game reads it right after RotTransPers for far billboard sizing. */
extern "C" { int g_PsxWholeMapLastSz = 0; }

/* PGXP far-W clamp (u_pgxpFarW, SZ units; 256 = 1 world unit; 0 = disabled).
 * Beyond this view depth the per-vertex W stops growing, so varying
 * interpolation converges to AFFINE — the PSX-authentic distant look. Fixes
 * the "distant road looks worse with PGXP on" report: perspective-correct UVs
 * at grazing angles sample the compressed horizon gradient with no mipmaps ->
 * texel shimmer/moire, where affine smears smoothly. Near geometry (< clamp)
 * keeps full perspective correction. NDC positions are unaffected (W scales
 * clip coords homogeneously). Console `PGXPFARW <sz>` tunes it live. */
extern "C" { float g_PgxpFarWClamp = 12288.0f; } /* ~48 world units */

/* PGXP near-plane clipping (docs/PGXP_NearClip_Design.md). A poly that straddles
 * the camera plane has behind-the-eye vertices with no valid projection (SZ3==0 ->
 * W=0); PSX hardware — and this port until now — fell back to affine for them,
 * mixing per-vertex modes across the poly and smearing it whenever the FPS camera
 * leans into geometry. When on, such polys are clipped against z=NEAR in view
 * space (positions from the VsEntry shadow, which PGXP now also fills) and the
 * clip vertices re-projected with the GTE's own projection constants. OFF path is
 * byte-identical to before. Console `pgxpnearclip` / `pgxpnearz`. */
extern "C" { int g_PsxPgxpNearClip = 1; }
/* Clip plane view-space depth, GTE SZ units. Small enough to be an invisible cut
 * right at the eye, large enough that H/z and 1/W stay numerically tame. */
extern "C" { float g_PgxpNearZ = 16.0f; }
/* GTE projection registers of the most recent RTPS (PsyX_GTE.cpp): OFX/OFY as
 * float pixels, H the projection distance. Fallback only — the near clipper
 * prefers the per-vertex copies carried in the VsEntry shadow, because these
 * are consumed at DrawOTag time and SH1 changes the registers mid-frame. */
extern "C" { float g_PgxpGteOfx = 0.0f, g_PgxpGteOfy = 0.0f, g_PgxpGteH = 1.0f; }

/* Projection registers belonging to the poly currently being built, latched by
 * VsFillVertex from the shadow entry of each vertex it resolves. Valid flag
 * distinguishes "no vertex of this poly was tracked" (fall back to the globals)
 * from a genuine zero offset. */
static float s_curPolyOfx = 0.0f, s_curPolyOfy = 0.0f, s_curPolyH = 1.0f;
static int   s_curPolyProjValid = 0;

/* PSX GPU polygon size rule (PSXSPX): the rasterizer silently rejects any
 * triangle whose screen bounding box exceeds 1023 (w) x 511 (h). PsyX never had
 * the rule, so GTE-saturated (+-0x400 box) or guard-band-dropped polys drew as
 * screen-crossing wedges (elevator doors / spiral staircase) instead of
 * vanishing for the frame like on hardware. Enforced per-triangle on the
 * integer/affine path only (PgxpEmitPoly below); never in whole-map far mode,
 * where distant town geometry saturates to the box by design. Console
 * `polysizecull` / config `psx_poly_size_cull` is the escape hatch. */
extern "C" { int g_PsxPolySizeCull = 1; }
/* Triangles culled by the size rule, reported as oversize= on the [PGXP] cov
 * line so future user logs self-diagnose the wedge class. */
static unsigned int s_pgxpOversize = 0;

/* GPU draw resolve (DuckStation GetPreciseVertex): shadow at the prim-field
 * address, validated by exact value. Miss / behind-near-plane (W=0) -> affine
 * (ppw=0). rawX/rawY = the integer in the field; ofsX/ofsY = draw-env offset
 * added to land in vertex.x/.y space. */
static inline bool GetPreciseVertex(const void* addr, unsigned value, int rawX, int rawY,
                                    float ofsX, float ofsY, float* ox, float* oy, float* ow) {
	const ShadowEntry* e = Shadow_Get(addr);
	if (e && e->value == value && e->w > 0.0f) {
		/* Keep EVERY valid (W>0) vertex on the perspective path. The warp at the screen
		 * edge is a MIXED polygon: some verts perspective (PGXP), some affine -- the
		 * interpolation across the poly is then inconsistent and smears right where the
		 * affine verts are (just off the 4:3 / 16:9 edge). Rejecting off-screen verts to
		 * affine (what the old +-2px / magnitude-bound code did) CREATES that mix. Instead
		 * use the precise coord so the whole poly is consistently perspective-correct.
		 *
		 * Clamp only for guard-band safety: geometry very close to the camera and off to
		 * the side projects to tens of thousands of units, and such extreme positions
		 * stretch under rasterization. Clamp the POSITION but KEEP W>0 so the vertex stays
		 * perspective (no affine mix). PGXP_OFFSCREEN_MAX is well past the visible width so
		 * the on-screen + just-off-screen geometry (the part that matters) is exact.
		 *
		 * Only W=0 verts -- behind / at the near plane (no valid projection), set in
		 * PsyX_GTE.cpp -- fall through to affine below. */
		const float m = g_PgxpEdgeMax;
		/* Past the guard band the precise projection is a near-camera grazing
		 * vertex whose TRUE screen coord is enormous. The old code clamped the
		 * POSITION to +/-m but kept the small true W — so the GPU interpolated a
		 * thin spike from the on-screen verts toward that clamp point (the Nowhere
		 * blue-spike / black-wedge report; [PGXP-SPIKE] spikeclamp confirmed it
		 * fires 300-747x/60f on the artifact frames). Drop this vertex to affine
		 * instead: PgxpFillVertex's whole-poly consistency then renders the poly
		 * with the GTE integer coords, which the GTE already saturates to the
		 * +/-0x400 screen box (PsyX_GTE.cpp) — bounded, PSX-authentic, no spike.
		 * Only verts ALREADY being clamped change; |proj| <= m is byte-identical. */
		if (e->x < -m || e->x > m || e->y < -m || e->y > m) {
			s_pgxpClamp++;
			*ox = (float)rawX + ofsX; *oy = (float)rawY + ofsY; *ow = 0.0f; return false;
		}
		*ox = e->x + ofsX; *oy = e->y + ofsY; *ow = e->w; return true;
	}
	*ox = (float)rawX + ofsX; *oy = (float)rawY + ofsY; *ow = 0.0f; return false;
}

/* Precise backface test for the lit-character drawer. The game's gte_nclip runs
 * on the rounded 16-bit screen coords; at distance the cross product of small
 * integers flips sign on near-edge-on faces -> faces get false-culled and the
 * model sheds chunks far away (waist/silhouette first). Recompute the cross
 * product from the PGXP precise float projection (keyed by the screenXy address,
 * the same address SH_PGXP_PROP copies from). Returns 1 = backface (cull), 0 =
 * frontface (keep); falls back to the GTE integer sign when PGXP is off or a
 * vertex isn't tracked. The offset cancels in the cross product, so pass 0. */
static inline bool Pgxp_FetchXY(const void* a, float* x, float* y) {
	float w;
	return GetPreciseVertex(a, *(const unsigned*)a, 0, 0, 0.0f, 0.0f, x, y, &w);
}

extern "C" int PsyX_PGXP_TriBackface(const void* a0, const void* a1, const void* a2, int intNcl)
{
	if (g_PsxUsePgxp) {
		float x0,y0, x1,y1, x2,y2;
		if (Pgxp_FetchXY(a0,&x0,&y0) && Pgxp_FetchXY(a1,&x1,&y1) && Pgxp_FetchXY(a2,&x2,&y2)) {
			float ncl = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
			return ncl <= 0.0f ? 1 : 0;
		}
	}
	return intNcl <= 0 ? 1 : 0;
}

/* Quad = two triangles; the game culls only if BOTH are backfacing (n012<=0 AND
 * n312>=0, opposite winding on the 2nd). Same precise/integer fallback. */
/* Console `wholemap depth 0|1`. Off by default: the town's streets are coplanar
 * layers (asphalt over a lighter base) that only the painter order stacks
 * correctly, and per-vertex depth interpolation picks a different winner per
 * tile, which tiled the far road white. Painter order is right within a chunk,
 * and the chunks themselves are submitted far-to-near. */
extern "C" { int g_PsxWholeMapFarDepth = 0; }
/* [WMPROBE] one-shot (console `wholemap probe`): counts one frame of far-band
 * prims through backface, capture and parse, plus a few per-vertex samples.
 * 2 = armed, counts during the next frame; 1 = dump at the following tick. */
extern "C" { int g_PsxWholeMapProbe = 0; }
static unsigned s_wmLastPrims, s_wmLastAffine, s_wmLastHit;
static unsigned s_wmDevPrims, s_wmPrecPrims;
/* Probe-frame CPU time (ms) by stage, and batch volume. */
extern "C" { double g_wmTParse = 0, g_wmTDraw = 0, g_wmTChunks = 0, g_wmTXform = 0, g_wmTMesh = 0; unsigned g_wmSplits = 0, g_wmVerts = 0, g_wmOtDraws = 0; }
static Uint64 s_wmFrameStart = 0;
/* Probe-frame cycle counts per GT4 parse step (world prims are GT4). */
/* The rate is calibrated against SDL each probe frame, so any monotonic
 * counter serves where there is no TSC (arm64 macOS). */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <x86intrin.h>
#define WM_TICKS() __rdtsc()
#else
#define WM_TICKS() SDL_GetPerformanceCounter()
#endif
static unsigned long long s_wmCyc[9];
static unsigned s_wmGt4;
/* Probe-frame parse cost by prim class: [code>>4], 16 = 0xB5 far quads,
 * 17 = mid-parse vertex-buffer flushes. TSC rate is calibrated against the
 * SDL counter over the probe frame to report milliseconds. */
static unsigned long long s_wmClsCyc[18];
static unsigned s_wmClsN[18];
static unsigned long long s_wmTscStart;
extern "C" { double g_wmTGpu = 0; unsigned g_wmFarQuads = 0, g_wmFarSplits = 0, g_wmFarFlushes = 0, g_wmFarMeshKeys = 0, g_wmFarMeshDraws = 0, g_wmFarMeshQueued = 0; }
#define WM_CLS(i, stmt) do { if (g_PsxWholeMapProbe == 1) { unsigned long long _c0 = WM_TICKS(); stmt; s_wmClsCyc[i] += WM_TICKS() - _c0; s_wmClsN[i]++; } else { stmt; } } while (0)
#define WM_CYC(i, stmt) do { if (g_PsxWholeMapProbe == 1) { unsigned long long _c0 = WM_TICKS(); stmt; s_wmCyc[i] += WM_TICKS() - _c0; } else { stmt; } } while (0)
static unsigned s_wmBfCalls, s_wmBfFallback, s_wmBfCull, s_wmCaptured, s_wmParsed, s_wmAffineVerts, s_wmPreciseVerts;
static float s_wmWMin, s_wmWMax;
static unsigned char s_lastLookupFar;

extern "C" int PsyX_PGXP_QuadBackface(const void* a0, const void* a1, const void* a2, const void* a3,
                                      int intN012, int intN312)
{
	if (g_PsxUsePgxp) {
		float x0,y0, x1,y1, x2,y2, x3,y3;
		if (Pgxp_FetchXY(a0,&x0,&y0) && Pgxp_FetchXY(a1,&x1,&y1) &&
		    Pgxp_FetchXY(a2,&x2,&y2) && Pgxp_FetchXY(a3,&x3,&y3)) {
			float n012 = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
			float n312 = (x1-x3)*(y2-y3) - (x2-x3)*(y1-y3);
			int cull = (n012 <= 0.0f && n312 >= 0.0f) ? 1 : 0;
			if (g_PsxWholeMapProbe == 1) { s_wmBfCalls++; s_wmBfCull += cull; }
			return cull;
		}
	}
	if (g_PsxWholeMapProbe == 1) { s_wmBfCalls++; s_wmBfFallback++; s_wmBfCull += (intN012 <= 0 && intN312 >= 0); }
	return (intN012 <= 0 && intN312 >= 0) ? 1 : 0;
}

/* Coverage instrumentation: precise (det) vs affine (miss) per 3D vertex, dumped
 * ~once a second when PGXP is on. Also bumps the frame generation. */
static unsigned int s_pgxpDet = 0, s_pgxpMiss = 0, s_pgxpFrames = 0, s_pgxpClip = 0;

/* [PGXPDEPTH] Step-1 probe (temporary; depth-channel plan). Answers, at runtime:
 * does the GsDrawOt-start wipe discard the frame's captured SZ entries BEFORE
 * the parse reads them (i.e. is the FLAT/WORLD machinery inert in gameplay),
 * and is the whole-town vista ordered without parse hits (sort-time ordering)?
 * Dumped with the [PGXP] coverage line once per 60 frames, PGXP-on only. */
static unsigned s_dbgWipeFlat = 0, s_dbgWipeExact = 0, s_dbgWipeNone = 0, s_dbgWipeSkips = 0;
static unsigned s_dbgParseHit = 0, s_dbgParseMiss = 0;
static unsigned s_dbgParseHitFlat = 0, s_dbgParseHitExact = 0, s_dbgParseHitNone = 0;
static unsigned s_dbgSplitWorld = 0, s_dbgSzExhaust = 0;
static int s_dbgSplitHighWater = 0;
/* Step-4 anomaly hunt: split "parse not reached" from "entry rejected".
 * armF   = PsyX_SetNextPrimSz calls (world polys armed at build)
 * capF   = captures that actually stored kind FLAT
 * apply  = ApplyGtePerVertexDepth entries (parse-side call volume)
 * staleR = gen-stale rejects across both readers (lookup + split classify) */
static unsigned s_dbgArmF = 0, s_dbgCapF = 0, s_dbgApplyCalls = 0, s_dbgStaleRej = 0;
/* Console PGXPDEPTHSTATS: the depth channel is verified live (2026-07-27 run:
 * armF~350k/s capF~100k/s hit F==splitWORLD, staleR<400/s, splitHW 794/4096,
 * szExhaust 0, no shift disagreement) — the dump is now opt-in diagnostics. */
extern "C" { int g_PsxPgxpDepthStats = 0; }
extern "C" void PGXP_CoverageTick(void)
{
	PGXP_BumpGen();
	if (g_PsxWholeMapProbe == 1)
	{
		eprintinfo("[WMPROBE] time: frame=%.2fms chunkSubmit=%.2fms parse=%.2fms drawSplits=%.2fms (of submit: transform=%.2fms meshEmit=%.2fms) | OT draws=%u splits=%u verts=%u\n",
			(double)(SDL_GetPerformanceCounter() - s_wmFrameStart) * 1000.0 / (double)SDL_GetPerformanceFrequency(),
			g_wmTChunks, g_wmTParse, g_wmTDraw, g_wmTXform, g_wmTMesh, g_wmOtDraws, g_wmSplits, g_wmVerts);
		{
			const double frameMs = (double)(SDL_GetPerformanceCounter() - s_wmFrameStart) * 1000.0 / (double)SDL_GetPerformanceFrequency();
			const double cycPerMs = (frameMs > 0.0) ? (double)(WM_TICKS() - s_wmTscStart) / frameMs : 1.0;
			char buf[1024];
			int len = 0;
			for (int c = 0; c < 18; c++)
			{
				if (!s_wmClsN[c]) continue;
				len += snprintf(buf + len, sizeof(buf) - len, " %s%X=%u/%.2fms",
					c == 16 ? "far" : (c == 17 ? "flush" : "0x"), c >= 16 ? 0 : c * 16, s_wmClsN[c], (double)s_wmClsCyc[c] / cycPerMs);
				if (len >= (int)sizeof(buf) - 64) break;
			}
			eprintinfo("[WMPROBE] parse by class (count/ms):%s | farQuads=%u farSplits=%u farFlushes=%u | gpuMeshes=%u keys=%u draws=%u | gpuFinish=%.2fms%c", buf, g_wmFarQuads, g_wmFarSplits, g_wmFarFlushes, g_wmFarMeshQueued, g_wmFarMeshKeys, g_wmFarMeshDraws, g_wmTGpu, 10);
		}
		if (s_wmGt4)
			eprintinfo("[WMPROBE] GT4 cycles/prim over %u: begin=%llu drop=%llu hires=%llu split=%llu vert=%llu depth=%llu tex=%llu col=%llu emit=%llu\n",
				s_wmGt4, s_wmCyc[0] / s_wmGt4, s_wmCyc[1] / s_wmGt4, s_wmCyc[2] / s_wmGt4, s_wmCyc[3] / s_wmGt4,
				s_wmCyc[4] / s_wmGt4, s_wmCyc[5] / s_wmGt4, s_wmCyc[6] / s_wmGt4, s_wmCyc[7] / s_wmGt4, s_wmCyc[8] / s_wmGt4);
		eprintinfo("[WMPROBE] frame: backface calls=%u intFallback=%u culled=%u | far captured=%u parsed=%u | verts precise=%u affine=%u W=%.0f..%.0f | farDepth=%d pgxp=%d | lastBucket prims=%u affine=%u szHit=%u | precise prims=%u offByOver2px=%u\n",
			s_wmBfCalls, s_wmBfFallback, s_wmBfCull, s_wmCaptured, s_wmParsed, s_wmPreciseVerts, s_wmAffineVerts,
			(double)s_wmWMin, (double)s_wmWMax, g_PsxWholeMapFarDepth, g_PsxUsePgxp,
			s_wmLastPrims, s_wmLastAffine, s_wmLastHit, s_wmPrecPrims, s_wmDevPrims);
		g_PsxWholeMapProbe = 0;
	}
	else if (g_PsxWholeMapProbe == 2)
	{
		s_wmBfCalls = s_wmBfFallback = s_wmBfCull = s_wmCaptured = s_wmParsed = s_wmAffineVerts = s_wmPreciseVerts = 0;
		s_wmWMin = s_wmWMax = 0.0f;
		s_wmLastPrims = s_wmLastAffine = s_wmLastHit = 0;
		s_wmDevPrims = s_wmPrecPrims = 0;
		memset(s_wmCyc, 0, sizeof(s_wmCyc)); s_wmGt4 = 0;
		memset(s_wmClsCyc, 0, sizeof(s_wmClsCyc)); memset(s_wmClsN, 0, sizeof(s_wmClsN));
		g_wmTGpu = 0; g_wmFarQuads = g_wmFarSplits = g_wmFarFlushes = g_wmFarMeshKeys = g_wmFarMeshDraws = g_wmFarMeshQueued = 0; s_wmTscStart = WM_TICKS();
		g_wmTParse = g_wmTDraw = g_wmTChunks = g_wmTXform = g_wmTMesh = 0; g_wmSplits = g_wmVerts = g_wmOtDraws = 0;
		s_wmFrameStart = SDL_GetPerformanceCounter();
		g_PsxWholeMapProbe = 1;
	}
	if (!g_PsxUsePgxp) { s_pgxpDet = s_pgxpMiss = s_pgxpClip = s_pgxpClamp = s_pgxpOversize = 0; return; }
	if (++s_pgxpFrames >= 60)
	{
		unsigned int tot = s_pgxpDet + s_pgxpMiss;
		if (tot && g_PsxPgxpDepthStats)
			eprintinfo("[PGXP] cov %uf: det=%u(%.0f%%) miss=%u(%.0f%%) clip=%u spikeclamp=%u oversize=%u\n",
				s_pgxpFrames,
				s_pgxpDet,  100.0 * (double)s_pgxpDet  / (double)tot,
				s_pgxpMiss, 100.0 * (double)s_pgxpMiss / (double)tot,
				s_pgxpClip, s_pgxpClamp, s_pgxpOversize);
		if (g_PsxPgxpDepthStats &&
		    s_dbgWipeFlat + s_dbgWipeExact + s_dbgWipeNone + s_dbgParseHit + s_dbgParseMiss + s_dbgArmF + s_dbgApplyCalls)
			eprintinfo("[PGXPDEPTH] wiped F=%u E=%u N=%u skips=%u | parse hit=%u(F=%u E=%u N=%u) miss=%u | splitWORLD=%u | splitHW=%d | szExhaust=%u | armF=%u capF=%u apply=%u staleR=%u\n",
				s_dbgWipeFlat, s_dbgWipeExact, s_dbgWipeNone, s_dbgWipeSkips,
				s_dbgParseHit, s_dbgParseHitFlat, s_dbgParseHitExact, s_dbgParseHitNone,
				s_dbgParseMiss, s_dbgSplitWorld, s_dbgSplitHighWater, s_dbgSzExhaust,
				s_dbgArmF, s_dbgCapF, s_dbgApplyCalls, s_dbgStaleRej);
		s_dbgWipeFlat = s_dbgWipeExact = s_dbgWipeNone = s_dbgWipeSkips = 0;
		s_dbgParseHit = s_dbgParseMiss = 0;
		s_dbgParseHitFlat = s_dbgParseHitExact = s_dbgParseHitNone = 0;
		s_dbgSplitWorld = s_dbgSzExhaust = 0;
		s_dbgSplitHighWater = 0;
		s_dbgArmF = s_dbgCapF = s_dbgApplyCalls = s_dbgStaleRej = 0;
		s_pgxpDet = s_pgxpMiss = s_pgxpFrames = s_pgxpClip = s_pgxpClamp = s_pgxpOversize = 0;
	}
}

extern "C" void PGXP_FrameReset(void) { /* shadow is gen-stamped; no reset needed */ }

/* ---- Per-prim affine flag (billboards) -------------------------------------
 * Screen-space prims (billboards, 2D/HUD) build their corners directly, never
 * through the GTE, so they have no shadow and naturally miss to affine. We mark
 * them explicitly too: PsyX_SetNextPrimAffine sets a pending flag, addPrim
 * (PsyX_CaptureGteDepths) records the prim pointer here, and the draw side reads
 * it to force affine. gen-stamped so a reused packet address from last frame is
 * rejected. */
/* noFl: keep this prim OUT of the per-pixel flashlight (force view-Z<=0 so the
 * shader's flP.z>0 gate skips it), same idea as g_PsyX_ForceItemDepth. The
 * reflective sewer-water octagon (water.c func_8008EA68) propagates a real
 * view-space shadow so its surface would be flashlight-lit, but under PGXP the
 * bright reflection albedo blows out white; opting it out makes it match the
 * Classic (no per-pixel) look at all times. Both flags PGXP-on only. */
struct AffineEntry { uintptr_t key; unsigned gen; unsigned char affine; unsigned char noFl; };
#define AFFINE_BITS 15
#define AFFINE_SIZE (1u << AFFINE_BITS)
#define AFFINE_MASK (AFFINE_SIZE - 1u)
static AffineEntry s_affine[AFFINE_SIZE];
static int g_primPgxpForceAffine = 0;
static int g_primNoFlashlight = 0;

extern "C" void PsyX_SetNextPrimAffine(void)
{
	if (!g_PsxUsePgxp) return;
	g_primPgxpForceAffine = 1;
}

extern "C" void PsyX_SetNextPrimNoFlashlight(void)
{
	if (!g_PsxUsePgxp) return;
	g_primNoFlashlight = 1;
}

static void AffineStore(const void* prim, unsigned char affine, unsigned char noFl) {
	uintptr_t key = (uintptr_t)prim;
	unsigned s = (unsigned)((key >> 2) * 2654435761u) & AFFINE_MASK;
	for (int i = 0; i < 16; i++) {
		AffineEntry* e = &s_affine[(s + i) & AFFINE_MASK];
		if (e->key == key || e->key == 0 || e->gen != s_pgxpGen) {
			e->key = key; e->gen = s_pgxpGen; e->affine = affine; e->noFl = noFl; return;
		}
	}
	s_affine[s].key = key; s_affine[s].gen = s_pgxpGen; s_affine[s].affine = affine; s_affine[s].noFl = noFl;
}

static const AffineEntry* AffineFind(const void* prim) {
	uintptr_t key = (uintptr_t)prim;
	unsigned s = (unsigned)((key >> 2) * 2654435761u) & AFFINE_MASK;
	for (int i = 0; i < 16; i++) {
		const AffineEntry* e = &s_affine[(s + i) & AFFINE_MASK];
		if (e->key == key) return (e->gen == s_pgxpGen) ? e : nullptr;
		if (e->key == 0 || e->gen != s_pgxpGen) return nullptr;
	}
	return nullptr;
}

/* ---- Per-pixel lighting side channel (PsyX_PpLitPrim) ----------------------
 * Keyed by prim address and stamped with the PGXP frame gen, exactly like the
 * affine table above: the game builds a prim, records its corner normals, and
 * the parse finds them when it emits that prim's vertices. */
extern "C" { int g_PsyX_PpLitActive = 0; float g_PsyX_HeroLight[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; }
struct PpLitEntry { uintptr_t key; unsigned gen; int kind; float n[12]; };
#define PPLIT_BITS 15
#define PPLIT_SIZE (1u << PPLIT_BITS)
#define PPLIT_MASK (PPLIT_SIZE - 1u)
static PpLitEntry s_ppLit[PPLIT_SIZE];

extern "C" void PsyX_PpLitPrim(const void* prim, int kind, const float* n)
{
	uintptr_t key = (uintptr_t)prim;
	unsigned s = (unsigned)((key >> 2) * 2654435761u) & PPLIT_MASK;
	PpLitEntry* e = &s_ppLit[s];
	for (int i = 0; i < 16; i++) {
		PpLitEntry* c = &s_ppLit[(s + i) & PPLIT_MASK];
		if (c->key == key || c->key == 0 || c->gen != s_pgxpGen) { e = c; break; }
	}
	e->key = key; e->gen = s_pgxpGen; e->kind = kind;
	memcpy(e->n, n, sizeof(e->n));
}

static const PpLitEntry* PpLitFind(const void* prim)
{
	uintptr_t key = (uintptr_t)prim;
	unsigned s = (unsigned)((key >> 2) * 2654435761u) & PPLIT_MASK;
	for (int i = 0; i < 16; i++) {
		const PpLitEntry* e = &s_ppLit[(s + i) & PPLIT_MASK];
		if (e->key == key) return (e->gen == s_pgxpGen) ? e : nullptr;
		if (e->key == 0 || e->gen != s_pgxpGen) return nullptr;
	}
	return nullptr;
}

/* Parallel to g_vertexBuffer, index for index. Vertices nothing lit are zero
 * (w = 0 reads as "use the vertex colour"), written lazily up to the next lit
 * prim so the cost stays with the prims that use it. */
float g_ppLitBuffer[MAX_VERTEX_BUFFER_SIZE][4];
static int s_ppLitCursor = 0;

static void PpLitZeroTo(int end)
{
	if (end > s_ppLitCursor)
		memset(g_ppLitBuffer[s_ppLitCursor], 0, (size_t)(end - s_ppLitCursor) * sizeof(g_ppLitBuffer[0]));
	if (end > s_ppLitCursor)
		s_ppLitCursor = end;
}

extern "C" const float* PsyX_PpLitBufferFinish(int count)
{
	PpLitZeroTo(count);
	return g_ppLitBuffer[0];
}

/* map[i] = which prim corner buffer vertex i came from. Anything the near
 * clipper or the size cull reshaped no longer lines up with the corners, so it
 * stays on the vertex colour. */
static void PpLitFill(const void* prim, int first, int emitted, int expected, bool reshaped, const int* map)
{
	const PpLitEntry* e = (emitted == expected && !reshaped) ? PpLitFind(prim) : nullptr;
	PpLitZeroTo(first);
	if (e == nullptr) {
		PpLitZeroTo(first + emitted);
		return;
	}
	for (int i = 0; i < emitted; i++) {
		const float* n = &e->n[map[i] * 3];
		g_ppLitBuffer[first + i][0] = n[0];
		g_ppLitBuffer[first + i][1] = n[1];
		g_ppLitBuffer[first + i][2] = n[2];
		g_ppLitBuffer[first + i][3] = (float)e->kind;
	}
	s_ppLitCursor = first + emitted;
}

static bool s_curPgxpAffine = false;
static bool s_curNoFlashlight = false;
static void PGXP_BeginPrim(const void* prim) {
	const AffineEntry* e = AffineFind(prim);
	s_curPgxpAffine = e && e->affine;
	s_curNoFlashlight = e && e->noFl;
}

/* ---- Sub-pixel weld (close PGXP cross-bone joint seams) ---------------------
 * Even with complete coverage a thin residual seam survives: a joint shared by
 * two bone meshes is TWO distinct verts at the same 3D point, and independent
 * fixed-point matrix math per bone projects them up to ~1px apart. PSX integer
 * rounding hid this; PGXP's sub-pixel positions expose it as a seam that shifts
 * with the pose (flickers during animation). Snap a precise vert onto a near-
 * coincident earlier vert THIS FRAME — same screen position within g_pgxpWeldPx
 * AND near-identical depth W — so the shared point renders once. The depth gate
 * is the safety: only verts that are genuinely the same 3D point merge; two
 * different surfaces that merely overlap on screen never fuse. Gen-stamped per
 * frame like the shadow table. Unlike the old weld this runs over COMPLETE
 * coverage with a tight radius, so it only dedups coincident points — it is not
 * papering over missing precise data. */
/* OFF by default: a global distance weld can't tell a real shared joint from any
 * other nearby same-depth vert (a character's whole body is ~one depth), so it
 * flattens detail and spawns new seams — the historical weld failure. Kept behind
 * the console WELD cmd only as an experimental knob; the clean shadow model (WELD 0)
 * is the shipped behaviour. */
float g_pgxpWeldPx     = 0.0f;   /* console WELD:  0 = off (default) */
float g_pgxpWeldWRatio = 1.04f;  /* console WELDW: max depth (W) ratio to weld */
struct WeldEntry { unsigned gen; float x, y, w; };
#define WELD_BITS 17
#define WELD_SIZE (1u << WELD_BITS)
#define WELD_MASK (WELD_SIZE - 1u)
static WeldEntry s_weld[WELD_SIZE];
static inline unsigned WeldHash(int ix, int iy) {
	return ((unsigned)ix * 73856093u) ^ ((unsigned)iy * 19349663u);
}
static void WeldVertex(float* x, float* y, float* w)
{
	if (g_pgxpWeldPx <= 0.0f) return;
	const float r2 = g_pgxpWeldPx * g_pgxpWeldPx;
	int ix = (int)(*x < 0 ? *x - 0.5f : *x + 0.5f);
	int iy = (int)(*y < 0 ? *y - 0.5f : *y + 0.5f);
	int r = (int)(g_pgxpWeldPx + 0.999f);
	if (r < 1) r = 1; else if (r > 4) r = 4;
	for (int dy = -r; dy <= r; dy++)
	for (int dx = -r; dx <= r; dx++) {
		WeldEntry* e = &s_weld[WeldHash(ix + dx, iy + dy) & WELD_MASK];
		if (e->gen != s_pgxpGen) continue;
		float ex = e->x - *x, ey = e->y - *y;
		if (ex * ex + ey * ey > r2) continue;
		float lo = e->w < *w ? e->w : *w, hi = e->w < *w ? *w : e->w;
		if (lo > 0.0f && hi <= lo * g_pgxpWeldWRatio) { *x = e->x; *y = e->y; *w = e->w; return; }
	}
	WeldEntry* e = &s_weld[WeldHash(ix, iy) & WELD_MASK];
	e->gen = s_pgxpGen; e->x = *x; e->y = *y; e->w = *w;
}

/* Fill a GrVertex's precise PGXP fields (ppx/ppy/ppw) from the shadow at the
 * vertex's prim-field address. ppw>0 selects the shader's perspective path;
 * ppw=0 is affine. addr = the field pointer (MakeVertex has it); rawX/rawY = the
 * integer coord in that field. */
/* Console PGXPDBG bitmask (diagnostic, remove once the PGXP-only far ground
 * holes are found): turns one PGXP subsystem off while PGXP itself stays on.
 * 1 = no precise vertices (all affine), 2 = legacy depth instead of the PGXP
 * depth channel, 4 = per-prim alpha forced opaque, 8 = no world split mode. */
extern "C" { int g_PsxPgxpDbg = 0; }

static inline void PgxpFillVertex(GrVertex* v, const void* addr, int rawX, int rawY, float ofsX, float ofsY)
{
	if (s_curPgxpAffine || (g_PsxPgxpDbg & 1)) {
		v->ppx = (float)v->x; v->ppy = (float)v->y; v->ppw = 0.0f; s_pgxpMiss++; return;
	}
	float ox, oy, ow;
	if (GetPreciseVertex(addr, *(const unsigned*)addr, rawX, rawY, ofsX, ofsY, &ox, &oy, &ow)) {
		WeldVertex(&ox, &oy, &ow);
		v->ppx = ox; v->ppy = oy; v->ppw = ow; s_pgxpDet++;
		if (g_PsxPgxpDbg & 64) v->ppw = 1.0f;
		if (g_PsxPgxpDbg & 128) { v->ppx = (float)v->x; v->ppy = (float)v->y; }
	} else {
		v->ppx = (float)v->x; v->ppy = (float)v->y; v->ppw = 0.0f; s_pgxpMiss++;
	}
}

/* Set by the game (PsyX_ForceItemDepthBegin/End) around the isolated item-model
 * draw — the inventory carousel and the world pickup, where OT0 holds only that
 * one TMD. It already forces per-pixel depth for those; we also read it here to
 * keep the item OUT of the world's per-pixel flashlight. Since the PGXP item fix,
 * item vertices carry a propagated view-space shadow, so without this the isolated
 * preview model would light up as if it were standing in the scene. */
extern int g_PsyX_ForceItemDepth;

/* Fill a GrVertex's view-space position (vsx/vsy/vsz) from the flashlight shadow
 * at the vertex's prim-field address (same address-keyed lookup as PGXP). A miss
 * leaves the memset-0 default, which the shader treats as "untracked" (vsz<=0,
 * not lit). Called when g_PsyX_UsePerPixelFlashlight or g_PsxUsePgxp (near clip). */
static inline void VsFillVertex(GrVertex* v, const void* addr)
{
	const VsEntry* e = Vs_Get(addr, *(const unsigned*)addr);
	/* nx doubles as the shadow-caster suppress flag (a_normal is otherwise unused —
	 * the cone shader reconstructs its normal from derivatives). A miss leaves the
	 * memset-0 default = casts normally. ny doubles as the "view-space entry valid"
	 * marker for the near clipper: a behind-the-eye vertex legitimately has vsz<=0,
	 * so presence can't be inferred from the position itself. No shader reads ny. */
	if (e) {
		v->vsx = e->vx; v->vsy = e->vy; v->vsz = e->vz; v->nx = e->nocast; v->ny = 1.0f;
		v->nz = e->fade;
		/* Latch this vertex's projection registers for the near clipper: every
		 * vertex of a poly was transformed under the same GTE state, and the
		 * clip runs a few calls later in the same ParsePrimitive. */
		s_curPolyOfx = e->ofx; s_curPolyOfy = e->ofy; s_curPolyH = e->h;
		s_curPolyProjValid = 1;
	}
	/* Opt this prim out of the per-pixel flashlight: a non-positive view-Z is the
	 * shader's "untracked, not lit" sentinel (flP.z>0 gate). Position/depth are
	 * unaffected — these prims are also forced affine, so ppw comes from the screen
	 * path, not vsz. Used for the reflective sewer water (see PsyX_SetNextPrimNoFlashlight). */
	if (s_curNoFlashlight) v->vsz = -1.0f;
}

/* True when the near clipper will take this poly: every vertex carries a
 * validated view-space entry (ny marker) and the poly straddles the near plane.
 * MakeVertexTriangle/Quad consult this to keep per-vertex precise data intact —
 * their whole-poly affine drop would otherwise destroy the in-front vertices'
 * projections before the clipper runs. Callers ensure g_PsxUsePgxp. */
static inline bool PgxpNearClipEligible(const GrVertex* v, int n)
{
	if (!g_PsxPgxpNearClip || s_curPgxpAffine)
		return false;
	int front = 0, behind = 0;
	for (int i = 0; i < n; i++) {
		if (v[i].ny < 0.5f)
			return false;
		if (v[i].vsz >= g_PgxpNearZ) front++; else behind++;
	}
	return front > 0 && behind > 0;
}

DISPENV currentDispEnv;
DISPENV activeDispEnv;
DRAWENV activeDrawEnv;

static const char* currentSplitDebugText = nullptr;
TextureID overrideTexture = 0;
/* Companion normal map for overrideTexture (0 = none). No producer yet — the
 * flashlight normal-map path sets it. Declared here so the batch key can
 * account for it before anything binds it. */
TextureID overrideNormalTexture = 0;
int overrideTextureWidth = 0;
int overrideTextureHeight = 0;
int overrideTextureOffsetX = 0;
int overrideTextureOffsetY = 0;
/* Hi-res GL texture pixel dims (0 = unknown/legacy) — feed the shader's
 * per-native-texel footprint clamp so LINEAR sampling can't bleed into the
 * neighboring atlas cell (font glyph / cursor edge artifacts with packs). */
int overrideTextureHiresW = 0;
int overrideTextureHiresH = 0;

// DR_PSYX_TEX packet state, kept separately so the hi-res override lookup
// below can restore it on a miss instead of clobbering it to zero.
static TextureID drPsyxTexOverride = 0;
static int drPsyxTexOverrideWidth = 0;
static int drPsyxTexOverrideHeight = 0;

/* Scene VRAM-scratch redirect latch (PsyX_render.cpp) — see ProcessDrawEnv
 * case 0x4. */
extern "C" void GR_SetSceneFbRedirect(int x, int y, int w, int h);

/* Hi-res texture overrides (host side, e.g. pc_port/src/hires_override.c).
 * Returns a GL texture + the ORIGINAL TIM's native pixel size + the
 * tpage-origin offset inside that TIM when the host registered a
 * replacement for this tpage/clut, else 0. Weak stub so PsyCross still
 * links for hosts that don't provide the table. */
extern "C" unsigned int __attribute__((weak))
HiresOverride_LookupByTpageClut(int tpage, int clut, int* outW, int* outH,
                                int* outOffX, int* outOffY,
                                int* outHiresW, int* outHiresH)
{
	(void)tpage; (void)clut; (void)outW; (void)outH; (void)outOffX; (void)outOffY;
	(void)outHiresW; (void)outHiresH;
	return 0;
}

/* Route a textured prim through the (otherwise dormant) overrideTexture
 * path when the host has a hi-res replacement for its tpage/clut. textureId
 * is one of AddSplit's key terms (see the full list there — it is NOT the
 * only one), so batches open/close exactly at matching prims;
 * overrideTextureWidth/Height feed texelSize with the NATIVE size,
 * so the prim's tpage-relative UVs map 0..1 across any upscale factor.
 * The offset shifts those UVs when the prim's tpage sits partway into the
 * replaced TIM (surfaces wider than one tpage draw as several prims whose
 * UVs restart at each tpage — without it every chunk showed the image
 * from x=0). On a miss the DR_PSYX_TEX packet state is restored, so that
 * path keeps its original semantics. */
/* Semi-transparency flag of the primitive being parsed; set by ParsePrimitive
 * for the feedback detector below. */
static int s_curPrimSemiTrans = 0;

/* Set by that detector for the primitive currently being built, so the rect
 * builder can stretch it across a widened ortho (see g_PsxFeedbackWideScale).
 * Cleared per primitive in ParsePrimitive. */
static int s_curPrimIsFeedback = 0;

/* The primitive samples the live scene-scratch rect: a soft-focus composite
 * strip (see GR_SceneFbRedirectCovers). Cleared per primitive. */
static int s_curPrimIsScratch = 0;

/* Host-supplied: does this tpage/CLUT pair address glyphs that must not be
 * filtered (in this port, the kanji atlas: dense one-pixel strokes that a
 * bilinear tap at four or five times their size runs together into blots)?
 * Such a split is always point-sampled. NULL = none. */
extern "C" int (*g_PsyX_TextGlyphTest)(int tpage, int clut);
int (*g_PsyX_TextGlyphTest)(int tpage, int clut) = NULL;
static int s_curPrimIsGlyph = 0;
extern int g_PsxSplitIsGlyph;
extern "C" int GR_SceneFbRedirectCovers(int x, int y);
extern "C" float g_PsxFeedbackDampBlend;

/* Red modulation of the primitive being parsed (the loading trail alternates
 * 127/128, and 127 is a PS1 decay frame for its copy loop). */
static int s_curPrimR0 = 128;

static inline void ApplyHiresOverride(int tpage, int clut)
{
	int nW = 0, nH = 0, offX = 0, offY = 0, hiW = 0, hiH = 0;

	s_curPrimIsGlyph = (g_PsyX_TextGlyphTest != NULL) ? g_PsyX_TextGlyphTest(tpage, clut) : 0;

	/* Framebuffer-feedback consumers announce themselves here: 16bpp (tp >= 2)
	 * addressing the left 320 VRAM columns is the display-buffer band, which is
	 * the whole effect family's signature and nothing else's -- real 16bpp game
	 * textures live at x >= 512. The store then knows it has a reader this frame,
	 * which pass will redraw the capture, and whether that reader blends. Same
	 * test hires_override.c uses to refuse an override for these prims. */
	if (((tpage >> 7) & 0x3) >= 2 && ((tpage & 0xF) * 64) < 320)
	{
		s_curPrimIsFeedback = 1;
		GR_NoteFeedbackSamplerPrim(s_curPrimSemiTrans, s_curPrimR0);
	}
	else if (((tpage >> 7) & 0x3) >= 2 &&
	         GR_SceneFbRedirectCovers((tpage & 0xF) * 64, ((tpage >> 4) & 1) * 256))
	{
		s_curPrimIsScratch = 1;
	}

	unsigned int hi = HiresOverride_LookupByTpageClut(tpage, clut, &nW, &nH, &offX, &offY, &hiW, &hiH);
	if (hi != 0) {
		overrideTexture        = (TextureID)hi;
		overrideTextureWidth   = nW;
		overrideTextureHeight  = nH;
		overrideTextureOffsetX = offX;
		overrideTextureOffsetY = offY;
		overrideTextureHiresW  = hiW;
		overrideTextureHiresH  = hiH;
	}
	else {
		overrideTexture        = drPsyxTexOverride;
		overrideTextureWidth   = drPsyxTexOverrideWidth;
		overrideTextureHeight  = drPsyxTexOverrideHeight;
		overrideTextureOffsetX = 0;
		overrideTextureOffsetY = 0;
		overrideTextureHiresW  = 0;
		overrideTextureHiresH  = 0;
	}
}

/* A clut word with bit 15 set names no palette that VRAM sampling can reach,
 * and drawing it produces PERMANENT wrong colours rather than a visible glitch:
 * the word travels to the shader in GrVertex.clut, which is uploaded as part of
 * a_position with glVertexAttribPointer(..., GL_SHORT, ...) — SIGNED — so
 * `v_page_clut.w = floor(a_position.w / 64.0) / 512.0` evaluates NEGATIVE, and
 * the VRAM texture declares no wrap mode, so GL_REPEAT folds that negative V
 * back onto some arbitrary real VRAM row. The prim then samples a valid-looking
 * but unrelated 16-colour palette every frame it is drawn: rainbow geometry.
 *
 * Bit-15 cluts are legitimate in exactly one case — the host's hires-override
 * pool deliberately keys its GL-backed virtual texture slots on them. Those
 * prims are textured from a GL texture and never sample VRAM at all, so the
 * override lookup is the discriminator, not the bit pattern. */
static inline bool ClutHasNoPalette(int tpage, int clut)
{
	/* 16bpp/direct tpages sample no palette at all — the 16-bit samplePSX
	 * variant never touches v_page_clut.zw, and PSX hardware likewise ignores
	 * clut in that mode, so framebuffer-sampling prims (cutscene ghosting
	 * overlays) legitimately ship uninitialized clut bytes. The chain above
	 * cannot reach them, so dropping them would be pure loss. */
	if (GET_TPAGE_FORMAT(tpage) >= TF_16_BIT)
		return false;

	/* Read unsigned and do NOT mask to 0x1FF: masking caps clutY at 511 and
	 * makes the test below dead code. */
	int clutY = ((unsigned short)clut) >> 6;
	if (clutY <= 511)
		return false;

	return HiresOverride_LookupByTpageClut(tpage, clut, nullptr, nullptr, nullptr,
	                                       nullptr, nullptr, nullptr) == 0;
}

/* Test + rate-limited report, shared by every textured prim type. The counter
 * is shared too, so a single bad frame across several prim types still cannot
 * flood the log. */
static inline bool ShouldDropForClut(const char* primType, int tpage, int clut)
{
	if (!ClutHasNoPalette(tpage, clut))
		return false;

	static int s_clutDropCount = 0;
	if (s_clutDropCount < 32)
	{
		s_clutDropCount++;
		eprintinfo("[CLUTDROP] type=%s tpage=0x%04X clut=0x%04X clutY=%d reason=clutY_oob\n",
			primType, (unsigned)(unsigned short)tpage, (unsigned)(unsigned short)clut,
			((unsigned short)clut) >> 6);
	}
	return true;
}

int g_GPUDisabledState = 0;
int g_DrawPrimMode = 0;

// Per-primitive GTE SZ depth table.  Populated at addPrim time (GTE SZ registers
// valid immediately after RotTransPers calls), looked up during primitive parsing
// to give GL per-vertex perspective depth.  Cleared each GsDrawOt call.
// 4096 slots, linear probe ≤16; collision rate is negligible for typical scene sizes.
/* 12 bits = 4096 entries, and a dense outdoor frame submits MORE prims than
 * that, so the table saturated and the 16-probe insert force-evicted. A prim
 * whose lookup then misses gets depthMode DISABLED while its neighbours get
 * WORLD -- and depthMode is part of the split merge key, so every miss forces
 * a NEW split. That is what exhausted MAX_DRAW_SPLITS. 15 bits = 32768 entries
 * (~1 MB) puts the load factor back under 20%. */
#define SZ_TABLE_BITS 18
#define SZ_TABLE_SIZE (1 << SZ_TABLE_BITS)
#define SZ_TABLE_MASK (SZ_TABLE_SIZE - 1)

/* PGXP coplanar depth fix (docs/PGXP_PR51_Vetting.md). Each per-prim SZ entry
 * also records how its depth was produced: FLAT = the static world-mesh drawer
 * (Gfx_MeshDraw, the SOLE caller of PsyX_SetNextPrimSz), EXACT = decals/inventory
 * authored Z (PsyX_SetNextPrimSzExact). This is the WORLD-vs-actor discriminator:
 * FLAT prims are drawn GL_ALWAYS (painter order among themselves) so coplanar
 * static faces — rugs on floors, paper on desks, distant road/wall panels — stop
 * z-fighting, while still leaving a real depth buffer for actors to LEQUAL-test
 * against. All of it is gated on g_PsxUsePgxp; with PGXP off nothing reads kind. */
enum { SZ_KIND_NONE = 0, SZ_KIND_FLAT = 1, SZ_KIND_EXACT = 2, SZ_KIND_ACTOR = 3 };
/* ACTOR: a character prim armed with its own per-vertex SZ by the game's
 * skeleton drawers (PsyX_SetNextPrimSzActor while g_PsxActorDepth is set).
 * Unarmed, those drawers reach the capture with the GTE FIFO's SZ, which the
 * lighting steps have clobbered: with PGXP off each prim took a stale flat
 * depth, and with it on the flat bucket seed tied every prim of a bucket. Either
 * way overlapping surfaces (Harry's belt under the jacket hem, a high-poly
 * model's mouth behind its lips) flickered as the pose moved. Armed prims take
 * per-vertex depth instead -- from the PGXP W when it exists (shader,
 * a_extra.w == 2, without the world's far-push so they still win against the
 * floor they stand on), else from the SZ. */
extern "C" { int g_PsxActorDepth = 0; int g_PsxPgxpActorDepth = 1; }
/* WORLD_FAR: whole-map far-band world geometry. Every poly past the last OT
 * bucket shares that one bucket, so painter order cannot sort the distant town
 * (blocks and trees composite in submission order); those prims draw GL_LEQUAL
 * on their true per-vertex depth instead. */
enum { SPLIT_DEPTH_DISABLED = 0, SPLIT_DEPTH_WORLD = 2, SPLIT_DEPTH_WORLD_FAR = 3 };

/* sz widened u16 -> u32: the whole-town far-depth substitution below feeds true
 * cell-center view depths that exceed the 0xFFFF SZ clamp. u16 values still fit
 * exactly, so normal play is numerically identical. */
/* gen: frame stamp (s_pgxpGen, bumped once per frame in PGXP_CoverageTick) so the
 * table can retire stale entries WITHOUT the GsDrawOt memset — groundwork for the
 * depth channel (plan Step 3 skips the memset when PGXP is on; a reused packet
 * address from a previous frame must then read as a miss, exactly like the
 * s_shadow / s_affine tables already do). Inert while the memset still runs. */
struct SZEntry { uintptr_t key; uint32_t sz[4]; unsigned gen; unsigned wipe; unsigned char kind; unsigned char alpha; unsigned char far; };

/* Per-prim ALPHA (0..255, default 255 = opaque semantics unchanged), armed like
 * the SZ payload and carried in the same entry. BM_AVERAGE is real
 * SRC_ALPHA blending on PC, so scaling a prim's alpha is a true transparency
 * fade -- the one thing an average-blended DARK texture (the bullet decal)
 * needs to dissolve into fog, since no vertex-colour arithmetic can lift a dark
 * texture above the fog it darkens. */
static int g_primAlphaNext = 255;
/* Set per prim by ApplyGtePerVertexDepthImpl (which runs before the colour
 * builders), consumed and reset by them. */
static unsigned char s_primAlphaPending = 255;

extern "C" void PsyX_SetNextPrimAlpha(int a)
{
	if (a < 0)   a = 0;
	if (a > 255) a = 255;
	g_primAlphaNext = a;
}
static SZEntry g_szTable[SZ_TABLE_SIZE];
/* The PGXP-off GsDrawOt wipe bumps this instead of memsetting the 8 MB table
 * (twice a frame, a few ms on a weak CPU). An entry stamped with an older wipe
 * reads exactly as the zeroed entry the memset left. */
static unsigned s_szWipe = 0;
#define SZ_KEY(s) (g_szTable[s].wipe == s_szWipe ? g_szTable[s].key : (uintptr_t)0)

/* --- Depth channel Step 3 (docs: PGXP_PR51_Vetting.md; live only when PGXP is
 * on and outside the inventory item pass) ---------------------------------
 * One CONSTANT linear viewZ->NDC scale for every depth source so flat testers,
 * bucket seeds and (Step 4) per-vertex world writes are commensurable by
 * construction: ndc(vz) = 1 - 2*vz/2^18. Linear (not reciprocal) keeps uniform
 * far resolution where the distant-gap fix lives, and no depth-buffer format
 * change is needed — a 16-bit grant degrades ties toward painter order, the
 * PSX direction. */
#define PGXP_DEPTH_SZMAX 262144.0f /* 2^18 SZ units (~1024 world units) */
/* Kill-switch (console PGXPWORLDDEPTH): suppresses FLAT promotion everywhere,
 * dropping the whole feature back to bucket+painter behavior instantly. */
extern "C" { int g_PsxPgxpWorldDepth = 1; }
/* Writer-side far-push margin M in SZ units (console PGXPWALLBIAS): world
 * geometry is pushed slightly FARTHER so coplanar testers (props against a
 * wall/floor) win LEQUAL without any tester-side bias or tie-rank. */
extern "C" { int g_PsxPgxpWorldFarBias = 64; }

static inline float PgxpNdcFromViewZ(float vz)
{
	float z = 1.0f - 2.0f * vz * (1.0f / PGXP_DEPTH_SZMAX);
	if (z < -1.0f) z = -1.0f;
	if (z >  1.0f) z =  1.0f;
	return z;
}

/* OT-bucket -> viewZ shift (shiftEff): the game inserts world prims at
 * org[SZ >> (arg3+2)] (Gfx_MeshDraw) and TMD actors at org[p >> shift] with
 * p ~ SZ>>2 (GsSortObject4J drawers), so bucket index b covers viewZ
 * ~ b << shiftEff. Registered by both producers; a disagreement is logged
 * once (the world scale wins — testers then read slightly nearer, which only
 * widens their margin). 0 = never registered -> legacy index-based seeds. */
static int s_otViewZShift = 0;
extern "C" void PsyX_SetOtViewZShift(int shiftEff)
{
	if (shiftEff <= 0) return;
	if (s_otViewZShift != 0 && s_otViewZShift != shiftEff)
	{
		static int s_shiftWarned = 0;
		if (!s_shiftWarned)
		{
			s_shiftWarned = 1;
			eprintwarn("[PGXPDEPTH] OT viewZ shift disagreement: %d vs %d (keeping latest)\n",
				s_otViewZShift, shiftEff);
		}
	}
	s_otViewZShift = shiftEff;
}

/* Whole-town far depth (docs/WholeMap_Far_Projection_Task.md). The GTE SZ3
 * register saturates at 0xFFFF = ~256 world units, so EVERY poly beyond 256u
 * gets the same clamped depth -> identical GL depth -> GL_LEQUAL ties resolve by
 * draw order and distant town blocks composite over each other (the "block
 * merged into the intersection" report). Chunks are cell-confined to 40u, so the
 * game hands us each far chunk's true cell-center view depth here (SZ units, set
 * per chunk around Ipd_ChunkDraw); a fully-saturated far poly takes it instead of
 * 0xFFFF, giving correct block-vs-block ordering. 0 / mode-off => untouched.
 * (g_PsxWholeMapFar itself is defined earlier in this file.) */
extern "C" { int g_PsxWholeMapChunkSz = 0; }

// Global SZ scale: maximum SZ seen in the previous frame, used as the
// depth reference so all polygons share a consistent window_depth space
// regardless of which OT bucket they landed in.
static uint32_t g_szMaxThisFrame = 0;
static uint32_t g_szMaxPrevFrame = 0;

/* PGXP depth fix: the previous frame's max SZ, used to normalize per-vertex
 * SZ3 into NDC depth in the shader (same formula as ApplyGtePerVertexDepth but
 * per-vertex + unquantized, so coplanar faces no longer share a depth bucket).
 * Returns 1 as a safe floor before the first frame. */
extern "C" float PGXP_GetSzMax(void)
{
	/* Depth channel: one CONSTANT normalization when PGXP is on, so depth no
	 * longer wobbles with the prev-frame content maximum. Legacy value kept
	 * for the off path (and the item pass reads g_szMaxPrevFrame directly). */
	if (g_PsxUsePgxp)
		return PGXP_DEPTH_SZMAX;
	return (g_szMaxPrevFrame < 1) ? 1.0f : (float)g_szMaxPrevFrame;
}

// World-geometry renderers (Gfx_MeshDraw) bulk-transform vertices before the
// polygon loop, so the GTE SZ FIFO is stale at each polygon's addPrim call.
// They call PsyX_SetNextPrimSz with the polygon's field_18C SZ values so the
// next PsyX_CaptureGteDepths invocation uses the correct per-vertex depths.
static uint32_t g_primSzNext[4];
static int g_primSzNextValid = 0;
static unsigned char g_primSzNextKind = SZ_KIND_NONE; /* producer kind for the next captured prim */
static unsigned char g_primSzNextFar = 0;

extern "C" void PsyX_SetNextPrimSz(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3, int arg3)
{
	uint32_t avg_q;
	uint32_t mx;

	/* Whole-town far poly (all 4 verts saturated at the 0xFFFF ~256u SZ clamp):
	 * substitute the chunk's true cell-center view depth so the far town depth-
	 * sorts instead of collapsing to one plane. Only when that depth is itself
	 * past the clamp (a genuinely-far chunk); nearer chunks keep real per-poly SZ,
	 * and the 128-256u band is already monotonic (u16 un-wrap). */
	if (g_PsxWholeMapFar && g_PsxWholeMapChunkSz > 0xFFFF &&
	    s0 == 0xFFFF && s1 == 0xFFFF && s2 == 0xFFFF && s3 == 0xFFFF)
	{
		avg_q = (uint32_t)g_PsxWholeMapChunkSz;
		mx    = avg_q;
	}
	else
	{
		uint32_t avg = ((unsigned)s0 + s1 + s2 + s3) >> 2;
		/* Depth channel: keep the RAW average when PGXP is on — the 64-unit
		 * quantization existed to force coplanar neighbours into shared depth
		 * buckets, which the GL_ALWAYS world painter now makes unnecessary.
		 * Off path keeps the historical quantized value. */
		avg_q = g_PsxUsePgxp ? avg : ((avg >> 6) << 6);
		// Calibrate with unquantised real max so character/item GL depths stay accurate.
		mx = s0 > s1 ? s0 : s1;
		if (s2 > mx) mx = s2;
		if (s3 > mx) mx = s3;
	}
	if (mx > g_szMaxThisFrame) g_szMaxThisFrame = mx;
	g_primSzNextFar = (g_PsxWholeMapFar && g_PsxWholeMapFarDepth && arg3 >= 0 && arg3 <= 3 &&
	                   mx >= (2047u << (arg3 + 2))) ? 1 : 0;
	if (g_PsxUsePgxp) s_dbgArmF++;
	g_primSzNext[0] = g_primSzNext[1] = g_primSzNext[2] = g_primSzNext[3] = avg_q;
	g_primSzNextValid = 1;
	g_primSzNextKind = SZ_KIND_FLAT; /* SOLE caller is Gfx_MeshDraw (static world) */
}

extern "C" float PsyX_GetItemDepthSzMax(void)
{
	return (g_szMaxPrevFrame < 1) ? 1.0f : (float)g_szMaxPrevFrame;
}

/* A replacement drawer that emits its own packet instead of running the stock
 * prim builder never reaches PsyX_SetNextPrimSz*, so it contributes nothing to
 * the frame maximum — yet it still normalizes against that maximum one GsDrawOt
 * later. Feed it here, at sort time, exactly where the builder it replaced fed
 * it. This bumps the maximum ONLY: arming g_primSzNext would hand the payload to
 * whatever legacy prim happens to be captured next. */
extern "C" void PsyX_NoteItemDepthSz(unsigned int sz)
{
	if (sz > g_szMaxThisFrame) g_szMaxThisFrame = sz;
}

/* Corners in prim order; a triangle passes n = 3 (the parse reads a triangle's
 * corners from slots 1..3, the FIFO's SZ1..SZ3). */
extern "C" void PsyX_SetNextPrimSzActor(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3, int n)
{
	if (n == 3) { s3 = s2; s2 = s1; s1 = s0; }
	g_primSzNext[0] = s0; g_primSzNext[1] = s1;
	g_primSzNext[2] = s2; g_primSzNext[3] = s3;
	g_primSzNextValid = 1;
	g_primSzNextKind = SZ_KIND_ACTOR;
	g_primSzNextFar = 0;
}

extern "C" void PsyX_SetNextPrimSzExact(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3)
{
	uint32_t mx = s0 > s1 ? s0 : s1;
	if (s2 > mx) mx = s2;
	if (s3 > mx) mx = s3;
	if (mx > g_szMaxThisFrame) g_szMaxThisFrame = mx;
	g_primSzNext[0] = s0; g_primSzNext[1] = s1;
	g_primSzNext[2] = s2; g_primSzNext[3] = s3;
	g_primSzNextValid = 1;
	g_primSzNextKind = SZ_KIND_EXACT; /* decals / inventory authored Z — NOT world painter */
}

/* True depth past the u16 SZ clamp, for whole-map billboards: their prims carry
 * no SZ of their own, so the auto-capture would hand them the last OT bucket's
 * seed and every distant tree would paint in front of the depth-tested town. */
extern "C" void PsyX_SetNextPrimDepthSz(unsigned int sz)
{
	if (sz > g_szMaxThisFrame) g_szMaxThisFrame = sz;
	g_primSzNext[0] = g_primSzNext[1] = g_primSzNext[2] = g_primSzNext[3] = sz;
	g_primSzNextValid = 1;
	g_primSzNextKind = SZ_KIND_EXACT;
}

extern "C" void PsyX_CancelNextPrimSz(void)
{
	g_primSzNextValid = 0;
	g_primSzNextKind = SZ_KIND_NONE;
	g_primSzNextFar = 0;
}

extern "C" void PsyX_CaptureGteDepths(void* prim)
{
	/* PGXP: if the next prim was flagged screen-space (billboards), record it so
	 * the draw side forces affine. Per-prim, then cleared. */
	if (g_primPgxpForceAffine || g_primNoFlashlight) {
		AffineStore(prim, (unsigned char)g_primPgxpForceAffine, (unsigned char)g_primNoFlashlight);
		g_primPgxpForceAffine = 0;
		g_primNoFlashlight = 0;
	}

	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);

	uint32_t s0, s1, s2, s3;
	unsigned char kind;
	unsigned char far = 0;
	if (g_primSzNextValid) {
		s0 = g_primSzNext[0]; s1 = g_primSzNext[1];
		s2 = g_primSzNext[2]; s3 = g_primSzNext[3];
		kind = g_primSzNextKind;
		if (g_PsxUsePgxp && kind == SZ_KIND_FLAT) s_dbgCapF++;
		far = g_primSzNextFar;
		if (far && g_PsxWholeMapProbe == 1) s_wmCaptured++;
		g_primSzNextValid = 0;
		g_primSzNextKind = SZ_KIND_NONE;
		g_primSzNextFar = 0;
	} else {
		s0 = (uint16_t)C2_SZ0; s1 = (uint16_t)C2_SZ1;
		s2 = (uint16_t)C2_SZ2; s3 = (uint16_t)C2_SZ3;
		kind = SZ_KIND_NONE;
	}

	// Track per-frame SZ maximum for global depth calibration
	uint32_t mx = s0 > s1 ? s0 : s1;
	if (s2 > mx) mx = s2;
	if (s3 > mx) mx = s3;
	if (mx > g_szMaxThisFrame) g_szMaxThisFrame = mx;

	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		/* Stale-gen slots are reclaimable (same probe rule as Shadow_Put/
		 * AffineStore): identical placement while the memset wipes the table
		 * (stale entries then never exist mid-probe). */
		if (SZ_KEY(s) == 0 || SZ_KEY(s) == key || g_szTable[s].gen != s_pgxpGen) {
			g_szTable[s].key = key;
			g_szTable[s].wipe = s_szWipe;
			g_szTable[s].sz[0] = s0; g_szTable[s].sz[1] = s1;
			g_szTable[s].sz[2] = s2; g_szTable[s].sz[3] = s3;
			g_szTable[s].gen = s_pgxpGen;
			g_szTable[s].kind = kind;
			g_szTable[s].far = far;
			g_szTable[s].alpha = (unsigned char)g_primAlphaNext;
			g_primAlphaNext = 255;
			return;
		}
	}
	// Probe exhausted — overwrite initial slot
	if (g_PsxUsePgxp) s_dbgSzExhaust++;
	g_szTable[slot].key = key;
	g_szTable[slot].wipe = s_szWipe;
	g_szTable[slot].alpha = (unsigned char)g_primAlphaNext;
	g_primAlphaNext = 255;
	g_szTable[slot].sz[0] = s0; g_szTable[slot].sz[1] = s1;
	g_szTable[slot].sz[2] = s2; g_szTable[slot].sz[3] = s3;
	g_szTable[slot].gen = s_pgxpGen;
	g_szTable[slot].kind = kind;
	g_szTable[slot].far = far;
}

/* Depth mode for a prim's split: WORLD only for static-world-mesh (FLAT) opaque
 * geometry, so it draws GL_ALWAYS in painter order (coplanar z-fight fix). Hard-
 * gated on g_PsxUsePgxp -> returns DISABLED when PGXP is off, so the kind byte is
 * never consulted and split classification/batching is byte-identical to today. */
static int SplitDepthForPrim(const void* prim)
{
	if (!g_PsxUsePgxp || !g_PsxPgxpWorldDepth || (g_PsxPgxpDbg & 8))
		return SPLIT_DEPTH_DISABLED;
	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);
	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		if (SZ_KEY(s) == key)
		{
			/* Stale-gen = captured a previous frame, not this one: a reused
			 * packet address must not inherit last frame's kind. (Function is
			 * already PGXP-gated at entry.) */
			if (g_szTable[s].gen != s_pgxpGen)
			{
				s_dbgStaleRej++;
				return SPLIT_DEPTH_DISABLED;
			}
			if (g_szTable[s].kind == SZ_KIND_FLAT) { s_dbgSplitWorld++; return g_szTable[s].far ? SPLIT_DEPTH_WORLD_FAR : SPLIT_DEPTH_WORLD; }
			return SPLIT_DEPTH_DISABLED;
		}
		if (SZ_KEY(s) == 0)
			break;
	}
	return SPLIT_DEPTH_DISABLED;
}

extern "C" void PsyX_ClearGteDepthTable(void)
{
	g_szMaxPrevFrame = g_szMaxThisFrame;
	g_szMaxThisFrame = 0;
	/* Inventory item pass only: the item's precise per-prim SZ was captured into
	 * g_szTable during the GsSortObject4J sort earlier THIS frame, and the item's
	 * own GsDrawOt(OT0) is the first draw after that sort (no intervening GsDrawOt),
	 * so wiping the table here would drop it before ApplyGtePerVertexDepth reads it —
	 * collapsing every item poly to one bucket depth (radio antenna through the body).
	 * Skip the wipe while g_PsyX_ForceItemDepth is set (scoped by game code to the
	 * item-only OT0 draw in GameState_InventoryScreen); every world/pickup draw has
	 * the flag == 0 and clears normally. g_szMaxPrevFrame still swaps to the item's
	 * own max above, so the item's depth normalizes against itself. */
	/* Uses the file-scope decl at the top of this file. NOT redeclared here: this
	 * function is extern "C", so a block-scope `extern int` would take C language
	 * linkage and clash with the C++-linkage file-scope decl / definition — Clang
	 * (macOS CI) errors "different language linkage"; GCC just ignored it. */
	if (!g_PsyX_ForceItemDepth)
	{
		/* Depth channel: with PGXP on the table is NOT wiped — entries retire
		 * by gen stamp instead (Step-2 plumbing), so the frame's captured
		 * FLAT/EXACT kinds finally survive from addPrim to the GsDrawOt parse.
		 * The [PGXPDEPTH] probe proved the wipe discarded ~70k world entries/s
		 * with parse hit=0 — the machinery was inert in gameplay. Off path
		 * wipes exactly as before (byte-identical); a runtime PGXP on->off
		 * toggle self-heals here on the first off-frame GsDrawOt. */
		if (!g_PsxUsePgxp)
			s_szWipe++;
	}
	else if (g_PsxUsePgxp)
		s_dbgWipeSkips++;
	g_primSzNextValid = 0;
	/* s_shadow / s_affine are gen-stamped, NOT cleared here: this runs at the start
	 * of GsDrawOt, after addPrim filled them but before DrawOTag reads them, so a
	 * memset would wipe the current frame's entries before use. */
	g_primPgxpForceAffine = 0;
	s_curPgxpAffine = false;
	g_primNoFlashlight = 0;
	s_curNoFlashlight = false;
}

static unsigned char PsyX_LookupGteAlpha(const void* prim)
{
	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);
	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		if (SZ_KEY(s) == key)
			/* Gen-validate ALWAYS (unlike the depths lookup's PGXP-gated
			 * check): arm and consume are same-frame, so a match from an
			 * earlier gen is a reused packet address, not this prim. Without
			 * this, a decal's fade alpha from gameplay attached itself to
			 * whatever landed on that address later -- the load screen drew
			 * Harry semi-transparent, ghosting over the uncleared background. */
			return (g_szTable[s].gen == s_pgxpGen) ? g_szTable[s].alpha : 255;
		if (SZ_KEY(s) == 0) break;
	}
	return 255;
}

static bool PsyX_LookupGteDepths(const void* prim, uint32_t* sz, unsigned char* kindOut = nullptr)
{
	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);
	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		if (SZ_KEY(s) == key) {
			/* Gen-validate gated on PGXP: the legacy pgxpZBuffer path (PGXP
			 * off) keeps its exact historical behavior; when PGXP is on a
			 * reused packet address from a prior frame reads as a miss. */
			if (g_PsxUsePgxp && g_szTable[s].gen != s_pgxpGen)
			{
				s_dbgStaleRej++;
				return false;
			}
			sz[0] = g_szTable[s].sz[0]; sz[1] = g_szTable[s].sz[1];
			sz[2] = g_szTable[s].sz[2]; sz[3] = g_szTable[s].sz[3];
			if (kindOut) *kindOut = g_szTable[s].kind;
			s_lastLookupFar = g_szTable[s].far;
			return true;
		}
		if (SZ_KEY(s) == 0) break;
	}
	return false;
}

// Overrides flat bucket z with GTE SZ-based depth.
// Uses average SZ across polygon vertices: geometrically more accurate than max_SZ,
// which can be dominated by a single near vertex and sort adjacent coplanar surfaces
// into the wrong OT depth relationship.  Uniform depth per polygon (all vertices
// share one value) eliminates the per-vertex interpolation that caused diffuse
// Z-fighting along polygon edges.
static void ApplyGtePerVertexDepthImpl(GrVertex* vertex, const P_TAG* polyTag, bool isQuad)
{
	/* Depth channel (PGXP on, outside the inventory item pass): flat per-prim
	 * depth on the shared constant linear scale — no prev-frame normalizer
	 * needed. Everything else keeps the legacy self-normalized behavior. */
	const bool pgxpWorldPath = g_PsxUsePgxp && !g_PsyX_ForceItemDepth && !(g_PsxPgxpDbg & 2);
	if (g_PsxUsePgxp) s_dbgApplyCalls++;
	if (!pgxpWorldPath && g_szMaxPrevFrame < 1) return;

	if (g_PsxWholeMapProbe == 1)
	{
		const int n = isQuad ? 4 : 3;
		float dev = 0.0f;
		int prec = 0;
		for (int i = 0; i < n; i++)
		{
			if (vertex[i].ppw <= 0.0f) continue;
			float dx = vertex[i].ppx - vertex[i].x, dy = vertex[i].ppy - vertex[i].y;
			if (dx < 0) dx = -dx;
			if (dy < 0) dy = -dy;
			if (dx > dev) dev = dx;
			if (dy > dev) dev = dy;
			prec = 1;
		}
		s_wmPrecPrims += prec;
		s_wmDevPrims += (dev > 2.0f);
	}
	if (g_PsxWholeMapProbe == 1 && s_otBucketAbs == g_currentOTBucketCount - 1)
	{
		const int n = isQuad ? 4 : 3;
		int aff = 0;
		uint32_t tsz[4];
		for (int i = 0; i < n; i++) if (vertex[i].ppw <= 0.0f) aff = 1;
		s_wmLastPrims++;
		s_wmLastAffine += aff;
		if (PsyX_LookupGteDepths(polyTag, tsz)) s_wmLastHit++;
	}
	uint32_t sz[4];
	unsigned char kind = SZ_KIND_NONE;
	if (!PsyX_LookupGteDepths(polyTag, sz, &kind))
	{
		if (g_PsxUsePgxp) s_dbgParseMiss++;
		return;
	}
	if (g_PsxUsePgxp)
	{
		s_dbgParseHit++;
		if (kind == SZ_KIND_FLAT)       s_dbgParseHitFlat++;
		else if (kind == SZ_KIND_EXACT) s_dbgParseHitExact++;
		else                             s_dbgParseHitNone++;
	}

	/* Per-prim alpha: 255 for everything that never armed it. NOT written to
	 * the vertices here -- the parse calls the COLOUR builders after this
	 * function, and every one of them writes a = 255, which silently stomped
	 * the first version of this. Stashed instead; the colour builders consume
	 * it (s_primAlphaPending) and reset it to 255. */
	s_primAlphaPending = (g_PsxPgxpDbg & 4) ? 255 : PsyX_LookupGteAlpha(polyTag);


	float sv0, sv1, sv2, sv3 = 0.0f;
	if (isQuad) {
		sv0 = (float)sz[0]; sv1 = (float)sz[1];
		sv2 = (float)sz[3]; sv3 = (float)sz[2];  // buffer[2]=V3, buffer[3]=V2
	} else {
		sv0 = (float)sz[1]; sv1 = (float)sz[2]; sv2 = (float)sz[3];
	}

	float sz_avg = isQuad ? (sv0 + sv1 + sv2 + sv3) * 0.25f
	                      : (sv0 + sv1 + sv2) * (1.0f / 3.0f);
	if (sz_avg < 1.0f) return;  // 2D/HUD prim — keep bucket depth

	if (kind == SZ_KIND_ACTOR && g_PsxPgxpActorDepth && !g_PsyX_ForceItemDepth)
	{
		const float sv[4] = { sv0, sv1, sv2, sv3 };
		const int n = isQuad ? 4 : 3;
		for (int i = 0; i < n; i++)
		{
			float z;
			if (pgxpWorldPath)
			{
				z = PgxpNdcFromViewZ(sv[i]);
				vertex[i]._p1 = 2;
			}
			else
			{
				z = 1.0f - 2.0f * sv[i] / (float)g_szMaxPrevFrame;
				if (z < -1.0f) z = -1.0f;
				if (z >  1.0f) z =  1.0f;
			}
			vertex[i].z = z;
		}
		return;
	}

	/* Item pass (g_PsyX_ForceItemDepth): TRUE per-vertex depth. A flat
	 * per-poly average cannot order a large foreshortened face against
	 * interior geometry a few SZ behind it — the ammo-box take-screen drew
	 * its tray through the bottom face because the face's average sat
	 * farther than the tray's. The per-vertex SZs are already captured
	 * (ITEM_PRECISE_SZ -> SZ_KIND_EXACT); interpolated GL depth resolves
	 * the overlap per-pixel. Non-EXACT prims keep the legacy flat average.
	 * Flag is 0 outside the bracketed item-only OT0 draw. */
	/* Discriminator for the item-pass see-through reports: any prim reaching
	 * this pass WITHOUT the precise-SZ feed would stamp near-depth garbage and
	 * bleed at every angle (feed-failure class). Zero lines during a repro
	 * proves the remaining artifacts are authored crossing geometry resolved
	 * z-nearest instead of PSX-painter (UNQC2 tray/bevel class). */
	if (g_PsyX_ForceItemDepth && kind != SZ_KIND_EXACT)
	{
		static int s_itemDepthNonExact = 0;
		if (s_itemDepthNonExact < 16)
		{
			eprintinfo("[ITEMDEPTH] non-EXACT kind=%d szMaxPrev=%u\n", (int)kind, (unsigned)g_szMaxPrevFrame);
			s_itemDepthNonExact++;
		}
	}

	if (g_PsyX_ForceItemDepth && kind == SZ_KIND_EXACT)
	{
		const float inv = 2.0f / (float)g_szMaxPrevFrame;
		float z;
		z = 1.0f - sv0 * inv; if (z < -1.0f) z = -1.0f; if (z > 1.0f) z = 1.0f; vertex[0].z = z;
		z = 1.0f - sv1 * inv; if (z < -1.0f) z = -1.0f; if (z > 1.0f) z = 1.0f; vertex[1].z = z;
		z = 1.0f - sv2 * inv; if (z < -1.0f) z = -1.0f; if (z > 1.0f) z = 1.0f; vertex[2].z = z;
		if (isQuad)
		{
			z = 1.0f - sv3 * inv; if (z < -1.0f) z = -1.0f; if (z > 1.0f) z = 1.0f; vertex[3].z = z;
		}
		return;
	}

	if (pgxpWorldPath)
	{
		/* kind NONE = auto-captured GTE FIFO values — documented lighting
		 * garbage for every GsTMDfast* drawer (NormalClip/NormalColorCol
		 * clobber the FIFO before addPrim). Those prims keep the bucket seed,
		 * which is already remapped onto this same linear scale. */
		if (kind == SZ_KIND_NONE)
			return;
		/* Kill-switch: FLAT falls back to bucket behavior entirely. */
		if (kind == SZ_KIND_FLAT && !g_PsxPgxpWorldDepth)
			return;
		/* World geometry is pushed M farther (writer-side margin) so coplanar
		 * testers — props on floors, posters' host walls vs pickups — win
		 * LEQUAL without any tester-side bias. EXACT (item/decal authored Z)
		 * takes the same scale without the margin. */
		float vz = sz_avg + ((kind == SZ_KIND_FLAT) ? (float)g_PsxPgxpWorldFarBias : 0.0f);
		float z_val = PgxpNdcFromViewZ(vz);
		vertex[0].z = vertex[1].z = vertex[2].z = z_val;
		if (isQuad) vertex[3].z = z_val;
		/* Step 4: mark OPAQUE world verts for per-vertex depth in the vertex
		 * shader (_p1 -> a_extra.w). Opaque world draws GL_ALWAYS — it never
		 * depth-tests itself, so per-vertex depth there cannot flicker; it
		 * only makes the depth field actors test against per-pixel accurate
		 * (the distant-gap fix). SEMI-TRANS world prims draw LEQUAL against
		 * their coplanar opaque host — per-vertex depth on a tester re-creates
		 * the two-interpolants coin-flip, so they keep this flat depth. */
		if (kind == SZ_KIND_FLAT && !(polyTag->code & 2))
		{
			vertex[0]._p1 = vertex[1]._p1 = vertex[2]._p1 = 1;
			if (isQuad) vertex[3]._p1 = 1;
		}
		if (g_PsxWholeMapProbe == 1 && kind == SZ_KIND_FLAT && s_lastLookupFar)
		{
			const int n = isQuad ? 4 : 3;
			s_wmParsed++;
			for (int i = 0; i < n; i++)
			{
				if (vertex[i].ppw > 0.0f) {
					s_wmPreciseVerts++;
					if (s_wmWMin == 0.0f || vertex[i].ppw < s_wmWMin) s_wmWMin = vertex[i].ppw;
					if (vertex[i].ppw > s_wmWMax) s_wmWMax = vertex[i].ppw;
				} else s_wmAffineVerts++;
			}
		}
		return;
	}

	float z_val = 1.0f - 2.0f * sz_avg * (1.0f / (float)g_szMaxPrevFrame);
	if (z_val < -1.0f) z_val = -1.0f;
	if (z_val >  1.0f) z_val =  1.0f;
	vertex[0].z = vertex[1].z = vertex[2].z = z_val;
	if (isQuad) vertex[3].z = z_val;
}

/* =====================================================================
 * [ITEMDEPTH] one-shot item-model depth probe   (config item_depth_probe)
 * ---------------------------------------------------------------------
 * DIAGNOSTIC ONLY — every function below READS state; none writes any
 * rendering state, so with the flag off (default) the build is unchanged
 * and with it on the rendered image is identical.
 *
 * It joins the two halves of the item pipeline by packet address:
 *   SORT side  (libgs_stub.c GsTMDfast* drawers, via ITEM_PROBE_SORT):
 *              per-vertex GTE SZ, the IR0 depth cue `p`, the chosen OT
 *              bucket, the OT's length and the drawer's shift.
 *   DRAW side  (this file's OT parse, via the ApplyGtePerVertexDepth
 *              wrapper): the bucket seed the prim arrived with, the SZ
 *              table `kind`, whether the EXACT per-vertex branch actually
 *              rewrote z, the final NDC z per vertex, and the raster
 *              sequence number.
 * Plus the REAL glGet* depth state sampled inside DrawSplit immediately
 * before the item's own glDrawArrays — not a tracker variable.
 *
 * Armed once per item-screen ENTRY (pickup take-screen / inventory /
 * puzzle) so a live repro costs one dump, not one per frame.
 * ===================================================================== */
extern "C" { int g_PsyX_ItemDepthProbe = 0; }

extern int g_vertexIndex; /* defined further down this file */

#define ITEMPROBE_MAX_PRIMS  640
#define ITEMPROBE_MAX_MODELS 16
#define ITEMPROBE_MAX_GL     8

struct ItemProbeRec
{
	const void*   prim;     /* packet address — the join key */
	unsigned      sz[4];    /* per-vertex GTE SZ handed over by ITEM_PRECISE_SZ */
	long          ir0;      /* gte_stdp `p`: the perspective CUE the bucket uses */
	int           otz;      /* bucket the drawer picked */
	int           seq;      /* raster order (OT parse order); -1 = never parsed */
	short         otLen;    /* log2(bucket count) of the OT it went into */
	short         shift;    /* drawer shift: otz = ir0 >> shift */
	short         model;    /* model-draw ordinal inside this entry */
	short         primIdx;  /* submission index inside that model */
	unsigned char code;     /* P_TAG code seen at draw time */
	signed char   nv;       /* 3 or 4; -1 = never reached the draw parse */
	signed char   kind;     /* SZ_KIND_* at draw time; -1 = SZ-table lookup MISS */
	signed char   fired;    /* 1 = the per-vertex branch actually rewrote z */
	float         zSeed;    /* g_otBucketDepth the prim arrived with */
	float         zOut[4];  /* NDC z the vertices ended up with */
};

struct ItemProbeGl
{
	int   split, blend, numVerts, dfe;
	int   depthTest, depthFunc, depthMask, depthBits, fbo, cullFace;
	int   trackerDepthMode, forceItemDepth;
	float rangeNear, rangeFar;
};

static ItemProbeRec s_ipRec[ITEMPROBE_MAX_PRIMS];
static ItemProbeGl  s_ipGl[ITEMPROBE_MAX_GL];
static int s_ipModelSlot[ITEMPROBE_MAX_MODELS];
static int s_ipModelArg2[ITEMPROBE_MAX_MODELS];
static int s_ipModelPrims[ITEMPROBE_MAX_MODELS];
static int s_ipCount    = 0;   /* records used */
static int s_ipDropped  = 0;   /* sort-side prims that didn't fit */
static int s_ipModels   = 0;   /* model draws seen this entry */
static int s_ipScreen   = 0;   /* 1=TAKE 2=INV 3=PUZZLE */
static int s_ipArmed    = 0;   /* collecting right now */
static int s_ipCaptured = 0;   /* this entry already dumped */
static int s_ipSawModel = 0;   /* a model was sorted this frame */
static int s_ipSeq      = 0;   /* raster counter (counts foreign prims too) */
static int s_ipForeign  = 0;   /* parsed prims with no sort record */
static int s_ipVertLo   = 0x7FFFFFFF;
static int s_ipVertHi   = -1;
static int s_ipGlCount  = 0;

/* Called from the game (item_screens_cam.c func_8004BD74) just before the
 * GsSortObject4J that emits one item model. screen: 1=TAKE 2=INV 3=PUZZLE. */
extern "C" void PsyX_ItemProbeModelBegin(int screen, int slot, int arg2)
{
	if (!g_PsyX_ItemDepthProbe || screen == 0)
		return;

	if (!s_ipSawModel)
	{
		/* First model of this frame. A new ENTRY is either "the screen was
		 * absent last frame" (s_ipCaptured cleared by EndFrame) or "a
		 * different item screen than the one already captured". */
		if (!s_ipCaptured || screen != s_ipScreen)
		{
			s_ipCount = s_ipDropped = s_ipModels = 0;
			s_ipSeq = s_ipForeign = s_ipGlCount = 0;
			s_ipVertLo = 0x7FFFFFFF;
			s_ipVertHi = -1;
			s_ipScreen = screen;
			s_ipArmed  = 1;
		}
		s_ipSawModel = 1;
	}

	if (!s_ipArmed)
		return;

	if (s_ipModels < ITEMPROBE_MAX_MODELS)
	{
		s_ipModelSlot[s_ipModels]  = slot;
		s_ipModelArg2[s_ipModels]  = arg2;
		s_ipModelPrims[s_ipModels] = 0;
	}
	s_ipModels++;
}

/* Called from every GsTMDfast* drawer at addPrim time (ITEM_PROBE_SORT). */
extern "C" void PsyX_ItemProbeSortPrim(const void* prim, unsigned s0, unsigned s1,
                                       unsigned s2, unsigned s3, long ir0,
                                       int otz, int otLen, int shift)
{
	if (!s_ipArmed)
		return;

	int m = s_ipModels - 1;
	if (m < 0) m = 0;
	if (m >= ITEMPROBE_MAX_MODELS) m = ITEMPROBE_MAX_MODELS - 1;
	s_ipModelPrims[m]++;

	if (s_ipCount >= ITEMPROBE_MAX_PRIMS)
	{
		s_ipDropped++;
		return;
	}

	ItemProbeRec& r = s_ipRec[s_ipCount++];
	r.prim  = prim;
	r.sz[0] = s0; r.sz[1] = s1; r.sz[2] = s2; r.sz[3] = s3;
	r.ir0   = ir0;
	r.otz   = otz;
	r.otLen = (short)otLen;
	r.shift = (short)shift;
	r.model = (short)m;
	r.primIdx = (short)(s_ipModelPrims[m] - 1);
	r.seq   = -1;
	r.code  = 0;
	r.nv    = -1;
	r.kind  = -1;
	r.fired = 0;
	r.zSeed = 0.0f;
	r.zOut[0] = r.zOut[1] = r.zOut[2] = r.zOut[3] = 0.0f;
}

static void ItemProbe_RecordDraw(const GrVertex* vertex, const P_TAG* polyTag,
                                 bool isQuad, float zBefore)
{
	const int seq = s_ipSeq++;
	const int n   = isQuad ? 4 : 3;

	ItemProbeRec* r = nullptr;
	for (int i = 0; i < s_ipCount; i++)
	{
		if (s_ipRec[i].prim == (const void*)polyTag) { r = &s_ipRec[i]; break; }
	}
	if (!r)
	{
		s_ipForeign++;
		return;
	}

	r->seq   = seq;
	r->nv    = (signed char)n;
	r->code  = polyTag->code;
	r->zSeed = zBefore;
	for (int i = 0; i < n; i++)
		r->zOut[i] = vertex[i].z;

	uint32_t sz[4];
	unsigned char kind = SZ_KIND_NONE;
	r->kind  = PsyX_LookupGteDepths(polyTag, sz, &kind) ? (signed char)kind : (signed char)-1;
	r->fired = (r->zOut[0] != zBefore) ? 1 : 0;

	/* Vertex window this prim occupies, so DrawSplit knows which splits
	 * actually rasterize probed geometry. */
	if (g_vertexIndex < s_ipVertLo) s_ipVertLo = g_vertexIndex;
	if (g_vertexIndex + 6 > s_ipVertHi) s_ipVertHi = g_vertexIndex + 6;
}

/* GL state sampler lives in PsyX_render.cpp (this TU has no GL headers). */
extern "C" void PsyX_ItemProbe_ReadGlDepthState(int* depthTest, int* depthFunc, int* depthMask,
                                                int* depthBits, float* rangeNear, float* rangeFar,
                                                int* fbo, int* cullFace, int* trackerDepthMode);

static void ItemProbe_RecordSplitGl(int splitIdx, int blend, int numVerts, int dfe)
{
	if (s_ipGlCount >= ITEMPROBE_MAX_GL)
		return;
	ItemProbeGl& g = s_ipGl[s_ipGlCount++];
	g.split = splitIdx; g.blend = blend; g.numVerts = numVerts; g.dfe = dfe;
	g.forceItemDepth = g_PsyX_ForceItemDepth;
	PsyX_ItemProbe_ReadGlDepthState(&g.depthTest, &g.depthFunc, &g.depthMask, &g.depthBits,
	                                &g.rangeNear, &g.rangeFar, &g.fbo, &g.cullFace,
	                                &g.trackerDepthMode);
}

static const char* ItemProbe_KindName(int k)
{
	switch (k)
	{
		case -1:             return "MISS";
		case SZ_KIND_NONE:   return "NONE";
		case SZ_KIND_FLAT:   return "FLAT";
		case SZ_KIND_EXACT:  return "EXCT";
		default:             return "????";
	}
}

static const char* ItemProbe_FuncName(int f)
{
	switch (f)
	{
		case 0x0200: return "NEVER";
		case 0x0201: return "LESS";
		case 0x0202: return "EQUAL";
		case 0x0203: return "LEQUAL";
		case 0x0204: return "GREATER";
		case 0x0205: return "NOTEQUAL";
		case 0x0206: return "GEQUAL";
		case 0x0207: return "ALWAYS";
		default:     return "?";
	}
}

static void ItemProbe_Dump(void)
{
	const char* scr = (s_ipScreen == 1) ? "TAKE"
	                : (s_ipScreen == 2) ? "INV"
	                : (s_ipScreen == 3) ? "PUZZLE" : "?";

	/* Window depth uses the queried glDepthRange; the renderer's ortho has
	 * znear=-1/zfar=+1, so clip.z = -vertex.z and win = n + (1-z)/2*(f-n).
	 * Higher win = FARTHER. */
	float rn = 0.0f, rf = 1.0f;
	if (s_ipGlCount > 0) { rn = s_ipGl[0].rangeNear; rf = s_ipGl[0].rangeFar; }

	int parsed = 0, fired = 0, miss = 0;
	int kNone = 0, kFlat = 0, kExact = 0;
	float zMinAll = 1e30f, zMaxAll = -1e30f;
	float seedMin = 1e30f, seedMax = -1e30f;
	for (int i = 0; i < s_ipCount; i++)
	{
		const ItemProbeRec& r = s_ipRec[i];
		if (r.seq < 0) continue;
		parsed++;
		if (r.fired) fired++;
		if (r.kind < 0) miss++;
		else if (r.kind == SZ_KIND_NONE)  kNone++;
		else if (r.kind == SZ_KIND_FLAT)  kFlat++;
		else if (r.kind == SZ_KIND_EXACT) kExact++;
		if (r.zSeed < seedMin) seedMin = r.zSeed;
		if (r.zSeed > seedMax) seedMax = r.zSeed;
		for (int v = 0; v < r.nv; v++)
		{
			if (r.zOut[v] < zMinAll) zMinAll = r.zOut[v];
			if (r.zOut[v] > zMaxAll) zMaxAll = r.zOut[v];
		}
	}
	if (parsed == 0) { zMinAll = zMaxAll = seedMin = seedMax = 0.0f; }

	eprintinfo("[ITEMDEPTH] ================ ENTRY screen=%s models=%d prims=%d parsed=%d dropped=%d foreignPrims=%d\n",
		scr, s_ipModels, s_ipCount, parsed, s_ipDropped, s_ipForeign);
	eprintinfo("[ITEMDEPTH] env: pgxp=%d pgxpZBuffer=%d szMaxPrevFrame=%u otBucketsDrawn=%d otViewZShift=%d worldDepth=%d\n",
		g_PsxUsePgxp, g_cfg_pgxpZBuffer, (unsigned)g_szMaxPrevFrame,
		g_currentOTBucketCount, s_otViewZShift, g_PsxPgxpWorldDepth);

	for (int g = 0; g < s_ipGlCount; g++)
	{
		const ItemProbeGl& q = s_ipGl[g];
		eprintinfo("[ITEMDEPTH] GL@draw split=%d verts=%d blend=%d dfe=%d | depthTest=%d func=%s mask=%d bits=%d range=[%.4f,%.4f] fbo=%d cullFace=%d | tracker(g_PreviousDepthMode)=%d forceItemDepth=%d\n",
			q.split, q.numVerts, q.blend, q.dfe,
			q.depthTest, ItemProbe_FuncName(q.depthFunc), q.depthMask, q.depthBits,
			q.rangeNear, q.rangeFar, q.fbo, q.cullFace,
			q.trackerDepthMode, q.forceItemDepth);
	}
	if (s_ipGlCount == 0)
		eprintinfo("[ITEMDEPTH] GL@draw NONE - no split rasterized a probed primitive\n");

	const int nModels = (s_ipModels < ITEMPROBE_MAX_MODELS) ? s_ipModels : ITEMPROBE_MAX_MODELS;
	for (int m = 0; m < nModels; m++)
	{
		eprintinfo("[ITEMDEPTH] MODEL m=%d slot=%d arg2=%d primsSorted=%d\n",
			m, s_ipModelSlot[m], s_ipModelArg2[m], s_ipModelPrims[m]);
	}

	eprintinfo("[ITEMDEPTH] hdr  m  idx code nv    sz0    sz1    sz2    sz3      ir0    otz otL sh   seq kind fire     zSeed      zMin      zMax    winMin    winMax\n");
	for (int i = 0; i < s_ipCount; i++)
	{
		const ItemProbeRec& r = s_ipRec[i];
		float zmin = 0.0f, zmax = 0.0f, wmin = 0.0f, wmax = 0.0f;
		if (r.seq >= 0)
		{
			zmin = zmax = r.zOut[0];
			for (int v = 1; v < r.nv; v++)
			{
				if (r.zOut[v] < zmin) zmin = r.zOut[v];
				if (r.zOut[v] > zmax) zmax = r.zOut[v];
			}
			/* win is monotone DECREASING in z, so max z -> min win. */
			wmin = rn + (1.0f - zmax) * 0.5f * (rf - rn);
			wmax = rn + (1.0f - zmin) * 0.5f * (rf - rn);
		}
		eprintinfo("[ITEMDEPTH] row %2d %4d 0x%02X %2d %6u %6u %6u %6u %8ld %6d %3d %2d %5d %4s   %c %9.5f %9.5f %9.5f %9.6f %9.6f\n",
			(int)r.model, (int)r.primIdx, (unsigned)r.code, (int)r.nv,
			r.sz[0], r.sz[1], r.sz[2], r.sz[3],
			r.ir0, r.otz, (int)r.otLen, (int)r.shift,
			r.seq, ItemProbe_KindName(r.kind), r.fired ? 'Y' : 'N',
			r.zSeed, zmin, zmax, wmin, wmax);
	}

	eprintinfo("[ITEMDEPTH] SUM screen=%s parsed=%d fired=%d bucketFallback=%d lookupMiss=%d kindNONE=%d kindFLAT=%d kindEXACT=%d\n",
		scr, parsed, fired, parsed - fired, miss, kNone, kFlat, kExact);
	eprintinfo("[ITEMDEPTH] SUM zSeed=[%.5f..%.5f] zOut=[%.5f..%.5f] win=[%.6f..%.6f] (higher win = FARTHER)\n",
		seedMin, seedMax, zMinAll, zMaxAll,
		rn + (1.0f - zMaxAll) * 0.5f * (rf - rn),
		rn + (1.0f - zMinAll) * 0.5f * (rf - rn));
	eprintinfo("[ITEMDEPTH] ================ END screen=%s\n", scr);
}

/* Called unconditionally from game_main.c right after the OT0 GsDrawOt +
 * force-item-depth bracket. Dumps an armed capture and re-arms when the
 * item screen has been left. */
extern "C" void PsyX_ItemProbeEndFrame(void)
{
	if (!g_PsyX_ItemDepthProbe)
		return;

	if (s_ipArmed)
	{
		/* An entry's first frame(s) can announce a model before its TMD is
		 * linked, sorting nothing. Spending the one-shot on an empty dump would
		 * lose the real capture, so only a frame that actually sorted primitives
		 * consumes it; otherwise ModelBegin restarts the capture next frame. */
		if (s_ipCount > 0)
		{
			ItemProbe_Dump();
			s_ipArmed = 0;
			s_ipCaptured = 1;
		}
	}
	if (!s_ipSawModel)
	{
		s_ipCaptured = 0;   /* screen gone — the next entry re-arms */
		s_ipArmed = 0;
	}
	s_ipSawModel = 0;
}

/* Probe wrapper. With the flag off this is a straight tail call. */
static void ApplyGtePerVertexDepth(GrVertex* vertex, const P_TAG* polyTag, bool isQuad)
{
	if (!g_PsyX_ItemDepthProbe || !s_ipArmed)
	{
		ApplyGtePerVertexDepthImpl(vertex, polyTag, isQuad);
		return;
	}
	const float zBefore = vertex[0].z; /* == g_otBucketDepth, just written by MakeVertex* */
	ApplyGtePerVertexDepthImpl(vertex, polyTag, isQuad);
	ItemProbe_RecordDraw(vertex, polyTag, isQuad, zBefore);
}

enum GPUDrawSplitKind { GPU_SPLIT_LEGACY, GPU_SPLIT_MODERN, GPU_SPLIT_FBCAPTURE, GPU_SPLIT_FARMESH };

struct GPUDrawSplit
{
	GPUDrawSplitKind kind;
	unsigned int	modernHandle;
	DRAWENV			drawenv;
	DISPENV			dispenv;

	BlendMode		blendMode;

	TexFormat		texFormat;
	TextureID		textureId;
	TextureID		normalTextureId;

	int				drawPrimMode;
	int				depthMode; /* SplitDepthMode; always DISABLED when PGXP off (byte-identical) */

	unsigned int	startVertex; /* widened from u_short: MAX_VERTEX_BUFFER_SIZE now > 65535 */
	unsigned int	numVerts;

	/* Hi-res override texture dims for this split (0 = unknown) — the
	 * shader's per-native-texel footprint clamp needs the upscale factor. */
	int				overrideHiresW;
	int				overrideHiresH;

	int				isGlyph; /* see g_PsyX_TextGlyphTest */
	int				mixed;   /* drawn with the all-format program (g_gte_shader_mix) */

	const char*		debugText;
};

/* 4096 was reached in ordinary play on a dense outdoor map (a user capture hit
 * it on 89% of its log lines). Every split past the cap is DROPPED geometry, so
 * the frame renders partially -- the black/flickering frames and the "expect
 * rendering errors" spam. ~180 bytes each, so 16384 costs ~3 MB. */
#define MAX_DRAW_SPLITS	 16384

GrVertex g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
GPUDrawSplit g_splits[MAX_DRAW_SPLITS];

int g_vertexIndex = 0;
int g_splitIndex = 0;

/* Per-frame parsed-vertex tally for the grey-flash probe: a 1-frame grey flash
 * is a fog-coloured clear presented with a near-empty world, and this is the
 * cheap signal for "near-empty". Accumulated across ClearSplits calls (several
 * per frame), published/zeroed at GR_SwapWindow. */
extern "C" { int g_PsxFrameVerts = 0; int g_PsxLastFrameVerts = 0; }

void ClearSplits()
{
	currentSplitDebugText = nullptr;
	g_PsxFrameVerts += g_vertexIndex;
	g_vertexIndex = 0;
	s_ppLitCursor = 0;
	g_splitIndex = 0;
	g_splits[0].kind = GPU_SPLIT_LEGACY;
	g_splits[0].texFormat = (TexFormat)0xFFFF;
	/* Don't let a hi-res override leak across frames. Restoring the
	 * DR_PSYX_TEX packet state (instead of zeroing) keeps that path's
	 * persist-until-changed semantics; it's a no-op when unused. */
	overrideTexture = drPsyxTexOverride;
	overrideTextureWidth = drPsyxTexOverrideWidth;
	overrideTextureHeight = drPsyxTexOverrideHeight;
	overrideTextureOffsetX = 0;
	overrideTextureOffsetY = 0;
	overrideTextureHiresW = 0;
	overrideTextureHiresH = 0;
}

template<class T>
void DrawEnvDimensions(T& width, T& height)
{
	if (activeDrawEnv.dfe)
	{
		width = activeDispEnv.disp.w;
		height = activeDispEnv.disp.h;
	}
	else
	{
		width = activeDrawEnv.clip.w;
		height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float& ofsX, float& ofsY)
{
	if (activeDrawEnv.dfe)
	{
		// also make offset in draw dimensions range to prevent flicker
		const int x = activeDispEnv.disp.x;
		const int y = activeDispEnv.disp.y;
		ofsX = activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		ofsY = activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		ofsX = 0.0f;
		ofsY = 0.0f;
	}
}

extern "C" void PsyX_GetDrawEnvOffset(float* x, float* y)
{
	if (x != nullptr && y != nullptr)
		DrawEnvOffset(*x, *y);
}

inline void ScreenCoordsToEmulator(GrVertex* vertex, int count)
{
}

void LineSwapSourceVerts(VERTTYPE*& p0, VERTTYPE*& p1, unsigned char*& c0, unsigned char*& c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if ((p0[0] > p1[0]) ||
		(p0[1] > p1[1] && p0[0] == p1[0]))
	{
		VERTTYPE* tmp = p0;
		p0 = p1;
		p1 = tmp;

		unsigned char* tmpCol = c0;
		c0 = c1;
		c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, ushort gteidx)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	if (dx > abs((short)dy)) 
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else 
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	ScreenCoordsToEmulator(vertex, 4);
}

/* View-space data feeds two consumers only: the per-pixel flashlight (a few
 * metres of reach) and the near-plane clipper (vsz < g_PgxpNearZ). A prim whose
 * every vertex projected precisely past 64u needs neither, and its four view-
 * space lookups were half the per-prim parse cost of a whole-town frame -- the
 * OT walk visits prims in depth order, so each is a cache miss. Callers fill
 * PGXP first so ppw is known here. */
static inline bool PgxpAllBeyondViewSpace(const GrVertex* v, int n)
{
	if (!g_PsxUsePgxp)
		return false;
	for (int i = 0; i < n; i++)
		if (!(v[i].ppw > 16384.0f))
			return false;
	return true;
}

void MakeVertexTriangle(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[0].z = vertex[1].z = vertex[2].z = g_otBucketDepth;

	/* On a frame that draws the 3D world, a POLY primitive IS world geometry.
	 * The 2D content the game puts through the ordering table is SPRT and TILE,
	 * neither of which reaches this function, and the fullscreen 2D screens that
	 * do use POLY are exactly the frames g_PsxFrame3dClass excludes. Seeding the
	 * marker here is what stops a primitive whose vertices all missed the shadow
	 * lookup from rendering point-sampled beside filtered neighbours: walls and
	 * tree quads coming out blocky at 16x anisotropic. The lookup below still
	 * runs and still sets the marker; this only removes the dependency on it
	 * resolving, which it does for ~82% of vertices. */
	if (g_PsxFrame3dClass && !g_PsyX_ForceItemDepth && !OtBucketIsUi2d())
		vertex[0].geom3d = vertex[1].geom3d = vertex[2].geom3d = 1.0f;

	/* Precise projection first, so the view-space fill can be skipped for prims
	 * wholly past its consumers' reach; the near-clip test after it reads that
	 * data. The isolated item model skips view space: it must not join the
	 * world's per-pixel flashlight (it doesn't cross the near plane, so losing
	 * near-clip eligibility is harmless). */
	if (g_PsxUsePgxp)
	{
		PgxpFillVertex(&vertex[0], p0, p0[0], p0[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[1], p1, p1[0], p1[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[2], p2, p2[0], p2[1], ofsX, ofsY);
	}

	if (GR_NeedViewSpaceData() && !g_PsyX_ForceItemDepth && !PgxpAllBeyondViewSpace(vertex, 3))
	{
		VsFillVertex(&vertex[0], p0);
		VsFillVertex(&vertex[1], p1);
		VsFillVertex(&vertex[2], p2);

		/* Per-PRIMITIVE 3D marker. The lookup resolves ~84% of vertices, and
		 * geom3d reaches the fragment stage as a varying -- so a prim with a
		 * mix of resolved and unresolved vertices would interpolate across 0.5
		 * and split one surface into filtered and unfiltered halves. One
		 * resolved vertex is proof the whole primitive came from the GTE. */
		if (vertex[0].ny > 0.5f || vertex[1].ny > 0.5f || vertex[2].ny > 0.5f)
			vertex[0].geom3d = vertex[1].geom3d = vertex[2].geom3d = 1.0f;
	}

	if (g_PsxUsePgxp)
	{
		/* Per-poly consistency: if ANY vertex fell to affine (ppw<=0 — at/behind the
		 * near plane, where there's no valid perspective projection), drop the WHOLE
		 * poly to affine. A poly with some verts perspective and some affine shears at
		 * the screen edge (the grazing-angle case); consistent affine matches PSX.
		 * EXCEPT when the near clipper will split this straddling poly — it needs the
		 * in-front vertices' precise projections kept intact. */
		if ((vertex[0].ppw <= 0.0f || vertex[1].ppw <= 0.0f || vertex[2].ppw <= 0.0f) &&
		    !PgxpNearClipEligible(vertex, 3))
			vertex[0].ppw = vertex[1].ppw = vertex[2].ppw = 0.0f;
	}

	ScreenCoordsToEmulator(vertex, 3);
}

void MakeVertexQuad(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, VERTTYPE* p3, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	/* Frame-class 3D marker, same reasoning as MakeVertexTriangle. */
	if (g_PsxFrame3dClass && !g_PsyX_ForceItemDepth && !OtBucketIsUi2d())
	{
		vertex[0].geom3d = vertex[1].geom3d =
		vertex[2].geom3d = vertex[3].geom3d = 1.0f;
	}

	/* Precise projection first, so the view-space fill can be skipped for prims
	 * wholly past its consumers' reach; the near-clip test after it reads that
	 * data. The isolated item model skips view space so it stays out of the
	 * world's per-pixel flashlight (see MakeVertexTriangle). */
	if (g_PsxUsePgxp)
	{
		PgxpFillVertex(&vertex[0], p0, p0[0], p0[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[1], p1, p1[0], p1[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[2], p2, p2[0], p2[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[3], p3, p3[0], p3[1], ofsX, ofsY);
	}

	if (GR_NeedViewSpaceData() && !g_PsyX_ForceItemDepth && !PgxpAllBeyondViewSpace(vertex, 4))
	{
		VsFillVertex(&vertex[0], p0);
		VsFillVertex(&vertex[1], p1);
		VsFillVertex(&vertex[2], p2);
		VsFillVertex(&vertex[3], p3);

		/* Per-primitive, same reasoning as MakeVertexTriangle. */
		if (vertex[0].ny > 0.5f || vertex[1].ny > 0.5f ||
		    vertex[2].ny > 0.5f || vertex[3].ny > 0.5f)
		{
			vertex[0].geom3d = vertex[1].geom3d =
			vertex[2].geom3d = vertex[3].geom3d = 1.0f;
		}
	}

	if (g_PsxUsePgxp)
	{
		/* Per-poly consistency (see MakeVertexTri): any affine vertex -> whole poly
		 * affine — unless the near clipper will split this straddling poly. */
		if ((vertex[0].ppw <= 0.0f || vertex[1].ppw <= 0.0f ||
		     vertex[2].ppw <= 0.0f || vertex[3].ppw <= 0.0f) &&
		    !PgxpNearClipEligible(vertex, 4))
			vertex[0].ppw = vertex[1].ppw = vertex[2].ppw = vertex[3].ppw = 0.0f;
	}

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexRect(GrVertex* vertex, VERTTYPE* p0, short w, short h, ushort gteidx)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	/* Widescreen feedback: the effect's strips are authored 320 wide and would
	 * otherwise blur the 4:3 core and leave the margins sharp, with a hard seam
	 * down each side. Stretch them across the widened ortho; the capture is
	 * stretched to match, so the loop stays 1:1. No-op at 4:3.
	 *
	 * UI pass only. The scale is the UI ortho's, and the capture widens only for
	 * a UI-pass reader. The loading trail draws in the WORLD pass and also runs
	 * at room transitions, where the UI ortho is widened: stretching its strips
	 * over an unstretched capture rescales the loop every frame, which is the
	 * vertical-streak class this store has hit before. */
	/* The scene-scratch soft focus (Alessa, Lisa) is the same idea in the WORLD
	 * pass: its strips are 320 wide and would soften only the 4:3 core. Stretch
	 * them across the world ortho; GR_CaptureFrameToVramRect captures the full
	 * width to match, so the composite stays 1:1 with the scene under it. */
	if (s_curPrimIsScratch && !g_PsxUIOrthoPass && g_PsxWorldOrthoValid && g_PsxWorldDisp[0] > 0.0f)
	{
		const float c = (g_PsxWorldOrtho[0] + g_PsxWorldOrtho[1]) * 0.5f;
		const float k = (g_PsxWorldOrtho[1] - g_PsxWorldOrtho[0]) / g_PsxWorldDisp[0];
		if (k > 1.001f)
		{
			int i;
			for (i = 0; i < 4; i++)
				vertex[i].x = (short)floorf(c + ((float)vertex[i].x - c) * k + 0.5f);
		}
	}

	if (s_curPrimIsFeedback && g_PsxUIOrthoPass && g_PsxFeedbackWideScale > 1.001f)
	{
		int i;
		for (i = 0; i < 4; i++)
		{
			const float fx = g_PsxFeedbackWideCenter +
			                 ((float)vertex[i].x - g_PsxFeedbackWideCenter) * g_PsxFeedbackWideScale;
			vertex[i].x = (short)floorf(fx + 0.5f);
		}
	}

	/* [FBGEOM] one-shot: where a feedback strip actually lands, in PSX display
	 * coordinates, against the ortho it is drawn under. Chasing the unblurred
	 * sliver along the bottom edge -- the strips are authored 224 tall, so if
	 * they stop short of the ortho's bottom this line says by how much and
	 * whether the draw-env offset or the display height is responsible. */
	/* One line per ortho pass, so the loading screen (world pass) cannot use up
	 * the budget before an in-scene overlay (UI pass) is ever drawn. */
	if (s_curPrimIsFeedback)
	{
		static int s_fbGeomLogged[2] = { 0, 0 };
		const int  pass = g_PsxUIOrthoPass ? 1 : 0;
		if (!s_fbGeomLogged[pass])
		{
			s_fbGeomLogged[pass] = 1;
			eprintinfo("[FBGEOM] %s strip x %d..%d y %d..%d  ofs %.1f,%.1f  wide %.4f about %.1f\n",
				pass ? "ui" : "world",
				(int)vertex[0].x, (int)vertex[2].x, (int)vertex[0].y, (int)vertex[2].y,
				ofsX, ofsY, g_PsxFeedbackWideScale, g_PsxFeedbackWideCenter);
		}
	}

	ScreenCoordsToEmulator(vertex, 4);
}


/* A flat fill authored to cover the whole PSX frame -- a scene tint, a flash --
 * drawn in the WORLD pass. That pass's ortho is widened for Hor+ and cropped
 * vertically by g_PsxWorldVScale, both of which exist for 3D geometry, so the
 * fill came out as a 4:3 box that stopped short of the bottom: the blue wash in
 * the carousel's Flauros scene (map6_s04 func_800E74C4, a 320x224 TILE in OT0).
 * Remap it onto the world ortho's own extent, which is exactly the picture.
 *
 * Untextured only, and only a rect that covers the entire display: a flat
 * colour has no aspect to distort, whereas a full-frame TEXTURED sprite may be a
 * picture authored for 4:3 and must not be stretched. Identity in 4:3, in the UI
 * pass (whose ortho is already the picture) and on 2D screens. */
static inline void StretchFullFrameFillToWorldOrtho(GrVertex* v)
{
	float x0, x1, y0, y1;
	float L, R, T, B, W, H;
	int   i;

	if (g_PsxUIOrthoPass || !g_PsxWorldOrthoValid)
		return;

	L = g_PsxWorldOrtho[0]; R = g_PsxWorldOrtho[1];
	T = g_PsxWorldOrtho[2]; B = g_PsxWorldOrtho[3];
	W = g_PsxWorldDisp[0];  H = g_PsxWorldDisp[1];

	if (W <= 0.0f || H <= 0.0f || R <= L || B <= T)
		return;
	if (fabsf(L) < 0.01f && fabsf(R - W) < 0.01f && fabsf(T) < 0.01f && fabsf(B - H) < 0.01f)
		return;

	x0 = x1 = (float)v[0].x;
	y0 = y1 = (float)v[0].y;
	for (i = 1; i < 4; i++)
	{
		if (v[i].x < x0) x0 = (float)v[i].x;
		if (v[i].x > x1) x1 = (float)v[i].x;
		if (v[i].y < y0) y0 = (float)v[i].y;
		if (v[i].y > y1) y1 = (float)v[i].y;
	}

	if (!(x0 <= 0.0f && x1 >= W && y0 <= 0.0f && y1 >= H))
		return;

	/* Round OUTWARD: the ortho edges are fractional (-57.4 at 16:9), and rounding
	 * a corner inward leaves a column of unfilled pixels down the frame edge. */
	for (i = 0; i < 4; i++)
	{
		const float fx = L + (float)v[i].x * (R - L) / W;
		const float fy = T + (float)v[i].y * (B - T) / H;
		v[i].x = (short)(((float)v[i].x <= x0) ? floorf(fx) : ceilf(fx));
		v[i].y = (short)(((float)v[i].y <= y0) ? floorf(fy) : ceilf(fy));
	}
}

/* Stamp the polygon's UV bounding box on every vertex (see GrVertex.ulo). */
static inline void SetUvLimits(GrVertex* vertex, int count)
{
	unsigned char ulo = vertex[0].u, uhi = vertex[0].u;
	unsigned char vlo = vertex[0].v, vhi = vertex[0].v;
	int i;

	for (i = 1; i < count; i++)
	{
		if (vertex[i].u < ulo) ulo = vertex[i].u;
		if (vertex[i].u > uhi) uhi = vertex[i].u;
		if (vertex[i].v < vlo) vlo = vertex[i].v;
		if (vertex[i].v > vhi) vhi = vertex[i].v;
	}

	for (i = 0; i < count; i++)
	{
		vertex[i].ulo = ulo; vertex[i].vlo = vlo;
		vertex[i].uhi = uhi; vertex[i].vhi = vhi;
	}
}

/* Vertex page: tpage X/Y (bits 0-4) plus the tpage format (bits 7-8) moved to
 * bits 5-6, so the all-format program can pick its sampler per vertex. The
 * per-format programs only read bits 0-4. Untextured prims use
 * TEXCOORD_PAGE_UNTEXTURED. */
#define TEXCOORD_PAGE_UNTEXTURED 0x80
static inline short TexcoordPage(int tpage)
{
	return (short)((tpage & 0x1F) | (((tpage >> 7) & 0x3) << 5));
}

void MakeTexcoordQuad(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, unsigned char* uv3, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const unsigned char bright = 2;
	short pageCoord = TexcoordPage(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = pageCoord;
	vertex[3].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/

	SetUvLimits(vertex, 4);
}

void MakeTexcoordTriangle(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const unsigned char bright = 2;
	short pageCoord = TexcoordPage(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/

	SetUvLimits(vertex, 3);
}

void MakeTexcoordRect(GrVertex* vertex, unsigned char* uv, short page, short clut, short w, short h)
{
	assert(uv);

	/* A sprite whose far edge lands EXACTLY on the page edge (u+w or v+h == 256)
	 * is not an overflow: its last texel is 255, and PSX reads 0..255 exactly.
	 * Only the edge coordinate, one past the last texel, fails to fit in the u8
	 * vertex field. Clamping w/h to 255-uv there kept the full screen size but
	 * sampled one texel fewer, stretching the sprite by a texel -- and a
	 * framebuffer-feedback strip (256 wide from u=0; 224 tall from v=32 on
	 * buffer 0) then rescales the stored frame on EVERY pass: the smear and the
	 * lost bottom line on the dream blur, the sideways stretch on the loading
	 * trail. Store 255 and carry the missing texel in the per-vertex texcoord
	 * offset instead (a_extra.xy * 0.5 in the vertex shader, so 2 = +1 texel).
	 * Genuine overflows past 256 keep the old clamp. */
	int extendU = 0, extendV = 0;
	/* The sprite's own texels end one BEFORE u+w / v+h -- the far edge is
	 * exclusive and the hardware never reads it. Only meaningful when the
	 * size went through untouched. */
	const int lastU = (int(uv[0]) + w <= 255 && w > 0) ? int(uv[0]) + w - 1 : -1;
	const int lastV = (int(uv[1]) + h <= 255 && h > 0) ? int(uv[1]) + h - 1 : -1;

	if (int(uv[0]) + w == 256)     { w = 255 - uv[0]; extendU = 1; }
	else if (int(uv[0]) + w > 255) { w = 255 - uv[0]; }
	if (int(uv[1]) + h == 256)     { h = 255 - uv[1]; extendV = 1; }
	else if (int(uv[1]) + h > 255) { h = 255 - uv[1]; }

	const unsigned char bright = 2;
	const unsigned char dither = 0;
	short pageCoord = TexcoordPage(page);

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv[0];
	vertex[1].v = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv[0] + w;
	vertex[2].v = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;

	vertex[3].u = uv[0] + w;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = pageCoord;
	vertex[3].clut = clut;

	/* Vertex order matches MakeVertexRect: 0 top-left, 1 bottom-left,
	 * 2 bottom-right, 3 top-right. */
	if (extendU)
	{
		vertex[2].tcx = 2;
		vertex[3].tcx = 2;
	}
	if (extendV)
	{
		vertex[1].tcy = 2;
		vertex[2].tcy = 2;
	}

	/* An upstream half-texel UV nudge used to sit here, applied to every RECT
	 * whenever filtering was enabled: tcx/tcy reach the vertex shader as
	 * a_extra.xy and it adds a_extra.xy * 0.5 to the texture coordinate.
	 *
	 * RECTs are how 2D sprites and TEXT GLYPHS are drawn, and font atlases pack
	 * their cells edge to edge with no gutter -- so half a texel over lands
	 * inside the neighbouring glyph and draws a slice of it beside the letter,
	 * with the whole 2D layer shifted down and right. That is the reported
	 * corruption, and it appeared for every mode except Off and Dithering
	 * because those are the only two that leave this flag clear.
	 *
	 * It also could not have been doing any good: the gate reports 0 on menu
	 * frames, so those glyphs are point-sampled anyway and were paying the
	 * offset for filtering they never received. The two sibling call sites in
	 * MakeTexcoordQuad/Triangle were already commented out for what looks like
	 * the same reason; this one was missed. The sampler now brackets its taps
	 * around P - 0.5 itself, so nothing needs compensating here. */

	SetUvLimits(vertex, 4);

	/* The filter's taps are clamped to this box, and a box ending at u+w / v+h
	 * let them read the texel past the sprite: whatever VRAM held beside it,
	 * drawn as a coloured line down the edge of the loading-screen picture
	 * (and anything else built from abutting sprites). End it at the last
	 * texel the sprite really shows. */
	for (int i = 0; i < 4; i++)
	{
		if (lastU >= 0) vertex[i].uhi = (unsigned char)lastU;
		if (lastV >= 0) vertex[i].vhi = (unsigned char)lastV;
	}
}

void MakeTexcoordLineZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = TEXCOORD_PAGE_UNTEXTURED;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex* vertex, int n)
{
	const unsigned char a = s_primAlphaPending;

	s_primAlphaPending = 255;
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = a;
		vertex[n]._p0 = 0;
		--n;
	}
}

void MakeColourLine(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1)
{
	const unsigned char _pa = s_primAlphaPending;

	s_primAlphaPending = 255;

	if (!shadeTexOn)
	{
		/* Hand the taken alpha back: NoShade takes the stash itself, and the
		 * take at this function's head already cleared it. */
		s_primAlphaPending = _pa;
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = _pa;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = _pa;
	vertex[1]._p0 = 0;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = _pa;
	vertex[2]._p0 = 0;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = _pa;
	vertex[3]._p0 = 0;
}

void MakeColourTriangle(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2)
{
	const unsigned char _pa = s_primAlphaPending;

	s_primAlphaPending = 255;

	if (!shadeTexOn)
	{
		/* Hand the taken alpha back: NoShade takes the stash itself, and the
		 * take at this function's head already cleared it. */
		s_primAlphaPending = _pa;
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = _pa;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = _pa;
	vertex[1]._p0 = 0;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = _pa;
	vertex[2]._p0 = 0;
}

void MakeColourQuad(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2, unsigned char* col3)
{
	const unsigned char _pa = s_primAlphaPending;

	s_primAlphaPending = 255;

	if (!shadeTexOn)
	{
		/* Hand the taken alpha back: NoShade takes the stash itself, and the
		 * take at this function's head already cleared it. */
		s_primAlphaPending = _pa;
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = _pa;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = _pa;
	vertex[1]._p0 = 0;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = _pa;
	vertex[2]._p0 = 0;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = _pa;
	vertex[3]._p0 = 0;
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	g_vertexBuffer[g_vertexIndex + 4] = g_vertexBuffer[g_vertexIndex + 3];

	g_vertexBuffer[g_vertexIndex + 5] = g_vertexBuffer[g_vertexIndex + 2];
	g_vertexBuffer[g_vertexIndex + 2] = g_vertexBuffer[g_vertexIndex + 3];
	g_vertexBuffer[g_vertexIndex + 3] = g_vertexBuffer[g_vertexIndex + 1];
}

/* ---- PGXP near-plane clipping (docs/PGXP_NearClip_Design.md) -----------------
 * Runs on the freshly-built triangle list of ONE 3D poly (3 verts, or 6 after
 * TriangulateQuad) before g_vertexIndex advances, and only for polys where every
 * vertex has a validated view-space entry and at least one sits on each side of
 * z = g_PgxpNearZ. Each triangle is Sutherland-Hodgman clipped against that plane
 * in view space; attributes interpolate along the crossing edges (linear in view
 * space = perspective-correct for UV/position; RGB differs slightly from PSX
 * screen-space Gouraud, acceptable — PSX never drew these polys correctly at
 * all); the resulting 0..4-vertex polygon is fan-triangulated back in place.
 * Only the GL vertex stream changes: the prim's integer data, OT position and
 * split (texture/blend state) are untouched, so painter's order is unaffected. */

/* Build the clip vertex where edge a->b crosses z = g_PgxpNearZ. Non-interpolated
 * fields (page/clut/bright/dither/tcx/tcy, flat prim z, nocast) copy from a. */
static void PgxpNearClipLerp(const GrVertex* a, const GrVertex* b, GrVertex* out, float ofsX, float ofsY)
{
	const float t = (g_PgxpNearZ - a->vsz) / (b->vsz - a->vsz);

	*out = *a;
	out->vsx = a->vsx + (b->vsx - a->vsx) * t;
	out->vsy = a->vsy + (b->vsy - a->vsy) * t;
	out->vsz = g_PgxpNearZ;

	/* Re-project with the GTE's own formula (sx = OFX + x*H/z), landing in the
	 * same space PgxpFillVertex stores (draw-env offset included). W = view z,
	 * the same unquantized scale pgxpW uses, so 1/W interpolation lines up with
	 * the kept vertices. No g_PgxpEdgeMax clamp: with a true W the GPU clips
	 * far-off-screen positions exactly in homogeneous space; clamping would drag
	 * the vertex and distort the visible part. */
	const float pOfx = s_curPolyProjValid ? s_curPolyOfx : g_PgxpGteOfx;
	const float pOfy = s_curPolyProjValid ? s_curPolyOfy : g_PgxpGteOfy;
	const float pH   = s_curPolyProjValid ? s_curPolyH   : g_PgxpGteH;

	const float hz = pH / g_PgxpNearZ;
	out->ppx = pOfx + out->vsx * hz + ofsX;
	out->ppy = pOfy + out->vsy * hz + ofsY;
	out->ppw = g_PgxpNearZ;

	/* Integer x/y are unread on the PGXP shader path (ppw>0) — keep them sane
	 * for debug views; a raw float->short cast of a huge coord is UB. */
	out->x = (short)(out->ppx < -32767.0f ? -32767.0f : (out->ppx > 32767.0f ? 32767.0f : out->ppx));
	out->y = (short)(out->ppy < -32767.0f ? -32767.0f : (out->ppy > 32767.0f ? 32767.0f : out->ppy));

	out->u = (u_char)((float)a->u + ((float)b->u - (float)a->u) * t + 0.5f);
	out->v = (u_char)((float)a->v + ((float)b->v - (float)a->v) * t + 0.5f);
	out->r = (u_char)((float)a->r + ((float)b->r - (float)a->r) * t + 0.5f);
	out->g = (u_char)((float)a->g + ((float)b->g - (float)a->g) * t + 0.5f);
	out->b = (u_char)((float)a->b + ((float)b->b - (float)a->b) * t + 0.5f);
	out->a = (u_char)((float)a->a + ((float)b->a - (float)a->a) * t + 0.5f);
	/* Per-vertex fog rides _p0 (0..127). */
	out->_p0 = (char)((float)a->_p0 + ((float)b->_p0 - (float)a->_p0) * t + 0.5f);
}

/* A kept in-front vertex normally keeps its GTE-precise projection (bit-identical
 * to the unclipped case, so shared edges with neighbouring unclipped polys can't
 * crack). If the PGXP shadow missed it (ppw==0) but view-space is valid,
 * reconstruct the projection the same way the clip vertices get theirs. */
static void PgxpNearClipReproject(GrVertex* v, float ofsX, float ofsY)
{
	if (v->ppw > 0.0f)
		return;
	const float pOfx = s_curPolyProjValid ? s_curPolyOfx : g_PgxpGteOfx;
	const float pOfy = s_curPolyProjValid ? s_curPolyOfy : g_PgxpGteOfy;
	const float pH   = s_curPolyProjValid ? s_curPolyH   : g_PgxpGteH;

	const float hz = pH / v->vsz;
	v->ppx = pOfx + v->vsx * hz + ofsX;
	v->ppy = pOfy + v->vsy * hz + ofsY;
	v->ppw = v->vsz;
}

/* Clip the poly's triangle list in place; returns the new vertex count (a
 * multiple of 3; unchanged when the poly isn't eligible). Worst case growth is
 * 6 -> 12 verts (each straddling triangle yields up to 2). */
static int PgxpNearClipEmit(GrVertex* v, int count)
{
	if (!g_PsxUsePgxp || !PgxpNearClipEligible(v, count))
		return count;

	/* [PGXPCLIP]: the first dozen polys the clipper takes, per session, with
	 * each vertex's screen position, view depth and W. A far quad losing one
	 * corner (the mall screens) means one vertex carries a view depth that
	 * says "behind the eye"; this names which. Event-capped, never per frame. */
	{
		static int s_clipLog = 0;
		/* Only the suspicious kind: every vertex projects on-screen (a real
		 * straddler's behind-eye vertex saturates the GTE screen box) yet one
		 * still reads as behind the near plane. */
		bool onScreen = s_clipLog < 24;
		for (int j = 0; onScreen && j < count; j++)
			if (v[j].x < -400.0f || v[j].x > 400.0f || v[j].y < -300.0f || v[j].y > 300.0f)
				onScreen = false;
		if (onScreen && s_clipLog < 24)
		{
			s_clipLog++;
			eprintinfo("[PGXPCLIP] n=%d nearZ=%.1f | v0=(%.0f,%.0f z=%.0f w=%.3f) v1=(%.0f,%.0f z=%.0f w=%.3f) v2=(%.0f,%.0f z=%.0f w=%.3f) v3=(%.0f,%.0f z=%.0f w=%.3f)\n",
				count, (double)g_PgxpNearZ,
				(double)v[0].x, (double)v[0].y, (double)v[0].vsz, (double)v[0].ppw,
				(double)v[1].x, (double)v[1].y, (double)v[1].vsz, (double)v[1].ppw,
				(double)v[2].x, (double)v[2].y, (double)v[2].vsz, (double)v[2].ppw,
				(double)(count > 3 ? v[3].x : 0.0f), (double)(count > 3 ? v[3].y : 0.0f),
				(double)(count > 3 ? v[3].vsz : 0.0f), (double)(count > 3 ? v[3].ppw : 0.0f));
		}
	}

	/* Growth headroom: never write past the vertex buffer; keeping the
	 * unclipped poly stays within the pre-existing envelope. Force the whole
	 * poly affine (same rule as the guard-band bail below): an eligible poly
	 * straddles the near plane, and rasterizing behind-eye vertices with
	 * ppw>0 produces garbage — worse now that a marked world vertex would
	 * also derive per-vertex DEPTH from that ppw. Affine = flat z, one frame,
	 * imperceptible. */
	if (g_vertexIndex + 12 > MAX_VERTEX_BUFFER_SIZE)
	{
		for (int j = 0; j < count; j++)
			v[j].ppw = 0.0f;
		return count;
	}

	GrVertex out[12];
	int outCount = 0;

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	for (int tri = 0; tri + 2 < count; tri += 3)
	{
		GrVertex clipped[4];
		int m = 0;

		for (int i = 0; i < 3; i++)
		{
			const GrVertex* a = &v[tri + i];
			const GrVertex* b = &v[tri + (i + 1) % 3];
			const bool aIn = a->vsz >= g_PgxpNearZ;
			const bool bIn = b->vsz >= g_PgxpNearZ;

			if (aIn)
				clipped[m++] = *a;
			if (aIn != bIn)
				PgxpNearClipLerp(a, b, &clipped[m++], ofsX, ofsY);
		}

		for (int i = 0; i < m; i++)
			PgxpNearClipReproject(&clipped[i], ofsX, ofsY);

		/* Fan-triangulate (m is 0, 3 or 4; winding preserved by the clip). */
		for (int i = 2; i < m; i++)
		{
			out[outCount++] = clipped[0];
			out[outCount++] = clipped[i - 1];
			out[outCount++] = clipped[i];
		}
	}

	/* Guard-band bound on the clip result, checked while out[] is still local
	 * (v is unrecoverable after the memcpy). The ortho PGXP shader cancels W
	 * (NDC pos IS ppx/ppy), so an out-of-band clip vertex would rasterize as a
	 * screen-crossing wedge, not get clipped homogeneously. Abandon the clip:
	 * force the whole original poly affine and let PgxpEmitPoly's PSX size rule
	 * judge the GTE integer coords — net hardware behavior (poly not drawn). */
	for (int i = 0; i < outCount; i++)
	{
		if (out[i].ppx < -g_PgxpEdgeMax || out[i].ppx > g_PgxpEdgeMax ||
		    out[i].ppy < -g_PgxpEdgeMax || out[i].ppy > g_PgxpEdgeMax)
		{
			for (int j = 0; j < count; j++)
				v[j].ppw = 0.0f;
			return count;
		}
	}

	memcpy(v, out, outCount * sizeof(GrVertex));
	s_pgxpClip++;
	return outCount;
}

/* FIX 1 (PSX polygon size rule — see g_PsxPolySizeCull). Bbox test per
 * triangle, matching hardware, which processes a 4-point poly as two
 * independent triangles. x/y are PSX units (ScreenCoordsToEmulator is a no-op);
 * the shared draw-env offset cancels in the extent. */
static inline bool PsxTriOversized(const GrVertex* v)
{
	int mnx = v[0].x, mxx = v[0].x, mny = v[0].y, mxy = v[0].y;
	for (int i = 1; i < 3; i++)
	{
		if (v[i].x < mnx) mnx = v[i].x;
		if (v[i].x > mxx) mxx = v[i].x;
		if (v[i].y < mny) mny = v[i].y;
		if (v[i].y > mxy) mxy = v[i].y;
	}
	return (mxx - mnx) > 1023 || (mxy - mny) > 511;
}

/* Emit one 3D poly's freshly built triangle list: near-clip when eligible,
 * then the PSX size rule on whatever stayed affine. Returns the vertex count
 * g_vertexIndex advances by. The size rule runs ONLY when the whole poly is on
 * the integer path — every vertex ppw<=0 (PGXP off, or the whole-poly-affine
 * drop in MakeVertexTriangle/Quad) — never on precise polys, whose integer
 * coords may be saturated while ppx/ppy are valid, and never in whole-map far
 * mode (box saturation is the design there, a cull would eat the vista). The
 * rare mixed-ppw poly (near-clip headroom bail) is left as-is, pre-existing. */
static int PgxpEmitPoly(GrVertex* v, int count)
{
	count = PgxpNearClipEmit(v, count);

	if (!g_PsxPolySizeCull || g_PsxWholeMapFar)
		return count;

	for (int i = 0; i < count; i++)
	{
		if (v[i].ppw > 0.0f)
			return count;
	}

	int kept = 0;
	for (int tri = 0; tri + 2 < count; tri += 3)
	{
		if (PsxTriOversized(&v[tri]))
		{
			s_pgxpOversize++;
			continue;
		}
		if (kept != tri)
			memcpy(&v[kept], &v[tri], 3 * sizeof(GrVertex));
		kept += 3;
	}
	return kept;
}

//------------------------------------------------------------------------------------------------------------------------

/* Console `mixshader 0|1`. Ok is set at startup only if the program built. */
extern "C" { int g_PsxMixedShader = 1; int g_PsxMixedShaderOk = 0; }

static void AddSplit(bool semiTrans, bool textured, int depthMode = SPLIT_DEPTH_DISABLED)
{
	int tpage = activeDrawEnv.tpage;
	GPUDrawSplit& curSplit = g_splits[g_splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	if (s_curPrimIsScratch && !semiTrans && g_PsxFeedbackDampBlend < 1.0f)
		blendMode = BM_CONSTANT_ALPHA;
	TexFormat texFormat = GET_TPAGE_FORMAT(tpage);
	TextureID textureId = textured ? g_vramTexture : g_whiteTexture;

	if (textured && overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = overrideTexture;
	}

	/* Gated exactly like the colour handle above, NOT read straight off the
	 * global: an ungated read would key untextured/white splits on override
	 * state they never sample, fragmenting them once normal maps exist. */
	TextureID normalTextureId = (textured && overrideTexture != 0) ? overrideNormalTexture : 0;

	/* Every VRAM format and untextured prims share one program, so only a
	 * hi-res override (its own texture) still splits on texture. Tpage format
	 * 3 is reserved and keeps its old path. */
	const int mixed = (g_PsxMixedShader && g_PsxMixedShaderOk && (!textured || texFormat <= TF_16_BIT)) ? 1 : 0;
	if (mixed)
	{
		texFormat = TF_16_BIT;
		textureId = g_vramTexture;
	}

	// FIXME: compare drawing environment too?
	if (curSplit.kind == GPU_SPLIT_LEGACY &&
		curSplit.blendMode == blendMode &&
		curSplit.texFormat == texFormat &&
		curSplit.textureId == textureId &&
		/* a second bound texture needs its own key term: two surfaces sharing
		 * one colour texture but wanting different normal maps must not batch. */
		curSplit.normalTextureId == normalTextureId &&
		/* keep world-painter prims out of non-world batches (else GL_ALWAYS
		 * would leak onto actors sharing a texture). Always DISABLED when off. */
		curSplit.depthMode == depthMode &&
		/* tw.x/y carry the hi-res override UV offset: two chunks of the same
		 * override texture with different tpage origins must NOT batch
		 * together (same textureId!) or they'd share one offset uniform. */
		curSplit.drawenv.tw.x == overrideTextureOffsetX &&
		curSplit.drawenv.tw.y == overrideTextureOffsetY &&
		curSplit.drawPrimMode == g_DrawPrimMode &&
		curSplit.drawenv.clip.x == activeDrawEnv.clip.x &&
		curSplit.drawenv.clip.y == activeDrawEnv.clip.y &&
		curSplit.drawenv.clip.w == activeDrawEnv.clip.w &&
		curSplit.drawenv.clip.h == activeDrawEnv.clip.h &&
		curSplit.drawenv.dfe == activeDrawEnv.dfe &&
		curSplit.isGlyph == (textured ? s_curPrimIsGlyph : 0) &&
		curSplit.mixed == mixed &&
		curSplit.debugText == currentSplitDebugText)
	{
		return;
	}

	curSplit.numVerts = g_vertexIndex - curSplit.startVertex;

	if (g_splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		/* Bounded: this fires per DROPPED SPLIT, so an overflowing frame used to
		 * emit thousands of lines. That alone cost hundreds of ms a frame -- the
		 * reported "performance drops" were substantially this log write. */
		static unsigned s_dropped = 0, s_logged = 0;
		s_dropped++;
		if (s_logged < 5 || (s_dropped & 0xFFFu) == 0) {
			s_logged++;
			eprinterr("MAX_DRAW_SPLITS (%d) reached -- geometry dropped this frame; "
			          "%u splits dropped so far\n", MAX_DRAW_SPLITS, s_dropped);
		}
		return;
	}

	GPUDrawSplit& split = g_splits[++g_splitIndex];
	split.kind = GPU_SPLIT_LEGACY;
	split.blendMode = blendMode;
	split.texFormat = texFormat;
	split.textureId = textureId;
	split.normalTextureId = normalTextureId;
	split.drawPrimMode = g_DrawPrimMode;
	split.depthMode = depthMode;
	split.drawenv = activeDrawEnv;
	split.dispenv = activeDispEnv;
	split.debugText = currentSplitDebugText;
	split.isGlyph   = textured ? s_curPrimIsGlyph : 0;
	split.mixed     = mixed;

	split.drawenv.tw.w = overrideTextureWidth;
	split.drawenv.tw.h = overrideTextureHeight;
	split.drawenv.tw.x = overrideTextureOffsetX;
	split.drawenv.tw.y = overrideTextureOffsetY;
	split.overrideHiresW = overrideTextureHiresW;
	split.overrideHiresH = overrideTextureHiresH;

	split.startVertex = g_vertexIndex;
	split.numVerts = 0;
}

static void AddModernSplit(unsigned int handle)
{
	GPUDrawSplit& current = g_splits[g_splitIndex];
	current.numVerts = g_vertexIndex - current.startVertex;
	if (g_splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		eprinterr("MAX_DRAW_SPLITS reached while appending modern mesh\n");
		return;
	}
	GPUDrawSplit& split = g_splits[++g_splitIndex];
	split.kind = GPU_SPLIT_MODERN;
	split.modernHandle = handle;
	split.drawenv = activeDrawEnv;
	split.startVertex = g_vertexIndex;
	split.numVerts = 0;
}

/* A capture point in draw order (DR_PSYX_FBCAPTURE, 0xB4): everything drawn so
 * far this frame is packed into the VRAM rect carried in drawenv.clip, so the
 * prims after it sample it. The scene-scratch effects depend on exactly this
 * ordering -- draw the scene, then composite it back -- and PC draws the scene
 * on screen instead of into VRAM, so the capture has to sit where the game's
 * DR_AREA switched back. See GR_CaptureFrameToVramRect. */
static void AddFbCaptureSplit(int x, int y, int w, int h)
{
	GPUDrawSplit& current = g_splits[g_splitIndex];
	current.numVerts = g_vertexIndex - current.startVertex;
	if (g_splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		eprinterr("MAX_DRAW_SPLITS reached while appending fb capture\n");
		return;
	}
	GPUDrawSplit& split = g_splits[++g_splitIndex];
	split.kind = GPU_SPLIT_FBCAPTURE;
	split.drawenv.clip.x = (short)x;
	split.drawenv.clip.y = (short)y;
	split.drawenv.clip.w = (short)w;
	split.drawenv.clip.h = (short)h;
	split.startVertex = g_vertexIndex;
	split.numVerts = 0;
}

/* Debug isolation of the additive (BM_ADD) layer, driven by the `add` console cmd.
 * 0 = drop every additive split (confirm whether a fire/lightning effect is additive
 * geometry), 1 = normal, 2 = draw additive WITH the depth test that GR_SetBlendMode
 * normally disables (test the "additive draws through the floor" hypothesis). */
/* World-opaque depth function under PGXP. 1 = GL_ALWAYS (the PS1-OT painter
 * design: the painter winner leaves true per-pixel depth behind). 0 = GL_LEQUAL,
 * which keeps painter order for an exact depth tie but lets a genuinely nearer
 * coplanar face win regardless of the order it was drawn in.
 *
 * ALWAYS hands the whole decision to paint order, so two coplanar road quads
 * whose GTE depth straddles an OT bucket boundary swap which one paints last as
 * the camera moves, and the surface flickers between them. That is the road
 * flicker in central Silent Hill, and it stops with PGXP off because the off
 * path still has LEQUAL arbitrating. Console: `worlddepth 0|1`. */
int g_PsxWorldDepthAlways = 1;

int g_PsxDbgAddMode = 1;

void DrawSplit(const GPUDrawSplit& split)
{
	const bool isAdditive = (split.blendMode == BM_ADD || split.blendMode == BM_ADD_QUATER_SOURCE);

	if (g_PsxDbgAddMode == 0 && isAdditive)
		return;

	{
		/* [WORLDSPLIT] Identify which render path the 3D WORLD uses. The old
		 * cap of 40 only caught boot/title 2D prims (verts 6-18, dfe=1). World
		 * geometry chunks have many more verts; log the first 40 big splits so
		 * the gameplay world's dfe (-> enable=!dfe -> which GR_SetOffscreenState
		 * branch / ortho) is visible in one in-game capture. */
		static int bigSplitLog = 0;
		if (bigSplitLog < 40 && split.numVerts >= 60) {
			eprintf("[WORLDSPLIT] verts=%d dfe=%d fmt=%d blend=%d texId=%u clip=(%d,%d,%d,%d)\n",
				split.numVerts, split.drawenv.dfe, split.texFormat, split.blendMode,
				(unsigned)split.textureId, split.drawenv.clip.x, split.drawenv.clip.y,
				split.drawenv.clip.w, split.drawenv.clip.h);
			bigSplitLog++;
		}
	}
	if(split.debugText)
		GR_PushDebugLabel(split.debugText);

	GR_SetStencilMode(split.drawPrimMode);	// draw with mask 0x16

	g_PsxSplitIsGlyph = split.isGlyph;
	if (split.mixed)
		GR_SetTextureMixed(split.textureId);
	else
		GR_SetTexture(split.textureId, split.texFormat);

	if (split.texFormat == TF_32_BIT_RGBA)
		GR_SetOverrideTextureSize(split.drawenv.tw.w, split.drawenv.tw.h,
		                          split.drawenv.tw.x, split.drawenv.tw.y,
		                          split.overrideHiresW, split.overrideHiresH);

	const bool drawOnScreen = split.drawenv.dfe;
	GR_SetupClipMode(&split.drawenv.clip, drawOnScreen);
	GR_SetOffscreenState(&split.drawenv.clip, !drawOnScreen);

	GR_SetBlendMode(split.blendMode);

	/* PGXP coplanar fix (docs/PGXP_PR51_Vetting.md): draw static-world opaque
	 * geometry with GL_ALWAYS instead of GL_LEQUAL, keeping GR_SetBlendMode's
	 * depth test+write on. Coplanar world faces then resolve by painter (OT)
	 * order among themselves — no z-fight — while still leaving a per-pixel depth
	 * buffer that actors/effects LEQUAL-test against. Every on-path split sets the
	 * func explicitly, so a world split's GL_ALWAYS can't leak onto the next.
	 * Gate is folded into the arg (not an `if`) so a runtime PGXP on->off toggle
	 * still self-heals GL_ALWAYS back to LEQUAL on the first off frame; on steady
	 * off the cache is already 0 and GR_SetDepthFuncAlways early-returns without
	 * touching glDepthFunc, so pixels stay byte-identical. (depthMode is always
	 * DISABLED when off.) */
	GR_SetDepthFuncAlways(g_PsxUsePgxp && g_PsxWorldDepthAlways &&
	                      split.depthMode == SPLIT_DEPTH_WORLD && split.blendMode == BM_NONE);

	if (g_PsxDbgAddMode == 2 && isAdditive)
		GR_EnableDepth(1);

	/* [ITEMDEPTH] sample the REAL GL depth state for the splits that actually
	 * rasterize probed item primitives — after all state setup, immediately
	 * before the draw call, so a tracker-vs-driver desync shows up here. */
	if (g_PsyX_ItemDepthProbe && s_ipArmed && split.numVerts > 0 &&
	    (int)split.startVertex < s_ipVertHi &&
	    (int)(split.startVertex + split.numVerts) > s_ipVertLo)
	{
		ItemProbe_RecordSplitGl((int)(&split - (const GPUDrawSplit*)g_splits), (int)split.blendMode,
		                        (int)split.numVerts, (int)split.drawenv.dfe);
	}

	GR_DrawTriangles(split.startVertex, split.numVerts / 3);

	if (split.debugText)
		GR_PopDebugLabel();
}

extern int g_dbg_polygonSelected;

static bool ShadowTriangleCanCast(const GrVertex* vertex)
{
	for (int i = 0; i < 3; i++)
	{
		if (vertex[i].ny < 0.5f || vertex[i].nx > 0.5f || !(vertex[i].vsz > 0.0f))
			return false;
		/* Geometry sanity, not just provenance: !(vsz>0) rejects NaN but +inf
		 * passes it, and vsx/vsy were never checked. One non-finite/huge caster
		 * vertex rasterizes a near-full-map depth splat -> a giant false shadow
		 * wedge over the scene (the PR#8 wedge class, from the depth side). */
		if (!isfinite(vertex[i].vsx) || !isfinite(vertex[i].vsy) || !isfinite(vertex[i].vsz) ||
		    fabsf(vertex[i].vsx) > 1.0e6f || fabsf(vertex[i].vsy) > 1.0e6f || vertex[i].vsz > 1.0e6f)
			return false;
		/* Inside the near plane = degenerate caster. A vertex closer than the
		 * near-clip distance projects to an extreme shadow-map coord and paints a
		 * black wedge; this is the mode-3 half of the Nowhere report and, unlike
		 * the blue spike, is PGXP-independent (the shadow pass reads a_viewpos
		 * whether or not PGXP is on) — which is why the artifact survived flipping
		 * use_pgxp. Legit casters sit beyond the near plane, so this only drops
		 * geometry clipping the lens. */
		if (vertex[i].vsz < g_PgxpNearZ)
			return false;
	}
	return true;
}

/* The pass is opened lazily on the first caster so a batch with none (the
 * UI OT, the water OT, 2D-only frames) neither clears the shadow map built by
 * the world batch nor disturbs GL state. The map therefore always holds the
 * last world batch, which is what the lit shader samples on frames that do
 * not rebuild it. */
static void DrawShadowCasters(const GPUDrawSplit& split, bool& passBegun)
{
	int runStart = -1;
	for (int offset = 0; offset + 2 < split.numVerts; offset += 3)
	{
		if (ShadowTriangleCanCast(&g_vertexBuffer[split.startVertex + offset]))
		{
			if (runStart < 0)
				runStart = offset;
		}
		else if (runStart >= 0)
		{
			if (!passBegun) { GR_ShadowPassBegin(); passBegun = true; }
			GR_ShadowPassDraw(split.startVertex + runStart, offset - runStart);
			runStart = -1;
		}
	}

	if (runStart >= 0)
	{
		if (!passBegun) { GR_ShadowPassBegin(); passBegun = true; }
		GR_ShadowPassDraw(split.startVertex + runStart, split.numVerts - runStart);
	}
}

//
// Draws all polygons after AggregatePTAG
//
static void DrawFarMeshSplit(const GPUDrawSplit& split);

void DrawAllSplits()
{
#ifdef _DEBUG
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex* vert = &g_vertexBuffer[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			eprintf("==========================================\n");
			eprintf("POLYGON: %d\n", g_dbg_polygonSelected);
			eprintf("X: %d Y: %d
", vert->x, vert->y);
			eprintf("U: %d V: %d
", vert->u, vert->v);
			eprintf("TP: %d CLT: %d
", vert->page, vert->clut);
			
			eprintf("==========================================\n");
		}

		PsyX_UpdateInput();
	}
#endif // _DEBUG

	// next code ideally should be called before EndScene
	GR_UpdateVertexBuffer(g_vertexBuffer, g_vertexIndex);

	/* Flashlight shadow map: depth-only pre-pass over the OPAQUE splits from the
	 * light POV, into the shadow FBO, while the frame VAO is still bound. Only
	 * BM_NONE (opaque) casts — every semi-transparent mode is skipped so effects
	 * don't throw hard shadows: additive/subtractive (fire, blood) AND the 50/50
	 * BM_AVERAGE the PC muzzle flash uses (a quad at the muzzle, right next to the
	 * close flashlight, was flashing a huge gun/magazine silhouette on the wall
	 * for the frame or two it existed). Caster eligibility is checked per
	 * triangle so an invalid or suppressed vertex cannot stretch the remaining
	 * vertices into a false shadow. */
	if (GR_FlashlightShadowActive())
	{
		bool passBegun = false;
		for (int i = 1; i <= g_splitIndex; i++)
		{
			const GPUDrawSplit& s = g_splits[i];
			if (s.kind != GPU_SPLIT_LEGACY || s.numVerts < 3)
				continue;
			if (s.blendMode != BM_NONE)
				continue;
			DrawShadowCasters(s, passBegun);
		}
		if (passBegun)
			GR_ShadowPassEnd();
	}

	if (g_PsxUsePgxp && g_splitIndex > s_dbgSplitHighWater)
		s_dbgSplitHighWater = g_splitIndex;

	for (int i = 1; i <= g_splitIndex; i++)
	{
		if (g_splits[i].kind == GPU_SPLIT_MODERN)
			GR_DrawModernMesh(g_splits[i].modernHandle, &g_splits[i].drawenv);
		else if (g_splits[i].kind == GPU_SPLIT_FARMESH)
			DrawFarMeshSplit(g_splits[i]);
		else if (g_splits[i].kind == GPU_SPLIT_FBCAPTURE)
			GR_CaptureFrameToVramRect(g_splits[i].drawenv.clip.x, g_splits[i].drawenv.clip.y,
			                          g_splits[i].drawenv.clip.w, g_splits[i].drawenv.clip.h);
		else
			DrawSplit(g_splits[i]);
	}

	ClearSplits();
}

// forward declarations
int ParsePrimitive(P_TAG* polyTag);

/* Nodes of a game-side sub-OT spliced into a bucket (whole-map far list):
 * empty tags like real buckets, but not buckets of THIS OT, so counting them
 * would shift every later bucket's depth seed and the UI-bucket test. */
extern "C" { uintptr_t g_PsxOtNoBucketLo = 0, g_PsxOtNoBucketHi = 0; }

void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive)
{
	if (!p)
		return;

	// setup single primitive flag (needed for AddSplits)
	g_DrawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG* polyTag = reinterpret_cast<P_TAG*>(p);
		ParsePrimitive(polyTag);

		GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
		lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
	}
	else
	{
		// Bucket-accurate depth: all primitives inside the same OT bucket share
		// one depth value — matching the PSX's painter's-algorithm intent.
		// g_otBucketDepth advances only at tagLength==0 bucket-boundary entries.
		int otBucketIdx = 0;
		s_otBucketAbs = -1;
		const float otBucketStep = (g_currentOTBucketCount > 1)
			? (2.0f / (float)(g_currentOTBucketCount - 1)) : 0.0f;
		g_otBucketDepth = -1.0f;
		// walk OT_TAG linked list with safety guards
		uintptr_t basePacket = reinterpret_cast<uintptr_t>(p);
		/* The safety cap guards against corrupt/cyclic lists, but it counts OT
		 * NODES (2048 buckets + one per prim) — the whole-town render mode
		 * legitimately submits far more than the old 16k prims. */
		for (int safety = 0; safety < (1 << 20); safety++)
		{
			const int tagLength = getlen(basePacket);
			if (tagLength > 0 && tagLength <= 32)
			{
				uintptr_t currentPacket = basePacket;
				const uintptr_t endPacket = basePacket + (tagLength + P_LEN) * sizeof(u_int);
				int primLength = 0;
				while (currentPacket < endPacket)
				{
					/* Whole-town mode can submit far more geometry than one
					 * vertex buffer holds. When the buffer nears full, FLUSH:
					 * finalize the open split, draw the accumulated splits,
					 * and keep walking. Painter's far->near order is preserved
					 * across flushes (earlier flush = farther geometry, drawn
					 * first; the GL depth buffer persists between flushes).
					 * Checked per PRIM, not per packet — a multi-prim (or
					 * corrupt) packet can emit far more than one prim's worth
					 * between packet boundaries. Reserve 24 verts = the
					 * largest single-prim emit (LINE_F4 = 18). The flashlight
					 * shadow pre-pass inside DrawAllSplits only sees each
					 * flush's casters, but the near geometry a flashlight can
					 * touch is always in the FINAL flush, so shadows stay
					 * correct in practice. */
					if (g_vertexIndex + 24 > MAX_VERTEX_BUFFER_SIZE)
					{
						GPUDrawSplit& flushSplit = g_splits[g_splitIndex];
						flushSplit.numVerts = g_vertexIndex - flushSplit.startVertex;
						WM_CLS(17, DrawAllSplits());
					}
					if (g_PsxWholeMapProbe == 1)
					{
						const unsigned code = reinterpret_cast<P_TAG*>(currentPacket)->code;
						const int cls = (code == 0xB5) ? 16 : (int)(code >> 4);
						WM_CLS(cls, primLength = ParsePrimitive(reinterpret_cast<P_TAG*>(currentPacket)));
					}
					else
						primLength = ParsePrimitive(reinterpret_cast<P_TAG*>(currentPacket));
					if (primLength <= 0) break;
					currentPacket += (primLength + P_LEN) * sizeof(u_int);
				}

				if (currentPacket != endPacket)
				{
					eprinterr("did not output valid primitive or ptag length is not valid (diff=%d)\n", endPacket-currentPacket);
					/* One-shot dump: the corrupted prim's raw bytes
					 * fingerprint the writer. Same approach that pinned
					 * the knife OT corruption to func_800611C0 via the
					 * recognizable .NHS. tail bytes from POLY_FT4 vertex
					 * data. After the first dump, fall back to the
					 * existing rate-limited summary above. */
					static int s_badPrimDumped = 0;
					if (!s_badPrimDumped) {
						s_badPrimDumped = 1;
						const uint32_t* w = reinterpret_cast<const uint32_t*>(basePacket);
						eprintinfo("[OT-PRIM] FIRST corrupt prim at %p tagLen=%d code=0x%02x\n",
							(void*)basePacket, tagLength,
							reinterpret_cast<P_TAG*>(basePacket)->code);
						eprintinfo("[OT-PRIM]   raw 64 bytes: %08x %08x %08x %08x %08x %08x %08x %08x\n",
							(unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
							(unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
						eprintinfo("[OT-PRIM]                 %08x %08x %08x %08x %08x %08x %08x %08x\n",
							(unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11],
							(unsigned)w[12], (unsigned)w[13], (unsigned)w[14], (unsigned)w[15]);
					}
				}
			}
			else if (tagLength == 0 &&
			         !(basePacket >= g_PsxOtNoBucketLo && basePacket < g_PsxOtNoBucketHi))
			{
				// OT bucket boundary — advance to the next bucket's depth.
				if (g_PsxUsePgxp && s_otViewZShift > 0)
				{
					/* Depth channel: seed the bucket on the SAME constant
					 * linear viewZ scale the world/EXACT prims use, so
					 * untracked (kind NONE) content is commensurable by
					 * construction and inherits its authored OT painter
					 * placement in depth space. The OT is walked far->near,
					 * so absolute bucket index = count-1-walked; a bucket
					 * covers viewZ ~ index << shiftEff (+half a quantum to
					 * center it). Monotone in walk order, exactly like the
					 * legacy index seed — today's relations are preserved. */
					int b = g_currentOTBucketCount - 1 - otBucketIdx;
					if (b < 0) b = 0;
					float vz = (float)((unsigned)b << s_otViewZShift)
					         + 0.5f * (float)(1 << s_otViewZShift);
					g_otBucketDepth = PgxpNdcFromViewZ(vz);
				}
				else
				{
					g_otBucketDepth = -1.0f + (float)otBucketIdx * otBucketStep;
					if (g_otBucketDepth > 1.0f) g_otBucketDepth = 1.0f;
				}
				s_otBucketAbs = g_currentOTBucketCount - 1 - otBucketIdx;
				otBucketIdx++;
			}
			else if (tagLength > 32)
			{
				eprinterr("got invalid tag length %d, code %d\n", tagLength, reinterpret_cast<P_TAG*>(basePacket)->code);
				static int s_badTagDumped = 0;
				if (!s_badTagDumped) {
					s_badTagDumped = 1;
					const uint32_t* w = reinterpret_cast<const uint32_t*>(basePacket);
					eprintinfo("[OT-PRIM] FIRST bad-tag-len at %p tagLen=%d\n",
						(void*)basePacket, tagLength);
					eprintinfo("[OT-PRIM]   raw 64 bytes: %08x %08x %08x %08x %08x %08x %08x %08x\n",
						(unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
						(unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
					eprintinfo("[OT-PRIM]                 %08x %08x %08x %08x %08x %08x %08x %08x\n",
						(unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11],
						(unsigned)w[12], (unsigned)w[13], (unsigned)w[14], (unsigned)w[15]);
				}
			}

			GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
			lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;

			if (isendprim(basePacket))
				break;

			// Validate next pointer before following it.
			// Crash root-caused via WinDbg minidump on the muzzle-flash repro:
			// FAILURE_BUCKET_ID INVALID_POINTER_READ at this exact site,
			// stack ParsePrimitivesLinkedList+0xa5 -> DrawOTag -> GsDrawOt.
			// The next-pointer can land on:
			//   1. NULL / very low (uninitialized OT bucket)        — break
			//   2. (uintptr_t)-1 == 0xFFFF..FF (PSX legacy terminator
			//      written by some not-fully-ported code; differs from
			//      &prim_terminator that isendprim looks for)        — break
			//   3. Unmapped high address (Windows user mode tops at
			//      0x7FFF'FFFF'FFFF; anything past that is kernel)   — break
			//   4. Wild but technically-mapped — can't catch without
			//      VirtualQuery; rely on the 1<<20 node safety counter.
			uintptr_t nextPtr = reinterpret_cast<uintptr_t>(nextPrim(basePacket));
			if (nextPtr < 0x10000 ||
			    nextPtr == static_cast<uintptr_t>(-1)
#if defined(_WIN32)
			    || nextPtr >= 0x7FFFFFFFFFFFULL
#endif
			) {
				static int s_badNextLogged = 0;
				if (s_badNextLogged < 16) {
					eprintinfo("[OT] bad nextPtr=0x%llX at %p — chain walk halted\n",
						(unsigned long long)nextPtr, (void*)basePacket);
					s_badNextLogged++;
				}
				break;
			}
			basePacket = nextPtr;
		}
	}
}

inline int IsNull(POLY_FT3* poly)
{
	return  poly->x0 == -1 &&
		poly->y0 == -1 &&
		poly->x1 == -1 &&
		poly->y1 == -1 &&
		poly->x2 == -1 &&
		poly->y2 == -1;
}

static int ProcessFlatLines(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2* poly = (LINE_F2*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = c0;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3* poly = (LINE_F3*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 5;
	}
	case 0xc:
	{
		int i;
		LINE_F4* poly = (LINE_F4*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x2;
			VERTTYPE* p1 = &poly->x3;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 6;
	}
	}
	return 0;
}

static int ProcessGouraudLines(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2* poly = (LINE_G2*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = &poly->r1;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

static int ProcessFlatPoly(P_TAG* polyTag)
{
	/* PGXP hint: the prim's stamped GTE ring position (0xFFFF / ignored when
	 * PGXP off). 3D polygons only — sprites/tiles/lines stay 0xFFFF. */
	const u_short gteIndex = g_PsxUsePgxp ? polyTag->pgxp_index : (u_short)0xFFFF;
	if (g_PsxUsePgxp) PGXP_BeginPrim(polyTag);

	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3* poly = (POLY_F3*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

		g_vertexIndex += PgxpEmitPoly(firstVertex, 3);

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x4:
	{
		POLY_FT3* poly = (POLY_FT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly) && !ShouldDropForClut("FT3", poly->tpage, poly->clut))
		{
			ApplyHiresOverride(poly->tpage, poly->clut);

			AddSplit(semiTrans, true, SplitDepthForPrim(polyTag));

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
			ApplyGtePerVertexDepth(firstVertex, polyTag, false);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			g_vertexIndex += PgxpEmitPoly(firstVertex, 3);

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4* poly = (POLY_F4*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += PgxpEmitPoly(firstVertex, 6);
#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 5;
	}
	case 0xC:
	{
		POLY_FT4* poly = (POLY_FT4*)polyTag;
		/* Historical note: this guard predates the other prim types' copies and
		 * was originally written as a broad "bogus tpage/clut/UV" filter for
		 * combat particle prims; the tpage/UV halves never fired and are gone.
		 * The surviving clut test is now ClutHasNoPalette (see there for why).
		 * The early return is kept because its length is proven correct. */
		if (ShouldDropForClut("FT4", poly->tpage, poly->clut))
			return 9;  /* skip rendering, advance past prim */

		activeDrawEnv.tpage = poly->tpage;
		ApplyHiresOverride(poly->tpage, poly->clut);

		AddSplit(semiTrans, true, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += PgxpEmitPoly(firstVertex, 6);

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	}
	return 0;
}

static int ProcessGouraudPoly(P_TAG* polyTag)
{
	/* PGXP hint (3D polygons only). 0xFFFF / ignored when PGXP off. */
	const u_short gteIndex = g_PsxUsePgxp ? polyTag->pgxp_index : (u_short)0xFFFF;
	if (g_PsxUsePgxp) WM_CYC(0, PGXP_BeginPrim(polyTag));

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3* poly = (POLY_G3*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		// Per-vertex fog factor packed into pad1/pad2 (game writes fog amount there).
		// v0 shares v1's fog (code byte occupies v0's pad slot).
		firstVertex[0]._p0 = poly->pad1;
		firstVertex[1]._p0 = poly->pad1;
		firstVertex[2]._p0 = poly->pad2;

		g_vertexIndex += PgxpEmitPoly(firstVertex, 3);

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 6;
	}
	case 0x4:
	{
		POLY_GT3* poly = (POLY_GT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;
		if (!ShouldDropForClut("GT3", poly->tpage, poly->clut))
		{
			ApplyHiresOverride(poly->tpage, poly->clut);

			AddSplit(semiTrans, true, SplitDepthForPrim(polyTag));

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
			ApplyGtePerVertexDepth(firstVertex, polyTag, false);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

			/* Per-vertex fog. v1 and v2 ride their colour-word pad; v0's pad IS
			 * the GPU command code, so its fog travels in the unused trailing
			 * pad2 short, exactly as the GT4 case below already does. The game
			 * marks that short with bit 15 so a triangle from a path that does
			 * not carry v0's fog still falls back to the old behaviour instead
			 * of reading whatever the reused prim buffer left there.
			 *
			 * Without v0's own value every textured triangle had one corner
			 * fogged as if it were at its neighbour's distance, which is the
			 * skewed fog gradient on large receding surfaces. The quad path was
			 * fixed for this; triangles were missed. */
			firstVertex[0]._p0 = (poly->pad2 & 0x8000u) ? (char)(poly->pad2 & 0x7F)
			                                            : poly->p1;
			firstVertex[1]._p0 = poly->p1;  // v1
			firstVertex[2]._p0 = poly->p2;  // v2

			const int ppFirst = g_vertexIndex;
			const bool ppReshaped = g_PsyX_PpLitActive && g_PsxUsePgxp && PgxpNearClipEligible(firstVertex, 3);
			g_vertexIndex += PgxpEmitPoly(firstVertex, 3);
			if (g_PsyX_PpLitActive)
			{
				static const int s_triCorners[3] = { 0, 1, 2 };
				PpLitFill(polyTag, ppFirst, g_vertexIndex - ppFirst, 3, ppReshaped, s_triCorners);
			}

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 9;
	}
	case 0x8:
	{
		POLY_G4* poly = (POLY_G4*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		// Per-vertex fog factor packed into pad1/pad2/pad3 (note: MakeColourQuad swaps v2/v3).
		firstVertex[0]._p0 = poly->pad1;
		firstVertex[1]._p0 = poly->pad1;
		firstVertex[2]._p0 = poly->pad3;
		firstVertex[3]._p0 = poly->pad2;

		TriangulateQuad();

		g_vertexIndex += PgxpEmitPoly(firstVertex, 6);

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 8;
	}
	case 0xC:
	{
		POLY_GT4* poly = (POLY_GT4*)polyTag;
		bool gt4Drop;
		activeDrawEnv.tpage = poly->tpage;
		if (g_PsxWholeMapProbe == 1) s_wmGt4++;
		WM_CYC(1, gt4Drop = ShouldDropForClut("GT4", poly->tpage, poly->clut));
		if (!gt4Drop)
		{
			WM_CYC(2, ApplyHiresOverride(poly->tpage, poly->clut));

			WM_CYC(3, AddSplit(semiTrans, true, SplitDepthForPrim(polyTag)));

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			WM_CYC(4, MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex));
			WM_CYC(5, ApplyGtePerVertexDepth(firstVertex, polyTag, true));
			WM_CYC(6, MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd));
			WM_CYC(7, MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2));

			// Copy per-vertex fog factor from pad bytes (note: MakeColourQuad swaps v2/v3)
			firstVertex[0]._p0 = (unsigned char)poly->pad2;  // v0: own fog (game carries it in pad2; v0 color word's pad is the code byte)
			firstVertex[1]._p0 = poly->p1;  // v1
			firstVertex[2]._p0 = poly->p3;  // v3 (buffer[2] = poly vertex 3 due to swap)
			firstVertex[3]._p0 = poly->p2;  // v2 (buffer[3] = poly vertex 2 due to swap)

			TriangulateQuad();

			const int ppFirst = g_vertexIndex;
			const bool ppReshaped = g_PsyX_PpLitActive && g_PsxUsePgxp && PgxpNearClipEligible(firstVertex, 6);
			WM_CYC(8, g_vertexIndex += PgxpEmitPoly(firstVertex, 6));
			if (g_PsyX_PpLitActive)
			{
				/* MakeVertexQuad takes x0 x1 x3 x2 and TriangulateQuad expands that
				 * to corners 0 1 2 / 1 2 3. */
				static const int s_quadCorners[6] = { 0, 1, 2, 1, 2, 3 };
				PpLitFill(polyTag, ppFirst, g_vertexIndex - ppFirst, 6, ppReshaped, s_quadCorners);
			}

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 12;
	}
	}
	return 0;
}

static int ProcessTileAndSprt(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE* poly = (TILE*)polyTag;

		/* The lighthouse glow (map6_s02 func_800D2364; map7_s00 has a twin) pairs its
		 * feedback sprites with add/subtract fills under the same display-band
		 * tpage. The sprites get stretched across the widened UI ortho; the
		 * fills must follow or their contrast lands as a 4:3 box. */
		if (semiTrans && poly->h >= 224 &&
		    ((activeDrawEnv.tpage >> 7) & 0x3) >= 2 && ((activeDrawEnv.tpage & 0xF) * 64) < 320)
			s_curPrimIsFeedback = 1;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		StretchFullFrameFillToWorldOrtho(firstVertex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x64:
	{
		SPRT* poly = (SPRT*)polyTag;
		if (!ShouldDropForClut("SPRT", activeDrawEnv.tpage, poly->clut))
		{
			ApplyHiresOverride(activeDrawEnv.tpage, poly->clut);

			/* dream_blur off: the soft-focus composite is not drawn at all (its
			 * capture is skipped too), leaving the plain scene. */
			if (s_curPrimIsScratch && !g_cfg_dreamFeedback)
				return 4;

			AddSplit(semiTrans, true, SplitDepthForPrim(polyTag));

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
			MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h);
			MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

			/* dream_blur_strength: every soft-focus layer lerps from what is
			 * under it toward its hardware result, so 1.0 is the PS1 image and 0
			 * the plain scene, at unchanged brightness. The average layers scale
			 * their 0.5 weight, the additive layer its colour, and the opaque
			 * base layer is BM_CONSTANT_ALPHA (AddSplit). */
			if (s_curPrimIsScratch && semiTrans && g_PsxFeedbackDampBlend < 1.0f)
			{
				const float s = g_PsxFeedbackDampBlend;
				const bool  avg = GET_TPAGE_BLEND(activeDrawEnv.tpage) == BM_AVERAGE;
				int i;
				for (i = 0; i < 4; i++)
				{
					if (avg)
					{
						firstVertex[i].a = (unsigned char)(firstVertex[i].a * s + 0.5f);
					}
					else
					{
						firstVertex[i].r = (unsigned char)(firstVertex[i].r * s + 0.5f);
						firstVertex[i].g = (unsigned char)(firstVertex[i].g * s + 0.5f);
						firstVertex[i].b = (unsigned char)(firstVertex[i].b * s + 0.5f);
					}
				}
			}

			TriangulateQuad();

			g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 4;
	}
	case 0x68:
	{
		TILE_1* poly = (TILE_1*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x70:
	{
		TILE_8* poly = (TILE_8*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x74:
	{
		SPRT_8* poly = (SPRT_8*)polyTag;
		if (!ShouldDropForClut("SPRT_8", activeDrawEnv.tpage, poly->clut))
		{
			ApplyHiresOverride(activeDrawEnv.tpage, poly->clut);

			AddSplit(semiTrans, true, SplitDepthForPrim(polyTag));

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
			MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8);
			MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

			TriangulateQuad();

			g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 3;
	}
	case 0x78:
	{
		TILE_16* poly = (TILE_16*)polyTag;

		AddSplit(semiTrans, false, SplitDepthForPrim(polyTag));

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x7C:
	{
		SPRT_16* poly = (SPRT_16*)polyTag;
		if (!ShouldDropForClut("SPRT_16", activeDrawEnv.tpage, poly->clut))
		{
			ApplyHiresOverride(activeDrawEnv.tpage, poly->clut);

			AddSplit(semiTrans, true, SplitDepthForPrim(polyTag));

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
			MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16);
			MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

			TriangulateQuad();

			g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 3;
	}
	}
	return 0;
}

static int ProcessDrawEnv(P_TAG* polyTag)
{
	const u_int* codePtr = (u_int*)&polyTag->pad0;
	int processedLongs = 0;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u_int code = codePtr[i];
		const int primSubType = code >> 24 & 0x0F;

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			activeDrawEnv.dfe = 1; // Force dfe=1: PSX dfe only controls display-during-draw for interlace, not rendering target
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w -= activeDrawEnv.clip.x;
			activeDrawEnv.clip.h -= activeDrawEnv.clip.y;

			/* Scene VRAM-scratch redirect: a draw-area at x >= 320 can't be a
			 * display buffer (both live in the x < 320 column) — it's a scene
			 * rendering the frame into offscreen VRAM to resample it as a
			 * texture (map4_s04 Lisa dream strips, map3_s02 sibling). GL never
			 * rasterizes into VRAM, so latch the rect and let the render side
			 * blit the stored frame there each present (GR_SetSceneFbRedirect).
			 * Size floor skips small CLUT/strip areas. */
			if (activeDrawEnv.clip.x >= 320 &&
			    activeDrawEnv.clip.w >= 64 && activeDrawEnv.clip.h >= 64)
			{
				GR_SetSceneFbRedirect(activeDrawEnv.clip.x, activeDrawEnv.clip.y,
				                      activeDrawEnv.clip.w, activeDrawEnv.clip.h);
				activeDrawEnv.dfe = 1;
			}
			else if (activeDrawEnv.clip.x >= 320 &&
			         activeDrawEnv.clip.w > 0 && activeDrawEnv.clip.h > 0)
			{
				/* Small offscreen VRAM scratch (sewer water caustic: 832,224
				 * 32x32, water.c func_8008E5B4): the game renders prims into
				 * this VRAM tile and later SAMPLES it as a texture. Route the
				 * split through the offscreen FBO (dfe=0) — GR_SetOffscreenState
				 * sizes the FBO to this rect and, on the transition back to an
				 * on-screen split, packs the pixels into vram[] + the VRAM
				 * textures at (clip.x, clip.y). dfe self-restores at the next
				 * on-screen DR_AREA below or any DR_TPAGE (case 0x1), so the
				 * scratch scope is exactly area-prim .. env-restore. */
				activeDrawEnv.dfe = 0;
			}
			else
			{
				activeDrawEnv.dfe = 1;
			}
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			// TODO
			activeDrawEnv.ofs[0] = code & 2047;
			activeDrawEnv.ofs[1] = (code >> 11) & 2047;
			break;
		}
		case 0x6:
		{
			eprintf("Mask setting: %08x\n", code);
			//MaskSetOR = (*cb & 1) ? 0x8000 : 0x0000;
			//MaskEvalAND = (*cb & 2) ? 0x8000 : 0x0000;
			break;
		}
		case 0:
			// proceed to next primitive tag — but consume the rest of the
			// declared packet length so the caller's
			//   currentPacket += (primLength + P_LEN) * 4
			// advance lands at endPacket. Returning the partial count made
			// currentPacket short of endPacket; the leftover bytes (zero
			// padding) then got mis-parsed as a fresh prim with code=0x00 /
			// primLength=3, which overshot endPacket by 16 bytes (exactly
			// the diff=-16 in the OT-PRIM log) and started chasing wild
			// next-pointers — repro: handgun fire, stack
			// ParsePrimitivesLinkedList+0xae -> DrawOTag -> GsDrawOt.
			return polyTag->len;
		}
		++processedLongs;
	}

	return processedLongs;
}

#define PSYX_FAR_QUAD_MAX (1 << 18)
static PsyX_FarQuad s_farQuads[PSYX_FAR_QUAD_MAX];
static int          s_farQuadCount = 0;

/* Per-frame memo of the texture decisions for one tpage/clut: a whole-town
 * frame draws ~90k far quads over a few hundred keys, and the per-prim CLUT
 * check, glyph callback and hi-res lookup (a linear entry scan for non-pool
 * textures) were repeated for every one. Keys that touch the framebuffer
 * feedback or scene-scratch bands are never memoized (their per-prim side
 * effects matter); they take EmitFarQuad. Valid for one frame only, so the
 * hi-res LRU stamp and the lazy-compose demand signal still fire each frame. */
struct FarTexMemo
{
	unsigned  key;
	unsigned  gen;
	unsigned char drop, glyph, special, _pad;
	TextureID tex;
	int       w, h, ox, oy, hw, hh;
};
#define FAR_TEX_MEMO_SIZE 2048
static FarTexMemo s_farTexMemo[FAR_TEX_MEMO_SIZE];
static unsigned   s_farTexGen = 1;

extern "C" void PsyX_FarQuadsReset(void)
{
	s_farQuadCount = 0;
	if (++s_farTexGen == 0)
	{
		memset(s_farTexMemo, 0, sizeof(s_farTexMemo));
		s_farTexGen = 1;
	}
}

extern "C" PsyX_FarQuad* PsyX_FarQuadAlloc(int* index)
{
	if (s_farQuadCount >= PSYX_FAR_QUAD_MAX)
		return NULL;
	*index = s_farQuadCount;
	return &s_farQuads[s_farQuadCount++];
}

/* Emit one prepared far quad exactly as the GT4 parse would emit a world quad:
 * same split classification, hi-res lookup, vertex/texcoord/colour builders and
 * fog, but from the game's own projection instead of the GTE and shadow tables. */
static void EmitFarQuad(const PsyX_FarQuad* q)
{
	static const int ord[4] = { 0, 1, 3, 2 }; /* MakeVertexQuad's v0,v1,v3,v2 buffer order */
	float ofsX, ofsY;
	float wsum = 0.0f;

	s_curPrimSemiTrans  = 0;
	s_curPrimIsFeedback = 0;
	s_curPrimIsScratch  = 0;
	s_curPrimIsGlyph    = 0;
	activeDrawEnv.tpage = q->tpage;
	if (ClutHasNoPalette(q->tpage, q->clut))
		return;
	ApplyHiresOverride(q->tpage, q->clut);
	AddSplit(false, true, (g_PsxUsePgxp && g_PsxPgxpWorldDepth) ? SPLIT_DEPTH_WORLD : SPLIT_DEPTH_DISABLED);

	if (g_vertexIndex + 24 > MAX_VERTEX_BUFFER_SIZE)
	{
		GPUDrawSplit& flushSplit = g_splits[g_splitIndex];
		flushSplit.numVerts = g_vertexIndex - flushSplit.startVertex;
		DrawAllSplits();
		AddSplit(false, true, (g_PsxUsePgxp && g_PsxPgxpWorldDepth) ? SPLIT_DEPTH_WORLD : SPLIT_DEPTH_DISABLED);
	}

	GrVertex* v = &g_vertexBuffer[g_vertexIndex];
	DrawEnvOffset(ofsX, ofsY);
	memset(v, 0, sizeof(GrVertex) * 4);
	for (int k = 0; k < 4; k++)
	{
		const int   s  = ord[k];
		const float fx = q->sx[s], fy = q->sy[s];
		int ix = (int)(fx >= 0.0f ? fx + 0.5f : fx - 0.5f);
		int iy = (int)(fy >= 0.0f ? fy + 0.5f : fy - 0.5f);
		if (ix < -1024) ix = -1024; else if (ix > 1023) ix = 1023;
		if (iy < -1024) iy = -1024; else if (iy > 1023) iy = 1023;
		v[k].x = (short)(ix + (int)ofsX);
		v[k].y = (short)(iy + (int)ofsY);
		v[k].geom3d = 1.0f;
		if (g_PsxUsePgxp)
		{
			v[k].ppx = fx + ofsX;
			v[k].ppy = fy + ofsY;
			v[k].ppw = q->w[s];
		}
		else
		{
			v[k].ppx = (float)v[k].x;
			v[k].ppy = (float)v[k].y;
		}
		wsum += q->w[s];
	}
	if (g_PsxUsePgxp)
	{
		const float z = PgxpNdcFromViewZ(wsum * 0.25f + (float)g_PsxPgxpWorldFarBias);
		for (int k = 0; k < 4; k++) { v[k].z = z; v[k]._p1 = 1; }
	}
	else
	{
		float z = 1.0f - 2.0f * (wsum * 0.25f) / (g_szMaxPrevFrame < 1 ? 1.0f : (float)g_szMaxPrevFrame);
		if (z < -1.0f) z = -1.0f;
		for (int k = 0; k < 4; k++) v[k].z = z;
	}

	MakeTexcoordQuad(v, (unsigned char*)q->uv[0], (unsigned char*)q->uv[1], (unsigned char*)q->uv[3], (unsigned char*)q->uv[2],
	                 q->tpage, q->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
	MakeColourQuad(v, true, (unsigned char*)q->rgb[0], (unsigned char*)q->rgb[1], (unsigned char*)q->rgb[3], (unsigned char*)q->rgb[2]);
	v[0]._p0 = (char)q->fog[0];
	v[1]._p0 = (char)q->fog[1];
	v[2]._p0 = (char)q->fog[3];
	v[3]._p0 = (char)q->fog[2];

	TriangulateQuad();
	g_vertexIndex += PgxpEmitPoly(v, 6);
}

static const FarTexMemo* FarTexLookup(unsigned short tpage, unsigned short clut)
{
	const unsigned key = ((unsigned)tpage << 16) | clut;
	unsigned slot = (key * 2654435761u) >> 21;
	for (int i = 0; i < 16; i++, slot = (slot + 1) & (FAR_TEX_MEMO_SIZE - 1))
	{
		FarTexMemo* m = &s_farTexMemo[slot];
		if (m->gen == s_farTexGen && m->key == key)
			return m;
		if (m->gen != s_farTexGen)
		{
			m->key  = key;
			m->gen  = s_farTexGen;
			m->drop = ClutHasNoPalette(tpage, clut) ? 1 : 0;
			s_curPrimIsFeedback = 0;
			s_curPrimIsScratch  = 0;
			ApplyHiresOverride(tpage, clut);
			m->special = (s_curPrimIsFeedback || s_curPrimIsScratch) ? 1 : 0;
			m->glyph   = (unsigned char)s_curPrimIsGlyph;
			m->tex = overrideTexture;
			m->w   = overrideTextureWidth;
			m->h   = overrideTextureHeight;
			m->ox  = overrideTextureOffsetX;
			m->oy  = overrideTextureOffsetY;
			m->hw  = overrideTextureHiresW;
			m->hh  = overrideTextureHiresH;
			return m;
		}
	}
	return NULL;
}

/* The far-quad chain's hot loop: same output as EmitFarQuad (vertex fields,
 * triangle order, split key) for PGXP-on world quads, built straight into the
 * vertex buffer. EmitFarQuad's generic builders memset, copy for the
 * triangulation and run the near clipper, none of which a quad 16u+ out needs. */
static void EmitFarChain(int first)
{
	if (!g_PsxUsePgxp)
	{
		for (int i = first; i >= 0 && i < s_farQuadCount; i = s_farQuads[i].next)
			EmitFarQuad(&s_farQuads[i]);
		return;
	}

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);
	const int ofsXi = (int)ofsX, ofsYi = (int)ofsY;
	const int depthMode = g_PsxPgxpWorldDepth ? SPLIT_DEPTH_WORLD : SPLIT_DEPTH_DISABLED;
	const float farBias = (float)g_PsxPgxpWorldFarBias;
	unsigned lastKey = 0xFFFFFFFFu;

	s_curPrimSemiTrans = 0;
	s_primAlphaPending = 255;

	for (int i = first; i >= 0 && i < s_farQuadCount; i = s_farQuads[i].next)
	{
		const PsyX_FarQuad* q = &s_farQuads[i];
		const unsigned key = ((unsigned)q->tpage << 16) | q->clut;

		if (g_PsxWholeMapProbe == 1) g_wmFarQuads++;

		if (key != lastKey)
		{
			const FarTexMemo* m = FarTexLookup(q->tpage, q->clut);
			if (m == NULL || m->special)
			{
				EmitFarQuad(q);
				lastKey = 0xFFFFFFFFu;
				continue;
			}
			if (m->drop)
			{
				lastKey = 0xFFFFFFFFu;
				continue;
			}
			s_curPrimIsFeedback = 0;
			s_curPrimIsScratch  = 0;
			s_curPrimIsGlyph    = m->glyph;
			overrideTexture        = m->tex;
			overrideTextureWidth   = m->w;
			overrideTextureHeight  = m->h;
			overrideTextureOffsetX = m->ox;
			overrideTextureOffsetY = m->oy;
			overrideTextureHiresW  = m->hw;
			overrideTextureHiresH  = m->hh;
			activeDrawEnv.tpage = q->tpage;
			if (g_PsxWholeMapProbe == 1)
			{
				const int before = g_splitIndex;
				AddSplit(false, true, depthMode);
				g_wmFarSplits += (unsigned)(g_splitIndex - before);
			}
			else
				AddSplit(false, true, depthMode);
			lastKey = key;
		}

		if (g_vertexIndex + 24 > MAX_VERTEX_BUFFER_SIZE)
		{
			GPUDrawSplit& flushSplit = g_splits[g_splitIndex];
			flushSplit.numVerts = g_vertexIndex - flushSplit.startVertex;
			if (g_PsxWholeMapProbe == 1) g_wmFarFlushes++;
			DrawAllSplits();
			AddSplit(false, true, depthMode);
		}

		GrVertex qv[4];
		const unsigned char dither = (GET_TPAGE_DITHER(q->tpage) || activeDrawEnv.dtd) ? 1 : 0;
		const short page = q->tpage & 0x1F;
		unsigned char ulo = q->uv[0][0], uhi = ulo, vlo = q->uv[0][1], vhi = vlo;
		for (int k = 1; k < 4; k++)
		{
			if (q->uv[k][0] < ulo) ulo = q->uv[k][0];
			if (q->uv[k][0] > uhi) uhi = q->uv[k][0];
			if (q->uv[k][1] < vlo) vlo = q->uv[k][1];
			if (q->uv[k][1] > vhi) vhi = q->uv[k][1];
		}
		const float z = PgxpNdcFromViewZ((q->w[0] + q->w[1] + q->w[2] + q->w[3]) * 0.25f + farBias);
		for (int k = 0; k < 4; k++)
		{
			GrVertex& v = qv[k];
			const float fx = q->sx[k], fy = q->sy[k];
			int ix = (int)(fx >= 0.0f ? fx + 0.5f : fx - 0.5f);
			int iy = (int)(fy >= 0.0f ? fy + 0.5f : fy - 0.5f);
			if (ix < -1024) ix = -1024; else if (ix > 1023) ix = 1023;
			if (iy < -1024) iy = -1024; else if (iy > 1023) iy = 1023;
			v.x = (short)(ix + ofsXi);
			v.y = (short)(iy + ofsYi);
			v.page = page;
			v.clut = (short)q->clut;
			v.z = z;
			v.u = q->uv[k][0];
			v.v = q->uv[k][1];
			v.bright = 2;
			v.dither = dither;
			v.r = q->rgb[k][0];
			v.g = q->rgb[k][1];
			v.b = q->rgb[k][2];
			v.a = 255;
			v.tcx = 0;
			v.tcy = 0;
			v._p0 = (char)q->fog[k];
			v._p1 = 1;
			v.ppx = fx + ofsX;
			v.ppy = fy + ofsY;
			v.ppw = q->w[k];
			v.nx = v.ny = v.nz = 0.0f;
			v.vsx = v.vsy = v.vsz = 0.0f;
			v.geom3d = 1.0f;
			v.ulo = ulo; v.vlo = vlo; v.uhi = uhi; v.vhi = vhi;
		}
		/* PSX quad order 0,1,2,3 = TL,TR,BL,BR; TriangulateQuad's result is
		 * (v0,v1,v2)(v1,v2,v3). */
		GrVertex* out = &g_vertexBuffer[g_vertexIndex];
		out[0] = qv[0]; out[1] = qv[1]; out[2] = qv[2];
		out[3] = qv[1]; out[4] = qv[2]; out[5] = qv[3];
		g_vertexIndex += 6;
	}
}

/* ---- Whole-map GPU far meshes (0xB6) ---- */
extern "C" unsigned GR_FarMeshUpload(unsigned handle, const PsyX_FarMeshVertex* v, int vertexCount,
                                     const unsigned* indices, int indexCount);
extern "C" void GR_FarMeshFree(unsigned handle);
extern "C" void GR_FarMeshSetUniforms(const PsyX_FarMeshFrame* f, float ofsX, float ofsY, float layerK);
extern "C" void GR_FarMeshDraw(unsigned handle, const float base[3], const float eye[3], int firstIndex, int indexCount);
extern "C" void GR_FarMeshEnd(void);

struct FarMeshQueued
{
	unsigned                 handle;
	float                    base[3], eye[3];
	const PsyX_FarMeshRange* ranges;
	int                      rangeCount;
};
#define FAR_MESH_QUEUE_MAX 512
static FarMeshQueued     s_fmQueue[FAR_MESH_QUEUE_MAX];
static int               s_fmCount = 0;
static PsyX_FarMeshFrame s_fmFrame;
static float             s_fmOfsX = 0.0f, s_fmOfsY = 0.0f;
/* Coplanar layering: each pixel's depth moves this fraction of the way toward
 * its quad's farthest point, so where two layers share a plane the smaller one
 * (a marking inside the asphalt it sits on) is nearer, the same rule the PSX
 * far list's max-depth order applies. Console `wholemap layer`. */
extern "C" { float g_PsxFarLayerK = 0.0625f; }

extern "C" unsigned PsyX_FarMeshUpload(unsigned handle, const PsyX_FarMeshVertex* v, int vertexCount,
                                       const unsigned* indices, int indexCount)
{
	return GR_FarMeshUpload(handle, v, vertexCount, indices, indexCount);
}

extern "C" void PsyX_FarMeshFree(unsigned handle)
{
	GR_FarMeshFree(handle);
}

extern "C" void PsyX_FarMeshFrameBegin(const PsyX_FarMeshFrame* frame)
{
	s_fmFrame = *frame;
	s_fmCount = 0;
}

extern "C" void PsyX_FarMeshQueue(unsigned handle, const float base[3], const float eye[3],
                                  const PsyX_FarMeshRange* ranges, int rangeCount)
{
	if (s_fmCount >= FAR_MESH_QUEUE_MAX || handle == 0 || rangeCount <= 0)
		return;
	FarMeshQueued& q = s_fmQueue[s_fmCount++];
	q.handle = handle;
	q.base[0] = base[0]; q.base[1] = base[1]; q.base[2] = base[2];
	q.eye[0] = eye[0]; q.eye[1] = eye[1]; q.eye[2] = eye[2];
	q.ranges = ranges;
	q.rangeCount = rangeCount;
}

static void AddFarMeshSplit(void)
{
	GPUDrawSplit& current = g_splits[g_splitIndex];
	current.numVerts = g_vertexIndex - current.startVertex;
	if (g_splitIndex + 1 >= MAX_DRAW_SPLITS)
		return;
	GPUDrawSplit& split = g_splits[++g_splitIndex];
	split.kind = GPU_SPLIT_FARMESH;
	split.drawenv = activeDrawEnv;
	split.dispenv = activeDispEnv;
	split.drawPrimMode = g_DrawPrimMode;
	split.startVertex = g_vertexIndex;
	split.numVerts = 0;
	DrawEnvOffset(s_fmOfsX, s_fmOfsY);
}

struct FarMeshItem { unsigned key; unsigned short q, r; };

static void DrawFarMeshSplit(const GPUDrawSplit& split)
{
	static FarMeshItem items[1 << 16];
	int n = 0;

	if (s_fmCount == 0)
		return;
	if (g_PsxWholeMapProbe == 1)
		g_wmFarMeshQueued = (unsigned)s_fmCount;
	for (int q = 0; q < s_fmCount; q++)
		for (int r = 0; r < s_fmQueue[q].rangeCount && n < (int)(sizeof(items) / sizeof(items[0])); r++)
		{
			items[n].key = ((unsigned)s_fmQueue[q].ranges[r].tpage << 16) | s_fmQueue[q].ranges[r].clut;
			items[n].q = (unsigned short)q;
			items[n].r = (unsigned short)r;
			n++;
		}
	std::sort(items, items + n, [](const FarMeshItem& a, const FarMeshItem& b) { return a.key < b.key; });

	for (int i = 0; i < n; )
	{
		const unsigned key = items[i].key;
		int j = i;
		while (j < n && items[j].key == key)
			j++;

		const unsigned short tpage = (unsigned short)(key >> 16), clut = (unsigned short)(key & 0xFFFF);
		const FarTexMemo* m = FarTexLookup(tpage, clut);
		if (m != NULL && !m->drop)
		{
			TexFormat texFormat = GET_TPAGE_FORMAT(tpage);
			TextureID textureId = g_vramTexture;
			if (m->tex != 0)
			{
				texFormat = TF_32_BIT_RGBA;
				textureId = m->tex;
			}
			GR_SetStencilMode(split.drawPrimMode);
			g_PsxSplitIsGlyph = m->glyph;
			GR_SetTexture(textureId, texFormat);
			if (texFormat == TF_32_BIT_RGBA)
				GR_SetOverrideTextureSize(m->w, m->h, m->ox, m->oy, m->hw, m->hh);
			GR_SetupClipMode(&split.drawenv.clip, split.drawenv.dfe);
			GR_SetOffscreenState(&split.drawenv.clip, !split.drawenv.dfe);
			GR_SetBlendMode(BM_NONE);
			GR_SetDepthFuncAlways(0);
			GR_FarMeshSetUniforms(&s_fmFrame, s_fmOfsX, s_fmOfsY, g_PsxFarLayerK);
			for (int k = i; k < j; k++)
			{
				const FarMeshQueued&     fq = s_fmQueue[items[k].q];
				const PsyX_FarMeshRange& rg = fq.ranges[items[k].r];
				GR_FarMeshDraw(fq.handle, fq.base, fq.eye, rg.firstIndex, rg.indexCount);
			}
			if (g_PsxWholeMapProbe == 1)
			{
				g_wmFarMeshKeys++;
				g_wmFarMeshDraws += (unsigned)(j - i);
			}
		}
		i = j;
	}
	GR_FarMeshEnd();
}

static int ProcessPsyXPrims(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX* psytex = (DR_PSYX_TEX*)polyTag;
		overrideTexture = psytex->code[0] & 0xFFFFFF;
		overrideTextureWidth = psytex->code[1] & 0xFFF;
		overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		overrideTextureHiresW = 0; /* unknown upscale: shader clamp off */
		overrideTextureHiresH = 0;
		drPsyxTexOverride = overrideTexture;
		drPsyxTexOverrideWidth = overrideTextureWidth;
		drPsyxTexOverrideHeight = overrideTextureHeight;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom texture packet
		DR_PSYX_DBGMARKER* psydbg = (DR_PSYX_DBGMARKER*)polyTag;
		currentSplitDebugText = psydbg->text;
		return 2;
	}
	case 0x03:
	{
		DR_PSYX_MODERN_MESH* modern = (DR_PSYX_MODERN_MESH*)polyTag;
		AddModernSplit(modern->code[1]);
		return 2;
	}
	case 0x06:
		AddFarMeshSplit();
		return 2;
	case 0x05:
	{
		const DR_PSYX_FARQUADS* fq = (const DR_PSYX_FARQUADS*)polyTag;
		EmitFarChain((int)fq->code[1]);
		return 2;
	}
	case 0x04:
	{
		const DR_PSYX_FBCAPTURE* cap = (const DR_PSYX_FBCAPTURE*)polyTag;
		AddFbCaptureSplit((int)(cap->code[0] & 0x3FF), (int)((cap->code[0] >> 10) & 0x1FF),
		                  (int)(cap->code[1] & 0xFFFF), (int)(cap->code[1] >> 16));
		return 2;
	}
	}

	return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;

	/* For the framebuffer-feedback detector in ApplyHiresOverride, which sees
	 * the tpage but not the primitive. A feedback overlay that blends (the
	 * per-map dream shots) needs the stored frame packed with the mask bit set
	 * so its texels read back as semi-transparent; the loading trail draws
	 * opaque and needs it clear, or its black packs to an opaque black rect
	 * instead of nothing. */
	s_curPrimSemiTrans  = (polyTag->code & 2) ? 1 : 0;
	s_curPrimIsFeedback = 0;
	s_curPrimIsScratch  = 0;
	s_curPrimIsGlyph    = 0;
	s_curPrimR0         = polyTag->pad0; /* r0 in every prim struct */

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		if (primSubType == 0x0)
		{
			/* A code=0x00 tag is a NOP / zeroed prim: an unfilled DR-type prim
			 * from a not-fully-ported builder, or an OLD SetDrawOffset that
			 * emitted code[0]=0 / setlen(2) (fixed in libgpu.c 0d7a237). Consume
			 * the tag's DECLARED length so the OT walker stays in sync and draws
			 * nothing. The hardcoded 3 overshot any len!=3 zeroed prim by
			 * (3-len)*4 bytes, so a len=2 zeroed prim logged
			 * "ptag length is not valid (diff=-4)" every frame (86k lines in a
			 * single session before the SetDrawOffset fix). Mirrors
			 * ProcessDrawEnv's case 0 (return polyTag->len) — same failure class. */
			primLength = polyTag->len;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE* drmove = (DR_MOVE*)polyTag;

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drmove->code[2];
			*(uint*)&rect.w = *(uint*)&drmove->code[4];

			MoveImage(&rect, x, y);
			primLength = 5;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD* drload = (DR_LOAD*)polyTag;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drload->code[1];
			*(uint*)&rect.w = *(uint*)&drload->code[2];

			LoadImage(&rect, (u_long*)drload->p);
			//Emulator_UpdateVRAM();			// FIXME: should it be updated immediately?

			// FIXME: is there othercommands?
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
	//default:
	//	eprinterr("got %0x primitive\n", primType);
	}

	if(primLength == 0)
	{
		eprinterr("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}
