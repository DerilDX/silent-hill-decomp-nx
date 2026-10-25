#ifndef PSYX_BACKEND_H
#define PSYX_BACKEND_H

/*
 * Render backend selection.
 *
 * PsyCross draws through one GL-shaped code path (the GR_* API in
 * PsyX_render.h). Rather than reimplement that path per graphics API, the
 * non-GL backends are reached by running the SAME path against an OpenGL ES 3.0
 * context supplied by a translator (ANGLE), which emits D3D11 / D3D9 / Vulkan
 * underneath. SDL2 already knows how to load ANGLE on Windows: request an ES
 * context and it picks up libEGL.dll / libGLESv2.dll from the exe directory.
 * ANGLE's own backend is chosen by the ANGLE_DEFAULT_PLATFORM environment
 * variable, which must be set before the context is created.
 *
 * The point of the non-GL backends is not raw speed — it is that overlays and
 * capture tools (ReShade, RTSS, OBS, Special K) hook DXGI/Vulkan far more
 * reliably than they hook opengl32, and that a D3D11 device is a much better
 * behaved thing on Windows than a legacy GL context.
 *
 * BACKEND_GL is the default and is bit-for-bit the historical path: when it is
 * selected, nothing in this file changes a single GL call.
 */

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

typedef enum
{
	PSYX_BACKEND_AUTO = 0,	/* probe: ANGLE-backed D3D11 if present, else native GL */
	PSYX_BACKEND_GL,		/* native desktop OpenGL 3.1+ (historical default) */
	PSYX_BACKEND_GLES,		/* OpenGL ES 3.0 — native driver ES, or ANGLE */
	PSYX_BACKEND_D3D11,		/* ANGLE -> Direct3D 11 */
	PSYX_BACKEND_VULKAN,	/* ANGLE -> Vulkan */
	PSYX_BACKEND_WARP,		/* ANGLE -> D3D11 WARP (Microsoft's CPU rasterizer) */
	PSYX_BACKEND_SOFTWARE,	/* ANGLE -> SwiftShader (CPU raster, no GPU needed) */

	PSYX_BACKEND_COUNT
} PsyXBackend;

/* Requested backend. Set BEFORE PsyX_Initialise; ignored afterwards. Default
 * PSYX_BACKEND_GL so an unconfigured build behaves exactly as it always has. */
extern int g_cfg_renderBackend;

/* What we actually ended up on, after fallback. Valid after PsyX_Initialise. */
extern int g_grActiveBackend;

/* Runtime GL flavour of the live context: 1 when the context is OpenGL ES
 * (ANGLE or a native ES driver), 0 for desktop GL. The desktop build compiles
 * BOTH paths and branches on this, because the backend is a user setting now
 * and not a compile-time target. */
extern int g_grIsGLES;

/* Capabilities probed once at context creation. Entry points that exist only in
 * desktop GL resolve to NULL under ANGLE, so every desktop-only call site tests
 * the matching flag instead of assuming. */
typedef struct
{
	int polygonMode;		/* glPolygonMode — wireframe. Desktop only. */
	int getTexImage;		/* glGetTexImage — whole-texture readback. Desktop only. */
	int mapBuffer;			/* glMapBuffer — ES3 has glMapBufferRange instead. */
	int drawBuffer;			/* glDrawBuffer (singular). ES3 has glDrawBuffers. */
	int clearDepthDouble;	/* glClearDepth(double). ES3 has glClearDepthf. */
	int texLevelParam;		/* glGetTexLevelParameteriv — ES 3.1+ only. */
	int debugGroups;		/* glPushDebugGroup / glPopDebugGroup (KHR_debug). */
	int noperspective;		/* 'noperspective' interpolation qualifier in GLSL. */
} PsyXGlCaps;

extern PsyXGlCaps g_grCaps;

/* "gl", "d3d11", "vulkan", ... -> PsyXBackend. Unknown names return
 * PSYX_BACKEND_GL so a typo in a config file cannot stop the game booting. */
extern int			PsyX_Backend_FromName(const char* name);
extern const char*	PsyX_Backend_GetName(int backend);

/* Human-readable, for logs and the options UI. */
extern const char*	PsyX_Backend_GetDescription(int backend);

/* Whether this backend is reached through ANGLE rather than a native driver. */
extern int			PsyX_Backend_IsTranslated(int backend);

/* 1 when ANGLE's libEGL/libGLESv2 are actually loadable next to the exe. A
 * translated backend cannot be honoured without them. */
extern int			PsyX_Backend_AngleAvailable(void);

/* Which GLSL dialect a shader body is written in. The PC port has both. */
typedef enum
{
	/* in / out / texture() / fragColor. Builds as GLSL 1.40 or GLSL ES 3.00. */
	PSYX_GLSL_MODERN = 0,
	/* attribute / varying / texture2D / gl_FragColor. Builds as desktop GLSL
	 * 1.10 (the no-#version default) or GLSL ES 1.00 — which an ES 3.0 context
	 * is still required to accept, so these bodies need no rewriting. */
	PSYX_GLSL_LEGACY = 1,
} PsyXGlslDialect;

/* GLSL preamble for the overlay/UI shaders that PC-port code compiles itself
 * (debug overlay, FMV blit, achievement toasts, ...). Prepend it as a separate
 * glShaderSource string and one shader body builds on every backend: only the
 * #version line and ES's mandatory precision qualifiers actually differ.
 *
 * Pass 1 in fragmentStage for a fragment shader, 0 for a vertex shader. The
 * returned pointer is static storage; do not free it. Meaningful only once the
 * GL context exists — before that it assumes desktop GL. */
extern const char*	PsyX_Shader_Preamble(int fragmentStage, int dialect);

/*
 * ANGLE/EGL context ownership.
 *
 * SDL2 can load ANGLE, but it creates the EGL display with plain eglGetDisplay,
 * and on that path ANGLE ignores ANGLE_DEFAULT_PLATFORM entirely — every
 * backend silently comes up as its Windows default, D3D11 (measured, not
 * assumed). Choosing D3D11 vs Vulkan vs a CPU rasterizer requires
 * eglGetPlatformDisplayEXT with EGL_PLATFORM_ANGLE_TYPE_ANGLE attributes, which
 * SDL exposes no way to pass. So for translated backends PsyCross creates the
 * EGL display, surface and context itself against SDL's window handle, and SDL
 * is left owning only the window.
 *
 * Native GL and native-driver ES still go entirely through SDL; none of this
 * runs for them.
 */

/* 1 once PsyX_Angle_Create has succeeded — swap/proc-address must then route
 * through EGL rather than SDL_GL_*. */
extern int			PsyX_Angle_Active(void);

/* Bring up EGL on an existing native window. nativeWindow is an HWND on
 * Windows. Returns 1 on success; on failure nothing is left current and the
 * caller should fall back to native GL. */
extern int			PsyX_Angle_Create(void* nativeWindow, int backend, int msaaSamples);

extern void			PsyX_Angle_Destroy(void);
extern void			PsyX_Angle_Swap(void);
extern void			PsyX_Angle_SetSwapInterval(int interval);
extern void*		PsyX_Angle_GetProcAddress(const char* name);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif /* PSYX_BACKEND_H */
