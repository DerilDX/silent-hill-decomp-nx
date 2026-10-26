#include "psx/libgte.h"
#include "psx/inline_c.h"
#include "psx/gtemac.h"
#include "psx/gtereg.h"

#include <stdio.h>
#include "PsyX/PsyX_render.h"

#include <assert.h>
#include <string.h>

#include "../gte/PsyX_GTE.h"
#include "../gte/rcossin_tbl.h"
#include "../gte/ratan_tbl.h"
#include "../gte/sqrt_tbl.h"

#define ONE_BITS			12
#define ONE					(1 << 12)
#define	FIXED(a)			((a) >> 12)

#ifndef MIN
#define MIN(a,b)	fst_min(a,b)
#endif

#ifndef MAX
#define MAX(a,b)	fst_max(a,b)
#endif

inline int fst_min(int a, int b)
{
	int diff = a - b;
	int dsgn = diff >> 31;
	return b + (diff & dsgn);
}

inline int fst_max(int a, int b)
{
	int diff = a - b;
	int dsgn = diff >> 31;
	return a - (diff & dsgn);
}

void InitGeom()
{
	C2_ZSF3 = 341;
	C2_ZSF4 = 256;
	C2_H = 1000;
	C2_DQA = -98;
	C2_DQB = 340;
	C2_OFX = 0;
	C2_OFY = 0;
}

void SetGeomOffset(int ofx, int ofy)
{
	C2_OFX = (ofx << 16);
	C2_OFY = (ofy << 16);
}

void SetGeomScreen(int h)
{
	C2_H = h;
}

void SetRotMatrix(MATRIX* m)
{
	gte_SetRotMatrix(m);
}

void SetLightMatrix(MATRIX* m)
{
	gte_SetLightMatrix(m);
}

void SetColorMatrix(MATRIX* m)
{
	gte_SetColorMatrix(m);
}

void SetTransMatrix(MATRIX* m)
{
	gte_SetTransMatrix(m);
}

#define MAX_NUM_MATRICES 20
int matrixLevel = 0;
MATRIX stack[MAX_NUM_MATRICES];//unk_410
MATRIX* currentMatrix = &stack[0];//unk_40C

void PushMatrix()
{
	if (matrixLevel < 20)
	{
		MATRIX* m = &stack[matrixLevel];//$t7

		gte_ReadRotMatrix(m);
		gte_sttr(m->t);

		currentMatrix++;
		matrixLevel++;
	}
	else
	{
		printf("Error: Can't push matrix,stack(max 20) is full!\n");
	}
}

void PopMatrix()
{
	if (matrixLevel > 0)
	{
		currentMatrix--;
		matrixLevel--;
		MATRIX* m = &stack[matrixLevel];//$t7

		gte_SetRotMatrix(m);
		gte_SetTransMatrix(m);
	}
	else
	{
		printf("Error: Can't pop matrix,stack is empty!\n");
	}
}

/* PGXP shadow registration for the RotTransPers* wrappers below.
 *
 * These project through PsyCross's own gte_stsxy* macros (psx/inline_c.h), which
 * — unlike the game's gte_stsxy3c in pc_port/include — never call PGXP_StoreAddr.
 * Nothing was ever recorded for the vertices they write, so the draw side resolved
 * no shadow at the prim address and fell back to affine: that is why the
 * GsSortObject4J item drawers (inventory carousel + world pickup) rendered affine
 * no matter what use_pgxp said.
 *
 * `slot` is the vertex's position in the GTE's 3-deep SXY FIFO at the moment of the
 * store (2 = newest), mirrored by the precise FIFO in PsyX_GTE.cpp. The FIFO shifts
 * on every RTPS, so a capture must happen before any further projection. */
extern int  g_PsxUsePgxp;
extern void PGXP_StoreAddr(void* addr, int slot);

/* Exact-transform twin provenance (PsyX_GTE.cpp). These carry the unquantized
 * matrix product/scale results across the Q12 matrix ops so RTPS can project
 * from full precision. All internally no-op when PGXP is off; the call sites
 * below additionally guard on g_PsxUsePgxp so the off path is branch-only. */
extern int  PGXP_MatrixLookup(const MATRIX* m, double exact[9]);
extern int  PGXP_MatrixLookupCurrent(double exact[9]);
extern void PGXP_MatrixRegister(const MATRIX* m, const double exact[9]);
extern void PGXP_MatrixInvalidate(MATRIX* m);
extern void PGXP_MatrixInvalidateTranslation(MATRIX* m);
extern void PGXP_MatrixCopy(MATRIX* dst, const MATRIX* src);

void RotTrans(SVECTOR* v0, VECTOR* v1, long* flag)
{
	gte_RotTrans(v0, v1, flag);
}

void RotTransSV(SVECTOR* v0, SVECTOR* v1, long* flag)
{
	gte_ldv0(v0);
	gte_rt();
	gte_stsv(v1);
	gte_stflg(flag);
}

/* PC port: raw per-vertex view-space SZ from the LAST RotTransPers/3/4, captured
 * here (before the GS TMD drawers' NormalClip/NormalColorCol clobber the SZ FIFO)
 * so the inventory item pass can feed each face's true unquantized depth into the
 * GL depth test — the OTZ return (gte_stszotz) is perspective-divided + clamped and
 * too coarse to separate a radio's antenna face from its body face. Read by
 * ITEM_PRECISE_SZ in libgs_stub.c, gated on g_PcItemPreciseDepth (inventory only). */
unsigned short g_PsyX_RtpSz[4] = { 0, 0, 0, 0 };

int RotTransPers(SVECTOR* v0, int* sxy, long* p, long* flag)
{
	int sz;
	gte_RotTransPers(v0, sxy, p, flag, &sz);

	/* One RTPS: this vertex is the newest FIFO entry. (The quad drawers in
	 * libgs_stub.c project v3 through here after a RotTransPers3 on v0..v2.) */
	if (g_PsxUsePgxp)
		PGXP_StoreAddr(sxy, 2);

	/* Mirror the shifted SZ FIFO: after RTPT(v0..v2) + this RTPS(v3) the
	 * slots hold v0..v3 in order, which is what the quad path of
	 * ApplyGtePerVertexDepth expects. Without this the lit-quad drawers fed
	 * [stale, v0, v1, v2] — v3's depth was never captured. The mirror is only
	 * read under g_PcItemPreciseDepth at addPrim, so other callers see no
	 * behavior change. */
	g_PsyX_RtpSz[0] = (unsigned short)C2_SZ0;
	g_PsyX_RtpSz[1] = (unsigned short)C2_SZ1;
	g_PsyX_RtpSz[2] = (unsigned short)C2_SZ2;
	g_PsyX_RtpSz[3] = (unsigned short)C2_SZ3;

	return sz;
}

int RotTransPers3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, long* sxy0, long* sxy1, long* sxy2, long* p, long* flag)
{
	int sz;
	gte_RotTransPers3(v0, v1, v2, sxy0, sxy1, sxy2, p, flag, &sz);

	/* RTPT leaves v0..v2 in FIFO slots 0..2. Capture before the caller projects
	 * anything else (the quad drawers follow up with RotTransPers on v3). */
	if (g_PsxUsePgxp)
	{
		PGXP_StoreAddr(sxy0, 0);
		PGXP_StoreAddr(sxy1, 1);
		PGXP_StoreAddr(sxy2, 2);
	}

	/* Mirror the SZ FIFO 1:1 (SZ0=oldest/unused, SZ1..3 = v0..v2), matching the
	 * indexing ApplyGtePerVertexDepth expects (triangle reads sz[1..3]). */
	g_PsyX_RtpSz[0] = (unsigned short)C2_SZ0;
	g_PsyX_RtpSz[1] = (unsigned short)C2_SZ1;
	g_PsyX_RtpSz[2] = (unsigned short)C2_SZ2;
	g_PsyX_RtpSz[3] = (unsigned short)C2_SZ3;

	return sz;
}

int RotTransPers4(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, SVECTOR* v3, long* sxy0, long* sxy1, long* sxy2, long* sxy3, long* p, long* flag)
{
	int _flag;
	int sz;

	gte_ldv3(v0, v1, v2);
	gte_rtpt();

	gte_stsxy3(sxy0, sxy1, sxy2);

	/* Capture v0..v2 NOW: the RTPS below shifts the 3-deep FIFO by one, which
	 * would drop v0 beyond recovery. */
	if (g_PsxUsePgxp)
	{
		PGXP_StoreAddr(sxy0, 0);
		PGXP_StoreAddr(sxy1, 1);
		PGXP_StoreAddr(sxy2, 2);
	}

	gte_stflg(&_flag);

	gte_ldv0(v3);
	gte_rtps();

	gte_stsxy(sxy3);

	if (g_PsxUsePgxp)
		PGXP_StoreAddr(sxy3, 2); /* the RTPS above made v3 the newest entry */

	gte_stflg(flag);
	gte_stdp(p);

	/* FIFO now holds SZ0=v0, SZ1=v1, SZ2=v2, SZ3=v3 (rtps shifted the 4th in);
	 * mirror it 1:1 for the quad path of ApplyGtePerVertexDepth (sz[0],sz[1],sz[3],sz[2]). */
	g_PsyX_RtpSz[0] = (unsigned short)C2_SZ0;
	g_PsyX_RtpSz[1] = (unsigned short)C2_SZ1;
	g_PsyX_RtpSz[2] = (unsigned short)C2_SZ2;
	g_PsyX_RtpSz[3] = (unsigned short)C2_SZ3;

	*flag |= _flag;
	gte_stszotz(&sz);

	return sz;
}

void NormalColor(SVECTOR* v0, CVECTOR* v1)
{
	gte_NormalColor(v0, v1);
}

void NormalColor3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, CVECTOR* v3, CVECTOR* v4, CVECTOR* v5)
{
	gte_NormalColor3(v0, v1, v2, v3, v4, v5);
}

void NormalColorDpq(SVECTOR* v0, CVECTOR* v1, int p, CVECTOR* v2)
{
	gte_NormalColorDpq(v0, v1, p, v2);
}

void NormalColorCol(SVECTOR* v0, CVECTOR* v1, CVECTOR* v2)
{
	gte_NormalColorCol(v0, v1, v2);
}

void NormalColorCol3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, CVECTOR* v3, CVECTOR* v4, CVECTOR* v5, CVECTOR* v6)
{
	gte_NormalColorCol3(v0,v1,v2,v3,v4,v5,v6);
}

void DpqColor(CVECTOR* v0, int p, CVECTOR* v1)
{
	gte_DpqColor(v0, &p, v1);
}

void ColorDpq(VECTOR* v0, CVECTOR* v1, int p, CVECTOR* v2)
{
	gte_ColorDpq(v0, v1, p, v2);
}

void ColorCol(VECTOR* v0, CVECTOR* v1, CVECTOR* v2)
{
	gte_ColorCol(v0, v1, v2);
}

int NormalClip(int sxy0, int sxy1, int sxy2)
{
	int opz;

	gte_NormalClip(&sxy0, &sxy1, &sxy2, &opz);
	
	return opz;
}

void LocalLight(SVECTOR* v0, VECTOR* v1)
{
	gte_LocalLight(v0, v1);
}

int RotAverageNclip4(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, SVECTOR* v3, long* sxy0, long* sxy1, long* sxy2, long* sxy3, long* p, long* otz, long* flag)
{
	gte_ldv3(v0, v1, v2);
	gte_rtpt();
	gte_stflg(flag);

	gte_nclip();

	int opz;
	gte_stopz(&opz);

	if (opz > 0)
	{
		gte_stsxy3(sxy0, sxy1, sxy2);

		gte_ldv0(v3);

		gte_rtps();

		gte_stsxy(sxy3);
		gte_stdp(p);
		gte_stflg(flag);

		gte_avsz4();

		gte_stotz(otz);
	}

	return opz;
}

MATRIX* MulMatrix0(MATRIX* m0, MATRIX* m1, MATRIX* m2)
{
#if 1
	// correct Psy-Q implementation
	SVECTOR v0, r0, r1, r2;

	/* PGXP exact twin: compute the product in double from the operands' twins
	 * before the Q12 integer multiply below truncates it. */
	double exactOut[9];
	int haveExact = 0;
	if (g_PsxUsePgxp)
	{
		double exact0[9], exact1[9];
		haveExact = PGXP_MatrixLookup(m0, exact0) && PGXP_MatrixLookup(m1, exact1);
		if (haveExact)
		{
			int row, col;
			for (row = 0; row < 3; ++row)
				for (col = 0; col < 3; ++col)
					exactOut[row * 3 + col] =
						exact0[row * 3 + 0] * exact1[0 * 3 + col] +
						exact0[row * 3 + 1] * exact1[1 * 3 + col] +
						exact0[row * 3 + 2] * exact1[2 * 3 + col];
		}
	}

	gte_SetRotMatrix(m0);

	v0.vx = m1->m[0][0];
	v0.vy = m1->m[1][0];
	v0.vz = m1->m[2][0];

	gte_ldv0(&v0);
	gte_rtv0();
	gte_stsv(&r0);

	v0.vx = m1->m[0][1];
	v0.vy = m1->m[1][1];
	v0.vz = m1->m[2][1];

	gte_ldv0(&v0);
	gte_rtv0();
	gte_stsv(&r1);

	v0.vx = m1->m[0][2];
	v0.vy = m1->m[1][2];
	v0.vz = m1->m[2][2];

	gte_ldv0(&v0);
	gte_rtv0();
	gte_stsv(&r2);

	m2->m[0][0] = r0.vx;
	m2->m[0][1] = r1.vx;
	m2->m[0][2] = r2.vx;

	m2->m[1][0] = r0.vy;
	m2->m[1][1] = r1.vy;
	m2->m[1][2] = r2.vy;

	m2->m[2][0] = r0.vz;
	m2->m[2][1] = r1.vz;
	m2->m[2][2] = r2.vz;

	if (g_PsxUsePgxp)
	{
		if (haveExact)
			PGXP_MatrixRegister(m2, exactOut);
		else
			PGXP_MatrixInvalidate(m2);
	}

#else
	/* ����ł�m0==m2�̎����o�C */
	int vx, vy, vz;
	MATRIX tmp;
	/* �̂�m0��tmp�ɃR�s�[ */
	if (m0 == m2) {
		tmp = *m0; m0 = &tmp;
	}

	vx = m1->m[0][0];
	vy = m1->m[1][0];
	vz = m1->m[2][0];
	m2->m[0][0] = FIXED(m0->m[0][0] * vx + m0->m[0][1] * vy + m0->m[0][2] * vz);
	m2->m[1][0] = FIXED(m0->m[1][0] * vx + m0->m[1][1] * vy + m0->m[1][2] * vz);
	m2->m[2][0] = FIXED(m0->m[2][0] * vx + m0->m[2][1] * vy + m0->m[2][2] * vz);
	vx = m1->m[0][1];
	vy = m1->m[1][1];
	vz = m1->m[2][1];
	m2->m[0][1] = FIXED(m0->m[0][0] * vx + m0->m[0][1] * vy + m0->m[0][2] * vz);
	m2->m[1][1] = FIXED(m0->m[1][0] * vx + m0->m[1][1] * vy + m0->m[1][2] * vz);
	m2->m[2][1] = FIXED(m0->m[2][0] * vx + m0->m[2][1] * vy + m0->m[2][2] * vz);
	vx = m1->m[0][2];
	vy = m1->m[1][2];
	vz = m1->m[2][2];
	m2->m[0][2] = FIXED(m0->m[0][0] * vx + m0->m[0][1] * vy + m0->m[0][2] * vz);
	m2->m[1][2] = FIXED(m0->m[1][0] * vx + m0->m[1][1] * vy + m0->m[1][2] * vz);
	m2->m[2][2] = FIXED(m0->m[2][0] * vx + m0->m[2][1] * vy + m0->m[2][2] * vz);
#endif

	return m2;
}

MATRIX* MulMatrix(MATRIX* m0, MATRIX* m1)
{
	MATRIX tmp;
	gte_MulMatrix0(m0, m1, &tmp);

	/* MulMatrix0 only computes the 3x3 rotation; tmp.t[] is uninitialized.
	 * Copy only the rotation part to preserve m0's translation vector. */
	memcpy(m0->m, tmp.m, sizeof(m0->m));
	if (g_PsxUsePgxp)
		PGXP_MatrixCopy(m0, &tmp);

	return m0;
}

MATRIX* MulMatrix2(MATRIX* m0, MATRIX* m1)
{
	// Same as MulMatrix but result goes to m1
	MATRIX tmp;
	gte_MulMatrix0(m0, m1, &tmp);

	/* MulMatrix0 only computes the 3x3 rotation; tmp.t[] is uninitialized.
	 * Copy only the rotation part to preserve m1's translation vector. */
	memcpy(m1->m, tmp.m, sizeof(m1->m));
	if (g_PsxUsePgxp)
		PGXP_MatrixCopy(m1, &tmp);

	return m1;
}

/* m0 = R * m0, where R is the GTE's current rotation matrix. R is left as it
 * was (gte_MulMatrix0 re-sets it to the value just read back).
 *
 * The old implementation fed each ROW of m0 through RTV0, which computes
 * m0 * transpose(R) — a different matrix, and the wrong one for every caller.
 * Callers build a local->world rotation, put a view-space position in ->t, and
 * then hand the whole thing to SetRotMatrix/SetTransMatrix, so the world->view
 * rotation has to arrive on the LEFT for the rotation and the translation to
 * describe the same space. That is what broke the map6_s04 Flauros ray: both
 * of its endpoints are correct in world space (verified from a capture), but
 * the transposed product aimed the drawn ribbon somewhere else entirely, so
 * the beam passed over Alessa instead of through her. */
MATRIX* MulRotMatrix(MATRIX* m0)
{
	MATRIX cur, out;

	gte_ReadRotMatrix(&cur);
	gte_MulMatrix0(&cur, m0, &out);

	/* MulMatrix0 computes only the 3x3; m0 keeps its own translation. */
	memcpy(m0->m, out.m, sizeof(m0->m));
	if (g_PsxUsePgxp)
		PGXP_MatrixCopy(m0, &out);

	return m0;
}

/* GTE rotation matrix = R * m. m itself is not modified. Was an empty stub in
 * pc_port/src/stubs/func_stubs.c, which left the caller's own rotation out of
 * the product entirely — map6_s04's force-field grid (func_800DFA38) drew with
 * the bare world->view matrix instead of its own ~106 degree yaw. */
MATRIX* SetMulRotMatrix(MATRIX* m)
{
	MATRIX cur, out;

	gte_ReadRotMatrix(&cur);
	gte_MulMatrix0(&cur, m, &out);
	gte_SetRotMatrix(&out);

	return m;
}

void SetBackColor(int rbk, int gbk, int bbk)
{
	gte_SetBackColor(rbk,gbk,bbk);
}

void SetFarColor(int rfc, int gfc, int bfc)
{
	gte_SetFarColor(rfc, gfc, bfc);
}

#define	APPLYMATRIX(m,v0,v1)	{\
	int vx = v0->vx;\
	int vy = v0->vy;\
	int vz = v0->vz;\
	v1->vx = FIXED(m->m[0][0]*vx + m->m[0][1]*vy + m->m[0][2]*vz );\
	v1->vy = FIXED(m->m[1][0]*vx + m->m[1][1]*vy + m->m[1][2]*vz );\
	v1->vz = FIXED(m->m[2][0]*vx + m->m[2][1]*vy + m->m[2][2]*vz );\
}

VECTOR* ApplyMatrix(MATRIX* m, SVECTOR* v0, VECTOR* v1)
{
#if 1
	// correct Psy-Q implementation
	gte_SetRotMatrix(m);
	gte_ldv0(v0);
	gte_rtv0();
	gte_stlvnl(v1);
#else
	APPLYMATRIX(m, v0, v1);
#endif
	return v1;
}

VECTOR* ApplyRotMatrix(SVECTOR* v0, VECTOR* v1)
{
#if 1
	// correct Psy-Q implementation
	gte_ldv0(v0);
	gte_rtv0();
	gte_stlvnl(v1);
#else
	MATRIX temp;
	gte_ReadRotMatrix(&temp);

	MATRIX* m = &temp;

	APPLYMATRIX(m, v0, v1);
#endif
	return v1;
}

VECTOR* ApplyRotMatrixLV(VECTOR* v0, VECTOR* v1)
{
#if 1
	// correct Psy-Q implementation
	VECTOR tmpHI;
	VECTOR tmpLO;

	tmpHI.vx = v0->vx;
	tmpHI.vy = v0->vy;
	tmpHI.vz = v0->vz;

	if (tmpHI.vx < 0)
	{
		tmpLO.vx = -(-tmpHI.vx >> 0xf);
		tmpHI.vx = -(-tmpHI.vx & 0x7fff);
	}
	else
	{
		tmpLO.vx = tmpHI.vx >> 0xf;
		tmpHI.vx = tmpHI.vx & 0x7fff;
	}

	if (tmpHI.vy < 0)
	{
		tmpLO.vy = -(-tmpHI.vy >> 0xf);
		tmpHI.vy = -(-tmpHI.vy & 0x7fff);
	}
	else
	{
		tmpLO.vy = tmpHI.vy >> 0xf;
		tmpHI.vy = tmpHI.vy & 0x7fff;
	}

	if (tmpHI.vz < 0)
	{
		tmpLO.vz = -(-tmpHI.vz >> 0xf);
		tmpHI.vz = -(-tmpHI.vz & 0x7fff);
	}
	else
	{
		tmpLO.vz = tmpHI.vz >> 0xf;
		tmpHI.vz = tmpHI.vz & 0x7fff;
	}

	gte_ldlvl(&tmpLO);
	gte_rtir_sf0();
	gte_stlvnl(&tmpLO);

	gte_ldlvl(&tmpHI);
	gte_rtir();

	if (tmpLO.vx < 0)
		tmpLO.vx *= 8;
	else
		tmpLO.vx <<= 3;

	if (tmpLO.vy < 0)
		tmpLO.vy *= 8;
	else
		tmpLO.vy <<= 3;

	if (tmpLO.vz < 0)
		tmpLO.vz *= 8;
	else
		tmpLO.vz <<= 3;

	gte_stlvnl(&tmpHI);

	v1->vx = tmpHI.vx + tmpLO.vx;
	v1->vy = tmpHI.vy + tmpLO.vy;
	v1->vz = tmpHI.vz + tmpLO.vz;
#else
	MATRIX temp;
	gte_ReadRotMatrix(&temp);

	MATRIX* m = &temp;

	APPLYMATRIX(m, v0, v1);
#endif
	return v1;
}

SVECTOR* ApplyMatrixSV(MATRIX* m, SVECTOR* v0, SVECTOR* v1)
{
#if 1
	// correct Psy-Q implementation
	gte_SetRotMatrix(m);
	gte_ldv0(v0);
	gte_rtv0();
	gte_stsv(v1);
#else
	APPLYMATRIX(m, v0, v1);
#endif
	return v1;
}

VECTOR* ApplyMatrixLV(MATRIX* m, VECTOR* v0, VECTOR* v1)
{
#if 1
	// correct Psy-Q implementation
	VECTOR tmpHI;
	VECTOR tmpLO;

	gte_SetRotMatrix(m);

	tmpHI.vx = v0->vx;
	tmpHI.vy = v0->vy;
	tmpHI.vz = v0->vz;

	if (tmpHI.vx < 0)
	{
		tmpLO.vx = -(-tmpHI.vx >> 0xf);
		tmpHI.vx = -(-tmpHI.vx & 0x7fff);
	}
	else
	{
		tmpLO.vx = tmpHI.vx >> 0xf;
		tmpHI.vx = tmpHI.vx & 0x7fff;
	}

	if (tmpHI.vy < 0)
	{
		tmpLO.vy = -(-tmpHI.vy >> 0xf);
		tmpHI.vy = -(-tmpHI.vy & 0x7fff);
	}
	else
	{
		tmpLO.vy = tmpHI.vy >> 0xf;
		tmpHI.vy = tmpHI.vy & 0x7fff;
	}

	if (tmpHI.vz < 0)
	{
		tmpLO.vz = -(-tmpHI.vz >> 0xf);
		tmpHI.vz = -(-tmpHI.vz & 0x7fff);
	}
	else
	{
		tmpLO.vz = tmpHI.vz >> 0xf;
		tmpHI.vz = tmpHI.vz & 0x7fff;
	}

	gte_ldlvl(&tmpLO);
	gte_rtir_sf0();
	gte_stlvnl(&tmpLO);

	gte_ldlvl(&tmpHI);
	gte_rtir();

	if (tmpLO.vx < 0)
		tmpLO.vx *= 8;
	else
		tmpLO.vx <<= 3;

	if (tmpLO.vy < 0)
		tmpLO.vy *= 8;
	else
		tmpLO.vy <<= 3;

	if (tmpLO.vz < 0)
		tmpLO.vz *= 8;
	else
		tmpLO.vz <<= 3;

	gte_stlvnl(&tmpHI);

	v1->vx = tmpHI.vx + tmpLO.vx;
	v1->vy = tmpHI.vy + tmpLO.vy;
	v1->vz = tmpHI.vz + tmpLO.vz;
#else
	APPLYMATRIX(m, v0, v1);
#endif
	return v1;
}

MATRIX* RotMatrix(SVECTOR* r, MATRIX* m)
{
	// correct Psy-Q implementation
	int c0, c1, c2;
	int s0, s1, s2;
	int s2p0, s2m0, c2p0, c2m0;
	int	s2c0, s2s0, c2c0, c2s0;

	c0 = rcos(r->vx);
	c1 = rcos(r->vy);
	c2 = rcos(r->vz);
	s0 = rsin(r->vx);
	s1 = rsin(r->vy);
	s2 = rsin(r->vz);
	s2p0 = rsin(r->vz + r->vx);
	s2m0 = rsin(r->vz - r->vx);
	c2p0 = rcos(r->vz + r->vx);
	c2m0 = rcos(r->vz - r->vx);
	s2c0 = (s2p0 + s2m0) / 2;
	c2s0 = (s2p0 - s2m0) / 2;
	s2s0 = (c2m0 - c2p0) / 2;
	c2c0 = (c2m0 + c2p0) / 2;

	m->m[0][0] = FIXED(c2 * c1);
	m->m[1][0] = s2c0 + FIXED(c2s0 * s1);
	m->m[2][0] = s2s0 - FIXED(c2c0 * s1);
	m->m[0][1] = -FIXED(s2 * c1);
	m->m[1][1] = c2c0 - FIXED(s2s0 * s1);
	m->m[2][1] = c2s0 + FIXED(s2c0 * s1);
	m->m[0][2] = s1;
	m->m[1][2] = -FIXED(c1 * s0);
	m->m[2][2] = FIXED(c1 * c0);
	/* CPU-built from fixed sin/cos: no unquantized source, so drop any older
	 * exact twin at this address rather than let it leak. */
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidate(m);

	return m;
}

MATRIX* RotMatrixYXZ(SVECTOR* r, MATRIX* m)
{
	// correct Psy-Q implementation
	int c0, c1, c2;
	int s0, s1, s2;

	c0 = rcos(r->vx);
	c1 = rcos(r->vy);
	c2 = rcos(r->vz);
	s0 = rsin(r->vx);
	s1 = rsin(r->vy);
	s2 = rsin(r->vz);

	// Y-axis
	m->m[1][0] = FIXED(s2 * c0);
	m->m[1][1] = FIXED(c2 * c0);
	m->m[1][2] = -s0;

	// X-axis
	int x0 = FIXED(s1 * s0);
	m->m[0][0] = FIXED(c1 * c2) + FIXED(x0 * s2);
	m->m[0][1] = FIXED(x0 * c2) - FIXED(c1 * s2);
	m->m[0][2] = FIXED(s1 * c0);

	// Z-axis
	int z0 = FIXED(c1 * s0);
	m->m[2][1] = FIXED(s1 * s2) + FIXED(z0 * c2);
	m->m[2][0] = FIXED(z0 * s2) - FIXED(s1 * c2);
	m->m[2][2] = FIXED(c1 * c0);
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidate(m);

	return m;
}

MATRIX* RotMatrixX(int r, MATRIX* m)
{
	// correct Psy-Q implementation
	int s0 = rsin(r);
	int c0 = rcos(r);
	int t1, t2;
	t1 = m->m[1][0];
	t2 = m->m[2][0];
	m->m[1][0] = FIXED(t1 * c0 - t2 * s0);
	m->m[2][0] = FIXED(t1 * s0 + t2 * c0);
	t1 = m->m[1][1];
	t2 = m->m[2][1];
	m->m[1][1] = FIXED(t1 * c0 - t2 * s0);
	m->m[2][1] = FIXED(t1 * s0 + t2 * c0);
	t1 = m->m[1][2];
	t2 = m->m[2][2];
	m->m[1][2] = FIXED(t1 * c0 - t2 * s0);
	m->m[2][2] = FIXED(t1 * s0 + t2 * c0);
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidate(m);

	return m;
}

MATRIX* RotMatrixY(int r, MATRIX* m)
{
	// correct Psy-Q implementation
	int s0 = rsin(r);
	int c0 = rcos(r);
	int t1, t2;
	t1 = m->m[0][0];
	t2 = m->m[2][0];
	m->m[0][0] = FIXED(t1 * c0 + t2 * s0);
	m->m[2][0] = FIXED(-t1 * s0 + t2 * c0);
	t1 = m->m[0][1];
	t2 = m->m[2][1];
	m->m[0][1] = FIXED(t1 * c0 + t2 * s0);
	m->m[2][1] = FIXED(-t1 * s0 + t2 * c0);
	t1 = m->m[0][2];
	t2 = m->m[2][2];
	m->m[0][2] = FIXED(t1 * c0 + t2 * s0);
	m->m[2][2] = FIXED(-t1 * s0 + t2 * c0);
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidate(m);

	return m;
}

MATRIX* RotMatrixZ(int r, MATRIX* m)
{
	// correct Psy-Q implementation
	int s0 = rsin(r);
	int c0 = rcos(r);
	int t1, t2;
	t1 = m->m[0][0];
	t2 = m->m[1][0];
	m->m[0][0] = FIXED(t1 * c0 - t2 * s0);
	m->m[1][0] = FIXED(t1 * s0 + t2 * c0);
	t1 = m->m[0][1];
	t2 = m->m[1][1];
	m->m[0][1] = FIXED(t1 * c0 - t2 * s0);
	m->m[1][1] = FIXED(t1 * s0 + t2 * c0);
	t1 = m->m[0][2];
	t2 = m->m[1][2];
	m->m[0][2] = FIXED(t1 * c0 - t2 * s0);
	m->m[1][2] = FIXED(t1 * s0 + t2 * c0);
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidate(m);

	return m;
}

MATRIX* RotMatrixZYX_gte(SVECTOR* r, MATRIX* m)
{
#if 0
	// TODO: correct Psy-Q implementation
#else
	m->m[0][0] = 0x1000;
	m->m[0][1] = 0;
	m->m[0][2] = 0;

	m->m[1][0] = 0;
	m->m[1][1] = 0x1000;
	m->m[1][2] = 0;

	m->m[2][0] = 0;
	m->m[2][1] = 0;
	m->m[2][2] = 0x1000;

	RotMatrixX(r->vx, m);
	RotMatrixY(r->vy, m);
	RotMatrixZ(r->vz, m);
#endif
	return m;
}

MATRIX* CompMatrix(MATRIX* m0, MATRIX* m1, MATRIX* m2)
{
	// UNTESTED
	SVECTOR tmp;
	gte_MulMatrix0(m0, m1, m2);

	tmp.vx = m1->t[0];
	tmp.vy = m1->t[1];
	tmp.vz = m1->t[2];

	gte_ldv0(&tmp);
	gte_rtv0();
	gte_stlvnl(m2->t);

	m2->t[0] += m0->t[0];
	m2->t[1] += m0->t[1];
	m2->t[2] += m0->t[2];
	/* Rotation provenance rides gte_MulMatrix0 above; the legacy translation
	 * path truncates through SVECTOR, so reject any stale exact t. */
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidateTranslation(m2);

	return m2;
}

MATRIX* CompMatrixLV(MATRIX* m0, MATRIX* m1, MATRIX* m2)
{
	// UNTESTED
	// correct Psy-Q implementation
	VECTOR tmpHI;
	VECTOR tmpLO;

	gte_MulMatrix0(m0, m1, m2);

	// next... same as ApplyMatrixLV
	tmpHI.vx = m1->t[0];
	tmpHI.vy = m1->t[1];
	tmpHI.vz = m1->t[2];

	if (tmpHI.vx < 0)
	{
		tmpLO.vx = -(-tmpHI.vx >> 0xf);
		tmpHI.vx = -(-tmpHI.vx & 0x7fff);
	}
	else 
	{
		tmpLO.vx = tmpHI.vx >> 0xf;
		tmpHI.vx = tmpHI.vx & 0x7fff;
	}

	if (tmpHI.vy < 0)
	{
		tmpLO.vy = -(-tmpHI.vy >> 0xf);
		tmpHI.vy = -(-tmpHI.vy & 0x7fff);
	}
	else 
	{
		tmpLO.vy = tmpHI.vy >> 0xf;
		tmpHI.vy = tmpHI.vy & 0x7fff;
	}

	if (tmpHI.vz < 0)
	{
		tmpLO.vz = -(-tmpHI.vz >> 0xf);
		tmpHI.vz = -(-tmpHI.vz & 0x7fff);
	}
	else
	{
		tmpLO.vz = tmpHI.vz >> 0xf;
		tmpHI.vz = tmpHI.vz & 0x7fff;
	}

	gte_ldlvl(&tmpLO);
	gte_rtir_sf0();
	gte_stlvnl(&tmpLO);

	gte_ldlvl(&tmpHI);
	gte_rtir();

	if (tmpLO.vx < 0)
		tmpLO.vx = tmpLO.vx * 8;
	else
		tmpLO.vx = tmpLO.vx << 3;

	if (tmpLO.vy < 0)
		tmpLO.vy = tmpLO.vy * 8;
	else
		tmpLO.vy = tmpLO.vy << 3;

	if (tmpLO.vz < 0)
		tmpLO.vz = tmpLO.vz * 8;
	else
		tmpLO.vz = tmpLO.vz << 3;

	gte_stlvnl(&tmpHI);

	m2->t[0] = tmpHI.vx + tmpLO.vx + m0->t[0];
	m2->t[1] = tmpHI.vy + tmpLO.vy + m0->t[1];
	m2->t[2] = tmpHI.vz + tmpLO.vz + m0->t[2];
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidateTranslation(m2);

	return m2;
}

MATRIX* TransMatrix(MATRIX* m, VECTOR* v)
{
	m->t[0] = v->vx;
	m->t[1] = v->vy;
	m->t[2] = v->vz;
	if (g_PsxUsePgxp)
		PGXP_MatrixInvalidateTranslation(m);
	return m;
}

MATRIX* ScaleMatrix(MATRIX* m, VECTOR* v)
{
	double exact[9];
	int haveExact = 0;
	if (g_PsxUsePgxp)
	{
		double input[9];
		haveExact = PGXP_MatrixLookup(m, input);
		if (haveExact)
		{
			const double scale[3] = { (double)v->vx / 4096.0, (double)v->vy / 4096.0, (double)v->vz / 4096.0 };
			int row, col;
			for (row = 0; row < 3; ++row)
				for (col = 0; col < 3; ++col)
					exact[row * 3 + col] = input[row * 3 + col] * scale[row];
		}
	}
	m->m[0][0] = FIXED(m->m[0][0] * v->vx);
	m->m[0][1] = FIXED(m->m[0][1] * v->vx);
	m->m[0][2] = FIXED(m->m[0][2] * v->vx);
	m->m[1][0] = FIXED(m->m[1][0] * v->vy);
	m->m[1][1] = FIXED(m->m[1][1] * v->vy);
	m->m[1][2] = FIXED(m->m[1][2] * v->vy);
	m->m[2][0] = FIXED(m->m[2][0] * v->vz);
	m->m[2][1] = FIXED(m->m[2][1] * v->vz);
	m->m[2][2] = FIXED(m->m[2][2] * v->vz);
	if (g_PsxUsePgxp)
	{
		if (haveExact)
			PGXP_MatrixRegister(m, exact);
		else
			PGXP_MatrixInvalidate(m);
	}
	return m;
}

void SetDQA(int iDQA)
{
	CTC2(*(uint*)&iDQA, 27);
}

void SetDQB(int iDQB)
{
	CTC2(*(uint*)&iDQB, 28);
}

void SetFogNear(int a, int h)
{
	//Error division by 0
	assert(h != 0);
	int depthQ = -(((a << 2) + a) << 6);
	assert(h != -1 && depthQ != 0x8000);
	SetDQA(depthQ / h);
	SetDQB(20971520);
}

void SetFogNearFar(int a, int b, int h)
{
	if (b - a < 100)
		return;

	assert((b - a));
	assert((b - a) != -1 && (-a * b) != 32768);
	assert((b - a));
	assert((b - a) != -1 && (b << 12) != 32768);
	assert(h != 0);
	assert(h != -1 && (((-a * b) / (b - a)) << 8) != 32768);

	int dqa = (-a * b / (b - a) << 8) / h;

	SetDQA(MAX(MIN(dqa, 32767), -32767));
	SetDQB((b << 12) / (b - a) << 12);
}

int rsin(int a)
{
	if (a < 0)
		return -rcossin_tbl[(-a & 0xfffU) * 2];

	return rcossin_tbl[(a & 0xfffU) * 2];
}

int rcos(int a)
{
	if (a < 0)
		return rcossin_tbl[(-a & 0xfffU) * 2 + 1];

	return rcossin_tbl[(a & 0xfffU) * 2 + 1];
}

int ratan2(int y, int x)
{
	// correct Psy-Q implementation
	int v;
	uint ang;
	int xlt0, ylt0;

	xlt0 = x < 0;
	ylt0 = y < 0;

	if (x == 0 && y == 0)
		return 0;

	if (x < 0)
		x = -x;

	if (y < 0)
		y = -y;

	if (y < x)
	{
		if (((uint)y & 0x7fe00000U) == 0)
			ang = (y << 10) / x;
		else
			ang = y / (x >> 10);

		v = ratan_tbl[ang];
	}
	else
	{
		if (((uint)x & 0x7fe00000U) == 0)
			ang = (x << 10) / y;
		else
			ang = x / (y >> 10);

		v = 1024 - ratan_tbl[ang];
	}

	if (xlt0)
		v = 2048 - v;

	if (ylt0)
		v = -v;

	return v;
}

int SquareRoot0(int a)
{
	// correct Psy-Q implementation
	int idx;
	int lzcs;
	lzcs = gte_leadingzerocount(a);

	if (lzcs == 32)
		return 0;

	lzcs &= 0xfffffffe;

	if ((lzcs - 24) < 0)
		idx = a >> (24 - lzcs);
	else
		idx = a << (lzcs - 24);

	return SQRT[idx - 64] << (31 - lzcs >> 1) >> 12;
}
