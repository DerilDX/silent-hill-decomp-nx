#ifndef EMULATOR_H
#define EMULATOR_H

#include "PsyX/PsyX_config.h"

/*
 * Platform specific emulator setup
 */
#if defined(__APPLE__)
#   include <TargetConditionals.h>
#endif

/* iOS must be tested before the desktop branch below: __APPLE__ is defined on
 * iPhone too, so it would otherwise select desktop GL and glad, neither of
 * which exists there. iOS caps out at OpenGL ES 3.0 (deprecated since iOS 12,
 * still functional), and its GLES entry points link statically from
 * OpenGLES.framework, so no loader is used. */
#if defined(__APPLE__) && TARGET_OS_IPHONE
#   define PSYX_IOS 1
#   define RENDERER_OGLES
#   define OGLES_VERSION (3)
#elif (defined(_WIN32) || defined(__APPLE__) || defined(__linux__)) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__RPI__)
#   define RENDERER_OGL
#   define USE_GLAD
#elif defined(__RPI__)
#   define RENDERER_OGLES
#   define OGLES_VERSION (3)
#elif defined(__EMSCRIPTEN__)
#   define RENDERER_OGLES
#   define OGLES_VERSION (2)
#elif defined(__ANDROID__)
#   define RENDERER_OGLES
#   define OGLES_VERSION (3)
#endif

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
#   define USE_OPENGL 1
#else
#   define USE_OPENGL 0
#endif

#if OGLES_VERSION == 2
#   define ES2_SHADERS
#elif OGLES_VERSION == 3
#   define ES3_SHADERS
#endif

 /*
  * OpenGL
  */

#if defined (RENDERER_OGL)

#   define GL_GLEXT_PROTOTYPES

#if defined(USE_GLAD)
#   include "common/glad.h"
#endif

#elif defined (RENDERER_OGLES)

#   define GL_GLEXT_PROTOTYPES

#if defined(USE_GLAD)
#   include "common/glad.h"
#else
#   ifdef __EMSCRIPTEN__
#      include <GL/gl.h>
#   elif defined(PSYX_IOS)
/* Apple ships GLES under its own framework layout, not the Khronos paths. */
#      include <OpenGLES/ES3/gl.h>
#      include <OpenGLES/ES3/glext.h>
#   else
#      if OGLES_VERSION == 2
#          include <GLES2/gl2.h>
#          include <GLES2/gl2ext.h>
#      elif OGLES_VERSION == 3
#          include <GLES3/gl3.h>
#      endif
#   endif
#endif

/* iOS has no EGL at all — context creation goes through EAGL, which SDL owns. */
#if !defined(PSYX_IOS)
#   include <EGL/egl.h>
#endif

#endif

  // setup renderer texture formats
#if defined(RENDERER_OGL)
#   define TEXTURE_FORMAT GL_UNSIGNED_SHORT_1_5_5_5_REV
#elif defined(RENDERER_OGLES)
#   define TEXTURE_FORMAT GL_UNSIGNED_SHORT_5_5_5_1
#endif

#include "psx/types.h"

#include "common/pgxp_defs.h"

#include "psx/libgte.h"
#include "psx/libgpu.h"

#include <stdio.h>
#include <stddef.h>

#ifndef NULL
#define NULL		0
#endif

/*
// FIXME: enable when needed
#if defined(RENDERER_OGLES)

#	define glGenVertexArrays       glGenVertexArraysOES
#	define glBindVertexArray       glBindVertexArrayOES
#	define glDeleteVertexArrays    glDeleteVertexArraysOES

#endif
*/

/* VRAM holds each 16-bit PSX pixel as two bytes, and the shaders decode it with
 * floor(sample.rg * 255.0 + 0.5), so only 8 bits per channel are ever observed.
 * RG32F therefore bought nothing but made GR_UpdateVRAM upload 1024x512 bytes
 * into a 4 MB float texture, forcing a per-texel byte->float conversion in the
 * driver every frame. RG8 is the same 8-bit-normalised value to the shader, at
 * 1 MB and a straight copy. Costs a few ms on fast drivers; on weak GL 3.1
 * iGPUs the converting path is the difference between playable and ~3 FPS. */
#if defined(RENDERER_OGL)
#	define VRAM_FORMAT            GL_RG
#	define VRAM_INTERNAL_FORMAT   GL_RG8
#elif defined(RENDERER_OGLES)
#	define VRAM_FORMAT            GL_LUMINANCE_ALPHA
#	define VRAM_INTERNAL_FORMAT   GL_LUMINANCE_ALPHA
#endif

#define VRAM_WIDTH		(1024)
#define VRAM_HEIGHT		(512)

/* One entry per 16-bit VRAM value, indexed by (high byte, low byte). */
#define LUT_WIDTH		(256)
#define LUT_HEIGHT		(256)

#define TPAGE_WIDTH		(256)
#define TPAGE_HEIGHT	(256)

/* Was (1 << 16) = 65536, matching the u_short GPUDrawSplit indices. The whole-town
 * render mode (docs/WholeMap_Far_Projection_Task.md) submits far more geometry in
 * one frame than a streamed scene, so the ceiling is raised and the split indices
 * widened to unsigned int (GPUDrawSplit). Output-neutral for normal play — the
 * buffer is only ever filled to g_vertexIndex, and a streamed frame stays tiny. */
#define MAX_VERTEX_BUFFER_SIZE	(1 << 18)

#pragma pack(push,1)
typedef struct
{
	short		x, y, page, clut;
	float		z;

	u_char		u, v, bright, dither;
	u_char		r, g, b, a;

	char		tcx, tcy, _p0, _p1;

	/* PGXP (perspective-correct): precise float screen X/Y and a per-vertex
	 * W (~view depth). Appended at the END so every existing field offset is
	 * unchanged — the affine path never reads these, and the shader only uses
	 * them when the u_pgxpEnabled uniform is set (never when PGXP is off). */
	float		ppx, ppy, ppw;

	/* Per-pixel flashlight (optional): view-space unit normal, written from the
	 * GTE normal shadow when g_PsyX_UsePerPixelFlashlight is on; left zero
	 * otherwise. Appended at the END so all existing field offsets are unchanged
	 * — the shader only reads it under the u_flashlightOn uniform (off = unused). */
	float		nx, ny, nz;

	/* Per-pixel flashlight (optional): view-space (camera-space) position, the
	 * GTE RTPS MAC1/MAC2/MAC3 of this vertex, propagated through the same
	 * address-keyed shadow path as PGXP and written only when
	 * g_PsyX_UsePerPixelFlashlight is on (memset 0 otherwise). The fragment
	 * shader reads it only under u_flashlightOn, and gates on vsz>0 so untracked
	 * (zero) verts and 2D prims are never lit. */
	float		vsx, vsy, vsz;

	/* 1.0 when this vertex belongs to a primitive the GTE projected, i.e. world
	 * geometry rather than a 2D screen prim. Set per PRIMITIVE, not per vertex:
	 * the view-space lookup resolves ~84% of vertices, and because this reaches
	 * the fragment stage as a varying, a prim with a mix of resolved and
	 * unresolved vertices interpolates across 0.5 and splits a single surface
	 * into filtered and unfiltered halves. Marking the whole prim from any one
	 * resolved vertex removes that and lifts coverage to ~per-prim. Cannot reuse
	 * a_normal.y, which the near clipper reads as "this vertex has valid
	 * view-space data" and must stay per-vertex and truthful. */
	float		geom3d;

	/* Per-POLYGON UV bounds (min u,v / max u,v), identical on every vertex of
	 * the prim. The manual bilinear/aniso taps clamp to them so a tap at the
	 * edge of a polygon's texture region cannot read the neighbouring region of
	 * the page -- the bright seam down a character's UV boundary (Harry's
	 * pants), and the same class as the gutterless font bleed. All-zero means
	 * "no bounds known" and the shader skips the clamp, so prim paths that
	 * never set them keep the old behaviour. DuckStation's approach. */
	u_char		ulo, vlo, uhi, vhi;
} GrVertex;

typedef struct GrModernVertex
{
	float x, y, page, clut, z, u, v;
	u_char r, g, b, a;
	char tcx, tcy, fog, reserved;
	float ppx, ppy, ppw, nx, ny, nz, vsx, vsy, vsz;
	/* Skinned characters lit per pixel: view-space normal (w unused) and
	 * tangent (w = bitangent sign). Zero on every other modern mesh. */
	float snx, sny, snz, snw, stx, sty, stz, stw;
} GrModernVertex;
#pragma pack(pop)

typedef enum
{
	a_position,
	a_zw,
	a_texcoord,
	a_color,
	a_extra,
	a_pgxp,
	a_normal,
	a_viewpos,
	a_geom3d,
	a_uvlim,
	a_farP0,
	a_farP1,
	a_skinNormal,
	a_skinTangent,
	a_ppNormal,
} ShaderAttrib;

typedef enum
{
	BM_NONE,
	BM_AVERAGE,
	BM_ADD,
	BM_SUBTRACT,
	BM_ADD_QUATER_SOURCE,
	/* PC only: an opaque prim faded in at dream_blur_strength (the scene-scratch
	 * soft focus base layer). */
	BM_CONSTANT_ALPHA
} BlendMode;

typedef enum
{
	TF_4_BIT,
	TF_8_BIT,
	TF_16_BIT,

	TF_32_BIT_RGBA		// custom texture
} TexFormat;


#if defined(RENDERER_OGLES) || defined(RENDERER_OGL)
typedef uint TextureID;
typedef uint ShaderID;
#else
#error
#endif

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern TextureID	g_whiteTexture;
extern TextureID	g_vramTexture;

extern void			GR_SwapWindow();

// PSX VRAM operations
extern void			GR_SaveVRAM(const char* outputFileName, int x, int y, int width, int height, int bReadFromFrameBuffer);
extern void			GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y);
extern void			GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h);

extern void			GR_StoreFrameBuffer(int x, int y, int w, int h);
extern void			GR_UpdateVRAM();
extern void			GR_ReadFramebufferDataToVRAM();

/* PC port: framebuffer feedback. Silent Hill reads rendered pixels back from
 * VRAM (Screen_BackgroundMotionBlur = the Harry-running loading-screen trail;
 * the per-map ghosting/dream overlays), so the composed frame must be present
 * in the PSX display-buffer pages. GR_SetPsxDisplayBuffers records where those
 * pages are (the PC libgs stub collapses both display envs to (0,0), so
 * activeDispEnv.disp cannot be used); GR_StoreFrameBufferPsx packs the frame
 * into them each present, and GR_RepackFrameToVramBuffers restores it after a
 * full vram[] re-upload. The store is a PACKING SHADER, not a blit: VRAM is
 * GL_RG8 holding the two bytes of a 16-bit RGB555 pixel. */
extern void			GR_SetPsxDisplayBuffers(int x0, int y0, int x1, int y1, int w, int h);
extern void			GR_StoreFrameBufferPsx(void);
extern void			GR_RepackFrameToVramBuffers(void);

/* A prim sampling a display buffer was just drawn: keep the store running and
 * record which ortho pass will redraw the capture. Called for any 16bpp tpage
 * in the left 320 VRAM columns. g_cfg_dreamFeedback = 0 leaves the per-map
 * overlays switched off (the rects stay blanked, so they draw nothing). */
extern void			GR_NoteFeedbackSamplerPrim(int semiTrans, int modColour);
extern int			g_PsxFeedbackExact;
extern int			g_cfg_dreamFeedback;

/* Widescreen feedback: how far past the 320-wide display buffer the UI ortho
 * reaches, and about which point. The effect's primitives are stretched by this
 * and the capture is stretched with them, so the blur fills the window at any
 * aspect instead of blurring a 4:3 box with sharp margins. 1.0 = 4:3, no-op. */
extern float		g_PsxFeedbackWideScale;
extern float		g_PsxFeedbackWideCenter;

/* The WORLD pass's ortho (L, R, T, B, display coordinates) and the display size
 * it was built for. Full-frame flat fills drawn in OT0 are remapped onto it so
 * they cover the picture instead of a cropped 4:3 box. g_PsxUIOrthoPass tells
 * the prim builder which pass it is in. */
extern float		g_PsxWorldOrtho[4];
extern float		g_PsxWorldDisp[2];
extern int			g_PsxWorldOrthoValid;
extern int			g_PsxUIOrthoPass;

/* PC port: directly upload a vram[] sub-region to BOTH double-buffered VRAM
 * textures, bypassing the swap-then-upload dance. Used by the paper-map
 * TIM-protect helper to defeat any unfound framebuffer→GPU-texture path. */
extern void			GR_DirectUploadVRAMRegion(int x, int y, int w, int h);
extern void			GR_DumpVRAM(const char* path);

/* PC port: when non-zero, GR_StoreFrameBuffer and GR_ReadFramebufferDataToVRAM
 * are skipped. Set this during 2D screens that re-LoadImage TIMs into VRAM
 * regions that overlap the display rect (e.g. paper-map pickup CLUT at
 * (224,15) sits inside the (0,0)-(320,240) display area, so the framebuffer
 * blit blows away the freshly uploaded CLUT every frame). */
extern int			g_PsxSkipFramebufferStore;

extern TextureID	GR_CreateRGBATexture(int width, int height, u_char* data /*= nullptr*/);
extern ShaderID		GR_Shader_Compile(const char* source);

extern void			GR_SetShader(const ShaderID shader);
extern void			GR_Perspective3D(const float fov, const float width, const float height, const float zNear, const float zFar);
extern void			GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar);

extern void			GR_SetBlendMode(BlendMode blendMode);
extern void			GR_SetPolygonOffset(float ofs);
extern void			GR_SetStencilMode(int drawPrim);
extern void			GR_EnableDepth(int enable);
extern void			GR_SetDepthFuncAlways(int enable);
extern void			GR_SetScissorState(int enable);
extern void			GR_SetOffscreenState(const RECT16* offscreenRect, int enable);
extern void			GR_SetupClipMode(const RECT16* clipRect, int enable);
extern void			GR_SetViewPort(int x, int y, int width, int height);
extern void			GR_SetTexture(TextureID texture, TexFormat texFormat);
extern void			GR_SetTextureMixed(TextureID texture);
extern void			GR_InvalidateViewScissorCache(void);
extern void			GR_SetOverrideTextureSize(int width, int height, int offsetX, int offsetY, int hiresW, int hiresH);
extern void			GR_SetWireframe(int enable);

extern void			GR_DestroyTexture(TextureID texture);
extern void			GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b);
extern void			GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b);
extern void			GR_UpdateVertexBuffer(const GrVertex* vertices, int count);
extern void			GR_UpdateModernVertexBuffer(const GrModernVertex* vertices, int count);
extern int			GR_DrawModernMesh(unsigned int mesh_handle, const DRAWENV* env);
extern void			GR_DrawTriangles(int start_vertex, int triangles);

/* Flashlight shadow map depth pre-pass (see g_PsyX_UseFlashlightShadows). Call
 * GR_ShadowPassBegin() once after GR_UpdateVertexBuffer while the frame VAO is
 * still bound, replay opaque split ranges via GR_ShadowPassDraw(), then
 * GR_ShadowPassEnd(). Guarded by GR_FlashlightShadowActive(). */
extern int			GR_FlashlightShadowActive(void);
extern void			GR_ShadowPassBegin(void);
extern void			GR_ShadowPassDraw(int start_vertex, int num_verts);
extern void			GR_ShadowPassEnd(void);

extern void			GR_PushDebugLabel(const char* label);
extern void			GR_PopDebugLabel();

/* Per-frame fog color for shader-based fog blending.
 * Set from game code; shader reads via u_fogColor uniform.
 * Fog amount per-primitive comes from GrVertex._p0. */
extern float		g_PsyX_FogColor[3];

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif