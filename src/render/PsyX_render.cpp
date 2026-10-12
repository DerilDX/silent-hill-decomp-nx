#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <map>
#include <vector>
#include <algorithm>
#include <utility>

#include "PsyX/PsyX_public.h"

#include "../platform.h"
#include "../gpu/PsyX_GPU.h"
#include "psx/gtereg.h"

#include "PsyX/PsyX_render.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_backend.h"
#include "PsyX/util/timer.h"

#if defined(_WIN32) && defined(RENDERER_OGL)
/* For the HWND that the ANGLE/EGL surface is created against. */
#   include <SDL_syswm.h>
#endif

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif //def WIN32

#if defined(RENDERER_OGL)

#define USE_PBO					1
#define USE_OFFSCREEN_BLIT		1
#define USE_FRAMEBUFFER_BLIT	1

#else

// OpenGL ES/Web GL has slowdowns and doesn't allow GL_LUMINANCE_ALPHA format as framebuffer, so it's disabled
#define USE_PBO					(OGLES_VERSION == 3)
#define USE_OFFSCREEN_BLIT		(OGLES_VERSION == 3)
#define USE_FRAMEBUFFER_BLIT	(OGLES_VERSION == 3)

#endif

extern SDL_Window* g_window;


#define MAX_NUM_VERTEX_BUFFERS		(2)
#define PSX_SCREEN_ASPECT	(240.0f / 320.0f)			// PSX screen is mapped always to this aspect

/* Pixel aspect ratio: 1 horizontal ortho unit / 1 vertical ortho unit
 * ratio to apply on top of the Hor+ widening. PSX renders to a 320×240
 * framebuffer; displayed on a 4:3 CRT (320/240 = 4/3 matches the CRT
 * aspect exactly), each PSX pixel is SQUARE — so PAR=1.0 is the actual
 * pixel aspect for 320×240 PSX content on a 4:3 NTSC CRT. The 1.09375
 * value previously used here was over-correcting and making characters
 * ~9% taller than they appear on a real PSX (visible on Harry's torso
 * relative to a CRT screenshot at the same location).
 *
 * Runtime-settable so the host can override from config. Cull-bound
 * sites (vw_calc.c, Gfx_MeshDraw) read this same global so culling
 * tracks the active ortho. Default 1.0 = matches 320×240 PSX CRT;
 * 1.094 = some emulators' approximation; 1.143 = 8:7 (overscan look). */
extern "C" {
float g_PsxPixelAspect = 1.0f;
/* Vertical FOV scale for the 3D gameplay world (see GR_SetOffscreenState). Our render
 * shows ~14.7%% too much vertical world vs PSX/DuckStation (measured 0.872 vertical /
 * 1.0 horizontal at a fixed 4:3 spot, extra at the bottom). 0.872 crops the world ortho
 * top-anchored to match; 1.0 = no crop (old behavior). Console `vfov <n>`. */
float g_PsxWorldVScale = 1.0f; /* full 224-row frame; was 0.872 (crop) -- the
    * crop made top- and bottom-anchored shots need different vshift values.
    * Default vfov 1.0 + hfov 0.76 + vshift 11 matches DuckStation in both the
    * cafe and infirmary probes (user-validated 2026-08-25). */
/* EXPLICIT override for the cutscene vertical scale; <= 0 (default) means
 * cutscenes follow g_PsxWorldVScale, cropped exactly like gameplay. Console
 * `cutfov <n>`. Do NOT default this to 1.0 again: ab23f4f shipped that and it
 * squished every cutscene (~15%% more vertical scene) vs the 8/21 builds --
 * un-cropping cutscenes was already tried once before that and reverted for
 * reading as stretched (see the ortho comment at the vscale pick). The knob
 * exists purely for live tuning during the per-cutscene tilt investigation. */
float g_PsxCutsceneVScale = 0.0f;
/* Vertical view shift (amount, PSX screen-Y units) for FIXED-ANGLE camera shots, which
 * frame the top of the scene clipped vs PSX (e.g. a medkit off the top). The GAME applies
 * it (MainLoop, game_main.c) by shifting the GTE projection center down (SetGeomOffset)
 * while g_PsxFixedCamActive is set — NOT by shifting the ortho window here: the ortho
 * shift revealed rows above the frame that screen-space overlay prims (authored 0..224)
 * never cover, showing a faded band at the top. + = view up. Console `vshift`. */
float g_PsxWorldVShift = 0.0f; /* band-aid retired 2026-08-25: the true global
    * offset was the GsInit3D anchor (console 120 vs our 112) -- +8 is now baked
    * at the boot origin (libgs_stub GsInit3D), derived from the SDK convention
    * instead of eye-tuned. The knob remains for A/B. */
/* Same units, but for authored (cutscene / letterboxed) shots, which run at the
 * clean 0 baseline rather than inheriting the gameplay shift. Console `cutshift`.
 * Default 0 = today's framing exactly. */
float g_PsxCutsceneVShift = 0.0f;
int   g_PsxFixedCamActive = 0;
/* Set by the game while a cutscene is active. Cutscenes frame themselves with letterbox
 * bars, so the gameplay vertical crop (g_PsxWorldVScale) is skipped while this is set —
 * otherwise it scaled/clipped the bars + subtitles off the bottom of the frame. */
int   g_PsxCutsceneActive = 0;
/* Set by the game while the world item-pickup take screen is up. The item is the
 * only live 3D (the world behind it is a frozen present), staged by its own
 * authored camera like a cutscene -- so like cutscenes it renders at FULL
 * vertical scale. Leaving the gameplay vfov crop (0.872 top-anchored) on drew
 * every picked-up item 112*(1/0.872-1) ~= 16 PSX units below its PSX position
 * under Hor+ (reported as "pickups ~20 too low"; invisible in pillarbox, which
 * never crops). Horizontal Hor+ behaviour is unchanged. */
int   g_PsxItemTakeActive = 0;
/* Set by the game around the 2D-UI ordering-table draw (OrderingTable2: map-message
 * subtitles, screen fade, cutscene letterbox bars). When set, GR_SetOffscreenState
 * draws that pass at FULL vertical ortho (vscale 1.0) so it isn't clipped by the Hor+
 * world crop. The 3D world (OrderingTable0) keeps the crop, so 3D framing — including
 * cutscenes — is unchanged. This replaces the old g_PsxCutsceneActive vscale skip
 * (which un-cropped the whole cutscene frame and looked stretched). */
int   g_PsxUIOrthoPass = 0;
/* 3D-world HORIZONTAL ortho scale (Hor+ widescreen only). 1.0 = identity (current
 * behaviour); >1 narrows the ortho around center = wider models, <1 = narrower. Pure
 * tuning/preference knob, default neutral. Console `hfov`; not applied to the UI pass. */
float g_PsxWorldHScale = 0.76f; /* 0.872^2: preserves the user-validated shape
    * product (tallness ~ vfov*hfov) with the vertical opened to the full frame. */
/* Where the vertical world crop (g_PsxWorldVScale < 1) sits: 0 = keep the top
 * rows and cut the bottom (today's behaviour), 0.5 = centred, 1 = keep the
 * bottom. Experiment knob for the framing investigation (console `vcropanchor`);
 * a DuckStation-style overscan crop would be centred, ours is top-anchored. */
float g_PsxWorldVCropAnchor = 0.0f;
}
/* Display aspect mode. A real CRT scans the console's framebuffer out to a 4:3
 * screen whatever its line count, so a 320x224 NTSC frame is stretched to 4:3
 * and appears slightly squashed horizontally next to square pixels. Games were
 * composed on that picture, so it is the default.
 *
 * On the widened path the on-screen pixel aspect works out to exactly 1/PAR, so
 * stretching the real framebuffer to 4:3 means PAR = (dispW/dispH) / (4/3):
 * 1.0 for a 320x240 buffer, which is already 4:3, and 1.0714 for the 320x224
 * this game actually outputs. Pillarbox already looked right because its
 * viewport is hardcoded 4:3; only the full-window paths needed this, which is
 * why the two modes disagreed.
 *
 * g_PsxAspectRaw = 1 restores the previous behaviour: the framebuffer at
 * whatever PAR the `par` knob says, faithful to the game's own numbers rather
 * than to the television it was shown on. */
/* Trim on the CRT picture aspect, console `crtaspect`. 1.0 = a textbook 4:3.
 *
 * The 4:3 target assumes the 224-line frame fills the whole screen height,
 * but a console puts 224 active lines inside a 240-line window and how much a
 * given set overscans varies, so the true picture is a few percent either way
 * and no formula settles it. Environments hide that -- nobody knows how wide a
 * corridor should be -- while a human figure shows it immediately, which is why
 * Harry is the thing to judge it on. Below 1.0 makes him taller and thinner.
 *
 * The two textbook readings bracket 1.0 to 1.071: stretch the 224 lines to fill
 * a 4:3 screen, or let them sit inside the 240-line 4:3 window (square pixels).
 * The PC port ships 0.9 anyway -- config crt_aspect_trim, set from a side by
 * side against a real set -- so this default is only the neutral fallback for
 * a caller that never assigns it. */
float g_PsxCrtAspectTrim = 1.0f;
int   g_PsxAspectRaw = 0;
static float PsxDisplayPixelAspect(void)
{
	float w, h;

	if (g_PsxAspectRaw)
		return g_PsxPixelAspect;

	w = (float)activeDispEnv.disp.w;
	h = (float)activeDispEnv.disp.h;
	if (w <= 0.0f || h <= 0.0f)
		return g_PsxPixelAspect;

	/* Solve for the PAR that lands the FINAL on-screen picture on 4:3.
	 *
	 * The widened ortho spans psxW*effScale/hscale across the window and
	 * psxH*vscale down it, with effScale = horScale*PAR. Substituting
	 * horScale = winAspect/psxAspect, the window terms cancel and the
	 * on-screen pixel aspect comes out as vscale*hscale/PAR -- the hfov and
	 * vfov knobs are IN that expression, so a PAR derived from the display
	 * size alone lands wherever those knobs happen to sit. With the shipped
	 * hfov of 0.76 that is 0.709 rather than the 0.933 a television gives.
	 *
	 * Dividing them back out makes the mode mean what it says: the picture is
	 * 4:3 at any resolution and at any knob setting. hfov and vfov therefore
	 * do nothing here, which is the point -- they were eyeball compensations
	 * for this exact problem, and the television is not a matter of taste.
	 * display_aspect = raw restores them. */
	{
		const float trim   = (g_PsxCrtAspectTrim > 0.0f) ? g_PsxCrtAspectTrim : 1.0f;
		const float target = ((4.0f / 3.0f) * trim) / (w / h);
		/* The 2D UI pass renders with hscale and vscale pinned at 1 (its ortho
		 * block forces them), so dividing the WORLD knobs out of its PAR hands
		 * it a compensation for scaling it never had -- which widened the ortho
		 * until the 320-wide UI filled the window and the text stretched. The UI
		 * wants the plain 4:3 picture and nothing else, so it stays pillarboxed
		 * and unscaled at any window width. */
		/* Whatever the ortho block below actually installs, this must divide out
		 * the SAME numbers -- a knob divided out here but not applied there (or
		 * the reverse) is a picture scaled by the difference. The item-take
		 * screen is the case that bites: it pins vscale to 1 while hscale keeps
		 * the world value, so solving its PAR against the world's vfov left the
		 * held item 1/vfov too narrow, which at the shipped 1.06 is the
		 * noticeably tall, thin pickup. Keep these two conditions and the
		 * vscale/hscale lines in GR_SetOffscreenState identical. */
		/* ...and that includes the ortho's g_PcHorPlusEnabled gate. The ortho
		 * applies hscale/vscale ONLY on a Hor+ (3D gameplay) frame; on a Hor+-off
		 * frame -- 2D screens, the inventory, the area-load screen -- it installs
		 * the plain 4:3 ortho with both pinned at 1, so they must be divided out
		 * here as 1 too. This solve had no such gate and read the world vfov on
		 * every frame, so any Hor+-off 3D came out 1/vfov too narrow: invisible at
		 * vfov 1.0, but at the (correct) 1.08 Harry on the load screen was tall,
		 * thin and running off the bottom, and the inventory item kept a residual
		 * ~8% stretch. The vfov is right; this mismatch was the bug. Cutscenes are
		 * cropped by g_PsxCutsceneVScale in the ortho, so mirror that as well. */
		extern int g_PcHorPlusEnabled;
		const int   worldPass = (g_PcHorPlusEnabled && !g_PsxUIOrthoPass);
		const float hs = worldPass ? ((g_PsxWorldHScale > 0.0f) ? g_PsxWorldHScale : 1.0f)
		                           : 1.0f;
		float vs;
		if (!worldPass || g_PsxItemTakeActive)
			vs = 1.0f;
		else if (g_PsxCutsceneActive && g_PsxCutsceneVScale > 0.0f)
			vs = g_PsxCutsceneVScale;
		else
			vs = (g_PsxWorldVScale > 0.0f) ? g_PsxWorldVScale : 1.0f;
		return (hs * vs) / target;
	}
}
#define PSX_NTSC_PIXEL_ASPECT (PsxDisplayPixelAspect())

int g_PreviousBlendMode = BM_NONE;
int g_PreviousDepthMode = 0;
int g_PreviousDepthFuncAlways = 0; /* 0 = glDepthFunc(GL_LEQUAL) (init default), 1 = GL_ALWAYS */
int g_PreviousStencilMode = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
RECT16 g_PreviousFramebuffer = { 0,0,0,0 };
/* PC port: nonzero once GR_StoreFrameBuffer has stored at least one frame in
 * g_fbTexture, so GR_UpdateVRAM can re-blit it over the framebuffer pages a
 * full vram[] re-upload just replaced with stale CPU bytes. */
static int g_fbStoreValid = 0;
RECT16 g_PreviousOffscreen = { 0,0,0,0 };

ShaderID g_PreviousShader = -1;

TextureID g_vramTexturesDouble[2];
TextureID g_vramTexture;
int g_vramTextureIdx = 0;

TextureID g_fbTexture = -1;
TextureID g_offscreenRTTexture = -1;

TextureID g_whiteTexture = -1;
TextureID g_rgLutTexture = -1;
TextureID g_lastBoundTexture = -1;

int g_windowWidth = 0;
int g_windowHeight = 0;

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

// Set to 1 by game code only during states that render a 3D world (InGame, MapEvent).
// When 0, GR_SetOffscreenState uses 4:3 ortho so 2D UI screens don't show VRAM garbage.
int g_PcHorPlusEnabled = 1;

/* PC port: pillarbox 2D screens (menus, load screen) with 4:3 black bars
 * instead of stretching the 4:3 content to fill a widescreen window. Set
 * from g_PcConfig.menuPillarbox in main_pc.c. Default on. */
int g_PcMenuPillarbox = 1;

/* Widescreen presentation mode for 3D gameplay (only used when g_PcHorPlusEnabled=1):
 *   0 = Pillarbox (PSX-faithful): render 4:3 content centered in a 16:9 frame
 *       with black bars on the sides. Characters and scene framing identical to
 *       the original PSX game on a 4:3 CRT, just inside a wider window.
 *   1 = Hor+ widescreen (default): keep vertical FOV, widen horizontal FOV to
 *       fill the 16:9 frame with square pixels (correct character proportions).
 *       Reveals scene content that was cropped on PSX (extra bar counter, walls
 *       beyond the framebuffer edges); per-camera tuning in s_camCorrections[]
 *       compensates per-shot to keep Harry framed like the PSX original.
 *   2 = Stretch (anamorphic): no FOV change, no bars — 4:3 content stretched
 *       to fill 16:9. Characters appear ~33% wider. Not recommended.
 * Default 1 = Hor+ with square pixels. Override from config.cfg via widescreen_mode. */
int g_PcWidescreenMode = 1;

/* PC port: the visible rectangle of the OVERLAY pass (OT2, the g_OtTags0
 * layers), in prim coordinates {left, right, top, bottom}, as last set in
 * GR_SetOffscreenState on a gameplay frame. The SINGLE source of truth for
 * "where is the screen edge" for HUD elements (minimap) that must sit flush to
 * it. They must NOT re-derive it from a window size: the renderer chooses the
 * ortho from g_windowWidth/Height (the logical/render size), while
 * SDL_GetWindowSize returns the actual window, and in borderless those differ
 * (e.g. 640x480 render presented to a 1920x1080 desktop). Nor from the WORLD
 * ortho: its width is divided by hfov and its height scaled by vfov, neither of
 * which the overlay pass applies, so that placed the minimap off the edge at
 * any hfov other than 1. Defaults to the 4:3 frame until the first latch. */
float g_PcHudRect[4] = { -160.0f, 160.0f, -120.0f, 120.0f };

/* PC port: the WORLD (OT0) pass's ortho rect in prim coordinates, latched on
 * every gameplay frame. Same idea as g_PcHudRect above, for the other pass:
 * the world ortho carries hfov and vfov and, in Hor+, the widened margin, so
 * inverting it is the only way to turn a mouse position into the prim
 * coordinates a GTE-projected object lands in. Console object picking
 * (pc_pick.c) reads it. Defaults to the 4:3 frame until the first latch. */
float g_PcWorldOrthoRect[4] = { -160.0f, 160.0f, -120.0f, 120.0f };
extern "C" void PsyX_GetDrawEnvOffset(float* x, float* y);

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 1;

/* PC port (Silent Hill): runtime master gate for PGXP. When 0,
 * MakeVertex* writes 0 into a_zw, the shader takes the `a_zw.y > 100.0`
 * else branch (2D-ortho path). Set from main_pc.c after PcConfig_Load runs
 * (config key: use_pgxp). */
int g_PsxUsePgxp = 0;
int g_cfg_bilinearFiltering = 0;

/* Texture filtering, rebuilt as one mode instead of a single on/off flag whose
 * effect was baked into textures at creation time.
 *   0 off, 1 bilinear, 2 trilinear, 3 anisotropic
 * Trilinear and anisotropic need mip levels or a walked footprint, so what each
 * mode can deliver differs by texture class -- see GR_ApplyTextureFilter for the
 * hardware path (32-bit replacements) and the shader samplers for PSX CLUT
 * textures, where filtering has to be done by hand. */
int g_cfg_textureFilter = 0;
int g_cfg_anisoLevel    = 8;    /* user cap on taps / GL max anisotropy */
static float s_glMaxAniso = 0.0f;  /* 0 = extension absent */

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

/* Apply the current filtering mode to the texture that is bound RIGHT NOW.
 *
 * This used to be decided in GR_CreateRGBATexture, i.e. baked in when the
 * texture was created -- so changing the setting at runtime only affected
 * textures made afterwards, and everything already loaded kept whatever the
 * setting happened to be at boot. That is why the option behaved
 * inconsistently no matter which way it was toggled. Deciding it at bind time
 * makes the setting mean the same thing for every texture, always.
 *
 * Only the 32-bit replacement class can use hardware filtering: a PSX CLUT
 * texture holds palette indices, and interpolating an index yields a colour
 * that is in no neighbouring texel, so those are filtered in the shader instead.
 *
 * Trilinear needs mip levels. Rather than assume, level 1 is queried and the
 * mode falls back to plain linear when there is no mip chain -- asking for
 * MIPMAP_LINEAR on a texture without mips renders it undefined. The query is
 * desktop-only, so GLES takes the same safe fallback. */
/* Textures that must never be filtered, whatever the mode.
 *
 * Font atlases paint their glyph cells edge to edge with NO gutter, so a linear
 * tap at a cell boundary reaches into the neighbouring glyph and draws its first
 * column as a full-height bar beside the letter. hires_override.c already
 * point-samples them at upload for exactly that reason -- but a filter applied
 * per BIND overwrites that decision every frame, which put the ghost text back
 * the moment any filtering mode was selected. Marked textures keep the sampling
 * their loader chose. */
#define GR_NEAREST_SET_SIZE 256
static TextureID s_nearestTex[GR_NEAREST_SET_SIZE];
static int       s_nearestCount = 0;

extern "C" void GR_MarkTextureNearest(TextureID tex)
{
	int i;

	if (tex == 0)
		return;

	for (i = 0; i < s_nearestCount; i++)
	{
		if (s_nearestTex[i] == tex)
			return;
	}

	if (s_nearestCount < GR_NEAREST_SET_SIZE)
		s_nearestTex[s_nearestCount++] = tex;
}

static int GR_TextureIsNearest(TextureID tex)
{
	int i;

	for (i = 0; i < s_nearestCount; i++)
	{
		if (s_nearestTex[i] == tex)
			return 1;
	}

	return 0;
}

static void GR_ApplyTextureFilter(TextureID tex, TexFormat texFormat)
{
	GLint minFilter, magFilter;

	if (texFormat != TF_32_BIT_RGBA)
		return;

	/* Its loader asked for point sampling and meant it. */
	if (GR_TextureIsNearest(tex))
		return;

	if (g_cfg_textureFilter <= 0)
	{
		minFilter = magFilter = GL_NEAREST;
	}
	else
	{
		magFilter = GL_LINEAR;
		minFilter = GL_LINEAR;

		if (g_cfg_textureFilter >= 2 && g_grCaps.texLevelParam)
		{
			GLint mipW = 0;
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &mipW);
			if (mipW > 0)
				minFilter = GL_LINEAR_MIPMAP_LINEAR;
		}
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);

	if (s_glMaxAniso > 1.0f)
	{
		float want = 1.0f;

		if (g_cfg_textureFilter >= 3)
		{
			want = (float)g_cfg_anisoLevel;
			if (want > s_glMaxAniso) want = s_glMaxAniso;
			if (want < 1.0f)         want = 1.0f;
		}

		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, want);
	}
}

/* Does anything need the per-vertex view-space capture this frame?
 *
 * The capture is what stamps a_normal.y = 1.0 on vertices the GTE projected,
 * and that marker is the renderer's only trustworthy "this is world geometry"
 * signal. The flashlight cone and PGXP already required it; bilinear now does
 * too, because without it every vertex reads as 2D and the 3D world silently
 * stays point-sampled. Kept as one predicate so the five capture sites in
 * PsyX_GPU/PsyX_GTE cannot drift apart. */
extern "C" int GR_NeedViewSpaceData(void)
{
	return (g_PsyX_UsePerPixelFlashlight || g_PsxUsePgxp || g_cfg_textureFilter > 0) ? 1 : 0;
}
/* 1 = bilinear-filter menu / 2D-only frames (those set g_PsxDitherSuppressed),
 * independent of the 3D psx_dither setting. Passed to the sampler as bilinearFilter==2. */
int g_cfg_menuFilter = 0;
int g_cfg_affineTextures = 0;
/* When non-zero, the GPU_DITHERING macro applies the 4x4 PSX-style ordered
 * dither to every fragment regardless of the per-primitive `a_texcoord.w`
 * dither flag. Lets the PC port mimic the PSX 5-bit framebuffer noise
 * (which masks texture-page seams and gives the authentic look) on
 * primitives that don't request dither at the prim-tag level. */
int g_cfg_psxDither = 1;
int g_PsxDitherSuppressed = 0;

/* 1 = this frame draws the 3D world in perspective (set by the game from the
 * same test that decides Hor+ widening: InGame, not the paper map, not one of
 * the fullscreen 2D background screens). On such a frame a POLY primitive is
 * world geometry, so it seeds the per-primitive 3D marker that gates texture
 * filtering. Without it the marker came only from the view-space shadow, which
 * resolves ~82% of vertices, and a primitive whose vertices all missed rendered
 * point-sampled next to filtered neighbours -- the blocky walls and tree quads
 * in the filtering reports. SPRT and TILE never reach that marker, so in-game
 * text stays sharp. */
extern "C" { int g_PsxFrame3dClass = 0; }

/* PC port: MSAA sample count for the default framebuffer. 0 = off (no
 * multisample requested), 2/4/8 = N-sample MSAA. Read in GR_InitialiseRender
 * BEFORE the GL context is created (SDL_GL_MULTISAMPLE* attributes), so the
 * host must set it from config BEFORE PsyX_Initialise. When > 0, two blits that
 * read/write the (now multisample) default framebuffer with scaling or a
 * single-sample peer become illegal — GR_StoreFrameBuffer resolves first and
 * GR_PresentLastFrame draws a fullscreen quad instead of blitting. */
int g_cfg_msaaSamples = 0;

/* PC port: full-screen post-process look applied once per frame in
 * GR_PostProcess (PsyX_EndScene, after the freeze capture + console hook, just
 * before swap). 0 = off; 1.. select a built-in look (see the post fragment
 * shader switch). Runtime-settable (launcher config key post_process + the F2
 * in-game cycle). Reads the final composed backbuffer through a resolve
 * texture, so it sees everything (world, UI, console) and is MSAA-safe. */
int g_cfg_postProcess = 0;
#define POST_PROCESS_MODE_COUNT 8
/* PC port: tone-map operator applied as the final step of the post-process
 * shader. 0=off, 1=Reinhard, 2=ACES, 3=Filmic. F3 cycles it in-game. Defined
 * outside the PSYX_HAS_POSTPROCESS guard so non-GL builds still link. */
int g_cfg_tonemap = 0;

/* PC port: per-pixel (fragment-shader) flashlight cone instead of the PSX
 * per-vertex lighting. 0=off (per-vertex), 1=on. F4 toggles it in-game. */
int g_PsyX_UsePerPixelFlashlight = 0;

/* Per-pixel flashlight STYLE. 0 = MODERN: the stylized spotlight (per-fragment
 * Lambert, hard dark surround, linear falloff, warm color — the pre-PR#7 look).
 * 1 = CLASSIC: PSX-calibrated match of the original flashlight (PR#7 —
 * orientation-independent overlay, func_80057658-derived falloff, room color).
 * Pushed to the shader as u_flStyle; game/config key flashlight_style. */
int g_PsyX_FlashlightStyle = 0;

/* PC port: real flashlight shadow mapping. When on (and the per-pixel flashlight
 * is on + active), the frame's opaque geometry is rendered depth-only from the
 * flashlight's point of view into g_shadowDepthTex, and the cone fragment shader
 * samples it so monsters/props cast dynamic shadows inside the beam. 0 = off
 * (rendered output byte-identical to per-pixel flashlight without shadows). */
int   g_PsyX_UseFlashlightShadows = 0;
/* Depth-compare bias in light-clip [0,1] space; tunable via `shadowbias` console. */
float g_PsyX_FlashlightShadowBias = 0.0018f;
/* Optional shadow-look console tweaks, all DEFAULTING TO NO-OP. They exist for
 * live tuning of the prop-shadow "silhouette" look; see the shader.
 *
 * Receiver offset (`shadownormal`, 0 = off): move the sample toward the light
 * along the light ray by (this * dist-to-light). */
float g_PsyX_FlashlightShadowNormalOffset = 0.0f;
/* How much light a fully-occluded pixel loses (`shadowstrength`): 1.0 = pitch black
 * (default/original), lower = softer half-shadow. */
float g_PsyX_FlashlightShadowStrength = 1.0f;
/* Contact-shadow fade distance in view units (`shadowfade`, 0 = off): when > 0, a
 * receiver this far BEHIND its occluder gets no shadow, so a prop drops a tight
 * fading contact shadow instead of a tall silhouette smeared onto the wall. */
float g_PsyX_FlashlightShadowFadeDist = 0.0f;
/* The shadow frustum's near/far, published by GR_BuildShadowMatrix so the cone
 * shader can linearize shadow-map depth for the contact fade above. */
static float g_shadowZNear = 20.0f;
static float g_shadowZFar  = 5200.0f;

/* PC port: per-frame flashlight cone parameters (view space), set by game code.
 * The shader consumes them only when (g_PsyX_UsePerPixelFlashlight &&
 * g_PsyX_FlashlightActive). Defaults make the cone inert (active=0). */
int   g_PsyX_FlashlightActive   = 0;
/* PC port: shadow depth pre-pass master gate. The game re-arms this to 1 only
 * during settled gameplay (SysState_Gameplay, no screen fade / cutscene) and it
 * is reset to 0 at the top of every frame. The flashlight CONE is fine outside
 * gameplay, but the light-POV depth pre-pass + its GL-state churn corrupt
 * unrelated rendering on menu / room-load / transition frames (white flash on
 * transitions, dropped geometry on the options screen). Shadows are a
 * live-gameplay-only effect, so gate the whole effect on this. */
int   g_PsyX_ShadowsAllowed     = 0;
float g_PsyX_FlashlightPos[3]   = { 0.0f, 0.0f, 0.0f };
/* View-space PHYSICAL flashlight position for the shadow map (Harry's chest/hand
 * light bone). Equals g_PsyX_FlashlightPos in third person, but in FPS the cone
 * is pinned at the eye while THIS stays at the real light — a shadow is a
 * world-space fact of light+occluder, so using the true light position makes FPS
 * shadows land exactly where the third-person camera shows them. */
float g_PsyX_FlashlightShadowPos[3] = { 0.0f, 0.0f, 0.0f };
float g_PsyX_FlashlightDir[3]   = { 0.0f, 0.0f, 1.0f };
float g_PsyX_FlashlightColor[3] = { 1.0f, 1.0f, 1.0f };
float g_PsyX_FlashlightInnerCos = 0.94f;  /* ~20 deg */
float g_PsyX_FlashlightOuterCos = 0.76f;  /* ~41 deg */
float g_PsyX_FlashlightRange    = 4000.0f;
/* PC port: flashlight cone coverage-area multiplier, applied to Inner/OuterCos at
 * push time (1.0 = base cone; 1.5 default = ~1.5x coverage). Live-editable via
 * [ / ] + backslash and persisted as config key flashlight_size. */
float g_PsyX_FlashlightSize     = 3.0f;

/* PC port: live per-effect intensity -- [ lowers, ] raises, backslash switches
 * which effect (among the enabled ones); also the FLINT/POSTINT/TMINT console
 * commands. Persisted to config. */
float g_PsyX_FlashlightIntensity = 1.20f; /* cone brightness scale, 0..3 */
/* FPS-mode flashlight overrides: a head-mounted light wants a tighter, dimmer
 * cone than the third-person one. g_PsyX_FlashlightFpsMode is set by the game
 * each frame (= g_PcFpsCam); when 1 the shader uses these instead of the values
 * above. Separately config/console-tunable (flashlight_*_fps). */
float g_PsyX_FlashlightSizeFps      = 1.30f;
float g_PsyX_FlashlightIntensityFps = 2.10f;
int   g_PsyX_FlashlightFpsMode      = 0;
/* FPS shadow parallax. In first person the game pins the flashlight at the eye
 * (bodyprog_80055028.c) so the cone follows the view — but that makes the SHADOW
 * light coincident with the camera, so the depth map equals the camera's own
 * view and every visible surface reads as lit (no shadows). Pull the shadow
 * light BACK along -viewDir (behind the camera) to restore parallax so occluders
 * throw their shadow forward onto the wall/floor; the cone still originates at
 * the eye. (Earlier this dropped straight DOWN, which sat the light below
 * waist-high props and threw their silhouette up out of the object's top.) TPS
 * is unaffected (FpsMode 0). Tunable via `shadowfpsdrop`. Default 0: the shadow
 * now originates at the real chest/hand light (g_PsyX_FlashlightShadowPos), so no
 * artificial offset is needed; this is just an optional extra-parallax nudge. */
float g_PsyX_FlashlightShadowFpsDrop = 0.0f;
float g_cfg_postProcessIntensity = 1.0f; /* post-process effect mix, 0..1 */
float g_cfg_tonemapIntensity     = 1.0f; /* tonemap mix, 0..1 */

/* Image adjustments applied to the final frame ALWAYS (not gated on the
 * post_process filter). 1.0 = neutral for all three. Set from the Brightness
 * screen. When any is off-neutral, GR_PostProcess runs even with no filter. */
float g_cfg_brightness = 1.0f;
float g_cfg_contrast   = 1.0f;
float g_cfg_saturation = 1.0f;

/* Defined later in the file (post-process module); called from GR_InitialisePSX
 * and PsyX_EndScene. */
void GR_InitPostProcess(void);
void GR_PostProcess(void);

int vram_need_update = 1;

/* PC port: runtime gate for framebuffer→VRAM feedback. See PsyX_render.h. */
int g_PsxSkipFramebufferStore = 0;
/* PC port: force the GL error sweep on for any backend (it is automatic on
 * GLES/ANGLE, where silent failures are the norm). */
extern "C" { int g_dbg_glDiag = 0; }
void GR_DiagGLError(const char* where);

/* PC port: how long the framebuffer-feedback store keeps running. Armed to 2 by
 * GR_NoteFeedbackSamplerPrim whenever a prim samples a display buffer, and by
 * Screen_BackgroundMotionBlur for the loading trail's first frame; the store
 * decrements it each present. While it is 0, GR_StoreFrameBufferPsx stamps the
 * feedback rects BLACK (word 0 → the samplers discard → draw nothing) rather
 * than leaving a real or stale frame for some unrelated prim to sample.
 *
 * It was long pinned to the loading screen alone because the per-map dream and
 * ghosting overlays (map6 otherworld, the Lisa scene, after Split Head) read
 * their capture back under the UI ortho while the capture was taken through the
 * world one, and a feedback loop turns that mismatch into striping and doubled
 * subtitles within a few frames. Both rects are recorded now, so the capture
 * matches whichever pass redraws it. */
int g_PsxFeedbackStoreAllowed = 0;

/* PC port: freeze-frame presentation for pause/console/message states.
 * PSX hardware never auto-cleared the framebuffer, so SH1's pause screen
 * simply stopped drawing the world and the last gameplay frame stayed
 * visible under the PAUSED text. PsyCross clears every frame, so the
 * game instead sets g_PsxPresentLastFrame while frozen: every EndScene
 * captures the composed frame into a texture (skipped on frames where
 * the capture was presented, so UI text never bakes in), and BeginScene
 * re-presents it under the new frame's prims. The game sets the flag on
 * freeze ENTRY (same tick) and clears it on exit. */
int g_PsxPresentLastFrame = 0;
static GLuint g_freezeFrameTex = 0;
static GLuint g_freezeFrameFBO = 0;
static int    g_freezeFrameW = 0;
static int    g_freezeFrameH = 0;
int           g_freezeFrameValid = 0; /* exposed: game-side grey-flash probe reads it */
static int    g_freezePresentedThisFrame = 0;
int framebuffer_need_update = 0;

#if defined(__EMSCRIPTEN__) || defined(__RPI__) || defined(__ANDROID__)
#if defined(RENDERER_OGL)
#error It should not be enabled
#endif
#endif



#if USE_OPENGL
typedef struct
{
	GLenum fmt;
	GLuint* pbos;
	uint64_t num_pbos;
	uint64_t dx;
	uint64_t num_downloads;

	int width;
	int height;
	int nbytes; /* number of bytes in the pbo buffer. */
	unsigned char* pixels; /* the downloaded pixels. */
} GrPBO;

int PBO_Init(GrPBO* pbo, GLenum format, int w, int h, int num)
{
	if (pbo->pbos)
	{
		eprinterr("Already initialized. Not necessary to initialize again; or shutdown first.");
		return -1;
	}

	if (0 >= num)
	{
		eprinterr("Invalid number of PBOs: %d", num);
		return -2;
	}

	pbo->fmt = format;
	pbo->width = w;
	pbo->height = h;
	pbo->num_pbos = num;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#if USE_PBO
	if (GL_RED == pbo->fmt || GL_GREEN == pbo->fmt || GL_BLUE == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height;
	}
	else if (GL_RGB == pbo->fmt || GL_BGR == pbo->fmt)
	{
		pbo->nbytes = pbo->width * pbo->height * 3;
	}
	else if (GL_RGBA == pbo->fmt || GL_BGRA == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height * 4;
	}
	else
	{
		eprinterr("Unhandled pixel format, use GL_R, GL_RG, GL_RGB or GL_RGBA.");
		return -3;
	}

	if (pbo->nbytes == 0)
	{
		eprinterr("Invalid width or height given: %d x %d", pbo->width, pbo->height);
		return -4;
	}

	pbo->pbos = (GLuint*)malloc(sizeof(GLuint) * num);
	pbo->pixels = (u_char*)malloc(pbo->nbytes);

	glGenBuffers(num, pbo->pbos);
	for (int i = 0; i < num; ++i)
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, pbo->nbytes, NULL, GL_STREAM_READ);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
	return 0;
}

void PBO_Destroy(GrPBO* pbo)
{
#if USE_PBO
	if(pbo->pbos)
	{
		glDeleteBuffers(pbo->num_pbos, pbo->pbos);
	
		free(pbo->pbos);
		pbo->num_pbos = 0;
		pbo->pbos = NULL;
	}

#endif
	if (pbo->pixels)
	{
		free(pbo->pixels);
		pbo->pixels = NULL;
	}

	pbo->num_downloads = 0;
	pbo->dx = 0;
	pbo->fmt = 0;
	pbo->nbytes = 0;
}

void PBO_Download(GrPBO* pbo)
{
	unsigned char* ptr;
	
#if USE_PBO
	if (pbo->num_downloads < pbo->num_pbos)
	{
		/*
		   First we need to make sure all our pbos are bound, so glMap/Unmap will
		   read from the oldest bound buffer first.
		*/
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		/* glGetTexImage pulls the whole bound texture; ES 3.0 has no such call,
		 * so there we read the equivalent region out of the bound framebuffer
		 * instead. Callers always have both bound to the same image, which is
		 * what makes the two interchangeable (and is the path Android already
		 * ships). The trailing 0 is an offset into the bound pack buffer. */
		if (g_grCaps.getTexImage)
			glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
		else
			glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE, 0);   /* When a GL_PIXEL_PACK_BUFFER is bound, the last 0 is used as offset into the buffer to read into. */
#endif
	}
	else
	{
		/* Read from the oldest bound pbo */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		/* ES 3.0 kept only the ranged map. Same semantics for a full-buffer
		 * read, so the asynchronous double-buffered readback is preserved
		 * rather than degraded to a synchronous glReadPixels-to-CPU. */
		if (g_grCaps.mapBuffer)
			ptr = (unsigned char*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		else
			ptr = (unsigned char*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, pbo->nbytes, GL_MAP_READ_BIT);

		if (NULL != ptr)
		{
			memcpy(pbo->pixels, ptr, pbo->nbytes);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		else
			eprintwarn("Failed to map the buffer\n");

		/* Trigger the next read. */
		if (g_grCaps.getTexImage)
			glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
		else
			glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
	}

	++pbo->dx;
	pbo->dx = pbo->dx % pbo->num_pbos;

	pbo->num_downloads++;

	if (pbo->num_downloads == UINT64_MAX)
		pbo->num_downloads = pbo->num_pbos;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#else
	// FIXME: THIS is very slow
	// Do not use at all

	// glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); /* just make sure we're not accidentilly using a PBO. */
	// glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
}

GLuint		g_glVertexArray[2];
GLuint		g_glVertexBuffer[2];
int			g_curVertexBuffer = 0;

GLuint		g_glBlitFramebuffer;

/* Internal render target: borderless renders the scene at the CHOSEN resolution
 * and stretches it to the desktop, instead of ignoring the resolution and
 * running at desktop size.
 *
 * Everything in this renderer that means "the screen" binds framebuffer 0, so
 * the whole scene is redirected simply by handing those sites GR_ScreenFBO()
 * instead. It returns 0 whenever the target is inactive: always on the ES and
 * ANGLE backends unless borderless picked a different resolution, never on
 * native GL (GR_SceneAlwaysOffscreen). A failed allocation degrades to
 * framebuffer 0 automatically.
 *
 * g_windowWidth/Height stay the RENDER size (all viewport, scissor and aspect
 * maths already key off them and need no changes); g_presentWidth/Height are
 * the real window, used only by the final blit. */
GLuint g_internalFBO      = 0;
static GLuint s_internalColorTex = 0;
static GLuint s_internalColorRbo = 0;
static int    s_internalSamples = 0;
static GLuint s_internalDepthRbo = 0;
static int    s_internalW = 0, s_internalH = 0;
int g_presentWidth = 0, g_presentHeight = 0;

/* How the internal target is scaled onto the window when the two differ.
 * 0 = integer, 1 = nearest, 2 = bilinear, 3 = sharp bilinear. 2 is the default
 * because it is what the present always did. Only matters when a render
 * resolution below the window is in use, which in practice means borderless at
 * a deliberately low resolution. */
int g_PcPresentScale = 2;

/* Native (PSX) resolution: the scene target follows the display mode the game
 * sets each frame -- 320x224 in play, 640x448 for the Konami screens, 320x448
 * menus -- and is scaled onto the window at present, always as a 4:3 picture
 * the way a television showed every one of those modes. Set before
 * GR_InitialiseRender. The PC overlays draw after that scale, at window
 * resolution: at 320x224 their text would be a few pixels tall. */
extern "C" void (*g_PsyX_PostCaptureHook)(void);
int g_PcNativeRender = 0;
static int s_overlayToWindow = 0;

/* Sharp bilinear needs somewhere to put the nearest whole-multiple upscale
 * before the smooth pass. That intermediate is what keeps texel edges crisp
 * while still easing the leftover fraction. */
static GLuint s_sharpFBO = 0, s_sharpTex = 0;
static int    s_sharpW = 0, s_sharpH = 0;

/* What the launcher asked for, and which display mode. The window itself cannot
 * be trusted for this: a borderless window is created at desktop size and SDL
 * then fires a resize event with the desktop size, which is exactly how the
 * chosen resolution used to get lost. */
int g_cfgRenderWidth = 0, g_cfgRenderHeight = 0, g_cfgFullscreenMode = 0;

static GLuint s_resolveFBO = 0, s_resolveTex = 0;
static int    s_suppressWindowMsaa = 0;

extern "C" void GR_MarkTextureNearest(TextureID tex);

extern "C" GLuint GR_ScreenFBO(void)
{
	return s_overlayToWindow ? 0 : g_internalFBO;
}

extern "C" void GR_ApplyPresentSize(int realW, int realH);

/* The scene target as a READ source.
 *
 * Identical to GR_ScreenFBO() unless the scene target is multisampled, in which
 * case reading it directly is illegal -- so it is resolved 1:1 into the mirror
 * and the mirror is returned. Every read of the screen goes through here, which
 * is what makes a multisample scene target survive the freeze capture and the
 * framebuffer-feedback paths. */
extern "C" GLuint GR_ScreenReadFBO(void)
{
	if (s_internalSamples > 0 && s_resolveFBO != 0)
	{
		/* The resolve must leave the caller's DRAW binding alone. Callers
		 * bind their destination first and then ask for this as the source
		 * (the freeze capture binds its FBO, then reads the scene), and a
		 * mid-frame read is followed by more scene drawing. Left pointing at
		 * the mirror, the capture blitted the mirror onto itself -- the frozen
		 * frame stayed black -- and everything drawn after a read landed in
		 * the mirror and was resolved over at present: black pickup and save
		 * prompts with MSAA on native GL. Scissor would clip the resolve. */
		GLint           prevDraw = 0;
		const GLboolean scissor  = glIsEnabled(GL_SCISSOR_TEST);

		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
		if (scissor)
			glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_internalFBO);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_resolveFBO);
		glBlitFramebuffer(0, 0, s_internalW, s_internalH,
		                  0, 0, s_internalW, s_internalH,
		                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDraw);
		if (scissor)
			glEnable(GL_SCISSOR_TEST);
		return s_resolveFBO;
	}

	return g_internalFBO;
}

/* Where the internal target lands inside the real window.
 *
 * Stretch to fill by default. When pillarboxing is on, preserve the render
 * aspect instead and centre it -- so a 4:3 render scaled to a 16:9 desktop gets
 * bars rather than being distorted, in borderless exactly as it would in
 * exclusive fullscreen. Shared with PsyX_MapWindowToViewport so the pointer
 * cannot disagree with what is on screen. */
static void GR_PresentRect(int* outX, int* outY, int* outW, int* outH)
{
	int x = 0, y = 0, w = g_presentWidth, h = g_presentHeight;

	/* Every PSX mode is a 4:3 picture, whatever its pixel count. */
	if (g_PcNativeRender && w > 0 && h > 0)
	{
		if (w * 3 > h * 4)
		{
			w = (h * 4 + 1) / 3;
			x = (g_presentWidth - w) / 2;
		}
		else
		{
			h = (w * 3 + 2) / 4;
			y = (g_presentHeight - h) / 2;
		}
		*outX = x; *outY = y; *outW = w; *outH = h;
		return;
	}
	const bool wantPillarbox =
		!g_PcNativeRender &&
		((g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
		 (!g_PcHorPlusEnabled && g_PcMenuPillarbox));

	if (wantPillarbox && s_internalW > 0 && s_internalH > 0 && h > 0)
	{
		const float srcAspect = (float)s_internalW / (float)s_internalH;
		const float dstAspect = (float)w / (float)h;

		if (dstAspect > srcAspect)
		{
			w = (int)(h * srcAspect + 0.5f);
			x = (g_presentWidth - w) / 2;
		}
		else if (dstAspect < srcAspect)
		{
			h = (int)(w / srcAspect + 0.5f);
			y = (g_presentHeight - h) / 2;
		}
	}

	*outX = x; *outY = y; *outW = w; *outH = h;
}

/* Scale the internal target into the window rect by the configured method.
 *
 * Integer and sharp bilinear both start from the largest whole multiple of the
 * render size that still fits, because that is what puts every texel on the
 * same number of pixels. Integer stops there and centres the result; sharp
 * bilinear takes that crisp intermediate and eases only the leftover fraction,
 * so it fills the window without the mush of a plain linear stretch. Nearest
 * and bilinear stretch straight to the rect. */
static void GR_PresentBlit(GLuint src, int dx, int dy, int dw, int dh)
{
	const int mode = g_PcPresentScale;

	if (s_internalW <= 0 || s_internalH <= 0 || dw <= 0 || dh <= 0)
		return;

	/* Nothing to choose when the sizes already match. */
	if (dw == s_internalW && dh == s_internalH)
	{
		glBlitFramebuffer(0, 0, s_internalW, s_internalH, dx, dy, dx + dw, dy + dh,
		                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
		return;
	}

	if (mode == 1 || mode == 2)
	{
		glBlitFramebuffer(0, 0, s_internalW, s_internalH, dx, dy, dx + dw, dy + dh,
		                  GL_COLOR_BUFFER_BIT, mode == 1 ? GL_NEAREST : GL_LINEAR);
		return;
	}

	{
		int       n  = dw / s_internalW;
		const int ny = dh / s_internalH;

		if (ny < n) n = ny;
		if (n < 1)  n = 1; /* window smaller than the render: just fit it */

		if (mode == 0 && g_PcNativeRender)
		{
			/* A PSX mode's pixels are not square (320x224 fills a 4:3 screen),
			 * so whole multiples go on the LINES, as a television's scan does,
			 * and the width follows 4:3. Shimmer-free vertically, which is where
			 * the eye catches it. */
			int       k  = dh / s_internalH;
			const int iw0 = (s_internalH * 4) / 3;

			if (k < 1) k = 1;
			while (k > 1 && iw0 * k > dw)
				k--;
			{
				const int ih = s_internalH * k;
				const int iw = (ih * 4) / 3;
				const int ix = dx + (dw - iw) / 2;
				const int iy = dy + (dh - ih) / 2;

				glBlitFramebuffer(0, 0, s_internalW, s_internalH, ix, iy, ix + iw, iy + ih,
				                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			return;
		}

		if (mode == 0)
		{
			/* Centre the whole-multiple image inside the rect it was given. */
			const int iw = s_internalW * n;
			const int ih = s_internalH * n;
			const int ix = dx + (dw - iw) / 2;
			const int iy = dy + (dh - ih) / 2;

			glBlitFramebuffer(0, 0, s_internalW, s_internalH, ix, iy, ix + iw, iy + ih,
			                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
			return;
		}

		/* Sharp bilinear. At 1x the intermediate buys nothing and this would be
		 * a plain linear stretch, so skip the extra pass. */
		if (n < 2)
		{
			glBlitFramebuffer(0, 0, s_internalW, s_internalH, dx, dy, dx + dw, dy + dh,
			                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
			return;
		}

		{
			const int sw = s_internalW * n;
			const int sh = s_internalH * n;

			if (s_sharpFBO == 0 || s_sharpW != sw || s_sharpH != sh)
			{
				if (s_sharpTex) { glDeleteTextures(1, &s_sharpTex); s_sharpTex = 0; }
				if (s_sharpFBO) { glDeleteFramebuffers(1, &s_sharpFBO); s_sharpFBO = 0; }

				glGenTextures(1, &s_sharpTex);
				glBindTexture(GL_TEXTURE_2D, s_sharpTex);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA,
				             GL_UNSIGNED_BYTE, NULL);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glGenFramebuffers(1, &s_sharpFBO);
				glBindFramebuffer(GL_FRAMEBUFFER, s_sharpFBO);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				                       GL_TEXTURE_2D, s_sharpTex, 0);

				if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				{
					/* Degrade to the smooth stretch, never to a black frame. */
					glDeleteFramebuffers(1, &s_sharpFBO); s_sharpFBO = 0;
					glDeleteTextures(1, &s_sharpTex);     s_sharpTex = 0;
					s_sharpW = 0; s_sharpH = 0;
					glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
					glBlitFramebuffer(0, 0, s_internalW, s_internalH,
					                  dx, dy, dx + dw, dy + dh,
					                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
					return;
				}
				s_sharpW = sw; s_sharpH = sh;
			}

			glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_sharpFBO);
			glBlitFramebuffer(0, 0, s_internalW, s_internalH, 0, 0, sw, sh,
			                  GL_COLOR_BUFFER_BIT, GL_NEAREST);

			glBindFramebuffer(GL_READ_FRAMEBUFFER, s_sharpFBO);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			glBlitFramebuffer(0, 0, sw, sh, dx, dy, dx + dw, dy + dh,
			                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
	}
}

void GR_DestroyInternalTarget(void)
{
	if (s_sharpTex) { glDeleteTextures(1, &s_sharpTex); s_sharpTex = 0; }
	if (s_sharpFBO) { glDeleteFramebuffers(1, &s_sharpFBO); s_sharpFBO = 0; }
	s_sharpW = 0; s_sharpH = 0;
	if (s_internalColorTex) { glDeleteTextures(1, &s_internalColorTex); s_internalColorTex = 0; }
	if (s_internalColorRbo) { glDeleteRenderbuffers(1, &s_internalColorRbo); s_internalColorRbo = 0; }
	if (s_resolveTex)       { glDeleteTextures(1, &s_resolveTex); s_resolveTex = 0; }
	if (s_resolveFBO)       { glDeleteFramebuffers(1, &s_resolveFBO); s_resolveFBO = 0; }
	s_internalSamples = 0;
	if (s_internalDepthRbo) { glDeleteRenderbuffers(1, &s_internalDepthRbo); s_internalDepthRbo = 0; }
	if (g_internalFBO)      { glDeleteFramebuffers(1, &g_internalFBO); g_internalFBO = 0; }
	s_internalW = s_internalH = 0;
}

/* Returns 1 when the scene should render into the internal target at w x h. */
int GR_SetInternalResolution(int w, int h)
{
	GLenum st;

	if (w <= 0 || h <= 0)
	{
		GR_DestroyInternalTarget();
		return 0;
	}

	if (g_internalFBO != 0 && s_internalW == w && s_internalH == h)
		return 1;

	GR_DestroyInternalTarget();

	/* Multisample the scene target when MSAA is on, so borderless keeps its
	 * antialiasing, and resolve into a single-sample MIRROR that every read goes
	 * through (GR_ScreenReadFBO). Reading a multisample framebuffer is illegal,
	 * and the freeze capture and framebuffer-feedback paths do read the scene
	 * target -- that is what black-screened the first attempt.
	 *
	 * Desktop GL only. There the read-back helper uses glGetTexImage on its own
	 * texture, so no read ever touches the scene target; on GLES it falls back to
	 * glReadPixels from the bound framebuffer, which would be the multisample
	 * target. Rather than guess at that path on ANGLE, GLES keeps the
	 * single-sample target and the existing "MSAA off in borderless" notice. */
	s_internalSamples = (g_cfg_msaaSamples > 0 && !g_grIsGLES) ? g_cfg_msaaSamples : 0;

	if (s_internalSamples > 0)
	{
		glGenRenderbuffers(1, &s_internalColorRbo);
		glBindRenderbuffer(GL_RENDERBUFFER, s_internalColorRbo);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_internalSamples, GL_RGBA8, w, h);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	glGenTextures(1, &s_internalColorTex);
	glBindTexture(GL_TEXTURE_2D, s_internalColorTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenRenderbuffers(1, &s_internalDepthRbo);
	glBindRenderbuffer(GL_RENDERBUFFER, s_internalDepthRbo);
	if (s_internalSamples > 0)
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_internalSamples, GL_DEPTH24_STENCIL8, w, h);
	else
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	/* The scene target. Multisample colour when AA is on; the texture created
	 * above then becomes the resolve mirror instead of the scene's own colour. */
	glGenFramebuffers(1, &g_internalFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, g_internalFBO);
	if (s_internalSamples > 0)
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_internalColorRbo);
	else
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_internalColorTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_internalDepthRbo);

	st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (st != GL_FRAMEBUFFER_COMPLETE)
	{
		eprintwarn("internal render target %dx%d incomplete (0x%04X); rendering at window size\n", w, h, (unsigned)st);
		GR_DestroyInternalTarget();
		return 0;
	}

	if (s_internalSamples > 0)
	{
		glGenFramebuffers(1, &s_resolveFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, s_resolveFBO);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_internalColorTex, 0);
		st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		if (st != GL_FRAMEBUFFER_COMPLETE)
		{
			eprintwarn("internal resolve mirror %dx%d incomplete (0x%04X); rendering at window size\n", w, h, (unsigned)st);
			GR_DestroyInternalTarget();
			return 0;
		}
	}

	s_internalW = w;
	s_internalH = h;
	eprintf("*internal render target: %dx%d, stretched to the window\n", w, h);
	return 1;
}
GrPBO		g_glFramebufferPBO;

GLuint		g_glVRAMFramebuffer;

GLuint		g_glOffscreenFramebuffer;
GrPBO		g_glOffscreenPBO;

#endif

/* Resolve g_cfg_renderBackend into something this machine can actually provide,
 * and prepare the environment for it. Runs BEFORE the window exists, because
 * ANGLE reads ANGLE_DEFAULT_PLATFORM when libEGL creates its display and SDL
 * decides GL-vs-ES from the attributes set before SDL_CreateWindow.
 *
 * Anything unavailable degrades to native GL rather than failing: a config
 * asking for Vulkan on a box with no ANGLE must still boot the game. */
static void GR_ResolveBackend(void)
{
#if defined(RENDERER_OGL)
	int requested = g_cfg_renderBackend;

	if (requested == PSYX_BACKEND_AUTO)
	{
		/* Deliberately conservative: native GL is the path with years of
		 * testing behind it, so "auto" does NOT quietly move players onto a
		 * translator. It only exists so a future build can flip the default. */
		requested = PSYX_BACKEND_GL;
	}

	if (PsyX_Backend_IsTranslated(requested) && !PsyX_Backend_AngleAvailable())
	{
		eprintwarn("Render backend '%s' needs ANGLE (libEGL.dll + libGLESv2.dll) next to the game; falling back to native OpenGL\n",
			PsyX_Backend_GetName(requested));
		requested = PSYX_BACKEND_GL;
	}

	g_grActiveBackend = requested;
	g_grIsGLES = (requested == PSYX_BACKEND_GLES || PsyX_Backend_IsTranslated(requested)) ? 1 : 0;

	/* Translated backends do NOT ask SDL for a GL context at all — PsyCross
	 * builds the EGL display itself in GR_InitialiseGLContext so it can pass
	 * ANGLE the platform attributes that pick D3D11 vs Vulkan. Only a plain
	 * 'gles' request goes through SDL, where the native driver's ES context is
	 * a perfectly good answer. */
	if (!PsyX_Backend_IsTranslated(requested) && g_grIsGLES)
	{
		/* Must be set before SDL_CreateWindow: SDL picks WGL vs EGL from the
		 * profile mask while choosing the window's pixel format, not later when
		 * the context is created. */
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	}

	if (g_grActiveBackend != PSYX_BACKEND_GL)
		eprintf("*Render backend: %s\n", PsyX_Backend_GetDescription(g_grActiveBackend));
#else
	/* Fixed-target builds (Android/iOS/RPi/web) have exactly one context type. */
	g_grActiveBackend = PSYX_BACKEND_GLES;
	g_grIsGLES = 1;
#endif
}

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
int GR_InitialiseGLContext(char* windowName, int fullscreen)
{
	int windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

#if defined(RENDERER_OGL)
	/* A window created with SDL_WINDOW_OPENGL gets an SDL-chosen pixel format
	 * for an SDL-owned GL context. On the translated backends the context comes
	 * from our own EGL display instead, and ANGLE wants a plain window to make
	 * its swapchain against, so the flag is dropped there. */
	if (PsyX_Backend_IsTranslated(g_grActiveBackend))
		windowFlags &= ~SDL_WINDOW_OPENGL;
#endif

#if defined(__ANDROID__)
	windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
	/* 0 = windowed, 1 = exclusive fullscreen, 2 = borderless (fullscreen
	 * desktop: covers the screen at desktop resolution, no mode switch). */
	if (fullscreen == 2)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	else if (fullscreen)
		windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

	if(g_windowWidth <= 0 || g_windowHeight <= 0)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);

	/* PC port: if MSAA was requested but the driver can't provide a multisample
	 * pixel format, SDL_CreateWindow fails. Drop MSAA and retry once so the game
	 * still launches (just without antialiasing). */
	if (g_window == NULL && g_cfg_msaaSamples > 0)
	{
		eprintwarn("Window creation with %dx MSAA failed (%s); retrying without MSAA\n", g_cfg_msaaSamples, SDL_GetError());
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		g_cfg_msaaSamples = 0;
		g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);
	}

	if (g_window == NULL)
	{
		eprinterr("Failed to initialise SDL window!\n");
		return 0;
	}

#if !defined(__ANDROID__) && !defined(RENDERER_OGLES)
	/* PC port: take the foreground on launch. The launcher spawns the game and
	 * keeps keyboard focus itself, so the FIRST Enter the player presses to skip
	 * the intro lands on the launcher's default (Launch) button and spawns ANOTHER
	 * copy of the game — repeatedly. Windows lets a child of the foreground process
	 * set foreground, so raising the freshly-created window puts intro-skip (and
	 * all) input on the game where it belongs. */
	SDL_ShowWindow(g_window);
	SDL_RaiseWindow(g_window);
#endif

#if defined(RENDERER_OGLES)

#if defined(__ANDROID__)
	//Override to full screen.
	SDL_DisplayMode displayMode;
	if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0)
	{
		screenWidth = displayMode.w;
		windowWidth = displayMode.w;
		screenHeight = displayMode.h;
		windowHeight = displayMode.h;
	}
#endif

	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, OGLES_VERSION);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

	if(!SDL_GL_CreateContext(g_window))
	{
		eprinterr("Failed to initialise - OpenGL ES %d.x is not supported.\n", OGLES_VERSION);
		return 0;
	}

#elif defined(RENDERER_OGL)

	/* Translated backends: build the EGL display ourselves against SDL's window
	 * so ANGLE can be told which device to use. Everything after this point —
	 * the whole GR_* renderer — is unaware of the difference. */
	if (PsyX_Backend_IsTranslated(g_grActiveBackend))
	{
		void* nativeWindow = NULL;
#if defined(_WIN32)
		SDL_SysWMinfo wmInfo;
		SDL_VERSION(&wmInfo.version);
		if (SDL_GetWindowWMInfo(g_window, &wmInfo))
			nativeWindow = (void*)wmInfo.info.win.window;
#endif

		if (!PsyX_Angle_Create(nativeWindow, g_grActiveBackend, g_cfg_msaaSamples))
		{
			eprintwarn("Backend '%s' could not be initialised through ANGLE; falling back to native OpenGL\n",
				PsyX_Backend_GetName(g_grActiveBackend));

			SDL_DestroyWindow(g_window);
			g_window = NULL;

			g_grIsGLES = 0;
			g_grActiveBackend = PSYX_BACKEND_GL;
			SDL_GL_ResetAttributes();
			SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
			SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);
			if (g_cfg_msaaSamples > 0 && !s_suppressWindowMsaa)
			{
				SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
				SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_cfg_msaaSamples);
			}

			windowFlags |= SDL_WINDOW_OPENGL;
			g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);
			if (g_window == NULL)
			{
				eprinterr("Failed to re-create window for the OpenGL fallback!\n");
				return 0;
			}
			SDL_ShowWindow(g_window);
			SDL_RaiseWindow(g_window);
		}
	}

	/* An explicit 'gles' request still goes through SDL: the profile attributes
	 * were set in GR_ResolveBackend, before SDL_CreateWindow, because that is
	 * where SDL decides between WGL and EGL. */
	if (g_grIsGLES && !PsyX_Angle_Active())
	{
		if (!SDL_GL_CreateContext(g_window))
		{
			eprintwarn("Backend '%s' could not create an OpenGL ES 3.0 context (%s); falling back to native OpenGL\n",
				PsyX_Backend_GetName(g_grActiveBackend), SDL_GetError());

			/* The window was created with an ES pixel format, so it cannot be
			 * reused for a desktop GL context — tear it down and start over. */
			SDL_DestroyWindow(g_window);
			g_window = NULL;

			g_grIsGLES = 0;
			g_grActiveBackend = PSYX_BACKEND_GL;
			SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "0");
			SDL_GL_ResetAttributes();
			SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
			SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);
			if (g_cfg_msaaSamples > 0 && !s_suppressWindowMsaa)
			{
				SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
				SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_cfg_msaaSamples);
			}

			g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);
			if (g_window == NULL)
			{
				eprinterr("Failed to re-create window for the OpenGL fallback!\n");
				return 0;
			}
			SDL_ShowWindow(g_window);
			SDL_RaiseWindow(g_window);
		}
	}

	if (!g_grIsGLES)
	{
		int major_version = 3;
		int minor_version = 3;
		int profile = SDL_GL_CONTEXT_PROFILE_CORE;

		// find best OpenGL version
		do
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

			if (SDL_GL_CreateContext(g_window))
				break;

			minor_version--;

		} while (minor_version >= 0);

		if (minor_version == -1)
		{
			eprinterr("Failed to initialise - OpenGL 3.x is not supported. Please update video drivers.\n");
			return 0;
		}
	}
#endif

	return 1;
}
#endif

/* Desktop GL and ES 3.0 share most entry points but not all, and under ANGLE
 * the desktop-only ones simply fail to resolve. glad leaves those pointers NULL,
 * so probing them is both the cheapest and the most honest capability test —
 * it asks the loader what it actually got rather than trusting a version string. */
static void GR_ProbeCapabilities(void)
{
#ifdef USE_GLAD
	g_grCaps.polygonMode      = (glad_glPolygonMode != NULL);
	g_grCaps.getTexImage      = (glad_glGetTexImage != NULL);
	g_grCaps.mapBuffer        = (glad_glMapBuffer != NULL);
	g_grCaps.drawBuffer       = (glad_glDrawBuffer != NULL);
	g_grCaps.clearDepthDouble = (glad_glClearDepth != NULL);
	g_grCaps.debugGroups      = (glad_glPushDebugGroup != NULL && glad_glPopDebugGroup != NULL);
#else
	g_grCaps.polygonMode      = !g_grIsGLES;
	g_grCaps.getTexImage      = !g_grIsGLES;
	g_grCaps.mapBuffer        = !g_grIsGLES;
	g_grCaps.drawBuffer       = !g_grIsGLES;
	g_grCaps.clearDepthDouble = !g_grIsGLES;
	g_grCaps.debugGroups      = 0;
#endif

	/* ES 3.1 promoted glGetTexLevelParameteriv; ES 3.0 does not have it. */
	g_grCaps.texLevelParam = !g_grIsGLES;

#ifdef USE_GLAD
	/* Every branch keyed off these flags picks a fallback that must itself be
	 * resolved. Depth clear is the one with no third option, so verify rather
	 * than trust: a NULL on both sides means the loader pass above did not do
	 * what it claims, and a silent jump to 0 in the first frame is a miserable
	 * way to find that out. */
	if (!g_grCaps.clearDepthDouble && glad_glClearDepthf == NULL)
		eprinterr("Neither glClearDepth nor glClearDepthf resolved - the GL loader is incomplete\n");
#endif

	if (g_grIsGLES)
	{
		eprintf("*GL caps: polygonMode=%d getTexImage=%d mapBuffer=%d drawBuffer=%d clearDepthD=%d texLevelParam=%d noperspective=%d\n",
			g_grCaps.polygonMode, g_grCaps.getTexImage, g_grCaps.mapBuffer,
			g_grCaps.drawBuffer, g_grCaps.clearDepthDouble, g_grCaps.texLevelParam,
			g_grCaps.noperspective);
	}

	/* 'noperspective' is core in desktop GLSL 1.30+. GLES has it only through
	 * NV_shader_noperspective_interpolation, which ANGLE does expose on D3D11
	 * (HLSL has the qualifier natively). Without it, affine texture mapping
	 * silently becomes perspective-correct on PGXP-projected geometry. */
	if (!g_grIsGLES)
	{
		g_grCaps.noperspective = 1;
	}
	else
	{
		g_grCaps.noperspective = 0;
		const char* exts = (const char*)glGetString(GL_EXTENSIONS);
		if (exts && strstr(exts, "GL_NV_shader_noperspective_interpolation"))
			g_grCaps.noperspective = 1;
#ifdef USE_GLAD
		else if (glad_glGetStringi)
		{
			/* Core-profile / ES3 drivers return NULL for the monolithic string. */
			GLint n = 0;
			glGetIntegerv(GL_NUM_EXTENSIONS, &n);
			for (GLint i = 0; i < n; i++)
			{
				const char* e = (const char*)glGetStringi(GL_EXTENSIONS, i);
				if (e && strcmp(e, "GL_NV_shader_noperspective_interpolation") == 0)
				{
					g_grCaps.noperspective = 1;
					break;
				}
			}
		}
#endif
	}
}

int GR_InitialiseGLExt()
{
#ifdef USE_GLAD
	/* SDL_GL_GetProcAddress, not gladLoadGL(): glad's built-in loader goes
	 * straight to opengl32.dll, which resolves nothing when the live context
	 * came from ANGLE's libGLESv2. SDL's loader routes to whichever library
	 * actually backs the context, so this is correct for both and strictly more
	 * correct than the old path even for plain desktop GL. */
	/* When PsyCross owns the EGL context, SDL knows nothing about it and its
	 * loader returns NULL for everything — the entry points have to come from
	 * ANGLE's own modules. */
	GLADloadproc loader = PsyX_Angle_Active()
		? (GLADloadproc)PsyX_Angle_GetProcAddress
		: (GLADloadproc)SDL_GL_GetProcAddress;

	GLenum err = gladLoadGLLoader(loader);

	if (err == 0)
		return 0;

	/* This glad is generated as gl=3.1 + gles2=2.0, and the two loaders fill
	 * disjoint-but-overlapping halves of the same pointer table. The desktop
	 * loader above already resolved everything ES 3.0 shares with GL 3.1 (VAOs,
	 * glMapBufferRange, glDrawBuffers, glBlitFramebuffer...), because those are
	 * spelled identically in both APIs. What it cannot resolve are the entry
	 * points that exist ONLY in ES — glClearDepthf and glDepthRangef, the float
	 * spellings that desktop GL did not get until 4.1 — so they stay NULL and
	 * calling one is an immediate jump to address 0.
	 *
	 * Running the ES2 loader afterwards fills exactly those. It is not a
	 * replacement for the desktop pass: the ES2 set has none of the ES 3.0
	 * functions this renderer needs. Both, in this order. */
	if (g_grIsGLES)
		gladLoadGLES2Loader(loader);
#endif

	const char* rend = (const char*)glGetString(GL_RENDERER);
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	eprintf("*Video adapter: %s by %s\n", rend, vendor);

	const char* versionStr = (const char*)glGetString(GL_VERSION);
	eprintf("*OpenGL version: %s\n", versionStr);

	const char* glslVersionStr = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	eprintf("*GLSL version: %s\n", glslVersionStr);

	/* ANGLE reports the device it settled on inside GL_RENDERER, e.g.
	 * "ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 Direct3D11 vs_5_0 ps_5_0)". Worth
	 * logging plainly, because it is the only way to confirm from a user's log
	 * that the requested backend is the one really in use. */
	if (g_grIsGLES && rend && strstr(rend, "ANGLE") == NULL)
		eprintwarn("Backend '%s' expected ANGLE but got '%s'\n", PsyX_Backend_GetName(g_grActiveBackend), rend);

	GR_ProbeCapabilities();

	/* Anisotropy is an extension on both desktop GL and ES; 0 means absent and
	 * the hardware path simply never asks for it. */
	{
		const char* exts = (const char*)glGetString(GL_EXTENSIONS);
		s_glMaxAniso = 0.0f;
		if (exts && strstr(exts, "texture_filter_anisotropic"))
		{
			glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &s_glMaxAniso);
			eprintf("*Anisotropic filtering available, max %.0fx\n", s_glMaxAniso);
		}
	}

	/* GL is up, so the internal target can be built now. A borderless window was
	 * created at desktop size, so this is where the chosen resolution is turned
	 * back into the RENDER size instead of being lost. */
	{
		int realW = 0, realH = 0;
		SDL_GetWindowSize(g_window, &realW, &realH);
		GR_ApplyPresentSize(realW, realH);
	}

	return 1;
}

/* The single decision point for "what size do we render, what size do we
 * present". Called after window creation, on every SDL resize, and whenever the
 * resolution is changed at runtime -- the resize event is what used to silently
 * overwrite the chosen resolution with the desktop size in borderless. */
/* Native desktop OpenGL draws the scene into the internal target even at the
 * window's own size, and the window's back buffer is written once per frame,
 * by the present blit.
 *
 * The frame reads its own image back mid-frame -- the freeze capture, the
 * framebuffer-feedback store, the VRAM readback -- and with the scene in the
 * window's back buffer those were reads of the default framebuffer. On
 * NVIDIA's OpenGL present path the screen then showed those intermediate
 * states for a refresh or two: the fog-coloured clear before the world was
 * drawn (the whole-screen grey flash) and the world before the overlays (the
 * controls panel blinking out). A present-time readback proved every image
 * handed to the swap was complete, and the same build through ANGLE's D3D11
 * never did it. With the target, nothing but the final blit touches the
 * window. ES and ANGLE keep their existing path. */
static int GR_SceneAlwaysOffscreen(void)
{
	return g_grActiveBackend == PSYX_BACKEND_GL && !g_grIsGLES;
}

extern "C" void GR_ApplyPresentSize(int realW, int realH)
{
	if (realW <= 0 || realH <= 0)
		return;

	g_presentWidth  = realW;
	g_presentHeight = realH;

	/* Native: the target keeps whatever mode the game is in (GR_NativeTrack);
	 * a window resize only moves where it lands. 320x240 until the first
	 * PutDispEnv says otherwise. */
	if (g_PcNativeRender)
	{
		const int w = (s_internalW > 0) ? s_internalW : 320;
		const int h = (s_internalH > 0) ? s_internalH : 240;

		if (GR_SetInternalResolution(w, h))
		{
			g_windowWidth  = w;
			g_windowHeight = h;
			return;
		}
		eprintwarn("Native resolution unavailable: the internal target could not be created; "
		           "rendering at the window size instead.\n");
		g_PcNativeRender = 0;
	}

	if (g_cfgFullscreenMode == 2 && g_cfgRenderWidth > 0 && g_cfgRenderHeight > 0 &&
	    (g_cfgRenderWidth != realW || g_cfgRenderHeight != realH) &&
	    GR_SetInternalResolution(g_cfgRenderWidth, g_cfgRenderHeight))
	{
		g_windowWidth  = g_cfgRenderWidth;
		g_windowHeight = g_cfgRenderHeight;

		return;
	}

	if (GR_SceneAlwaysOffscreen() && GR_SetInternalResolution(realW, realH))
	{
		g_windowWidth  = realW;
		g_windowHeight = realH;

		return;
	}

	GR_DestroyInternalTarget();
	g_windowWidth  = realW;
	g_windowHeight = realH;
}

int GR_InitialiseRender(char* windowName, int width, int height, int fullscreen)
{
	g_windowWidth = width;
	g_windowHeight = height;

	/* Remember the request before SDL gets a chance to replace it. */
	g_cfgRenderWidth    = width;
	g_cfgRenderHeight   = height;
	g_cfgFullscreenMode = fullscreen;

	/* The window must NOT be multisampled if the scene is going to render into
	 * an internal target, because the present is a glBlitFramebuffer into the
	 * default framebuffer -- and blitting a single-sample source into a
	 * MULTISAMPLE destination is INVALID_OPERATION. The blit silently does
	 * nothing and the screen stays black, which is exactly what MSAA + borderless
	 * did. Decide it here, before the pixel format is chosen. */
	{
		SDL_DisplayMode dm;

		s_suppressWindowMsaa = 0;

		if (g_cfg_msaaSamples > 0 &&
		    (g_PcNativeRender ||
		     (fullscreen == 2 && width > 0 && height > 0 &&
		      SDL_GetDesktopDisplayMode(0, &dm) == 0 &&
		      (width != dm.w || height != dm.h))))
		{
			/* The WINDOW must stay single-sample either way: the present is a
			 * blit into the default framebuffer, and a single-sample source into
			 * a multisample destination is INVALID_OPERATION. Antialiasing moves
			 * onto the internal target instead, which is multisampled and
			 * resolved before every read and before the present -- so MSAA is
			 * kept, just not on the window. Not available on GLES, where the
			 * read-back helper would glReadPixels the scene target; there it
			 * really is off (see GR_SetInternalResolution). */
			s_suppressWindowMsaa = 1;

			if (g_grIsGLES)
			{
				eprintwarn("MSAA disabled: borderless at %dx%d renders through an internal target, and this "
				           "backend reads that target back directly, which a multisample target does not allow.\n",
				           width, height);
			}
		}
	}

	// Due to debugging in fullscreen
	SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	/* Before any GL attribute that depends on the context type, and before the
	 * window: picks GL vs ES and points ANGLE at D3D11/Vulkan if asked. */
	GR_ResolveBackend();

	/* Native GL always renders through the internal target (see
	 * GR_SceneAlwaysOffscreen), and the present blit cannot write a
	 * multisample window: antialiasing moves onto the target, as it already
	 * does for a borderless render resolution. */
	if (GR_SceneAlwaysOffscreen() && g_cfg_msaaSamples > 0)
		s_suppressWindowMsaa = 1;

	/* MSAA is not survivable on the ES backends. Confirmed by bisect:
	 * renderer=gles renders a completely black frame with MSAA on and works
	 * with it off, on a driver that reports a valid ES 3.2 context, compiles
	 * every shader, uploads every texture and submits the whole world each
	 * frame. Desktop GL tolerates the multisample framebuffer blits this
	 * renderer performs; ES enforces the spec and the frame never reaches the
	 * screen. FMV survives because it does not share that path, which is
	 * exactly how the report described it.
	 *
	 * Dropped here rather than left to fail: a translated/ES backend is what
	 * someone selects BECAUSE their GL driver is misbehaving, so handing them a
	 * black screen is the worst possible answer. Antialiasing is the thing
	 * worth losing. Native GL is untouched. */
	if (g_grIsGLES && g_cfg_msaaSamples > 0)
	{
		eprintwarn("MSAA %dx is not supported on the '%s' backend (its framebuffer blits are \n"
			"illegal on ES); disabling it for this run. Use the OpenGL (native) renderer \n"
			"if you want antialiasing.\n",
			g_cfg_msaaSamples, PsyX_Backend_GetName(g_grActiveBackend));
		g_cfg_msaaSamples = 0;
	}

#if USE_OPENGL
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

	/* PC port: request MSAA on the default framebuffer when enabled. Must be
	 * set before the window/context is created (GR_InitialiseGLContext). The
	 * driver picks a multisample pixel format; if it can't, window creation is
	 * retried without MSAA inside GR_InitialiseGLContext. */
	if (g_cfg_msaaSamples > 0 && !s_suppressWindowMsaa)
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_cfg_msaaSamples);
	}

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	if (!GR_InitialiseGLContext(windowName, fullscreen))
	{
		eprinterr("Failed to Initialise GL Context!\n");
		return 0;
	}
#endif

	if (!GR_InitialiseGLExt())
	{
		eprinterr("Failed to Intialise GL extensions\n");
		return 0;
	}
#endif
	
	return 1;
}

void GR_Shutdown()
{
#if USE_OPENGL
	glDeleteVertexArrays(2, g_glVertexArray);
	glDeleteBuffers(2, g_glVertexBuffer);

	PBO_Destroy(&g_glFramebufferPBO);
	PBO_Destroy(&g_glOffscreenPBO);

	glDeleteFramebuffers(1, &g_glBlitFramebuffer);
	glDeleteFramebuffers(1, &g_glOffscreenFramebuffer);
	glDeleteFramebuffers(1, &g_glVRAMFramebuffer);

	GR_DestroyTexture(g_vramTexturesDouble[0]);
	GR_DestroyTexture(g_vramTexturesDouble[1]);

	GR_DestroyTexture(g_whiteTexture);
	GR_DestroyTexture(g_rgLutTexture);
	GR_DestroyTexture(g_fbTexture);
	GR_DestroyTexture(g_offscreenRTTexture);
#endif

	/* After the GL objects above: destroying the EGL context first would make
	 * every one of those deletes a call with nothing current. */
	PsyX_Angle_Destroy();
}

void GR_UpdateSwapIntervalState(int swapInterval)
{
#if defined(RENDERER_OGL)
	/* Only on a change. This runs every frame from PsyX_BeginScene, and SDL
	 * hands every call straight to wglSwapIntervalEXT / eglSwapInterval, which
	 * on NVIDIA's DXGI-layered OpenGL present reconfigures presentation even
	 * for the same value. GR_ResetDevice drops it to 0 and the next frame puts
	 * it back, so a device reset still re-applies it. */
	static int s_applied = -1000;

	if (swapInterval == s_applied)
		return;
	s_applied = swapInterval;

	if (PsyX_Angle_Active())
		PsyX_Angle_SetSwapInterval(swapInterval);
	else
		SDL_GL_SetSwapInterval(swapInterval);
#endif
}

/* Follow the display mode the game set. Done at the top of a frame, so a frame
 * is never split across two targets; the one frame drawn after a switch is
 * still mapped correctly (DrawEnvDimensions reads the live mode), just at the
 * previous size. Sizes outside anything the PSX can display are ignored. */
static void GR_NativeTrack(void)
{
	const int w = activeDispEnv.disp.w;
	const int h = activeDispEnv.disp.h;

	if (!g_PcNativeRender || g_internalFBO == 0)
		return;
	if (w < 256 || w > 640 || h < 192 || h > 512)
		return;
	if (w == s_internalW && h == s_internalH)
		return;

	if (GR_SetInternalResolution(w, h))
	{
		g_windowWidth  = w;
		g_windowHeight = h;
	}
}

void GR_BeginScene()
{
	g_lastBoundTexture = 0;
	GR_NativeTrack();

#if USE_OPENGL
#ifdef RENDERER_OGLES
	glClearDepthf(1.0f);
#else
	/* glClearDepth (double) is desktop-only; ES 3.0 has the float form. */
	if (g_grCaps.clearDepthDouble)
		glClearDepth(1.0f);
	else
		glClearDepthf(1.0f);
#endif
	glClear(GL_DEPTH_BUFFER_BIT);
	glClear(GL_STENCIL_BUFFER_BIT);
#endif

	GR_UpdateVRAM();
	GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

	if (g_dbg_wireframeMode)
	{
		GR_SetWireframe(1);

#if USE_OPENGL
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
#endif
	}
}

void GR_EndScene()
{
	GR_DiagGLError("scene draw");

	framebuffer_need_update = 1;
	
	if (g_dbg_wireframeMode)
		GR_SetWireframe(0);

#if USE_OPENGL
	glBindVertexArray(0);
#endif
}

//----------------------------------------------------------------------------------------

unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];

void GR_ResetDevice()
{
	GR_UpdateSwapIntervalState(0);
}

typedef struct
{
	// shader itself
	ShaderID shader;

#if USE_OPENGL
	GLint projectionLoc;
	GLint projection3DLoc;
	GLint bilinearFilterLoc;
	GLint anisoTapsLoc;
	GLint ditherForceLoc;
	GLint pixelScaleLoc;
	GLint texelSizeLoc;
	GLint texOffsetLoc;
	GLint hiresHalfLoc;
	GLint fogColorLoc;
	GLint voidProbeLoc;
	GLint fogToBlackLoc;
	GLint fogStrengthLoc;
	GLint pgxpEnabledLoc;
	GLint szMaxLoc;
	GLint pgxpFarWLoc;
	GLint worldFarBiasLoc;
	GLint flashlightOnLoc;
	GLint untexturedLoc;
	GLint flStyleLoc;
	GLint flLightPosLoc;
	GLint flDirLoc;
	GLint flColorLoc;
	GLint flInnerCosLoc;
	GLint flOuterCosLoc;
	GLint flRangeLoc;
	GLint shadowOnLoc;
	GLint shadowMatrixLoc;
	GLint shadowBiasLoc;
	GLint shadowTexelLoc;
	GLint shadowNormalOffsetLoc;
	GLint shadowStrengthLoc;
	GLint shadowClipLoc;
	GLint shadowFadeDistLoc;
#endif
} GTEShader;

GTEShader g_gte_shader_4;
GTEShader g_gte_shader_8;
GTEShader g_gte_shader_16;
GTEShader g_gte_shader_32_rgba;
GTEShader g_modern_shader_4, g_modern_shader_8, g_modern_shader_16, g_modern_shader_32_rgba;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_bilinearFilterLoc;
GLint u_anisoTapsLoc;
GLint u_ditherForceLoc;
GLint u_pixelScaleLoc;
GLint u_texelSizeLoc;
GLint u_texOffsetLoc;
GLint u_hiresHalfLoc;
GLint u_fogColorLoc;
GLint u_voidProbeLoc;
extern "C" int g_PsxVoidProbeArmed;
GLint u_fogToBlackLoc;
GLint u_fogStrengthLoc;
GLint u_pgxpEnabledLoc;
GLint u_szMaxLoc;
GLint u_worldFarBiasLoc;
/* Depth channel Step 4: writer-side world far-push margin M (PsyX_GPU.cpp). */
extern "C" int g_PsxPgxpWorldFarBias;
GLint u_pgxpFarWLoc;
GLint u_flashlightOnLoc;
GLint u_untexturedLoc;
GLint u_flStyleLoc;
GLint u_flLightPosLoc;
GLint u_flDirLoc;
GLint u_flColorLoc;
GLint u_flInnerCosLoc;
GLint u_flOuterCosLoc;
GLint u_flRangeLoc;
GLint u_shadowOnLoc;
GLint u_shadowMatrixLoc;
GLint u_shadowBiasLoc;
GLint u_shadowTexelLoc;
GLint u_shadowNormalOffsetLoc;
GLint u_shadowStrengthLoc;
GLint u_shadowClipLoc;
GLint u_shadowFadeDistLoc;

/* Flashlight shadow map (see g_PsyX_UseFlashlightShadows). Depth-only FBO rendered
 * from the light POV each frame; g_shadowLightMatrix maps view space -> light clip.
 * Column-major, identity until the first shadow pass computes it. */
/* Allocated size of the flashlight shadow map. Runtime so it can be raised
 * without a rebuild; GR_EnsureShadowTarget reallocates when it changes. */
int g_PsyX_ShadowMapSize = 1024;
static int s_shadowTexSize = 0;
#define PSYX_SHADOW_MAP_SIZE g_PsyX_ShadowMapSize
static GLuint g_shadowFBO = 0;
static GLuint g_shadowDepthTex = 0;
static ShaderID g_shadowDepthShader = (ShaderID)-1;
static GLint g_shadowDepthMatrixLoc = -1;
static float g_shadowLightMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

float g_PsyX_FogColor[3] = { 0.0f, 0.0f, 0.0f };
/* World fog density multiplier. 1.0 = native PC shader fog; >1 deepens it toward the
 * PSX double-poly fog look the single-pass shader drops. Console `fogstr`; pushed as
 * u_fogStrength and applied against v_fogAmount in the fragment shader. Default 1.1 to
 * match the PSX/DuckStation fog depth (was the value baked in the original PR). */
float g_PsyX_FogStrength = 1.1f;
/* Fog mode for the CURRENT blend: 1 = fade additive/subtractive prims (blood, muzzle
 * flash, etc.) toward black so they fade OUT in fog, instead of blending toward the
 * light fog color — which whitened their edges/faded pixels in daytime. Set by
 * GR_SetBlendMode; pushed to the shader as u_fogToBlack. */
int g_PsxFogToBlack = 0;

/* Colour comes from a CPU-baked table (GR_InitRG8LUT) on texture unit 2
 * instead of being reconstructed per pixel with float division + floor():
 * samplePSX now hands back the two raw VRAM bytes and lut() indexes the table
 * with them, so no driver-sensitive arithmetic is left in the decode.
 *
 * Both the index and the sampled colour are recovered with floor(x*255+0.5) —
 * the same snap GPU_FETCH_VRAM_FUNC already uses — so neither depends on how
 * precisely a driver normalises UNSIGNED_BYTE to float. The table bakes each
 * 5-bit channel as v<<3, and v*8/256 == v/32 exactly in binary32, which is
 * what the old fract(floor(rg / K) / 32.0) produced: the decode is bit-exact
 * against the previous one for all 65536 inputs, colour and alpha alike.
 * Alpha must stay exactly 0.0 / 0.5 / 1.0 because BM_AVERAGE feeds it straight
 * to GL_SRC_ALPHA, and it is resolved per texel (not after the bilinear mix)
 * so a colour-0 texel still reads as a hole when its neighbours do not. */
#define GPU_PACK_RG_FUNC\
	"	uniform sampler2D s_rgLut;\n"

#define GPU_DECODE_RG_FUNC\
	"	vec4 lut(vec2 rg) {\n"\
	"		vec2 idx = (floor(rg * 255.0 + 0.5) + 0.5) * (1.0 / 256.0);\n"\
	"		vec4 t = texture2D(s_rgLut, idx);\n"\
	"		vec3 c = floor(t.rgb * 255.0 + 0.5) * (1.0 / 256.0);\n"\
	"		return vec4(c, (rg.x + rg.y == 0.0) ? 0.0 : (t.w > 0.25 ? 0.5 : 1.0));\n"\
	"	}\n"

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

/* PSX 4x4 ordered dither.
 *
 * Native hardware applies a signed offset before quantizing to the 5-bit
 * framebuffer. We match the matrix the original game's GPU uses (values
 * in [-4..+3]) divided by 255 so it lands in 24-bit color space.
 *
 * `u_ditherForce` is the master enable the PC config exposes: 1 when the
 * user selects "PSX dither", 0 for both "off" and "bilinear". When 0 we
 * emit NO dither at all — the per-primitive `v_texcoord.w` tpage flag is
 * deliberately NOT OR'd in here, because otherwise prims whose tpage sets
 * the DTD bit would still show a dither pattern with the setting off.
 *
 * After adding the dither offset we quantize to 5 bits per channel
 * (PSX framebuffer depth) so the noise translates to actual color
 * banding rather than staying as a smooth gradient — that's what
 * produces the visible "film grain" look. */
#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"\
		"		mat4 dither = mat4(\n"\
		"			-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"			+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"			-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"			+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 4.0) * 4.0);\n"\
		"		float dStrength = u_ditherForce * v_is3d;\n"\
		"		fragColor.xyz += vec3(dither[dc.x][dc.y] * dStrength);\n"\
		"		if (u_ditherForce > 0.5 && v_is3d > 0.5) {\n"\
		"		    fragColor.xyz = floor(fragColor.xyz * 32.0 + 0.5) / 32.0;\n"\
		"		}\n"

/* Same dither as GPU_DITHERING but skips the `fragColor *= v_color`
 * step — used by the main fragment shader where we now multiply by
 * v_color and apply fog BEFORE dithering, so the dither + quantize is
 * the very last operation on the fragment and the fog mix() can't
 * smooth out the quantization steps that produce the visible noise.
 *
 * 8-pixel cell, native PSX dither strength (1:1 with the original
 * matrix). Earlier resolution-scaling and 1.6x intensity made it
 * look chunkier than PSX; this version sticks closer to the
 * authentic noise level. Gated OFF for additive/subtractive prims (u_fogToBlack==1):
 * the signed dither offset over-brightened the faint low-cyan anti-aliased rim of the
 * subtractive blood decal, so it subtracted ~nothing over a light floor and leaked the
 * bright floor through as white speckled edges. Opaque geometry (u_fogToBlack==0) keeps
 * full dither.
 *
 * Dither and the 5-bit quantize both fade out with fog (scaled by 1 - fogAmt). The
 * void behind the world is cleared to the raw 8-bit fog colour (GR_Clear), but a
 * dithered, quantized fragment lands on quantize(fogColor) +- dither -- a constant
 * shade off the void plus a crosshatch -- so a fully fogged object never matches the
 * fog it sits in (reported: objects show as distinct outlines in the far void, in
 * daytime fog and night darkness alike). At full fog there is no object detail left
 * to preserve, so fading both makes a fogged fragment resolve to exactly fogColor =
 * the void (no seam, culling not needed to hide it), while the near scene is
 * untouched (fogAmt ~ 0 there). fogAmt comes from GPU_LIT_TAIL just above. */
#	define GPU_DITHERING_NO_VCOLOR\
		"		mat4 dither = mat4(\n"\
		"			-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"			+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"			-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"			+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 8.0) * 4.0);\n"\
		"		float dStrength = u_ditherForce * v_is3d * (1.0 - float(u_fogToBlack)) * (1.0 - fogAmt);\n"\
		"		fragColor.xyz += vec3(dither[dc.x][dc.y] * dStrength);\n"\
		"		if (u_ditherForce > 0.5 && v_is3d > 0.5) {\n"\
		"		    vec3 qcol = floor(fragColor.xyz * 32.0 + 0.5) / 32.0;\n"\
		"		    fragColor.xyz = mix(qcol, fragColor.xyz, fogAmt);\n"\
		"		}\n"

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { return array[idx]; }"

#else

#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"

/* GLES 2 fallback: no dither (would need ivec2 / mat4 indexing). */
#	define GPU_DITHERING_NO_VCOLOR\
		""

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { if(idx == 0) return array.x; else return array.y; }"

#endif

#define GPU_SAMPLE_TEXTURE_4BIT_FUNC\
    "   // returns the two VRAM bytes (lut index)\n"\
    "   vec2 samplePSX(vec2 tc){\n"\
    "       vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
    "       vec2 comp = VRAM(uv);\n"\
    "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"\
    "       float v = _idx2(comp, index / 2) * (255.0 / 16.0);\n"\
    "       float f = floor(v + 0.001);\n"\
    "       vec2 c = vec2( (v - f) * 16.0, f );\n"\
    "       vec2 clut_pos = v_page_clut.zw;\n"\
    "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;\n"\
    "       return VRAM(clut_pos);\n"\
    "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC\
	"	// returns the two VRAM bytes (lut index)\n"\
	"	vec2 samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 comp = VRAM(uv);\n"\
	"		vec2 clut_pos = v_page_clut.zw;\n"\
	"		int index = int(mod(tc.x, 2.0));\n"\
	"		clut_pos.x += _idx2(comp, index) * 255.0 * c_VRAMTexel.x;\n"\
	"		return VRAM(clut_pos);\n"\
	"	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC\
	"	vec2 samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		return VRAM(uv);\n"\
	"	}\n"


/* Filtered sampling of PSX CLUT textures.
 *
 * Hardware filtering cannot be used here: the VRAM texture holds palette
 * INDICES, so the GPU would interpolate index values and look up a colour that
 * is not in either neighbour. Every tap has to be resolved through lut() first
 * and combined afterwards, which is what these do.
 *
 * Coverage-weighted, which the previous version was not. PSX CLUT entry 0 is
 * transparent, and blending its colour in like any other tap dragged black into
 * every edge; the old code then thresholded the interpolated coverage at 0.5 and
 * discarded, turning soft edges into hard ones. Together that made filtered text
 * and sprites look WORSE than unfiltered -- dark-fringed and jagged. Weighting
 * each tap by its own opacity and dividing by the total keeps transparent texels
 * from contributing colour at all, and the fragment is only discarded when no
 * tap is opaque. */
#define GPU_BILINEAR_SAMPLE_FUNC \
	/* Declared HERE, not with the other uniforms further down the shader:
	 * GLSL requires a declaration before use, and these sampler functions are
	 * emitted BEFORE that uniform block. Declaring it late cost a fragment
	 * shader that failed to compile outright ("u_anisoTaps : undeclared
	 * identifier"), which is what garbled characters -- the whole program was
	 * dead, not just the filtering. */\
	"	uniform float u_anisoTaps; // >1 = anisotropic tap budget\n"\
	/* Texture coverage of the last filtered sample: opaque tap weight over total
	 * tap weight. main() uses it to fade the EDGE of an additive/subtractive
	 * sprite, where a partly covered fragment should add proportionally less
	 * light instead of all or nothing. Globals rather than out params so the
	 * sampler signatures (and the aniso loop) stay as they are. Left at 1.0 by
	 * the unfiltered path, so nearest sampling behaves exactly as before. */\
	"	float g_tapCov;\n"\
	"	float g_tapWt;\n"\
	/* One tap, weighted by its own opacity. PSX CLUT entry 0 is transparent;
	 * blending its colour like any other tap drags black into every edge, so
	 * a transparent tap contributes nothing and only adds to coverage when
	 * it is opaque. */\
	"	vec4 TapWeighted(vec2 idx, float w, inout float cov) {\n"\
	/* Clamp the tap to the POLYGON'S own UV bounds. PSX pages pack unrelated
	 * regions edge to edge, so a tap that steps one texel past a UV seam reads
	 * another body part's texels -- the bright line down a character's pants
	 * seam under bilinear/aniso. Bounds are the prim's vertex-UV bbox, stamped
	 * CPU-side; all-zero means a path that never sets them, which keeps the
	 * unclamped behaviour. DuckStation resolves this identically. */\
	"		if (v_uvlim.z > 0.0 || v_uvlim.w > 0.0)\n"\
	"			idx = clamp(idx, v_uvlim.xy, v_uvlim.zw);\n"\
	"		vec2 rg = samplePSX(idx);\n"\
	"		float o = (rg.x + rg.y > 0.0) ? w : 0.0;\n"\
	"		cov += o;\n"\
	"		g_tapWt += w;\n"\
	"		return lut(rg) * o;\n"\
	"	}\n"\
	/* Bilinear as premultiplied colour plus coverage, WITHOUT discarding.
	 * Anisotropy layers many of these, and a discard inside that loop would
	 * throw the whole fragment away the moment one tap landed on a
	 * transparent texel -- which is what reduced characters to silhouettes.
	 * The discard belongs at the end, once total coverage is known. */\
	"	vec4 BilinearCov(vec2 P, inout float cov) {\n"\
	"		vec2 tapP = P - 0.5;\n"\
	"		vec2 f = fract(tapP);\n"\
	"		vec2 p = floor(tapP);\n"\
	"		vec4 c = TapWeighted(p, (1.0 - f.x) * (1.0 - f.y), cov);\n"\
	"		c += TapWeighted(p + vec2(1.0, 0.0), f.x * (1.0 - f.y), cov);\n"\
	"		c += TapWeighted(p + vec2(0.0, 1.0), (1.0 - f.x) * f.y, cov);\n"\
	"		c += TapWeighted(p + vec2(1.0, 1.0), f.x * f.y, cov);\n"\
	"		return c;\n"\
	"	}\n"\
	"	vec4 BilinearTextureSample(vec2 P) {\n"\
	"		float cov = 0.0;\n"\
	"		g_tapWt = 0.0;\n"\
	"		vec4 c = BilinearCov(P, cov);\n"\
	"		if (cov <= 0.0) { discard; }\n"\
	"		g_tapCov = cov;\n"\
	"		return c / cov;\n"\
	"	}\n"\
	/* Anisotropic for CLUT textures: several bilinear taps along the major
	 * axis of the texture-coordinate derivative, the axis a surface seen at a
	 * grazing angle is stretched along.
	 * 
	 * The step is CLAMPED. PSX texture pages sit side by side in VRAM with no
	 * gutter, so a tap that walks past the page edge reads the neighbouring
	 * page and returns unrelated colours -- the stray bright fringes on a
	 * character. Bounding the total offset to a few texels keeps the footprint
	 * inside the page for the small textures this game uses. */\
	"	vec4 AnisoTextureSample(vec2 P) {\n"\
	"		vec2 dPdx = dFdx(P);\n"\
	"		vec2 dPdy = dFdy(P);\n"\
	"		float lx = length(dPdx);\n"\
	"		float ly = length(dPdy);\n"\
	"		vec2 major = (lx >= ly) ? dPdx : dPdy;\n"\
	"		float mn = min(lx, ly);\n"\
	"		float ratio = (mn > 0.0001) ? (max(lx, ly) / mn) : 1.0;\n"\
	"		float taps = clamp(floor(ratio), 1.0, u_anisoTaps);\n"\
	"		float span = length(major) * (taps - 1.0);\n"\
	"		if (taps <= 1.0 || span <= 0.0) { return BilinearTextureSample(P); }\n"\
	"		if (span > 4.0) { major *= 4.0 / span; }\n"\
	"		vec4 acc = vec4(0.0);\n"\
	"		float cov = 0.0;\n"\
	"		g_tapWt = 0.0;\n"\
	/* GLSL ES needs a constant loop bound; unused iterations are skipped.
	 * GLSL ES needs a constant loop bound; unused iterations are skipped. */\
	"		for (int i = 0; i < 16; i++) {\n"\
	"			if (float(i) >= taps) break;\n"\
	"			float t = (float(i) + 0.5) / taps - 0.5;\n"\
	"			acc += BilinearCov(P + major * t, cov);\n"\
	"		}\n"\
	"		if (cov <= 0.0) { discard; }\n"\
	"		g_tapCov = cov;\n"\
	"		return acc / cov;\n"\
	"	}\n"

#define GPU_NEAREST_SAMPLE_FUNC \
	"vec4 NearestTextureSample(vec2 P) {\n"\
	"	vec2 rg = samplePSX(P);\n"\
	"	if(rg.x + rg.y == 0.0) {discard;}\n"\
	"	return lut(rg);\n"\
	"}\n"

/* The VRAM texture stores each 16-bit PSX pixel as two normalised bytes
 * (low/high). Every downstream step — the 4/8-bit CLUT index math and the
 * decode-table index — treats the sampled value as an exact k/255 and feeds it
 * to floor(). The Windows GL driver normalises UNSIGNED_BYTE -> float
 * precisely enough that this holds; Mesa (Steam Deck / Proton) rounds
 * slightly differently, so floor() lands one bucket off and colours / palette
 * lookups corrupt. Snap each channel to its exact integer byte right at the
 * source so all downstream math is bit-exact on every driver (a no-op where
 * normalisation is already exact). */
#if (VRAM_FORMAT == GL_LUMINANCE_ALPHA)
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return floor(texture2D(s_texture, uv).ra * 255.0 + 0.5) * (1.0 / 255.0); }\n"
#else
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return floor(texture2D(s_texture, uv).rg * 255.0 + 0.5) * (1.0 / 255.0); }\n"
#endif

/* PGXP path (only when u_pgxpEnabled AND this vertex has a precise W>0):
 * build the SAME ortho clip position from the precise float screen X/Y, then
 * scale xyzw by W so the GPU's perspective divide returns the identical NDC
 * position but interpolates varyings (UV/colour) perspective-correctly. Depth
 * (a_zw.x) is left as the FLAT per-prim affine value so Z-ordering matches the
 * painter/submission model this renderer relies on — true per-vertex depth was
 * tried 3x (v1 67a852f / v2 f60aab4 / v3 99e5b18) and ALWAYS dropped/warped
 * coplanar triangles, because depth here is one flat value per prim and ties
 * resolve by OT order; a per-vertex subset is incompatible. Z-fight relief now
 * comes purely from un-quantising that flat depth (PsyX_SetNextPrimSz) when
 * PGXP is on. The else-branch is byte-identical to the legacy affine path. */
#define GTE_PERSPECTIVE_CORRECTION \
		"	if (u_pgxpEnabled > 0 && a_pgxp.z > 0.0) {\n"\
		"		vec4 b = Projection * vec4(a_pgxp.xy, a_zw.x, 1.0);\n"\
		"		float W = a_pgxp.z;\n"\
		"		if (u_pgxpFarW > 0.0) W = min(W, u_pgxpFarW);\n"\
		/* Depth channel Step 4: marked OPAQUE WORLD verts (a_extra.w, set by
		 * ApplyGtePerVertexDepth for GL_ALWAYS-class prims only) take true
		 * per-vertex depth from the unquantized view W (a_pgxp.z, pre-FarW-
		 * clamp), on the SAME constant linear scale as every flat depth:
		 * ndc = 2*vz/F - 1 — the ortho Projection negates a_zw.x, so this is
		 * exactly what the flat path yields for the same vz. World never
		 * depth-tests itself (ALWAYS painter), so this only makes the depth
		 * field actors LEQUAL against per-pixel accurate — closing the
		 * distant grazing gaps flat-average depth leaves. */\
		"		if (a_extra.w > 0.5) {\n"\
		"			float dvz = min(a_pgxp.z + u_worldFarBias, u_szMax);\n"\
		"			b.z = (2.0 * dvz / u_szMax - 1.0) * b.w;\n"\
		"		}\n"\
		"		gl_Position = vec4(b.xyz * W, b.w * W);\n"\
		"	} else {\n"\
		"		gl_Position = Projection * vec4(a_position.xy, a_zw.x, 1.0);\n"\
		"	}\n"

#define GTE_VERTEX_SHADER \
	"	attribute vec4 a_position;\n"\
	"	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"\
	"	attribute vec4 a_color;\n"\
	"	attribute vec4 a_extra; // texcoord.xy ofs, unused.xy\n"\
	"	attribute vec4 a_zw;\n"\
	"	attribute vec3 a_pgxp;\n"\
	"	attribute vec3 a_viewpos;\n"\
	"	attribute vec3 a_normal; // .z = character fade (0 lit, 1 faded out)\n"\
	"	attribute float a_geom3d; // 1 = GTE-projected primitive (world geometry)\n"\
	"	attribute vec4 a_uvlim; // per-poly UV bounds (texels), all-zero = none\n"\
	"	uniform mat4 Projection;\n"\
	"	uniform mat4 Projection3D;\n"\
	"	uniform int u_pgxpEnabled;\n"\
	"	uniform float u_szMax;\n"\
	"	uniform float u_pgxpFarW;\n"\
	"	uniform float u_worldFarBias;\n"\
	"	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"\
	"	void main() {\n"\
	"		v_texcoord = a_texcoord;\n"\
	"		v_texcoord.xy += a_extra.xy * 0.5;\n"\
	"		v_color = a_color;\n"\
	"		v_color.xyz *= a_texcoord.z;\n"\
	"		v_page_clut.x = fract(a_position.z / 16.0) * 1024.0;\n"\
	"		v_page_clut.y = floor(a_position.z / 16.0) * 256.0;\n"\
	"		v_page_clut.z = fract(a_position.w / 64.0);\n"\
	"		v_page_clut.w = floor(a_position.w / 64.0) / 512.0;\n"\
	"		v_page_clut.xy += c_UVFudge;\n"\
	"		v_page_clut.zw += c_UVFudge;\n"\
	GTE_PERSPECTIVE_CORRECTION\
	/* v_is3d gates dither + bilinear so 2D logos/UI render sharp.
	 * a_pgxp.z is the PGXP view W: > 0 only on vertices the GTE
	 * actually projected, which is exactly the 3D content. It only
	 * separates 3D from 2D while the runtime PGXP master gate is on —
	 * with PGXP off PgxpFillVertex never runs, so a_pgxp.z is 0 for
	 * everything and without the u_pgxpEnabled override every prim
	 * would read v_is3d=0 and lose dither / bilinear on real 3D
	 * geometry too (visibly blocky tree leaves, etc.). PGXP off falls
	 * back to "always treat as 3D" — the legacy behaviour. Anything
	 * that must hold with PGXP off therefore cannot lean on v_is3d;
	 * use the frame class (g_PsxDitherSuppressed) instead. */	"		v_is3d = (u_pgxpEnabled > 0) ? ((a_pgxp.z > 0.0) ? 1.0 : 0.0) : 1.0;\n"\
	"		v_z = (gl_Position.z - 40.0) * 0.005;\n"\
	"		v_fogAmount = clamp(a_extra.z / 127.0, 0.0, 1.0);\n"\
	"		v_viewpos = a_viewpos;\n"\
	"		v_fade = a_normal.z;\n"\
	/* a_normal.y is 1.0 only for a vertex that carried a view-space FIFO
	 * entry, i.e. one the GTE actually projected -- a true "this is world
	 * geometry" marker. v_is3d cannot answer that: it reads a_pgxp.z, which
	 * says "was PGXP-projected", so with PGXP off it is 1.0 for the 2D UI too
	 * and with PGXP on it is 0.0 for world prims that took the affine path.
	 * That is why bilinear filtered menu text and missed the world. */\
	"		v_geom3d = a_geom3d;\n"\
	"		v_uvlim = a_uvlim;\n"\
	/* The legacy affine screen path has gl_Position.w == 1, so v_viewpos is not
	 * perspective-correct there. Encode receiver position over view Z, adjusted
	 * for whichever clip W this vertex uses, then reconstruct it in the fragment
	 * shader. This also stays coherent for mixed PGXP/fallback triangles. */\
	"		float shadowInvZ = (a_viewpos.z > 0.0) ? (1.0 / a_viewpos.z) : 0.0;\n"\
	"		float shadowClipW = (u_pgxpEnabled > 0 && a_pgxp.z > 0.0) ? ((u_pgxpFarW > 0.0) ? min(a_pgxp.z, u_pgxpFarW) : a_pgxp.z) : 1.0;\n"\
	"		v_shadowViewPos = vec4(a_viewpos * shadowInvZ, shadowInvZ) * shadowClipW;\n"\
	"	}\n"

/* Fog + per-pixel flashlight + shadow uniforms shared by every shader that
 * renders lit world geometry - including the 32-bit RGBA override shader,
 * whose textures (virtual pool slots, hi-res/pack replacements) cover the
 * same world surfaces as the VRAM samplers. */
#define GPU_LIT_UNIFORMS\
	"	uniform vec3 u_fogColor;\n"\
	"	uniform int u_fogToBlack;\n"\
	"	uniform float u_fogStrength;\n"\
	"	uniform int u_voidProbe;\n"\
	"	uniform int u_flashlightOn;\n"\
	"	uniform int u_untextured;\n"\
	"	uniform int u_flStyle;\n"\
	"	uniform vec3 u_flLightPos;\n"\
	"	uniform vec3 u_flDir;\n"\
	"	uniform vec3 u_flColor;\n"\
	"	uniform float u_flInnerCos;\n"\
	"	uniform float u_flOuterCos;\n"\
	"	uniform float u_flRange;\n"\
	"	uniform int u_shadowOn;\n"\
	"	uniform sampler2D u_shadowTex;\n"\
	"	uniform mat4 u_shadowMatrix;\n"\
	"	uniform float u_shadowBias;\n"\
	"	uniform vec2 u_shadowTexel;\n"\
	"	uniform float u_shadowNormalOffset;\n"\
	"	uniform float u_shadowStrength;\n"\
	"	uniform vec2 u_shadowClip;\n"\
	"	uniform float u_shadowFadeDist;\n"\
	/* Window depth [0,1] -> linear distance along the light's forward axis, using the shadow frustum's near/far (u_shadowClip). Lets us measure how far a receiver sits BEHIND its occluder in world units. */\
	"	float shLinDepth(float zw) {\n"\
	"		float ndc = zw * 2.0 - 1.0;\n"\
	"		return (2.0 * u_shadowClip.x * u_shadowClip.y) / (u_shadowClip.y + u_shadowClip.x - ndc * (u_shadowClip.y - u_shadowClip.x));\n"\
	"	}\n"

/* The lit fragment tail: vertex-color modulate, per-pixel flashlight +
 * shadow, then fog - dither/quantize follows as the very last op. Shared so
 * override-drawn geometry matches the VRAM samplers exactly (missing fog on
 * override surfaces was visible as unfogged distant walls). */
#define GPU_LIT_TAIL\
	"		vec3 flAlbedo = fragColor.rgb;\n"\
	"		fragColor *= v_color;\n"\
	/* Untextured prims sample the white placeholder, so their texture "albedo"
	 * is 1.0 — the beam would add full-white light onto geometry whose real
	 * color is the (often dark) vertex color, and stacked additive layers then
	 * saturate to a white blob (sewer water octagon under PGXP, where the
	 * perspective-correct v_viewpos also resolves grazing pixels much closer
	 * to the light). Use the vertex-lit color as the albedo instead — bounded
	 * by what the surface actually looks like. Textured splits (u_untextured=0)
	 * are byte-identical. */\
	"		if (u_untextured > 0) flAlbedo = fragColor.rgb;\n"\
	/* Two flashlight styles, chosen by the UNIFORM u_flStyle (uniform control flow, so derivative use inside the branch is well-defined). 1 = CLASSIC: PSX-calibrated orientation-independent overlay -- no face normals, func_80057658-derived falloff, eased wide cone, 0.49 base dim. 0 = MODERN: stylized spotlight -- per-fragment Lambert from a dFdx/dFdy-reconstructed face normal, linear falloff, hard 0.15 dark surround. */\
	"		if (u_flashlightOn > 0) {\n"\
	"			vec3 flP = v_viewpos;\n"\
	"			if (flP.z > 0.0) {\n"\
	"				fragColor.rgb *= (u_flStyle > 0) ? 0.49 : 0.15;\n"\
	/* normalize() of a zero vector is 0/0 = NaN, and a NaN here propagates through
	 * dot/smoothstep into the += below, wiping the cone while the dim above has
	 * already landed -- a dark scene with no beam. Desktop drivers commonly flush
	 * that to zero; ANGLE -> Vulkan does not, which is why the same build lights
	 * correctly on one backend and not the other. Guard the degenerate case. */\
	"				vec3 flDirRaw = u_flDir;\n"\
	"				vec3 flDir = (dot(flDirRaw, flDirRaw) > 1e-12) ? normalize(flDirRaw) : vec3(0.0, 0.0, 1.0);\n"\
	"				vec3 flOrigin = (u_flStyle > 0) ? (u_flLightPos - flDir * 39.0) : u_flLightPos;\n"\
	"				vec3 L = flOrigin - flP;\n"\
	"				float d = length(L);\n"\
	"				L /= max(d, 0.0001);\n"\
	"				float cone  = smoothstep(u_flOuterCos, u_flInnerCos, dot(-L, flDir));\n"\
	"				float ndl = 1.0;\n"\
	"				float atten;\n"\
	"				if (u_flStyle > 0) {\n"\
	"					cone = cone * (2.0 - cone);\n"\
	/* Classic center-beam distance envelope derived from SH1's func_80057658 at full Q12 flashlight strength: its GTE projection reduces to a capped 1/d term plus a thresholded 1/d^2 term, normalized by the room-light cap. */\
	"					float attenD = d * 2.0;\n"\
	"					float invD = 1.0 / max(attenD, 1.0);\n"\
	"					atten = max(0.0, 134217728.0 * invD * invD - 16.0);\n"\
	"					atten += min(48.0, 32768.0 * invD);\n"\
	"					atten = clamp(atten / 255.0, 0.0, 1.0);\n"\
	"					atten *= 1.0 - smoothstep(u_flRange * 0.9, u_flRange, attenD);\n"\
	"				} else {\n"\
	/* Modern: GrVertex carries no usable normals, so the face normal is reconstructed from the view-space position gradient -- exact per triangle face, no GTE-side capture needed. The N.L term gives surfaces 3D shape under the beam. */\
	"					vec3 flN = cross(dFdx(flP), dFdy(flP));\n"\
	"					float nlen = length(flN);\n"\
	"					vec3 N = (nlen > 1e-9) ? flN / nlen : vec3(0.0, 0.0, -1.0);\n"\
	"					if (dot(N, flP) > 0.0) N = -N;\n"\
	"					ndl = 0.15 + 0.85 * max(dot(N, L), 0.0);\n"\
	"					atten = clamp(1.0 - d / u_flRange, 0.0, 1.0);\n"\
	"				}\n"\
	/* Bilinearly interpolated 3x3 PCF avoids kernel jumps as the projected receiver crosses shadow texels. */\
	"				float shadow = 1.0;\n"\
	"				if (u_shadowOn > 0) {\n"\
	/* Perspective-correct shadow receiver (PR#8): correctness for BOTH styles -- same shadow shapes, just stable under motion. */\
	"					vec3 flShadowP = v_shadowViewPos.xyz / max(v_shadowViewPos.w, 1e-9);\n"\
	"					vec3 flPs = flShadowP + L * (u_shadowNormalOffset * d);\n"\
	"					vec4 lp = u_shadowMatrix * vec4(flPs, 1.0);\n"\
	"					if (lp.w > 0.0) {\n"\
	"						vec3 luv = lp.xyz / lp.w * 0.5 + 0.5;\n"\
	"						if (luv.x > 0.0 && luv.x < 1.0 && luv.y > 0.0 && luv.y < 1.0 && luv.z < 1.0) {\n"\
	"							vec3 luvDx = dFdx(luv);\n"\
	"							vec3 luvDy = dFdy(luv);\n"\
	"							float det = luvDx.x * luvDy.y - luvDx.y * luvDy.x;\n"\
	"							float dzdu = 0.0;\n"\
	"							float dzdv = 0.0;\n"\
	"							if (abs(det) > 1e-9) {\n"\
	"								dzdu = (luvDx.z * luvDy.y - luvDy.z * luvDx.y) / det;\n"\
	"								dzdv = (luvDy.z * luvDx.x - luvDx.z * luvDy.x) / det;\n"\
	"							}\n"\
	"							float recvLin = (u_shadowFadeDist > 0.0) ? shLinDepth(luv.z) : 0.0;\n"\
	"							float occ = 0.0;\n"\
	"							vec2 texelPos = luv.xy / u_shadowTexel - vec2(0.5);\n"\
	"							vec2 texelBase = floor(texelPos);\n"\
	"							vec2 texelFrac = fract(texelPos);\n"\
	"							for (int sy = -1; sy <= 2; sy++) {\n"\
	"								float wy = (sy == -1) ? (1.0 - texelFrac.y) : ((sy == 2) ? texelFrac.y : 1.0);\n"\
	"								for (int sx = -1; sx <= 2; sx++) {\n"\
	"									float wx = (sx == -1) ? (1.0 - texelFrac.x) : ((sx == 2) ? texelFrac.x : 1.0);\n"\
	"									float weight = wx * wy;\n"\
	"									vec2 suv = (texelBase + vec2(float(sx), float(sy)) + vec2(0.5)) * u_shadowTexel;\n"\
	"									float sd = texture2D(u_shadowTex, suv).r;\n"\
	"									float receiverDepth = luv.z + dzdu * (suv.x - luv.x) + dzdv * (suv.y - luv.y);\n"\
	"									if (receiverDepth - u_shadowBias > sd) {\n"\
	"										if (u_shadowFadeDist > 0.0) {\n"\
	"											float gap = recvLin - shLinDepth(sd);\n"\
	"											occ += weight * (1.0 - clamp(gap / u_shadowFadeDist, 0.0, 1.0));\n"\
	"										} else {\n"\
	"											occ += weight;\n"\
	"										}\n"\
	"									}\n"\
	"								}\n"\
	"							}\n"\
	"							shadow = 1.0 - u_shadowStrength * (occ / 9.0);\n"\
	"						}\n"\
	"					}\n"\
	"				}\n"\
	"				vec3 fl = u_flColor * (cone * atten * ndl * shadow);\n"\
	"				fragColor.rgb += flAlbedo * fl * (1.0 - clamp(v_fade, 0.0, 1.0));\n"\
	"			}\n"\
	"		}\n"\
	"		float fogAmt = clamp(v_fogAmount * u_fogStrength, 0.0, 1.0);\n"\
	/* Plain PSX fog: the ramp reaches full fog at the map's fog distance and the
	 * world is culled on its nearest vertex past it, so nothing drawn sits short
	 * of full fog against the void. An easing curve was tried here and removed: it
	 * pushed everything past ~70% of the fog distance to 99%+, a metres-deep band
	 * where every building and post sat within one unit of the void as a ghost. */\
	"		if (u_fogToBlack > 0)\n"\
	"			fragColor.rgb *= (1.0 - fogAmt);\n"\
	"		else\n"\
	"			fragColor.rgb = mix(fragColor.rgb, u_fogColor, fogAmt);\n"\
	/* VOIDPROBE second frame: every fog-tail fragment is written as (fogAmt, is3d,
	 * 200) so the histogram shows which fog levels the off-colour pixels carry, and
	 * anything still at its real colour is proven to come from outside this tail. */\
	"		if (u_voidProbe > 0) fragColor = vec4(fogAmt, v_is3d, 200.0 / 255.0, 1.0);\n"

#define GPU_FRAGMENT_SAMPLE_SHADER(bit) \
	GPU_PACK_RG_FUNC\
	GPU_DECODE_RG_FUNC\
	GPU_FETCH_VRAM_FUNC\
	"	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"\
	GPU_ARRAY_FUNC\
	GPU_SAMPLE_TEXTURE_## bit ##BIT_FUNC\
	GPU_BILINEAR_SAMPLE_FUNC\
	GPU_NEAREST_SAMPLE_FUNC\
	"	uniform int bilinearFilter;\n"\
	"	uniform float u_ditherForce;\n"\
	"	uniform float u_pixelScale;\n"\
	GPU_LIT_UNIFORMS\
	"	void main() {\n"\
	"		g_tapCov = 1.0;\n"\
	"		g_tapWt = 1.0;\n"\
	/* bilinearFilter is the GATE (0 = off, 1 = 3D geometry only, 2 = this whole
	 * frame); u_anisoTaps chooses HOW to filter once it passes. Keeping the two
	 * separate is why anisotropic needs no new gate logic. */\
	"		if((bilinearFilter == 1 && v_geom3d > 0.5) || bilinearFilter >= 2)\n"\
	"			fragColor = (u_anisoTaps > 1.0) ? AnisoTextureSample(v_texcoord.xy)\n"\
	"			                                : BilinearTextureSample(v_texcoord.xy);\n"\
	"		else\n"\
	"			fragColor = NearestTextureSample(v_texcoord.xy);\n"\
	/* Untextured prims bind a 1x1 white placeholder, which decodes through the
	 * 5-bit table like any VRAM texel: 0xFFFF -> 248/256, so every flat or
	 * gouraud prim drew at 31/32 of its vertex colour. PSX draws them at the
	 * vertex colour exactly. The visible case was the full-screen fog-colour
	 * quad the game lays under the world: at 31/32, averaged with the clear, the
	 * void came out (107,99,114) against fully fogged geometry at the exact fog
	 * colour (108,100,116), so every building, tree and post stood out from it.
	 * Alpha is kept, since it carries the semi-transparency. */\
	"		if (u_untextured > 0) fragColor.rgb = vec3(1.0);\n"\
	/* Additive and subtractive prims fade their edges by texture coverage.
	 * TapWeighted divides the filtered colour by the OPAQUE tap weight, so a
	 * fragment with one opaque tap out of four comes out at full brightness and
	 * the sprite keeps a hard edge however far it is magnified -- correct for a
	 * character or a font, where a soft edge would be a dark fringe, but wrong
	 * for light being added: half-covered should add half as much. That hard cut
	 * is what makes a 2x2-texel snow flake a white square on a PC screen instead
	 * of the round dot the same two pixels were on a TV. Colour only; alpha
	 * keeps its meaning for BM_AVERAGE and for the cutout discard. */\
	"		if (u_fogToBlack > 0 && g_tapWt > 0.0)\n"\
	"			fragColor.rgb *= clamp(g_tapCov / g_tapWt, 0.0, 1.0);\n"\
	GPU_LIT_TAIL\
	GPU_DITHERING_NO_VCOLOR\
	"	}\n"

const char* gte_shader_4 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"varying float v_fade;\n"
	"varying float v_geom3d;\n"
	"varying vec4 v_uvlim;\n"
	"varying vec4 v_shadowViewPos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(4)
	"#endif\n";

const char* gte_shader_8 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"varying float v_fade;\n"
	"varying float v_geom3d;\n"
	"varying vec4 v_uvlim;\n"
	"varying vec4 v_shadowViewPos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(8)
	"#endif\n";

const char* gte_shader_16 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"varying float v_fade;\n"
	"varying float v_geom3d;\n"
	"varying vec4 v_uvlim;\n"
	"varying vec4 v_shadowViewPos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(16)
	"#endif\n";

const char* gte_shader_32_rgba =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"varying float v_fade;\n"
	"varying float v_geom3d;\n"
	"varying vec4 v_uvlim;\n"
	"varying vec4 v_shadowViewPos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	"	uniform sampler2D s_texture;\n"\
	"	uniform int bilinearFilter;\n"\
	"	uniform float u_ditherForce;\n"\
	"	uniform float u_pixelScale;\n"\
	"	uniform vec2 texelSize;\n"\
	"	uniform vec2 u_texOffset;\n"\
	"	uniform vec2 u_hiresHalf;\n"\
	GPU_LIT_UNIFORMS\
	"	void main() {\n"\
	/* u_texOffset: the prim's tpage origin relative to the replaced TIM's
	 * VRAM origin, in native texels. A surface wider than one tpage is drawn
	 * as several prims whose UVs each restart at their own tpage — without
	 * the offset every chunk sampled the override from x=0 (duplicated image).
	 *
	 * u_hiresHalf: half a HIRES texel in native-texel units (0.5*native/hires
	 * per axis). The old "+ 0.5 native texel" shift pushed edge fragments up
	 * to half a texel into the NEIGHBORING atlas cell (white marks hugging
	 * every font glyph / the cursor sprite with texture packs); clamping the
	 * fractional part instead keeps the LINEAR footprint inside the fragment's
	 * own native texel — full hires detail within the cell, zero cross-cell
	 * bleed at cell edges. (0,0) = free linear, no clamp (no-override path). */
	"		vec2 uvn = v_texcoord.xy + u_texOffset;\n"\
	"		vec2 cell = floor(uvn);\n"\
	"		vec2 tc = (cell + clamp(uvn - cell, u_hiresHalf, vec2(1.0) - u_hiresHalf)) * texelSize;\n"\
	/* Filtering off for this prim class (0 = whole 2D/menu frame, 1 = 3D frame but
	 * this prim is 2D): point-sample the replacement at its OWN resolution. The
	 * texture object is LINEAR(+mips) for every upscaled replacement and that is
	 * fixed at upload time, so the snap has to happen here; textureLod pins the base
	 * level, or a minified HD atlas would still blur in through the mip chain. A
	 * native-res replacement already lands on the texel centre (u_hiresHalf == 0.5),
	 * so it comes out bit-identical either way. */
	"		if (bilinearFilter == 0 || (bilinearFilter == 1 && v_geom3d < 0.5)) {\n"\
	"			vec2 hiresSize = vec2(textureSize(s_texture, 0));\n"\
	"			fragColor = textureLod(s_texture, (floor(tc * hiresSize) + 0.5) / hiresSize, 0.0);\n"\
	"		} else {\n"\
	"			fragColor = texture2D(s_texture, tc);\n"\
	"		}\n"\
	/* PSX colour-0 transparency for hi-res overrides: alpha 0 texels are
	 * holes on ANY prim (opaque prims ignore blending, so without the
	 * discard they'd render solid). 0.5 cutoff keeps authored soft-alpha
	 * edges blending on semi-transparent prims while opaque cutouts
	 * (foliage/UI) stay clean. */
	"		if (fragColor.a < 0.5) discard;\n"\
	/* Per-prim alpha fade (v_color.a, default 1.0 -- see
	 * PsyX_SetNextPrimAlpha). BM_AVERAGE is genuine SRC_ALPHA blending on
	 * PC, so scaling the surviving texels' alpha is a true transparency
	 * fade: the fog-faded bullet decals dissolve instead of darkening.
	 * Applied after the discard so the cutout shape is unchanged. */\
	"		fragColor.a *= v_color.a;\n"\
	GPU_LIT_TAIL\
	GPU_DITHERING_NO_VCOLOR\
	"	}\n"
	"#endif\n";

int GR_Shader_CheckShaderStatus(GLuint shader)
{
	char info[1024];
	GLint result;

	glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

	if (result == GL_TRUE)
		return 1;
	
	glGetShaderInfoLog(shader, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

int GR_Shader_CheckProgramStatus(GLuint program)
{
	char info[1024];
	GLint result;

	glGetProgramiv(program, GL_LINK_STATUS, &result);

	if (result == GL_TRUE)
		return 1;

	glGetProgramInfoLog(program, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

ShaderID GR_Shader_Compile(const char* source)
{
#if defined(ES2_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 100\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define VERTEX\n";

	const char* GLSL_HEADER_FRAG =
		"#version 100\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define fragColor gl_FragColor\n";
#elif defined(ES3_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 300 es\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	const char* GLSL_HEADER_FRAG =
		"#version 300 es\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";
#else
	/* The desktop build can be running either a desktop GL context or an ES 3.0
	 * one (ANGLE), so the #version line is a runtime choice. Everything below it
	 * is identical — the shader body is already written in the shared dialect
	 * that these defines paper over. */
	static const char* const GLSL_HEADER_VERT_GL =
		"#version 140\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	static const char* const GLSL_HEADER_FRAG_GL =
		"#version 140\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";

	/* GLSL ES 3.00 requires the #extension line to precede any other statement,
	 * so noperspective support is baked into the header rather than the trailing
	 * defines block. Requested only when the probe found the extension. */
	/* highp int, NOT lowp. GLSL ES only guarantees ~9 bits for lowp int
	 * (about +-256), and this renderer does integer work far past that:
	 * VRAM texel coordinates in a 1024x512 space, CLUT palette indices and
	 * tpage offsets. Overflowing those sends texture lookups to the wrong
	 * place, which is the whole-scene texture distortion and the wrong
	 * semitransparency patches reported on the Vulkan and D3D11 backends.
	 *
	 * Desktop GL IGNORES precision qualifiers, so this could only ever show
	 * on the ES path -- which is exactly what those backends are, since they
	 * reach the GPU through ANGLE. That is why plain GL was always fine. */
	static const char* const GLSL_HEADER_VERT_ES3 =
		"#version 300 es\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	static const char* const GLSL_HEADER_VERT_ES3_NP =
		"#version 300 es\n"
		"#extension GL_NV_shader_noperspective_interpolation : require\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	static const char* const GLSL_HEADER_FRAG_ES3 =
		"#version 300 es\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";

	static const char* const GLSL_HEADER_FRAG_ES3_NP =
		"#version 300 es\n"
		"#extension GL_NV_shader_noperspective_interpolation : require\n"
		"precision highp int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";

	const char* GLSL_HEADER_VERT;
	const char* GLSL_HEADER_FRAG;

	if (g_grIsGLES)
	{
		const int np = (g_grCaps.noperspective && g_cfg_affineTextures);
		GLSL_HEADER_VERT = np ? GLSL_HEADER_VERT_ES3_NP : GLSL_HEADER_VERT_ES3;
		GLSL_HEADER_FRAG = np ? GLSL_HEADER_FRAG_ES3_NP : GLSL_HEADER_FRAG_ES3;
	}
	else
	{
		GLSL_HEADER_VERT = GLSL_HEADER_VERT_GL;
		GLSL_HEADER_FRAG = GLSL_HEADER_FRAG_GL;
	}
#endif

	char extra_vs_defines[1024];
	char extra_fs_defines[1024];
	extra_vs_defines[0] = 0;
	extra_fs_defines[0] = 0;


	/* Affine (non-perspective-correct) texture mapping — matches PSX GPU behaviour.
	 * Uses noperspective interpolation qualifier (GLSL 1.30+, desktop only).
	 * centroid keeps UV interpolation inside the primitive under MSAA: edge
	 * samples otherwise extrapolate texcoords into the neighboring VRAM-atlas
	 * cell (the "weird lines"/light-texture artifacts, worst at 8x). Identical
	 * to center sampling when MSAA is off, so the non-MSAA image is unchanged. */
#if defined(ES2_SHADERS)
	#define SH_TC_CENTROID ""
#else
	#define SH_TC_CENTROID "centroid "
#endif
	/* `noperspective` is core in desktop GLSL but reaches GLES only through
	 * NV_shader_noperspective_interpolation, so whether it can be emitted is a
	 * property of the live context, not of the build. Emitting it unguarded on
	 * an ES context fails shader compilation outright rather than merely losing
	 * the affine look — which is why this is probed, not assumed.
	 *
	 * Losing it costs less than it appears: the legacy path leaves
	 * gl_Position.w == 1, where perspective-correct interpolation IS screen-
	 * linear. It only changes anything on PGXP-projected geometry, whose whole
	 * purpose is to be perspective-correct in the first place. */
	const char* tcNoPerspective = "";
	if (g_cfg_affineTextures)
	{
#if defined(RENDERER_OGLES)
		tcNoPerspective = "";
#else
		tcNoPerspective = g_grCaps.noperspective ? "noperspective " : "";
#endif
	}

	{
		char affineDef[128];
		snprintf(affineDef, sizeof(affineDef), "#define AFFINE_VARYING %s%svarying\n",
			tcNoPerspective, SH_TC_CENTROID);
		strcat(extra_vs_defines, affineDef);
		strcat(extra_fs_defines, affineDef);
	}

	const char* vs_list[] = { GLSL_HEADER_VERT, extra_vs_defines, source };
	const char* fs_list[] = { GLSL_HEADER_FRAG, extra_fs_defines, source };

	GLuint program = glCreateProgram();

	{
		GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(vertexShader, 3, vs_list, NULL);
		glCompileShader(vertexShader);

		if( GR_Shader_CheckShaderStatus(vertexShader) == 0 )
			eprinterr("Failed to compile Vertex Shader!\n");
	
		glAttachShader(program, vertexShader);
		glDeleteShader(vertexShader);
	}

	{
		GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(fragmentShader, 3, fs_list, NULL);
		glCompileShader(fragmentShader);

		if(GR_Shader_CheckShaderStatus(fragmentShader) == 0)
			eprinterr("Failed to compile Fragment Shader!\n");
	
		glAttachShader(program, fragmentShader);
		glDeleteShader(fragmentShader);
	}

	glBindAttribLocation(program, a_position, "a_position");
	glBindAttribLocation(program, a_zw, "a_zw");
	glBindAttribLocation(program, a_pgxp, "a_pgxp");
	glBindAttribLocation(program, a_texcoord, "a_texcoord");
	glBindAttribLocation(program, a_color, "a_color");
	glBindAttribLocation(program, a_extra, "a_extra");
	glBindAttribLocation(program, a_normal, "a_normal");
	glBindAttribLocation(program, a_viewpos, "a_viewpos");
	glBindAttribLocation(program, a_geom3d, "a_geom3d");
	glBindAttribLocation(program, a_uvlim, "a_uvlim");

	glLinkProgram(program);
	if(GR_Shader_CheckProgramStatus(program) == 0)
		eprinterr("Failed to link Shader!\n");

	GLint sampler = 0;
	GLint lutSampler = 2;
	glUseProgram(program);
	glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
	/* Shaders without the decode table return location -1, where this is a no-op. */
	glUniform1iv(glGetUniformLocation(program, "s_rgLut"), 1, &lutSampler);
	glUseProgram(0);

	return program;
}
#else
#error
#endif

//--------------------------------------------------------------------------------------------

static u_char s_rgLUT[LUT_WIDTH * LUT_HEIGHT * 4];

/* Bake the 16-bit-PSX -> RGBA table once with exact integer bit math, so the
 * shader can decode a texel by indexing it (lut()) instead of dividing and
 * flooring floats per pixel. Channels are stored as v<<3 rather than v so the
 * shader's v*8/256 lands on exactly v/32, matching the old decode bit-for-bit.
 * Ported from OpenDriver2/PsyCross GR_InitRG8LUT. */
void GR_InitRG8LUT()
{
	for (int y = 0; y < LUT_HEIGHT; y++)
	{
		u_char* row = s_rgLUT + y * (LUT_WIDTH * 4);
		for (int x = 0; x < LUT_WIDTH; x++)
		{
			const unsigned short c = (unsigned short)((y << 8) | x);
			u_char* pixel = row + x * 4;
			pixel[0] = (u_char)(((c)       & 31) << 3);
			pixel[1] = (u_char)(((c >>  5) & 31) << 3);
			pixel[2] = (u_char)(((c >> 10) & 31) << 3);
			pixel[3] = (u_char)(((c >> 15) &  1) << 7); // STP
		}
	}
}

void GR_GenerateCommonTextures()
{
	unsigned int pixelData = 0xFFFFFFFF;

#if USE_OPENGL
	glGenTextures(1, &g_whiteTexture);
		GR_MarkTextureNearest(g_whiteTexture);
	{
		glBindTexture(GL_TEXTURE_2D, g_whiteTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixelData);

		glBindTexture(GL_TEXTURE_2D, 0);
	}

	/* Unit 0 is the scene texture and unit 1 the shadow map, so the decode
	 * table takes unit 2. It is immutable, uploaded once here, and NEAREST —
	 * lut() indexes texel centres and any filtering would blend palette
	 * entries together. */
	GR_InitRG8LUT();
	glGenTextures(1, &g_rgLutTexture);
	{
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LUT_WIDTH, LUT_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_rgLUT);

		glActiveTexture(GL_TEXTURE0);
	}
#endif
}

TextureID GR_CreateRGBATexture(int width, int height, u_char* data /*= nullptr*/)
{
	TextureID newTexture;
	glGenTextures(1, &newTexture);

	glBindTexture(GL_TEXTURE_2D, newTexture);
	/* Neutral default only. The real filter is chosen per BIND by
	 * GR_ApplyTextureFilter -- deciding it here is what made the setting
	 * unchangeable at runtime for every texture that already existed. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	
	// another WebGL stuff. Texture will be black without clamp to edge
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_2D, 0);

	return newTexture;
}

static void GR_InitialisePSXShader(GTEShader* sh, ShaderID shader)
{
	sh->shader = shader;

#if USE_OPENGL
	
	sh->bilinearFilterLoc = glGetUniformLocation(sh->shader, "bilinearFilter");
	sh->anisoTapsLoc = glGetUniformLocation(sh->shader, "u_anisoTaps");
	sh->ditherForceLoc = glGetUniformLocation(sh->shader, "u_ditherForce");
	sh->pixelScaleLoc = glGetUniformLocation(sh->shader, "u_pixelScale");
	sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
	sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
	sh->texOffsetLoc = glGetUniformLocation(sh->shader, "u_texOffset");
	sh->hiresHalfLoc = glGetUniformLocation(sh->shader, "u_hiresHalf");
	sh->fogColorLoc = glGetUniformLocation(sh->shader, "u_fogColor");
	sh->voidProbeLoc = glGetUniformLocation(sh->shader, "u_voidProbe");
	sh->fogToBlackLoc = glGetUniformLocation(sh->shader, "u_fogToBlack");
	sh->fogStrengthLoc = glGetUniformLocation(sh->shader, "u_fogStrength");
	sh->pgxpEnabledLoc = glGetUniformLocation(sh->shader, "u_pgxpEnabled");
	sh->szMaxLoc = glGetUniformLocation(sh->shader, "u_szMax");
	sh->worldFarBiasLoc = glGetUniformLocation(sh->shader, "u_worldFarBias");
	sh->pgxpFarWLoc = glGetUniformLocation(sh->shader, "u_pgxpFarW");
	sh->flashlightOnLoc = glGetUniformLocation(sh->shader, "u_flashlightOn");
	sh->untexturedLoc = glGetUniformLocation(sh->shader, "u_untextured");
	sh->flStyleLoc = glGetUniformLocation(sh->shader, "u_flStyle");
	sh->flLightPosLoc = glGetUniformLocation(sh->shader, "u_flLightPos");
	sh->flDirLoc = glGetUniformLocation(sh->shader, "u_flDir");
	sh->flColorLoc = glGetUniformLocation(sh->shader, "u_flColor");
	sh->flInnerCosLoc = glGetUniformLocation(sh->shader, "u_flInnerCos");
	sh->flOuterCosLoc = glGetUniformLocation(sh->shader, "u_flOuterCos");
	sh->flRangeLoc = glGetUniformLocation(sh->shader, "u_flRange");

	/* A uniform that resolves to -1 was dropped by the compiler, which on a
	 * translated backend (ANGLE -> D3D11/Vulkan) is how the per-pixel flashlight
	 * silently does nothing: the uniforms are set, but into a program that has no
	 * such uniform. Report the resolution once per shader so one log from an
	 * affected machine settles whether the branch survived compilation. */
	{
		static int s_flLocLogs = 0;

		if (s_flLocLogs < 8)
		{
			s_flLocLogs++;
			eprintf("*[FLLOC] gles=%d on=%d style=%d pos=%d dir=%d col=%d inner=%d outer=%d range=%d shadowOn=%d\n",
			        g_grIsGLES, sh->flashlightOnLoc, sh->flStyleLoc, sh->flLightPosLoc,
			        sh->flDirLoc, sh->flColorLoc, sh->flInnerCosLoc, sh->flOuterCosLoc,
			        sh->flRangeLoc, sh->shadowOnLoc);
		}
	}
	sh->shadowOnLoc = glGetUniformLocation(sh->shader, "u_shadowOn");
	sh->shadowMatrixLoc = glGetUniformLocation(sh->shader, "u_shadowMatrix");
	sh->shadowBiasLoc = glGetUniformLocation(sh->shader, "u_shadowBias");
	sh->shadowTexelLoc = glGetUniformLocation(sh->shader, "u_shadowTexel");
	sh->shadowNormalOffsetLoc = glGetUniformLocation(sh->shader, "u_shadowNormalOffset");
	sh->shadowStrengthLoc = glGetUniformLocation(sh->shader, "u_shadowStrength");
	sh->shadowClipLoc = glGetUniformLocation(sh->shader, "u_shadowClip");
	sh->shadowFadeDistLoc = glGetUniformLocation(sh->shader, "u_shadowFadeDist");

	/* Shadow map lives on texture unit 1 (scene textures use unit 0). Bind the
	 * sampler once here; the depth texture is bound to GL_TEXTURE1 each frame. */
	{
		GLint prevProg = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
		GLint sloc = glGetUniformLocation(sh->shader, "u_shadowTex");
		if (sloc != -1)
		{
			glUseProgram(sh->shader);
			glUniform1i(sloc, 1);
			glUseProgram(prevProg);
		}
	}
#endif
}

void GR_CompilePSXShader(GTEShader* sh, const char* source)
{
	GR_InitialisePSXShader(sh, GR_Shader_Compile(source));
}

extern "C" void Pc_ModernShader_Initialise(const char*, const char*, const char*, const char*);
extern "C" ShaderID Pc_ModernShader_Get(TexFormat);

void GR_InitialisePSXShaders()
{
	GR_CompilePSXShader(&g_gte_shader_4, gte_shader_4);
	GR_CompilePSXShader(&g_gte_shader_8, gte_shader_8);
	GR_CompilePSXShader(&g_gte_shader_16, gte_shader_16);
	GR_CompilePSXShader(&g_gte_shader_32_rgba, gte_shader_32_rgba);
	Pc_ModernShader_Initialise(gte_shader_4, gte_shader_8, gte_shader_16, gte_shader_32_rgba);
	GR_InitialisePSXShader(&g_modern_shader_4, Pc_ModernShader_Get(TF_4_BIT));
	GR_InitialisePSXShader(&g_modern_shader_8, Pc_ModernShader_Get(TF_8_BIT));
	GR_InitialisePSXShader(&g_modern_shader_16, Pc_ModernShader_Get(TF_16_BIT));
	GR_InitialisePSXShader(&g_modern_shader_32_rgba, Pc_ModernShader_Get(TF_32_BIT_RGBA));
}

int GR_InitialisePSX()
{
	SDL_memset(vram, 0, VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
	GR_GenerateCommonTextures();
	GR_InitialisePSXShaders();

#if USE_OPENGL
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_STENCIL_TEST);
	glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);

	/* PC port: enable MSAA rasterisation when a multisample framebuffer was
	 * obtained. Core profiles default this on, but enable explicitly + report
	 * the sample count the driver actually granted (may differ from requested). */
	if (g_cfg_msaaSamples > 0)
	{
		glEnable(GL_MULTISAMPLE);
		int actualSamples = 0;
		SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &actualSamples);
		eprintf("*MSAA: requested %dx, got %dx\n", g_cfg_msaaSamples, actualSamples);
		if (actualSamples <= 1)
			g_cfg_msaaSamples = 0; /* driver gave us a single-sample buffer after all */
	}

	// gen framebuffer
	{
		memset(&g_glFramebufferPBO, 0, sizeof(g_glFramebufferPBO));
		PBO_Init(&g_glFramebufferPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// make a special texture
		// it will be resized later
		glGenTextures(1, &g_fbTexture);
		GR_MarkTextureNearest(g_fbTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_fbTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glBlitFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
		}
	}

	// gen offscreen RT
	{
		memset(&g_glOffscreenPBO, 0, sizeof(g_glOffscreenPBO));
		PBO_Init(&g_glOffscreenPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// offscreen texture render target
		glGenTextures(1, &g_offscreenRTTexture);
		GR_MarkTextureNearest(g_offscreenRTTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glOffscreenFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_offscreenRTTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
		}
	}

	// gen VRAM textures.
	// double-buffered
	{
		int i;

		glGenTextures(2, g_vramTexturesDouble);
		GR_MarkTextureNearest(g_vramTexturesDouble[0]);
		GR_MarkTextureNearest(g_vramTexturesDouble[1]);

		for(i = 0; i < 2; i++)
		{
			glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// set storage size
			glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);
		}

		g_vramTexture = g_vramTexturesDouble[0];

		glBindTexture(GL_TEXTURE_2D, 0);

		// VRAM framebuffer for offscreen blitting to VRAM
		glGenFramebuffers(1, &g_glVRAMFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
		}
	}

	// gen vertex buffer and index buffer
	{
		int i;

		glGenBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);
		glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);

		for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++)
		{
			glBindVertexArray(g_glVertexArray[i]);

			glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[i]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE, NULL, GL_DYNAMIC_DRAW);
		}

		glBindVertexArray(0);
	}
#else
#error
#endif

	GR_InitPostProcess();

	GR_ResetDevice();

	return 1;
}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	float a = 2.0f / (right - left);
	float b = 2.0f / (top - bottom);
	float c = 2.0f / (znear - zfar);

	float x = (left + right) / (left - right);
	float y = (bottom + top) / (bottom - top);

#if USE_OPENGL 
	// -1..1
	float z = (znear + zfar) / (znear - zfar);
#endif

	float ortho[16] = {
		a, 0, 0, 0,
		0, b, 0, 0,
		0, 0, c, 0,
		x, y, z, 1
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
#endif
}

void GR_Perspective3D(const float fov, const float width, const float height, const float zNear, const float zFar)
{
	float sinF, cosF;
	sinF = sinf(0.5f * fov);
	cosF = cosf(0.5f * fov);

	float h = cosF / sinF;
	float w = (h * height) / width;

	float persp[16] = {
		w, 0, 0, 0,
		0, h, 0, 0,
		0, 0, (zFar + zNear) / (zFar - zNear), 1,
		0, 0, -(2 * zFar * zNear) / (zFar - zNear), 0
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projection3DLoc, 1, GL_FALSE, persp);
#endif
}

void GR_SetupClipMode(const RECT16* rect, int enable)
{
	// [A] isinterlaced dirty hack for widescreen
	const bool scissorOn = enable && (activeDispEnv.isinter ||
		(	rect->x - activeDispEnv.disp.x > 0 ||
			rect->y - activeDispEnv.disp.y > 0 ||
			rect->w < activeDispEnv.disp.w - 1 ||
			rect->h < activeDispEnv.disp.h - 1));

	GR_SetScissorState(scissorOn);

	if (!scissorOn)
		return;

	// Runtime gate: same as the ortho selection — non-PGXP path uses
	// emuScreenAspect=1 (no widescreen aspect remap) so the scissor
	// rectangle stays in pixel-space coordinates that match the legacy
	// 2D ortho. With PGXP at compile time but off at runtime, falling
	// through to the PGXP aspect calc would shrink the scissor and
	// cause clipping inconsistent with the prim ortho.
	const float emuScreenAspect = 1.0f;

	const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
	const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;

	// first map to 0..1
	float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
	float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
	float clipRectW = (float)(rect->w) * psxScreenWInv;
	float clipRectH = (float)(rect->h) * psxScreenHInv;

	// then map to screen
	{
		clipRectX -= 0.5f;

		clipRectX *= emuScreenAspect;
		clipRectW *= emuScreenAspect;

		clipRectX += 0.5f;
	}

#if USE_OPENGL
	// adjust scissor rectangle by the backbuffer size (window dimensions)
	const float flipOffset = g_windowHeight - clipRectH * (float)g_windowHeight;
	const float crx = clipRectX * (float)g_windowWidth;
	const float cry = clipRectY * (float)g_windowHeight;
	const float crw = clipRectW * (float)g_windowWidth;
	const float crh = clipRectH * (float)g_windowHeight;

	glScissor(crx, flipOffset - cry, crw, crh);
#endif
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect)
{

	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;
	rect->w = activeDispEnv.disp.w;
	rect->h = activeDispEnv.disp.h;
}

void GR_SetShader(const ShaderID shader)
{
	if (g_PreviousShader != shader)
	{
#if USE_OPENGL
		glUseProgram(shader);
#else
#error
#endif

		g_PreviousShader = shader;
	}
}


static void GR_SetTextureShader(TextureID texture, TexFormat texFormat, GTEShader* shader)
{
	switch (texFormat)
	{
	case TF_4_BIT:
		GR_SetShader(shader->shader);
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_anisoTapsLoc = shader->anisoTapsLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = shader->pixelScaleLoc;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = -1;
		u_texOffsetLoc = -1;
		u_hiresHalfLoc = -1;
		u_fogColorLoc = shader->fogColorLoc;
		u_voidProbeLoc = shader->voidProbeLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
		break;
	case TF_8_BIT:
		GR_SetShader(shader->shader);
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_anisoTapsLoc = shader->anisoTapsLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = shader->pixelScaleLoc;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = -1;
		u_texOffsetLoc = -1;
		u_hiresHalfLoc = -1;
		u_fogColorLoc = shader->fogColorLoc;
		u_voidProbeLoc = shader->voidProbeLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
		break;
	case TF_16_BIT:
		GR_SetShader(shader->shader);
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_anisoTapsLoc = shader->anisoTapsLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = shader->pixelScaleLoc;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = -1;
		u_texOffsetLoc = -1;
		u_hiresHalfLoc = -1;
		u_fogColorLoc = shader->fogColorLoc;
		u_voidProbeLoc = shader->voidProbeLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
		break;
	case TF_32_BIT_RGBA:
		GR_SetShader(shader->shader);
		/* Upstream 5b70144: the 32-bit RGBA (hi-res override) path now honours
		 * menu_filter, so this must be the shader's REAL bilinearFilter location
		 * rather than -1. Read it off the parameterised shader so the modern-mesh
		 * variants get their own location (or -1 if absent, which the guarded
		 * glUniform1i below tolerates). */
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_anisoTapsLoc = shader->anisoTapsLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = -1;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = shader->texelSizeLoc;
		u_texOffsetLoc = shader->texOffsetLoc;
		u_hiresHalfLoc = shader->hiresHalfLoc;
		u_fogColorLoc = shader->fogColorLoc;
		u_voidProbeLoc = shader->voidProbeLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
		break;
	}

	/* Push u_pgxpEnabled every shader bind so vertex shader's v_is3d
	 * fallback ((u_pgxpEnabled > 0) ? a_zw.y test : 1.0) correctly
	 * drops the 3D-only gate when PGXP is off at runtime. Without
	 * this fallback, every prim got a_zw.y=0 → v_is3d=0 → no dither
	 * and forced-nearest sampling on actual 3D geometry (visible as
	 * blocky tree leaves). */
	if (u_pgxpEnabledLoc != -1)
		glUniform1i(u_pgxpEnabledLoc, g_PsxUsePgxp);

	/* PGXP depth normalize: prev-frame max SZ. Lets the vertex shader turn each
	 * vertex's unquantized SZ3 into continuous NDC depth (Z-fight fix). */
	if (u_szMaxLoc != -1)
		glUniform1f(u_szMaxLoc, PGXP_GetSzMax());
	if (u_worldFarBiasLoc != -1)
		glUniform1f(u_worldFarBiasLoc, (float)g_PsxPgxpWorldFarBias);
	{
		extern float g_PgxpFarWClamp;
		if (u_pgxpFarWLoc != -1)
			glUniform1f(u_pgxpFarWLoc, g_PgxpFarWClamp);
	}

	if (u_fogColorLoc != -1)
		glUniform3fv(u_fogColorLoc, 1, g_PsyX_FogColor);
	if (u_voidProbeLoc != -1)
		glUniform1i(u_voidProbeLoc, g_PsxVoidProbeArmed == 2 ? 1 : 0);

	if (u_fogToBlackLoc != -1)
		glUniform1i(u_fogToBlackLoc, g_PsxFogToBlack);

	if (u_fogStrengthLoc != -1)
		glUniform1f(u_fogStrengthLoc, g_PsyX_FogStrength);

	/* Per-pixel flashlight cone. u_flashlightOn is 0 unless BOTH the master
	 * config flag and the per-frame game push are set, so the OFF path never
	 * touches the spotlight branch (and the vsz>0 gate also keeps it off). */
	if (u_flashlightOnLoc != -1)
		glUniform1i(u_flashlightOnLoc,
		            (g_PsyX_UsePerPixelFlashlight && g_PsyX_FlashlightActive) ? 1 : 0);
	/* Untextured splits bind g_whiteTexture; tell the lit tail so the beam's
	 * albedo falls back to the vertex-lit color instead of full white. Keyed on
	 * the caller's texture, not g_lastBoundTexture — this block runs before the
	 * bind early-out, so it stays correct across program switches. */
	if (u_untexturedLoc != -1)
		glUniform1i(u_untexturedLoc, texture == g_whiteTexture ? 1 : 0);
	if (u_flStyleLoc != -1)
		glUniform1i(u_flStyleLoc, g_PsyX_FlashlightStyle ? 1 : 0);
	if (u_flLightPosLoc != -1)
		glUniform3fv(u_flLightPosLoc, 1, g_PsyX_FlashlightPos);

	/* The cone collapses if any of these is degenerate -- a zero direction most of
	 * all. Reported on change so a backend that lights wrongly can be compared
	 * against one that does not, without a per-frame flood. */
	{
		static float s_lastDir[3] = { 9e9f, 9e9f, 9e9f };
		static int   s_flValLogs  = 0;

		if (g_PsyX_FlashlightActive && s_flValLogs < 24 &&
		    (g_PsyX_FlashlightDir[0] != s_lastDir[0] ||
		     g_PsyX_FlashlightDir[1] != s_lastDir[1] ||
		     g_PsyX_FlashlightDir[2] != s_lastDir[2]))
		{
			s_lastDir[0] = g_PsyX_FlashlightDir[0];
			s_lastDir[1] = g_PsyX_FlashlightDir[1];
			s_lastDir[2] = g_PsyX_FlashlightDir[2];
			s_flValLogs++;
			eprintf("*[FLVAL] pos=(%.1f,%.1f,%.1f) dir=(%.3f,%.3f,%.3f) range=%.1f style=%d\n",
			        g_PsyX_FlashlightPos[0], g_PsyX_FlashlightPos[1], g_PsyX_FlashlightPos[2],
			        g_PsyX_FlashlightDir[0], g_PsyX_FlashlightDir[1], g_PsyX_FlashlightDir[2],
			        g_PsyX_FlashlightRange, g_PsyX_FlashlightStyle);
		}
	}
	if (u_flDirLoc != -1)
		glUniform3fv(u_flDirLoc, 1, g_PsyX_FlashlightDir);
	/* FPS mode swaps in its own (tighter/dimmer) cone size + brightness. */
	float flIntensityActive = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightIntensityFps : g_PsyX_FlashlightIntensity;
	float flSizeActive      = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightSizeFps      : g_PsyX_FlashlightSize;
	if (u_flColorLoc != -1) {
		float flCol[3];
		flCol[0] = g_PsyX_FlashlightColor[0] * flIntensityActive;
		flCol[1] = g_PsyX_FlashlightColor[1] * flIntensityActive;
		flCol[2] = g_PsyX_FlashlightColor[2] * flIntensityActive;
		glUniform3fv(u_flColorLoc, 1, flCol);
	}
	/* Scale cone coverage by g_PsyX_FlashlightSize: a cone's solid angle is
	 * ~proportional to (1 - cos(halfAngle)), so scaling that term scales the lit
	 * area ~linearly (size 1.0 = base, 1.5 = ~1.5x). Inner stays the tighter
	 * (higher-cos) angle; the 0.05 floor keeps the half-angle under 90 deg. */
	{
		/* Modern style keeps its pre-calibration ~35 deg base cone; the wider
		 * 0.76 default belongs to the classic (PSX-matched) style. */
		float baseOuterCos = g_PsyX_FlashlightStyle ? g_PsyX_FlashlightOuterCos : 0.82f;
		float flInner = 1.0f - flSizeActive * (1.0f - g_PsyX_FlashlightInnerCos);
		float flOuter = 1.0f - flSizeActive * (1.0f - baseOuterCos);
		if (flInner < 0.05f) flInner = 0.05f;
		if (flOuter < 0.05f) flOuter = 0.05f;
		if (u_flInnerCosLoc != -1)
			glUniform1f(u_flInnerCosLoc, flInner);
		if (u_flOuterCosLoc != -1)
			glUniform1f(u_flOuterCosLoc, flOuter);
	}
	if (u_flRangeLoc != -1)
		glUniform1f(u_flRangeLoc, g_PsyX_FlashlightRange);

	/* Flashlight shadow map: same gate as the depth pre-pass in DrawAllSplits, so the
	 * shader only samples the shadow texture on frames one was actually rendered. */
	{
		/* Deliberately does NOT require g_PsyX_ShadowsAllowed, unlike the depth
		 * pre-pass. That flag exists because RENDERING the light-POV pass on a
		 * menu / room-fade / transition frame corrupts unrelated drawing --
		 * SAMPLING an already-built map does nothing of the sort. Requiring it
		 * here made every shadow vanish the moment the game paused or opened the
		 * map, then reappear on unpause. The world is frozen during those
		 * frames, so the map from the last gameplay frame is still exactly
		 * right; the pre-pass simply stops refreshing it. The cone's own gate
		 * (u_flashlightOn) still turns everything off where the flashlight
		 * itself is inactive. */
		int shadowOn = (g_PsyX_UseFlashlightShadows && g_PsyX_UsePerPixelFlashlight &&
		                g_PsyX_FlashlightActive && g_shadowDepthTex != 0 &&
		                !g_PsxPresentLastFrame) ? 1 : 0;
		/* shadowOn is a BINARY whole-scene lighting term: when it drops, every
		 * shadowed surface becomes lit and the frame pops brighter. A single
		 * frame of that reads exactly like the dim-gate flicker already fixed on
		 * the game side. Report each flip with its terms so one run names which
		 * one is unstable, rather than guessing between them. */
		{
			static int s_prevShadowOn = -1;
			static int s_shadowLogs   = 0;

			if (shadowOn != s_prevShadowOn && s_shadowLogs < 40)
			{
				s_shadowLogs++;
				eprintf("[SHADOWDIAG] shadowOn=%d useShadows=%d perPixel=%d flActive=%d tex=%d allowed=%d freeze=%d\n",
				        shadowOn, g_PsyX_UseFlashlightShadows, g_PsyX_UsePerPixelFlashlight,
				        g_PsyX_FlashlightActive, (g_shadowDepthTex != 0),
				        g_PsyX_ShadowsAllowed, g_PsxPresentLastFrame);
			}

			s_prevShadowOn = shadowOn;
		}

		if (u_shadowOnLoc != -1)
			glUniform1i(u_shadowOnLoc, shadowOn);
		if (shadowOn)
		{
			if (u_shadowMatrixLoc != -1)
				glUniformMatrix4fv(u_shadowMatrixLoc, 1, GL_FALSE, g_shadowLightMatrix);
			if (u_shadowBiasLoc != -1)
				glUniform1f(u_shadowBiasLoc, g_PsyX_FlashlightShadowBias);
			if (u_shadowNormalOffsetLoc != -1)
				glUniform1f(u_shadowNormalOffsetLoc, g_PsyX_FlashlightShadowNormalOffset);
			if (u_shadowStrengthLoc != -1)
				glUniform1f(u_shadowStrengthLoc, g_PsyX_FlashlightShadowStrength);
			if (u_shadowClipLoc != -1)
				glUniform2f(u_shadowClipLoc, g_shadowZNear, g_shadowZFar);
			if (u_shadowFadeDistLoc != -1)
				glUniform1f(u_shadowFadeDistLoc, g_PsyX_FlashlightShadowFadeDist);
			if (u_shadowTexelLoc != -1)
				glUniform2f(u_shadowTexelLoc, 1.0f / (float)PSYX_SHADOW_MAP_SIZE, 1.0f / (float)PSYX_SHADOW_MAP_SIZE);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, g_shadowDepthTex);
			glActiveTexture(GL_TEXTURE0);
		}
	}

	/* Re-assert the decode table on unit 2, like the shadow map above: the
	 * upload in GR_GenerateCommonTextures is a one-shot, and a pass that ever
	 * leaves another texture on that unit would otherwise decode every texel
	 * through it. Cheap next to the uniform pushes below, and it restores
	 * unit 0 so nothing downstream inherits a stray active unit. */
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);
	glActiveTexture(GL_TEXTURE0);

	/* Push the dither-force uniform every shader bind. Cheap (single
	 * float upload) and ensures runtime config changes (if we add a
	 * hotkey toggle later) take effect on the next primitive.
	 * g_PsxDitherSuppressed lets the game disable dither per-frame on
	 * 2D-only states (menus, logos, inventory) without changing the
	 * config flag. */
	if (u_ditherForceLoc != -1)
		glUniform1f(u_ditherForceLoc,
		            (g_cfg_psxDither && !g_PsxDitherSuppressed) ? 1.0f : 0.0f);

	/* Pixel scale = window width / PSX native (320). Scales the dither
	 * cell so each PSX-pixel-equivalent on screen gets its own matrix
	 * lookup, keeping the pattern visually proportional to PSX
	 * regardless of resolution. */
	if (u_pixelScaleLoc != -1) {
		float pixelScale = (g_windowWidth > 0)
			? ((float)g_windowWidth / 320.0f)
			: 1.0f;
		glUniform1f(u_pixelScaleLoc, pixelScale);
	}

	/* Filtering is a per-FRAME decision (the frame class flips with
	 * g_PsxDitherSuppressed), so push it on every bind like the two above —
	 * NOT below the g_lastBoundTexture early-out, where it used to sit. A menu
	 * entered from gameplay re-binds the same VRAM texture, so the early-out
	 * skipped the push and the whole screen kept running on the 3D value.
	 *
	 * VRAM samplers: 1 = 3D bilinear (v_is3d-gated), 2 = menu/2D-frame filter
	 * (ignores v_is3d, independent of psx_dither), 0 = point-sample.
	 *
	 * The hi-res override sampler decodes nothing: it reads s_texture through GL
	 * sampler state, which upload_rgba fixes at LINEAR(+mips) for any replacement
	 * larger than native and never revisits -- so neither toggle can reach it from
	 * the bind side, and the snap has to happen in the shader. It takes the same
	 * gate value as the VRAM samplers, so one setting now covers both classes. */
	/* menu_filter used to be restricted to TF_32_BIT_RGBA, i.e. to hi-res
	 * replacements only. Native paletted art is nearly all of the UI, so on a
	 * stock install the setting did nothing whatsoever -- which is the "UI
	 * option is broken completely, everything stays pixellated" report.
	 *
	 * The restriction was a workaround for glyph bleed: the kanji atlas packs
	 * 12px cells edge to edge, so a bilinear tap at a cell boundary pulled in
	 * the next glyph's first column and drew it as a vertical bar. Five days
	 * later the UV-limits clamp landed (TapWeighted above), which pins every tap
	 * inside the primitive's own vertex-UV box -- and a glyph IS its own rect
	 * prim with its own box, so a tap cannot reach the neighbouring cell any
	 * more. At the box edge both taps read the edge texel, which is the
	 * PSX-faithful colour. The workaround has been redundant since then, and
	 * only the breakage was left.
	 *
	 * texture_filter now also governs hi-res art on 3D frames instead of being
	 * ignored for it. It was pinned to 1 there so texture packs "kept sampling
	 * exactly as before", but that is precisely the complaint that the mode has
	 * no effect: with Filtering off, pack art stayed smooth while native art
	 * changed, so the two classes disagreed about what the setting meant. Off
	 * means off everywhere now. A native-resolution replacement comes out the
	 * same either way (see the hi-res sampler's u_hiresHalf note). */
	if (u_bilinearFilterLoc != -1)
		glUniform1i(u_bilinearFilterLoc,
		            g_PsxDitherSuppressed
		                ? (g_cfg_menuFilter ? 2 : 0)
		                : ((g_cfg_textureFilter > 0) ? 1 : 0));

	/* How to filter, once the gate above says whether to. Anisotropic is walked
	 * by hand for CLUT textures (see AnisoTextureSample); 1.0 means plain
	 * bilinear. The hardware path ignores this -- it got its anisotropy from
	 * GR_ApplyTextureFilter at bind time. */
	if (u_anisoTapsLoc != -1)
	{
		float taps = 1.0f;

		if (g_cfg_textureFilter >= 3 && texFormat != TF_32_BIT_RGBA)
		{
			taps = (float)g_cfg_anisoLevel;
			if (taps > 16.0f) taps = 16.0f;   /* matches the shader's loop bound */
			if (taps < 1.0f)  taps = 1.0f;
		}

		glUniform1f(u_anisoTapsLoc, taps);
	}

	if (g_dbg_texturelessMode) {
		texture = g_whiteTexture;
	}

	if (g_lastBoundTexture == texture) {
		return;
	}

#if USE_OPENGL
	glBindTexture(GL_TEXTURE_2D, texture);
	GR_ApplyTextureFilter(texture, texFormat);
#endif

	g_lastBoundTexture = texture;
}

void GR_SetTexture(TextureID texture, TexFormat texFormat)
{
	GTEShader* shader;
	if (texFormat == TF_4_BIT) shader = &g_gte_shader_4;
	else if (texFormat == TF_8_BIT) shader = &g_gte_shader_8;
	else if (texFormat == TF_16_BIT) shader = &g_gte_shader_16;
	else shader = &g_gte_shader_32_rgba;
	GR_SetTextureShader(texture, texFormat, shader);
}

void GR_SetOverrideTextureSize(int width, int height, int offsetX, int offsetY,
                               int hiresW, int hiresH)
{
	if(u_texelSizeLoc == -1)
		return;

	// WebGL is fucking around with glUniform2f, so use vector version.
	// width/height are a pool slot's nativeW/nativeH; a slot whose registration
	// aborted mid-way (an Intel-HD-4600 upload failure) leaves nativeW==0, so
	// 1.0/width == +Inf -> NaN texture coords. NaN sampling is implementation-
	// defined (Intel returns arbitrary texels: the half-screen garbage quad), so
	// clamp the divisor to >=1 to keep coords finite. Valid slots are unaffected.
	float vec[] = { 1.0f / (float)(width  > 0 ? width  : 1),
	                1.0f / (float)(height > 0 ? height : 1) };
	glUniform2fv(u_texelSizeLoc, 1, vec);

	if(u_texOffsetLoc != -1)
	{
		float ofs[] = { (float)offsetX, (float)offsetY };
		glUniform2fv(u_texOffsetLoc, 1, ofs);
	}

	if(u_hiresHalfLoc != -1)
	{
		/* Half a hires texel in native-texel units; the fragment shader clamps
		 * its LINEAR footprint inside each native texel with this. Capped at
		 * 0.5 (native-res replacement = pure texel-center sampling); 0 when the
		 * hires size is unknown (legacy DR_PSYX_TEX path) = no clamp. */
		float hx = (hiresW > 0 && width  > 0) ? 0.5f * (float)width  / (float)hiresW : 0.0f;
		float hy = (hiresH > 0 && height > 0) ? 0.5f * (float)height / (float)hiresH : 0.0f;
		float hh[2];
		hh[0] = (hx > 0.5f) ? 0.5f : hx;
		hh[1] = (hy > 0.5f) ? 0.5f : hy;
		glUniform2fv(u_hiresHalfLoc, 1, hh);
	}
}

void GR_DestroyTexture(TextureID texture)
{
	if (texture == -1)
		return;

#if USE_OPENGL
	glDeleteTextures(1, &texture);
#else
#error
#endif
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	vram_need_update = 1;

	u_short* dst = vram + x + y * VRAM_WIDTH;

	if (x + w > VRAM_WIDTH)
		w = VRAM_WIDTH - x;

	if (y + h > VRAM_HEIGHT)
		h = VRAM_HEIGHT - y;

	// clear VRAM region with given color
	for (int i = 0; i < h; i++)
	{
		u_short* tmp = dst;

		for (int j = 0; j < w; j++)
			*tmp++ = r | (g << 5) | (b << 11);

		dst += VRAM_WIDTH;
	}
}

/* [VOIDPROBE] one-shot readback, armed by the console command of that name.
 * Records the exact bytes the clear was asked for, then on the next frame reads
 * three rows across the top of BOTH the composed scene target and the window
 * after the present blit, and prints the most common colours on each. That
 * answers, in one run and without guesswork, whether the far void and fully
 * fogged geometry are landing on the same value and at which stage they
 * diverge. Fires once per arm; costs nothing otherwise. */
extern "C" { int g_PsxVoidProbeArmed = 0; unsigned char g_PsxLastClearRGB[3] = { 0, 0, 0 }; }

/* [GREYFRAME] -- the whole-screen grey flash: a presented frame that is exactly
 * the clear colour everywhere, while the world WAS submitted (the old
 * [GREYFLASH] vertex tally stayed normal through a recorded run of them, so the
 * geometry is drawn and lost, not missing). This watches the symptom itself:
 * three rows of the image actually presented are read back asynchronously each
 * swap, and a frame whose rows are one flat colour equal to its clear is
 * logged with the state it drew under. Silent otherwise; capped. */
typedef struct
{
	unsigned frame;
	int draws, tris, clears, clearsAfterDraw, clearFbo, offscreen, glerr;
	int firstFbo, firstVp[4], firstSc, firstBox[4], firstMask[4], firstDTest, firstDFunc, firstProg, firstBlend;
	int endFbo, endVp[4], endSc;
	unsigned char clearRGB[3];
	int tag[6];
	int probe[6];   /* [PANELMISS] active, window x, y, expected rgb */
	int pstate[16]; /* [PANELMISS] the panel's GL state at its draw */
} GreyFrameSnap;

static GreyFrameSnap s_gf;
extern "C" { int g_PsxGreyTag[6] = { 0, 0, 0, 0, 0, 0 }; }
/* [PANELMISS] armed each frame by an overlay that wants its presence checked in
 * the presented image (pc_bind_panel.c); consumed at the swap. */
extern "C" { int g_PsxPanelProbe[6] = { 0 }; int g_PsxPanelState[16] = { 0 }; }

static void GreyFrame_FirstDraw(void)
{
#if USE_OPENGL
	GLboolean m[4] = { 1, 1, 1, 1 };
	GLint     iv = 0;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &iv); s_gf.firstFbo = iv;
	glGetIntegerv(GL_VIEWPORT, s_gf.firstVp);
	s_gf.firstSc = glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0;
	glGetIntegerv(GL_SCISSOR_BOX, s_gf.firstBox);
	glGetBooleanv(GL_COLOR_WRITEMASK, m);
	s_gf.firstMask[0] = m[0]; s_gf.firstMask[1] = m[1]; s_gf.firstMask[2] = m[2]; s_gf.firstMask[3] = m[3];
	s_gf.firstDTest = glIsEnabled(GL_DEPTH_TEST) ? 1 : 0;
	glGetIntegerv(GL_DEPTH_FUNC, &iv); s_gf.firstDFunc = iv;
	glGetIntegerv(GL_CURRENT_PROGRAM, &iv); s_gf.firstProg = iv;
	s_gf.firstBlend = glIsEnabled(GL_BLEND) ? 1 : 0;
#endif
}

static void VoidProbeRows(const char* tag, GLuint readFbo, int w, int h)
{
	/* Full frame to disk (PPM, bottom-up rows flipped) so the pixel POSITIONS can
	 * be analysed offline; a histogram cannot say where on screen a colour sits. */
	{
		unsigned char* fr = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
		if (fr != NULL)
		{
			char name[64]; FILE* fp;
			snprintf(name, sizeof(name), "voidprobe_%s.ppm", (strstr(tag, "classes") != NULL) ? "classes" : "scene");
			glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
			glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, fr);
			fp = fopen(name, "wb");
			if (fp != NULL)
			{
				int yy, xx;
				fprintf(fp, "P6\n%d %d\n255\n", w, h);
				for (yy = h - 1; yy >= 0; yy--)
					for (xx = 0; xx < w; xx++)
						fwrite(fr + ((size_t)yy * w + xx) * 4, 1, 3, fp);
				fclose(fp);
				eprintf("[VOIDPROBE] frame dumped to %s\n", name);
			}
			free(fr);
		}
	}
	std::map<unsigned int, int> hist;
	int row, total = 0;
	unsigned char* px = (unsigned char*)malloc((size_t)w * 4);

	if (px == NULL) return;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
	/* Nine rows from 10% to 90% of the height, every pixel. A full map rather
	 * than a handful of first-come bins: the bottom rows are noisy near ground
	 * and filled 16 bins before the void was ever reached (second run). */
	for (row = 0; row < 9; row++)
	{
		int y = (int)((float)h * (0.10f + 0.10f * (float)row));
		int x;
		if (y < 0 || y >= h) continue;
		glReadPixels(0, y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (x = 0; x < w; x++)
		{
			unsigned char r = px[x * 4], g = px[x * 4 + 1], b = px[x * 4 + 2];
			if (r > 150 && g > 150 && b > 150) continue; /* snow specks */
			total++;
			hist[((unsigned)r << 16) | ((unsigned)g << 8) | b]++;
		}
	}
	{
		std::vector<std::pair<int, unsigned int> > top;
		char line[512]; int len; size_t i;
		for (std::map<unsigned int, int>::const_iterator it = hist.begin(); it != hist.end(); ++it)
			top.push_back(std::make_pair(it->second, it->first));
		std::sort(top.begin(), top.end());
		len = snprintf(line, sizeof(line), "[VOIDPROBE] %s %dx%d samples=%d distinct=%u:", tag, w, h, total, (unsigned)hist.size());
		for (i = 0; i < top.size() && i < 12 && len < (int)sizeof(line) - 40; i++)
		{
			const std::pair<int, unsigned int>& e = top[top.size() - 1 - i];
			len += snprintf(line + len, sizeof(line) - (size_t)len, " (%u,%u,%u)x%d",
			                (e.second >> 16) & 255, (e.second >> 8) & 255, e.second & 255, e.first);
		}
		eprintf("%s\n", line);
	}
	free(px);
}

extern "C" void GR_VoidProbeScene(void)
{
	if (!g_PsxVoidProbeArmed) return;
	eprintf("[VOIDPROBE] clear=(%d,%d,%d) fogColor=(%.2f,%.2f,%.2f)/255 fogStrength=%.3f msaa=%d internalFBO=%u\n",
	        g_PsxLastClearRGB[0], g_PsxLastClearRGB[1], g_PsxLastClearRGB[2],
	        g_PsyX_FogColor[0] * 255.0f, g_PsyX_FogColor[1] * 255.0f, g_PsyX_FogColor[2] * 255.0f,
	        g_PsyX_FogStrength, s_internalSamples, (unsigned)g_internalFBO);
	/* With no internal target the scene IS the window: read framebuffer 0 at
	 * the present size, or the read is 0x0 and says nothing (first run). */
	if (g_internalFBO != 0)
		VoidProbeRows(g_PsxVoidProbeArmed == 2 ? "classes" : "scene", GR_ScreenReadFBO(), s_internalW, s_internalH);
	else
		VoidProbeRows(g_PsxVoidProbeArmed == 2 ? "classes(window)" : "scene(window)", 0, g_windowWidth, g_windowHeight);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());
}

void GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	framebuffer_need_update = 1;
	g_PsxLastClearRGB[0] = r; g_PsxLastClearRGB[1] = g; g_PsxLastClearRGB[2] = b;

	s_gf.clears++;
	if (s_gf.draws > 0)
		s_gf.clearsAfterDraw++;
	s_gf.clearRGB[0] = r; s_gf.clearRGB[1] = g; s_gf.clearRGB[2] = b;
#if USE_OPENGL
	{
		GLint fbo = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
		s_gf.clearFbo = fbo;
	}
#endif

#if USE_OPENGL
	/* PC port: when pillarboxing (4:3 content centered in a wider window), keep
	 * the side bars black even when the game clears the framebuffer to a
	 * non-black color. The item-examine ("story item") screen clears to the gray
	 * fog color, which otherwise tints the bars. Clear the whole window black,
	 * then clear only the 4:3 region to the requested color via a scissor.
	 * Skipped when the clear is already black (menus) — those bars are fine. */
	const bool wantPillarbox =
		!g_PcNativeRender &&
		((g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
		 (!g_PcHorPlusEnabled && g_PcMenuPillarbox));
	/* Menus are NOT excluded any more. They used to be, on the grounds that a
	 * black clear leaves black bars anyway -- but the fall-through path at the
	 * bottom runs with the PSX clip-rect SCISSOR still enabled, so its clear is
	 * confined to the 4:3 region and the bars are never written at all. They
	 * kept whatever was last drawn there, which is why warm resetting to the
	 * title left gameplay showing down both sides. Taking this branch for a
	 * black clear simply makes both clears black, and the bars get cleared
	 * because this path disables the scissor first. */
	if (wantPillarbox && g_windowWidth > 0 && g_windowHeight > 0)
	{
		const float psxAspect = 4.0f / 3.0f;
		const float winAspect = (float)g_windowWidth / (float)g_windowHeight;
		if (winAspect > psxAspect)
		{
			const int vpW = (int)(g_windowHeight * psxAspect + 0.5f);
			const int vpX = (g_windowWidth - vpW) / 2;

			glDisable(GL_SCISSOR_TEST);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

			glEnable(GL_SCISSOR_TEST);
			glScissor(vpX, 0, vpW, g_windowHeight);
			glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
			return;
		}
	}

	glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
}

/* Half-widths of the world ortho, in PSX X units measured from centre:
 * `out43` is the 4:3 frame the shot was authored for, `outWide` is what Hor+
 * actually installs. Their difference IS the extra picture widescreen reveals
 * on each side, which is what a shot framed to hide something off the 4:3 edge
 * needs to be rotated by. Reported rather than recomputed by the caller so the
 * two can never drift apart -- these are the same expressions used to build the
 * ortho above. Returns 0 when no widening is in effect. */
/* The pixel aspect the renderer is ACTUALLY using this frame.
 *
 * g_PsxPixelAspect is only the `par` knob, and display_aspect = crt does not
 * read it: it solves for the aspect that lands the picture on its 4:3 target
 * over the live hfov/vfov. Game-side code that sizes anything to the visible
 * frame -- overlay quads, cull bounds, the letterbox bars -- must use THIS,
 * or it computes the frame the port had before the CRT solve existed and
 * comes up short by exactly the ratio between the two. */
extern "C" float GR_LivePixelAspect(void)
{
	return (float)PSX_NTSC_PIXEL_ASPECT;
}

extern "C" int GR_HorPlusHalfWidths(float* out43, float* outWide)
{
	float psxW, psxH, psxAspect, winAspect, horScale, effectiveScale, margin, hscale;

	if (!out43 || !outWide || !g_PcHorPlusEnabled || g_PcWidescreenMode != 1)
		return 0;

	psxW = (float)activeDispEnv.disp.w;
	psxH = (float)activeDispEnv.disp.h;
	if (psxW <= 0.0f || psxH <= 0.0f || g_windowHeight <= 0)
		return 0;

	psxAspect = psxW / psxH;
	winAspect = (float)g_windowWidth / (float)g_windowHeight;
	horScale  = winAspect / psxAspect;
	if (horScale <= 1.0f)
		return 0;

	effectiveScale = horScale * PSX_NTSC_PIXEL_ASPECT;
	margin         = psxW * (effectiveScale - 1.0f) * 0.5f;
	hscale         = (g_PsxWorldHScale > 0.0f) ? g_PsxWorldHScale : 1.0f;

	/* The 4:3 reference is the TRUE PSX frame, NOT the hfov-scaled one. hfov
	 * (g_PsxWorldHScale) widens what is shown in every mode, so measuring
	 * against it under-reports how far past the authored 320-wide frame the
	 * picture now reaches -- which is the edge scenery was actually built to.
	 * outWide keeps hscale because that is the ortho really installed. */
	*out43   = psxW * 0.5f;
	*outWide = (psxW * 0.5f + margin) / hscale;
	return 1;
}

void GR_SaveVRAM(const char* outputFileName, int x, int y, int width, int height, int bReadFromFrameBuffer)
{
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

#if USE_OPENGL

#define FLIP_Y (VRAM_HEIGHT - i - 1)

#endif

	FILE* fp = fopen(outputFileName, "wb");
	if (fp == NULL)
		return;

	unsigned char TGAheader[12] = { 0,0,2,0,0,0,0,0,0,0,0,0 };
	unsigned char header[6];
	header[0] = (width % 256);
	header[1] = (width / 256);
	header[2] = (height % 256);
	header[3] = (height / 256);
	header[4] = 16;
	header[5] = 0;

	fwrite(TGAheader, sizeof(unsigned char), 12, fp);
	fwrite(header, sizeof(unsigned char), 6, fp);

	for (int i = 0; i < VRAM_HEIGHT; i++)
	{
		fwrite(vram + VRAM_WIDTH * FLIP_Y, sizeof(short), VRAM_WIDTH, fp);
	}

	fclose(fp);

#undef FLIP_Y
#endif
}

void GR_CopyRGBAFramebufferToVRAM(u_int* src, int x, int y, int w, int h, int update_vram, int flip_y)
{
	assert(x >= 0);
	assert(y >= 0);
	assert(x + w <= VRAM_WIDTH);
	assert(y + h <= VRAM_WIDTH);

	ushort* fb = (ushort*)malloc(w * h * sizeof(ushort));
	uint* data_src = (uint*)src;
	ushort* data_dst = (ushort*)fb;

	for (int i = 0; i < h; i++)
	{
		for (int j = 0; j < w; j++)
		{
			uint c = *data_src++;

			u_char b = ((c >> 3) & 0x1F);
			u_char g = ((c >> 11) & 0x1F);
			u_char r = ((c >> 19) & 0x1F);
			//u_char a = ((c >> 24) & 0x1F);

			int a = r == g == b == 0 ? 0 : 1;

			*data_dst++ = r | (g << 5) | (b << 10) | (a << 15);
		}
	}

	ushort* ptr = (ushort*)vram + VRAM_WIDTH * y + x;

	for (int fy = 0; fy < h; fy++)
	{
		int py = flip_y ? (h - fy - 1) : fy;
		ushort* fb_ptr = fb + (h * py / h) * w;

		for (int fx = 0; fx < w; fx++)
			ptr[fx] = fb_ptr[w * fx / w];

		ptr += VRAM_WIDTH;
	}

	free(fb);

	if (update_vram)
		vram_need_update = 1;
}

void GR_ReadFramebufferDataToVRAM()
{
	int x, y, w, h;
	if (!framebuffer_need_update)
		return;

	framebuffer_need_update = 0;

	/* PC port: skip readback while a paper-map / TIM-protect screen is active.
	 * The readback writes the framebuffer back into vram[] at (disp.x, disp.y)
	 * which can clobber CLUT/texture rows that live inside the display rect
	 * (paper-map CLUT lives at VRAM (224,15) — inside (0,0)-(320,240)). */
	if (g_PsxSkipFramebufferStore)
		return;

	x = g_PreviousFramebuffer.x;
	y = g_PreviousFramebuffer.y;
	w = g_PreviousFramebuffer.w;
	h = g_PreviousFramebuffer.h;

	/* The only writer of g_PreviousFramebuffer (GR_StoreFrameBuffer) is
	 * compiled out under PSYX_SKIP_FRAMEBUFFER_STORE, so this rect is 0x0 in
	 * the PC-port build — the 2 MB glGetTexImage + glMapBuffer readback below
	 * ran every frame only to feed a zero-iteration copy. Near-free on modern
	 * drivers (async PBO DMA); a synchronous full-pipeline flush per frame on
	 * the old-Intel GL 3.1 tier — the reported 10fps class. */
	if (w <= 0 || h <= 0)
		return;

	// now we can read it back to VRAM texture

#if USE_OPENGL && defined(USE_PBO)
	// read the texture
	if(g_glFramebufferPBO.pixels)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		PBO_Download(&g_glFramebufferPBO);
		glBindTexture(GL_TEXTURE_2D, 0);
		GR_CopyRGBAFramebufferToVRAM((u_int*)g_glFramebufferPBO.pixels, x, y, w, h, 0, 0);
	}
#endif
}

void GR_SetScissorState(int enable)
{
	if (g_PreviousScissorState == enable)
		return;

#if USE_OPENGL
	if (g_PreviousScissorState)
		glDisable(GL_SCISSOR_TEST);
	else
		glEnable(GL_SCISSOR_TEST);
#endif
	g_PreviousScissorState = enable;
}

/* PC port: map a window-pixel point to a [0,1] fraction of the letterboxed 4:3
 * display viewport — the exact pillarbox rect GR_SetOffscreenState installs
 * below. Used by the mouse-cursor feature to convert an OS cursor position into
 * the game's 2D space. Returns 1 if the point is inside the viewport (0 = in the
 * black bars). Mirrors the viewport block near "Display viewport" below; keep in
 * sync if that math changes. */
/* PC port: the rect the display viewport block picks for the picture (render
 * pixels, the pillarbox rule included), computed from the current flags rather
 * than from whatever GL_VIEWPORT the last draw left. A GL overlay that lays
 * itself out against this stays the same size on frames that drew nothing and
 * lines up with PsyX_MapWindowToViewport's pointer fractions. */
extern "C" void PsyX_GetDisplayViewport(int* outX, int* outY, int* outW, int* outH)
{
	int vpX = 0, vpY = 0, vpW = g_windowWidth, vpH = g_windowHeight;

	if (s_overlayToWindow)
	{
		GR_PresentRect(&vpX, &vpY, &vpW, &vpH);
		if (outX) *outX = vpX;
		if (outY) *outY = vpY;
		if (outW) *outW = vpW;
		if (outH) *outH = vpH;
		return;
	}
	const bool wantPillarbox =
		!g_PcNativeRender &&
		((g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
		 (!g_PcHorPlusEnabled && g_PcMenuPillarbox));
	if (wantPillarbox && g_windowHeight > 0) {
		const float psxAspect = 4.0f / 3.0f;
		const float winAspect = (float)g_windowWidth / (float)g_windowHeight;
		if (winAspect > psxAspect) {
			vpW = (int)(g_windowHeight * psxAspect + 0.5f);
			vpX = (g_windowWidth - vpW) / 2;
		}
	}
	if (outX) *outX = vpX;
	if (outY) *outY = vpY;
	if (outW) *outW = vpW;
	if (outH) *outH = vpH;
}

extern "C" int PsyX_MapWindowToViewport(int mx, int my, float* outFracX, float* outFracY)
{
	int vpX = 0, vpY = 0, vpW = g_windowWidth, vpH = g_windowHeight;

	/* mx/my arrive in WINDOW pixels from SDL, but everything below works in
	 * render pixels. Those are the same thing except in borderless with an
	 * internal target, where the scene is drawn smaller and stretched at
	 * present -- so scale the pointer by the same factor or the cursor drifts
	 * from what it is pointing at. Single conversion point, so every mouse
	 * consumer is corrected at once. */
	if (g_internalFBO != 0 && g_presentWidth > 0 && g_presentHeight > 0)
	{
		int dx, dy, dw, dh;
		GR_PresentRect(&dx, &dy, &dw, &dh);

		if (dw > 0 && dh > 0)
		{
			mx = (int)((float)(mx - dx) * (float)g_windowWidth  / (float)dw);
			my = (int)((float)(my - dy) * (float)g_windowHeight / (float)dh);
		}
	}
	const bool wantPillarbox =
		!g_PcNativeRender &&
		((g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
		 (!g_PcHorPlusEnabled && g_PcMenuPillarbox));
	if (wantPillarbox && g_windowHeight > 0) {
		const float psxAspect = 4.0f / 3.0f;
		const float winAspect = (float)g_windowWidth / (float)g_windowHeight;
		if (winAspect > psxAspect) {
			vpW = (int)(g_windowHeight * psxAspect + 0.5f);
			vpX = (g_windowWidth - vpW) / 2;
		}
	}
	const float fx = (vpW > 0) ? (float)(mx - vpX) / (float)vpW : 0.0f;
	const float fy = (vpH > 0) ? (float)(my - vpY) / (float)vpH : 0.0f;
	if (outFracX) *outFracX = fx;
	if (outFracY) *outFracY = fy;
	return (fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f) ? 1 : 0;
}

/* PC port: the window sub-rect the game is actually presented into (pillarbox
 * leaves black bars at the sides). The framebuffer-feedback capture must read
 * THIS, not the whole window: squashing the full window into the 320x224
 * feedback buffer shrinks the image horizontally by vpW/windowWidth, and because
 * the effect re-reads its own output every frame that scale compounds — the
 * picture collapses toward the centre and builds up a vertical line after a few
 * seconds. Recorded from the same block that calls GR_SetViewPort so the two can
 * never disagree. */
static int g_presentVp[4] = { 0, 0, 0, 0 };

/* Window rect (GL coords, x0/y0/x1/y1, origin bottom-left) that the PSX display
 * buffer occupies under the world ortho. Set alongside g_presentVp; this is what
 * the framebuffer-feedback capture reads, so capture and redraw are 1:1. */
static float g_psxAreaVp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static int   g_psxAreaVpValid = 0;

/* The same rect under the 2D UI ortho (OT2). The two passes put the PSX display
 * buffer in DIFFERENT window rects -- the world pass carries the Hor+ widening
 * and the g_PsxWorldVScale crop, the UI pass carries neither -- so a capture
 * read through one and redrawn under the other is rescaled every iteration.
 * Which one a feedback prim needs is decided by the pass it is drawn in:
 * the loading blur draws with the world ortho, the per-map dream overlays are
 * OT2 prims. */
static float g_psxUiAreaVp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static int   g_psxUiAreaVpValid = 0;

/* Set when a prim sampling a display buffer was drawn in the UI pass, so the
 * capture below knows which of the two rects to read. */
static int   g_fbSamplerUiPass = 0;

/* Widescreen feedback. The effect's primitives are 320 PSX pixels wide, so on a
 * widened ortho they cover the 4:3 core and leave the margins sharp -- a blurred
 * letterbox with two hard seams. Both ends are stretched instead: the capture
 * reads the WHOLE picture rect into the same 320-wide store, and the primitives
 * are scaled about the ortho centre to span it again. The store is a blur, so
 * spreading its 320 columns over a wider window costs nothing that matters, and
 * because both ends move together the loop stays exactly 1:1 -- the one thing a
 * feedback effect cannot tolerate getting wrong. Scale is 1.0 in 4:3, where this
 * is a no-op and the proven path is untouched. */
extern "C" { float g_PsxFeedbackWideScale  = 1.0f; }
extern "C" { float g_PsxFeedbackWideCenter = 160.0f; }

/* The WORLD pass's ortho in display coordinates (L, R, T, B) and the display
 * size it was built for. A flat fill authored to cover the whole 320x224 frame
 * (a scene tint, a flash) is drawn in OT0, so it inherits the Hor+ widening and
 * the g_PsxWorldVScale crop that exist for 3D geometry, and comes out as a 4:3
 * box that stops short of the bottom. The rect builder remaps such prims onto
 * this rect, which is exactly the picture. Valid once a world pass has run. */
extern "C" { float g_PsxWorldOrtho[4]  = { 0.0f, 320.0f, 0.0f, 224.0f }; }
extern "C" { float g_PsxWorldDisp[2]   = { 320.0f, 224.0f }; }
extern "C" { int   g_PsxWorldOrthoValid = 0; }

/* A prim is sampling a display buffer this frame, so the store has work to do.
 * Called from the one place every textured prim passes through, keyed on the
 * signature of the whole effect family: a 16bpp tpage addressing the left 320
 * VRAM columns, where the display buffers live (real 16bpp game textures sit at
 * x >= 512 in every session log). That is also what tells the store WHICH pass
 * will redraw the capture, so no scene has to declare itself.
 *
 * The count is a short trailing window rather than a bool: the store runs at
 * the end of the frame that armed it, and one frame where the overlay happens
 * not to draw should not blank the rect underneath a running effect. */
extern "C" { int g_cfg_dreamFeedback = 1; }

/* Does the reader BLEND the stored frame (per-map dream overlay) or replace it
 * (loading trail)? Decides the mask bit the frame is packed with, and the loop
 * gain: a blending reader composites 0.5*stored + 0.5*live, which is itself the
 * decay PSX relies on, so its store runs at unity. An opaque reader has no such
 * decay and its loop has to be damped or it diverges. */
static int g_fbSamplerSemiTrans = 0;

/* The reader's modulation this frame was below 128: a PS1 decay frame, where the
 * copy loses one 5-bit step. The loading trail alternates 127/128 on the game's
 * own vblank cadence; the exact pack applies the step only then. */
static int g_fbSamplerDecay = 0;

/* What the last per-present store was packed with, so a repack after a full
 * vram[] re-upload reproduces it instead of re-deciding. */
static int g_fbLastStoreDecay = 0;
static int g_fbLastStoreSemi  = 0;

/* 1 = opaque readers use the exact PS1 loop (see the pack shader), 0 = the old
 * damped loop. Console FBEXACT. */
extern "C" { int g_PsxFeedbackExact = 1; }

/* Set by a scene that steps slower than the present rate (the loading screen)
 * on each frame between its steps: that present takes no feedback pass. */
extern "C" { int g_PsxFeedbackHoldFrame = 0; }

extern "C" void GR_NoteFeedbackSamplerPrim(int semiTrans, int modColour)
{
	g_fbSamplerUiPass    = g_PsxUIOrthoPass;
	g_fbSamplerSemiTrans = semiTrans;
	g_fbSamplerDecay     = (modColour < 128) ? 1 : 0;

	if (g_cfg_dreamFeedback)
		g_PsxFeedbackStoreAllowed = 2;
}

void GR_SetOffscreenState(const RECT16* offscreenRect, int enable)
{
	if (enable)
	{
		// setup render target viewport
		GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
	}
	else
	{
		// setup default viewport

		/* The ortho actually installed below, kept in this scope so the viewport
		 * block can work out where the PSX display buffer lands in the window
		 * (g_psxAreaVp — the framebuffer-feedback capture source). */
		float fbOrthoL = 0.0f, fbOrthoR = 0.0f, fbOrthoT = 0.0f, fbOrthoB = 0.0f;
		float fbPsxW = 0.0f, fbPsxH = 0.0f;

		{
			// Widescreen presentation. Three modes (g_PcWidescreenMode):
			//   0 = Pillarbox (PSX-faithful, default): 4:3 ortho rendered into
			//       a centered 4:3 viewport of the window. Black bars on
			//       sides. Characters and framing match PSX CRT exactly.
			//   1 = Hor+: widen ortho horizontally so GTE-projected geometry
			//       beyond the PSX framebuffer is visible. Reveals scene
			//       content cropped on PSX (extra bar counter, walls etc.).
			//   2 = Stretch (anamorphic): 4:3 content fills 16:9, chars wider.
			//
			// g_PcHorPlusEnabled=0 (2D UI screens) always uses the unwidened
			// 4:3 ortho with full-window viewport — UI was authored stretched.
			//
			// The actual viewport for pillarbox is set in the !enable branch
			// below (search "Pillarbox viewport"); ortho here is matched to
			// whatever viewport that branch picks.
			const float psxW = (float)activeDispEnv.disp.w;  // 320
			float psxH = (float)activeDispEnv.disp.h;   // 224 — aspect/horizontal only
			float orthoTop = 0.0f, orthoBot = psxH;
			if (g_PcHorPlusEnabled) {
				/* 3D gameplay world renders ~14.7% too much vertical world (measured: vertical
				 * scale 0.872 vs DuckStation at a fixed 4:3 spot, horizontal 1.0, top-aligned
				 * with the extra at the BOTTOM = near foreground). Crop the world ortho to
				 * g_PsxWorldVScale of the buffer, top-anchored (keep ceiling, clip foreground),
				 * which also zooms objects ~1/scale taller. psxH (aspect) stays full so the
				 * horizontal + Hor+ logic is unchanged. Console `vfov` tunes it. */
				/* The world (OT0) is ALWAYS cropped — gameplay and cutscenes alike — so
				 * 3D framing matches PSX/DuckStation (the old cutscene vscale-skip un-cropped
				 * the whole frame and read as stretched). The 2D UI pass (g_PsxUIOrthoPass:
				 * OT2 — subtitles, fade, cutscene letterbox bars) instead gets full vertical
				 * ortho so it isn't scaled/clipped off the bottom. The FIX_ANG framing shift
				 * (g_PsxWorldVShift) is applied at the GTE projection center by the game, not
				 * here — an ortho-window shift reveals rows overlay prims never cover. */
				const float vscale = (g_PsxUIOrthoPass || g_PsxItemTakeActive) ? 1.0f
				                   : (g_PsxCutsceneActive && g_PsxCutsceneVScale > 0.0f
				                      ? g_PsxCutsceneVScale : g_PsxWorldVScale);
				orthoTop = psxH * (1.0f - vscale) * g_PsxWorldVCropAnchor;
				orthoBot = orthoTop + psxH * vscale;
			}
			const float psxAspect = psxW / psxH;
			const float winAspect = (g_windowHeight > 0)
				? ((float)g_windowWidth / (float)g_windowHeight)
				: psxAspect;
			const float horScale = winAspect / psxAspect;
			fbPsxW   = psxW;
			fbPsxH   = (float)activeDispEnv.disp.h;
			fbOrthoT = orthoTop;
			fbOrthoB = orthoBot;
			fbOrthoL = 0.0f;
			fbOrthoR = psxW;
			/* hfov for the 4:3 3D branches below: scale the horizontal extent
			 * around the centre exactly like the Hor+ branch does, so hfov works in
			 * 4:3-window GAMEPLAY too. The g_PcHorPlusEnabled gate is load-bearing:
			 * it is the "3D gameplay vs 2D screen" signal, and applying hfov to 2D
			 * screens shrank the NTSC title background and revealed VRAM garbage at
			 * its sides (user-reported 2026-08-25). Never the UI pass either. */
			/* The 4:3 paths carry hfov too, so a CRT picture has to drop it here as
			 * well or pillarbox stays squashed while the widened path is correct --
			 * which is exactly how the two modes came to disagree. */
			const float hs43   = (!g_PsxAspectRaw) ? 1.0f
			                   : ((g_PcHorPlusEnabled && !g_PsxUIOrthoPass) ? g_PsxWorldHScale : 1.0f);
			const float half43 = (psxW * 0.5f) / ((hs43 > 0.0f) ? hs43 : 1.0f);
			if (!g_PcHorPlusEnabled || horScale <= 1.0f) {
				/* 2D UI or non-widescreen window: 4:3 ortho, full viewport. */
				if (hs43 != 1.0f) { fbOrthoL = psxW * 0.5f - half43; fbOrthoR = psxW * 0.5f + half43; }
				GR_Ortho2D(fbOrthoL, fbOrthoR, orthoBot, orthoTop, -1.0f, 1.0f);
			} else if (g_PcWidescreenMode == 1) {
				/* Hor+ widescreen: widen ortho, full-window viewport. PSX_NTSC_PIXEL_ASPECT
				 * preserves 1 H px = 1 V px scaling for character proportions. */
				const float effectiveScale = horScale * PSX_NTSC_PIXEL_ASPECT;
				const float margin = psxW * (effectiveScale - 1.0f) * 0.5f;
				/* hfov: scale the horizontal world extent around center (g_PsxWorldHScale).
				 * 1.0 = identity (-margin..psxW+margin); >1 shrinks the ortho width = wider
				 * models. Skipped for the UI pass so 2D UI stays aligned. */
				const float hscale = g_PsxUIOrthoPass ? 1.0f : g_PsxWorldHScale;
				const float cx     = psxW * 0.5f;
				const float halfW  = (psxW * 0.5f + margin) / hscale;
				fbOrthoL = cx - halfW;
				fbOrthoR = cx + halfW;
				GR_Ortho2D(cx - halfW, cx + halfW, orthoBot, orthoTop, -1.0f, 1.0f);
			} else {
				/* Pillarbox (mode 0, default) or stretch (mode 2): 4:3 ortho.
				 * The viewport (below) handles pillarbox vs full-window. */
				if (hs43 != 1.0f) { fbOrthoL = psxW * 0.5f - half43; fbOrthoR = psxW * 0.5f + half43; }
				GR_Ortho2D(fbOrthoL, fbOrthoR, orthoBot, orthoTop, -1.0f, 1.0f);
			}

			/* Publish the OVERLAY pass's visible rectangle, in prim coordinates,
			 * for HUD placement. OT2 (the g_OtTags0 layers: minimap, cutscene
			 * bars, crosshair) is drawn under the UI ortho, which carries neither
			 * hfov nor vfov, so the HUD has to be placed against THIS ortho.
			 * Placing it against the world ortho, whose width is divided by hfov,
			 * pushed the minimap past the visible edge whenever hfov was not 1.
			 * Latched on gameplay frames only (the flag is 0 on menu frames) so a
			 * menu's 4:3 ortho cannot move it. The vertical span is the real one
			 * too, so a 224-line display no longer masquerades as 240. */
			if (g_PcHorPlusEnabled && g_PsxUIOrthoPass) {
				float ox = 0.0f, oy = 0.0f;
				PsyX_GetDrawEnvOffset(&ox, &oy);
				g_PcHudRect[0] = fbOrthoL - ox;
				g_PcHudRect[1] = fbOrthoR - ox;
				g_PcHudRect[2] = fbOrthoT - oy;
				g_PcHudRect[3] = fbOrthoB - oy;
			}

			/* Same latch for the world pass, for console object picking. */
			if (g_PcHorPlusEnabled && !g_PsxUIOrthoPass) {
				float ox = 0.0f, oy = 0.0f;
				PsyX_GetDrawEnvOffset(&ox, &oy);
				g_PcWorldOrthoRect[0] = fbOrthoL - ox;
				g_PcWorldOrthoRect[1] = fbOrthoR - ox;
				g_PcWorldOrthoRect[2] = fbOrthoT - oy;
				g_PcWorldOrthoRect[3] = fbOrthoB - oy;
			}

			/* [ASPECT] ground-truth dump of the ACTUAL runtime projection
			 * inputs so the on-screen aspect/FOV can be computed from real
			 * values instead of assumptions. Goes to g_logStream (=SilentHill.log
			 * when debug logging is on) so it lands in the captured log, not
			 * stderr. Logs once per distinct (disp,win,HorPlus,WS) state so the
			 * 2D menu and the 3D gameplay framing are both captured. C2_H is the
			 * GTE geom-screen distance (sets horizontal FOV); C2_OFX/OFY are the
			 * projection center (>>16 = pixels). */
			{
				extern FILE* g_logStream;
				FILE* aspOut = g_logStream ? g_logStream : stderr;
				static unsigned s_aspSeen[8];
				static int s_aspSeenN = 0;
				/* viewport that the block below will pick (recomputed here). */
				int dbgVpW = g_windowWidth, dbgVpX = 0;
				const bool dbgPillar =
					(g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
					(!g_PcHorPlusEnabled && g_PcMenuPillarbox);
				if (dbgPillar && g_windowHeight > 0 && winAspect > psxAspect) {
					dbgVpW = (int)(g_windowHeight * psxAspect + 0.5f);
					dbgVpX = (g_windowWidth - dbgVpW) / 2;
				}
				const float dbgMargin = (g_PcHorPlusEnabled && horScale > 1.0f && g_PcWidescreenMode == 1)
					? psxW * (horScale * PSX_NTSC_PIXEL_ASPECT - 1.0f) * 0.5f : 0.0f;
				unsigned key = ((unsigned)(activeDispEnv.disp.w & 0x3FF) << 22) ^
					((unsigned)(activeDispEnv.disp.h & 0x3FF) << 12) ^
					((unsigned)(g_windowWidth & 0xFFF)) ^
					((unsigned)(g_PcHorPlusEnabled ? 1u : 0u) << 31) ^
					((unsigned)(g_PcWidescreenMode & 3) << 29);
				int seen = 0;
				for (int i = 0; i < s_aspSeenN; i++) if (s_aspSeen[i] == key) { seen = 1; break; }
				if (!seen && s_aspSeenN < 8) {
					s_aspSeen[s_aspSeenN++] = key;
					fprintf(aspOut, "[ASPECT] disp=%dx%d win=%dx%d psxAspect=%.4f winAspect=%.4f horScale=%.4f HorPlus=%d WS=%d PAR=%.4f margin=%.1f vp=%d+%dx%d C2_H=%d OFX=%d OFY=%d\n",
						(int)activeDispEnv.disp.w, (int)activeDispEnv.disp.h,
						g_windowWidth, g_windowHeight, psxAspect, winAspect, horScale,
						g_PcHorPlusEnabled, g_PcWidescreenMode, (float)PSX_NTSC_PIXEL_ASPECT,
						dbgMargin, dbgVpX, dbgVpW, g_windowHeight,
						(int)C2_H, (int)(C2_OFX >> 16), (int)(C2_OFY >> 16));
					fflush(aspOut);
				}
			}
		}

		/* Display viewport — set EVERY call, not just on offscreen-state
		 * change. GR_BeginScene resets the viewport to the full window each
		 * frame, so a stable screen (e.g. a menu that never toggles the
		 * offscreen state) would otherwise lose its pillarbox after frame 1.
		 * Pillarbox the central 4:3 region (black bars) for 3D gameplay in
		 * mode 0, or for 2D screens (menus/load) when g_PcMenuPillarbox is on;
		 * otherwise full window (stretch). */
		{
			int vpX = 0, vpY = 0, vpW = g_windowWidth, vpH = g_windowHeight;
			const bool wantPillarbox =
				!g_PcNativeRender &&
				((g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
				 (!g_PcHorPlusEnabled && g_PcMenuPillarbox));
			if (wantPillarbox && g_windowHeight > 0) {
				const float psxAspect = 4.0f / 3.0f;
				const float winAspect = (float)g_windowWidth / (float)g_windowHeight;
				if (winAspect > psxAspect) {
					vpW = (int)(g_windowHeight * psxAspect + 0.5f);
					vpX = (g_windowWidth - vpW) / 2;
				}
			}
			GR_SetViewPort(vpX, vpY, vpW, vpH);

			/* Presented sub-rect of the window (pillarbox bars excluded). */
			g_presentVp[0] = vpX; g_presentVp[1] = vpY;
			g_presentVp[2] = vpW; g_presentVp[3] = vpH;

			/* Where the PSX display buffer [0,dispW]x[0,dispH] actually lands in
			 * the window under THIS ortho — the source rect the feedback capture
			 * must read. It is NOT the viewport: Hor+ widens the ortho past the
			 * buffer (the 4:3 core covers only 1/effectiveScale of the window) and
			 * g_PsxWorldVScale crops it vertically, so capturing the viewport
			 * rescales the picture every frame. The blur redraws the captured rect
			 * over exactly these PSX coordinates, so any mismatch compounds — see
			 * GR_StoreFrameBufferPsx. Recorded from the WORLD pass only, because
			 * that is the ortho the blur's OT0 prims are drawn under.
			 * GL window coords, origin bottom-left: ortho y=orthoTop is the TOP. */
			if (fbOrthoR > fbOrthoL && fbOrthoB > fbOrthoT)
			{
				const float sx  = (float)vpW / (fbOrthoR - fbOrthoL);
				const float sy  = (float)vpH / (fbOrthoB - fbOrthoT);
				float*      dst = g_PsxUIOrthoPass ? g_psxUiAreaVp : g_psxAreaVp;

				dst[0] = (float)vpX + (0.0f    - fbOrthoL) * sx;
				dst[2] = (float)vpX + (fbPsxW  - fbOrthoL) * sx;
				dst[3] = (float)(vpY + vpH) - (0.0f   - fbOrthoT) * sy;
				dst[1] = (float)(vpY + vpH) - (fbPsxH - fbOrthoT) * sy;

				if (g_PsxUIOrthoPass)
				{
					g_psxUiAreaVpValid = 1;

					/* How far past the 320-wide buffer this ortho reaches, and
					 * about which point. Symmetric in every mode, but derived
					 * rather than assumed. */
					g_PsxFeedbackWideCenter = (fbOrthoL + fbOrthoR) * 0.5f;
					g_PsxFeedbackWideScale  = (fbPsxW > 0.0f)
					                        ? ((fbOrthoR - fbOrthoL) / fbPsxW) : 1.0f;
				}
				else
				{
					g_psxAreaVpValid = 1;

					g_PsxWorldOrtho[0]   = fbOrthoL;
					g_PsxWorldOrtho[1]   = fbOrthoR;
					g_PsxWorldOrtho[2]   = fbOrthoT;
					g_PsxWorldOrtho[3]   = fbOrthoB;
					g_PsxWorldDisp[0]    = fbPsxW;
					g_PsxWorldDisp[1]    = fbPsxH;
					g_PsxWorldOrthoValid = 1;
				}
			}
		}

	}

	if (g_PreviousOffscreenState == enable)
		return;

	g_PreviousOffscreenState = enable;
	if (enable)
		s_gf.offscreen++;

#if USE_OPENGL
	if (enable)
	{
		// set storage size first
		if (g_PreviousOffscreen.w != offscreenRect->w &&
			g_PreviousOffscreen.h != offscreenRect->h)
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, offscreenRect->w, offscreenRect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		g_PreviousOffscreen = *offscreenRect;

		GR_SetViewPort(0, 0, offscreenRect->w, offscreenRect->h);
		glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

		// clear it out
		glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		/* (Display viewport / pillarbox is set above, before the early-out,
		 * so it applies every call. Here we only do the VRAM writeback.) */

		/* PC port: skip the offscreen-RT → VRAM writeback when a TIM-protect
		 * screen is active. This branch fires whenever a draw split has
		 * dfe=0 (offscreen render target). Both the GL blit and the CPU
		 * GR_CopyRGBAFramebufferToVRAM below write to VRAM at
		 * (g_PreviousOffscreen.x, g_PreviousOffscreen.y, w, h) — for paper-map
		 * pickup screens that rect overlaps the CLUT at (224,15) and texture
		 * origin (320,16), tiling rendered framebuffer content over the
		 * just-uploaded TIM. The viewport / state-tracking above must still
		 * run so subsequent draws use the correct framebuffer binding;
		 * only the writes are suppressed. */
		if (g_PsxSkipFramebufferStore)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
		}
		else if (g_PreviousOffscreen.w > 0 && g_PreviousOffscreen.h > 0 &&
		         g_PreviousOffscreen.w <= 64 && g_PreviousOffscreen.h <= 64)
		{
			/* Small VRAM-scratch writeback (sewer water caustic 32x32): the
			 * legacy path below is wrong for these — the raw glBlitFramebuffer
			 * writes unpacked RGBA into the packed-RG8 VRAM texture (corrupts
			 * it), and the full-size PBO adds 2 frames of latency. A scratch
			 * this small is a synchronous glReadPixels of a few KB: read the
			 * FBO, pack into vram[] (kept authoritative so any later full
			 * GR_UpdateVRAM upload re-stamps the same bytes), then sub-upload
			 * the packed region into BOTH double-buffered VRAM textures.
			 * vram_need_update is NOT set — no 1MB full re-upload per frame. */
			static u_int  s_scratchRGBA[64 * 64];
			static ushort s_scratchPacked[64 * 64];
			const int sw = g_PreviousOffscreen.w, sh = g_PreviousOffscreen.h;

			glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);
			glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, s_scratchRGBA);
			glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());

			GR_CopyRGBAFramebufferToVRAM(s_scratchRGBA,
				g_PreviousOffscreen.x, g_PreviousOffscreen.y, sw, sh, 0, 1);

			{
				const ushort* src = (const ushort*)vram +
					VRAM_WIDTH * g_PreviousOffscreen.y + g_PreviousOffscreen.x;
				for (int row = 0; row < sh; row++)
					memcpy(&s_scratchPacked[row * sw], src + row * VRAM_WIDTH, sw * sizeof(ushort));

				for (int t = 0; t < 2; t++)
				{
					glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[t]);
					glTexSubImage2D(GL_TEXTURE_2D, 0,
						g_PreviousOffscreen.x, g_PreviousOffscreen.y, sw, sh,
						VRAM_FORMAT, GL_UNSIGNED_BYTE, s_scratchPacked);
				}
				glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
			}
		}
		else
		{
#if USE_OFFSCREEN_BLIT
		// before drawing set source and target
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			// rebind texture
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

			// setup draw and read framebuffers
			glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);					// source is backbuffer
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

			glBlitFramebuffer(0, 0, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
								g_PreviousOffscreen.x, g_PreviousOffscreen.y + g_PreviousOffscreen.h, g_PreviousOffscreen.x + g_PreviousOffscreen.w, g_PreviousOffscreen.y,
								GL_COLOR_BUFFER_BIT, GL_NEAREST);

			// done, unbind
			glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());
		}
#endif

		glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
		// copy rendering results to VRAM texture
		{
			// reat the texture
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			//glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			PBO_Download(&g_glOffscreenPBO);
			glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

			// Don't forcely update VRAM
			GR_CopyRGBAFramebufferToVRAM((u_int*)g_glOffscreenPBO.pixels,
				g_PreviousOffscreen.x, g_PreviousOffscreen.y, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
				USE_OFFSCREEN_BLIT == 0, 1);
		}
		}

	}
#endif
}

/* ============================================================================
 * PC port: full-screen post-process pass + MSAA-safe fullscreen blit helper.
 *
 * One tiny shader program draws a single full-screen triangle (from gl_VertexID,
 * no vertex buffer) sampling a source texture, applying the look selected by
 * g_cfg_postProcess. The same helper is reused to "present" the freeze-frame
 * when MSAA is on (a single-sample -> multisample glBlitFramebuffer is illegal,
 * but a shader draw into the multisample default FBO is fine).
 * ========================================================================== */
#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)
#define PSYX_HAS_POSTPROCESS 1
#else
#define PSYX_HAS_POSTPROCESS 0
#endif

#if PSYX_HAS_POSTPROCESS

static ShaderID g_postShader = (ShaderID)-1;
static GLint    g_postLoc_mode = -1;
static GLint    g_postLoc_texSize = -1;
static GLint    g_postLoc_time = -1;
static GLint    g_postLoc_tonemap = -1;
static GLint    g_postLoc_postInt = -1;
static GLint    g_postLoc_tmInt = -1;
static GLint    g_postLoc_bright = -1;
static GLint    g_postLoc_contrast = -1;
static GLint    g_postLoc_satur = -1;
static GLint    g_postLoc_calib = -1;

/* Set nonzero by the Brightness screen so GR_PostProcess overlays the reference
 * bar; cleared when it exits. */
extern "C" { int g_cfg_calibBar = 0; }

/* Image-adjustment scope (config key image_adjust_scope).
 *
 * Brightness/contrast/saturation are a grade of the COMPOSED frame, so they
 * used to hit the menus, the paper map, the save screen and every FMV as well
 * as the world -- the complaint in issue #150. With world-only scope the grade
 * is skipped on frames that render no 3D world, which on this game means the
 * UI and movie screens, since there is no persistent in-game HUD to worry
 * about.
 *
 * The look filters (CRT, scanlines, vignette, grain, sharpen) and the tonemap
 * are deliberately NOT scoped: those emulate a display or a film response and
 * belong over the whole frame.
 *
 * g_cfg_sceneFrame is published once per frame by the game side
 * (DbgOverlay_Render). g_cfg_calibBar forces the grade back on for the
 * Brightness screen itself -- it IS a UI screen, and without this exception
 * its own calibration preview would stop responding to the sliders. */
extern "C" { int g_cfg_imageAdjustScope = 1; int g_cfg_sceneFrame = 1; }

extern "C" int GR_ImageAdjustActive(void)
{
	return g_cfg_imageAdjustScope == 0 || g_cfg_sceneFrame || g_cfg_calibBar;
}
static GLuint   g_postVAO = 0;
static GLuint   g_postFBO = 0;
static TextureID g_postTex = (TextureID)-1;
static int      g_postW = 0;
static int      g_postH = 0;
static unsigned g_postFrame = 0;

static const char* s_postShaderSrc =
	"varying vec2 v_uv;\n"
	"#ifdef VERTEX\n"
	"void main() {\n"
	"	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	v_uv = (p + 1.0) * 0.5;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n"
	"#else\n"
	"uniform sampler2D s_texture;\n"
	"uniform int   u_postMode;\n"
	"uniform vec2  u_texSize;\n"  /* (1/width, 1/height) of the source */
	"uniform float u_time;\n"
	"uniform int   u_tonemap;\n"
	"uniform float u_postIntensity;\n"
	"uniform float u_tmIntensity;\n"
	"uniform float u_brightness;\n"     /* image adjust, 1.0 = neutral */
	"uniform float u_contrast;\n"
	"uniform float u_saturation;\n"
	"uniform int   u_calibBar;\n"       /* 1 = overlay the reference bar (Brightness screen) */
	"float hash(vec2 p) {\n"
	"	p = fract(p * vec2(123.34, 456.21));\n"
	"	p += dot(p, p + 45.32);\n"
	"	return fract(p.x * p.y);\n"
	"}\n"
	/* Film grain. Time seeds the NOISE, it never moves it.
	 *
	 * Two ways to get this wrong, and the first version had one and my first fix
	 * had the other. hash(pixel + time) slides the field diagonally across the
	 * screen. Offsetting the lookup by a per-frame amount is the same mistake in
	 * steps: the pattern is spatially coherent, so it visibly jumps around rather
	 * than re-randomising. Time has to enter the hash as its own dimension, which
	 * is what this does -- every pixel keeps its position and only its value
	 * changes, which is what film grain actually looks like. */
	"float grainNoise(vec2 p, float t) {\n"
	"	vec3 q = fract(vec3(p.xy, t) * vec3(443.897, 441.423, 437.195));\n"
	"	q += dot(q, q.yzx + 19.19);\n"
	"	return fract((q.x + q.y) * q.z);\n"
	"}\n"
	"vec3 colorGrade(vec3 c) {\n"
	"	c = (c - 0.5) * 1.12 + 0.5;\n"                       /* contrast */
	"	float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
	"	c = mix(vec3(l), c, 1.15);\n"                        /* saturation */
	"	c *= vec3(1.06, 1.0, 0.94);\n"                       /* warm tint */
	"	return c;\n"
	"}\n"
	"vec2 curve(vec2 uv) {\n"
	"	uv = uv * 2.0 - 1.0;\n"
	"	vec2 o = abs(uv.yx) / vec2(6.0, 5.0);\n"
	"	uv += uv * o * o;\n"
	"	return uv * 0.5 + 0.5;\n"
	"}\n"
	"vec3 tonemap(vec3 c) {\n"
	"	if (u_tonemap == 1) { return c / (c + vec3(1.0)); }\n"                          /* Reinhard */
	"	if (u_tonemap == 2) {\n"                                                          /* ACES (Narkowicz) */
	"		c *= 0.6;\n"
	"		return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14), 0.0, 1.0);\n"
	"	}\n"
	"	if (u_tonemap == 3) {\n"                                                          /* Filmic (Hejl/Burgess) */
	"		vec3 x = max(vec3(0.0), c - 0.004);\n"
	"		return (x*(6.2*x+0.5))/(x*(6.2*x+1.7)+0.06);\n"
	"	}\n"
	"	return c;\n"
	"}\n"
	"void main() {\n"
	"	vec2 uv = v_uv;\n"
	"	vec3 col;\n"
	"	vec3 origCol = texture2D(s_texture, v_uv).rgb;\n"
	"	if (u_postMode == 1) {\n"                            /* CRT */
	"		uv = curve(uv);\n"
	"		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		col *= 0.75 + 0.25 * abs(sin(uv.y * 240.0 * 3.14159));\n"
	"		int m = int(mod(gl_FragCoord.x, 3.0));\n"
	"		vec3 mask = m == 0 ? vec3(1.0, 0.72, 0.72) : (m == 1 ? vec3(0.72, 1.0, 0.72) : vec3(0.72, 0.72, 1.0));\n"
	"		col *= mask * 1.25;\n"
	"		vec2 d = uv - 0.5; col *= clamp(1.0 - dot(d, d) * 1.1, 0.0, 1.0);\n"
	"	} else if (u_postMode == 2) {\n"                     /* Scanlines */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		col *= 0.7 + 0.3 * abs(sin(uv.y * 240.0 * 3.14159));\n"
	"	} else if (u_postMode == 3) {\n"                     /* Vignette */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		vec2 d = uv - 0.5; col *= clamp(1.0 - dot(d, d) * 1.3, 0.0, 1.0);\n"
	"	} else if (u_postMode == 4) {\n"                     /* Color grade */
	"		col = colorGrade(texture2D(s_texture, uv).rgb);\n"
	"	} else if (u_postMode == 5) {\n"                     /* Film grain */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		float n = grainNoise(gl_FragCoord.xy, u_time);\n"
	"		col += (n - 0.5) * 0.10;\n"
	"	} else if (u_postMode == 6) {\n"                     /* Sharpen */
	"		vec3 c = texture2D(s_texture, uv).rgb;\n"
	"		vec3 b = (texture2D(s_texture, uv + vec2(u_texSize.x, 0.0)).rgb\n"
	"		        + texture2D(s_texture, uv - vec2(u_texSize.x, 0.0)).rgb\n"
	"		        + texture2D(s_texture, uv + vec2(0.0, u_texSize.y)).rgb\n"
	"		        + texture2D(s_texture, uv - vec2(0.0, u_texSize.y)).rgb) * 0.25;\n"
	"		col = c + (c - b) * 0.85;\n"
	"	} else if (u_postMode == 7) {\n"                     /* PSX retro: downsample + dither + 5-bit */
	"		vec2 grid = vec2(320.0, 240.0);\n"
	"		vec2 quv = (floor(uv * grid) + 0.5) / grid;\n"
	"		col = texture2D(s_texture, quv).rgb;\n"
	"		mat4 dith = mat4(-4.0, 0.0, -3.0, 1.0, 2.0, -2.0, 3.0, -1.0, -3.0, 1.0, -4.0, 0.0, 3.0, -1.0, 2.0, -2.0) / 255.0;\n"
	"		ivec2 dc = ivec2(mod(gl_FragCoord.xy, 4.0));\n"
	"		col += vec3(dith[dc.x][dc.y]);\n"
	"		col = floor(col * 32.0 + 0.5) / 32.0;\n"
	"	} else if (u_postMode == 8) {\n"                     /* Cinematic: grade + vignette + grain */
	"		col = colorGrade(texture2D(s_texture, uv).rgb);\n"
	"		vec2 d = uv - 0.5; col *= clamp(1.0 - dot(d, d) * 0.9, 0.0, 1.0);\n"
	"		float n = grainNoise(gl_FragCoord.xy, u_time);\n"
	"		col += (n - 0.5) * 0.045;\n"
	"	} else {\n"                                          /* passthrough */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"	}\n"
	"	col = mix(origCol, col, u_postIntensity);\n"
	"	col = mix(col, tonemap(col), u_tmIntensity);\n"
	/* Reference bar (Brightness screen): a black->white ramp across the top with a
	 * primary-colour strip beneath it, so brightness/contrast/saturation changes
	 * are visible against a known target. Drawn BEFORE the adjustments so the bar
	 * is graded exactly like the game image. */
	"	if (u_calibBar == 1) {\n"
	"		if (uv.y < 0.09) { float g = clamp(uv.x, 0.0, 1.0); col = vec3(g); }\n"
	"		else if (uv.y < 0.15) {\n"
	"			float s = uv.x * 6.0;\n"
	"			if      (s < 1.0) col = vec3(1.0, 0.0, 0.0);\n"
	"			else if (s < 2.0) col = vec3(0.0, 1.0, 0.0);\n"
	"			else if (s < 3.0) col = vec3(0.0, 0.0, 1.0);\n"
	"			else if (s < 4.0) col = vec3(1.0, 1.0, 0.0);\n"
	"			else if (s < 5.0) col = vec3(0.0, 1.0, 1.0);\n"
	"			else              col = vec3(1.0, 0.0, 1.0);\n"
	"		}\n"
	"	}\n"
	/* Image adjustments (always applied; 1.0 = neutral each). Saturation toward
	 * Rec.601 luma, contrast around mid-grey, then brightness scale. */
	"	{\n"
	"		float luma = dot(col, vec3(0.299, 0.587, 0.114));\n"
	"		col = mix(vec3(luma), col, u_saturation);\n"
	"		col = (col - 0.5) * u_contrast + 0.5;\n"
	"		col *= u_brightness;\n"
	"	}\n"
	"	fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
	"}\n"
	"#endif\n";

void GR_InitPostProcess(void)
{
	if (g_postShader != (ShaderID)-1)
		return;

	g_postShader = GR_Shader_Compile(s_postShaderSrc);
	g_postLoc_mode    = glGetUniformLocation(g_postShader, "u_postMode");
	g_postLoc_texSize = glGetUniformLocation(g_postShader, "u_texSize");
	g_postLoc_time    = glGetUniformLocation(g_postShader, "u_time");
	g_postLoc_tonemap = glGetUniformLocation(g_postShader, "u_tonemap");
	g_postLoc_postInt = glGetUniformLocation(g_postShader, "u_postIntensity");
	g_postLoc_tmInt   = glGetUniformLocation(g_postShader, "u_tmIntensity");
	g_postLoc_bright   = glGetUniformLocation(g_postShader, "u_brightness");
	g_postLoc_contrast = glGetUniformLocation(g_postShader, "u_contrast");
	g_postLoc_satur    = glGetUniformLocation(g_postShader, "u_saturation");
	g_postLoc_calib    = glGetUniformLocation(g_postShader, "u_calibBar");

	glGenVertexArrays(1, &g_postVAO);
}

static void GR_EnsurePostTarget(int w, int h)
{
	if (g_postTex != (TextureID)-1 && g_postW == w && g_postH == h)
		return;

	if (g_postTex == (TextureID)-1)
	{
		glGenTextures(1, &g_postTex);
		glGenFramebuffers(1, &g_postFBO);
	}

	g_postW = w;
	g_postH = h;

	glBindTexture(GL_TEXTURE_2D, g_postTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, g_postFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_postTex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
}

/* Draw a full-screen triangle sampling `tex` into the currently bound default
 * framebuffer, applying post mode `mode` (0 = straight copy). Disables depth /
 * blend / scissor / stencil for the draw, then invalidates the renderer's
 * cached GL state so the next frame's prims re-establish it. */
static void GR_DrawFullscreenTexture(TextureID tex, int mode)
{
	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
	glViewport(0, 0, g_windowWidth, g_windowHeight);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);

	glUseProgram(g_postShader);
	if (g_postLoc_mode != -1)
		glUniform1i(g_postLoc_mode, mode);
	if (g_postLoc_texSize != -1)
		glUniform2f(g_postLoc_texSize,
		            g_windowWidth  > 0 ? 1.0f / (float)g_windowWidth  : 0.0f,
		            g_windowHeight > 0 ? 1.0f / (float)g_windowHeight : 0.0f);
	if (g_postLoc_time != -1)
		glUniform1f(g_postLoc_time, (float)(g_postFrame & 1023));
	if (g_postLoc_tonemap != -1)
		glUniform1i(g_postLoc_tonemap, g_cfg_tonemap);
	if (g_postLoc_postInt != -1)
		glUniform1f(g_postLoc_postInt, g_cfg_postProcessIntensity);
	if (g_postLoc_tmInt != -1)
		glUniform1f(g_postLoc_tmInt, g_cfg_tonemapIntensity);
	/* Out of scope = pass neutral rather than skip the pass, since a look filter
	 * or the tonemap may still need it this frame. */
	{
		const int   adj = GR_ImageAdjustActive();
		const float br  = adj ? g_cfg_brightness : 1.0f;
		const float ct  = adj ? g_cfg_contrast   : 1.0f;
		const float sa  = adj ? g_cfg_saturation : 1.0f;

		if (g_postLoc_bright != -1)
			glUniform1f(g_postLoc_bright, br);
		if (g_postLoc_contrast != -1)
			glUniform1f(g_postLoc_contrast, ct);
		if (g_postLoc_satur != -1)
			glUniform1f(g_postLoc_satur, sa);
	}
	if (g_postLoc_calib != -1)
		glUniform1i(g_postLoc_calib, g_cfg_calibBar);

	glBindTexture(GL_TEXTURE_2D, tex);
	glBindVertexArray(g_postVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glEnable(GL_STENCIL_TEST);

	/* Sentinels, NOT the real state. Recording the truth (BM_NONE/0) is what
	 * strands depth off: GR_EnableDepth is reachable only from inside
	 * GR_SetBlendMode's body, and that early-returns on an unchanged mode, so a
	 * following opaque draw matches BM_NONE, skips the body, and never re-applies
	 * the depth test this function just disabled. Cost the take-screen item its
	 * depth test entirely (antenna through the radio body), because the take
	 * screen composites over the frozen scene through this path while the
	 * inventory does not. Same reasoning as GR_ShadowPassEnd. */
	g_PreviousShader      = (ShaderID)-1;
	g_lastBoundTexture    = (TextureID)-1;
	g_PreviousBlendMode   = -999;
	g_PreviousDepthMode   = -999;
	g_PreviousScissorState = -999;
}

/* PC port: post-process the composed backbuffer in place. Resolves the (possibly
 * multisample) default framebuffer into a single-sample texture, then redraws it
 * full-screen through the selected look. No-op when g_cfg_postProcess <= 0. */
void GR_PostProcess(void)
{
	/* Also run when an image adjustment is off-neutral or the calibration bar is
	 * requested, so brightness/contrast/saturation apply with no filter selected. */
	const int bcsActive = GR_ImageAdjustActive() &&
	                      ((g_cfg_brightness != 1.0f) || (g_cfg_contrast != 1.0f) ||
	                       (g_cfg_saturation != 1.0f) || (g_cfg_calibBar != 0));
	if (g_cfg_postProcess <= 0 && g_cfg_tonemap <= 0 && !bcsActive)
		return;
	if (g_postShader == (ShaderID)-1)
		GR_InitPostProcess();

	GR_EnsurePostTarget(g_windowWidth, g_windowHeight);

	/* Resolve/copy backbuffer -> single-sample source texture (same size, so
	 * this is a legal multisample resolve when MSAA is on). */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_postFBO);
	glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, 0, g_postW, g_postH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());

	g_postFrame++;
	GR_DrawFullscreenTexture(g_postTex, g_cfg_postProcess);
}

#else  /* !PSYX_HAS_POSTPROCESS */
void GR_InitPostProcess(void) {}
void GR_PostProcess(void) {}
#endif

/* ------------------------------------------------------------------------
 * Flashlight shadow mapping (see g_PsyX_UseFlashlightShadows).
 *
 * Everything is done in the engine's CAMERA VIEW space: GrVertex carries
 * a_viewpos (view space) and the flashlight pos/dir are already pushed in
 * view space, so the light matrix is built purely from those with no
 * world-space / camera-inverse step. Each frame the opaque geometry is
 * rendered depth-only from the light POV into g_shadowDepthTex; the cone
 * fragment shader samples it. Feature is a no-op (byte-identical output)
 * when the master flag is off.
 * ---------------------------------------------------------------------- */
#if USE_OPENGL

static const char* s_shadowDepthShaderSrc =
	"#ifdef VERTEX\n"
	"attribute vec3 a_viewpos;\n"
	"attribute vec3 a_normal;\n"
	"uniform mat4 u_shadowMatrix;\n"
	"void main() {\n"
	/* a_normal.y marks a validated view-space entry; a_normal.x suppresses casting. */
	"	if (a_normal.y < 0.5 || a_viewpos.z <= 0.0 || a_normal.x > 0.5) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }\n"
	"	gl_Position = u_shadowMatrix * vec4(a_viewpos, 1.0);\n"
	"}\n"
	"#else\n"
	"void main() { }\n"
	"#endif\n";

static void sh_normalize3(float* v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static float sh_dot3(const float* a, const float* b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void sh_cross(const float* a, const float* b, float* r)
{
	r[0] = a[1] * b[2] - a[2] * b[1];
	r[1] = a[2] * b[0] - a[0] * b[2];
	r[2] = a[0] * b[1] - a[1] * b[0];
}

/* Column-major (GL) perspective + look-at + multiply. */
static void sh_perspective(float fovy, float aspect, float zn, float zf, float* m)
{
	float f = 1.0f / tanf(fovy * 0.5f);
	for (int i = 0; i < 16; i++) m[i] = 0.0f;
	m[0]  = f / aspect;
	m[5]  = f;
	m[10] = (zf + zn) / (zn - zf);
	m[11] = -1.0f;
	m[14] = (2.0f * zf * zn) / (zn - zf);
}

static void sh_lookat(const float* eye, const float* center, const float* up, float* m)
{
	float f[3] = { center[0] - eye[0], center[1] - eye[1], center[2] - eye[2] };
	sh_normalize3(f);
	float s[3]; sh_cross(f, up, s); sh_normalize3(s);
	float u[3]; sh_cross(s, f, u);
	m[0] = s[0];  m[4] = s[1];  m[8]  = s[2];  m[12] = -sh_dot3(s, eye);
	m[1] = u[0];  m[5] = u[1];  m[9]  = u[2];  m[13] = -sh_dot3(u, eye);
	m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = sh_dot3(f, eye);
	m[3] = 0.0f;  m[7] = 0.0f;  m[11] = 0.0f;  m[15] = 1.0f;
}

static void sh_mul(const float* a, const float* b, float* r)  /* r = a * b */
{
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++)
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] +
			                 a[1 * 4 + row] * b[c * 4 + 1] +
			                 a[2 * 4 + row] * b[c * 4 + 2] +
			                 a[3 * 4 + row] * b[c * 4 + 3];
}

static void GR_EnsureShadowTarget(void)
{
	/* Clamp once, so a bad config value cannot ask the driver for something
	 * absurd. 8192 is above ES 3.0's guaranteed minimum (4096), so sizes past
	 * that are additionally capped to the driver's real limit -- a GPU that
	 * can't do it gets the biggest map it can instead of an incomplete FBO. */
	if (g_PsyX_ShadowMapSize < 256)   g_PsyX_ShadowMapSize = 256;
	if (g_PsyX_ShadowMapSize > 8192)  g_PsyX_ShadowMapSize = 8192;
	if (g_PsyX_ShadowMapSize > 4096)
	{
		static GLint s_maxTexSize = 0;
		if (s_maxTexSize == 0)
			glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s_maxTexSize);
		if (s_maxTexSize > 0 && g_PsyX_ShadowMapSize > s_maxTexSize)
		{
			eprintf("*shadow map %d exceeds GL_MAX_TEXTURE_SIZE %d, clamping\n",
			        g_PsyX_ShadowMapSize, (int)s_maxTexSize);
			g_PsyX_ShadowMapSize = s_maxTexSize;
		}
	}

	/* Resolution changed at runtime: drop the old target and build a new one. */
	if (g_shadowFBO != 0 && s_shadowTexSize != g_PsyX_ShadowMapSize)
	{
		glDeleteFramebuffers(1, &g_shadowFBO);
		glDeleteTextures(1, &g_shadowDepthTex);
		g_shadowFBO      = 0;
		g_shadowDepthTex = 0;
	}

	if (g_shadowFBO != 0)
		return;

	s_shadowTexSize = g_PsyX_ShadowMapSize;

	glGenTextures(1, &g_shadowDepthTex);
	glBindTexture(GL_TEXTURE_2D, g_shadowDepthTex);
	/* ES pairs DEPTH_COMPONENT24 with UNSIGNED_INT only -- FLOAT is legal solely
	 * with DEPTH_COMPONENT32F (ES 3.0 table 3.2). Desktop GL tolerates the
	 * mismatch for a NULL upload, ANGLE raises INVALID_OPERATION and the texture
	 * is never allocated: the FBO's depth attachment is incomplete, the pre-pass
	 * writes nothing, and every receiver then samples as occluded. That zeroes
	 * the cone while the whole-scene dim above it still lands, which is why only
	 * the shadow flashlight modes went dark and Classic/Modern were fine. */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, PSYX_SHADOW_MAP_SIZE, PSYX_SHADOW_MAP_SIZE,
	             0, GL_DEPTH_COMPONENT, g_grIsGLES ? GL_UNSIGNED_INT : GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	/* CLAMP_TO_BORDER and the border colour are desktop GL / ES 3.2; ES 3.0 has
	 * neither. CLAMP_TO_EDGE is a safe stand-in here because the fragment shader
	 * already rejects receivers outside 0..1 in the light frustum before it ever
	 * samples, so the border texels are never observed. */
	if (!g_grIsGLES)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  /* outside the light frustum = fully lit */
		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &g_shadowFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, g_shadowFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_shadowDepthTex, 0);
	/* Depth-only target. ES 3.0 dropped the singular glDrawBuffer in favour of
	 * the array form, which takes the same GL_NONE. */
	if (g_grCaps.drawBuffer)
		glDrawBuffer(GL_NONE);
	else
	{
		const GLenum none = GL_NONE;
		glDrawBuffers(1, &none);
	}
	glReadBuffer(GL_NONE);

	/* An incomplete shadow FBO does not fail loudly -- it just makes every
	 * receiver read as shadowed. Say so once instead. */
	{
		GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);

		if (st != GL_FRAMEBUFFER_COMPLETE)
		{
			eprinterr("shadow FBO incomplete: 0x%04X (flashlight shadows will read as fully occluded)\n", st);
		}
		else
		{
			eprintf("*shadow target ready: %dx%d depth24\n", PSYX_SHADOW_MAP_SIZE, PSYX_SHADOW_MAP_SIZE);
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());

	if (g_shadowDepthShader == (ShaderID)-1)
	{
		g_shadowDepthShader = GR_Shader_Compile(s_shadowDepthShaderSrc);
		g_shadowDepthMatrixLoc = glGetUniformLocation(g_shadowDepthShader, "u_shadowMatrix");
	}
}

static void GR_BuildShadowMatrix(void)
{
	float sizeActive = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightSizeFps : g_PsyX_FlashlightSize;
	float flOuter = 1.0f - sizeActive * (1.0f - g_PsyX_FlashlightOuterCos);
	if (flOuter < 0.05f)  flOuter = 0.05f;
	if (flOuter > 0.999f) flOuter = 0.999f;

	float fov = acosf(flOuter) * 2.0f * 1.25f;  /* widen past the cone so edge shadows aren't clipped */
	if (fov > 2.9f) fov = 2.9f;
	if (fov < 0.2f) fov = 0.2f;

	float zn = 20.0f;
	float zf = g_PsyX_FlashlightRange * 1.3f;
	if (zf < zn + 1.0f) zf = zn + 1.0f;
	g_shadowZNear = zn;
	g_shadowZFar  = zf;

	/* FPS pins the CONE at the eye (so the beam follows the view), but the shadow
	 * must originate at the REAL light (chest/hand) — else the depth map is the
	 * camera's own view and the shadow either vanishes or floats beside the object.
	 * g_PsyX_FlashlightShadowPos carries that true light position (== FlashlightPos
	 * in TPS). */
	const float* srcPos = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightShadowPos : g_PsyX_FlashlightPos;
	float eye[3] = { srcPos[0], srcPos[1], srcPos[2] };
	float dir[3] = { g_PsyX_FlashlightDir[0], g_PsyX_FlashlightDir[1], g_PsyX_FlashlightDir[2] };
	sh_normalize3(dir);
	/* Optional fine-tune: nudge the shadow light back along -viewDir. Default 0
	 * (pure physical light position); `shadowfpsdrop` lets the user dial extra
	 * parallax if a scene wants it. */
	if (g_PsyX_FlashlightFpsMode && g_PsyX_FlashlightShadowFpsDrop != 0.0f)
	{
		eye[0] -= dir[0] * g_PsyX_FlashlightShadowFpsDrop;
		eye[1] -= dir[1] * g_PsyX_FlashlightShadowFpsDrop;
		eye[2] -= dir[2] * g_PsyX_FlashlightShadowFpsDrop;
	}
	float center[3] = { eye[0] + dir[0], eye[1] + dir[1], eye[2] + dir[2] };

	float up[3] = { 0.0f, 1.0f, 0.0f };
	if (fabsf(sh_dot3(dir, up)) > 0.99f) { up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f; }

	float proj[16], view[16];
	sh_perspective(fov, 1.0f, zn, zf, proj);
	sh_lookat(eye, center, up, view);
	sh_mul(proj, view, g_shadowLightMatrix);
}

int GR_FlashlightShadowActive(void)
{
	/* g_PsyX_ShadowsAllowed is re-armed by the game only during settled gameplay
	 * (see its definition). Outside that — menus, room-load fades, cutscenes and
	 * frozen/transition frames (g_PsxPresentLastFrame) — the light-POV depth
	 * pre-pass corrupts unrelated rendering (white flash on room/inventory/map
	 * transitions, Harry's face dropping out on the options screen). Shadows are a
	 * live-gameplay-only effect. */
	return (g_PsyX_UseFlashlightShadows && g_PsyX_UsePerPixelFlashlight &&
	        g_PsyX_FlashlightActive && g_PsyX_ShadowsAllowed &&
	        !g_PsxPresentLastFrame) ? 1 : 0;
}

static GLint s_shadowPrevFBO = 0;
static GLint s_shadowPrevViewport[4] = { 0, 0, 0, 0 };

void GR_ShadowPassBegin(void)
{
	GR_EnsureShadowTarget();
	GR_BuildShadowMatrix();

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_shadowPrevFBO);
	glGetIntegerv(GL_VIEWPORT, s_shadowPrevViewport);

	glBindFramebuffer(GL_FRAMEBUFFER, g_shadowFBO);
	glViewport(0, 0, PSYX_SHADOW_MAP_SIZE, PSYX_SHADOW_MAP_SIZE);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glClear(GL_DEPTH_BUFFER_BIT);

	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(1.5f, 1.0f);

	glUseProgram(g_shadowDepthShader);
	if (g_shadowDepthMatrixLoc != -1)
		glUniformMatrix4fv(g_shadowDepthMatrixLoc, 1, GL_FALSE, g_shadowLightMatrix);
}

void GR_ShadowPassDraw(int startVertex, int numVerts)
{
	glDrawArrays(GL_TRIANGLES, startVertex, numVerts);
}

void GR_ShadowPassEnd(void)
{
	glDisable(GL_POLYGON_OFFSET_FILL);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s_shadowPrevFBO);
	glViewport(s_shadowPrevViewport[0], s_shadowPrevViewport[1],
	           s_shadowPrevViewport[2], s_shadowPrevViewport[3]);
	glDepthFunc(GL_LEQUAL);  /* restore the renderer default; the pass set GL_LESS */

	/* We changed program/depth/blend/scissor/stencil. Sentinels that never equal a
	 * real mode force the first color split to fully re-establish GL state. */
	glUseProgram(0);
	g_PreviousShader       = (ShaderID)-1;
	g_lastBoundTexture     = (TextureID)-1;
	g_PreviousBlendMode    = -999;
	g_PreviousDepthMode    = -999;
	g_PreviousDepthFuncAlways = 0; /* this fn just set glDepthFunc(GL_LEQUAL) */
	g_PreviousStencilMode  = -999;
	g_PreviousScissorState = -999;
	glEnable(GL_STENCIL_TEST);
}

#else  /* !USE_OPENGL */
int  GR_FlashlightShadowActive(void) { return 0; }
void GR_ShadowPassBegin(void) {}
void GR_ShadowPassDraw(int startVertex, int numVerts) { (void)startVertex; (void)numVerts; }
void GR_ShadowPassEnd(void) {}
#endif

/* See g_PsxPresentLastFrame above. Called from PsyX_EndScene after the
 * frame is fully composed in the backbuffer, before the swap. */
void GR_CaptureLastFrame(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	/* A frame that re-presented the capture must not be re-captured,
	 * or the UI text drawn on top would bake into the frozen image. */
	if (g_freezePresentedThisFrame)
	{
		g_freezePresentedThisFrame = 0;
		return;
	}

	if (!g_freezeFrameTex)
	{
		glGenTextures(1, &g_freezeFrameTex);
		glGenFramebuffers(1, &g_freezeFrameFBO);
	}

	if (g_freezeFrameW != g_windowWidth || g_freezeFrameH != g_windowHeight)
	{
		g_freezeFrameW = g_windowWidth;
		g_freezeFrameH = g_windowHeight;
		glBindTexture(GL_TEXTURE_2D, g_freezeFrameTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_freezeFrameW, g_freezeFrameH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, 0);
		g_freezeFrameValid = 0;
	}

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_freezeFrameFBO);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_freezeFrameTex, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());

	glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight,
		0, 0, g_freezeFrameW, g_freezeFrameH,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());

	g_freezeFrameValid = 1;
#endif
}

/* Called from PsyX_BeginScene right after the frame clear while the game
 * holds g_PsxPresentLastFrame: re-presents the captured frame so this
 * frame's prims (PAUSED text, console, messages) draw on top of it. */
void GR_PresentLastFrame(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	if (!g_freezeFrameValid)
		return;

#if PSYX_HAS_POSTPROCESS
	/* MSAA: the default framebuffer is multisample, and a single-sample ->
	 * multisample glBlitFramebuffer is illegal. Draw the captured frame as a
	 * full-screen textured triangle instead (writing into the multisample FBO
	 * is fine). */
	if (g_cfg_msaaSamples > 0 && g_postShader != (ShaderID)-1)
	{
		GR_DrawFullscreenTexture(g_freezeFrameTex, 0);
		g_freezePresentedThisFrame = 1;
		return;
	}
#endif

	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_freezeFrameFBO);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());

	glBlitFramebuffer(0, 0, g_freezeFrameW, g_freezeFrameH,
		0, 0, g_windowWidth, g_windowHeight,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());

	g_freezePresentedThisFrame = 1;
#endif
}

/* PC port: some cutscenes (map4_s04 Lisa dream, map3_s02 sibling) redirect the
 * PSX draw-area into an offscreen VRAM scratch rect (e.g. (320,256) 320x224),
 * render the frame THERE, and repaint the visible screen from it via 4
 * blend-layer SPRT strips — the dream-blur effect. GL never rasterizes into
 * VRAM, so on PC those strips sampled whatever TIMs happened to occupy the
 * scratch pages: a rainbow noise band, or a whole character atlas blown up on
 * screen, varying run to run with VRAM contents. ProcessDrawEnv latches the
 * redirect rect when a DR_AREA targets x >= 320 (display buffer columns are
 * always x < 320); while latched, the stored frame is blitted into the scratch
 * rect of g_vramTexture each present (and after every full VRAM re-upload), so
 * the strips composite the previous frame — the effect's actual source on PSX.
 * By PSX construction nothing else can live in the scene's scratch rect while
 * the scene runs, so the blit cannot clobber a live texture. */
static RECT16 g_sceneFbRedirect = { 0, 0, 0, 0 };
static int    g_sceneFbRedirectTtl = 0;
static int    s_sceneFbRedirectArms = 0;

/* Does a 16bpp tpage at VRAM (x, y) sample the live scene-scratch rect? Those
 * are the soft-focus composite strips (map3_s02 Alessa, map4_s04 Lisa,
 * map7_s02): widened with the world ortho, and dropped when dream_blur is off. */
extern "C" int GR_SceneFbRedirectCovers(int x, int y)
{
	return g_sceneFbRedirectTtl > 0 &&
	       x >= g_sceneFbRedirect.x && x < g_sceneFbRedirect.x + g_sceneFbRedirect.w &&
	       y >= g_sceneFbRedirect.y && y < g_sceneFbRedirect.y + g_sceneFbRedirect.h;
}

extern "C" void GR_SetSceneFbRedirect(int x, int y, int w, int h)
{
	g_sceneFbRedirect.x = x;
	g_sceneFbRedirect.y = y;
	g_sceneFbRedirect.w = w;
	g_sceneFbRedirect.h = h;
	/* Refreshed every frame the scene submits its DR_AREA; a small TTL lets
	 * the blit die out a couple of presents after the scene stops. */
	/* Log every ARM-after-lapse, not just the first ever. The recurring cutscene
	 * rainbow bar is the strips sampling this rect while the blit is NOT live,
	 * and a once-per-session line cannot tell those runs apart: it proves the
	 * mechanism exists, never that it was active during the scene that
	 * corrupted. Paired with the lapse line below, a user log now says which.
	 * Rate-capped so a scene re-arming every frame cannot flood. */
	if (g_sceneFbRedirectTtl == 0 && s_sceneFbRedirectArms < 32)
	{
		s_sceneFbRedirectArms++;
		eprintinfo("[FBSCRATCH] redirect ARMED (%d,%d %dx%d) - feedback blit live\n", x, y, w, h);
	}

	g_sceneFbRedirectTtl = 3;
}

/* ===================== framebuffer feedback (packed RGB555) =================
 * Silent Hill DOES read rendered pixels back from VRAM: Screen_BackgroundMotionBlur
 * (the Harry-running loading-screen trail, every frame from GameBoot_LoadingScreen)
 * and the per-map ghosting/dream overlays all sample getTPage(2, 0, ...) display
 * pages. Those pages must therefore hold the previous frame.
 *
 * The catch that broke the first attempt at this: VRAM is a GL_RG8 texture whose
 * two channels hold the packed BYTES of a 16-bit PSX pixel — the sampling shader
 * decodes them by indexing s_rgLut with (high byte, low byte), a table baked as
 * bit 0-4 = R, 5-9 = G, 10-14 = B, 15 = mask; R channel = low byte, G = high.
 * A raw glBlitFramebuffer from the RGBA8 frame therefore CANNOT work: GL drops B/A
 * and keeps the R,G *colour* bytes, which the shader then decodes as a bit-packed
 * 555 word — saturated garbage stripes. (The CPU helper GR_CopyRGBAFramebufferToVRAM
 * packs R and B the wrong way round against this shader, so it is no use either.)
 *
 * So the store is a shader pass that packs RGBA8 -> RGB555 into RG, rendered
 * straight into the VRAM texture. Mask bit is left 0, so a pure-black source pixel
 * packs to word 0 and the sampler's zero-texel discard treats it as
 * transparent — exactly PSX texel-0 behaviour, which is what makes the loading
 * screen show a trail of Harry rather than an opaque black rectangle. */
/* Feedback-loop gain, pushed to the pack shader every store. 0.5 is the shipped
 * steady-state value; the door out-fade wants it near identity. Console: FBDAMP. */
extern "C" { float g_PsxFeedbackDamp = 0.7f; } /* loading-trail ghost strength, matched by eye to a real PS1 */
/* Loop gain for a BLENDING reader (the per-map dream overlays). Unity: the
 * overlay's own 50/50 composite is the decay. Console FBDAMP takes it as a
 * second argument. */
extern "C" { float g_PsxFeedbackDampBlend = 1.0f; }

static ShaderID g_fbPackShader = (ShaderID)-1;
static GLuint   g_fbPackVAO = 0;
static GLuint   g_fbPackTex = 0;   /* captured frame, RGBA8 */
static GLuint   g_fbPackFBO = 0;
static int      g_fbPackW = 0, g_fbPackH = 0;
static int      g_fbPackValid = 0; /* a frame has been captured this session */
/* Set around the scene-scratch capture while the world ortho is widened. */
static int      g_fbCaptureWorldWide = 0;

/* PSX display-buffer rects recorded from GsDefDispBuff2 (SH: (0,32)/(0,256),
 * 320x224). The PC libgs stub collapses both display envs to (0,0) because there
 * is no VRAM double-buffering here, so activeDispEnv.disp is NOT a usable store
 * target — it would land on the CLUT strip at y<32 (paper map at (224,15)). */
static RECT16 g_psxDispBuf[2] = { {0,0,0,0}, {0,0,0,0} };
static int    g_psxDispBufValid = 0;

/* Frames remaining for which the feedback store stands down because the game
 * uploaded its own data into a display-buffer rect (see GR_CopyVRAM). A few
 * frames of hysteresis so a scene that uploads intermittently doesn't flicker
 * the effect on between uploads. */
static int g_fbFeedbackSuppress = 0;

void GR_NoteVramUploadForFeedback(int x, int y, int w, int h)
{
	int i;

	if (!g_psxDispBufValid)
		return;

	for (i = 0; i < 2; i++)
	{
		const RECT16* b = &g_psxDispBuf[i];
		if (b->w <= 0 || b->h <= 0)
			continue;
		/* rect overlap */
		if (x < b->x + b->w && x + w > b->x &&
		    y < b->y + b->h && y + h > b->y)
		{
			static int s_logged = 0;
			if (!s_logged) {
				s_logged = 1;
				eprintinfo("[FBFEEDBACK] VRAM upload (%d,%d %dx%d) lands in display buffer %d "
					"(%d,%d %dx%d) — feedback store standing down\n",
					x, y, w, h, i, b->x, b->y, b->w, b->h);
			}
			g_fbFeedbackSuppress = 4;
			return;
		}
	}
}

extern "C" void GR_SetPsxDisplayBuffers(int x0, int y0, int x1, int y1, int w, int h)
{
	int gap;
	g_psxDispBuf[0].x = x0; g_psxDispBuf[0].y = y0;
	g_psxDispBuf[1].x = x1; g_psxDispBuf[1].y = y1;

	/* Clamp so buffer 0 can never spill into buffer 1's rows (SH's are exactly
	 * adjacent: 32 + 224 == 256). */
	gap = (y1 > y0) ? (y1 - y0) : h;
	if (gap > 0 && h > gap)
		h = gap;

	g_psxDispBuf[0].w = g_psxDispBuf[1].w = w;
	g_psxDispBuf[0].h = g_psxDispBuf[1].h = h;
	g_psxDispBufValid = (w > 0 && h > 0);
}

static const char* s_fbPackShaderSrc =
	"varying vec2 v_uv;\n"
	"#ifdef VERTEX\n"
	"void main() {\n"
	"	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	v_uv = (p + 1.0) * 0.5;\n"
	/* The capture below is an unflipped window->texture blit, so texture row 0 is
	 * the screen BOTTOM. VRAM rows run downward with screen rows (the game samples
	 * buffer 0 from v=32 at the top of the screen), and viewport y=0 is the rect's
	 * first VRAM row — so the first VRAM row must receive the screen TOP. Flip. */
	"	v_uv.y = 1.0 - v_uv.y;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n"
	"#else\n"
	"uniform sampler2D s_texture;\n"
	/* Feedback gain. The game's blur SPRT is OPAQUE (code 0x64, no semi-trans bit)
	 * and modulates at 128/128 = identity, or 127/128 on the odd frame, so on PSX
	 * the loop is stored' = ~0.992*stored with new geometry overwriting on top:
	 * the last frame FREEZES on screen through a door load and Harry's running
	 * pose leaves a slow-decaying trail. Identity here reproduces that exactly.
	 *
	 * This used to be 0.5 to kill "an overexposed pile of ghosts smearing
	 * sideways". That pile was not a gain problem — it was the capture reading a
	 * different rect than the game redraws (see GR_StoreFrameBufferPsx), so each
	 * generation rescaled and piled onto itself. With the capture rect fixed the
	 * loop is stable at identity, and 0.5 would be wrong twice over: it fades a
	 * frame PSX holds, and each halving re-quantizes to 5 bits at an ever lower
	 * level, which is what turned the ghost into saturated colour blotches.
	 *
	 * Identity gain was tried (255/256, cancelling our (mod/255)*2 modulation
	 * against PSX's mod/128) and is WRONG here, because the two problems were
	 * independent: PSX can run at identity only because GsDefDispBuff2 CLEARS its
	 * draw buffer every frame (isbg=1), so "stored" is replaced. We have no VRAM
	 * double-buffer to clear, so stored accumulates and a unity loop diverges —
	 * it converged to a flat mid-grey field over the 4:3 core for a whole door
	 * load. Fixing the capture rect removed the rescale, not the accumulation.
	 *
	 * So keep damping the loop to the same steady state hardware reaches:
	 * stored = k*(0.992*stored + frame) settles at k/(1 - 0.992k), and k = 0.5
	 * gives ~1.0x — one frame's worth of ghost. Lower this if the trail reads too
	 * strong; do NOT raise it toward 1.0 without first clearing the destination. */
	/* Runtime-tunable (g_PsxFeedbackDamp, console FBDAMP) so the value can be
	 * found in-game rather than by rebuilds. The door out-fade wants a gain near
	 * identity so the held frame decays over ~1s like retail, while the
	 * Harry-running loading screen composites new geometry every frame and needs
	 * damping to reach a steady state -- one constant cannot serve both. Raising
	 * it toward 1.0 without first neutralising the blur SPRT's 2x modulation is
	 * what produced the flat mid-grey field recorded above. */
	"	uniform float u_feedbackDamp;\n"
	/* PSX mask bit. A 16bpp texel carries it as bit 15, and it is what decides
	 * whether a SEMI-TRANSPARENT primitive blends that texel or draws it solid.
	 * The per-map dream overlays are semi-transparent prims (RECT_BLEND) and
	 * expect the stored frame to blend 50/50 with the live one; packing without
	 * the bit made every texel read opaque, so the overlay replaced the frame
	 * instead of ghosting over it. The loading trail is an OPAQUE prim and must
	 * pack WITHOUT it: its black would otherwise become opaque black (0x8000)
	 * rather than the transparent word 0 the sampler discards, and the trail
	 * would be a black rectangle. Word 0 stays word 0 either way. */
	"	uniform float u_packMaskBit;\n"
	/* EXACT loop, for an OPAQUE reader (the loading trail and door fade).
	 *
	 * The level each pixel was stored at is recovered exactly -- the LUT decodes
	 * a level as L*8, and the shader's mod*2/255 modulation moves that by under
	 * half a step -- so the round trip through the 320x224 store is lossless,
	 * with the NEAREST capture keeping it pixel-sharp. The old /31 requantize did
	 * not match that decode and a LINEAR capture smeared every pass.
	 *
	 * Persistence is then set by the gain, u_feedbackDamp (FBDAMP, 0.7): each
	 * pass keeps that fraction of the level. It is NOT unity. Real hardware shows
	 * a faint ghost on the hands and feet only while Harry jogs in place at
	 * normal speed -- a short-lived ghost, which only shows where the pose moves
	 * most. A unity copy with the 127/128 one-step fade keeps every limb position
	 * for about a second and smears the whole body sideways; that was tried and
	 * rejected against a real PS1. The one-step fade on the game's 127 frames
	 * still applies on top. */
	"	uniform float u_packExact;\n"
	"	uniform float u_packDecay;\n"
	"void main() {\n"
	"	vec3 src = texture2D(s_texture, v_uv).rgb;\n"
	"	if (u_packExact > 0.5) {\n"
	"		vec3 L = clamp(floor(src * 31.875 + 0.5), 0.0, 31.0);\n"
	"		L = floor(L * u_feedbackDamp + 0.001);\n"
	"		L = max(L - vec3(u_packDecay), vec3(0.0));\n"
	"		float e16 = L.r + L.g * 32.0 + L.b * 1024.0;\n"
	"		if (u_packMaskBit > 0.5 && e16 > 0.0) e16 += 32768.0;\n"
	"		float ehi = floor(e16 / 256.0);\n"
	"		float elo = e16 - ehi * 256.0;\n"
	"		fragColor = vec4(elo / 255.0, ehi / 255.0, 0.0, 1.0);\n"
	"		return;\n"
	"	}\n"
	"	vec3 c = src * u_feedbackDamp;\n"
	/* TRUNCATE, do not round. Retail's decay does not come from the gain -- at
	 * 127/128 over ~60 frames the frame would only reach ~0.61 -- it comes from
	 * this requantize dropping exactly one 5-bit level per pass, 31 passes to
	 * black. Rounding maps a damped level back onto itself, so the loop stalls
	 * and the frame freezes and smears instead of fading (what the Xbox port
	 * does). Inert at the shipped damp of 0.5, where the value halves regardless. */
	"	float r5 = floor(c.r * 31.0 + 0.002);\n"
	"	float g5 = floor(c.g * 31.0 + 0.002);\n"
	"	float b5 = floor(c.b * 31.0 + 0.002);\n"
	"	float w16 = r5 + g5 * 32.0 + b5 * 1024.0;\n"
	"	if (u_packMaskBit > 0.5 && w16 > 0.0) w16 += 32768.0;\n"
	"	float hi  = floor(w16 / 256.0);\n"
	"	float lo  = w16 - hi * 256.0;\n"
	"	fragColor = vec4(lo / 255.0, hi / 255.0, 0.0, 1.0);\n"
	"}\n"
	"#endif\n";

static void GR_EnsureFbPackTarget(int w, int h)
{
	if (g_fbPackShader == (ShaderID)-1)
	{
		g_fbPackShader = GR_Shader_Compile(s_fbPackShaderSrc);
		glUseProgram(g_fbPackShader);
		{
			GLint loc = glGetUniformLocation(g_fbPackShader, "s_texture");
			if (loc != -1) glUniform1i(loc, 0);
		}
		glUseProgram(0);
		glGenVertexArrays(1, &g_fbPackVAO);
		glGenTextures(1, &g_fbPackTex);
		glGenFramebuffers(1, &g_fbPackFBO);
	}

	if (g_fbPackW == w && g_fbPackH == h)
		return;

	g_fbPackW = w;
	g_fbPackH = h;

	glBindTexture(GL_TEXTURE_2D, g_fbPackTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, g_fbPackFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbPackTex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
}

/* Pack the captured frame into one VRAM rect. Saves/restores viewport + FBO and
 * invalidates the renderer's cached GL state, so this is safe to run mid-frame
 * (GR_UpdateVRAM calls it after a full vram[] re-upload). */
static void GR_PackFrameToVramRectGain(int x, int y, int w, int h, float gain)
{
#if USE_OPENGL
	GLint vp[4];

	if (!g_fbPackValid || w <= 0 || h <= 0)
		return;

	glGetIntegerv(GL_VIEWPORT, vp);

	glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);
	glViewport(x, y, w, h);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);

	glUseProgram(g_fbPackShader);
	{
		const GLint dampLoc  = glGetUniformLocation(g_fbPackShader, "u_feedbackDamp");
		const GLint maskLoc  = glGetUniformLocation(g_fbPackShader, "u_packMaskBit");
		const GLint exactLoc = glGetUniformLocation(g_fbPackShader, "u_packExact");
		const GLint decayLoc = glGetUniformLocation(g_fbPackShader, "u_packDecay");
		const int   exact    = (!g_fbSamplerSemiTrans && g_PsxFeedbackExact) ? 1 : 0;
		if (dampLoc != -1)
			glUniform1f(dampLoc, gain);
		if (maskLoc != -1)
			glUniform1f(maskLoc, g_fbSamplerSemiTrans ? 1.0f : 0.0f);
		if (exactLoc != -1)
			glUniform1f(exactLoc, exact ? 1.0f : 0.0f);
		if (decayLoc != -1)
			glUniform1f(decayLoc, (exact && g_fbSamplerDecay) ? 1.0f : 0.0f);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_fbPackTex);
	glBindVertexArray(g_fbPackVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glEnable(GL_STENCIL_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
	glViewport(vp[0], vp[1], vp[2], vp[3]);

	/* Sentinels, not the real state — see GR_DrawFullscreenTexture: recording the
	 * truth lets GR_SetBlendMode early-return and strand depth off. */
	g_PreviousShader       = (ShaderID)-1;
	g_lastBoundTexture     = (TextureID)-1;
	g_PreviousBlendMode    = -999;
	g_PreviousDepthMode    = -999;
	g_PreviousScissorState = -999;
#endif
}

static void GR_PackFrameToVramRect(int x, int y, int w, int h)
{
	/* Unity for a blending reader: the 50/50 composite it performs IS the decay,
	 * exactly as on hardware, where the draw buffer is cleared every frame and
	 * the overlay sums to a total weight of 1. Damping that loop as well only
	 * darkens the scene. g_PsxFeedbackDamp stays for the opaque loading trail,
	 * which has no such decay of its own. */
	GR_PackFrameToVramRectGain(x, y, w, h,
		g_fbSamplerSemiTrans ? g_PsxFeedbackDampBlend : g_PsxFeedbackDamp);
}

/* Pack the captured frame into every rect the game may read back: both PSX
 * display buffers, plus a latched scene scratch rect if one is active. */
static void GR_PackFrameToAllFeedbackRects(void)
{
	if (!g_psxDispBufValid)
		return;

	GR_PackFrameToVramRect(g_psxDispBuf[0].x, g_psxDispBuf[0].y,
	                       g_psxDispBuf[0].w, g_psxDispBuf[0].h);
	GR_PackFrameToVramRect(g_psxDispBuf[1].x, g_psxDispBuf[1].y,
	                       g_psxDispBuf[1].w, g_psxDispBuf[1].h);

	if (g_sceneFbRedirectTtl > 0)
	{
		GR_PackFrameToVramRect(g_sceneFbRedirect.x, g_sceneFbRedirect.y,
		                       g_sceneFbRedirect.w, g_sceneFbRedirect.h);
	}
}

/* Stamp a VRAM rect to packed word 0 (RG = 0,0), which the samplers treat as
 * texel-0 = transparent (discard). Used to blank the feedback rects when the
 * loading blur is not active, so the per-map overlay samplers draw nothing. */
static void GR_ClearVramRect(int x, int y, int w, int h)
{
#if USE_OPENGL
	GLfloat cc[4];

	if (w <= 0 || h <= 0)
		return;

	glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);

	glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, y, w, h);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(cc[0], cc[1], cc[2], cc[3]);

	/* [FBCLEAR] one-shot proof the blank reached the SAMPLED texture (the
	 * ANGLE rainbow diagnosis): FBO status + a readback of the rect's first
	 * texel, which must be packed word 0. Three lines per session. */
	{
		static int s_fbClearLogs = 0;
		if (s_fbClearLogs < 3)
		{
			GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			unsigned char px[4] = { 255, 255, 255, 255 };
			glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
			s_fbClearLogs++;
			eprintf("*[FBCLEAR] rect=(%d,%d %dx%d) tex=%u fboStatus=0x%x readback=(%d,%d) %s\n",
			        x, y, w, h, (unsigned)g_vramTexture, (unsigned)st, px[0], px[1],
			        (st == GL_FRAMEBUFFER_COMPLETE && px[0] == 0 && px[1] == 0) ? "OK" : "NOT BLANK");
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());

	/* Sentinels, not the real state — see GR_DrawFullscreenTexture. */
	g_PreviousBlendMode    = -999;
	g_PreviousDepthMode    = -999;
	g_PreviousScissorState = -999;
#endif
}

static void GR_ClearAllFeedbackRects(void)
{
	if (!g_psxDispBufValid)
		return;

	GR_ClearVramRect(g_psxDispBuf[0].x, g_psxDispBuf[0].y, g_psxDispBuf[0].w, g_psxDispBuf[0].h);
	GR_ClearVramRect(g_psxDispBuf[1].x, g_psxDispBuf[1].y, g_psxDispBuf[1].w, g_psxDispBuf[1].h);

	if (g_sceneFbRedirectTtl > 0)
		GR_ClearVramRect(g_sceneFbRedirect.x, g_sceneFbRedirect.y, g_sceneFbRedirect.w, g_sceneFbRedirect.h);
}

/* Capture the frame as rendered so far into the (w x h) pack texture, mapped so
 * PSX coordinate (u,v) of the display buffer lands on texel (u,v). Shared by the
 * per-present feedback store and the mid-frame scene-scratch capture; see the
 * g_psxAreaVp note below for why the source rect is not simply the viewport. */
static void GR_CaptureFrameToPackTex(int w, int h)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	GLuint readFBO = 0;

	GR_EnsureFbPackTarget(w, h);

#if PSYX_HAS_POSTPROCESS
	/* MSAA: a multisample backbuffer cannot be the source of a scaled blit —
	 * resolve same-size into the post texture first, then downscale from there. */
	if (g_cfg_msaaSamples > 0)
	{
		GR_EnsurePostTarget(g_windowWidth, g_windowHeight);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_postFBO);
		glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, 0, g_postW, g_postH,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		readFBO = g_postFBO;
	}
#endif
	/* Otherwise the scene target itself. Framebuffer 0 is the window, which
	 * holds the scene only when nothing renders offscreen: since native GL
	 * always does (GR_SceneAlwaysOffscreen), reading 0 captured the last
	 * PRESENTED frame, not this one, and every feedback effect -- the scene
	 * soft-focus (map3_s02 Alessa, map4_s04 Lisa), the dream overlays, the
	 * loading trail -- composited a stale or empty image. */
	if (readFBO == 0)
		readFBO = GR_ScreenReadFBO();

	/* Unflipped downscale of the window rect the PSX DISPLAY BUFFER occupies into
	 * the 320x224 capture; the pack shader does the one flip needed to land
	 * screen-top on the rect's first VRAM row. LINEAR because this is a large
	 * downsample feeding a blur.
	 *
	 * The source MUST be g_psxAreaVp, not the viewport. The game redraws this
	 * capture over PSX coordinates (0,0)-(dispW,dispH), so whatever window rect
	 * those coordinates cover is the only rect that reads back 1:1. The viewport
	 * is a different rect in two ways: Hor+ widens the ortho so the 4:3 core is
	 * 1/effectiveScale of the window (0.75 at 16:9), and g_PsxWorldVScale crops
	 * the world ortho to 0.872 of the buffer. Capturing the viewport therefore
	 * shrank the picture 0.75x horizontally and stretched it 1.147x vertically
	 * EVERY frame, and because the effect feeds on its own output that compounds:
	 * within a few frames the last gameplay frame collapsed into a narrow,
	 * repeatedly 5-bit-requantized smear — saturated vertical streaks for a
	 * fraction of a second at every room transition (the only time this store is
	 * armed). Rows the ortho crops are left black: the frame simply has no pixels
	 * there, and leaving them black keeps the redraw scale-exact. */
	{
		/* Read through the ortho of the pass that will REDRAW this capture. The
		 * per-map dream overlays are OT2 prims and the loading blur is not, and
		 * the two passes map the display buffer to different window rects. */
		const int useUi = (g_fbSamplerUiPass && g_psxUiAreaVpValid);
		const float* area = useUi ? g_psxUiAreaVp : g_psxAreaVp;
		const int areaValid = useUi ? g_psxUiAreaVpValid : g_psxAreaVpValid;

		float ax0 = area[0], ay0 = area[1];
		float ax1 = area[2], ay1 = area[3];
		int vx = g_presentVp[0], vy = g_presentVp[1];
		int vw = g_presentVp[2], vh = g_presentVp[3];
		float sx0, sy0, sx1, sy1;
		int dx0, dy0, dx1, dy1;

		if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = g_windowWidth; vh = g_windowHeight; }

		/* Widened ortho: the primitives are stretched to span it, so the store
		 * has to hold the whole picture rect rather than its 4:3 core. The ortho
		 * is fitted to the viewport, so that rect IS the viewport. */
		if (useUi && g_PsxFeedbackWideScale > 1.001f)
		{
			ax0 = (float)vx;          ay0 = (float)vy;
			ax1 = (float)(vx + vw);   ay1 = (float)(vy + vh);
		}
		/* The scene-scratch strips are stretched across the WORLD ortho's width
		 * (MakeVertexRect), which the Hor+ world ortho fits to the viewport, so
		 * the capture spans the viewport horizontally too. Vertical stays the
		 * 224-line buffer: that is all the strips cover. */
		else if (!useUi && g_fbCaptureWorldWide)
		{
			ax0 = (float)vx;
			ax1 = (float)(vx + vw);
		}

		if (!areaValid || ax1 <= ax0 || ay1 <= ay0)
		{
			ax0 = (float)vx; ay0 = (float)vy;
			ax1 = (float)(vx + vw); ay1 = (float)(vy + vh);
		}

		/* Clip to what was actually rendered. */
		sx0 = ax0 < (float)vx ? (float)vx : ax0;
		sy0 = ay0 < (float)vy ? (float)vy : ay0;
		sx1 = ax1 > (float)(vx + vw) ? (float)(vx + vw) : ax1;
		sy1 = ay1 > (float)(vy + vh) ? (float)(vy + vh) : ay1;

		/* The matching sub-rect of the capture, so the clip does not rescale. */
		dx0 = (int)((sx0 - ax0) / (ax1 - ax0) * (float)w + 0.5f);
		dx1 = (int)((sx1 - ax0) / (ax1 - ax0) * (float)w + 0.5f);
		dy0 = (int)((sy0 - ay0) / (ay1 - ay0) * (float)h + 0.5f);
		dy1 = (int)((sy1 - ay0) / (ay1 - ay0) * (float)h + 0.5f);

		{
			GLfloat cc[4];
			glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbPackFBO);
			glDisable(GL_SCISSOR_TEST);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glClearColor(cc[0], cc[1], cc[2], cc[3]);
		}

		/* [FBGEOM] one-shot companion to the strip line in MakeVertexRect: the
		 * window rect the capture reads and the sub-rect it lands on. The two
		 * together say whether the sliver is missing capture or short geometry. */
		{
			static int s_fbGeomSrcLogged[2] = { 0, 0 };
			if (!s_fbGeomSrcLogged[useUi ? 1 : 0])
			{
				s_fbGeomSrcLogged[useUi ? 1 : 0] = 1;
				eprintinfo("[FBGEOM] capture src %.1f,%.1f..%.1f,%.1f of vp %d,%d %dx%d -> dst %d,%d..%d,%d of %dx%d (ui=%d wide=%.4f)\n",
					sx0, sy0, sx1, sy1, vx, vy, vw, vh, dx0, dy0, dx1, dy1, w, h,
					useUi, g_PsxFeedbackWideScale);
			}
		}

		if (sx1 > sx0 && sy1 > sy0 && dx1 > dx0 && dy1 > dy0)
		{
			/* NEAREST for the exact loop. The stored frame is redrawn as blocks of
			 * one PSX pixel each, and a point sample from the centre of each block
			 * lands back on that same block, so the round trip is lossless. A
			 * filtered downscale mixes neighbours on every pass, and over a door
			 * load that diffusion is a grey haze. The blending dream overlays keep
			 * LINEAR: their loop halves every frame, so diffusion never builds. */
			const int exact = (!g_fbSamplerSemiTrans && g_PsxFeedbackExact) ? 1 : 0;
			glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);
			glBlitFramebuffer((int)(sx0 + 0.5f), (int)(sy0 + 0.5f),
			                  (int)(sx1 + 0.5f), (int)(sy1 + 0.5f),
			                  dx0, dy0, dx1, dy1,
			                  GL_COLOR_BUFFER_BIT, exact ? GL_NEAREST : GL_LINEAR);
		}
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());
	g_PreviousScissorState = 0;

	g_fbPackValid = 1;
#endif
}

/* One present has passed for the scene scratch-redirect. Runs on EVERY present,
 * not only when the feedback store is allowed: the redirect is now armed by the
 * game's own DR_AREA every frame the scene draws, so if this only ticked on the
 * store path the rect would be blanked forever after the scene ended -- and
 * (320,256 320x224) holds real map textures in other rooms. */
static void GR_SceneRedirectTick(void)
{
	if (g_sceneFbRedirectTtl <= 0)
		return;
	g_sceneFbRedirectTtl--;
	if (g_sceneFbRedirectTtl == 0 && s_sceneFbRedirectArms < 32)
	{
		eprintinfo("[FBSCRATCH] redirect LAPSED (%d,%d %dx%d) - rect no longer refreshed\n",
		           g_sceneFbRedirect.x, g_sceneFbRedirect.y,
		           g_sceneFbRedirect.w, g_sceneFbRedirect.h);
	}
}

/* Called once per present: capture the composed frame, then pack it into the
 * feedback rects. */
extern "C" void GR_StoreFrameBufferPsx(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	/* Consumed here, before any early return, so a hold set on a frame the
	 * store stands down for cannot leak into the next frame. */
	const int holdFrame = g_PsxFeedbackHoldFrame;
	g_PsxFeedbackHoldFrame = 0;

	if (!g_psxDispBufValid || g_PsxSkipFramebufferStore)
		return;

	/* The game just wrote its own data into a display-buffer rect — leave it
	 * alone until it stops (see GR_NoteVramUploadForFeedback). */
	if (g_fbFeedbackSuppress > 0)
	{
		g_fbFeedbackSuppress--;
		GR_SceneRedirectTick();
		return;
	}

	/* Loading-screen-only: while the loading/transition blur is not drawing,
	 * blank the feedback rects (word 0 → transparent) so the per-map overlays
	 * this store would otherwise drive read nothing instead of a stale/garbage
	 * frame. See g_PsxFeedbackStoreAllowed. */
	if (g_PsxFeedbackStoreAllowed <= 0)
	{
		GR_ClearAllFeedbackRects();
		GR_SceneRedirectTick();
		return;
	}
	g_PsxFeedbackStoreAllowed--;

	/* A HOLD frame: the scene is between two of its own steps and must not take
	 * a loop pass, or the ghost keeps fading at the present rate instead of the
	 * scene's. The rects keep the last store, which is what the scene redraws;
	 * a vram[] re-upload in the meantime still repacks it (GR_RepackFrameTo-
	 * VramBuffers does not look at this flag). */
	if (holdFrame)
	{
		GR_SceneRedirectTick();
		return;
	}

	GR_CaptureFrameToPackTex(g_psxDispBuf[0].w, g_psxDispBuf[0].h);

	GR_PackFrameToAllFeedbackRects();
	g_fbLastStoreDecay = g_fbSamplerDecay;
	g_fbLastStoreSemi  = g_fbSamplerSemiTrans;
	GR_SceneRedirectTick();
#endif
}

/* Mid-frame: pack everything drawn so far into a VRAM rect, in draw order, so
 * the prims that follow can sample it. This is how the scene-scratch effects
 * (map4_s04 Lisa, map3_s02, map7_s02) work: on PSX the DR_AREA points the whole
 * scene at offscreen VRAM and then eight SPRTs composite it back at 1-2 px
 * offsets with different blends -- a soft-focus. PC draws the scene on screen,
 * so the moment the game switches the area back is where the capture goes
 * (game_main.c rewrites that DR_AREA into DR_PSYX_FBCAPTURE). One-shot, not a
 * loop -- the rect is fully rewritten every frame -- so the gain is unity; the
 * feedback damp exists only for rects that feed on their own output. */
extern "C" void GR_CaptureFrameToVramRect(int x, int y, int w, int h)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	/* The scene scratch-redirect is its own consumer: the game redraws this rect
	 * with world-pass prims at PSX coordinates, so it wants the world mapping and
	 * no widening, whatever the last display-buffer sampler happened to be. */
	const int savedUiPass  = g_fbSamplerUiPass;
	const int savedSemi    = g_fbSamplerSemiTrans;
	const int savedExact   = g_PsxFeedbackExact;
	/* This runs MID-PASS, between splits that DrawAllSplits draws from the
	 * vertex array it bound once at the top. The pack leaves VAO 0 bound
	 * (harmless at end of frame, where it was written for), and on a core
	 * context every draw after that is an error: the soft-focus strips, and
	 * anything else behind the capture in the pass, were never rasterized. */
	GLint     savedVao     = 0;

	/* dream_blur off: no capture. The strips that would composite it are
	 * dropped at parse (GR_SceneFbRedirectCovers), so the scene draws plain. */
	if (w <= 0 || h <= 0 || !g_cfg_dreamFeedback)
		return;

	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVao);
	g_fbCaptureWorldWide = g_PsxWorldOrthoValid && g_PsxWorldDisp[0] > 0.0f &&
	                       (g_PsxWorldOrtho[1] - g_PsxWorldOrtho[0]) / g_PsxWorldDisp[0] > 1.001f;

	/* A one-shot copy, not a loop: unity gain, filtered.
	 *
	 * Packed WITH the mask bit. These scenes render into the rect under
	 * DR_STP(1) (queued at the far bucket, ahead of the whole scene -- see
	 * map3_s02_2.c / map4_s04_2.c), so on PS1 every texel the strips read
	 * carries STP, and that bit is what lets their semi-transparent layers
	 * blend: 50/50 on the average strips, where a clear bit draws the texel
	 * solid. PC strips the DR_STP packets, so the capture has to set it. Left
	 * clear, the two average layers stamped solid over each other and the
	 * four-tap soft focus came out as one offset, darkened copy. */
	g_fbSamplerUiPass    = 0;
	g_fbSamplerSemiTrans = 1;
	g_PsxFeedbackExact   = 0;
	GR_CaptureFrameToPackTex(w, h);
	GR_PackFrameToVramRectGain(x, y, w, h, 1.0f);
	g_fbSamplerUiPass    = savedUiPass;
	g_fbSamplerSemiTrans = savedSemi;
	g_PsxFeedbackExact   = savedExact;
	g_fbCaptureWorldWide = 0;
	glBindVertexArray((GLuint)savedVao);
#endif
}

/* Re-pack after a full vram[] re-upload has stamped CPU bytes over the feedback
 * rects (GR_UpdateVRAM). */
extern "C" void GR_RepackFrameToVramBuffers(void)
{
	if (g_PsxSkipFramebufferStore)
		return;

	/* Re-blank (not re-pack) when the loading blur is off, so a full vram[]
	 * re-upload that just stamped CPU bytes over the feedback rects can't leave
	 * garbage for the per-map overlay samplers. */
	if (g_PsxFeedbackStoreAllowed <= 0)
	{
		GR_ClearAllFeedbackRects();
		return;
	}

	if (!g_fbPackValid)
		return;

	/* Restore exactly what the last store wrote, from the same capture and with
	 * the same flags. This runs whenever a LoadImage re-uploads vram[], which a
	 * loading screen does constantly; packing with THIS frame's decay flag would
	 * take the trail down an extra step on every such frame. */
	{
		const int savedDecay = g_fbSamplerDecay;
		const int savedSemi  = g_fbSamplerSemiTrans;

		g_fbSamplerDecay     = g_fbLastStoreDecay;
		g_fbSamplerSemiTrans = g_fbLastStoreSemi;
		GR_PackFrameToAllFeedbackRects();
		g_fbSamplerDecay     = savedDecay;
		g_fbSamplerSemiTrans = savedSemi;
	}
}

/* Legacy raw-blit scene-redirect helper, superseded by the packed path above.
 * Kept as a no-op shim so the old call site in GR_StoreFrameBuffer (which is
 * itself compiled out) does not need to change. */
static void GR_BlitStoredFrameToSceneRedirect(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	if (g_sceneFbRedirectTtl <= 0)
		return;
#endif
}

void GR_StoreFrameBuffer(int x, int y, int w, int h)
{
	/* PC port: skip the entire framebuffer→VRAM blit when a TIM-protect
	 * screen is active. Without this, every PsyX_EndScene blits the rendered
	 * frame onto g_vramTexture at (disp.x, disp.y, disp.w, disp.h), which
	 * destroys CLUTs/textures that the game just LoadImage'd into that
	 * region (paper-map CLUT at (224,15) is the canonical victim). */
	if (g_PsxSkipFramebufferStore)
		return;

#if USE_OPENGL
	// set storage size first
	if (g_PreviousFramebuffer.w != w ||
		g_PreviousFramebuffer.h != h)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	g_PreviousFramebuffer.x = x;
	g_PreviousFramebuffer.y = y;
	g_PreviousFramebuffer.w = w;
	g_PreviousFramebuffer.h = h;

#if USE_FRAMEBUFFER_BLIT
	glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

	// before drawing set source and target
	{
		GLuint storeReadFBO = 0;	// default: read straight from the backbuffer
#if PSYX_HAS_POSTPROCESS
		/* MSAA: a multisample backbuffer cannot be the source of a *scaled*
		 * blit (this one shrinks window -> w×h and flips Y). Resolve it
		 * same-size into the single-sample post texture first, then scale
		 * from there. */
		if (g_cfg_msaaSamples > 0)
		{
			GR_EnsurePostTarget(g_windowWidth, g_windowHeight);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_postFBO);
			glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, 0, g_postW, g_postH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			storeReadFBO = g_postFBO;
		}
#endif
		if (storeReadFBO == 0)
			storeReadFBO = GR_ScreenReadFBO();	/* the scene target, not the window */
		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, storeReadFBO);		// backbuffer, or resolved MSAA copy
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glBlitFramebuffer);

		/* PC port: destination is the w-by-h g_fbTexture, so the frame must be
		 * stored at its origin. The old (x, y+h)..(x+w, y) rect only landed
		 * inside the texture when disp.y == 0 — for the second display buffer
		 * (disp.y != 0) the whole blit was clipped away and g_fbTexture kept a
		 * stale older frame, which the next blit then stamped into VRAM. */
		glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Blit framebuffer to VRAM screen area

		// before drawing set source and target
		glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

		// rebind vram texture
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glBlitFramebuffer);					// source is backbuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

		glBlitFramebuffer(0, 0, w, h,
			x, y + h, x + w, y,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		
		// done, unbind
		glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());
	}

	g_fbStoreValid = 1;

	/* Scene scratch-redirect feedback (see GR_SetSceneFbRedirect): give the
	 * effect's SPRT strips the frame they expect to find at the redirect rect.
	 * TTL decremented here — once per present. */
	GR_BlitStoredFrameToSceneRedirect();
	if (g_sceneFbRedirectTtl > 0)
	{
		g_sceneFbRedirectTtl--;
		/* Lapse = the scene stopped re-submitting its DR_AREA, so from the next
		 * present its strips sample whatever is really in VRAM at the rect. If
		 * the scene is still on screen when this prints, that IS the bar. */
		if (g_sceneFbRedirectTtl == 0 && s_sceneFbRedirectArms < 32)
		{
			eprintinfo("[FBSCRATCH] redirect LAPSED (%d,%d %dx%d) - strips now sample raw VRAM\n",
			           g_sceneFbRedirect.x, g_sceneFbRedirect.y,
			           g_sceneFbRedirect.w, g_sceneFbRedirect.h);
		}
	}

	// after drawing
	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
	glFlush();
#endif

	GR_ReadFramebufferDataToVRAM();
#endif
}

/* PC port: re-blit the last stored frame (g_fbTexture) over its framebuffer
 * rect in the CURRENT g_vramTexture. GR_UpdateVRAM must call this after every
 * full vram[] re-upload: any LoadImage between frames triggers a texture swap
 * plus whole-texture upload from the CPU vram[] array, whose framebuffer
 * region only ever holds stale lossy PBO-readback bytes. Framebuffer-feedback
 * effects (Screen_BackgroundMotionBlur and the per-map ghosting overlays that
 * sample getTPage(2, ...) display-buffer pages) then read that junk — the
 * accumulating rainbow corruption in TIM-streaming cutscenes. */
static void GR_RestoreStoredFramebufferRegion(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	if (!g_fbStoreValid || g_PsxSkipFramebufferStore)
		return;
	/* glBlitFramebuffer is a glad-loaded pointer (NULL until loaded). If a
	 * driver/context leaves it unloaded, calling it jumps to 0x0 -- the
	 * issue #102 crash signature (GR_UpdateVRAM -> call 0x0 on the cafe map
	 * pickup). Skip the restore rather than fault; the feedback effect just
	 * degrades. */
	if (!glBlitFramebuffer)
		return;

	const int x = g_PreviousFramebuffer.x;
	const int y = g_PreviousFramebuffer.y;
	const int w = g_PreviousFramebuffer.w;
	const int h = g_PreviousFramebuffer.h;

	if (w <= 0 || h <= 0)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glBlitFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

	glBlitFramebuffer(0, 0, w, h,
		x, y + h, x + w, y,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GR_ScreenFBO());
	glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());

	/* A full vram[] re-upload also stamped TIM bytes over the scene scratch
	 * rect — re-blit the stored frame there too (no TTL decrement here). */
	GR_BlitStoredFrameToSceneRedirect();
#endif
}

void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	vram_need_update = 1;

	int stride = w;

	if (!src)
	{
		framebuffer_need_update = 1;

		src = vram;
		stride = VRAM_WIDTH;
	}

	src += x + y * stride;

	/* Clamp the destination to the VRAM rectangle. A well-formed PSX upload never
	 * exceeds VRAM, but a mismatched/wrong-region TIM can report a rect that runs
	 * off the bottom/right edge — without this the row memcpy walks past vram[]
	 * and crashes (seen feeding a PAL disc a US-shaped upload). */
	if (dst_x < 0) { w += dst_x; src -= dst_x; dst_x = 0; }
	if (dst_y < 0) { h += dst_y; src -= dst_y * stride; dst_y = 0; }
	if (dst_x + w > VRAM_WIDTH)  w = VRAM_WIDTH  - dst_x;
	if (dst_y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - dst_y;
	if (w <= 0 || h <= 0)
		return;

	/* PC port: self-protect the framebuffer-feedback store. It repacks the frame
	 * into the PSX display-buffer rects every present, which is safe only while
	 * nothing else lives there. Most scenes keep CLUTs above the buffers (y<32)
	 * or below them (y>=480) — but some cutscenes LoadImage CLUTs straight into
	 * the display region as characters appear (map7_s03's endings hand-guard
	 * exactly that with g_PsxSkipFramebufferStore). Rather than rely on finding
	 * every such scene by hand, notice the upload here and stand the store down
	 * for a few frames so the game's own data always wins. Scenes that keep
	 * uploading there simply keep the effect off, which is the correct trade. */
	GR_NoteVramUploadForFeedback(dst_x, dst_y, w, h);

	unsigned short* dst = vram + dst_x + dst_y * VRAM_WIDTH;

	for (int i = 0; i < h; i++) {
		SDL_memcpy(dst, src, w * sizeof(short));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
	unsigned short* src = vram + x + VRAM_WIDTH * y;

	for (int i = 0; i < dst_h; i++) {
		SDL_memcpy(dst, src, dst_w * sizeof(short));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

void GR_UpdateVRAM()
{
	if (!vram_need_update)
		return;

	vram_need_update = 0;

#if USE_OPENGL
	g_vramTexture = g_vramTexturesDouble[g_vramTextureIdx];
	g_vramTextureIdx++;
	g_vramTextureIdx &= 1;

	glBindTexture(GL_TEXTURE_2D, g_vramTexture);

	/* Always a sub-image upload: storage was allocated at creation. The old
	 * RENDERER_OGL branch RE-SPECIFIED the texture with glTexImage2D on every
	 * upload, which native GL tolerates while the texture is an FBO attachment
	 * but ANGLE (ES over D3D11/Vulkan) may orphan -- the feedback-rect blank in
	 * GR_ClearVramRect then cleared dead storage while the sampled texture kept
	 * the CPU vram[] bytes just stamped over the display-buffer rects: the
	 * map4_s01 (Lisa) cutscene overlay drew that garbage as a rainbow band,
	 * ANGLE-only, worsening with TIM streaming. */
#if 0
	glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#else
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#endif

	GR_RestoreStoredFramebufferRegion();

	/* PC port: the full vram[] re-upload just stamped CPU bytes over the
	 * framebuffer-feedback pages — re-pack the last captured frame into them so
	 * TIM streaming mid-scene can't blank the motion-blur / dream source. */
	GR_RepackFrameToVramBuffers();

#endif
}

/* PC port: directly upload a sub-region of vram[] to BOTH double-buffered VRAM
 * textures, bypassing the vram_need_update / texture-swap dance. Used by the
 * paper-map TIM-protect helper to guarantee the paper-map data is present in
 * whichever texture the renderer is about to sample, even if some unfound
 * code path has stamped framebuffer pixels into the other texture.
 *
 * Why this exists: the gating in GR_StoreFrameBuffer / GR_ReadFramebufferDataToVRAM
 * via g_PsxSkipFramebufferStore is necessary but apparently not sufficient —
 * paper-map pickup screens still show tiled gameplay framebuffer content
 * sampled from the (320, 16+) VRAM region even with all known framebuffer→VRAM
 * paths gated. A direct upload is the nuclear-option fallback that defeats
 * any unfound corruption path: whatever wrote framebuffer pixels into the
 * GPU texture after the last GR_UpdateVRAM gets unconditionally overwritten. */
void GR_DirectUploadVRAMRegion(int x, int y, int w, int h)
{
#if USE_OPENGL
	if (x < 0 || y < 0 || w <= 0 || h <= 0)
		return;
	if (x + w > VRAM_WIDTH)  w = VRAM_WIDTH  - x;
	if (y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - y;
	if (w <= 0 || h <= 0)
		return;

	/* Build a contiguous block from the vram[] sub-region (vram is 1024 stride). */
	unsigned short* block = (unsigned short*)malloc((size_t)w * (size_t)h * sizeof(unsigned short));
	if (!block)
		return;

	for (int row = 0; row < h; row++)
	{
		SDL_memcpy(block + (size_t)row * w,
		           vram + (size_t)(y + row) * VRAM_WIDTH + x,
		           (size_t)w * sizeof(unsigned short));
	}

	for (int i = 0; i < 2; i++)
	{
		glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, VRAM_FORMAT, GL_UNSIGNED_BYTE, block);
	}
	glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

	free(block);
#endif
}

/* PC port debug: dump the entire 1024x512 16-bit VRAM to a raw file so the
 * loaded textures/CLUTs can be inspected offline (decode 5551). */
void GR_DumpVRAM(const char* path)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(vram, sizeof(unsigned short), VRAM_WIDTH * VRAM_HEIGHT, f);
	fclose(f);
}

/* PC port: one-shot GL error sweep, for diagnosing a renderer that draws
 * without complaining. Nothing in the frame path calls glGetError, so an
 * illegal call — and ES is far stricter than desktop GL about framebuffer
 * blits, formats and multisample resolves — fails silently and the frame just
 * comes out black. Reported once per call site so a single log names the first
 * thing that broke instead of a wall of repeats. */
void GR_DiagGLError(const char* where)
{
	static const char* s_seen[16];
	static int         s_seenCount = 0;
	GLenum             err;

	if (!g_grIsGLES && !g_dbg_glDiag)
		return;

	err = glGetError();
	if (err == GL_NO_ERROR)
		return;

	for (int i = 0; i < s_seenCount; i++)
	{
		if (s_seen[i] == where)
			return;
	}
	if (s_seenCount < 16)
		s_seen[s_seenCount++] = where;

	eprintwarn("[GLDIAG] GL error 0x%04X at %s (backend=%s)\n",
		(unsigned)err, where, PsyX_Backend_GetName(g_grActiveBackend));
}

/* [GREYFRAME] per-swap half, run on the window framebuffer right before the
 * swap, i.e. on exactly the image that is presented. Four pack buffers in a
 * ring: each swap queues this frame's rows and inspects the slot it is about to
 * reuse, four presents old, which has long finished -- no stall. */
static void GreyFrame_Present(int w, int h)
{
#if USE_OPENGL
	static GLuint        s_pbo[4];
	static int           s_pboW[4];
	static GreyFrameSnap s_ring[4];
	static int           s_idx = 0, s_filled = 0, s_logs = 0, s_plogs = 0;
	static unsigned      s_frame = 0;
	GLint                prevRead = 0, prevPack = 0, fbo = 0;
	const int            slot = s_idx;

	if (g_grIsGLES || w <= 0 || h <= 0)
		return;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	s_gf.endFbo = fbo;
	glGetIntegerv(GL_VIEWPORT, s_gf.endVp);
	s_gf.endSc  = glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0;
	s_gf.glerr  = (int)glGetError();
	s_gf.frame  = ++s_frame;
	memcpy(s_gf.tag, g_PsxGreyTag, sizeof(s_gf.tag));
	memcpy(s_gf.probe, g_PsxPanelProbe, sizeof(s_gf.probe));
	memcpy(s_gf.pstate, g_PsxPanelState, sizeof(s_gf.pstate));
	g_PsxPanelProbe[0] = 0;

	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);

	if (s_pbo[0] == 0)
		glGenBuffers(4, s_pbo);

	/* The slot about to be reused holds the frame presented four swaps ago. */
	if (s_filled >= 4 && (s_logs < 40 || s_plogs < 40))
	{
		const GreyFrameSnap* g = &s_ring[slot];
		const int            n = s_pboW[slot] * 3;
		const unsigned char* px;

		glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[slot]);
		px = (const unsigned char*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, n * 4 + 4, GL_MAP_READ_BIT);
		if (px)
		{
			int i, flat = 1;

			/* [PANELMISS] the overlay's own title-band pixel, in the image that
			 * was presented. */
			if (g->probe[0] && s_plogs < 40)
			{
				const unsigned char* q = px + n * 4;

				/* One line proving the check is live, so a session with no
				 * misses reads as "every frame had the panel", not "the probe
				 * never ran". */
				{
					static int s_live = 0;
					if (!s_live)
					{
						s_live = 1;
						eprintinfo("[PANELMISS] check live: frame=%u at=(%d,%d) got=(%d,%d,%d) want=(%d,%d,%d)\n",
							g->frame, g->probe[1], g->probe[2], q[0], q[1], q[2],
							g->probe[3], g->probe[4], g->probe[5]);
					}
				}
				const int dr = (int)q[0] - g->probe[3], dg = (int)q[1] - g->probe[4], db = (int)q[2] - g->probe[5];
				if (dr * dr + dg * dg + db * db > 3 * 10 * 10)
				{
					const int* st = g->pstate;
					s_plogs++;
					eprintinfo("[PANELMISS] frame=%u at=(%d,%d) got=(%d,%d,%d) want=(%d,%d,%d) | draw fbo=%d sc=%d box=%d,%d,%d,%d stencil=%d func=0x%x ref=%d mask=0x%x vp=%dx%d prog=%d tex=%d glerr=0x%x phase=%d | end fbo=%d vp=%d,%d,%d,%d sc=%d glerr=0x%x | game=%d sys=%d vbl=%d world=%d freeze=%d\n",
						g->frame, g->probe[1], g->probe[2], q[0], q[1], q[2], g->probe[3], g->probe[4], g->probe[5],
						st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9], st[10], st[11],
						st[12], st[13], st[14], st[15],
						g->endFbo, g->endVp[0], g->endVp[1], g->endVp[2], g->endVp[3], g->endSc, g->glerr,
						g->tag[0], g->tag[1], g->tag[2], g->tag[3], g->tag[5]);
				}
			}
			for (i = 1; i < n && flat; i++)
				flat = px[i * 4] == px[0] && px[i * 4 + 1] == px[1] && px[i * 4 + 2] == px[2];

			/* Black is every fade and loading gap; only a flat NON-black frame
			 * that is exactly its own clear is the flash. */
			if (s_logs < 40 && flat && (px[0] | px[1] | px[2]) > 8 &&
			    px[0] == g->clearRGB[0] && px[1] == g->clearRGB[1] && px[2] == g->clearRGB[2])
			{
				s_logs++;
				eprintinfo("[GREYFRAME] frame=%u rgb=(%d,%d,%d) draws=%d tris=%d clears=%d clearsAfterDraw=%d clearFbo=%d offscreen=%d glerr=0x%x | first fbo=%d vp=%d,%d,%d,%d sc=%d box=%d,%d,%d,%d mask=%d%d%d%d dtest=%d dfunc=0x%x prog=%d blend=%d | end fbo=%d vp=%d,%d,%d,%d sc=%d | game=%d sys=%d vbl=%d world=%d dt=%d freeze=%d\n",
					g->frame, px[0], px[1], px[2], g->draws, g->tris, g->clears, g->clearsAfterDraw, g->clearFbo,
					g->offscreen, g->glerr,
					g->firstFbo, g->firstVp[0], g->firstVp[1], g->firstVp[2], g->firstVp[3],
					g->firstSc, g->firstBox[0], g->firstBox[1], g->firstBox[2], g->firstBox[3],
					g->firstMask[0], g->firstMask[1], g->firstMask[2], g->firstMask[3],
					g->firstDTest, g->firstDFunc, g->firstProg, g->firstBlend,
					g->endFbo, g->endVp[0], g->endVp[1], g->endVp[2], g->endVp[3], g->endSc,
					g->tag[0], g->tag[1], g->tag[2], g->tag[3], g->tag[4], g->tag[5]);
			}
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[slot]);
	if (s_pboW[slot] != w)
	{
		glBufferData(GL_PIXEL_PACK_BUFFER, w * 3 * 4 + 4, NULL, GL_STREAM_READ);
		s_pboW[slot] = w;
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadPixels(0, h / 4,     w, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void*)(uintptr_t)0);
	glReadPixels(0, h / 2,     w, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void*)(uintptr_t)(w * 4));
	glReadPixels(0, h * 3 / 4, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void*)(uintptr_t)(w * 8));
	if (s_gf.probe[0])
		glReadPixels(s_gf.probe[1], s_gf.probe[2], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void*)(uintptr_t)(w * 12));

	glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prevPack);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);

	s_ring[slot] = s_gf;
	s_idx = (slot + 1) & 3;
	if (s_filled < 4)
		s_filled++;
#endif
	memset(&s_gf, 0, sizeof(s_gf));
}

void GR_SwapWindow()
{
	{
		extern int g_PsxFrameVerts, g_PsxLastFrameVerts;
		g_PsxLastFrameVerts = g_PsxFrameVerts;
		g_PsxFrameVerts     = 0;
	}
	GR_DiagGLError("end of frame");

	/* Stretch the internal target onto the real window. This is the only bind of
	 * framebuffer 0 that genuinely means "the window" -- every other one means
	 * "the scene target" and goes through GR_ScreenFBO(). LINEAR so a lower
	 * internal resolution scales up smoothly rather than blockily. */
	if (g_internalFBO != 0 && g_presentWidth > 0 && g_presentHeight > 0)
	{
		int dx, dy, dw, dh;
		GLuint src = GR_ScreenReadFBO();   /* resolves first when multisampled */

		GR_PresentRect(&dx, &dy, &dw, &dh);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glViewport(0, 0, g_presentWidth, g_presentHeight);

		/* Scissor would clip both the clear and the blit to whatever the frame
		 * last set, so it is off for the present. */
		glDisable(GL_SCISSOR_TEST);

		/* Only needed when the image does not fill the window, but harmless
		 * otherwise and it keeps stale edges from ever showing through. */
		if (dw != g_presentWidth || dh != g_presentHeight)
		{
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}

		GR_PresentBlit(src, dx, dy, dw, dh);

		/* Native: the PC overlays draw now, onto the window over the scaled
		 * picture, in the same 4:3 rect -- so their pointer fractions still
		 * line up (PsyX_MapWindowToViewport works in fractions of it). */
		if (g_PcNativeRender && g_PsyX_PostCaptureHook)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(dx, dy, dw, dh);
			s_overlayToWindow = 1;
			g_PsyX_PostCaptureHook();
			s_overlayToWindow = 0;
			glViewport(0, 0, g_presentWidth, g_presentHeight);
		}
		if (g_PsxVoidProbeArmed)
		{
			VoidProbeRows("window", 0, g_presentWidth, g_presentHeight);
			g_PsxVoidProbeArmed = (g_PsxVoidProbeArmed == 1) ? 2 : 0; /* second pass = tagged frame */
		}
		glBindFramebuffer(GL_FRAMEBUFFER, g_internalFBO);
	}
	else if (g_PsxVoidProbeArmed)
	{
		g_PsxVoidProbeArmed = (g_PsxVoidProbeArmed == 1) ? 2 : 0; /* second pass = tagged frame */
	}

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	if (PsyX_Angle_Active())
		PsyX_Angle_Swap();
	else
	{
		GreyFrame_Present(g_internalFBO ? g_presentWidth : g_windowWidth,
		                  g_internalFBO ? g_presentHeight : g_windowHeight);
		/* Swap with the window framebuffer bound. macOS 26's GL-on-Metal
		 * presents nothing (a black window) when an FBO is still bound at
		 * flushBuffer, even though framebuffer 0 holds the finished frame. */
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		SDL_GL_SwapWindow(g_window);
		if (g_internalFBO)
			glBindFramebuffer(GL_FRAMEBUFFER, g_internalFBO);
	}
#endif

	//glFinish();
}

/* PC port: force GL depth test ON for the inventory item pass (see
 * PsyX_ForceItemDepthBegin). When set, depth stays on even for the item's
 * semi-transparent faces (GR_SetBlendMode would otherwise GR_EnableDepth(0)),
 * so the model's own front faces occlude its back faces (radio antenna through
 * the body). Scoped by game code to the inventory screen and the pickup take
 * screen, where OT0 holds the item alone — never the live world. */
int g_PsyX_ForceItemDepth = 0;

void GR_EnableDepth(int enable)
{
	/* Track the APPLIED GL state (not the requested `enable`) so toggling
	 * g_PsyX_ForceItemDepth mid-frame re-applies on the next call. */
	int applied = ((enable && g_cfg_pgxpZBuffer) || g_PsyX_ForceItemDepth) ? 1 : 0;

	if (g_PreviousDepthMode == applied)
		return;

	g_PreviousDepthMode = applied;

#if USE_OPENGL
	if (applied)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
#endif
}

/* PGXP coplanar fix: switch the depth comparison between GL_ALWAYS (static-world
 * painter pass — every world face passes so coplanar faces resolve by OT order,
 * not depth test) and GL_LEQUAL (everything else). Only called on the PGXP-on
 * path (DrawSplit gates on g_PsxUsePgxp); when off, glDepthFunc stays at its
 * GL_LEQUAL init default and this never runs. State-cached like the siblings. */
void GR_SetDepthFuncAlways(int enable)
{
	enable = enable ? 1 : 0;
	if (g_PreviousDepthFuncAlways == enable)
		return;
	g_PreviousDepthFuncAlways = enable;
#if USE_OPENGL
	glDepthFunc(enable ? GL_ALWAYS : GL_LEQUAL);
#endif
}

/* [ITEMDEPTH] probe support: report the driver's ACTUAL depth state, not the
 * renderer's cached trackers. g_PreviousDepthMode is handed back alongside so a
 * tracker-vs-driver desync is visible in one line. Read-only — no glEnable /
 * glDepthFunc / glDepthMask here, and the GL error queue is drained so a driver
 * that rejects the attachment query cannot leak an error into the next draw. */
extern "C" void PsyX_ItemProbe_ReadGlDepthState(int* depthTest, int* depthFunc, int* depthMask,
                                                int* depthBits, float* rangeNear, float* rangeFar,
                                                int* fbo, int* cullFace, int* trackerDepthMode)
{
#if USE_OPENGL
	GLint    iv = 0;
	GLboolean bv = GL_FALSE;
	GLfloat  rv[2] = { 0.0f, 1.0f };

	if (depthTest) *depthTest = glIsEnabled(GL_DEPTH_TEST) ? 1 : 0;
	if (cullFace)  *cullFace  = glIsEnabled(GL_CULL_FACE) ? 1 : 0;

	glGetIntegerv(GL_DEPTH_FUNC, &iv);
	if (depthFunc) *depthFunc = (int)iv;

	glGetBooleanv(GL_DEPTH_WRITEMASK, &bv);
	if (depthMask) *depthMask = bv ? 1 : 0;

	glGetFloatv(GL_DEPTH_RANGE, rv);
	if (rangeNear) *rangeNear = rv[0];
	if (rangeFar)  *rangeFar  = rv[1];

	iv = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &iv);
	if (fbo) *fbo = (int)iv;

	{
		GLint bits = -1;
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
			iv ? GL_DEPTH_ATTACHMENT : GL_DEPTH,
			GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
		while (glGetError() != GL_NO_ERROR) { }
		if (depthBits) *depthBits = (int)bits;
	}
#else
	if (depthTest) *depthTest = -1;
	if (depthFunc) *depthFunc = -1;
	if (depthMask) *depthMask = -1;
	if (depthBits) *depthBits = -1;
	if (rangeNear) *rangeNear = 0.0f;
	if (rangeFar)  *rangeFar  = 1.0f;
	if (fbo)       *fbo       = -1;
	if (cullFace)  *cullFace  = -1;
#endif
	if (trackerDepthMode) *trackerDepthMode = g_PreviousDepthMode;
}

/* Bracket the inventory item OT0 draw: clear depth so the item tests only
 * against itself, force depth test+write on for every item face regardless of
 * blend, then restore. Both are no-ops for any other pass (game code only calls
 * them around the GameState_InventoryScreen item draw). */
extern "C" void PsyX_ForceItemDepthBegin(void)
{
#if USE_OPENGL
	g_PsyX_ForceItemDepth = 1;
	g_PreviousDepthMode = -1;   /* force next GR_EnableDepth to re-apply */
	glDepthMask(GL_TRUE);
	glClear(GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
#endif
}

extern "C" void PsyX_ForceItemDepthEnd(void)
{
#if USE_OPENGL
	g_PsyX_ForceItemDepth = 0;
	g_PreviousDepthMode = -1;   /* force the next prim's GR_EnableDepth to re-apply */
	glDisable(GL_DEPTH_TEST);
#endif
}

void GR_SetStencilMode(int drawPrim)
{
	if (g_PreviousStencilMode == drawPrim)
		return;

	g_PreviousStencilMode = drawPrim;

#if USE_OPENGL
	if (drawPrim)
	{
		glStencilFunc(GL_ALWAYS, 1, 0x10);
		glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
	}
	else
	{
		glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
		glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
	}
#endif
}

void GR_SetBlendMode(BlendMode blendMode)
{
	if (g_PreviousBlendMode == blendMode)
		return;

#if USE_OPENGL
	/* BM_ADD_QUATER_SOURCE reads the constant set when blending is enabled,
	 * which a direct switch out of BM_CONSTANT_ALPHA never passes through. */
	if (g_PreviousBlendMode == BM_CONSTANT_ALPHA)
		glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);

	/* Fog mode for this blend: additive/subtractive prims (blood, muzzle flash) must fade
	 * toward black under fog, not blend toward the light fog color (which whitened their
	 * edges/faded pixels in daytime). Push now — the fog shader is the bound program here —
	 * and again at every shader bind, so it's correct regardless of bind/blend order. */
	g_PsxFogToBlack = (blendMode == BM_ADD || blendMode == BM_SUBTRACT ||
	                   blendMode == BM_ADD_QUATER_SOURCE) ? 1 : 0;
	if (u_fogToBlackLoc != -1)
		glUniform1i(u_fogToBlackLoc, g_PsxFogToBlack);
	if (blendMode == BM_NONE)
	{
		if (g_PreviousBlendMode != BM_NONE)
		{
			glBlendColor(1.f, 1.f, 1.f, 1.f);
			glDisable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(1);
		return;
	}
	else
	{
		/* g_PreviousBlendMode < 0 is the post-shadow-pass "unknown" sentinel
		 * (-999): the pass glDisable()d blending, so treat it like BM_NONE here
		 * or the first blended split after the pass renders opaque (solid black
		 * where a subtractive/average prim was expected). */
		if(g_PreviousBlendMode == BM_NONE || g_PreviousBlendMode < 0)
		{
			glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
			glEnable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(0);
	}

	glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
	switch (blendMode) {
	case BM_AVERAGE:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case BM_ADD:
	case BM_SUBTRACT:
		glBlendFunc(GL_ONE, GL_ONE);
		break;
	case BM_ADD_QUATER_SOURCE:
		glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
		break;
	case BM_CONSTANT_ALPHA:
		glBlendColor(0.25f, 0.25f, 0.25f, g_PsxFeedbackDampBlend);
		glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
		break;
	default:
		break;
	}
#endif

	g_PreviousBlendMode = blendMode;
}

void GR_SetPolygonOffset(float ofs)
{
#if USE_OPENGL
	if (ofs == 0.0f)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	else
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(0.0f, ofs);
	}
#endif
}

void GR_SetViewPort(int x, int y, int width, int height)
{
#if USE_OPENGL
	glViewport(x, y, width, height);
#endif
}

void GR_SetWireframe(int enable)
{
#if defined(RENDERER_OGL)
	/* ES has no glPolygonMode at all, so the debug wireframe view is simply
	 * unavailable on a translated backend. Emulating it would mean re-issuing
	 * every draw as GL_LINES, which is not worth it for a debug toggle. */
	if (g_grCaps.polygonMode)
		glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

void GR_BindVertexBuffer()
{
#if USE_OPENGL
	glBindVertexArray(g_glVertexArray[g_curVertexBuffer]);

	glEnableVertexAttribArray(a_position);
	glEnableVertexAttribArray(a_texcoord);
	glEnableVertexAttribArray(a_color);
	glEnableVertexAttribArray(a_extra);

	glVertexAttribPointer(a_position, 4, GL_SHORT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->x);
	glVertexAttribPointer(a_zw, 1, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->z);
	glEnableVertexAttribArray(a_zw);
	glVertexAttribPointer(a_pgxp, 3, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->ppx);
	glEnableVertexAttribArray(a_pgxp);
	glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->u);
	glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GrVertex), &((GrVertex*)NULL)->r);
	glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->tcx);
	glVertexAttribPointer(a_normal, 3, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->nx);
	glEnableVertexAttribArray(a_normal);
	glVertexAttribPointer(a_viewpos, 3, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->vsx);
	glEnableVertexAttribArray(a_viewpos);
	glVertexAttribPointer(a_geom3d, 1, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->geom3d);
	glEnableVertexAttribArray(a_geom3d);
	glVertexAttribPointer(a_uvlim, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->ulo);
	glEnableVertexAttribArray(a_uvlim);

	g_curVertexBuffer++;
	g_curVertexBuffer &= 1;
#else
#error
#endif
}

extern "C" void Pc_ModernVertex_Upload(const GrModernVertex* vertices, int count);
static_assert(a_viewpos + 1 <= 8, "all vertex layouts must fit the GLES2 8-attribute limit");

void GR_UpdateModernVertexBuffer(const GrModernVertex* vertices, int num_vertices)
{
	Pc_ModernVertex_Upload(vertices, num_vertices);
}

struct PcModernDrawBinding { unsigned int textureId; int texFormat, vertexCount; int nativeWidth, nativeHeight; int offsetX, offsetY; int hiresWidth, hiresHeight; };
extern "C" int Pc_ModernMesh_PrepareDraw(unsigned int, PcModernDrawBinding*);

int GR_DrawModernMesh(unsigned int mesh_handle)
{
	PcModernDrawBinding binding; GTEShader* modern;
	if (!Pc_ModernMesh_PrepareDraw(mesh_handle, &binding) || binding.vertexCount < 3)
		return 0;
	TextureID texture = binding.textureId; int format = binding.texFormat, count = binding.vertexCount;
	if (format == TF_4_BIT) modern=&g_modern_shader_4;
	else if (format == TF_8_BIT) modern=&g_modern_shader_8;
	else if (format == TF_16_BIT) modern=&g_modern_shader_16;
	else modern=&g_modern_shader_32_rgba;
	/* Modern item meshes are opaque replacement geometry. Do not inherit the
	 * previous legacy split's blend equation (notably reverse-subtract). */
	GR_SetBlendMode(BM_NONE);
	/* ...nor its depth COMPARISON. The PGXP static-world pass leaves
	 * glDepthFunc(GL_ALWAYS) so coplanar world faces resolve by OT order; every
	 * fragment then wins, and this mesh is a single glDrawArrays with no software
	 * culling, so its back faces paint straight over its front ones — the model
	 * reads as see-through. Per-vertex depth is already correct here
	 * (Pc_ModernDepth_Apply), only the comparison was wrong.
	 *
	 * This has to be issued BEFORE the draw. The state reset at the end of this
	 * function assigns g_PreviousDepthFuncAlways = 0 without ever calling
	 * glDepthFunc, so GL could sit at GL_ALWAYS while the tracker claimed LEQUAL —
	 * a desync that also made the next GR_SetDepthFuncAlways(0) early-out. */
	GR_SetDepthFuncAlways(0);
	GR_SetTextureShader(texture, (TexFormat)format, modern);
	GR_SetOverrideTextureSize(binding.nativeWidth, binding.nativeHeight, binding.offsetX, binding.offsetY, binding.hiresWidth, binding.hiresHeight);
	/* Item OTs are drawn in the ACTIVE display coordinate space, which is not a
	 * constant: the inventory carousel runs a 320x448 double-buffer (vertices for
	 * the lower buffer carry y=224..448) while the world item pickup runs 320x224.
	 * Hardcoding 448 here made the pickup project into an ortho twice as tall as
	 * its real display, halving every y and parking the model near the top of the
	 * frame; the carousel only looked correct because 448 happened to match it.
	 * Derive it the same way the legacy path does (GR_SetOffscreenState reads
	 * activeDispEnv.disp.w/h) so both cases are right for the same reason. */
	GR_Ortho2D(0.0f, (float)activeDispEnv.disp.w, (float)activeDispEnv.disp.h, 0.0f, -1.0f, 1.0f);
	GR_DrawTriangles(0, count / 3);
	glBindVertexArray(g_glVertexArray[g_curVertexBuffer ^ 1]);
	glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[g_curVertexBuffer ^ 1]);
	glUseProgram(0);
	g_PreviousShader = (ShaderID)-1; g_lastBoundTexture = (TextureID)-1;
	g_PreviousBlendMode = g_PreviousDepthMode = g_PreviousStencilMode = -999;
	g_PreviousDepthFuncAlways = 0; g_PreviousScissorState = g_PreviousOffscreenState = -999;
	return 1;
}

void GR_UpdateVertexBuffer(const GrVertex* vertices, int num_vertices)
{
	if (num_vertices >= MAX_VERTEX_BUFFER_SIZE)
	{
		eprinterr("MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors\n");
		num_vertices = MAX_VERTEX_BUFFER_SIZE;
	}

	//assert(num_vertices <= MAX_VERTEX_BUFFER_SIZE);
	GR_BindVertexBuffer();

#if USE_OPENGL
	/* ORPHAN the buffer before writing it.
	 *
	 * Overwriting storage the GPU may still be reading forces the driver to
	 * either block until those draws retire or silently shadow the allocation.
	 * GR_BindVertexBuffer alternates between two VBOs, which halves the odds but
	 * does not remove them: with several DrawAllSplits per frame each buffer
	 * comes back around while its previous contents can still be in flight.
	 *
	 * Passing NULL for the same size says "the old contents are dead" — the
	 * driver hands back fresh storage immediately and the in-flight draws keep
	 * reading the old block. Measured on a Mali-T720 (Arcade1Up cabinet), where
	 * a tiler defers work to end-of-tile and the buffer is almost always still
	 * busy: GL submission fell from 167.9 ms/frame to 7.3 ms, and an in-game
	 * cutscene went from ~6.7 fps to ~53 fps. The win scales with how long the
	 * GPU holds the buffer, so it is largest on tilers and shared-memory iGPUs
	 * and smallest on a discrete desktop card — but it is never a pessimisation,
	 * and the size is constant so drivers can recycle identical blocks. */
	glBufferData(GL_ARRAY_BUFFER, MAX_VERTEX_BUFFER_SIZE * sizeof(GrVertex), NULL, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, num_vertices * sizeof(GrVertex), vertices);
#else
#error
#endif
}

void GR_DrawTriangles(int start_vertex, int triangles)
{
#if USE_OPENGL
	if (s_gf.draws++ == 0)
		GreyFrame_FirstDraw();
	s_gf.tris += triangles;
	glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
#else
#error
#endif
}

void GR_PushDebugLabel(const char* label)
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPushDebugGroup)
		return;
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void GR_PopDebugLabel()
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPopDebugGroup)
		return;
	glPopDebugGroup();
#endif
}