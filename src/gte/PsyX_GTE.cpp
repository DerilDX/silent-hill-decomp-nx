#include "PsyX_GTE.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"

#include "psx/libgte.h"
#include "psx/gtereg.h"

#include <math.h>

extern "C" int GR_NeedViewSpaceData(void);



GTERegisters gteRegs;

#define GTE_SF(op)			((op >> 19) & 1)
#define GTE_MX(op)			((op >> 17) & 3)
#define GTE_V(op)			((op >> 15) & 3)
#define GTE_CV(op)			((op >> 13) & 3)
#define GTE_LM(op)			((op >> 10) & 1)
#define GTE_FUNCT(op)		(op & 63)

#define gteop(code)			(code & 0x1ffffff)

#define VX(n)				(n < 3 ? gteRegs.CP2D.p[ n << 1 ].sw.l : C2_IR1)
#define VY(n)				(n < 3 ? gteRegs.CP2D.p[ n << 1 ].sw.h : C2_IR2)
#define VZ(n)				(n < 3 ? gteRegs.CP2D.p[ (n << 1) + 1 ].sw.l : C2_IR3)
#define MX11(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) ].sw.l : -C2_R << 4)
#define MX12(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) ].sw.h : C2_R << 4)
#define MX13(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 1 ].sw.l : C2_IR0)
#define MX21(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 1 ].sw.h : C2_R13)
#define MX22(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 2 ].sw.l : C2_R13)
#define MX23(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 2 ].sw.h : C2_R13)
#define MX31(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 3 ].sw.l : C2_R22)
#define MX32(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 3 ].sw.h : C2_R22)
#define MX33(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 4 ].sw.l : C2_R22)
#define CV1(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 5 ].sd : 0)
#define CV2(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 6 ].sd : 0)
#define CV3(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 7 ].sd : 0)

#ifndef max
#   define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
#   define min(a, b) ((a) < (b) ? (a) : (b))
#endif


static int m_sf;
static long long m_mac0;
static long long m_mac3;

unsigned int gte_leadingzerocount(unsigned int lzcs) 
{
#if 0 // OLD AND SLOW WAY
	unsigned int lzcr = 0;

	if ((lzcs & 0x80000000) == 0)
		lzcs = ~lzcs;

	while ((lzcs & 0x80000000) != 0) {
		lzcr++;
		lzcs <<= 1;
	}

	return lzcr;
#endif

	if (!lzcs)
		return 32;

	// perform fast bit scan

	unsigned int lzcr = lzcs;
	static char debruijn32[32] = {
        0, 31, 9, 30, 3, 8, 13, 29, 2, 5, 7, 21, 12, 24, 28, 19,
        1, 10, 4, 14, 6, 22, 25, 20, 11, 15, 23, 26, 16, 27, 17, 18
    };

	lzcr |= lzcr >> 1;
	lzcr |= lzcr >> 2;
	lzcr |= lzcr >> 4;
	lzcr |= lzcr >> 8;
	lzcr |= lzcr >> 16;
	lzcr++;

    return debruijn32[lzcr * 0x076be629 >> 27];
}

int LIM(int value, int max, int min, unsigned int flag) {
	if (value > max) {
		C2_FLAG |= flag;
		return max;
	}
	else if (value < min) {
		C2_FLAG |= flag;
		return min;
	}

	return value;
}

#define _oB_ (gteRegs.GPR.r[_Rs_] + _Imm_)

inline long long gte_shift(long long a, int sf) {
	if (sf > 0)
		return a >> 12;
	else if (sf < 0)
		return a << 12;

	return a;
}

int BOUNDS(/*int44*/long long value, int max_flag, int min_flag) {
	if (value/*.positive_overflow()*/ > (long long)0x7ffffffffff)
		C2_FLAG |= max_flag;

	if (value/*.negative_overflow()*/ < (long long)-0x8000000000)
		C2_FLAG |= min_flag;

	return int(gte_shift(value/*.value()*/, m_sf));
}

unsigned int gte_divide(unsigned short numerator, unsigned short denominator)
{
	if (numerator < (denominator * 2))
	{
		static unsigned char table[] =
		{
			0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
			0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5, 0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
			0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb, 0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
			0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
			0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
			0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
			0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
			0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
			0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
			0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
			0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
			0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
			0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
			0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
			0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
			0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
			0x00
		};

		int shift = gte_leadingzerocount(denominator) - 16;

		int r1 = (denominator << shift) & 0x7fff;
		int r2 = table[((r1 + 0x40) >> 7)] + 0x101;
		int r3 = ((0x80 - (r2 * (r1 + 0x8000))) >> 8) & 0x1ffff;
		unsigned int reciprocal = ((r2 * r3) + 0x80) >> 8;

		return (unsigned int)((((unsigned long long)reciprocal * (numerator << shift)) + 0x8000) >> 16);
	}

	return 0xffffffff;
}

/* Setting bits 12 & 19-22 in FLAG does not set bit 31 */

int A1(/*int44*/long long a) { return BOUNDS(a, (1 << 31) | (1 << 30), (1 << 31) | (1 << 27)); }
int A2(/*int44*/long long a) { return BOUNDS(a, (1 << 31) | (1 << 29), (1 << 31) | (1 << 26)); }
int A3(/*int44*/long long a) { m_mac3 = a; return BOUNDS(a, (1 << 31) | (1 << 28), (1 << 31) | (1 << 25)); }
int Lm_B1(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 24)); }
int Lm_B2(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 23)); }
int Lm_B3(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 22)); }

int Lm_B3_sf(long long value, int sf, int lm) {
	int value_sf = int(gte_shift(value, sf));
	int value_12 = int(gte_shift(value, 1));
	int max = 0x7fff;
	int min = 0;
	if (lm == 0)
		min = -0x8000;

	if (value_12 < -0x8000 || value_12 > 0x7fff)
		C2_FLAG |= (1 << 22);

	if (value_sf > max)
		return max;
	else if (value_sf < min)
		return min;

	return value_sf;
}

int Lm_C1(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 21)); }
int Lm_C2(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 20)); }
int Lm_C3(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 19)); }
int Lm_D(long long a, int sf) { return LIM(int(gte_shift(a, sf)), 0xffff, 0x0000, (1 << 31) | (1 << 18)); }

unsigned int Lm_E(unsigned int result) {
	if (result == 0xffffffff) {
		C2_FLAG |= (1 << 31) | (1 << 17);
		return 0x1ffff;
	}

	if (result > 0x1ffff)
		return 0x1ffff;

	return result;
}

long long F(long long a) {
	m_mac0 = a;

	if (a > 0x7fffffffLL)
		C2_FLAG |= (1 << 31) | (1 << 16);

	if (a < -0x80000000LL)
		C2_FLAG |= (1 << 31) | (1 << 15);

	return a;
}

int Lm_G1(long long a) {
	if (a > 0x3ff) {
		C2_FLAG |= (1 << 31) | (1 << 14);
		return 0x3ff;
	}
	if (a < -0x400) {
		C2_FLAG |= (1 << 31) | (1 << 14);
		return -0x400;
	}

	return int(a);
}

int Lm_G2(long long a) {
	if (a > 0x3ff) {
		C2_FLAG |= (1 << 31) | (1 << 13);
		return 0x3ff;
	}

	if (a < -0x400) {
		C2_FLAG |= (1 << 31) | (1 << 13);
		return -0x400;
	}

	return int(a);
}

int Lm_G1_ia(long long a) {
	if (a > 0x3ffffff)
		return 0x3ffffff;

	if (a < -0x4000000)
		return -0x4000000;

	return int(a);
}

int Lm_G2_ia(long long a) {
	if (a > 0x3ffffff)
		return 0x3ffffff;

	if (a < -0x4000000)
		return -0x4000000;

	return int(a);
}

int Lm_H(long long value, int sf) {
	long long value_sf = gte_shift(value, sf);
	int value_12 = int(gte_shift(value, 1));
	int max = 0x1000;
	int min = 0x0000;

	if (value_sf < min || value_sf > max)
		C2_FLAG |= (1 << 12);

	if (value_12 > max)
		return max;

	if (value_12 < min)
		return min;

	return value_12;
}



/* PGXP precise screen-coord FIFO, mirrors the GTE SXY0/SXY1/SXY2 FIFO so the
 * store macros can resolve a destination address to the precise float coord
 * the GTE just produced. Updated only when g_PsxUsePgxp. */
static float s_pgxpFifoX[3], s_pgxpFifoY[3], s_pgxpFifoW[3];

/* Parallel FIFO for the per-pixel flashlight: the GTE RTPS view-space (camera-
 * space) position MAC1/MAC2/MAC3 of each projected vertex. Mirrors the PGXP
 * SXY FIFO so the store hook can resolve a destination address to the view
 * position the GTE just produced. Updated only when g_PsyX_UsePerPixelFlashlight. */
static float s_vsFifoX[3], s_vsFifoY[3], s_vsFifoZ[3];

/* Projection registers per FIFO slot. SH1 reprograms OFX/OFY/H mid-frame (a
 * lighting helper runs SetGeomOffset(-1024,-1024)/SetGeomScreen(16) and restores
 * only the GTE side), and the near clipper consumes these at DrawOTag time, so
 * each vertex has to carry its own instead of reading a frame-global. */
static float s_vsFifoOfx[3], s_vsFifoOfy[3], s_vsFifoH[3];

extern "C" int g_PgxpUseUnquantizedDepth; /* defined in PsyX_GPU.cpp */
extern "C" float g_PgxpGteOfx, g_PgxpGteOfy, g_PgxpGteH; /* PsyX_GPU.cpp */
extern "C" void VShadow_Store(void* addr, float x, float y, float z,
                              float ofx, float ofy, float h); /* PsyX_GPU.cpp */

/* Whole-town render mode (set per world-chunk-draw by the game from
 * Pc_WholeMapDrawActive). When set, vertices whose true view depth exceeds what
 * the GTE depth register can hold (SZ3 clamps at 0xffff) OR whose view X/Y clamp
 * out of s16 (IR1/IR2) are re-projected from the UNCLAMPED view position so the
 * whole town projects at the correct scale instead of freezing ~6 cells out.
 * 0 (default) = untouched legacy path, byte-identical. Defined in PsyX_GPU.cpp. */
extern "C" int g_PsxWholeMapFar;
extern "C" int g_PsxWholeMapLastSz; /* true (unclamped) SZ of the newest RTPS, whole-map only */

/* Called from the gte_stsxy* store macros (only when g_PsxUsePgxp): the macro
 * just wrote the integer screen coord for FIFO slot `slot` (SXY0=0, SXY1=1,
 * SXY2=2) to `addr`, so record the shadow keyed by that address, validated by
 * the integer value the macro left there (DuckStation's SWC2 hook). */
extern "C" void Shadow_Store(void* addr, float x, float y, float w, unsigned value);
extern "C" void PGXP_StoreAddr(void* addr, int slot)
{
	if ((unsigned)slot > 2u) return;
	/* The gte_stsxy* macros now call this when (g_PsxUsePgxp ||
	 * g_PsyX_UsePerPixelFlashlight), so each store is independently gated:
	 * PGXP unchanged when its flag is off, flashlight a no-op when its flag is
	 * off, both off => not called at all (byte-identical legacy path). */
	if (g_PsxUsePgxp)
		Shadow_Store(addr, s_pgxpFifoX[slot], s_pgxpFifoY[slot], s_pgxpFifoW[slot], *(unsigned*)addr);
	/* View-space shadow also feeds the PGXP near-plane clipper, so it must be
	 * recorded whenever PGXP is on, not just for the per-pixel flashlight. Gate
	 * matches the vs FIFO fill in GTE_RotTransPers below. */
	if (GR_NeedViewSpaceData())
		VShadow_Store(addr, s_vsFifoX[slot], s_vsFifoY[slot], s_vsFifoZ[slot],
		              s_vsFifoOfx[slot], s_vsFifoOfy[slot], s_vsFifoH[slot]);
}

/* ===================== Exact-transform PGXP twins ==========================
 * Unquantized camera / model / vertex transforms captured at the GTE macro
 * layer BEFORE the Q12->Q8 truncation. RTPS (below) re-projects a coherent
 * unclamped view tuple from them instead of the saturated integer registers,
 * killing the residual distance jitter / geometry cuts the quantized path has.
 *
 * Storage mirrors the flat gen-stamped open-addressed s_shadow table in
 * PsyX_GPU.cpp exactly (16-probe, no per-frame clear — the shared s_pgxpGen
 * bump IS the clear, so there is no heap and no allocation). Every capture
 * entry point early-returns when !g_PsxUsePgxp, so the PGXP-off path touches
 * none of this and stays byte-identical + zero-cost. */
extern "C" unsigned PGXP_CurGen(void); /* PsyX_GPU.cpp: s_pgxpGen, bumped once/frame */

namespace {

#define TWIN_BITS 14
#define TWIN_SIZE (1u << TWIN_BITS)
#define TWIN_MASK (TWIN_SIZE - 1u)
static inline unsigned TwinHash(uintptr_t k) { return (unsigned)((k >> 2) * 2654435761u) & TWIN_MASK; }

/* Matrix (rotation) twin: q12 = the nine Q12 coefficients as they live in the
 * MATRIX (the content key), exact = their unquantized doubles. valid=false is a
 * deliberate "do not trust an older exact twin at this address" marker. */
struct MatTwin   { uintptr_t key; unsigned gen; short q12[9]; double exact[9]; bool valid; };
struct TransTwin { uintptr_t key; unsigned gen; int   q12[3]; double exact[3]; bool valid; };
struct VecTwin   { uintptr_t key; unsigned gen; short q12[3]; double exact[3]; bool valid; };

static MatTwin   s_matTwin[TWIN_SIZE];
static TransTwin s_transTwin[TWIN_SIZE];
static VecTwin   s_vecTwin[TWIN_SIZE];

/* ---- content keys (live GTE registers vs the source in memory) ---- */
static void MatKeyMem(const void* m, short k[9]) {
	const short* v = (const short*)m;
	for (int i = 0; i < 9; ++i) k[i] = v[i];
}
static void MatKeyGte(short k[9]) {
	k[0] = C2_R11; k[1] = C2_R12; k[2] = C2_R13;
	k[3] = C2_R21; k[4] = C2_R22; k[5] = C2_R23;
	k[6] = C2_R31; k[7] = C2_R32; k[8] = C2_R33;
}
static void TransKeyMem(const void* m, int k[3]) {
	const MATRIX* mat = (const MATRIX*)m;
	k[0] = (int)mat->t[0]; k[1] = (int)mat->t[1]; k[2] = (int)mat->t[2];
}
static void TransKeyGte(int k[3]) { k[0] = C2_TRX; k[1] = C2_TRY; k[2] = C2_TRZ; }
static void VecKeyMem(const void* v, short k[3]) {
	const short* s = (const short*)v; k[0] = s[0]; k[1] = s[1]; k[2] = s[2];
}
static void VecKeyGte(int slot, short k[3]) {
	k[0] = (short)VX(slot); k[1] = (short)VY(slot); k[2] = (short)VZ(slot);
}
static bool Eq9(const short* a, const short* b) { for (int i = 0; i < 9; ++i) if (a[i] != b[i]) return false; return true; }
static bool Eq3i(const int* a, const int* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }
static bool Eq3s(const short* a, const short* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

static bool IdentityKey(const short k[9]) {
	static const short id[9] = { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 };
	return Eq9(k, id);
}
static bool AspectIdentityKey(const short k[9]) {
	/* A 3/4 Y scale, exactly representable, so PGXP can shadow it without
	 * quantization error if a game builds one. NOT Psy-Q's GsIDMATRIX2, which
	 * is a plain identity (see libgs_stub.c) -- this used to claim it was. */
	static const short id[9] = { 4096, 0, 0, 0, 3072, 0, 0, 0, 4096 };
	return Eq9(k, id);
}
static void ExactIdentity(double e[9]) {
	static const double id[9] = { 1,0,0, 0,1,0, 0,0,1 }; for (int i = 0; i < 9; ++i) e[i] = id[i];
}
static void ExactAspectIdentity(double e[9]) {
	static const double id[9] = { 1,0,0, 0,0.75,0, 0,0,1 }; for (int i = 0; i < 9; ++i) e[i] = id[i];
}
static void ExactQuantized(const short k[9], double e[9]) { for (int i = 0; i < 9; ++i) e[i] = (double)k[i] / 4096.0; }

/* ---- flat gen-stamped tables (Shadow_Put/Get idiom) ---- */
static MatTwin* MatSlotPut(uintptr_t k) {
	unsigned gen = PGXP_CurGen(), s = TwinHash(k);
	for (int i = 0; i < 16; ++i) { MatTwin* e = &s_matTwin[(s + i) & TWIN_MASK];
		if (e->key == k || e->key == 0 || e->gen != gen) return e; }
	return &s_matTwin[s];
}
static MatTwin* MatSlotGet(uintptr_t k) {
	unsigned gen = PGXP_CurGen(), s = TwinHash(k);
	for (int i = 0; i < 16; ++i) { MatTwin* e = &s_matTwin[(s + i) & TWIN_MASK];
		if (e->key == k) return (e->gen == gen) ? e : nullptr;
		if (e->key == 0) return nullptr; }
	return nullptr;
}
static TransTwin* TransSlotPut(uintptr_t k) {
	unsigned gen = PGXP_CurGen(), s = TwinHash(k);
	for (int i = 0; i < 16; ++i) { TransTwin* e = &s_transTwin[(s + i) & TWIN_MASK];
		if (e->key == k || e->key == 0 || e->gen != gen) return e; }
	return &s_transTwin[s];
}
static TransTwin* TransSlotGet(uintptr_t k) {
	unsigned gen = PGXP_CurGen(), s = TwinHash(k);
	for (int i = 0; i < 16; ++i) { TransTwin* e = &s_transTwin[(s + i) & TWIN_MASK];
		if (e->key == k) return (e->gen == gen) ? e : nullptr;
		if (e->key == 0) return nullptr; }
	return nullptr;
}
static VecTwin* VecSlotPut(uintptr_t k) {
	unsigned gen = PGXP_CurGen(), s = TwinHash(k);
	for (int i = 0; i < 16; ++i) { VecTwin* e = &s_vecTwin[(s + i) & TWIN_MASK];
		if (e->key == k || e->key == 0 || e->gen != gen) return e; }
	return &s_vecTwin[s];
}
static VecTwin* VecSlotGet(uintptr_t k) {
	unsigned gen = PGXP_CurGen(), s = TwinHash(k);
	for (int i = 0; i < 16; ++i) { VecTwin* e = &s_vecTwin[(s + i) & TWIN_MASK];
		if (e->key == k) return (e->gen == gen) ? e : nullptr;
		if (e->key == 0) return nullptr; }
	return nullptr;
}

static void RegisterMatrix(const void* m, const short k[9], const double e[9]) {
	MatTwin* t = MatSlotPut((uintptr_t)m);
	t->key = (uintptr_t)m; t->gen = PGXP_CurGen(); t->valid = true;
	for (int i = 0; i < 9; ++i) { t->q12[i] = k[i]; t->exact[i] = e[i]; }
}
static void RegisterMatrixInvalid(const void* m, const short k[9]) {
	MatTwin* t = MatSlotPut((uintptr_t)m);
	t->key = (uintptr_t)m; t->gen = PGXP_CurGen(); t->valid = false;
	for (int i = 0; i < 9; ++i) t->q12[i] = k[i];
}
/* Always yields SOME exact (quantized worst case), so callers that need current
 * precision can always latch. */
static bool LookupMatrix(const void* m, double e[9]) {
	short key[9]; MatKeyMem(m, key);
	MatTwin* t = MatSlotGet((uintptr_t)m);
	if (t && Eq9(t->q12, key)) {
		if (!t->valid) { ExactQuantized(key, e); RegisterMatrix(m, key, e); return true; }
		for (int i = 0; i < 9; ++i) e[i] = t->exact[i]; return true;
	}
	if (IdentityKey(key))       { ExactIdentity(e);       RegisterMatrix(m, key, e); return true; }
	if (AspectIdentityKey(key)) { ExactAspectIdentity(e); RegisterMatrix(m, key, e); return true; }
	ExactQuantized(key, e); RegisterMatrix(m, key, e); return true;
}

static void RegisterTranslation(const void* m, const int k[3], const double e[3]) {
	TransTwin* t = TransSlotPut((uintptr_t)m);
	t->key = (uintptr_t)m; t->gen = PGXP_CurGen(); t->valid = true;
	for (int i = 0; i < 3; ++i) { t->q12[i] = k[i]; t->exact[i] = e[i]; }
}
static void RegisterTranslationInvalid(const void* m, const int k[3]) {
	TransTwin* t = TransSlotPut((uintptr_t)m);
	t->key = (uintptr_t)m; t->gen = PGXP_CurGen(); t->valid = false;
	for (int i = 0; i < 3; ++i) t->q12[i] = k[i];
}
static bool LookupTranslation(const void* m, double e[3]) {
	int key[3]; TransKeyMem(m, key);
	TransTwin* t = TransSlotGet((uintptr_t)m);
	if (t && Eq3i(t->q12, key)) {
		if (!t->valid) { for (int i = 0; i < 3; ++i) e[i] = (double)key[i]; RegisterTranslation(m, key, e); return true; }
		for (int i = 0; i < 3; ++i) e[i] = t->exact[i]; return true;
	}
	/* An integer GTE translation is exact in its own units: unknown provenance
	 * degrades to the legacy value, never rejected. */
	for (int i = 0; i < 3; ++i) e[i] = (double)key[i];
	RegisterTranslation(m, key, e); return true;
}

static void RegisterVector(const void* v, const short k[3], const double e[3]) {
	VecTwin* t = VecSlotPut((uintptr_t)v);
	t->key = (uintptr_t)v; t->gen = PGXP_CurGen(); t->valid = true;
	for (int i = 0; i < 3; ++i) { t->q12[i] = k[i]; t->exact[i] = e[i]; }
}
static bool LookupVector(const void* v, double e[3]) {
	short key[3]; VecKeyMem(v, key);
	VecTwin* t = VecSlotGet((uintptr_t)v);
	if (t && t->valid && Eq3s(t->q12, key)) { for (int i = 0; i < 3; ++i) e[i] = t->exact[i]; return true; }
	return false;
}

/* ---- current-latched transforms consumed by RTPS ---- */
struct CurrentMat  { short key[9]; double exact[9]; bool valid; };
struct CurrentTr   { int   key[3]; double exact[3]; bool valid; };
struct CurrentVec  { short key[3]; double exact[3]; bool valid; };
static CurrentMat s_currentRotation;
static CurrentTr  s_currentTranslation;
static CurrentVec s_currentVector[3];

static bool CurrentExact(double e[9]) {
	short key[9]; MatKeyGte(key);
	if (s_currentRotation.valid && Eq9(s_currentRotation.key, key)) { for (int i = 0; i < 9; ++i) e[i] = s_currentRotation.exact[i]; return true; }
	if (IdentityKey(key))       { ExactIdentity(e);       return true; }
	if (AspectIdentityKey(key)) { ExactAspectIdentity(e); return true; }
	return false;
}
static bool CurrentExactTranslation(double e[3]) {
	int key[3]; TransKeyGte(key);
	if (s_currentTranslation.valid && Eq3i(s_currentTranslation.key, key)) { for (int i = 0; i < 3; ++i) e[i] = s_currentTranslation.exact[i]; return true; }
	return false;
}
static bool CurrentExactVector(int slot, double e[3]) {
	if ((unsigned)slot > 2u) return false;
	short key[3]; VecKeyGte(slot, key);
	if (s_currentVector[slot].valid && Eq3s(s_currentVector[slot].key, key)) { for (int i = 0; i < 3; ++i) e[i] = s_currentVector[slot].exact[i]; return true; }
	return false;
}

/* Column-fed multiply (gte_ldclmv / gte_stclmv), stateful across 3 invocations. */
struct ColumnMul { const char* inBase; char* outBase; short inKey[9]; double result[9]; int column; bool valid; };
static ColumnMul s_columnMultiply;

} // namespace

/* ------------------------- extern "C" capture API --------------------------
 * The bridge the shared inline_c.h GTE macros and the libgte.c matrix ops call.
 * Every one folds `if (!g_PsxUsePgxp) return;` in as its first act, so with
 * PGXP off none of the tables above are ever touched. */

extern "C" void PGXP_MatrixRegister(const MATRIX* matrix, const double exactValues[9]) {
	if (!g_PsxUsePgxp || !matrix || !exactValues) return;
	short key[9]; MatKeyMem(matrix, key);
	RegisterMatrix(matrix, key, exactValues);
}
extern "C" int PGXP_MatrixLookup(const MATRIX* matrix, double exactValues[9]) {
	if (!g_PsxUsePgxp || !matrix || !exactValues) return 0;
	return LookupMatrix(matrix, exactValues) ? 1 : 0;
}
extern "C" int PGXP_MatrixLookupCurrent(double exactValues[9]) {
	if (!g_PsxUsePgxp || !exactValues) return 0;
	return CurrentExact(exactValues) ? 1 : 0;
}
extern "C" void PGXP_MatrixCopy(MATRIX* dst, const MATRIX* src) {
	if (!g_PsxUsePgxp || !dst || !src) return;
	double e[9];
	if (LookupMatrix(src, e)) { short k[9]; MatKeyMem(dst, k); RegisterMatrix(dst, k, e); }
	else { short k[9]; MatKeyMem(dst, k); RegisterMatrixInvalid(dst, k); }
}
extern "C" void PGXP_MatrixRegisterTranslation(MATRIX* matrix, const double exactValues[3]) {
	if (!g_PsxUsePgxp || !matrix || !exactValues) return;
	int k[3]; TransKeyMem(matrix, k); RegisterTranslation(matrix, k, exactValues);
}
extern "C" void PGXP_MatrixRegisterTranslationQ12(MATRIX* matrix, int x, int y, int z) {
	if (!g_PsxUsePgxp || !matrix) return;
	/* Validate the producer's Q12->Q8 result before trusting its four discarded
	 * fractional bits. */
	if ((int)matrix->t[0] != (x >> 4) || (int)matrix->t[1] != (y >> 4) || (int)matrix->t[2] != (z >> 4)) {
		int k[3]; TransKeyMem(matrix, k); RegisterTranslationInvalid(matrix, k); return;
	}
	int k[3]; TransKeyMem(matrix, k);
	double e[3] = { (double)x / 16.0, (double)y / 16.0, (double)z / 16.0 };
	RegisterTranslation(matrix, k, e);
}
extern "C" int PGXP_MatrixLookupTranslation(const MATRIX* matrix, double exactValues[3]) {
	if (!g_PsxUsePgxp || !matrix || !exactValues) return 0;
	return LookupTranslation(matrix, exactValues) ? 1 : 0;
}
extern "C" void PGXP_MatrixInvalidateTranslation(MATRIX* matrix) {
	if (!g_PsxUsePgxp || !matrix) return;
	int k[3]; TransKeyMem(matrix, k); RegisterTranslationInvalid(matrix, k);
}
extern "C" void PGXP_MatrixCopyFull(MATRIX* dst, const MATRIX* src) {
	if (!g_PsxUsePgxp || !dst || !src) return;
	PGXP_MatrixCopy(dst, src);
	double e[3];
	if (LookupTranslation(src, e)) { int k[3]; TransKeyMem(dst, k); RegisterTranslation(dst, k, e); }
	else PGXP_MatrixInvalidateTranslation(dst);
}
extern "C" void PGXP_VectorRegisterFixed(const void* vector, int x, int y, int z, int shift) {
	if (!g_PsxUsePgxp || !vector || shift < 0 || shift > 30) return;
	short k[3]; VecKeyMem(vector, k);
	if ((int)k[0] != (x >> shift) || (int)k[1] != (y >> shift) || (int)k[2] != (z >> shift)) return;
	const double scale = (double)(1u << shift);
	double e[3] = { (double)x / scale, (double)y / scale, (double)z / scale };
	RegisterVector(vector, k, e);
}
extern "C" void PGXP_VectorRegisterQ12(const void* vector, int x, int y, int z) {
	PGXP_VectorRegisterFixed(vector, x, y, z, 4);
}
extern "C" void PGXP_MatrixInvalidate(MATRIX* matrix) {
	if (!g_PsxUsePgxp || !matrix) return;
	short k[9]; MatKeyMem(matrix, k); RegisterMatrixInvalid(matrix, k);
}
extern "C" void PGXP_MatrixInvalidateCurrent(void) {
	if (!g_PsxUsePgxp) return;
	s_currentRotation.valid = false; s_columnMultiply = ColumnMul{};
}
extern "C" void PGXP_MatrixInvalidateCurrentTranslation(void) { if (!g_PsxUsePgxp) return; s_currentTranslation.valid = false; }
extern "C" void PGXP_VectorInvalidateCurrent(int slot) { if (!g_PsxUsePgxp) return; if ((unsigned)slot <= 2u) s_currentVector[slot].valid = false; }

extern "C" void PGXP_MatrixSetRot(const void* matrix) {
	if (!g_PsxUsePgxp) return;
	s_currentRotation.valid = false; s_columnMultiply = ColumnMul{};
	if (!matrix) return;
	short memKey[9], gteKey[9]; MatKeyMem(matrix, memKey); MatKeyGte(gteKey);
	double e[9];
	if (Eq9(memKey, gteKey) && LookupMatrix(matrix, e)) {
		for (int i = 0; i < 9; ++i) { s_currentRotation.key[i] = gteKey[i]; s_currentRotation.exact[i] = e[i]; }
		s_currentRotation.valid = true;
	}
}
extern "C" void PGXP_MatrixSetTrans(const void* matrix) {
	if (!g_PsxUsePgxp) return;
	s_currentTranslation.valid = false;
	if (!matrix) return;
	int memKey[3], gteKey[3]; TransKeyMem(matrix, memKey); TransKeyGte(gteKey);
	double e[3];
	if (Eq3i(memKey, gteKey) && LookupTranslation(matrix, e)) {
		for (int i = 0; i < 3; ++i) { s_currentTranslation.key[i] = gteKey[i]; s_currentTranslation.exact[i] = e[i]; }
		s_currentTranslation.valid = true;
	}
}
extern "C" void PGXP_VectorLoad(const void* vector, int slot) {
	if (!g_PsxUsePgxp) return;
	if ((unsigned)slot > 2u) return;
	s_currentVector[slot].valid = false;
	if (!vector) return;
	short memKey[3], gteKey[3]; VecKeyMem(vector, memKey); VecKeyGte(slot, gteKey);
	double e[3];
	if (Eq3s(memKey, gteKey) && LookupVector(vector, e)) {
		for (int i = 0; i < 3; ++i) { s_currentVector[slot].key[i] = gteKey[i]; s_currentVector[slot].exact[i] = e[i]; }
		s_currentVector[slot].valid = true;
	}
}
extern "C" void PGXP_MatrixCaptureCurrent(void* matrix) {
	if (!g_PsxUsePgxp || !matrix) return;
	short memKey[9], gteKey[9]; MatKeyMem(matrix, memKey); MatKeyGte(gteKey);
	double e[9];
	if (Eq9(memKey, gteKey) && CurrentExact(e)) RegisterMatrix(matrix, memKey, e);
	else PGXP_MatrixInvalidate((MATRIX*)matrix);
	int tMem[3], tGte[3]; TransKeyMem(matrix, tMem); TransKeyGte(tGte);
	double te[3];
	if (Eq3i(tMem, tGte) && CurrentExactTranslation(te)) RegisterTranslation(matrix, tMem, te);
	else PGXP_MatrixInvalidateTranslation((MATRIX*)matrix);
}
extern "C" void PGXP_MatrixLoadColumn(const void* columnPtr) {
	if (!g_PsxUsePgxp) return;
	if (!columnPtr) { s_columnMultiply.valid = false; return; }
	if (s_columnMultiply.column == 0) {
		s_columnMultiply = ColumnMul{};
		s_columnMultiply.inBase = (const char*)columnPtr;
		double lhs[9], rhs[9];
		if (CurrentExact(lhs) && LookupMatrix(columnPtr, rhs)) {
			MatKeyMem(columnPtr, s_columnMultiply.inKey);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 3; ++col)
					s_columnMultiply.result[row * 3 + col] =
						lhs[row * 3 + 0] * rhs[0 * 3 + col] +
						lhs[row * 3 + 1] * rhs[1 * 3 + col] +
						lhs[row * 3 + 2] * rhs[2 * 3 + col];
			s_columnMultiply.valid = true;
		}
	} else {
		const int col = s_columnMultiply.column;
		if ((const char*)columnPtr != s_columnMultiply.inBase + col * (int)sizeof(short)) s_columnMultiply.valid = false;
		if (s_columnMultiply.valid) {
			const short* p = (const short*)columnPtr;
			for (int row = 0; row < 3; ++row) if (p[row * 3] != s_columnMultiply.inKey[row * 3 + col]) s_columnMultiply.valid = false;
		}
	}
}
extern "C" void PGXP_MatrixStoreColumn(void* columnPtr) {
	if (!g_PsxUsePgxp) return;
	if (!columnPtr) { s_columnMultiply = ColumnMul{}; return; }
	const int col = s_columnMultiply.column;
	if (col == 0) {
		s_columnMultiply.outBase = (char*)columnPtr;
		/* Don't let an old address twin escape while this matrix is only partly
		 * overwritten. */
		short zero[9] = {0,0,0,0,0,0,0,0,0}; RegisterMatrixInvalid(columnPtr, zero);
	} else if ((char*)columnPtr != s_columnMultiply.outBase + col * (int)sizeof(short)) {
		s_columnMultiply.valid = false;
	}
	if (col == 2) {
		if (s_columnMultiply.valid) { short k[9]; MatKeyMem(columnPtr == s_columnMultiply.outBase ? columnPtr : s_columnMultiply.outBase, k); RegisterMatrix(s_columnMultiply.outBase, k, s_columnMultiply.result); }
		else if (s_columnMultiply.outBase) { short k[9]; MatKeyMem(s_columnMultiply.outBase, k); RegisterMatrixInvalid(s_columnMultiply.outBase, k); }
		s_columnMultiply = ColumnMul{};
	} else {
		s_columnMultiply.column = col + 1;
	}
}

int GTE_RotTransPers(int idx, int lm)
{
	int h_over_sz3;

	/* Whole-map far override: carries the unclamped re-projection of this vertex
	 * (computed once below) into the PGXP FIFO so PGXP-on gets correct far screen
	 * pos + depth too. wmFarHit stays false on the legacy path. */
	bool   wmFarHit = false;
	double wmFarX = 0.0, wmFarY = 0.0;
	float  wmFarW = 0.0f;

	C2_MAC1 = A1(/*int44*/(long long)((long long)C2_TRX << 12) + (C2_R11 * VX(idx)) + (C2_R12 * VY(idx)) + (C2_R13 * VZ(idx)));
	C2_MAC2 = A2(/*int44*/(long long)((long long)C2_TRY << 12) + (C2_R21 * VX(idx)) + (C2_R22 * VY(idx)) + (C2_R23 * VZ(idx)));
	C2_MAC3 = A3(/*int44*/(long long)((long long)C2_TRZ << 12) + (C2_R31 * VX(idx)) + (C2_R32 * VY(idx)) + (C2_R33 * VZ(idx)));
	C2_IR1 = Lm_B1(C2_MAC1, lm);
	C2_IR2 = Lm_B2(C2_MAC2, lm);
	C2_IR3 = Lm_B3_sf(m_mac3, m_sf, lm);
	C2_SZ0 = C2_SZ1;
	C2_SZ1 = C2_SZ2;
	C2_SZ2 = C2_SZ3;
	C2_SZ3 = Lm_D(m_mac3, 1);
	h_over_sz3 = Lm_E(gte_divide(C2_H, C2_SZ3));
	C2_SXY0 = C2_SXY1;
	C2_SXY1 = C2_SXY2;
	C2_SX2 = Lm_G1(F((long long)C2_OFX + ((long long)C2_IR1 * h_over_sz3)) >> 16);
	C2_SY2 = Lm_G2(F((long long)C2_OFY + ((long long)C2_IR2 * h_over_sz3)) >> 16);

	/* Whole-map far re-projection. The standard path above divides IR1/IR2 (view
	 * X/Y clamped to s16) by SZ3 (view depth clamped to 0xffff), so beyond the
	 * clamp the projection freezes. C2_MAC1/C2_MAC2 are the UNCLAMPED analogs of
	 * IR1/IR2, and gte_shift(m_mac3,1) is the UNCLAMPED analog of SZ3 (identical
	 * in range, no [0,0xffff] cap). Recompute in double precision only for
	 * vertices where a clamp actually fired, and overwrite the on-screen coord
	 * (still range-limited by Lm_G to the ±0x400 screen box; off-screen far
	 * geometry clamps to the edge exactly as a near off-screen vertex would).
	 * Gated on g_PsxWholeMapFar so the legacy path is byte-identical when off. */
	if (g_PsxWholeMapFar)
	{
		long long fz = gte_shift(m_mac3, 1); /* unclamped SZ3 */
		/* Export the true depth of the newest projection for game-side far
		 * consumers (billboard size divisor: SZ3 pegs at 0xFFFF ~256u, freezing
		 * sprite scale; this carries the real value past the clamp). */
		g_PsxWholeMapLastSz = (fz > 0x7FFFFFFFLL) ? 0x7FFFFFFF : ((fz < 0) ? 0 : (int)fz);
		if (fz > 0 && (fz > 0xffff || C2_MAC1 != C2_IR1 || C2_MAC2 != C2_IR2))
		{
			double r   = (double)C2_H / (double)fz;
			double fsx = (double)C2_OFX / 65536.0 + (double)C2_MAC1 * r;
			double fsy = (double)C2_OFY / 65536.0 + (double)C2_MAC2 * r;
			C2_SX2 = Lm_G1((long long)(fsx + (fsx >= 0.0 ? 0.5 : -0.5)));
			C2_SY2 = Lm_G2((long long)(fsy + (fsy >= 0.0 ? 0.5 : -0.5)));
			wmFarHit = true;
			wmFarX = fsx;
			wmFarY = fsy;
			wmFarW = (float)fz; /* unclamped depth -> correct perspective + GL depth via PGXP */
		}
	}

	/* PGXP: stash the full-precision projection keyed by the clamped integer screen
	 * coord the prim will store. Gated — zero cost / zero effect when PGXP is off. */
	if (g_PsxUsePgxp)
	{
		/* Project with a TRUE float divide, NOT the GTE's h_over_sz3. h_over_sz3 is the
		 * hardware UNR divide SATURATED by Lm_E to 0x1ffff (H/SZ3 capped at ~2.0) as soon
		 * as SZ3 < H/2, so geometry close to the camera got a WRONG precise coord AND was
		 * forced to affine (W=0) below. A poly straddling that threshold then renders
		 * half-perspective / half-affine and smears at the screen edge. The float divide
		 * has no cap: every in-front vertex (SZ3 > 0) projects correctly and stays on the
		 * perspective path, so the whole poly is consistent. (For SZ3 >= H/2 this is
		 * identical to the old path — only the previously-broken close case changes.)
		 * W = view-space SZ3 (only the per-vertex ratio matters; absolute scale cancels). */
		double fx, fy;
		float  pgxpW;
		if (C2_SZ3 > 0) {
			double ratio = (double)C2_H / (double)C2_SZ3;            /* H/SZ3, UNclamped */
			fx = (double)C2_OFX / 65536.0 + (double)C2_IR1 * ratio;
			fy = (double)C2_OFY / 65536.0 + (double)C2_IR2 * ratio;
			/* W for the shader's perspective divide. C2_SZ3 is the GTE's CLAMPED 16-bit
			 * depth register; two independent GTE calls for a shared edge quantize to
			 * SZ3 values 1-2 apart, so the per-vertex 1/W diverges and the perspective
			 * interpolation across the shared edge mismatches -> seam that gets more
			 * visible with distance. gte_shift(m_mac3,1) is the SAME shift Lm_D applies
			 * to make SZ3 but WITHOUT the [0,0xffff] clamp -> full precision, so coincident
			 * edges get matching W. Toggle (pgxpdepth 0/1) for A/B. */
			pgxpW = g_PgxpUseUnquantizedDepth ? (float)gte_shift(m_mac3, 1) : (float)C2_SZ3;
		} else {
			/* SZ3 == 0: at / behind the near plane, no valid projection -> affine (W=0). */
			fx = (double)C2_SX2; fy = (double)C2_SY2;
			pgxpW = 0.0f;
		}

		/* Exact-transform twin override: when the unquantized camera rotation AND
		 * translation are both latched for this draw, re-project from full-precision
		 * view space instead of the clamped s16 IR1/IR2 / 16-bit SZ3. The integer GTE
		 * registers above stay bit-identical; only these float FIFO locals change.
		 * This is what removes the residual distance jitter and geometry cuts:
		 * coincident edges from independent GTE calls now share one exact projection,
		 * and view coords are no longer frozen by the IR/SZ3 saturation. */
		{
			extern int g_PsxPgxpDbg;
			double exR[9], exT[3], exV[3];
			if (!(g_PsxPgxpDbg & 16) && CurrentExact(exR) && CurrentExactTranslation(exT)) {
				double vx = (double)VX(idx), vy = (double)VY(idx), vz = (double)VZ(idx);
				if (CurrentExactVector(idx, exV)) { vx = exV[0]; vy = exV[1]; vz = exV[2]; }
				double tvX = exT[0] + exR[0] * vx + exR[1] * vy + exR[2] * vz;
				double tvY = exT[1] + exR[3] * vx + exR[4] * vy + exR[5] * vz;
				double tvZ = exT[2] + exR[6] * vx + exR[7] * vy + exR[8] * vz;
				if (tvZ > 0.0) {
					double ratio = (double)C2_H / tvZ;
					fx = (double)C2_OFX / 65536.0 + tvX * ratio;
					fy = (double)C2_OFY / 65536.0 + tvY * ratio;
					pgxpW = g_PgxpUseUnquantizedDepth ? (float)tvZ : (float)C2_SZ3;
				}
			}
		}

		/* Whole-map far vertex: replace the SZ3-clamped precise coord/W with the
		 * unclamped re-projection so PGXP-on renders far town geometry at correct
		 * scale AND correct per-vertex depth (pgxpW = true unclamped view depth). */
		if (wmFarHit) {
			fx = wmFarX;
			fy = wmFarY;
			pgxpW = wmFarW;
		}
		{
			extern int g_PsxPgxpDbg;
			long long dz = gte_shift(m_mac3, 1);
			if ((g_PsxPgxpDbg & 32) && dz > 0) {
				double r = (double)C2_H / (double)dz;
				fx = (double)C2_OFX / 65536.0 + (double)C2_MAC1 * r;
				fy = (double)C2_OFY / 65536.0 + (double)C2_MAC2 * r;
				pgxpW = (float)dz;
			}
		}

		/* Mirror the GTE SXY FIFO with a precise FIFO so the gte_stsxy* store macros
		 * (which know the destination address but not the precise value) can record
		 * address->precise deterministically. Shift exactly as the C2_SXY shift above. */
		s_pgxpFifoX[0] = s_pgxpFifoX[1]; s_pgxpFifoX[1] = s_pgxpFifoX[2]; s_pgxpFifoX[2] = (float)fx;
		s_pgxpFifoY[0] = s_pgxpFifoY[1]; s_pgxpFifoY[1] = s_pgxpFifoY[2]; s_pgxpFifoY[2] = (float)fy;
		s_pgxpFifoW[0] = s_pgxpFifoW[1]; s_pgxpFifoW[1] = s_pgxpFifoW[2]; s_pgxpFifoW[2] = pgxpW;

		/* Near-clip reprojection constants: the projection registers active when
		 * this vertex was transformed. The GL near-plane clipper re-projects the
		 * clip vertices it creates with the exact same formula (sx = OFX + x*H/z).
		 * These globals are only the fallback for an untracked poly — the values
		 * that actually get used ride the per-vertex VsEntry shadow, because the
		 * clipper runs at DrawOTag time and SH1 reprograms OFX/OFY/H mid-frame. */
		g_PgxpGteOfx = (float)((double)C2_OFX / 65536.0);
		g_PgxpGteOfy = (float)((double)C2_OFY / 65536.0);
		g_PgxpGteH   = (float)C2_H;
	}

	/* View-space FIFO: stash this vertex's camera-space position (RTPS
	 * MAC1/MAC2/MAC3). Source data for the per-pixel flashlight AND the PGXP
	 * near-plane clipper, so it runs when either is on. Mirrors the SXY FIFO
	 * shift above so gte_stsxy* can resolve address->view-pos. Off = no cost. */
	if (GR_NeedViewSpaceData())
	{
		s_vsFifoX[0] = s_vsFifoX[1]; s_vsFifoX[1] = s_vsFifoX[2]; s_vsFifoX[2] = (float)C2_MAC1;
		s_vsFifoY[0] = s_vsFifoY[1]; s_vsFifoY[1] = s_vsFifoY[2]; s_vsFifoY[2] = (float)C2_MAC2;
		s_vsFifoZ[0] = s_vsFifoZ[1]; s_vsFifoZ[1] = s_vsFifoZ[2]; s_vsFifoZ[2] = (float)C2_MAC3;

		s_vsFifoOfx[0] = s_vsFifoOfx[1]; s_vsFifoOfx[1] = s_vsFifoOfx[2];
		s_vsFifoOfx[2] = (float)((double)C2_OFX / 65536.0);
		s_vsFifoOfy[0] = s_vsFifoOfy[1]; s_vsFifoOfy[1] = s_vsFifoOfy[2];
		s_vsFifoOfy[2] = (float)((double)C2_OFY / 65536.0);
		s_vsFifoH[0] = s_vsFifoH[1]; s_vsFifoH[1] = s_vsFifoH[2];
		s_vsFifoH[2] = (float)C2_H;
	}

	return h_over_sz3;
}

int GTE_operator(int op)
{
	int v;
	int lm;
	int cv;
	int mx;
	int h_over_sz3 = 0;

	lm = GTE_LM(gteop(op));
	m_sf = GTE_SF(gteop(op));

	C2_FLAG = 0;

	switch (GTE_FUNCT(gteop(op)))
	{
	case 0x00:
	case 0x01:
#ifdef GTE_LOG
		GTELOG("%08x RTPS", op);
#endif
		h_over_sz3 = GTE_RotTransPers(0, lm);

		C2_MAC0 = int(F((long long)C2_DQB + ((long long)C2_DQA * h_over_sz3)));
		C2_IR0 = Lm_H(m_mac0, 1);

		return 1;

	case 0x06:
#ifdef GTE_LOG
		GTELOG("%08x NCLIP", op);
#endif
		C2_MAC0 = int(F((long long)(C2_SX0 * C2_SY1) + (C2_SX1 * C2_SY2) + (C2_SX2 * C2_SY0) - (C2_SX0 * C2_SY2) - (C2_SX1 * C2_SY0) - (C2_SX2 * C2_SY1)));
		C2_FLAG = 0;
		return 1;

	case 0x0c:
#ifdef GTE_LOG
		GTELOG("%08x OP", op);
#endif

		C2_MAC1 = A1((long long)(C2_R22 * C2_IR3) - (C2_R33 * C2_IR2));
		C2_MAC2 = A2((long long)(C2_R33 * C2_IR1) - (C2_R11 * C2_IR3));
		C2_MAC3 = A3((long long)(C2_R11 * C2_IR2) - (C2_R22 * C2_IR1));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x10:
#ifdef GTE_LOG
		GTELOG("%08x DPCS", op);
#endif

		C2_MAC1 = A1((C2_R << 16) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_R << 16)), 0)));
		C2_MAC2 = A2((C2_G << 16) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_G << 16)), 0)));
		C2_MAC3 = A3((C2_B << 16) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_B << 16)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x11:
#ifdef GTE_LOG
		GTELOG("%08x INTPL", op);
#endif

		C2_MAC1 = A1((C2_IR1 << 12) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_IR1 << 12)), 0)));
		C2_MAC2 = A2((C2_IR2 << 12) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_IR2 << 12)), 0)));
		C2_MAC3 = A3((C2_IR3 << 12) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_IR3 << 12)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x12:
#ifdef GTE_LOG
		GTELOG("%08x MVMVA", op);
#endif

		mx = GTE_MX(gteop(op));
		v = GTE_V(gteop(op));
		cv = GTE_CV(gteop(op));

		switch (cv) {
		case 2:
			C2_MAC1 = A1((long long)(MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)));
			C2_MAC2 = A2((long long)(MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)));
			C2_MAC3 = A3((long long)(MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)));
			Lm_B1(A1(((long long)CV1(cv) << 12) + (MX11(mx) * VX(v))), 0);
			Lm_B2(A2(((long long)CV2(cv) << 12) + (MX21(mx) * VX(v))), 0);
			Lm_B3(A3(((long long)CV3(cv) << 12) + (MX31(mx) * VX(v))), 0);
			break;

		default:
			C2_MAC1 = A1(/*int44*/(long long)((long long)CV1(cv) << 12) + (MX11(mx) * VX(v)) + (MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)));
			C2_MAC2 = A2(/*int44*/(long long)((long long)CV2(cv) << 12) + (MX21(mx) * VX(v)) + (MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)));
			C2_MAC3 = A3(/*int44*/(long long)((long long)CV3(cv) << 12) + (MX31(mx) * VX(v)) + (MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)));
			break;
		}

		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x13:
#ifdef GTE_LOG
		GTELOG("%08x NCDS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x14:
#ifdef GTE_LOG
		GTELOG("%08x CDP", op);
#endif

		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x16:
#ifdef GTE_LOG
		GTELOG("%08x NCDT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
			C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
			C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x1b:
#ifdef GTE_LOG
		GTELOG("%08x NCCS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1c:
#ifdef GTE_LOG
		GTELOG("%08x CC", op);
#endif

		C2_MAC1 = A1(/*int44*/(long long)(((long long)C2_RBK) << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)(((long long)C2_GBK) << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)(((long long)C2_BBK) << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1e:
#ifdef GTE_LOG
		GTELOG("%08x NCS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x20:
#ifdef GTE_LOG
		GTELOG("%08x NCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x28:
#ifdef GTE_LOG
		GTELOG("%08x SQR", op);
#endif

		C2_MAC1 = A1(C2_IR1 * C2_IR1);
		C2_MAC2 = A2(C2_IR2 * C2_IR2);
		C2_MAC3 = A3(C2_IR3 * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x29:
#ifdef GTE_LOG
		GTELOG("%08x DPCL", op);
#endif

		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x2a:
#ifdef GTE_LOG
		GTELOG("%08x DPCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((C2_R0 << 16) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_R0 << 16)), 0)));
			C2_MAC2 = A2((C2_G0 << 16) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_G0 << 16)), 0)));
			C2_MAC3 = A3((C2_B0 << 16) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_B0 << 16)), 0)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x2d:
#ifdef GTE_LOG
		GTELOG("%08x AVSZ3", op);
#endif

		C2_MAC0 = int(F((long long)(C2_ZSF3 * C2_SZ1) + (C2_ZSF3 * C2_SZ2) + (C2_ZSF3 * C2_SZ3)));
		C2_OTZ = Lm_D(m_mac0, 1);
		return 1;

	case 0x2e:
#ifdef GTE_LOG
		GTELOG("%08x AVSZ4", op);
#endif

		C2_MAC0 = int(F((long long)(C2_ZSF4 * C2_SZ0) + (C2_ZSF4 * C2_SZ1) + (C2_ZSF4 * C2_SZ2) + (C2_ZSF4 * C2_SZ3)));
		C2_OTZ = Lm_D(m_mac0, 1);
		return 1;

	case 0x30:
#ifdef GTE_LOG
		GTELOG("%08x RTPT", op);
#endif

		for (v = 0; v < 3; v++)
			h_over_sz3 = GTE_RotTransPers(v, lm);

		C2_MAC0 = int(F((long long)C2_DQB + ((long long)C2_DQA * h_over_sz3)));
		C2_IR0 = Lm_H(m_mac0, 1);
		return 1;

	case 0x3d:
#ifdef GTE_LOG
		GTELOG("%08x GPF", op);
#endif

		C2_MAC1 = A1(C2_IR0 * C2_IR1);
		C2_MAC2 = A2(C2_IR0 * C2_IR2);
		C2_MAC3 = A3(C2_IR0 * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3e:
#ifdef GTE_LOG
		GTELOG("%08x GPL", op);
#endif

		C2_MAC1 = A1(gte_shift(C2_MAC1, -m_sf) + (C2_IR0 * C2_IR1));
		C2_MAC2 = A2(gte_shift(C2_MAC2, -m_sf) + (C2_IR0 * C2_IR2));
		C2_MAC3 = A3(gte_shift(C2_MAC3, -m_sf) + (C2_IR0 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3f:
#ifdef GTE_LOG
		GTELOG("%08x NCCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1((C2_R << 4) * C2_IR1);
			C2_MAC2 = A2((C2_G << 4) * C2_IR2);
			C2_MAC3 = A3((C2_B << 4) * C2_IR3);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;
	}

	return 0;
}