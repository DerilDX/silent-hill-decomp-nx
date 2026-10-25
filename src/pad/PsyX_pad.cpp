#include "psx/libpad.h"
#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "PsyX_pad.h"
#include "PsyX/PsyX_public.h"

#include <string.h>
#include <stdlib.h> /* abs */

extern "C"
{
extern int g_padCommEnable;
}

typedef struct
{
	Sint32				deviceId;	// linked device INDEX (from config pin or first open)
	SDL_JoystickID		instanceId;	// joystick INSTANCE id while open, -1 when closed --
									// the currency REMOVED events speak in
	SDL_GameController* gc;

	u_char*				padData;
	bool				switchingAnalog;
	u_short				hystWord[2]; /* PC: per-mapping Schmitt-trigger latch (analog->digital anti-chatter) */
} PsyXController;

int						g_cfg_controllerToSlotMapping[MAX_CONTROLLERS] = { -1, -1 };

/* PC port: movement source for the controller. 0 = analog stick only,
 * 1 = d-pad only (digital), 2 = both (default). Set from config in main_pc.c.
 * Drives whether the emulated pad sits in analog (0x73) or digital (0x41) mode. */
int						g_cfg_controllerMovement = 2;
int						g_cfg_disableDpadMovement = 0; /* 1 = controller D-pad no longer drives movement (freed for action binds); keyboard arrows unaffected */

PsyXController			g_controllers[MAX_CONTROLLERS];

/* PSX PadSetAct semantics: the game registers a LIVE actuator buffer once
 * and the pad driver transmits its current bytes to the controller every
 * vsync; the game then just mutates the bytes in place (Silent Hill's
 * vibration engine repacks them per frame in func_8009E718). A fire-once
 * PadSetAct loses every later value change, so register here and
 * retransmit from PsyX_Pad_InternalPadUpdates. */
static unsigned char*	g_actBufTable[MAX_CONTROLLERS];
static int				g_actBufLen[MAX_CONTROLLERS];
const u_char*			g_sdlKeyboardState = NULL;

void    PsyX_Pad_CloseController(int slot);
u_short PsyX_Pad_UpdateKeyboardInput();
void	PsyX_Pad_UpdateGameControllerInput(PsyXController* controller, LPPADRAW pad);
static int PsyX_Pad_MergeAllControllers(LPPADRAW pad);
static void PsyX_Pad_AdoptDevice(Sint32 deviceId);

/* The controller rumble follows: slot 0 by default, then whichever pad was last
 * actually used (button or stick). Rumble goes ONLY to this one, so a second
 * idle controller never buzzes. */
static int g_activeControllerSlot = 0;

/* The Steam Controller can be plugged in and still invisible to SDL: when the
 * Steam client is running it holds the device for its own desktop
 * configuration, so SDL's raw driver cannot open it and the pad behaves as a
 * mouse. Nothing in this process can take it back. What we can do is notice
 * the hardware is there while no joystick came of it, and say why, because the
 * report that arrives is "the controller does nothing" with no hint that Steam
 * was open in the background. 0x28DE is Valve; 0x1102 the wired controller,
 * 0x1142 the wireless dongle (which enumerates even with no pad paired, so the
 * message says "detected", not "connected"). */
static void PsyX_Pad_SteamControllerOwnershipHint(void)
{
	SDL_hid_device_info* list = SDL_hid_enumerate(0x28DE, 0);
	SDL_hid_device_info* it;
	int steamHw = 0, steamJoy = 0, i;

	for (it = list; it != NULL; it = it->next)
		if (it->product_id == 0x1102 || it->product_id == 0x1142)
			steamHw = 1;
	SDL_hid_free_enumeration(list);
	if (!steamHw)
		return;

	for (i = 0; i < SDL_NumJoysticks(); i++)
		if (SDL_JoystickGetDeviceVendor(i) == 0x28DE)
			steamJoy = 1;

	if (steamJoy)
		eprintf("[PAD] Steam Controller: hardware detected and opened by SDL\n");
	else
		eprintf("[PAD] Steam Controller: hardware detected but SDL could not open it. "
		        "The Steam client is holding it (desktop configuration = mouse). "
		        "Add the game to Steam and launch it from there, or exit Steam first.\n");
}

/* A joystick SDL has no mapping for is not a game controller at all:
 * SDL_IsGameController says no, PsyX_Pad_OpenController never sees it, and the
 * pad does nothing. It used to be invisible in the log as well, so the only
 * report that ever arrived was "my controller doesn't work".
 *
 * Name it instead, with the GUID and the raw shape a mapping line is written
 * from, so an unknown pad is one log line to diagnose. */
static void PsyX_Pad_ReportUnmappedJoysticks()
{
	for (int i = 0; i < SDL_NumJoysticks(); i++)
	{
		const char*   name;
		SDL_Joystick* js;
		char          guid[64];

		if (SDL_IsGameController(i))
			continue;

		name = SDL_JoystickNameForIndex(i);
		SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));

		/* Nothing else will ever open this device, so borrowing it for its
		 * button/axis counts cannot take it from anyone. */
		js = SDL_JoystickOpen(i);
		eprintwarn("[PAD] '%s' (vid=%04x pid=%04x guid=%s) has no SDL mapping, so the game "
		           "cannot use it. Raw shape: buttons=%d axes=%d hats=%d. Add a line for that "
		           "guid to gamecontrollerdb.txt next to the exe; SDL's controllermap tool "
		           "writes one.\n",
		           name ? name : "?",
		           (unsigned)SDL_JoystickGetDeviceVendor(i),
		           (unsigned)SDL_JoystickGetDeviceProduct(i), guid,
		           js ? SDL_JoystickNumButtons(js) : -1,
		           js ? SDL_JoystickNumAxes(js) : -1,
		           js ? SDL_JoystickNumHats(js) : -1);
		if (js)
			SDL_JoystickClose(js);
	}
}

// Initializes SDL controllers
int PsyX_Pad_InitSystem()
{
	// do not init second time!
	if (g_sdlKeyboardState != NULL)
		return 1;

	// DualSense needs the hidapi backend. Both hints already default to "1" on
	// every SDL2 that ships a PS5 driver (>= 2.0.14), so this is an explicit pin
	// rather than a fix — but it MUST stay above the SDL_InitSubSystem below,
	// because SDL latches joystick hints when the subsystem starts. An
	// SDL_JOYSTICK_HIDAPI=0 environment variable still wins (NORMAL priority),
	// which is what leaves Steam Input's overrides working.
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");

	/* Steam Controller and Steam Deck. SDL ships hidapi drivers for both, but
	 * the Steam Controller one defaults OFF on desktop because it fights the
	 * Steam client: when Steam launched us, Steam Input already presents the
	 * pad as a virtual XInput device and also holds the raw HID handle, so a
	 * second reader gets nothing or doubles every press. Outside Steam nothing
	 * else will ever open the device, and without the driver a Steam Controller
	 * is not a game controller to SDL at all -- SDL_IsGameController says no
	 * and PsyX_Pad_OpenController never sees it.
	 *
	 * So: raw driver on only when Steam did not launch us. Steam sets SteamAppId
	 * and SteamGameId for every title it starts, non-Steam shortcuts included,
	 * and Proton adds STEAM_COMPAT_APP_ID. The Deck hint is passed by name so
	 * an SDL older than the one that added it just ignores it. */
	{
		const int underSteam = (SDL_getenv("SteamAppId") != NULL ||
		                        SDL_getenv("SteamGameId") != NULL ||
		                        SDL_getenv("STEAM_COMPAT_APP_ID") != NULL);
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, underSteam ? "0" : "1");
		SDL_SetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "1");
		eprintf("[PAD] Steam Controller raw driver %s (%s)\n",
		        underSteam ? "off" : "on",
		        underSteam ? "launched by Steam, Steam Input owns the pad" : "not launched by Steam");
	}

	memset(g_controllers, 0, sizeof(g_controllers));
	for (int i = 0; i < MAX_CONTROLLERS; i++)
		g_controllers[i].instanceId = -1; /* memset zero is a VALID instance id */

	// init keyboard state
	g_sdlKeyboardState = SDL_GetKeyboardState(NULL);

	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
	{
		eprinterr("Failed to initialise SDL GameController subsystem!\n");
		return 0;
	}

	/* The community controller database, shipped beside the exe from
	 * pc_port/assets/gamecontrollerdb.txt. SDL's built-in list covers the big
	 * brands and little else, and a pad it has no mapping for is invisible to
	 * the game (see PsyX_Pad_ReportUnmappedJoysticks above) -- a PlayStation
	 * Classic controller, 054c:0cda, is exactly that: ten buttons and a d-pad
	 * on two axes, which SDL does not recognise on its own.
	 *
	 * Reported rather than ignored, because this call silently did nothing for
	 * as long as the file was simply missing from the build. */
	{
		const int added = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");

		if (added < 0)
			eprintwarn("[PAD] gamecontrollerdb.txt not loaded (%s). Only SDL's built-in "
			           "mappings are available, so an uncommon pad may not work.\n",
			           SDL_GetError());
		else
			eprintf("[PAD] gamecontrollerdb.txt: %d mappings added\n", added);
	}

	PsyX_Pad_SteamControllerOwnershipHint();
	PsyX_Pad_ReportUnmappedJoysticks();

	/* Open what is ALREADY plugged in.
	 *
	 * SDL enumerates devices inside the SDL_InitSubSystem above and posts
	 * CONTROLLERDEVICEADDED only for the ones it can map AT THAT MOMENT --
	 * which is before the database above has been read. It never re-announces
	 * a device that becomes mappable later. So a pad whose mapping comes from
	 * the file got no event at all, and since that event is the only thing
	 * that opens a controller, the pad stayed dead until the player unplugged
	 * and replugged it, which produced a fresh ADDED. Measured with SDL 2.28.0
	 * and a PlayStation Classic controller: JOYDEVICEADDED at init, and zero
	 * CONTROLLERDEVICEADDED either at init or after the mappings load.
	 *
	 * Reading the database before SDL_InitSubSystem would be the other fix,
	 * but the mapping calls are only documented as usable after init, and this
	 * needs no assumption about that. It also covers an ADDED event that goes
	 * missing before the event loop starts. */
	for (int i = 0; i < SDL_NumJoysticks(); i++)
	{
		if (SDL_IsGameController(i))
			PsyX_Pad_AdoptDevice(i);
	}

	return 1;
}

// Prints controller list into console
void PsyX_Pad_Debug_ListControllers()
{
	PsyX_Pad_SteamControllerOwnershipHint();

	int numJoysticks = SDL_NumJoysticks();
	int numHaptics = SDL_NumHaptics();

	int numMapped = 0;

	for (int i = 0; i < numJoysticks; i++)
	{
		if (SDL_IsGameController(i))
			numMapped++;
	}

	/* Counted first: joysticks SDL cannot map print nothing here, so the old
	 * test on numJoysticks announced a list and then showed an empty one. */
	if (numMapped)
	{
		eprintf("SDL GameController list:\n");

		for (int i = 0; i < numJoysticks; i++)
		{
			if (SDL_IsGameController(i))
			{
				eprintinfo("  %d '%s'\n", i, SDL_GameControllerNameForIndex(i));
			}
		}
	}
	else
		eprintwarn("No SDL GameControllers found!\n");

	PsyX_Pad_ReportUnmappedJoysticks();

	if (numHaptics)
	{
		eprintf("SDL haptic list:\n");

		for (int i = 0; i < numHaptics; i++)
		{
			eprintinfo("  %d '%s'\n", i, SDL_HapticName(i));
		}
	}
	else
		eprintwarn("No SDL haptics found!\n");
}

// Opens specific system controller and assigns to specified slot
void PsyX_Pad_OpenController(Sint32 deviceId, int slot)
{
	PsyXController* controller = &g_controllers[slot];

	if (controller->gc)
	{
		/* A live handle: nothing to do. A STALE one (device yanked, REMOVED
		 * missed or mismatched) must not block the slot forever -- that was
		 * the "restart the game to get the pad back". */
		if (SDL_GameControllerGetAttached(controller->gc))
			return;
		PsyX_Pad_CloseController(slot);
	}

	controller->gc = SDL_GameControllerOpen(deviceId);
	controller->switchingAnalog = false;
	controller->instanceId = -1;

	if (controller->gc)
	{
		controller->instanceId =
			SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller->gc));

		// assign device id automatically
		if (controller->deviceId == -1)
			controller->deviceId = deviceId;

		eprintinfo("Controller '%s' -> slot %d (instance %d)\n",
			SDL_GameControllerName(controller->gc), slot, (int)controller->instanceId);

		/* [PADPROBE] what SDL actually made of the device: a pad can be listed
		 * and opened yet deliver nothing (a driver or Steam holding it) or
		 * deliver on indices its mapping does not name. */
		{
			SDL_Joystick* js = SDL_GameControllerGetJoystick(controller->gc);
			char          guid[64];
			char*         map = SDL_GameControllerMapping(controller->gc);

			SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof(guid));
			eprintinfo("[PADPROBE] slot %d vid=%04x pid=%04x type=%d buttons=%d axes=%d hats=%d guid=%s\n",
				slot, (unsigned)SDL_JoystickGetVendor(js), (unsigned)SDL_JoystickGetProduct(js),
				(int)SDL_GameControllerGetType(controller->gc),
				SDL_JoystickNumButtons(js), SDL_JoystickNumAxes(js), SDL_JoystickNumHats(js), guid);
			eprintinfo("[PADPROBE] slot %d mapping: %s\n", slot, map ? map : "(none)");
			if (map)
				SDL_free(map);
		}
	}
}

/* [PADPROBE] Reports the first raw button / axis / hat changes on each open
 * pad -- the joystick level, beneath the controller mapping -- so a pad that
 * "does nothing" says whether any data arrives at all. Capped. */
static void PsyX_Pad_ProbeRawInput(int slot, SDL_GameController* gc)
{
	static Sint16 s_axis[MAX_CONTROLLERS][16];
	static Uint8  s_btn[MAX_CONTROLLERS][32];
	static Uint8  s_hat[MAX_CONTROLLERS][4];
	static int    s_init[MAX_CONTROLLERS];
	static int    s_logs = 0;
	SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
	int           nb, na, nh, k;

	if (!js || s_logs >= 16)
		return;

	nb = SDL_JoystickNumButtons(js); if (nb > 32) nb = 32;
	na = SDL_JoystickNumAxes(js);    if (na > 16) na = 16;
	nh = SDL_JoystickNumHats(js);    if (nh > 4)  nh = 4;

	for (k = 0; k < nb; k++)
	{
		const Uint8 v = SDL_JoystickGetButton(js, k);
		if (s_init[slot] && v != s_btn[slot][k] && s_logs < 16)
		{
			s_logs++;
			eprintinfo("[PADPROBE] slot %d raw button %d = %d\n", slot, k, (int)v);
		}
		s_btn[slot][k] = v;
	}
	for (k = 0; k < na; k++)
	{
		const Sint16 v = SDL_JoystickGetAxis(js, k);
		if (s_init[slot] && abs((int)v - (int)s_axis[slot][k]) > 12000 && s_logs < 16)
		{
			s_logs++;
			eprintinfo("[PADPROBE] slot %d raw axis %d = %d\n", slot, k, (int)v);
		}
		if (!s_init[slot] || abs((int)v - (int)s_axis[slot][k]) > 12000)
			s_axis[slot][k] = v;
	}
	for (k = 0; k < nh; k++)
	{
		const Uint8 v = SDL_JoystickGetHat(js, k);
		if (s_init[slot] && v != s_hat[slot][k] && s_logs < 16)
		{
			s_logs++;
			eprintinfo("[PADPROBE] slot %d raw hat %d = 0x%x\n", slot, k, (unsigned)v);
		}
		s_hat[slot][k] = v;
	}
	s_init[slot] = 1;
}

// Closes controller in specific slot
void PsyX_Pad_CloseController(int slot)
{
	PsyXController* controller = &g_controllers[slot];

	if (controller->gc)
		SDL_GameControllerClose(controller->gc);

	controller->gc = NULL;
	controller->instanceId = -1;
}

// Called from LIBPAD
void PsyX_Pad_InitPad(int slot, u_char* padData)
{
	PsyXController* controller = &g_controllers[slot];

	controller->padData = padData;
	controller->deviceId = g_cfg_controllerToSlotMapping[slot];

	if (padData)
	{
		LPPADRAW pad = (LPPADRAW)padData;
		
		bool wasConnected = (pad->id == 0x41 || pad->id == 0x73);

		if(!wasConnected)
			pad->id = slot == 0 ? 0x41 : 0xFF;	// since keyboard is a main controller - it's always on

		// only reset buttons
		pad->buttons[0] = 0xFF;
		pad->buttons[1] = 0xFF;
		pad->analog[0] = 128;
		pad->analog[1] = 128;
		pad->analog[2] = 128;
		pad->analog[3] = 128;
	}
}

/* Give a device index to the first slot that will take it. Shared by the ADDED
 * event and the startup scan in PsyX_Pad_InitSystem. */
static void PsyX_Pad_AdoptDevice(Sint32 deviceId)
{
	const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(deviceId);
	int i;

	/* Already ours. An ADDED event can arrive for a device the startup scan
	 * has just taken, and without this the same physical pad would be opened a
	 * second time into the next free slot and sit there as player 2. */
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		if (g_controllers[i].gc && g_controllers[i].instanceId == instance)
			return;
	}

	/* Config-pinned slots accept exactly their device index; unpinned slots
	 * accept any newcomer. Matching on controller->deviceId is wrong here: it
	 * auto-latches the index of the FIRST device, so a pad replugged into a
	 * different port (new index) could never rejoin its slot. */
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		PsyXController* controller = &g_controllers[i];

		if (controller->gc)
			continue;
		if (g_cfg_controllerToSlotMapping[i] != -1 &&
		    g_cfg_controllerToSlotMapping[i] != deviceId)
			continue;

		PsyX_Pad_OpenController(deviceId, i);
		break;
	}
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerAdded(Sint32 deviceId)
{
	/* `which` on an ADDED event is a DEVICE INDEX; see Removed for the id
	 * mismatch that used to strand pads. The old haptic subsystem reinit is
	 * gone: rumble runs through SDL_GameControllerRumble, no SDL_Haptic
	 * handles exist, and quitting the subsystem mid-event was the crash its
	 * own FIXME warned about. */
	int i;

	PsyX_Pad_Debug_ListControllers();

	/* Free any slot whose handle went stale (missed REMOVED, USB re-enumeration). */
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		if (g_controllers[i].gc && !SDL_GameControllerGetAttached(g_controllers[i].gc))
			PsyX_Pad_CloseController(i);
	}

	PsyX_Pad_AdoptDevice(deviceId);
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerRemoved(Sint32 instanceId)
{
	/* `which` on a REMOVED event is the joystick INSTANCE ID, not the device
	 * index ADDED carries. The old code compared it to the stored device
	 * index: the first unplug worked only while index 0 happened to meet
	 * instance 0, the replugged pad came back as instance 1, and from then on
	 * removals matched nothing -- the slot kept a dead handle forever, the
	 * ADDED handler saw the slot as occupied, and only a restart recovered
	 * the pad. */
	int i;

	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		PsyXController* controller = &g_controllers[i];

		if (controller->gc && controller->instanceId == (SDL_JoystickID)instanceId)
		{
			eprintinfo("Controller in slot %d disconnected\n", i);
			PsyX_Pad_CloseController(i);
		}
	}

	PsyX_Pad_Debug_ListControllers();
}

void PsyX_Pad_InternalPadUpdates()
{
	PsyXController* controller;
	LPPADRAW pad;
	u_short kbInputs;

	if (g_padCommEnable == 0)
		return;

	kbInputs = PsyX_Pad_UpdateKeyboardInput();

	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->padData)
		{
			int anyAttached;
			pad = (LPPADRAW)controller->padData;

			if (i == 0)
			{
				// Player 1: any connected controller drives it (single-player).
				anyAttached = PsyX_Pad_MergeAllControllers(pad);

				// P1 rumble goes to the ACTIVE pad only (last one used), so an
				// idle second controller never buzzes.
				if (g_actBufTable[i] && g_actBufLen[i] > 0)
				{
					int a = g_activeControllerSlot;
					if (a < 0 || a >= MAX_CONTROLLERS ||
					    !g_controllers[a].gc || !SDL_GameControllerGetAttached(g_controllers[a].gc))
						a = 0;
					if (g_controllers[a].gc && SDL_GameControllerGetAttached(g_controllers[a].gc))
						PsyX_Pad_Vibrate(0, a, g_actBufTable[i], g_actBufLen[i]);
				}
			}
			else
			{
				PsyX_Pad_UpdateGameControllerInput(controller, pad);

				// Retransmit the registered actuator buffer (PSX pad driver
				// behavior) so in-place value changes by the game reach SDL.
				if (g_actBufTable[i] && g_actBufLen[i] > 0 && controller->gc)
					PsyX_Pad_Vibrate(0, i, g_actBufTable[i], g_actBufLen[i]);

				anyAttached = (controller->gc && SDL_GameControllerGetAttached(controller->gc));
			}

			// PC port: analog mode is config-driven (controller_movement) rather
			// than the original Select+Start manual toggle. analog/both -> 0x73
			// (left stick active), dpad -> 0x41 (digital, stick ignored). Only
			// when a real controller is attached; keyboard stays digital below.
			if (anyAttached)
			{
				pad->id = (g_cfg_controllerMovement == 1) ? 0x41 : 0x73;
			}

			// Update keyboard for PAD
			if ((g_activeKeyboardControllers & (1 << i)) && kbInputs != 0xffff)
			{
				pad->status = 0;	// PadStateStable?

				if (pad->id != 0x41)
				{
					if(pad->id != 0x73)
						eprintf("Port %d ANALOG: OFF\n", i + 1);

					pad->id = 0x41; // force disable analog
				}

				*(u_short*)pad->buttons &= kbInputs;
			}
		}
	}

#if defined(__ANDROID__)
	///@TODO SDL_NumJoysticks always reports > 0 for some reason on Android.
#endif
}


int GetControllerButtonState(SDL_GameController* cont, int buttonOrAxis); /* defined below */

extern "C" int PsyX_Pad_SkipButtonHeld(void)
{
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (!gc)
			continue;

		/* Route the FMV/blocking-loop "skip" through the configured Action/Start
		 * binds (primary + alternate) instead of hardcoded A/Start, so a rebound
		 * controller still skips. */
		if (GetControllerButtonState(gc, g_cfg_controllerMapping.gc_cross)  > 16384 ||
		    GetControllerButtonState(gc, g_cfg_controllerMapping2.gc_cross) > 16384 ||
		    GetControllerButtonState(gc, g_cfg_controllerMapping.gc_start)  > 16384)
			return 1;
	}

	return 0;
}

int GetControllerButtonState(SDL_GameController* cont, int buttonOrAxis)
{
	if (buttonOrAxis & CONTROLLER_MAP_FLAG_AXIS)
	{
		int value = SDL_GameControllerGetAxis(cont, (SDL_GameControllerAxis)(buttonOrAxis & ~(CONTROLLER_MAP_FLAG_AXIS | CONTROLLER_MAP_FLAG_INVERSE)));

		if (abs(value) > 500 && (buttonOrAxis & CONTROLLER_MAP_FLAG_INVERSE))
			value *= -1;

		return value;
	}

	return SDL_GameControllerGetButton(cont, (SDL_GameControllerButton)buttonOrAxis) * 32767;
}

/* PC port: is an SDL game-controller button held on ANY attached physical
 * controller? Read straight from SDL, NOT the keyboard-merged PSX pad word, so a
 * keyboard key mapped to the same PSX button cannot trigger a controller-only
 * action (e.g. the Change-Camera pad bind). sdlGameControllerButton < 0 = unbound. */
extern "C" int PsyX_RawControllerButtonHeld(int sdlGameControllerButton)
{
	int i;
	if (sdlGameControllerButton < 0)
		return 0;
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (gc && SDL_GameControllerGetAttached(gc) &&
		    SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)sdlGameControllerButton))
			return 1;
	}
	return 0;
}

/* PC port: as above, but accepts a bind encoded by PsyX_LookupGameControllerMapping
 * — i.e. a digital button OR an axis (CONTROLLER_MAP_FLAG_AXIS), which is how
 * "lefttrigger"/"righttrigger" are represented. The PSX-button binds always went
 * through that encoding (pad_cross defaults to righttrigger), but the port's own
 * action binds resolved with SDL_GameControllerGetButtonFromString, which knows
 * only digital buttons and returns INVALID for a trigger — so binding an action to
 * L2/R2 silently did nothing. Digitised with the same >16384 half-scale threshold
 * the pad word uses elsewhere. */
extern "C" int PsyX_RawControllerBindHeld(int buttonOrAxis)
{
	int i;
	if (buttonOrAxis < 0)
		return 0;
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (gc && SDL_GameControllerGetAttached(gc) &&
		    abs(GetControllerButtonState(gc, buttonOrAxis)) > 16384)
			return 1;
	}
	return 0;
}

/* PC port: the first attached controller, for the in-game controls panel.
 * Name is SDL's display name; type is an SDL_GameControllerType so the panel
 * can label buttons the way that pad prints them. NULL / -1 when none. */
extern "C" const char* PsyX_Pad_ConnectedControllerName(void)
{
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (gc && SDL_GameControllerGetAttached(gc))
			return SDL_GameControllerName(gc);
	}
	return NULL;
}

extern "C" int PsyX_Pad_ConnectedControllerType(void)
{
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (gc && SDL_GameControllerGetAttached(gc))
			return (int)SDL_GameControllerGetType(gc);
	}
	return -1;
}

/* PC port: the bind name (what PsyX_LookupGameControllerMapping accepts) of a
 * button or trigger held on any attached controller, or NULL. Limited to the
 * launcher's bindable set -- face buttons, back/guide/start, stick clicks,
 * shoulders, d-pad, triggers -- so anything captured in game is a value the
 * launcher can show. Sticks are movement and never returned. */
extern "C" const char* PsyX_Pad_HeldBindName(void)
{
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (!gc || !SDL_GameControllerGetAttached(gc))
			continue;
		for (int b = SDL_CONTROLLER_BUTTON_A; b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; b++)
		{
			if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b))
				return SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
		}
		if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384)
			return SDL_GameControllerGetStringForAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
		if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384)
			return SDL_GameControllerGetStringForAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
	}
	return NULL;
}

/* PC port: signed stick axis, the largest magnitude across attached
 * controllers (PsyX_RawControllerBindHeld folds the sign away). */
extern "C" int PsyX_Pad_AxisValue(int sdlAxis)
{
	int best = 0;
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (!gc || !SDL_GameControllerGetAttached(gc))
			continue;
		int v = SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)sdlAxis);
		if (abs(v) > abs(best))
			best = v;
	}
	return best;
}

/* PC port: Schmitt-trigger digitization. An analog input (trigger/stick) mapped to a
   button presses only above HIGH and releases only below LOW, so a value wavering near a
   single 50% threshold can't chatter the digital bit -- that chatter double-fired the gun
   on analog triggers. Digital buttons report 0/32767 and clear both thresholds (unaffected).
   prevWord is the previous frame's post-hysteresis word for this mapping. */
static inline bool PadBtnPressed(SDL_GameController* cont, int getter, u_short prevWord, u_short bit)
{
	int v = GetControllerButtonState(cont, getter);
	bool was = (prevWord & bit) == 0; /* active-low: clear bit = was pressed */
	return was ? (v > 8000) : (v > 24000);
}

/* Build the active-low 16-bit PSX button word from one controller mapping (with hysteresis). */
static u_short PsyX_Pad_BuildPadWord(SDL_GameController* cont, const PsyXControllerMapping& mapping, u_short prevWord)
{
	u_short ret = 0xFFFF;
	if (PadBtnPressed(cont, mapping.gc_square,     prevWord, 0x8000)) ret &= ~0x8000; //Square
	if (PadBtnPressed(cont, mapping.gc_circle,     prevWord, 0x2000)) ret &= ~0x2000; //Circle
	if (PadBtnPressed(cont, mapping.gc_triangle,   prevWord, 0x1000)) ret &= ~0x1000; //Triangle
	if (PadBtnPressed(cont, mapping.gc_cross,      prevWord, 0x4000)) ret &= ~0x4000; //Cross
	if (PadBtnPressed(cont, mapping.gc_l1,         prevWord, 0x400))  ret &= ~0x400;  //L1
	if (PadBtnPressed(cont, mapping.gc_r1,         prevWord, 0x800))  ret &= ~0x800;  //R1
	if (PadBtnPressed(cont, mapping.gc_l2,         prevWord, 0x100))  ret &= ~0x100;  //L2
	if (PadBtnPressed(cont, mapping.gc_r2,         prevWord, 0x200))  ret &= ~0x200;  //R2
	if (PadBtnPressed(cont, mapping.gc_dpad_up,    prevWord, 0x10))   ret &= ~0x10;   //UP
	if (PadBtnPressed(cont, mapping.gc_dpad_down,  prevWord, 0x40))   ret &= ~0x40;   //DOWN
	if (PadBtnPressed(cont, mapping.gc_dpad_left,  prevWord, 0x80))   ret &= ~0x80;   //LEFT
	if (PadBtnPressed(cont, mapping.gc_dpad_right, prevWord, 0x20))   ret &= ~0x20;   //RIGHT
	if (PadBtnPressed(cont, mapping.gc_l3,         prevWord, 0x2))    ret &= ~0x2;    //L3
	if (PadBtnPressed(cont, mapping.gc_r3,         prevWord, 0x4))    ret &= ~0x4;    //R3
	if (PadBtnPressed(cont, mapping.gc_select,     prevWord, 0x1))    ret &= ~0x1;    //SELECT
	if (PadBtnPressed(cont, mapping.gc_start,      prevWord, 0x8))    ret &= ~0x8;    //START
	return ret;
}

void PsyX_Pad_UpdateGameControllerInput(PsyXController* controller, LPPADRAW pad)
{
	SDL_GameController* cont = controller->gc;
	short leftX, leftY, rightX, rightY;
	u_short ret;

	if (!cont)
	{
		pad->analog[0] = 127;
		pad->analog[1] = 127;
		pad->analog[2] = 127;
		pad->analog[3] = 127;

		*(u_short*)pad->buttons = 0xFFFF;
		return;
	}

	/* Primary binds AND the secondary (second-button-per-action) binds: active-low,
	 * so an action reads pressed if EITHER mapping clears its bit. Analog sticks come
	 * from the primary mapping's axes only. */
	u_short w1 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping,  controller->hystWord[0]);
	u_short w2 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping2, controller->hystWord[1]);
	controller->hystWord[0] = w1;
	controller->hystWord[1] = w2;
	ret = w1 & w2;

	/* "Disable D-pad for movement": un-press the controller D-pad bits (active-low,
	 * so OR them back to 1) so the D-pad no longer drives walk/turn. Keyboard arrows
	 * use a separate word (unaffected), and actions bound to the D-pad read the raw
	 * controller via PsyX_RawControllerButtonHeld, so binding still works. Bits:
	 * UP 0x10, DOWN 0x40, LEFT 0x80, RIGHT 0x20. */
	if (g_cfg_disableDpadMovement)
		ret |= 0x10 | 0x40 | 0x80 | 0x20;

	leftX = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_x);
	leftY = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_y);

	rightX = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_x);
	rightY = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_y);

	*(u_short*)pad->buttons = ret;

	// map to range
	pad->analog[0] = (rightX / 256) + 128;
	pad->analog[1] = (rightY / 256) + 128;
	pad->analog[2] = (leftX / 256) + 128;
	pad->analog[3] = (leftY / 256) + 128;
}

/* Single-player: ANY connected controller drives Player 1. Controllers are
 * assigned to slots in plug order, but the game only reads slot 0, so whichever
 * pad happened to land there was the only one that worked -- with two pads
 * connected the "wrong" one often won. Merge every attached controller into the
 * P1 pad instead: buttons are active-low so a bit is pressed if ANY pad clears
 * it (AND), and each analog axis takes whichever pad is pushed furthest from
 * centre. A pad sitting idle at neutral contributes nothing, so a second
 * controller left alone never fights the one in use. */
static int PsyX_Pad_MergeAllControllers(LPPADRAW pad)
{
	u_short buttons = 0xFFFF;
	short   bestLX = 0, bestLY = 0, bestRX = 0, bestRY = 0;
	int     any = 0, i;

	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* cont = g_controllers[i].gc;
		u_short w1, w2, ret;
		short   lx, ly, rx, ry;

		if (!cont || !SDL_GameControllerGetAttached(cont))
			continue;
		any = 1;

		PsyX_Pad_ProbeRawInput(i, cont);

		w1 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping,  g_controllers[i].hystWord[0]);
		w2 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping2, g_controllers[i].hystWord[1]);
		g_controllers[i].hystWord[0] = w1;
		g_controllers[i].hystWord[1] = w2;
		ret = w1 & w2;
		if (g_cfg_disableDpadMovement)
			ret |= 0x10 | 0x40 | 0x80 | 0x20;
		buttons &= ret;

		lx = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_x);
		ly = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_y);
		rx = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_x);
		ry = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_y);
		if (abs(lx) > abs(bestLX)) bestLX = lx;
		if (abs(ly) > abs(bestLY)) bestLY = ly;
		if (abs(rx) > abs(bestRX)) bestRX = rx;
		if (abs(ry) > abs(bestRY)) bestRY = ry;

		/* Follow the pad that is actually being used, so rumble targets it. A
		 * pressed button (ret != all-released) or a stick well off centre marks
		 * this pad active; an idle pad leaves the current choice alone. */
		if (ret != 0xFFFF ||
		    abs(lx) > 12000 || abs(ly) > 12000 || abs(rx) > 12000 || abs(ry) > 12000)
			g_activeControllerSlot = i;
	}

	if (!any)
	{
		pad->analog[0] = pad->analog[1] = pad->analog[2] = pad->analog[3] = 127;
		*(u_short*)pad->buttons = 0xFFFF;
		return 0;
	}

	*(u_short*)pad->buttons = buttons;
	pad->analog[0] = (bestRX / 256) + 128;
	pad->analog[1] = (bestRY / 256) + 128;
	pad->analog[2] = (bestLX / 256) + 128;
	pad->analog[3] = (bestLY / 256) + 128;
	return 1;
}

static u_short PsyX_Pad_BuildKbWord(const PsyXKeyboardMapping& mapping)
{
	u_short ret = 0xFFFF;

	if (g_sdlKeyboardState[mapping.kc_square])     ret &= ~0x8000;//Square
	if (g_sdlKeyboardState[mapping.kc_circle])     ret &= ~0x2000;//Circle
	if (g_sdlKeyboardState[mapping.kc_triangle])   ret &= ~0x1000;//Triangle
	if (g_sdlKeyboardState[mapping.kc_cross])      ret &= ~0x4000;//Cross
	if (g_sdlKeyboardState[mapping.kc_l1])         ret &= ~0x400; //L1
	if (g_sdlKeyboardState[mapping.kc_l2])         ret &= ~0x100; //L2
	if (g_sdlKeyboardState[mapping.kc_l3])         ret &= ~0x2;   //L3
	if (g_sdlKeyboardState[mapping.kc_r1])         ret &= ~0x800; //R1
	if (g_sdlKeyboardState[mapping.kc_r2])         ret &= ~0x200; //R2
	if (g_sdlKeyboardState[mapping.kc_r3])         ret &= ~0x4;   //R3
	if (g_sdlKeyboardState[mapping.kc_dpad_up])    ret &= ~0x10;  //UP
	if (g_sdlKeyboardState[mapping.kc_dpad_down])  ret &= ~0x40;  //DOWN
	if (g_sdlKeyboardState[mapping.kc_dpad_left])  ret &= ~0x80;  //LEFT
	if (g_sdlKeyboardState[mapping.kc_dpad_right]) ret &= ~0x20;  //RIGHT
	if (g_sdlKeyboardState[mapping.kc_select])     ret &= ~0x1;   //SELECT
	if (g_sdlKeyboardState[mapping.kc_start])      ret &= ~0x8;   //START

	return ret;
}

/* Mouse buttons -> PSX button word (active-low). Each pressed SDL mouse button
 * 1..5 clears whatever PSX bits the config bound it to (g_cfg_mouseButtonMask). */
static u_short PsyX_Pad_BuildMouseWord()
{
	extern int g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
	u_short ret = 0xFFFF;
	Uint32  mb  = SDL_GetMouseState(NULL, NULL);
	int     b;

	for (b = 1; b <= 5; b++)
	{
		if ((mb & SDL_BUTTON(b)) && g_cfg_mouseButtonMask[b])
			ret &= ~g_cfg_mouseButtonMask[b];
	}

	/* Mouse wheel up/down occupy mask slots 6/7 (see Pc_ParseMouseName). The
	 * latch is set on the scroll event and decayed once per frame in
	 * PsyX_EndScene — read it here (don't consume), so a wheel bound to a PSX
	 * button AND to the graphics keys both see the same notch. */
	if (g_PsyX_WheelUpFrames   > 0 && g_cfg_mouseButtonMask[6]) ret &= ~g_cfg_mouseButtonMask[6];
	if (g_PsyX_WheelDownFrames > 0 && g_cfg_mouseButtonMask[7]) ret &= ~g_cfg_mouseButtonMask[7];
	return ret;
}

u_short PsyX_Pad_UpdateKeyboardInput()
{
	u_short ret;

	//Not initialised yet
	if (g_sdlKeyboardState == NULL)
		return 0xFFFF;

	SDL_PumpEvents();

	ret = PsyX_Pad_BuildKbWord(g_cfg_keyboardMapping);

	/* Secondary key binds + mouse buttons (gated by allow_mouse_secondary,
	 * forced on in TPS). Active-low: a button is pressed if clear in ANY
	 * source, so the layers combine with AND. */
	if (g_cfg_allowMouseSecondary)
	{
		ret &= PsyX_Pad_BuildKbWord(g_cfg_keyboardMapping2);
		ret &= PsyX_Pad_BuildMouseWord();
	}

	return ret;
}

int PsyX_Pad_GetStatus(int mtap, int slot)
{
	PsyXController* controller;

	if (slot == 0)
		return 1;	// keyboard always here

	controller = &g_controllers[slot];

	if (controller->gc && SDL_GameControllerGetAttached(controller->gc))
		return 1;

	return 0;
}

void PsyX_Pad_Vibrate(int mtap, int slot, unsigned char* table, int len)
{
	PsyXController* controller = &g_controllers[slot];

	if (len == 0)
		return;

	Uint16 freq_high	= table[0] * 255;
	Uint16 freq_low		= len > 1 ? table[1] * 255 : 0;

	// apply minimal shake
	if(freq_low != 0 && freq_low < 4096)
		freq_low = 4096;

	if (freq_high != 0 && freq_high < 4096)
		freq_high = 4096;

	SDL_GameControllerRumble(controller->gc, freq_low, freq_high, 200);
}

void PsyX_Pad_SetActBuffer(int slot, unsigned char* table, int len)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return;

	g_actBufTable[slot] = table;
	g_actBufLen[slot]   = (table != NULL) ? len : 0;

	if (g_actBufLen[slot] == 0)
	{
		PsyXController* controller = &g_controllers[slot];
		if (controller->gc)
			SDL_GameControllerRumble(controller->gc, 0, 0, 0);
	}
}
