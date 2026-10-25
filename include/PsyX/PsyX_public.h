#ifndef EMULATOR_PUBLIC_H
#define EMULATOR_PUBLIC_H

#include <stdio.h>	/* FILE — PsyX_Log_SetStream */

#define CONTROLLER_MAP_FLAG_AXIS		0x4000
#define CONTROLLER_MAP_FLAG_INVERSE		0x8000

typedef struct
{
	int id;

	int kc_square, kc_circle, kc_triangle, kc_cross;

	int kc_l1, kc_l2, kc_l3;
	int kc_r1, kc_r2, kc_r3;

	int kc_start, kc_select;

	int kc_dpad_left, kc_dpad_right, kc_dpad_up, kc_dpad_down;
} PsyXKeyboardMapping;

typedef struct
{
	int id;

	// you can bind axis by adding CONTROLLER_MAP_AXIS_FLAG
	int gc_square, gc_circle, gc_triangle, gc_cross;

	int gc_l1, gc_l2, gc_l3;
	int gc_r1, gc_r2, gc_r3;

	int gc_start, gc_select;

	int gc_dpad_left, gc_dpad_right, gc_dpad_up, gc_dpad_down;

	int gc_axis_left_x, gc_axis_left_y;
	int gc_axis_right_x, gc_axis_right_y;
} PsyXControllerMapping;

typedef void(*GameDebugKeysHandlerFunc)(int nKey, char down);
typedef void(*GameDebugMouseHandlerFunc)(int x, int y);
typedef void(*GameOnTextInputHandler)(const char* buf);

//------------------------------------------------------------------------

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

/* Mapped inputs */
extern PsyXControllerMapping		g_cfg_controllerMapping;
extern PsyXControllerMapping		g_cfg_controllerMapping2;	/* PC: secondary controller binds (second button per action, AND-combined) */
extern PsyXKeyboardMapping			g_cfg_keyboardMapping;
extern PsyXKeyboardMapping			g_cfg_keyboardMapping2;		/* PC: secondary keyboard binds (active w/ allow_mouse_secondary) */
extern int							g_cfg_controllerToSlotMapping[2];
extern int							g_cfg_controllerMovement;	/* PC: 0=analog 1=dpad 2=both */
extern int							g_cfg_allowMouseSecondary;	/* PC: 1 = secondary + mouse-button binds active */
extern unsigned short				g_cfg_mouseButtonMask[8];	/* PC: [SDL button 1..5] -> PSX button bitmask */

/* Game inputs */
extern GameOnTextInputHandler		g_cfg_gameOnTextInput;

/* Graphics configuration */
extern int							g_cfg_swapInterval;
extern int							g_cfg_pgxpZBuffer;
extern int							g_cfg_bilinearFiltering;
extern int							g_cfg_menuFilter;
extern int							g_cfg_disableDpadMovement;
extern int							g_cfg_affineTextures;
extern int							g_cfg_psxDither;
/* Texture filtering mode: 0 off, 1 bilinear, 2 trilinear, 3 anisotropic.
 * Applied per BIND (32-bit replacements, via GL state) and per FRAGMENT (PSX
 * CLUT textures, in the shader, since interpolating palette indices is
 * meaningless). g_cfg_bilinearFiltering remains as the derived "any filtering"
 * flag for callers that only need a yes/no. */
extern int							g_cfg_textureFilter;
extern int							g_cfg_anisoLevel;
extern int							g_cfg_pgxpTextureCorrection;

/* PC port: MSAA sample count for the default framebuffer (0 = off, 2/4/8). Must
 * be set BEFORE PsyX_Initialise — it drives the SDL multisample GL attributes at
 * context-creation time. */
extern int							g_cfg_msaaSamples;

/* PC port: confine the mouse pointer to the game window while it holds focus
 * (0 = never). Only ever applied in fullscreen/borderless — a windowed game
 * that traps the pointer is hostile. Runtime-settable. */
extern int							g_cfg_confineCursor;

/* PC port: full-screen post-process look (0 = off, 1.. = a built-in filter).
 * Safe to change at runtime (launcher config + F2 in-game cycle). */
extern int							g_cfg_postProcess;

/* PC port: tone-map operator on the final image (0=off,1=Reinhard,2=ACES,
 * 3=Filmic). Runtime-settable (launcher config + F3 in-game cycle). */
extern int							g_cfg_tonemap;

/* PC port: per-pixel (fragment-shader) flashlight cone. Runtime-settable
 * (launcher config + F4 in-game toggle). */
extern int							g_PsyX_UsePerPixelFlashlight;

/* PC port: real flashlight shadow mapping (depth pre-pass from the light POV;
 * requires the per-pixel flashlight on). Runtime-settable (config + `shadows`
 * console). Bias is the light-clip depth-compare epsilon (`shadowbias`). */
extern int							g_PsyX_UseFlashlightShadows;
extern float						g_PsyX_FlashlightShadowBias;
/* Normal-offset amount (fraction of distance-to-light) that pushes the receiver
 * off its surface toward the light before the shadow lookup — kills grazing-angle
 * self-shadow acne on flat surfaces. Tunable via `shadownormal`. */
extern float						g_PsyX_FlashlightShadowNormalOffset;
/* How much light a fully-occluded pixel loses (1 = black, 0.5 = soft half-shadow).
 * Keeps the close point light's oversized clutter umbras subtle. `shadowstrength`. */
extern float						g_PsyX_FlashlightShadowStrength;
/* Contact-shadow fade distance (view units): a receiver this far behind its
 * occluder casts no shadow, so props drop tight contact shadows instead of tall
 * silhouettes smeared onto far walls. `shadowfade`. */
extern float						g_PsyX_FlashlightShadowFadeDist;
/* First-person shadow light drop (view-space units) — offsets the shadow light
 * below the eye so FPS shadows aren't self-cancelled by a camera-coincident light. */
extern float						g_PsyX_FlashlightShadowFpsDrop;
/* Set 1 by game code around a draw whose geometry should NOT cast a flashlight
 * shadow (Harry's own body); reset to 0 after. Per-vertex, rides the view-space FIFO. */
extern int							g_PsyX_NoShadowCast;

/* 0 = lit normally, 1 = fully faded. Set by game code around a character draw
 * whose LIGHTING the game is scaling down (func_8003DA9C's `timer`, which is how
 * a Larval Stalker vanishes). The per-pixel flashlight adds its light on top of
 * the TEXTURE, not the vertex colour, so without this it happily relights a
 * character the game has already faded to nothing. Per-vertex, rides the same
 * view-space FIFO as g_PsyX_NoShadowCast. */
extern float						g_PsyX_CharaFade;

/* PC port: optional per-pixel lighting for the stock world and characters.
 * The PSX lights every vertex with the GTE and Gouraud-shades the colour
 * across the face; with this on, the game hands the renderer each lit prim's
 * view-space corner normals and the frame's light, and the shader re-runs the
 * same GTE light sum at every pixel. Off (g_PsyX_PpLitActive 0) nothing is
 * recorded, uploaded or bound, and the shader skips the branch on a uniform. */
extern int							g_PsyX_PpLitActive;
typedef struct PsyX_PpLight
{
	int		mode;			/* g_WorldEnvWork.field_0: 1 point, 2 directional, 0 none */
	float	lightPos[3];	/* view space, same units as the GTE view-space capture */
	float	lightDir[3];	/* the light row (field_74) rotated into view space, Q12 */
	float	tint[3];		/* world tint, 0..255 */
	float	lc[3];			/* colour matrix column 0, Q12 */
	float	bk[3];			/* back colour, 0..255 */
	float	ambient;		/* field_20 */
	float	cap;			/* field_3 */
	float	dqa;			/* depth-cue A the light divide runs under */
} PsyX_PpLight;
extern void PsyX_PpLitSetLight(const PsyX_PpLight* light);
/* kind 1 = world (func_80057658 / func_80057A3C), 2 = character (func_8005AA08).
 * n holds the corners' view-space normals as the game stores them (inward),
 * x0 y0 z0 x1 ..., in the prim's own corner order (3 or 4). */
extern void PsyX_PpLitPrim(const void* prim, int kind, const float* n);
/* Fill light for characters drawn per pixel, a direction in view space toward
 * the light and a 0..1 strength. 0 strength leaves the lighting untouched. */
extern float						g_PsyX_HeroLight[4];

/* Optional per-sound sample replacement. A voice plays from an SPU address; the
 * host may answer that address with its own PCM (any length or rate) instead of
 * the ADPCM resident there, which is how loose-file sound mods bypass both the
 * bank container and its size ceiling. Returns non-zero on a hit. NULL by
 * default, so PsyCross on its own behaves exactly as before. */
/* outRate is the WAV's own sample rate, so the mixer can play a replacement at
 * the rate it was authored at instead of assuming the original's. 0 = unknown,
 * which keeps the old behaviour for that sample. */
typedef int (*PsyX_SfxOverrideFn)(int spuAddr, const short** outPcm, int* outSampleCount,
                                  int* outRate);
extern PsyX_SfxOverrideFn			g_PsyX_SfxOverride;

/* PC port: per-pixel flashlight cone parameters, pushed once per frame by game
 * code (bodyprog world-lighting setup). Position and direction are in VIEW
 * (camera) space — the same space as the per-vertex GrVertex.vsx/vsy/vsz the GTE
 * captures. The shader only consumes these when (g_PsyX_UsePerPixelFlashlight &&
 * g_PsyX_FlashlightActive). g_PsyX_FlashlightActive defaults 0 so nothing is lit
 * until the game pushes a valid light for the frame. */
extern int							g_PsyX_FlashlightActive;     /* 1 = push light this frame */
extern float						g_PsyX_FlashlightPos[3];     /* view-space xyz */
extern float						g_PsyX_FlashlightShadowPos[3]; /* view-space physical light pos for the shadow map (chest/hand); == FlashlightPos in TPS */
extern float						g_PsyX_FlashlightDir[3];     /* view-space unit dir the cone points along */
extern float						g_PsyX_FlashlightColor[3];   /* additive RGB at full strength */
extern float						g_PsyX_FlashlightInnerCos;   /* cos(inner half-angle) */
extern float						g_PsyX_FlashlightOuterCos;   /* cos(outer half-angle) */
extern float						g_PsyX_FlashlightRange;      /* distance falloff, view-space units */

/* PC port (Silent Hill): runtime master gate for PGXP perspective correction.
 * Set this from game code AFTER PsyX_Initialise. When the binary is built with
 * USE_PGXP=1 but this is 0, the prim emitters write a_zw=0 so the shader takes
 * the 2D-ortho (affine PSX-look) branch. */
extern int							g_PsxUsePgxp;

/* PC port: PGXP shadow-memory model (DuckStation-faithful). The GTE records the
 * precise projection of each stored vertex word keyed by its native address
 * (Shadow_Store, via the gte_stsxy* macros); drawers propagate that shadow when
 * they copy a vertex word into a prim field (Shadow_Copy); the GPU resolves each
 * prim vertex by address at draw. Entirely inside PsyCross; no effect when off. */
#if defined(__cplusplus)
extern "C" {
#endif
void Shadow_Store(void* addr, float x, float y, float w, unsigned value);
void Shadow_Copy(void* dst, const void* src);
void PGXP_FrameReset(void);
void PGXP_CoverageTick(void); /* per-frame; dumps [PGXP] det/miss when on */
float PGXP_GetSzMax(void);    /* prev-frame max SZ for shader depth normalize */
#if defined(__cplusplus)
}
#endif

/* PC port: per-frame override that suppresses dither even when
 * g_cfg_psxDither is enabled. Game code sets this to 1 on 2D-only states
 * (logos, menus, options, save/load, map screen, inventory) so the dither
 * pattern doesn't visibly chew up flat-shaded 2D art. Cleared back to 0
 * during 3D gameplay so the PSX look is preserved. The render path
 * effectively uses (g_cfg_psxDither && !g_PsxDitherSuppressed). */
extern int							g_PsxDitherSuppressed;

/* Debug inputs */
extern GameDebugKeysHandlerFunc		g_dbg_gameDebugKeys;
extern GameDebugMouseHandlerFunc	g_dbg_gameDebugMouse;

/* PC port: map a window-pixel point to a [0,1] fraction of the letterboxed 4:3
 * display viewport (the pillarbox rect the renderer installs). Returns 1 if the
 * point is inside the viewport, 0 if it falls in the black bars. Used to convert
 * an OS mouse position into the game's 2D coordinate space. */
extern int							PsyX_MapWindowToViewport(int mx, int my, float* outFracX, float* outFracY);

/* PC port: route PsyX logging into the host's stdio stream (pass NULL to
 * silence PsyX logging entirely). Call BEFORE PsyX_Initialise so PsyX never
 * creates its own "<appName>.log". PsyX will flush but never fclose an
 * externally-provided stream — the host owns its handle. */
extern void PsyX_Log_SetStream(FILE* stream);

/* Usually called at the beginning of main function */
extern void PsyX_Initialise(char* windowName, int screenWidth, int screenHeight, int fullscreen);

/* Cleans all resources and closes open instances */
extern void PsyX_Shutdown(void);

/* Returns the screen size dimensions */
extern void PsyX_GetScreenSize(int* screenWidth, int* screenHeight);

/* Sets mouse cursor position */
extern void PsyX_SetCursorPosition(int x, int y);

/* Usually called after ClearOTag/ClearOTagR */
extern char PsyX_BeginScene(void);

/* Usually called after DrawOTag/DrawOTagEnv */
extern void PsyX_EndScene(void);

/* Explicitly updates emulator input loop */
extern void PsyX_UpdateInput(void);

/* Returns keyboard mapping index */
extern int PsyX_LookupKeyboardMapping(const char* str, int default_value);

/* Returns controller mapping index */
extern int PsyX_LookupGameControllerMapping(const char* str, int default_value);

/* Is a bind held on any attached physical controller? Takes a value encoded by
 * PsyX_LookupGameControllerMapping, so it accepts axes (the triggers) as well as
 * digital buttons — unlike PsyX_RawControllerButtonHeld, which is buttons only. */
extern int PsyX_RawControllerBindHeld(int buttonOrAxis);

/* In-game controls panel helpers (PsyX_pad.cpp): the first attached
 * controller's SDL name / SDL_GameControllerType (NULL / -1 when none), the
 * bind name of a held button or trigger (NULL when none), and a signed stick
 * axis across all attached controllers. */
/* The picture's rect in render pixels (pillarbox rule included), from the
 * current display flags rather than the last GL_VIEWPORT. */
extern void PsyX_GetDisplayViewport(int* outX, int* outY, int* outW, int* outH);
extern const char* PsyX_Pad_ConnectedControllerName(void);
extern int         PsyX_Pad_ConnectedControllerType(void);
extern const char* PsyX_Pad_HeldBindName(void);
extern int         PsyX_Pad_AxisValue(int sdlAxis);

/* Screen size of emulated PSX viewport with widescreen offsets */
extern void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect);

/* Waits for timer */
extern void PsyX_WaitForTimestep(int count);

/* Changes swap interval state */
extern void PsyX_EnableSwapInterval(int enable);

/* Changes swap interval interval interval */
extern void PsyX_SetSwapInterval(int interval);

/* PC port: apply window mode + resolution at runtime.
 * fullscreen: 0 = windowed, 1 = exclusive fullscreen, 2 = borderless desktop. */
extern void PsyX_ApplyWindowState(int width, int height, int fullscreen);

/* PC port: re-evaluate mouse confinement (see g_cfg_confineCursor). Idempotent;
 * call after anything that changes window mode or focus. */
extern void PsyX_UpdateMouseConfinement(void);

/* PC port: apply vsync at runtime (0 = off, nonzero = on). Routes through the
 * per-frame swap-interval path so the change persists. */
extern void PsyX_ApplyVsync(int vsync);

/* 1 while Cross (A) or Start is held on any connected game controller.
 * For blocking loops (FMV playback) that run outside the game's pad
 * polling and otherwise only see the keyboard. */
extern int PsyX_Pad_SkipButtonHeld(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif