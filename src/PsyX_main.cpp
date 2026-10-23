#ifdef _WIN32
#include <windows.h>
#endif // _WIN32

#include "PsyX_main.h"

#include "PsyX/PsyX_version.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/util/timer.h"

#include "gpu/PsyX_GPU.h"
#include "pad/PsyX_pad.h"

#include "platform.h"
#include "util/crash_handler.h"

#include "psx/libetc.h"
#include "psx/libgte.h"
#include "psx/libgpu.h"
#include "psx/libspu.h"
#include "audio/PsyX_SPUAL.h"

#include <assert.h>
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>

#include <exception>

#include "PsyX/PsyX_render.h"

#ifndef _WIN32
#ifdef __EMSCRIPTEN__
int strcasecmp(const char* _l, const char* _r)
{
	const u_char* l = (u_char*)_l, * r = (u_char*)_r;
	for (; *l && *r && (*l == *r || tolower(*l) == tolower(*r)); l++, r++);
	return tolower(*l) - tolower(*r);
}
#else
#include <strings.h>
#endif
#define _stricmp(s1, s2) strcasecmp(s1, s2)
#endif // _WIN32

SDL_Window* g_window = NULL;
extern int  g_presentWidth, g_presentHeight;
extern int  g_cfgRenderWidth, g_cfgRenderHeight, g_cfgFullscreenMode;
extern "C" void GR_ApplyPresentSize(int realW, int realH);


/* PC port: mouse confinement. Default on; the launcher/config can clear it. */
int g_cfg_confineCursor = 1;

/* Keep the pointer inside the window while the game holds focus, the way
 * borderless games normally behave. Without this a multi-monitor borderless
 * setup lets the cursor walk onto the next screen during any mouse-driven
 * moment (item pickup, inventory, menus, puzzles) and a click there tabs the
 * game out (GitHub #87).
 *
 * MOUSE grab only, never SDL_SetWindowKeyboardGrab: the keyboard grab is what
 * would swallow Alt+Tab and the Windows key, and it is a separate SDL call, so
 * both stay working. SDL's Windows backend only clips while the window is
 * focused and drops the clip on deactivation, so alt-tabbing out releases the
 * pointer on its own and coming back re-clips it.
 *
 * Windowed mode is deliberately never confined - trapping the pointer in a
 * window the user can see past is hostile, and the reported problem is specific
 * to fullscreen/borderless. SDL_WINDOW_FULLSCREEN is set for exclusive AND
 * desktop-fullscreen, so this one test covers both. */
void PsyX_UpdateMouseConfinement(void)
{
	if (g_window == NULL)
		return;

	{
		const Uint32 flags = SDL_GetWindowFlags(g_window);
		const int    want  = (g_cfg_confineCursor != 0) &&
		                     (flags & SDL_WINDOW_FULLSCREEN) != 0;

		/* Relative mode (TPS/OTS mouse-look) already confines the pointer and
		 * owns the grab state; leave it alone so a mode change mid-look does
		 * not drop its capture. */
		if (SDL_GetRelativeMouseMode() == SDL_TRUE)
			return;

		if ((SDL_GetWindowMouseGrab(g_window) == SDL_TRUE) != (want != 0))
			SDL_SetWindowMouseGrab(g_window, want ? SDL_TRUE : SDL_FALSE);
	}
}
int g_swapInterval = 1;
int g_enableSwapInterval = 1;
int g_skipSwapInterval = 0;
timerCtx_t g_vblTimer;

int							g_cfg_swapInterval = 0;
PsyXKeyboardMapping			g_cfg_keyboardMapping;
PsyXKeyboardMapping			g_cfg_keyboardMapping2 = {0};	/* secondary binds; 0 = SDL_SCANCODE_UNKNOWN (unset) */
PsyXControllerMapping		g_cfg_controllerMapping;
PsyXControllerMapping		g_cfg_controllerMapping2;	/* secondary controller binds; all fields BUTTON_INVALID (unset) until configured */
int							g_cfg_allowMouseSecondary = 0;
unsigned short				g_cfg_mouseButtonMask[8] = {0};	/* [SDL button 1..5] -> PSX button bitmask */
GameOnTextInputHandler		g_cfg_gameOnTextInput = NULL;

GameDebugKeysHandlerFunc	g_dbg_gameDebugKeys = NULL;
GameDebugMouseHandlerFunc	g_dbg_gameDebugMouse = NULL;
int							g_dbg_polygonSelected = 0;

enum EPsxCounters
{
	PsxCounter_VBLANK,

	PsxCounter_Num
};

volatile int g_psxSysCounters[PsxCounter_Num];

SDL_Thread* g_intrThread = NULL;
SDL_mutex* g_intrMutex = NULL;
volatile char g_stopIntrThread = 0;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern void(*vsync_callback)(void);
extern int g_rcnt2_timer_active;
extern void PsyX_PumpRCnt2Timer(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

extern int	PsyX_Pad_InitSystem();
extern void PsyX_Pad_Event_ControllerRemoved(Sint32 deviceId);
extern void PsyX_Pad_Event_ControllerAdded(Sint32 deviceId);

extern int	GR_InitialisePSX();
extern int	GR_InitialiseRender(char* windowName, int width, int height, int fullscreen);

extern void GR_ResetDevice();
extern void GR_Shutdown();
extern void GR_BeginScene();
extern void GR_EndScene();
extern "C" void GR_VoidProbeScene(void);
extern void GR_UpdateSwapIntervalState(int swapInterval);

/* Default NTSC: the USA disc never calls SetVideoMode, so g_vmode stayed -1
 * (!= MODE_NTSC) and every vblank-rate decision fell through to PAL 50Hz —
 * making the frame limiter miss by 5/6 (a 60fps cap ran 50, 30 ran 25) and
 * frame-locked sequence audio play ~17% slow. */
int g_vmode = MODE_NTSC;
int g_frameSkip = 0;

#ifdef __EMSCRIPTEN__

int g_emIntrInterval = -1;
int g_intrVMode = MODE_NTSC;
double g_emOldDate = 0;

void emIntrCallback(void* userData)
{
	double timestep = g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL;

	int newVBlank = (Util_GetHPCTime(&g_vblTimer, 0) / timestep) + g_frameSkip;

	int diff = newVBlank - g_psxSysCounters[PsxCounter_VBLANK];

	while (diff--)
	{
		if (vsync_callback)
			vsync_callback();

		g_psxSysCounters[PsxCounter_VBLANK]++;
	}
}

EM_BOOL emIntrCallback2(double time, void* userData)
{
	emIntrCallback(userData);
	return g_stopIntrThread ? EM_FALSE : EM_TRUE;
}

#endif

int PsyX_Sys_SetVMode(int mode)
{
	int old = g_vmode;
	g_vmode = mode;

#ifdef __EMSCRIPTEN__
	if (old != g_vmode)
	{
		//if(g_emIntrInterval != -1)
		//	emscripten_clear_interval(g_emIntrInterval);
		g_stopIntrThread = 1;

		emscripten_sleep(100);

		g_stopIntrThread = 0;
		emscripten_set_timeout_loop(emIntrCallback2, 1.0, NULL);
	}
#endif

	return old;
}

int PsyX_Sys_GetVBlankCount()
{
	if (g_skipSwapInterval)
	{
		// extra speedup.
		// does not affect `vsync_callback` count
		g_psxSysCounters[PsxCounter_VBLANK] += 1;
		g_frameSkip++;
	}
	
	return g_psxSysCounters[PsxCounter_VBLANK];
}

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#elif defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

/* Sleeps for about `seconds`. Returns 0 where no sub-millisecond sleep exists
 * (Windows before 10 1803, consoles), and the caller keeps spinning instead:
 * a coarse 15 ms Sleep would drop vblanks and slow the RCnt2 music clock. */
static int IntrPreciseSleep(double seconds)
{
#ifdef _WIN32
	static HANDLE s_timer = NULL;
	static int    s_tried = 0;
	LARGE_INTEGER due;

	if (!s_tried)
	{
		s_tried = 1;
		s_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}
	if (s_timer == NULL)
		return 0;
	due.QuadPart = -(LONGLONG)(seconds * 1e7);
	if (!SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE))
		return 0;
	WaitForSingleObject(s_timer, INFINITE);
	return 1;
#elif defined(__linux__) || defined(__APPLE__)
	struct timespec ts;
	ts.tv_sec  = 0;
	ts.tv_nsec = (long)(seconds * 1e9);
	nanosleep(&ts, NULL);
	return 1;
#else
	(void)seconds;
	return 0;
#endif
}

static inline void IntrCpuRelax(void)
{
#ifdef _WIN32
	YieldProcessor();
#endif
}

/* Events run on absolute deadlines and the thread sleeps between them. It used
 * to spin flat out and re-arm each timer at its fire time, holding a whole core
 * for the session, which weak dual/quad-core machines feel as frame time. A
 * deadline that has fallen more than a period behind (a stall) re-arms from
 * now, so a stall still yields one event, not a burst. */
int intrThreadMain(void* data)
{
	/* PSX RCnt2 = system clock / 8 = 4233600 Hz.
	 * Target 7328 → fires at 4233600/7328 ≈ 577.8 Hz → ~1.73ms period */
	const double rcnt2Period = 1.0 / 577.8;
	const double freq        = (double)SDL_GetPerformanceFrequency();
	const Uint64 t0          = SDL_GetPerformanceCounter();
	double       nextVbl     = g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL;
	double       nextRcnt2   = 0.0;
	int          rcnt2Armed  = 0;

	Util_InitHPCTimer(&g_vblTimer);

	while (!g_stopIntrThread)
	{
		const double timestep = g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL;
		double       now      = (double)(SDL_GetPerformanceCounter() - t0) / freq;
		double       wake;

		if (now >= nextVbl)
		{
			SDL_LockMutex(g_intrMutex);

			if (vsync_callback)
				vsync_callback();

			SDL_UnlockMutex(g_intrMutex);

			// do vblank events
			g_psxSysCounters[PsxCounter_VBLANK]++;

			nextVbl += timestep;
			if (nextVbl <= now)
				nextVbl = now + timestep;
		}

		/* Pump RCnt2 timer for BGM/MIDI sequencer */
		if (g_rcnt2_timer_active)
		{
			if (!rcnt2Armed)
			{
				rcnt2Armed = 1;
				nextRcnt2  = now;
			}
			if (now >= nextRcnt2)
			{
				SDL_LockMutex(g_intrMutex);
				PsyX_PumpRCnt2Timer();
				SDL_UnlockMutex(g_intrMutex);

				nextRcnt2 += rcnt2Period;
				if (nextRcnt2 <= now)
					nextRcnt2 = now + rcnt2Period;
			}
		}
		else
			rcnt2Armed = 0;

		/* Advance SPU ADSR envelopes on the audio-timing thread, NOT from the
		 * render thread (PsyX_EndScene) — that placement deadlocked. Takes only
		 * g_SpuMutex (never nested under g_intrMutex), throttles on the
		 * SDL_GetTicks ms delta, no-op unless `adsr 1`. */
		PsyX_SPUAL_Update();

		/* Wake at least every 2 ms so the ms-throttled SPU update keeps its
		 * cadence while RCnt2 is idle. Sleep short of the deadline and spin the
		 * last half millisecond: the high-resolution timer can overshoot that. */
		wake = nextVbl;
		if (rcnt2Armed && nextRcnt2 < wake)
			wake = nextRcnt2;
		if (now + 0.002 < wake)
			wake = now + 0.002;
		now = (double)(SDL_GetPerformanceCounter() - t0) / freq;
		if (wake - now > 0.0007)
		{
			if (!IntrPreciseSleep(wake - now - 0.0005))
				IntrCpuRelax();
		}
		else
			IntrCpuRelax();
	}

	return 0;
}

static int PsyX_Sys_InitialiseCore()
{
#ifdef __EMSCRIPTEN__
	Util_InitHPCTimer(&g_vblTimer);
#else

	g_intrThread = SDL_CreateThread(intrThreadMain, "psyX_intr", NULL);

	if (NULL == g_intrThread)
	{
		eprinterr("SDL_CreateThread failed: %s\n", SDL_GetError());
		return 0;
	}
	
	g_intrMutex = SDL_CreateMutex();
	if (NULL == g_intrMutex)
	{
		eprinterr("SDL_CreateMutex failed: %s\n", SDL_GetError());
		return 0;
	}
#endif
	return 1;
}

static void PsyX_Sys_InitialiseInput()
{
	g_cfg_keyboardMapping.kc_square = SDL_SCANCODE_X;
	g_cfg_keyboardMapping.kc_circle = SDL_SCANCODE_V;
	g_cfg_keyboardMapping.kc_triangle = SDL_SCANCODE_Z;
	g_cfg_keyboardMapping.kc_cross = SDL_SCANCODE_C;

	/* Sidestep moved to A/D, aim moved to LSHIFT, view moved to RSHIFT.
	 * RCTRL was unreliable on the user's Win11 setup (never reached
	 * btnsHeld_C even though SDL_SCANCODE_RCTRL is the documented
	 * scancode for the right Control key). LCTRL also retired so the
	 * shift keys mirror cleanly. */
	g_cfg_keyboardMapping.kc_l1 = SDL_SCANCODE_A;            /* sidestep left */
	g_cfg_keyboardMapping.kc_l2 = SDL_SCANCODE_RSHIFT;       /* view */
	g_cfg_keyboardMapping.kc_l3 = SDL_SCANCODE_UNKNOWN  /* [ reserved for effect-intensity control */;

	g_cfg_keyboardMapping.kc_r1 = SDL_SCANCODE_D;            /* sidestep right */
	g_cfg_keyboardMapping.kc_r2 = SDL_SCANCODE_LSHIFT;       /* aim */
	g_cfg_keyboardMapping.kc_r3 = SDL_SCANCODE_UNKNOWN /* ] reserved for effect-intensity control */;

	g_cfg_keyboardMapping.kc_dpad_up = SDL_SCANCODE_UP;
	g_cfg_keyboardMapping.kc_dpad_down = SDL_SCANCODE_DOWN;
	g_cfg_keyboardMapping.kc_dpad_left = SDL_SCANCODE_LEFT;
	g_cfg_keyboardMapping.kc_dpad_right = SDL_SCANCODE_RIGHT;

	g_cfg_keyboardMapping.kc_select = SDL_SCANCODE_SPACE;
	g_cfg_keyboardMapping.kc_start = SDL_SCANCODE_RETURN;

	//----------------
	g_cfg_controllerMapping.gc_square = SDL_CONTROLLER_BUTTON_X;
	g_cfg_controllerMapping.gc_circle = SDL_CONTROLLER_BUTTON_B;
	g_cfg_controllerMapping.gc_triangle = SDL_CONTROLLER_BUTTON_Y;
	g_cfg_controllerMapping.gc_cross = SDL_CONTROLLER_BUTTON_A;

	g_cfg_controllerMapping.gc_l1 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	g_cfg_controllerMapping.gc_l2 = SDL_CONTROLLER_AXIS_TRIGGERLEFT | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_l3 = SDL_CONTROLLER_BUTTON_LEFTSTICK;

	g_cfg_controllerMapping.gc_r1 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	g_cfg_controllerMapping.gc_r2 = SDL_CONTROLLER_AXIS_TRIGGERRIGHT | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_r3 = SDL_CONTROLLER_BUTTON_RIGHTSTICK;

	g_cfg_controllerMapping.gc_dpad_up = SDL_CONTROLLER_BUTTON_DPAD_UP;
	g_cfg_controllerMapping.gc_dpad_down = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	g_cfg_controllerMapping.gc_dpad_left = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	g_cfg_controllerMapping.gc_dpad_right = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;

	g_cfg_controllerMapping.gc_select = SDL_CONTROLLER_BUTTON_BACK;
	g_cfg_controllerMapping.gc_start = SDL_CONTROLLER_BUTTON_START;

	g_cfg_controllerMapping.gc_axis_left_x = SDL_CONTROLLER_AXIS_LEFTX | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_left_y = SDL_CONTROLLER_AXIS_LEFTY | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_right_x = SDL_CONTROLLER_AXIS_RIGHTX | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_right_y = SDL_CONTROLLER_AXIS_RIGHTY | CONTROLLER_MAP_FLAG_AXIS;

	/* Secondary controller binds default to all-unset (every field
	 * SDL_CONTROLLER_BUTTON_INVALID == -1). The game's Pc_ApplyControlConfig sets
	 * the action buttons it wants; anything left here reads as "not pressed". */
	memset(&g_cfg_controllerMapping2, 0xFF, sizeof(g_cfg_controllerMapping2));

	PsyX_Pad_InitSystem();
}

// Keyboard mapping lookup
int PsyX_LookupKeyboardMapping(const char* str, int default_value)
{
	const char* scancodeName;
	int i;

	if (str)
	{
		if (!_stricmp("NONE", str))
			return SDL_SCANCODE_UNKNOWN;

		for (i = 0; i < SDL_NUM_SCANCODES; i++)
		{
			scancodeName = SDL_GetScancodeName((SDL_Scancode)i);

			if (strlen(scancodeName) && !_stricmp(scancodeName, str))
			{
				return i;
			}
		}
	}

	return default_value;
}

// Game controller mapping lookup
// Available controller binds(refer to SDL2 game controller)
//
// Axes:
//	leftx lefty
//	rightx righty
//	lefttrigger righttrigger
//
// NOTE: adding `-` before axis names makes it inverse, so `-leftx` inverse left stick X axis
//
// Buttons:
// 	a, b, x, y
// 	back guide start
// 	leftstick rightstick
// 	leftshoulder rightshoulder
// 	dpup dpdown dpleft dpright

int PsyX_LookupGameControllerMapping(const char* str, int default_value)
{
	const char* axisStr;
	const char* buttonOrAxisName;
	int i, axisFlags;

	if (str)
	{
		axisFlags = CONTROLLER_MAP_FLAG_AXIS;
		axisStr = str;

		if (*axisStr == '-')
		{
			axisFlags |= CONTROLLER_MAP_FLAG_INVERSE;
			axisStr++;
		}

		if (!_stricmp("NONE", str))
			return SDL_CONTROLLER_BUTTON_INVALID;

		// check buttons
		for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
		{
			buttonOrAxisName = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)i);

			if (strlen(buttonOrAxisName) && !_stricmp(buttonOrAxisName, str))
			{
				return i;
			}
		}

		// Check axes
		for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
		{
			buttonOrAxisName = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)i);

			if (strlen(buttonOrAxisName) && !_stricmp(buttonOrAxisName, axisStr))
			{
				return i | axisFlags;
			}
		}
	}

	return default_value;
}

char* g_appNameStr = NULL;

void PsyX_GetWindowName(char* buffer)
{
#ifdef _DEBUG
	sprintf(buffer, "%s | Debug", g_appNameStr);
#else
	sprintf(buffer, "%s", g_appNameStr);
#endif
}

FILE* g_logStream = NULL;

/* Set by PsyX_Exit() (the normal close path: SDL_QUIT / window-close / Alt+F4).
 * A benign C++ exception escapes during exit()/static-destruction teardown on
 * every normal close (the thrower is not yet isolated), tripping std::terminate
 * AFTER a fully clean shutdown. When this flag is set the terminate handler
 * treats it as the normal exit it is — no misleading FATAL line, clean exit. */
static volatile int g_psyxNormalExit = 0;

/* When the host app routes PsyX logging into its own stream (or NULL to
 * silence it), PsyX must not fopen its own "<app>.log" and must never
 * fclose the host's handle at shutdown — doing so left the host's stdio
 * FILE* dangling for any logging that ran after PsyX_Shutdown. */
static int g_logStreamExternal = 0;

void PsyX_Log_SetStream(FILE* stream)
{
	if (g_logStream && !g_logStreamExternal)
		fclose(g_logStream);

	g_logStream = stream;
	g_logStreamExternal = 1;
}

// intialise logging
void PsyX_Log_Initialise()
{
	char appLogFilename[128];

	if (g_logStreamExternal)
		return;

	sprintf(appLogFilename, "%s.log", g_appNameStr);

	g_logStream = fopen(appLogFilename, "wb");

	if (!g_logStream)
		eprinterr("Error - cannot create log file '%s'\n", appLogFilename);
}

void PsyX_Log_Finalise()
{
	if (g_logStreamExternal)
	{
		if (g_logStream)
			fflush(g_logStream);
		return;
	}

	PsyX_Log_Warning("---- LOG CLOSED ----\n");

	if (g_logStream)
		fclose(g_logStream);

	g_logStream = NULL;
}

void PsyX_Log_Flush()
{
	if (g_logStream)
		fflush(g_logStream);
}

// spew types
typedef enum
{
	SPEW_NORM,
	SPEW_INFO,
	SPEW_WARNING,
	SPEW_ERROR,
	SPEW_SUCCESS,
}SpewType_t;

#ifdef _WIN32
static unsigned short g_InitialColor = 0xFFFF;
static unsigned short g_LastColor = 0xFFFF;
static unsigned short g_BadColor = 0xFFFF;
static WORD g_BackgroundFlags = 0xFFFF;
CRITICAL_SECTION g_SpewCS;
char g_bSpewCSInitted = 0;

static void Spew_GetInitialColors()
{
	// Get the old background attributes.
	CONSOLE_SCREEN_BUFFER_INFO oldInfo;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &oldInfo);
	g_InitialColor = g_LastColor = oldInfo.wAttributes & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	g_BackgroundFlags = oldInfo.wAttributes & (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY);

	g_BadColor = 0;
	if (g_BackgroundFlags & BACKGROUND_RED)
		g_BadColor |= FOREGROUND_RED;
	if (g_BackgroundFlags & BACKGROUND_GREEN)
		g_BadColor |= FOREGROUND_GREEN;
	if (g_BackgroundFlags & BACKGROUND_BLUE)
		g_BadColor |= FOREGROUND_BLUE;
	if (g_BackgroundFlags & BACKGROUND_INTENSITY)
		g_BadColor |= FOREGROUND_INTENSITY;
}

static WORD Spew_SetConsoleTextColor(int red, int green, int blue, int intensity)
{
	WORD ret = g_LastColor;

	g_LastColor = 0;
	if (red)	g_LastColor |= FOREGROUND_RED;
	if (green) g_LastColor |= FOREGROUND_GREEN;
	if (blue)  g_LastColor |= FOREGROUND_BLUE;
	if (intensity) g_LastColor |= FOREGROUND_INTENSITY;

	// Just use the initial color if there's a match...
	if (g_LastColor == g_BadColor)
		g_LastColor = g_InitialColor;

	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), g_LastColor | g_BackgroundFlags);
	return ret;
}

static void Spew_RestoreConsoleTextColor(WORD color)
{
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color | g_BackgroundFlags);
	g_LastColor = color;
}

void Spew_ConDebugSpew(SpewType_t type, char* text)
{
	// Hopefully two threads won't call this simultaneously right at the start!
	if (!g_bSpewCSInitted)
	{
		Spew_GetInitialColors();
		InitializeCriticalSection(&g_SpewCS);
		g_bSpewCSInitted = 1;
	}

	WORD old;
	EnterCriticalSection(&g_SpewCS);
	{
		if (type == SPEW_NORM)
		{
			old = Spew_SetConsoleTextColor(1, 1, 1, 0);
		}
		else if (type == SPEW_WARNING)
		{
			old = Spew_SetConsoleTextColor(1, 1, 0, 1);
		}
		else if (type == SPEW_SUCCESS)
		{
			old = Spew_SetConsoleTextColor(0, 1, 0, 1);
		}
		else if (type == SPEW_ERROR)
		{
			old = Spew_SetConsoleTextColor(1, 0, 0, 1);
		}
		else if (type == SPEW_INFO)
		{
			old = Spew_SetConsoleTextColor(0, 1, 1, 1);
		}
		else
		{
			old = Spew_SetConsoleTextColor(1, 1, 1, 1);
		}

		OutputDebugStringA(text);
		printf("%s", text);

		Spew_RestoreConsoleTextColor(old);
	}
	LeaveCriticalSection(&g_SpewCS);
}
#endif

void PrintMessageToOutput(SpewType_t spewtype, char const* pMsgFormat, va_list args)
{
	static char pTempBuffer[4096];
	int len = 0;
	vsprintf(&pTempBuffer[len], pMsgFormat, args);

#ifdef WIN32
	Spew_ConDebugSpew(spewtype, pTempBuffer);
#elif defined(__EMSCRIPTEN__)
	if (spewtype == SPEW_INFO)
	{
		EM_ASM({
			console.info(UTF8ToString($0));
		}, pTempBuffer);
	}
	else if (spewtype == SPEW_WARNING)
	{
		EM_ASM({
			console.warn(UTF8ToString($0));
		}, pTempBuffer);
	}
	else if (spewtype == SPEW_ERROR)
	{
		EM_ASM({
			console.error(UTF8ToString($0));
		}, pTempBuffer);
	}
	else
	{
		EM_ASM({
			console.log(UTF8ToString($0));
		}, pTempBuffer);
	}
#else
	printf(pTempBuffer);
#endif

	if(g_logStream)
		fprintf(g_logStream, pTempBuffer);
}

void PsyX_Log(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_NORM, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Info(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_INFO, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Warning(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_WARNING, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Error(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_ERROR, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Success(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_SUCCESS, fmt, argptr);
	va_end(argptr);
}


static void sh_terminate_handler()
{
	/* abort() skips atexit/stdio teardown, so the host's buffered log
	 * (up to 64KB of tail) would be silently lost on every terminate.
	 * Flush it here so the log survives. */
	if (g_logStream)
		fflush(g_logStream);

	if (g_psyxNormalExit)
	{
		/* Normal close already shut everything down cleanly; the terminate is
		 * the benign teardown exception, not a crash. Exit clean, no FATAL
		 * spam. _Exit avoids re-entering the atexit chain we're already in. */
		_Exit(0);
	}

	fprintf(stderr, "[PsyX] FATAL: std::terminate() called!\n");
	fflush(stderr);
	abort();
}

void PsyX_Initialise(char* appName, int width, int height, int fullscreen)
{
	char windowNameStr[128];

	g_appNameStr = appName;

	std::set_terminate(sh_terminate_handler);
	InstallExceptionHandler();

	PsyX_Log_Initialise();
	PsyX_GetWindowName(windowNameStr);

#if defined(_WIN32) && defined(_DEBUG)
	if (AllocConsole())
	{
		freopen("CONOUT$", "w", stdout);
		SetConsoleTitleA(windowNameStr);
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
	}
#endif

	eprintinfo("Initialising Psy-X %d.%d\n", PSYX_MAJOR_VERSION, PSYX_MINOR_VERSION);
	eprintinfo("Build date: %s:%s\n", PSYX_COMPILE_DATE, PSYX_COMPILE_TIME);

#if defined(__EMSCRIPTEN__)
	SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0");
#endif
	
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		eprinterr("Failed to initialise SDL\n");
		PsyX_Shutdown();
		return;
	}
	
	if (!GR_InitialiseRender(windowNameStr, width, height, fullscreen))
	{
		eprinterr("Failed to Intialise Window\n");
		PsyX_Shutdown();
		return;
	}

	PsyX_UpdateMouseConfinement();

	if (!PsyX_Sys_InitialiseCore())
	{
		eprinterr("Failed to Intialise Psy-X Core.\n");
		PsyX_Shutdown();
		return;
	}

	if (!GR_InitialisePSX())
	{
		eprinterr("Failed to Intialise PSX.\n");
		PsyX_Shutdown();
		return;
	}

	PsyX_Sys_InitialiseInput();

	// set shutdown function (PSX apps usualy don't exit)
	atexit(PsyX_Shutdown);

	// disable cursor visibility
	SDL_ShowCursor(0);
}

void PsyX_GetScreenSize(int* screenWidth, int* screenHeight)
{
	SDL_GetWindowSize(g_window, screenWidth, screenHeight);
}

void PsyX_SetCursorPosition(int x, int y)
{
	SDL_WarpMouseInWindow(g_window, x, y);
}

void PsyX_Sys_DoDebugKeys(int nKey, char down); // forward decl
void PsyX_Sys_DoDebugMouseMotion(int x, int y);

void PsyX_Exit();

int g_activeKeyboardControllers = 0x1;

/* Mouse wheel is event-based (a notch, not a held state), so latch each scroll
 * for a few pad reads to give the game clean press/release edges — a scroll
 * bound to a PSX button then acts as a tap. Consumed in PsyX_Pad_BuildMouseWord. */
int g_PsyX_WheelUpFrames   = 0;
int g_PsyX_WheelDownFrames = 0;
/* Net notches since the last reader cleared it: the free camera's speed wheel
 * needs every notch, which the 2-frame latch above merges. */
int g_PsyX_WheelNotches    = 0;
/* Never cleared: readers diff it against their own last sample, so no reader
 * can steal another's notches. */
int g_PsyX_WheelTotal      = 0;
/* 1 once a controller button or a deliberate stick/trigger push arrives, 0
 * again on real mouse use; the host hides its UI cursor while it is 1. */
int g_PsyX_PadInputLast    = 0;

void PsyX_Sys_DoPollEvent()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
			case SDL_CONTROLLERDEVICEADDED:
				PsyX_Pad_Event_ControllerAdded(event.cdevice.which);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				PsyX_Pad_Event_ControllerRemoved(event.cdevice.which);
				break;
			case SDL_QUIT:
				PsyX_Exit();
				break;
			case SDL_WINDOWEVENT:
				switch (event.window.event)
				{
				case SDL_WINDOWEVENT_RESIZED:
					/* This used to assign the render size directly, which is how
					 * borderless lost the chosen resolution: a fullscreen-desktop
					 * window reports the DESKTOP size here right after creation. */
					GR_ApplyPresentSize(event.window.data1, event.window.data2);
					GR_ResetDevice();
					break;
				case SDL_WINDOWEVENT_CLOSE:
					PsyX_Exit();
					break;

				/* Re-assert the clip when the window is focused again: SDL drops
				 * it on deactivation, which is exactly what lets alt-tab out. */
				case SDL_WINDOWEVENT_FOCUS_GAINED:
					PsyX_UpdateMouseConfinement();
					break;
				}
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				g_PsyX_PadInputLast = 1;
				break;
			case SDL_CONTROLLERAXISMOTION:
				/* Past the resting noise of a worn stick, so an idle pad on the
				 * desk never hides the cursor from under the mouse. */
				if (event.caxis.value > 12000 || event.caxis.value < -12000)
					g_PsyX_PadInputLast = 1;
				break;
			case SDL_MOUSEBUTTONDOWN:
				if (event.button.which != SDL_TOUCH_MOUSEID)
					g_PsyX_PadInputLast = 0;
				break;
			case SDL_MOUSEMOTION:
				/* A one-pixel nudge is the pointer settling or a warp, not the
				 * player reaching for the mouse. */
				if (event.motion.which != SDL_TOUCH_MOUSEID &&
				    (event.motion.xrel > 1 || event.motion.xrel < -1 ||
				     event.motion.yrel > 1 || event.motion.yrel < -1))
					g_PsyX_PadInputLast = 0;

				PsyX_Sys_DoDebugMouseMotion(event.motion.x, event.motion.y);
				break;
			case SDL_MOUSEWHEEL:
			{
				int wy = event.wheel.y;
				if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
					wy = -wy;
				/* Latch a scroll notch "active" for ~2 frames. Decayed once per
				 * frame in PsyX_EndScene (NOT consumed by any one reader) so BOTH
				 * the pad word AND the graphics-tuning keys (dbg_overlay) can see
				 * the same scroll without racing to consume it. */
				g_PsyX_WheelNotches += wy;
				g_PsyX_WheelTotal   += wy;
				g_PsyX_PadInputLast  = 0;
				if (wy > 0)      g_PsyX_WheelUpFrames   = 2;
				else if (wy < 0) g_PsyX_WheelDownFrames = 2;
				break;
			}
			case SDL_KEYDOWN:
			case SDL_KEYUP:
			{
				int nKey = event.key.keysym.scancode;

				if (nKey == SDL_SCANCODE_RETURN)
				{
					/* Alt+Enter toggles fullscreen. Gate it on SDL's LIVE modifier
					 * state, not a hand-tracked alt flag: that flag missed the Alt
					 * key-up whenever the window lost focus (alt-tab), stuck true,
					 * and then a plain Enter — e.g. skipping an intro — flipped the
					 * window mode and read as the game alt-tabbing out. SDL clears
					 * its modifier state on focus loss, so it can't stick. */
					if ((SDL_GetModState() & KMOD_ALT) && event.type == SDL_KEYDOWN)
					{
						int fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) > 0;

						/* The menu's path, not a bare SetWindowFullscreen: writing the
						 * window size into g_windowWidth made the render size disagree
						 * with the internal target, which stayed at the old size. */
						PsyX_ApplyWindowState(g_cfgRenderWidth, g_cfgRenderHeight, fullscreen ? 0 : 2);
					}
					break;
				}

				// lshift/right shift
				if (nKey == SDL_SCANCODE_RSHIFT)
					nKey = SDL_SCANCODE_LSHIFT;
				else if (nKey == SDL_SCANCODE_RCTRL)
					nKey = SDL_SCANCODE_LCTRL;
				else if (nKey == SDL_SCANCODE_RALT)
					nKey = SDL_SCANCODE_LALT;

				if (g_cfg_gameOnTextInput && nKey == SDL_SCANCODE_BACKSPACE && event.type == SDL_KEYDOWN)
				{
					(g_cfg_gameOnTextInput)(NULL);
				}

				PsyX_Sys_DoDebugKeys(nKey, (event.type == SDL_KEYUP) ? 0 : 1);
				break;
			}
			case SDL_TEXTINPUT:
			{
				if(g_cfg_gameOnTextInput)
					(g_cfg_gameOnTextInput)(event.text.text);
				break;
			}			
		}
	}
}

char begin_scene_flag = 0;

char PsyX_BeginScene()
{
	PsyX_Sys_DoPollEvent();

	if (begin_scene_flag)
		return 0;

	assert(!begin_scene_flag);

	{
		int swapInterval = (g_cfg_swapInterval && g_enableSwapInterval && !g_skipSwapInterval) ? g_swapInterval : 0;

		// Maximum is (ScreenRefreshRate / 2). 
		// If our screen refresh rate is lower than our PSX vmode refresh rate, 
		// we reducing swap interval to maintain the framerate.
		// Example:
		//		target 60fps, 50hz screen = no interval (tearing)
		//		target 30fps, 50hz screen = 60hz interval (less tearing)
		//		target 30fps, 60hz screen = 30hz interval (no tearing)
		SDL_DisplayMode curMode;
		if (SDL_GetWindowDisplayMode(g_window, &curMode) == 0)
		{
			const int mode_frequency = g_vmode == MODE_NTSC ? VBLANK_FREQUENCY_NTSC : VBLANK_FREQUENCY_PAL;
			/* refresh_rate == 0 means "unknown" (common for a windowed window on
			 * some drivers); do NOT treat that as a low-refresh screen or it
			 * decrements a requested vsync=1 down to 0 and vsync appears stuck off. */
			if (curMode.refresh_rate > 0 && curMode.refresh_rate < mode_frequency)
				swapInterval--;
		}

		if (swapInterval < 0)
			swapInterval = 0;
		
		GR_UpdateSwapIntervalState(swapInterval);
	}

	GR_BeginScene();

	// Always clear the backbuffer at the start of every frame. The PSX
	// behavior gates this on activeDrawEnv.isbg, but during state
	// transitions (door fades, map pickups, screen wipes) the game can
	// briefly emit prims into the OT for a frame where isbg wasn't yet
	// set — leaving stale "street sign" garbage from the prior frame
	// visible for a split second. Forcing a clear every frame is the
	// PC equivalent of glClear(GL_COLOR_BUFFER_BIT) at frame start.
	{
		const RECT16 clipenv = activeDrawEnv.clip;
		const u_char r = activeDrawEnv.isbg ? activeDrawEnv.r0 : 0;
		const u_char g = activeDrawEnv.isbg ? activeDrawEnv.g0 : 0;
		const u_char b = activeDrawEnv.isbg ? activeDrawEnv.b0 : 0;
		GR_Clear(clipenv.x, clipenv.y, clipenv.w, clipenv.h, r, g, b);
	}

	/* PC port: while the game is frozen (pause / console / map message),
	 * re-present the last captured gameplay frame so this frame's UI prims
	 * draw on top of the frozen world — PSX got this for free because it
	 * never auto-cleared the framebuffer. */
	{
		extern int g_PsxPresentLastFrame;
		extern void GR_PresentLastFrame(void);
		if (g_PsxPresentLastFrame)
			GR_PresentLastFrame();
	}

	begin_scene_flag = 1;

	PsyX_Log_Flush();

	return 1;
}

uint PsyX_CalcFPS();

/* PC port: optional hook the game registers (DbgOverlay_Render) to draw the dev console
 * AFTER the freeze-frame is captured, so the console is never baked into a frozen
 * pause / "no map" image (which would ghost the live console against the frozen copy).
 * NULL = nothing extra drawn. */
extern "C" void (*g_PsyX_PostCaptureHook)(void) = NULL;

void PsyX_EndScene()
{
	if (!begin_scene_flag)
		return;

	assert(begin_scene_flag);
	begin_scene_flag = 0;

	/* Decay the mouse-wheel latch once per frame (set in PsyX_Sys_DoPollEvent,
	 * read by the pad word + the graphics-tuning keys). */
	if (g_PsyX_WheelUpFrames   > 0) g_PsyX_WheelUpFrames--;
	if (g_PsyX_WheelDownFrames > 0) g_PsyX_WheelDownFrames--;

	PGXP_CoverageTick();

	GR_EndScene();

#ifndef PSYX_SKIP_FRAMEBUFFER_STORE
	/* The naive whole-display-rect store. Disabled for Silent Hill: the PC libgs
	 * stub reports disp as (0,0) so this lands on the CLUT strip at y<32, and it
	 * reaches VRAM by raw blit, which cannot produce the packed RG8 the sampler
	 * decodes. GR_StoreFrameBufferPsx below is the working replacement. */
	GR_StoreFrameBuffer(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
#endif

	/* PC port: pack the composed frame into the PSX display-buffer pages so the
	 * game's framebuffer-feedback effects can read the previous frame back —
	 * the Harry-running loading-screen motion blur and the per-map dream
	 * overlays. No-op until GsDefDispBuff2 reports the buffer origins, and
	 * honours the g_PsxSkipFramebufferStore per-frame opt-out. */
	GR_StoreFrameBufferPsx();

	/* PC port: g_PsxSkipFramebufferStore is a per-frame opt-out — the game must
	 * re-set it each tick during a TIM-protect screen (e.g. paper-map pickup). */
	g_PsxSkipFramebufferStore = 0;

	/* PC port: keep a copy of every composed frame for freeze-frame
	 * presentation (skipped internally on frames that re-presented it). */
	{
		extern void GR_CaptureLastFrame(void);
		GR_CaptureLastFrame();
		GR_VoidProbeScene();
	}

	/* PC port: draw overlays that must NOT be baked into the freeze-frame (the dev
	 * console) — AFTER the capture, BEFORE the swap, so the console is a true live
	 * overlay and never doubles against a frozen copy on pause / "no map" screens. */
	/* Native resolution draws these after the present instead, at window size
	 * (GR_SwapWindow). */
	{
		extern int          g_PcNativeRender;
		extern unsigned int g_internalFBO;

		if (g_PsyX_PostCaptureHook && !(g_PcNativeRender && g_internalFBO != 0))
			g_PsyX_PostCaptureHook();
	}

	/* PC port: apply the selected full-screen post-process look (color grade,
	 * CRT, scanlines, vignette, grain, sharpen, PSX downsample, ...) to the
	 * fully composed frame just before presenting. No-op when off. */
	{
		extern void GR_PostProcess(void);
		GR_PostProcess();
	}

	GR_SwapWindow();
}

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
void PsyX_TakeScreenshot()
{
	u_char* pixels = (u_char*)malloc(g_windowWidth * g_windowHeight * 4);
	
#if defined(RENDERER_OGL)
	glReadPixels(0, 0, g_windowWidth, g_windowHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
#elif defined(RENDERER_OGLES)
	glReadPixels(0, 0, g_windowWidth, g_windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);	// FIXME: is that correct format?
#endif

	SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(pixels, g_windowWidth, g_windowHeight, 8 * 4, g_windowWidth * 4, 0, 0, 0, 0);

	SDL_SaveBMP(surface, "SCREENSHOT.BMP");
	SDL_FreeSurface(surface);

	free(pixels);
}
#endif

void PsyX_Sys_DoDebugMouseMotion(int x, int y)
{
	if (g_dbg_gameDebugMouse)
		g_dbg_gameDebugMouse(x, y);
}

void PsyX_Sys_DoDebugKeys(int nKey, char down)
{
	if (g_dbg_gameDebugKeys)
		g_dbg_gameDebugKeys(nKey, down);

	/* Fast-forward, held on CTRL+F5. This was on bare Backspace once and was
	 * removed for being trivially hit by accident, which silently ran the game
	 * at uncapped speed. Behind Ctrl it cannot be tripped by a stray press, and
	 * it clears on key-up so it can never latch on. */
	if (nKey == SDL_SCANCODE_F5)
	{
		/* TOGGLE, matching the quick options row -- press once for fast
		 * forward, again for normal. s_f5Down filters SDL's key repeats,
		 * which would otherwise flip it many times per second while held.
		 * g_PcFastForward is the single source of truth; the game mirrors
		 * it into g_skipSwapInterval so pacing and the clock agree. */
		static int s_f5Down = 0;
		if (down)
		{
			if (!s_f5Down && (SDL_GetModState() & KMOD_CTRL))
			{
				extern int g_PcFastForward;
				g_PcFastForward = !g_PcFastForward;
			}
			s_f5Down = 1;
		}
		else
		{
			s_f5Down = 0;
		}
		return;
	}

	if (!down)
	{
		/* EVERY bind below requires CTRL.
		 *
		 * These share the keyboard with the game and with the launcher's own
		 * bindings, and an unmodified function key is far too easy to hit by
		 * accident: F10 alone collided with the quick options overlay and made
		 * every open and close dump the whole VRAM to disk (a 2+ second freeze),
		 * and the old bare F5/F6 flipped PGXP settings silently, with nothing on
		 * screen and nothing in the log to say why the picture changed.
		 *
		 * The PGXP toggles are gone entirely rather than gated: they did nothing
		 * visible in game, and F6 is the quick-save key. */
		if (!(SDL_GetModState() & KMOD_CTRL))
			return;

		switch (nKey)
		{
		/* Shipped in release builds, not just _DEBUG: with Ctrl required these
		 * are safe to expose, and they are genuinely useful to players poking at
		 * the renderer. */
		case SDL_SCANCODE_F1:
			g_dbg_wireframeMode ^= 1;
			eprintwarn("wireframe mode: %d\n", g_dbg_wireframeMode);
			break;

		case SDL_SCANCODE_F2:
			g_dbg_texturelessMode ^= 1;
			eprintwarn("textureless mode: %d\n", g_dbg_texturelessMode);
			break;
#ifdef _DEBUG
		/* Debug-only: inert unless g_dbg_emulatorPaused is set, and nothing
		 * binds that, so shipping it would only risk the arrow keys. */
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_DOWN:
			if (g_dbg_emulatorPaused)
			{
				g_dbg_polygonSelected += (nKey == SDL_SCANCODE_UP) ? 3 : -3;
			}
			break;
#endif
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
		case SDL_SCANCODE_F10:
			/* The key diagnostic for texture/VRAM bugs (boss-FX ghost textures
			 * and friends). Writes VRAM.TGA to the cwd. */
			eprintwarn("saving VRAM.TGA\n");
			GR_SaveVRAM("VRAM.TGA", 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 1);
			break;
		case SDL_SCANCODE_F12:
			eprintwarn("Saving screenshot...\n");
			PsyX_TakeScreenshot();
			break;
#endif
		/* F3 is the game-side tone-map cycle (dbg_overlay.c). F4 was a keyboard
		 * controller-slot cycle: a stray tap moved keyboard + mouse off player 1
		 * and silently killed fire/aim, so it stays removed. */
		}
	}
}

void PsyX_UpdateInput()
{
	// also poll events here
	PsyX_Sys_DoPollEvent();

	/* Re-assert cursor hidden every frame. SDL_ShowCursor(0) at init isn't
	 * sticky on Windows: alt-tabbing back into fullscreen, or a window-mode
	 * transition triggered by SDL itself, can flip cursor visibility back
	 * on. The user kept seeing the OS arrow pinned at screen center every
	 * gameplay screenshot. SDL_ShowCursor with the same value is a no-op
	 * (just queries internal state), so calling it per-frame is free. */
	if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
		SDL_ShowCursor(SDL_DISABLE);

	/* Skip pad polling while Alt is held (Alt+Tab / Alt+Enter) — live SDL modifier
	 * state, so it can't stick true and freeze the pad after a focus change. */
	if (!(SDL_GetModState() & KMOD_ALT))
		PsyX_Pad_InternalPadUpdates();
}

uint PsyX_CalcFPS()
{
#define FPS_INTERVAL 1.0

	static unsigned int lastTime = 0;
	static unsigned int currentFps = 0;
	static unsigned int passedFrames = 0;

	lastTime = SDL_GetTicks();

	passedFrames++;
	if (lastTime < SDL_GetTicks() - FPS_INTERVAL * 1000)
	{
		lastTime = SDL_GetTicks();
		currentFps = passedFrames;
		passedFrames = 0;
	}

	return currentFps;
}

void PsyX_SetSwapInterval(int interval)
{
	g_swapInterval = interval;
}

void PsyX_EnableSwapInterval(int enable)
{
	g_enableSwapInterval = enable;
}

void PsyX_ApplyVsync(int vsync)
{
	/* Drive vsync through the per-frame swap-interval path (PsyX_BeginScene): a
	 * direct SDL_GL_SetSwapInterval is overwritten every frame from
	 * g_cfg_swapInterval, so toggling that gate is what actually sticks. */
	g_cfg_swapInterval = (vsync != 0) ? 1 : 0;
	g_swapInterval = 1;
}

void PsyX_ApplyWindowState(int width, int height, int fullscreen)
{
	if (!g_window)
		return;

	/* Always drop to windowed first: SDL will not switch DIRECTLY between two
	 * fullscreen states (exclusive<->desktop) or re-apply a new exclusive mode
	 * while already fullscreen, so a naked SetWindowFullscreen from the menu
	 * silently no-ops. Clearing the flag first makes every transition apply. */
	SDL_SetWindowFullscreen(g_window, 0);

	if (fullscreen == 1) /* exclusive fullscreen at the requested resolution */
	{
		SDL_DisplayMode want, got;
		SDL_zero(want);
		want.w = width;
		want.h = height;
		int disp = SDL_GetWindowDisplayIndex(g_window);
		if (disp < 0)
			disp = 0;
		if (SDL_GetClosestDisplayMode(disp, &want, &got) != NULL)
			SDL_SetWindowDisplayMode(g_window, &got);
		SDL_SetWindowSize(g_window, width, height);
		SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN);
	}
	else if (fullscreen == 2) /* borderless: desktop-sized window, scene rendered at the chosen size */
	{
		SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	else /* windowed */
	{
		SDL_SetWindowSize(g_window, width, height);
		SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	}

	SDL_GetWindowSize(g_window, &g_windowWidth, &g_windowHeight);

	/* The window is now the PRESENT size. Borderless used to stop here, which is
	 * why it ignored the chosen resolution and simply ran at desktop size.
	 *
	 * Instead, keep the desktop-sized window and render the scene into an
	 * internal target at the chosen resolution, stretched to the window at
	 * present. g_windowWidth/Height stay the RENDER size, so every viewport,
	 * scissor and aspect calculation keeps working untouched.
	 *
	 * Skipped when the request matches the desktop (nothing to gain) or when
	 * MSAA is on: the multisampling lives on the window's own buffer, so
	 * redirecting the scene into a single-sampled target would silently throw
	 * antialiasing away. If the target cannot be allocated the helper returns 0
	 * and the stock desktop-resolution path runs exactly as before. */
	g_cfgRenderWidth    = width;
	g_cfgRenderHeight   = height;
	g_cfgFullscreenMode = fullscreen;
	GR_ApplyPresentSize(g_windowWidth, g_windowHeight);

	GR_ResetDevice();
	PsyX_UpdateMouseConfinement();
}

void PsyX_WaitForTimestep(int count)
{
#if 0 // defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	glFinish(); // best time to complete GPU drawing
#endif

	// wait for vblank
	if (!g_skipSwapInterval)
	{	
		static int swapLastVbl = 0;

		int vbl;
		do
		{
#ifdef __EMSCRIPTEN__
			emscripten_sleep(0);
#endif
			IntrCpuRelax();
			vbl = PsyX_Sys_GetVBlankCount();
		}
		while (vbl - swapLastVbl < count);

		swapLastVbl = PsyX_Sys_GetVBlankCount();
	}
}

void PsyX_Exit()
{
	g_psyxNormalExit = 1;
	fprintf(stderr, "[PsyX] PsyX_Exit() called — normal game exit.\n");
	fflush(stderr);
	exit(0);
}

/* Stage markers: every normal close currently ends in std::terminate (an
 * exception escaping somewhere below, under exit()/atexit). stderr is
 * unbuffered, so the last marker printed before the FATAL line in the log
 * names the throwing step. Remove once the thrower is found and fixed. */
#define SHUTDOWN_STAGE(name) do { fprintf(stderr, "[PsyX] PsyX_Shutdown: %s\n", name); fflush(stderr); } while (0)

void PsyX_Shutdown()
{
	fprintf(stderr, "[PsyX] PsyX_Shutdown() called (g_window=%p).\n", (void*)g_window);
	fflush(stderr);
	/* This is the intentional teardown path (MainLoop returned, incl. Esc warm-
	 * reboot which doesn't go through PsyX_Exit). Mark the exit normal so the
	 * benign teardown exception takes the clean _Exit(0) branch instead of the
	 * FATAL+abort that scared users at the end of ending-map sessions. */
	g_psyxNormalExit = 1;
	if (!g_window)
		return;

	// quit vblank thread
	if (g_intrThread)
	{
		g_stopIntrThread = 1;

		int returnValue;
		SDL_WaitThread(g_intrThread, &returnValue);

		SDL_DestroyMutex(g_intrMutex);
	}
	SHUTDOWN_STAGE("vblank thread joined");

	SDL_DestroyWindow(g_window);
	g_window = NULL;
	SHUTDOWN_STAGE("window destroyed");

	GR_Shutdown();
	SHUTDOWN_STAGE("GR_Shutdown done");

	SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

	SDL_Quit();
	SHUTDOWN_STAGE("SDL_Quit done");

	UnInstallExceptionHandler();
	SHUTDOWN_STAGE("exception handler uninstalled");

	PsyX_Log_Finalise();
}
