#include "../PsyX_main.h"

#include "psx/libspu.h"
#include "psx/libetc.h"
#include "psx/libmath.h"
#include "PsyX_SPUAL.h"
#include "PsyX/PsyX_public.h" /* g_PsyX_SfxOverride */

#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#ifndef __EMSCRIPTEN__
#include <AL/efx.h>
#endif

/* ALC_SOFT_output_mode / loopback channel tokens. Values are ABI-stable;
 * bare OpenAL 1.1 headers (macOS system framework) lack them, so define
 * fallbacks — every use is still gated on a runtime extension check. */
#ifndef ALC_OUTPUT_MODE_SOFT
#define ALC_OUTPUT_MODE_SOFT  0x19AC
#define ALC_ANY_SOFT          0x19AD
#define ALC_STEREO_BASIC_SOFT 0x19AE
#define ALC_STEREO_UHJ_SOFT   0x19AF
#define ALC_STEREO_HRTF_SOFT  0x19B2
#endif
#ifndef ALC_MONO_SOFT
#define ALC_MONO_SOFT   0x1500
#define ALC_STEREO_SOFT 0x1501
#define ALC_QUAD_SOFT   0x1503
#endif
#ifndef ALC_SURROUND_5_1_SOFT
#define ALC_SURROUND_5_1_SOFT 0x1504
#define ALC_SURROUND_6_1_SOFT 0x1505
#define ALC_SURROUND_7_1_SOFT 0x1506
#endif
#ifndef ALC_SOFT_HRTF
typedef ALCboolean(ALC_APIENTRY* LPALCRESETDEVICESOFT)(ALCdevice* device, const ALCint* attribs);
#endif

/* This TU is compiled with -fvisibility=hidden on ELF so its global OpenAL
 * EFX function-pointer variables do not interpose libopenal for other
 * modules (see PsyCross/CMakeLists.txt). The PsyX_SPUAL_ API below, however,
 * is called directly by map overlay .so's, which on Linux now import the host
 * exe's single PsyCross instance instead of linking their own copy, so these
 * entry points must stay default-visibility to be exported via -rdynamic. */
#if defined(__GNUC__) && !defined(_WIN32)
#  define PSX_API_EXPORT __attribute__((visibility("default")))
#else
#  define PSX_API_EXPORT
#endif

// TODO: implement XA, implement ADSR

static const char* getALCErrorString(int err)
{
	switch (err)
	{
	case ALC_NO_ERROR:
		return "AL_NO_ERROR";
	case ALC_INVALID_DEVICE:
		return "ALC_INVALID_DEVICE";
	case ALC_INVALID_CONTEXT:
		return "ALC_INVALID_CONTEXT";
	case ALC_INVALID_ENUM:
		return "ALC_INVALID_ENUM";
	case ALC_INVALID_VALUE:
		return "ALC_INVALID_VALUE";
	case ALC_OUT_OF_MEMORY:
		return "ALC_OUT_OF_MEMORY";
	default:
		return "AL_UNKNOWN";
	}
}

static const char* getALErrorString(int err)
{
	switch (err)
	{
	case AL_NO_ERROR:
		return "AL_NO_ERROR";
	case AL_INVALID_NAME:
		return "AL_INVALID_NAME";
	case AL_INVALID_ENUM:
		return "AL_INVALID_ENUM";
	case AL_INVALID_VALUE:
		return "AL_INVALID_VALUE";
	case AL_INVALID_OPERATION:
		return "AL_INVALID_OPERATION";
	case AL_OUT_OF_MEMORY:
		return "AL_OUT_OF_MEMORY";
	default:
		return "AL_UNKNOWN";
	}
}

#define SPU_REALMEMSIZE			(512 * 1024)
#define SPU_MEMSIZE				(2048*1024)		// SPU_REALMEMSIZE

typedef struct
{
	u_char samplemem[SPU_MEMSIZE];
	u_char* writeptr;
} SPUMemory;

static SPUMemory s_SpuMemory;
static SDL_mutex* g_SpuMutex = NULL;
static int g_spuInit = 0;
static int s_spuMallocVal = 0;

/* SPU ADSR envelope master gate. Default ON since 2026-07-06: the historical
 * render-thread deadlock was fixed by moving the tick to the audio-timing
 * thread, the key-status release-tail hang was fixed (keyed-off enveloped
 * voices report free immediately), and side-by-side PSX comparison confirmed
 * envelopes are required for the sequenced BGM's instrument fades. Opt-out via
 * the `adsr 0` console command or `adsr = 0` in config.cfg. When OFF,
 * SetKey/SetVoiceAttr/Update all take their pre-envelope code paths. */
int g_SpuAdsrEnabled = 1;

PSX_API_EXPORT void PsyX_SPUAL_SetAdsrEnabled(int on) { g_SpuAdsrEnabled = on ? 1 : 0; }
PSX_API_EXPORT int  PsyX_SPUAL_GetAdsrEnabled(void)   { return g_SpuAdsrEnabled; }

/* Speaker layout (config key audio_output): 0=auto 1=stereo 2=quad 3=5.1
 * 4=7.1 5=hrtf. AL-free enum so the console/launcher (and a future non-AL
 * backend) can share it. "auto" passes NO ALC_OUTPUT_MODE_SOFT attribute:
 * OpenAL Soft then detects the system layout itself AND the user's
 * alsoft.ini keeps authority (the attribute would override it). Voice
 * routing trusts only the ACHIEVED mode — a 5.1 request on a stereo
 * endpoint silently degrades, it does not error. */
enum
{
	PSYX_SPK_AUTO = 0,
	PSYX_SPK_STEREO,
	PSYX_SPK_QUAD,
	PSYX_SPK_51,
	PSYX_SPK_71,
	PSYX_SPK_HRTF,
};
static int s_speakersRequest  = PSYX_SPK_AUTO;
static int s_speakersAchieved = PSYX_SPK_AUTO;
static int s_surroundActive   = 0; // achieved layout has rear speakers

/* Emitter azimuth side-channel (PSX Q12 angle: 0 = dead ahead, positive =
 * right, +/-2048 = behind). Game code recovers the true camera-relative
 * angle before it collapses direction to an L/R balance and stashes it
 * here; the next key-on's start-address write claims it.
 *
 * Ownership: the arm -> key-on chain is synchronous on the arming thread,
 * while sequencer note-ons run on the intr/timer thread — the claim is
 * therefore restricted to WDSA writes from the ARMING thread, so a BGM
 * note interleaving between arm and key-on can neither steal the angle
 * nor get mispositioned by it. The TTL expires orphans (key-on failed:
 * no free voice, bad VAB) that would otherwise wait to mistag a later
 * same-thread sound. */
static int           s_nextAzimuthQ12    = 0;
static int           s_nextAzimuthValid  = 0;
static SDL_threadID  s_nextAzimuthThread = 0;
static Uint32        s_nextAzimuthMs     = 0;
#define AZIMUTH_STASH_TTL_MS 100

PSX_API_EXPORT void PsyX_SPUAL_SetOutputMode(int mode) { s_speakersRequest = mode; }
PSX_API_EXPORT int  PsyX_SPUAL_GetOutputMode(void)     { return s_speakersAchieved; }
PSX_API_EXPORT int  PsyX_SPUAL_GetSurroundActive(void) { return s_surroundActive; }

/* Arm the azimuth for the sound about to key on (claimed by the next
 * start-address write in SetVoiceAttr). Clear covers the compute-without-
 * play case so an unrelated later key-on can't inherit a stale angle. */
PSX_API_EXPORT void PsyX_SPUAL_SetNextKeyOnAzimuth(int azimuthQ12)
{
	SDL_LockMutex(g_SpuMutex);
	s_nextAzimuthQ12    = azimuthQ12;
	s_nextAzimuthThread = SDL_ThreadID();
	s_nextAzimuthMs     = SDL_GetTicks();
	s_nextAzimuthValid  = 1;
	SDL_UnlockMutex(g_SpuMutex);
}

PSX_API_EXPORT void PsyX_SPUAL_ClearNextKeyOnAzimuth(void)
{
	SDL_LockMutex(g_SpuMutex);
	s_nextAzimuthValid = 0;
	SDL_UnlockMutex(g_SpuMutex);
}

/* Live-update path (Sd_SfxAttributesUpdate): the voice is already playing
 * and identified; the following SpuSetVoiceAttr volume write repositions it. */
PSX_API_EXPORT void PsyX_SPUAL_SetVoiceAzimuth(int voiceIdx, int azimuthQ12);

typedef enum
{
	ENV_OFF = 0,
	ENV_ATTACK,
	ENV_DECAY,
	ENV_SUSTAIN,
	ENV_RELEASE,
} EnvPhase;

typedef struct
{
	SpuVoiceAttr attr;	// .voice is Id of this channel

	ALuint alBuffer;
	ALuint alSource;
	ushort sampledirty;
	ushort reverb;

	// PSX SPU ADSR envelope (PC port). Engaged for EVERY keyed voice that
	// programmed a real ADSR (adsr1/adsr2 != 0), looping or not — the SPU
	// hardware doesn't distinguish. One-shot melodic voices (bells, chimes)
	// need the release ring-out and the decay/sustain shaping just as much
	// as loops; gating on looping made every sequencer note-off a hard cut.
	// Hardware runs the envelope at 44100Hz on a 0..0x7FFF level and
	// multiplies the sample by it.
	int      envPhase;     // EnvPhase
	int      envLevel;     // 0..0x7FFF
	int      envCounter;   // accumulated 44100Hz samples toward next step
	float    baseGain;     // volume-derived gain before envelope
	ushort   hasEnvelope;  // adsr programmed
	ushort   looping;      // AL_LOOPING was set for the current sample

	// Replacement samples (pc_sfx_override.c) upload at the FILE's rate, so
	// the pitch the voice was keyed with becomes the 1.0 baseline: AL_PITCH
	// writes divide by this instead of 4096 while the override buffer plays.
	// 0 = native sample, normal 4096 semantics.
	int      overridePitchBase;

	// Spatial routing (PC surround). azimuth claimed from the key-on stash
	// at each start-address write, so voice reuse can't inherit stale state.
	ushort   isWide;       // libsd wide-stereo (negated right volume) latch
	ushort   azimuthValid;
	int      azimuthQ12;   // PSX Q12 angle: 0 ahead, positive right

	u_int    relStartMs;   // SDL tick at key-off; caps pathological release tails
} SPUALVoice;

const int s_spuVoiceCount = 24;

SPUALVoice	g_SpuVoices[s_spuVoiceCount];
ALCdevice*	g_ALCdevice = NULL;
ALCcontext* g_ALCcontext = NULL;
int			g_SPUMuted = 0;
ALuint		g_ALEffectSlots[2];
int			g_currEffectSlotIdx = 0;
ALuint		g_nAlReverbEffect = 0;
int			g_enableSPUReverb = 0;
int			g_ALEffectsSupported = 0;

#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)

LPALGENEFFECTS alGenEffects = NULL;
LPALDELETEEFFECTS alDeleteEffects = NULL;
LPALEFFECTI alEffecti = NULL;
LPALEFFECTF alEffectf = NULL;
LPALGENAUXILIARYEFFECTSLOTS alGenAuxiliaryEffectSlots = NULL;
LPALDELETEAUXILIARYEFFECTSLOTS alDeleteAuxiliaryEffectSlots = NULL;
LPALAUXILIARYEFFECTSLOTI alAuxiliaryEffectSloti = NULL;
LPALAUXILIARYEFFECTSLOTF alAuxiliaryEffectSlotf = NULL;

#endif

/* SPU reverb depth (wet level). The game drives this constantly: a per-track
 * depth on every BGM bank load (g_Sd_ReverbDepths), a per-tick RAMP from 0 to
 * the track target on sequence (re)start (libsd replay_reverb_set — the PSX
 * "music fades in with its echo"), and SdSetRVol/SdUtSetReverbDepth one-shots.
 * PSX depth is s16 (typical game values (u8 depth)<<8, i.e. 2560..30720);
 * mapped to the OpenAL aux effect SLOT gain so it applies live to every
 * routed voice without touching the effect parameters. */
static short g_reverbDepthL = 0;
static short g_reverbDepthR = 0;
static int   g_reverbMode = 1;
float g_SpuReverbDepthScale = 2.0f; /* wet = |depth|/32768 * scale; `revscale` console knob */

static void ApplyReverbWet(void)
{
#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)
	if (!g_ALEffectsSupported || !alAuxiliaryEffectSlotf)
		return;
	int dl = g_reverbDepthL < 0 ? -g_reverbDepthL : g_reverbDepthL;
	int dr = g_reverbDepthR < 0 ? -g_reverbDepthR : g_reverbDepthR;
	float wet = (float)(dl > dr ? dl : dr) / 32768.0f * g_SpuReverbDepthScale;
	if (wet < 0.0f) wet = 0.0f;
	if (wet > 1.0f) wet = 1.0f;
	alAuxiliaryEffectSlotf(g_ALEffectSlots[g_currEffectSlotIdx], AL_EFFECTSLOT_GAIN, wet);
#endif
}

static void InitOpenAlEffects()
{
	g_ALEffectsSupported = 0;
#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)
	if (!alcIsExtensionPresent(g_ALCdevice, ALC_EXT_EFX_NAME))
	{
		eprintf("PSX SPU effects are NOT supported!\n");
		return;
	}

	alGenEffects = (LPALGENEFFECTS)alGetProcAddress("alGenEffects");
	alDeleteEffects = (LPALDELETEEFFECTS)alGetProcAddress("alDeleteEffects");
	alEffecti = (LPALEFFECTI)alGetProcAddress("alEffecti");
	alEffectf = (LPALEFFECTF)alGetProcAddress("alEffectf");
	alGenAuxiliaryEffectSlots = (LPALGENAUXILIARYEFFECTSLOTS)alGetProcAddress("alGenAuxiliaryEffectSlots");
	alDeleteAuxiliaryEffectSlots = (LPALDELETEAUXILIARYEFFECTSLOTS)alGetProcAddress("alDeleteAuxiliaryEffectSlots");
	alAuxiliaryEffectSloti = (LPALAUXILIARYEFFECTSLOTI)alGetProcAddress("alAuxiliaryEffectSloti");
	alAuxiliaryEffectSlotf = (LPALAUXILIARYEFFECTSLOTF)alGetProcAddress("alAuxiliaryEffectSlotf");

	int max_sends = 0;
	alcGetIntegerv(g_ALCdevice, ALC_MAX_AUXILIARY_SENDS, 1, &max_sends);

	// make reverb effect slot
	g_currEffectSlotIdx = 0;
	alGenAuxiliaryEffectSlots(1, g_ALEffectSlots);

	// make reverb effect
	alGenEffects(1, &g_nAlReverbEffect);
	alEffecti(g_nAlReverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);

	// setup defaults of effect
	alEffectf(g_nAlReverbEffect, AL_REVERB_GAIN, 0.45f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_GAINHF, 0.25f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DECAY_TIME, 2.0f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DECAY_HFRATIO, 0.9f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_REFLECTIONS_DELAY, 0.08f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_REFLECTIONS_GAIN, 0.2f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DIFFUSION, 0.9f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DENSITY, 0.1f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_AIR_ABSORPTION_GAINHF, 0.1f);

	g_ALEffectsSupported = 1;

	eprintf("PSX SPU effects are supported and initialized\n");

	alAuxiliaryEffectSloti(g_ALEffectSlots[g_currEffectSlotIdx], AL_EFFECTSLOT_EFFECT, g_nAlReverbEffect);
#endif
}

#ifndef __EMSCRIPTEN__

static int SpeakersToAlcOutputMode(int spk)
{
	switch (spk)
	{
	case PSYX_SPK_STEREO: return ALC_STEREO_BASIC_SOFT;
	case PSYX_SPK_QUAD:   return ALC_QUAD_SOFT;
	case PSYX_SPK_51:     return ALC_SURROUND_5_1_SOFT;
	case PSYX_SPK_71:     return ALC_SURROUND_7_1_SOFT;
	case PSYX_SPK_HRTF:   return ALC_STEREO_HRTF_SOFT;
	}
	return ALC_ANY_SOFT;
}

static int AlcOutputModeToSpeakers(int alcMode)
{
	switch (alcMode)
	{
	case ALC_QUAD_SOFT:           return PSYX_SPK_QUAD;
	case ALC_SURROUND_5_1_SOFT:
	case ALC_SURROUND_6_1_SOFT:   return PSYX_SPK_51;
	case ALC_SURROUND_7_1_SOFT:   return PSYX_SPK_71;
	case ALC_STEREO_HRTF_SOFT:    return PSYX_SPK_HRTF;
	}
	return PSYX_SPK_STEREO;
}

static const char* s_speakerModeNames[] = { "auto", "stereo", "quad", "5.1", "7.1", "hrtf" };

/* attrs must have room for 8 ints. The output-mode pair is added only for an
 * explicit override — never for auto (see the enum comment above). */
static void BuildContextAttrs(int* attrs)
{
	int n = 0;

	attrs[n++] = ALC_FREQUENCY;
	attrs[n++] = 44100;
	attrs[n++] = ALC_MAX_AUXILIARY_SENDS;
	attrs[n++] = 2;

	if (s_speakersRequest != PSYX_SPK_AUTO && g_ALCdevice &&
	    alcIsExtensionPresent(g_ALCdevice, "ALC_SOFT_output_mode"))
	{
		attrs[n++] = ALC_OUTPUT_MODE_SOFT;
		attrs[n++] = SpeakersToAlcOutputMode(s_speakersRequest);
	}

	attrs[n] = 0;
}

static void QueryAchievedOutputMode(void)
{
	int alcMode = 0;

	s_speakersAchieved = PSYX_SPK_AUTO;
	s_surroundActive   = 0;

	if (!g_ALCdevice || !alcIsExtensionPresent(g_ALCdevice, "ALC_SOFT_output_mode"))
		return;

	alcGetIntegerv(g_ALCdevice, ALC_OUTPUT_MODE_SOFT, 1, &alcMode);
	s_speakersAchieved = AlcOutputModeToSpeakers(alcMode);
	s_surroundActive   = s_speakersAchieved == PSYX_SPK_QUAD ||
	                     s_speakersAchieved == PSYX_SPK_51 ||
	                     s_speakersAchieved == PSYX_SPK_71;

	eprintinfo("speaker layout: %s (requested %s, ALC mode 0x%x)%s\n",
		s_speakerModeNames[s_speakersAchieved], s_speakerModeNames[s_speakersRequest],
		alcMode, s_surroundActive ? " [surround routing active]" : "");
}

#endif // __EMSCRIPTEN__

/* Live layout switch (console AUDIOOUT): renegotiates the output mode on the
 * open device via alcResetDeviceSOFT — sources keep playing, only the mix
 * format flips. Returns 1 on success; 0 means restart required. Called
 * before init it just latches the request for InitSound. */
PSX_API_EXPORT int PsyX_SPUAL_ApplyOutputMode(int mode)
{
	s_speakersRequest = mode;

#ifndef __EMSCRIPTEN__
	if (!g_ALCdevice)
		return 1;

	LPALCRESETDEVICESOFT resetFn = (LPALCRESETDEVICESOFT)alcGetProcAddress(g_ALCdevice, "alcResetDeviceSOFT");
	if (!resetFn)
		return 0;

	int attrs[8];
	BuildContextAttrs(attrs);

	int ok = resetFn(g_ALCdevice, attrs) == ALC_TRUE;
	QueryAchievedOutputMode();
	return ok;
#else
	return 0;
#endif
}

int PsyX_SPUAL_InitSound()
{
	if (!g_SpuMutex)
		g_SpuMutex = SDL_CreateMutex();

	if (!g_spuInit)
		memset(&s_SpuMemory, 0, sizeof(s_SpuMemory));

	g_spuInit = 1;

	int numDevices, alErr, i;
	const char* devices;
	const char* devStrptr;

	if (g_ALCdevice)
		return 1;

	numDevices = 0;

	// Init openAL
	// check devices list

	devStrptr = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
	devices = devStrptr;

	// go through device list (each device terminated with a single NULL, list terminated with double NULL)
	while ((*devStrptr) != '\0')
	{
		eprintinfo("found sound device: %s\n", devStrptr);
		devStrptr += strlen(devStrptr) + 1;
		numDevices++;
	}

	if(numDevices == 0)
		return 0;
	
	g_ALCdevice = alcOpenDevice(NULL);

	alErr = AL_NO_ERROR;

	if (!g_ALCdevice)
	{
		alErr = alcGetError(NULL);
		eprinterr("alcOpenDevice: NULL DEVICE error: %s\n", getALCErrorString(alErr));
		return 0;
	}

#ifndef __EMSCRIPTEN__
	int al_context_params[8];
	BuildContextAttrs(al_context_params);
	g_ALCcontext = alcCreateContext(g_ALCdevice, al_context_params);
#else
	g_ALCcontext = alcCreateContext(g_ALCdevice, NULL);
#endif

	alErr = alcGetError(g_ALCdevice);
	if (alErr != AL_NO_ERROR)
	{
		eprinterr("alcCreateContext error: %s\n", getALCErrorString(alErr));
		return 0;
	}

	alcMakeContextCurrent(g_ALCcontext);

	alErr = alcGetError(g_ALCdevice);
	if (alErr != AL_NO_ERROR)
	{
		eprinterr("alcMakeContextCurrent error: %s\n", getALCErrorString(alErr));
		return 0;
	}

	// Setup defaults
	alListenerf(AL_GAIN, 1.0f);
	alDistanceModel(AL_NONE);

#ifndef __EMSCRIPTEN__
	QueryAchievedOutputMode();
#endif

	// create channels
	for (i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		memset(voice, 0, sizeof(SPUALVoice));

		alGenSources(1, &voice->alSource);
		alGenBuffers(1, &voice->alBuffer);
#ifdef AL_SOFT_source_resampler
		alSourcei(voice->alSource, AL_SOURCE_RESAMPLER_SOFT, 2);	// Use cubic resampler
#endif
		alSourcei(voice->alSource, AL_SOURCE_RELATIVE, AL_TRUE);
	}


	InitOpenAlEffects();

	return 1;
}

void PsyX_SPUAL_ShutdownSound()
{
	g_spuInit = 0;

#ifndef __EMSCRIPTEN__
	if (!g_ALCcontext)
		return;

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		alDeleteSources(1, &voice->alSource);
		alDeleteBuffers(1, &voice->alBuffer);
	}

	if (g_ALEffectsSupported)
	{
#ifndef __SWITCH__
		alDeleteEffects(1, &g_nAlReverbEffect);
		alDeleteAuxiliaryEffectSlots(1, g_ALEffectSlots);
#endif
		g_ALEffectsSupported = AL_NONE;
	}

	alcDestroyContext(g_ALCcontext);
	alcCloseDevice(g_ALCdevice);

	g_ALCcontext = NULL;
	g_ALCdevice = NULL;
#endif // __EMSCRIPTEN__
}

//--------------------------------------------------------------------------------

int PsyX_SPUAL_Alloc(int size)
{
	int addr = s_spuMallocVal;
	s_spuMallocVal += size;

	if (s_spuMallocVal > SPU_MEMSIZE)
		return -1;

	return addr;
}

int PsyX_SPUAL_InitAlloc(int num, char* top)
{
	s_spuMallocVal = 0;
	return 0;
}

void PsyX_SPUAL_Free(u_int addr)
{
	s_spuMallocVal = 0;
}

u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr)
{
	s_SpuMemory.writeptr = s_SpuMemory.samplemem + addr;

	if (addr > SPU_MEMSIZE)
		return 0;

	if (addr < 0x1010)
		return 0;

	return 1;
}

u_int PsyX_SPUAL_Write(u_char* addr, u_int size)
{
	//if (0x7EFF0 < size)
	//	size = 0x7EFF0;

	volatile int wptr_ofs = s_SpuMemory.writeptr - s_SpuMemory.samplemem;

	if (wptr_ofs + size > SPU_REALMEMSIZE)
	{
		eprintf("SPU WARNING: SpuWrite exceeded SPU_REALMEMSIZE by %d bytes!\n", wptr_ofs + size - SPU_REALMEMSIZE);
	}
	assert(size > 0 && wptr_ofs + size < SPU_MEMSIZE);

	// simply copy to the writeptr
	memcpy(s_SpuMemory.writeptr, addr, size);

#if 0 // BANK TEST
	{
		static short waveBuffer[SPU_MEMSIZE];

		ALuint alSource;
		ALuint alBuffer;

		alGenSources(1, &alSource);
		alGenBuffers(1, &alBuffer);

		int loopStart = 0, loopLen = 0;
		int count = decodeSound(addr, size, waveBuffer, &loopStart, &loopLen);

		// update AL buffer
		alBufferData(alBuffer, AL_FORMAT_MONO16, waveBuffer, count * sizeof(short), 11000);

		// set the buffer
		alSourcei(alSource, AL_BUFFER, alBuffer);
		alSourcef(alSource, AL_GAIN, 1.0f);// TODO: panning
		alSourcef(alSource, AL_PITCH, 1);

		alSourcePlay(alSource);
		int status;
		do
		{
			alGetSourcei(alSource, AL_SOURCE_STATE, &status);
		} while (status == AL_PLAYING);

		alSourceStop(alSource);

		alDeleteSources(1, &alSource);
		alDeleteBuffers(1, &alBuffer);
	}
#endif

	return size;
}

u_int PsyX_SPUAL_Read(u_char* addr, u_int size)
{
	volatile int rptr_ofs = s_SpuMemory.writeptr - s_SpuMemory.samplemem;

	if (rptr_ofs + size > SPU_REALMEMSIZE)
	{
		eprintf("SPU WARNING: SpuRead exceeded SPU_REALMEMSIZE by %d bytes!\n", rptr_ofs + size - SPU_REALMEMSIZE);
	}
	assert(size > 0 && rptr_ofs + size < SPU_MEMSIZE);

	// simply copy to the writeptr
	memcpy(addr, s_SpuMemory.writeptr, size);

	return size;
}

// PSX ADPCM coefficients
static const float K0[5] = { 0, 0.9375, 1.796875, 1.53125, 1.90625 };
static const float K1[5] = { 0, 0, -0.8125, -0.859375, -0.9375 };

// PSX ADPCM decoding routine - decodes a single sample
static short vagToPcm(u_char soundParameter, int soundData, float* vagPrev1, float* vagPrev2)
{
	int resultInt = 0;

	float dTmp1 = 0.0;
	float dTmp2 = 0.0;
	float dTmp3 = 0.0;

	if (soundData > 7)
		soundData -= 16;

	dTmp1 = (float)soundData * pow(2, (float)(12 - (soundParameter & 0x0F)));

	dTmp2 = (*vagPrev1) * K0[(soundParameter >> 4) & 0x0F];
	dTmp3 = (*vagPrev2) * K1[(soundParameter >> 4) & 0x0F];

	(*vagPrev2) = (*vagPrev1);
	(*vagPrev1) = dTmp1 + dTmp2 + dTmp3;

	resultInt = (int)round((*vagPrev1));

	if (resultInt > 32767)
		resultInt = 32767;

	if (resultInt < -32768)
		resultInt = -32768;

	return (short)resultInt;
}

typedef enum 
{
	LoopEnd = 1 << 0,		// Jump to repeat address after this block
							// 1 - Copy repeatAddress to currentAddress AFTER this block
							//     set ENDX (TODO: Immediately or after this block?)
							// 0 - Nothing

	Repeat = 1 << 1,		// Takes an effect only with LoopEnd bit set.
							// 1 - Loop normally
							// 0 - Loop and force Release

	LoopStart = 1 << 2,		// Mark current address as the beginning of repeat
							// 1 - Load currentAddress to repeatAddress
							// 0 - Nothing
} ADPCM_FLAGS;


// Main decoding routine - Takes PSX ADPCM formatted audio data and converts it to PCM. It also extracts the looping information if used.
static int decodeSound(u_char* iData, int soundSize, short* oData, int* loopStart, int* loopLength, int breakOnEnd /*= 0*/)
{
	u_char sp;
	u_char flag;
	int sd = 0;
	float vagPrev1 = 0.0;
	float vagPrev2 = 0.0;
	int k = 0;

	int loopStrt = 0, loopEnd = 0;
	int breakOn = -1;

	/* Cap output to prevent buffer overflow — caller passes waveBuffer[SPU_REALMEMSIZE] */
	const int maxOutputSamples = SPU_REALMEMSIZE - 2;

	for (int i = 0; i < soundSize; i++)
	{
		if (i % 16 == 0)
		{
			sp = iData[i];
			flag = iData[i+1];
			i += 2;
		}

		if (k >= maxOutputSamples)
			break;

		sd = (int)iData[i] & 0xF;
		oData[k++] = vagToPcm(sp, sd, &vagPrev1, &vagPrev2);

		sd = ((int)iData[i] >> 4) & 0xF;
		oData[k++] = vagToPcm(sp, sd, &vagPrev1, &vagPrev2);

		if (breakOnEnd && k == breakOn)
			return k;

		if (breakOn == -1)
		{
			// flags parsed
			if (flag & LoopStart)
			{
				loopStrt = k + 26; // FIXME: is that correct?
			}

			if (flag & LoopEnd)
			{
				loopEnd = k + 26;

				if (flag & Repeat)
				{
					*loopStart = loopStrt;
					*loopLength = loopEnd - loopStrt;
				}

				if (breakOnEnd)
					breakOn = k + 26;
			}
		}
	}

	return k;
}

/* Optional per-sound sample replacement, installed by the host port. Left NULL
 * here so PsyCross still links and behaves identically on its own — the host
 * assigns it when loose-file sound mods are in play. */
extern "C" PsyX_SfxOverrideFn g_PsyX_SfxOverride = NULL;

static void UpdateVoiceSample(SPUALVoice* voice)
{
	static short waveBuffer[SPU_REALMEMSIZE];
	int loopStart, loopLen, count;
	ALuint alSource, alBuffer;

	//if (!voice->sampledirty)
	//	return;

	voice->sampledirty = 0;

	alSource = voice->alSource;
	alBuffer = voice->alBuffer;

	if (alSource == AL_NONE)
		return;

	loopStart = 0;
	loopLen = 0;

	{
		// Loose per-sound replacement (pc_sfx_override.c). The registry is keyed
		// on the SPU address a voice plays from, so only replaced samples divert
		// here — everything else in the same bank decodes from the original data
		// below, unchanged. PC-owned PCM has no ADPCM size ceiling, which is what
		// lifts the limit on how big a replacement sound can be.
		const short* modPcm = NULL;
		int modCount = 0;
		int modRate = 0;

		if (g_PsyX_SfxOverride != NULL &&
			g_PsyX_SfxOverride((int)voice->attr.addr, &modPcm, &modCount, &modRate) && modCount > 0)
		{
			// A replacement plays at the rate it was AUTHORED at, whatever that is.
			//
			// The voice multiplies whatever we upload by attr.pitch/4096 (the
			// AL_PITCH write in the attribute path), so uploading at a fixed 44100
			// like the native decode path made the final rate 44100*pitch/4096 —
			// the rate the ORIGINAL sample happened to play at. A replacement then
			// had to be authored at exactly that rate or it came out slow and low,
			// like tape speed, which is unguessable without the launcher's Audio
			// tool and worst on the map banks (several are authored very low).
			//
			// So: upload at the file's own rate and latch the pitch this key-on
			// carries as the voice's 1.0 baseline — AL_PITCH writes divide by the
			// latch instead of 4096 while this buffer is playing. The first attempt
			// scaled the BUFFER rate by 4096/pitch instead, but that overflows any
			// sane rate band for the low-pitch bank samples (a 44.1k file over an
			// ~8k sample wants a 226kHz buffer) — precisely the sounds this exists
			// to fix. The latch keeps the buffer at its real rate always.
			//
			// Pitch the game applies LATER still scales RELATIVE to the latch, so
			// a modulated sound (elevator hum, ambience) keeps its modulation. What
			// this gives up is pitch variation applied at trigger time — a sample
			// the game keys at two different pitches now starts at its authored
			// pitch both times. That is the trade the contract asks for: the
			// replacement sounds like the file.
			int rate = (modRate > 0) ? modRate : 44100;
			voice->overridePitchBase = (modRate > 0 && voice->attr.pitch > 0) ? voice->attr.pitch : 0;
			alSourcei(alSource, AL_BUFFER, 0);
			alBufferData(alBuffer, AL_FORMAT_MONO16, modPcm, modCount * sizeof(short), rate);
			alSourcei(alSource, AL_BUFFER, alBuffer);
			// The attribute path already ran for this key-on and divided by the
			// PREVIOUS occupant's base; re-assert with the fresh latch.
			if (voice->attr.pitch > 0)
				alSourcef(alSource, AL_PITCH, (float)voice->attr.pitch / (voice->overridePitchBase > 0 ? (float)voice->overridePitchBase : 4096.0f));
			return;
		}
	}

	count = decodeSound(s_SpuMemory.samplemem + voice->attr.addr, SPU_MEMSIZE - voice->attr.addr, waveBuffer, &loopStart, &loopLen, 1);

	if (count == 0)
		return;

	// Voice reuse: a native sample after a replacement must not inherit its
	// pitch baseline, and the attribute path may have divided by it already.
	if (voice->overridePitchBase > 0)
	{
		voice->overridePitchBase = 0;
		if (voice->attr.pitch > 0)
			alSourcef(alSource, AL_PITCH, (float)voice->attr.pitch / 4096.0f);
	}

	alSourcei(alSource, AL_BUFFER, 0);
	alBufferData(alBuffer, AL_FORMAT_MONO16, waveBuffer, count * sizeof(short), 44100);

	if (loopLen > 0)
	{
		// On PSX, the SPU hardware tracks loop_addr during playback.
		// On PC, we pre-decode the whole sample, so loop_addr is stale
		// from previous voice assignments. The original adjustment
		// (loopStart += loop_addr - addr) can produce plausible-but-wrong
		// loop points when a voice channel is reused with a different sample.
		//
		// Honor the PSX loop region [loopStart, loopStart+loopLen] instead of
		// looping the whole buffer. Many one-shot SFX (e.g. cutscene grunts,
		// the boss shove) carry a tiny sustain-tail loop region at the end;
		// whole-buffer looping made the ENTIRE sound repeat forever, which is
		// audibly wrong. AL_LOOP_POINTS_SOFT loops only the tail, matching SPU
		// hardware. Genuine full-sample loops (loopStart=0) are unchanged.
		if (loopStart >= 0 && loopStart < count)
		{
			ALint loopEnd = loopStart + loopLen;
			if (loopEnd > count) loopEnd = count;
			ALint loopPoints[2] = { loopStart, loopEnd };
			alGetError();
			alBufferiv(alBuffer, AL_LOOP_POINTS_SOFT, loopPoints);
		}
		alSourcei(alSource, AL_LOOPING, AL_TRUE);
		voice->looping = 1;
	}
	else
	{
		alSourcei(alSource, AL_LOOPING, AL_FALSE);
		voice->looping = 0;
	}

	// set the buffer
	alSourcei(alSource, AL_BUFFER, alBuffer);
}

// ---- PSX SPU ADSR envelope ------------------------------------------------
// Reference: nocash psx-spx "Envelope Operation". adsr1/adsr2 are the raw
// hardware registers (the game programs them via SpuSetVoiceAttr mask
// SPU_VOICE_ADSR_ADSR1/ADSR2). The envelope advances a 0..0x7FFF level at
// 44100Hz; each "step" the level changes by AdsrStep every AdsrCycles samples.

#define ADSR_MAX 0x7FFF

static int adsr_sustain_level(u_short adsr1)
{
	int sl = adsr1 & 0x0F;                 // bits 0-3
	int lvl = (sl + 1) << 11;              // (N+1)*0x800
	return lvl > ADSR_MAX ? ADSR_MAX : lvl;
}

// Decompose a 0..0x7F rate into (cycles, step) at the current level.
static void adsr_step_params(int rate, int increase, int exponential, int level,
	int* outCycles, int* outStep)
{
	int shift = (rate >> 2) & 0x1F;
	int sel   = rate & 3;
	int step  = increase ? (7 - sel) : (-8 + sel);
	int cycles = 1 << (shift > 11 ? shift - 11 : 0);

	if (shift < 11)
		step <<= (11 - shift);

	if (exponential)
	{
		if (increase && level > 0x6000)
			cycles <<= 2;                  // exponential attack slows near the top
		if (!increase)
			step = (step * level) >> 15;   // exponential decrease scales with level
	}

	if (cycles < 1)
		cycles = 1;

	*outCycles = cycles;
	*outStep = step;
}

static void EnvelopeKeyOn(SPUALVoice* voice)
{
	voice->envPhase   = ENV_ATTACK;
	voice->envLevel   = 0;
	voice->envCounter = 0;
}

static void EnvelopeKeyOff(SPUALVoice* voice)
{
	if (voice->envPhase != ENV_OFF)
		voice->envPhase = ENV_RELEASE;
}

// Advance one voice's envelope by `samples` 44100Hz ticks. Returns the
// 0..0x7FFF level. Recomputes step params each step so phase/level changes
// (exponential curves) track correctly.
static int EnvelopeAdvance(SPUALVoice* voice, int samples)
{
	const u_short adsr1 = voice->attr.adsr1;
	const u_short adsr2 = voice->attr.adsr2;

	const int sustainLevel = adsr_sustain_level(adsr1);

	voice->envCounter += samples;

	for (int guard = 0; guard < 200000 && voice->envPhase != ENV_OFF; guard++)
	{
		int rate, increase, exponential;

		switch (voice->envPhase)
		{
		case ENV_ATTACK:
			rate        = (adsr1 >> 8) & 0x7F;
			increase    = 1;
			exponential = (adsr1 >> 15) & 1;
			break;
		case ENV_DECAY:
			rate        = ((adsr1 >> 4) & 0x0F) << 2; // 4-bit, *4
			increase    = 0;
			exponential = 1;                          // decay is always exponential
			break;
		case ENV_SUSTAIN:
			rate        = (adsr2 >> 6) & 0x7F;
			increase    = !((adsr2 >> 14) & 1);       // bit14: 0=increase 1=decrease
			exponential = (adsr2 >> 15) & 1;
			break;
		case ENV_RELEASE:
		default:
			rate        = (adsr2 & 0x1F) << 2;        // 5-bit, *4
			increase    = 0;
			exponential = (adsr2 >> 5) & 1;
			break;
		}

		int cycles, step;
		adsr_step_params(rate, increase, exponential, voice->envLevel, &cycles, &step);

		if (voice->envCounter < cycles)
			break;

		voice->envCounter -= cycles;
		voice->envLevel += step;

		if (voice->envLevel > ADSR_MAX) voice->envLevel = ADSR_MAX;
		if (voice->envLevel < 0)        voice->envLevel = 0;

		switch (voice->envPhase)
		{
		case ENV_ATTACK:
			if (voice->envLevel >= ADSR_MAX)
				voice->envPhase = ENV_DECAY;
			break;
		case ENV_DECAY:
			if (voice->envLevel <= sustainLevel)
			{
				voice->envLevel = sustainLevel;
				voice->envPhase = ENV_SUSTAIN;
			}
			break;
		case ENV_SUSTAIN:
			// holds until key-off; a decreasing sustain naturally rings out
			break;
		case ENV_RELEASE:
			if (voice->envLevel <= 0)
				voice->envPhase = ENV_OFF;
			break;
		}
	}

	return voice->envLevel;
}

void PsyX_SPUAL_Update()
{
	if (!g_spuInit || !g_SpuAdsrEnabled)
		return;

	static u_int s_lastTicks = 0;
	u_int now = SDL_GetTicks();
	if (s_lastTicks == 0)
		s_lastTicks = now;

	int dtMs = (int)(now - s_lastTicks);
	s_lastTicks = now;

	if (dtMs <= 0)
		return;
	if (dtMs > 100)
		dtMs = 100; // clamp hitches so the envelope can't jump seconds at once

	int samples = (dtMs * 44100) / 1000;

	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];

		if (!voice->hasEnvelope || voice->envPhase == ENV_OFF)
			continue;

		ALuint alSource = voice->alSource;
		if (alSource == AL_NONE)
			continue;

		// A non-looping voice whose buffer ran out (never keyed off) must not
		// stay in SUSTAIN forever — the phase-based key status would read it
		// busy for good and starve the voice allocator. Real SPU hardware
		// forces the envelope to zero at sample end (END+MUTE); mirror it.
		{
			ALint state = AL_STOPPED;
			alGetSourcei(alSource, AL_SOURCE_STATE, &state);
			if (state != AL_PLAYING && state != AL_PAUSED)
			{
				voice->envLevel    = 0;
				voice->envPhase    = ENV_OFF;
				voice->hasEnvelope = 0;
				continue;
			}
		}

		int level = EnvelopeAdvance(voice, samples);
		float env = (float)level / (float)ADSR_MAX;

		alSourcef(alSource, AL_GAIN, voice->baseGain * env);

		// Since key-off keeps AL_LOOPING (the SPU loops the sustain block
		// through release), a near-infinite programmed release would ring a
		// looped source forever. Nothing musical needs more than a few
		// seconds; cap the tail.
		if (voice->envPhase == ENV_RELEASE && (now - voice->relStartMs) > 10000)
		{
			voice->envLevel = 0;
			voice->envPhase = ENV_OFF;
		}

		if (voice->envPhase == ENV_OFF)
		{
			// release finished: silence the (looping) source for real
			alSourceStop(alSource);
			voice->hasEnvelope = 0;
		}
	}
	SDL_UnlockMutex(g_SpuMutex);
}

int PsyX_SPUAL_SetMute(int on_off)
{
	int old_state = g_SPUMuted;
	g_SPUMuted = on_off;
	return old_state;
}

void PsyX_SPUAL_GetVoiceVolume(int vNum, short* volL, short* volR)
{
	if (volL)
		*volL = g_SpuVoices[vNum].attr.volume.left;

	if (volR)
		*volR = g_SpuVoices[vNum].attr.volume.right;
}

void PsyX_SPUAL_GetVoicePitch(int vNum, u_short* pitch)
{
	*pitch = g_SpuVoices[vNum].attr.pitch;
}

void PsyX_SPUAL_GetVoiceAttr(SpuVoiceAttr* psxAttrib)
{
	/* The game uses this to read back the current pitch (and other
	 * attributes) for an already-keyed voice; libsd's
	 * Sd_PlaySfx → Sd_SfxAttributesUpdate path stashes the result in
	 * g_AudioPlayingPitchList[voiceIdx] and re-uses it on every per-frame
	 * SfxAttributesUpdate. If we leave it as a no-op, the stashed pitch
	 * is 0, so the per-frame SetVoiceAttr passes pitch=0 and our handler
	 * pauses the OpenAL source — the voice plays for one frame and goes
	 * silent (manifested as the radio static "playing for a second then
	 * inaudible").
	 *
	 * .voice on input is a single-bit value (the bit corresponding to the
	 * voice index). Find that bit, then copy the cached attr fields into
	 * the caller's struct. */
	if (!g_spuInit || !psxAttrib)
		return;

	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((psxAttrib->voice & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		/* Preserve caller's .voice and .mask (mask is invalid on Get,
		 * but the call site may inspect or pass it on). Don't overwrite
		 * those — only fill the read-out fields. */
		psxAttrib->volume     = voice->attr.volume;
		psxAttrib->volmode    = voice->attr.volmode;
		psxAttrib->volumex    = voice->attr.volume;  /* current vol = set vol */
		psxAttrib->pitch      = voice->attr.pitch;
		psxAttrib->note       = voice->attr.note;
		psxAttrib->sample_note = voice->attr.sample_note;
		psxAttrib->envx       = 0; /* current envelope unused on PC */
		psxAttrib->addr       = voice->attr.addr;
		psxAttrib->loop_addr  = voice->attr.loop_addr;
		psxAttrib->a_mode     = voice->attr.a_mode;
		psxAttrib->s_mode     = voice->attr.s_mode;
		psxAttrib->r_mode     = voice->attr.r_mode;
		psxAttrib->ar         = voice->attr.ar;
		psxAttrib->dr         = voice->attr.dr;
		psxAttrib->sr         = voice->attr.sr;
		psxAttrib->rr         = voice->attr.rr;
		psxAttrib->sl         = voice->attr.sl;
		psxAttrib->adsr1      = voice->attr.adsr1;
		psxAttrib->adsr2      = voice->attr.adsr2;
		break; /* only one voice per Get call */
	}

	SDL_UnlockMutex(g_SpuMutex);
}

PSX_API_EXPORT void PsyX_SPUAL_SetVoiceAzimuth(int voiceIdx, int azimuthQ12)
{
	if (!g_spuInit || voiceIdx < 0 || voiceIdx >= s_spuVoiceCount)
		return;

	SDL_LockMutex(g_SpuMutex);
	g_SpuVoices[voiceIdx].azimuthQ12   = azimuthQ12;
	g_SpuVoices[voiceIdx].azimuthValid = 1;
	SDL_UnlockMutex(g_SpuMutex);
}

void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* psxAttrib)
{
	if (!g_spuInit)
		return;

	const float STEREO_FACTOR = 3.0f;
	
	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((psxAttrib->voice & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		if (alSource == AL_NONE)
			continue;

		// update sample
		if ((psxAttrib->mask & SPU_VOICE_WDSA) || (psxAttrib->mask & SPU_VOICE_LSAX))
		{
			if (psxAttrib->mask & SPU_VOICE_WDSA)
			{
				if (voice->attr.addr != psxAttrib->addr)
					voice->sampledirty++;

				voice->attr.addr = psxAttrib->addr;

				// A start-address write accompanies every key-on: claim the
				// azimuth the game stashed for the upcoming sound, or clear
				// stale spatial state when there is none (voice reuse — a
				// sequencer note must not inherit the last SFX's position).
				// Claim only on the arming thread and within the TTL: the
				// arm -> key-on chain is synchronous, so an intr-thread BGM
				// note or an expired orphan (failed key-on) never matches.
				if (s_nextAzimuthValid &&
				    s_nextAzimuthThread == SDL_ThreadID() &&
				    (Uint32)(SDL_GetTicks() - s_nextAzimuthMs) <= AZIMUTH_STASH_TTL_MS)
				{
					voice->azimuthValid = 1;
					voice->azimuthQ12   = s_nextAzimuthQ12;
					s_nextAzimuthValid  = 0;
				}
				else
				{
					voice->azimuthValid = 0;
				}
			}

			if (psxAttrib->mask & SPU_VOICE_LSAX)
			{
				if(voice->attr.loop_addr != psxAttrib->loop_addr)
					voice->sampledirty++;

				voice->attr.loop_addr = psxAttrib->loop_addr;
			}
		}

		// update volume
		if ((psxAttrib->mask & SPU_VOICE_VOLL) || (psxAttrib->mask & SPU_VOICE_VOLR))
		{
			if (psxAttrib->mask & SPU_VOICE_VOLL)
				voice->attr.volume.left = psxAttrib->volume.left;

			if (psxAttrib->mask & SPU_VOICE_VOLR)
				voice->attr.volume.right = psxAttrib->volume.right;

			// libsd negates volume.right for "wide stereo" (diffuse ambience)
			// channels — KDT CC 0x0F -> wide_flag_21. Latch it from the signs
			// while both sides are live; (0,0) keeps the last state so a note
			// fading to silence doesn't snap back to the front stage.
			if (voice->attr.volume.left > 0 && voice->attr.volume.right < 0)
				voice->isWide = 1;
			else if (voice->attr.volume.left != 0 || voice->attr.volume.right != 0)
				voice->isWide = 0;

			// PSX direct-mode voice volume is signed: negative = phase-inverted
			// playback at |vol| amplitude (the wide-stereo trick above). A mono
			// OpenAL source can't invert one channel, and averaging signed gains
			// cancels L + (-L) to 0 — take magnitudes so wide voices keep their
			// loudness.
			float left_gain = fabsf((float)(voice->attr.volume.left)) / (float)(16384);
			float right_gain = fabsf((float)(voice->attr.volume.right)) / (float)(16384);

			if(left_gain > 1.0f)
				left_gain = 1.0f;

			if(right_gain > 1.0f)
				right_gain = 1.0f;

			if (voice->azimuthValid)
			{
				// True 3D: game code recovered the emitter's camera-relative
				// azimuth before collapsing it to L/R balance — place on the
				// full circle (rears included on surround layouts). Gain uses
				// the louder side: the balance attenuation is already baked
				// into L/R and AL panning would apply it a second time.
				float az = (float)voice->azimuthQ12 * (float)(M_PI / 2048.0);
				alSource3f(alSource, AL_POSITION, sinf(az), 0.0f, -cosf(az));

				voice->baseGain = (left_gain > right_gain) ? left_gain : right_gain;
			}
			else
			{
				float pan = (acosf(left_gain) + asinf(right_gain)) / ((float)(M_PI)); // average angle in [0,1]
				pan = 2.0f * pan - 1.0f; // convert to [-1, 1]
				pan = pan * 0.5f; // 0.5 = sin(30') for a +/- 30 degree arc

				// Wide (diffuse) voices belong on the surrounds when the
				// layout has them: mirror the frontal arc behind the listener.
				// On stereo output this is a no-op (front placement), keeping
				// the pre-surround mix byte-identical.
				float z = sqrtf(1.0f - pan * pan);
				if (s_surroundActive && voice->isWide)
					z = -z;
				alSource3f(alSource, AL_POSITION, pan * STEREO_FACTOR, 0, -z);

				voice->baseGain = (left_gain + right_gain) * 0.5f;
			}

			// While an envelope is running it owns the source gain (the tick
			// applies baseGain*level); otherwise apply the volume directly.
			float g = voice->baseGain;
			if (voice->hasEnvelope && voice->envPhase != ENV_OFF)
				g *= (float)voice->envLevel / (float)ADSR_MAX;
			alSourcef(alSource, AL_GAIN, g);
		}

		// Capture the raw ADSR registers so SetKey can run the envelope.
		if (psxAttrib->mask & SPU_VOICE_ADSR_ADSR1)
			voice->attr.adsr1 = psxAttrib->adsr1;
		if (psxAttrib->mask & SPU_VOICE_ADSR_ADSR2)
			voice->attr.adsr2 = psxAttrib->adsr2;

		// update pitch
		if (psxAttrib->mask & SPU_VOICE_PITCH)
		{
			ALint state;
			alGetSourcei(alSource, AL_SOURCE_STATE, &state);

			if (psxAttrib->pitch == 0 && state == AL_PLAYING)
				alSourcePause(alSource);
			else if (voice->attr.pitch == 0 && state == AL_PAUSED)
				alSourcePlay(alSource);

			voice->attr.pitch = psxAttrib->pitch;

			const float pitchBase = (voice->overridePitchBase > 0) ? (float)voice->overridePitchBase : 4096.0f;
			const float pitch = (float)(voice->attr.pitch) / pitchBase;
			alSourcef(alSource, AL_PITCH, pitch);
		}
		
		// TODO: ADSR and other stuff
	}
	SDL_UnlockMutex(g_SpuMutex);
}

void PsyX_SPUAL_SetKey(int on_off, u_int voice_bit)
{
	if (!g_spuInit)
		return;

	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((voice_bit & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		if (alSource == AL_NONE)
			continue;

		if (on_off && !g_SPUMuted)
		{
			alSourceStop(alSource);
			UpdateVoiceSample(voice);

			// Engage the ADSR envelope for ANY voice that programmed one —
			// the SPU applies it regardless of the sample's loop flag. Gating
			// on looping voices made every one-shot sequencer note (bells,
			// chimes, most melodic instruments) stop dead at key-off instead
			// of ringing out through release, and skipped their decay/sustain
			// shaping ("sudden stops", chimes cutting abruptly vs PSX).
			if (g_SpuAdsrEnabled && (voice->attr.adsr1 || voice->attr.adsr2))
			{
				voice->hasEnvelope = 1;
				EnvelopeKeyOn(voice);
				// Pre-charge ~2ms of envelope (the tick thread's worst-case
				// latency) so instant attacks (rate 0, MAX within 3 samples)
				// keep their transient punch — a gunshot must not start
				// silent. Slow ramps lose only an inaudible 2ms of curve.
				int lvl = EnvelopeAdvance(voice, 44100 / 500);
				alSourcef(alSource, AL_GAIN, voice->baseGain * ((float)lvl / (float)ADSR_MAX));
			}
			else
			{
				voice->hasEnvelope = 0;
				voice->envPhase = ENV_OFF;
			}

			alSourcePlay(alSource);
		}
		else
		{
			if (g_SpuAdsrEnabled && voice->hasEnvelope && voice->envPhase != ENV_OFF)
			{
				// Enter the release phase and let it ring out; the tick stops
				// the source once the envelope reaches zero. AL_LOOPING is
				// deliberately KEPT: the SPU keeps looping a sample's sustain
				// block while release fades it — clearing it here ended
				// looped-sustain instruments at their loop-block boundary,
				// mid-release (the sewer "moaning" that never faded out).
				// Pathological tails are capped by the tick via relStartMs.
				EnvelopeKeyOff(voice);
				voice->relStartMs = SDL_GetTicks();
			}
			else
			{
				// No envelope: PSX-less hard stop, and clear AL_LOOPING so a
				// Repeat=1 sample can't keep repeating under a stopped voice.
				if (voice->looping)
				{
					alSourcei(alSource, AL_LOOPING, AL_FALSE);
					voice->looping = 0;
				}
				alSourceStop(alSource);
			}
		}
	}
	SDL_UnlockMutex(g_SpuMutex);
}

// Stop every LOOPING voice, and only those.
//
// The PSX ran a blanket SfxStop (SD_Call(22)) when a scene tore down its audio.
// That blanket stop is disabled on PC because OpenAL cuts a sample dead where
// PSX ADPCM decays naturally, so it audibly chopped the death scream and the
// pickup voice mid-word. Removing it outright, though, left nothing that ends
// an ambient LOOP: a looping bed (the bridge wind) survived a quickload and
// played forever, because a loop is by definition the one voice that will never
// finish on its own.
//
// Looping voices are also exactly the ones with no decay tail to lose, so
// stopping only those ends the stuck ambience without reintroducing the
// truncation that motivated disabling the blanket stop.
extern "C" void Pc_SpuStopLoopingVoices(void)
{
	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];

		if (!voice->looping)
			continue;

		alSourcei(voice->alSource, AL_LOOPING, AL_FALSE);
		alSourceStop(voice->alSource);

		voice->looping  = 0;
		voice->envPhase = ENV_OFF;
	}

	SDL_UnlockMutex(g_SpuMutex);
}

// PSX SpuGetKeyStatus is a 4-state value, and the SH sound driver (libsd)
// depends on the full distinction — collapsing it to on/off silently breaks
// voice allocation:
//   SPU_ON         keyed on, envelope live (attack/decay/sustain, level > 0)
//   SPU_ON_ENV_OFF keyed on, but the envelope has decayed to 0 while held
//   SPU_OFF_ENV_ON KEYED OFF but the release tail is still ringing out
//   SPU_OFF        fully idle (release finished, or never keyed)
//
// The allocator voice_check() (smf_io.c) makes two passes: pass 1 grabs only a
// SPU_OFF (truly-free) voice; a still-releasing SPU_OFF_ENV_ON voice is left
// alone and only stolen in pass 2 when nothing free remains. That is exactly
// what lets a release tail ring INTO the next note instead of being cut. The
// old code reported every releasing voice as SPU_OFF, so pass 1 stole it and
// rr_off-cut its tail even with the 24-voice pool half empty — the tester's
// "BGM fades then abruptly gets cut off." SPU_ON_ENV_OFF likewise lets
// SdAutoKeyOffCheck reclaim a decayed-but-held voice instead of it reading busy
// forever. This does NOT reintroduce the old end-of-dialogue freeze: every
// libsd "wait for silence" spin (sound_seq_off/sound_off/SdAutoKeyOffCheck)
// exits on `stat == SPU_OFF_ENV_ON || stat == SPU_OFF`, and the key-on scans
// (SdVoKeyOn/SdUtKeyOn) iterate voices rather than spinning on one — none block
// on the release state.
static int spual_voice_key_status(const SPUALVoice* voice)
{
	switch (voice->envPhase)
	{
	case ENV_ATTACK:
	case ENV_DECAY:
		return SPU_ON;
	case ENV_SUSTAIN:
		return voice->envLevel > 0 ? SPU_ON : SPU_ON_ENV_OFF;
	case ENV_RELEASE:
		return voice->envLevel > 0 ? SPU_OFF_ENV_ON : SPU_OFF;
	case ENV_OFF:
	default:
		return SPU_OFF;
	}
}

int PsyX_SPUAL_GetKeyStatus(u_int voice_bit)
{
	int status = SPU_OFF;
	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if (voice_bit != SPU_VOICECH(i))
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		if (alSource == AL_NONE)
			break; // SpuOff?

		if (g_SpuAdsrEnabled && voice->hasEnvelope)
		{
			status = spual_voice_key_status(voice);
		}
		else
		{
			int state = AL_STOPPED;
			alGetSourcei(alSource, AL_SOURCE_STATE, &state);
			status = (state == AL_PLAYING) ? SPU_ON : SPU_OFF;
		}
		break;
	}

	SDL_UnlockMutex(g_SpuMutex);

	return status;
}

void PsyX_SPUAL_GetAllKeysStatus(char* status)
{
	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;
		if (alSource == AL_NONE)
		{
			status[i] = 0; // SpuOff?
			continue;
		}

		if (g_SpuAdsrEnabled && voice->hasEnvelope)
		{
			status[i] = spual_voice_key_status(voice);
			continue;
		}

		int state;
		alGetSourcei(alSource, AL_SOURCE_STATE, &state);
		status[i] = (state == AL_PLAYING) ? SPU_ON : SPU_OFF;
	}
	SDL_UnlockMutex(g_SpuMutex);
}

int PsyX_SPUAL_SetReverb(int on_off)
{
	int old_state = g_enableSPUReverb;
	g_enableSPUReverb = on_off;

	if (!g_spuInit)
		return old_state;
#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)
	// switch if needed
	if (g_ALEffectsSupported && old_state != g_enableSPUReverb)
	{
		if (g_enableSPUReverb)
		{
			alAuxiliaryEffectSloti(g_ALEffectSlots[g_currEffectSlotIdx], AL_EFFECTSLOT_EFFECT, g_nAlReverbEffect);
		}
		else
		{
			g_currEffectSlotIdx = 0;
			alAuxiliaryEffectSloti(g_ALEffectSlots[0], AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
			alAuxiliaryEffectSloti(g_ALEffectSlots[1], AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
		}
	}
#endif
	return old_state;
}

int PsyX_SPUAL_GetReverbState()
{
	return g_enableSPUReverb;
}

/* Reverb depth (wet level), from SpuSetReverbModeParam / SpuSetReverbDepth.
 * maskL/maskR say which sides the caller set; the other keeps its value
 * (matches the PSX per-side mask semantics). Called from BOTH the game
 * thread (bank loads) and the interrupt thread (the per-tick replay ramp). */
void PsyX_SPUAL_SetReverbDepthMasked(int maskL, int maskR, short depthL, short depthR)
{
	SDL_LockMutex(g_SpuMutex);
	if (maskL) g_reverbDepthL = depthL;
	if (maskR) g_reverbDepthR = depthR;
	ApplyReverbWet();
	SDL_UnlockMutex(g_SpuMutex);
}

/* Reverb mode (preset type). SH1 only ever sets mode 1 (Room) at boot and the
 * current effect parameters were tuned for this game, so the mode is stored
 * for readback but does not retune the effect. */
int PsyX_SPUAL_SetReverbMode(int mode)
{
	int old = g_reverbMode;
	g_reverbMode = mode;
	return old;
}

/* Console calibration knob (`revscale`): scales depth -> wet mapping. */
void PsyX_SPUAL_SetReverbDepthScale(float scale)
{
	SDL_LockMutex(g_SpuMutex);
	g_SpuReverbDepthScale = scale;
	ApplyReverbWet();
	SDL_UnlockMutex(g_SpuMutex);
}

float PsyX_SPUAL_GetReverbDepthScale(void)
{
	return g_SpuReverbDepthScale;
}

u_int PsyX_SPUAL_SetReverbVoice(int on_off, u_int voice_bit)
{
	if (!g_spuInit)
		return 0;

	if (!g_ALEffectsSupported)
		return 0;

	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((voice_bit & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;
		if (alSource == AL_NONE)
			continue;

		voice->reverb = on_off > 0;
#ifndef __EMSCRIPTEN__
		if (on_off)
			alSource3i(alSource, AL_AUXILIARY_SEND_FILTER, g_ALEffectSlots[g_currEffectSlotIdx], 0, AL_FILTER_NULL);
		else
			alSource3i(alSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
#endif // __EMSCRIPTEN__
	}

	SDL_UnlockMutex(g_SpuMutex);

	return 0;
}

u_int PsyX_SPUAL_GetReverbVoice()
{
	u_int bits = 0;
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		if (voice->reverb)
			bits |= SPU_KEYCH(i);
	}
	return bits;
}