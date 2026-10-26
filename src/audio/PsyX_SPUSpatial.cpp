#include "PsyX_SPUSpatial.h"
#include "PsyX_SPUCore.h"
#include "../PsyX_main.h"

#include <SDL.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <math.h>
#include <string.h>
#include <vector>

/* Output-mode tokens come from a current OpenAL Soft alext.h; bare OpenAL 1.1
 * headers (macOS system framework) lack them, so define fallbacks. Every use
 * is still gated on a runtime extension check. Kept identical to the copies in
 * PsyX_SPUAL.cpp -- the two renderers must agree on what a layout id means. */
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

/* PsyX_SPUSoftware.cpp: the emitter bearing the game stashed for a voice.
 * extern "C" to match its definition inside that file export block -- the
 * mangled name would not link. */
extern "C" int PsyX_SPUSoftware_VoiceAzimuth(int voiceIdx, int* azQ12);

namespace
{
using PsyX::SPUCore;

/* One render block per queued buffer. 512 frames is 11.6ms at 44100, so the
 * four-deep queue below buffers about 46ms: enough to ride out a scheduling
 * hiccup without adding latency anyone notices on a menu blip. */
const int kBlockFrames = 512;
const int kQueueDepth  = 4;
const int kRate        = 44100;

/* Dry buses across the FRONT arc only. The PSX image is stereo, so there is no
 * such thing as a voice the game meant to place behind you, and spreading the
 * dry signal into the rears would invent information the mix never had. Five
 * directions is enough that a hard-panned voice lands convincingly. */
const float kDryAzimuthsDeg[] = { -90.0f, -45.0f, 0.0f, 45.0f, 90.0f };
const int   kDryBuses = (int)(sizeof(kDryAzimuthsDeg) / sizeof(kDryAzimuthsDeg[0]));

/* The reverb return goes BEHIND the listener. It is the one genuinely ambient
 * part of the mix, and it is what makes a surround layout worth having here. */
const float kWetAzimuthsDeg[] = { -135.0f, 135.0f };
const int   kWetBuses = 2;

/* CD/XA (music and voice tracks) is an ordinary stereo bed at the usual angles. */
const float kCdAzimuthsDeg[] = { -30.0f, 30.0f };
const int   kCdBuses = 2;

const int kTotalBuses = kDryBuses + kWetBuses + kCdBuses;

struct Bus
{
    ALuint source;
    ALuint buffers[kQueueDepth];
    /* Buffers not currently queued on the source. alBufferData on a QUEUED
     * buffer fails and leaves it holding the previous block, so the only safe
     * refill target is a name alSourceUnqueueBuffers actually handed back.
     * Indexing by the queued COUNT instead was the "every sound plays over
     * itself" bug: that index usually names a still-queued buffer, whose stale
     * audio then gets re-queued (reported on 5.1 2026-10-01). */
    ALuint free[kQueueDepth];
    int    freeCount;
    std::vector<int16_t> scratch;
};

/* What the device actually gave us, in the shared 0..5 layout ids (0 auto,
 * 1 stereo, 2 quad, 3 5.1, 4 7.1, 5 hrtf). The console reports these, so they
 * must be measured rather than echoed back from the request: asking for 5.1 on
 * a stereo endpoint silently degrades. */
/* Direct speaker output.
 *
 * The buses below sit at FIXED azimuths that are the real speaker positions,
 * so handing them to OpenAL as positioned mono sources asks the panner to
 * solve a problem that is already solved. OpenAL Soft mixes through an
 * ambisonic bus and decodes to the layout, and that decode puts every point
 * source into several speakers at once. With nine buses carrying correlated
 * content (a voice is crossfaded across two of them by design) the same signal
 * reaches most of the room at different gains, which combs: thin, tinny, and
 * seemingly coming from everywhere. Reported on 5.1 2026-10-01.
 *
 * So on a layout with real speakers the buses are written straight into their
 * own channels of one multichannel buffer. OpenAL plays a multichannel buffer
 * to the matching speakers untouched -- no panning, no decode, no smear.
 * Stereo and HRTF keep the positional path: there the panner is doing the
 * work we actually want, and HRTF has nothing to do without it. */
int    g_directOut      = 0;
int    g_directChannels = 0;
ALenum g_directFormat   = 0;
ALuint g_directSource   = 0;
ALuint g_directBuffers[kQueueDepth];
ALuint g_directFree[kQueueDepth];
int    g_directFreeCount = 0;
std::vector<int16_t> g_directScratch;

int g_achievedSpeakers = 1;
int g_surroundActive   = 0;

int AlcOutputModeToSpeakers(int alcMode)
{
    switch (alcMode)
    {
    case ALC_QUAD_SOFT:         return 2;
    case ALC_SURROUND_5_1_SOFT:
    case ALC_SURROUND_6_1_SOFT: return 3;
    case ALC_SURROUND_7_1_SOFT: return 4;
    case ALC_STEREO_HRTF_SOFT:  return 5;
    }
    return 1;
}

int SpeakersToAlcOutputMode(int spk)
{
    switch (spk)
    {
    case 1: return ALC_STEREO_BASIC_SOFT;
    case 2: return ALC_QUAD_SOFT;
    case 3: return ALC_SURROUND_5_1_SOFT;
    case 4: return ALC_SURROUND_7_1_SOFT;
    case 5: return ALC_STEREO_HRTF_SOFT;
    }
    return ALC_ANY_SOFT;
}

ALCdevice*  g_dev = NULL;
ALCcontext* g_ctx = NULL;
ALCcontext* g_prevCtx = NULL;
Bus         g_bus[kTotalBuses];
SPUCore*    g_core = NULL;
SDL_mutex*  g_coreMutex = NULL;
SDL_Thread* g_thread = NULL;
volatile int g_running = 0;
int          g_active = 0;

void (*g_xaPump)(void* user, int frames) = NULL;
void*  g_xaPumpUser = NULL;

std::vector<int16_t> g_voiceBuf[PsyX::kNumVoices];
std::vector<int16_t> g_wetBuf;
std::vector<int16_t> g_cdBuf;
std::vector<float>   g_busAccum[kTotalBuses];

void PlaceSource(ALuint src, float azimuthDeg)
{
    const float az = azimuthDeg * 3.14159265358979f / 180.0f;
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(src, AL_POSITION, sinf(az), 0.0f, -cosf(az));
    alSourcef(src, AL_ROLLOFF_FACTOR, 0.0f); /* placement only, no distance law */
    alSourcef(src, AL_GAIN, 1.0f);
}

/* PSX voice volumes -> where that voice sits across the front arc.
 *
 * The hardware pans by giving a voice independent left and right volumes, so
 * the balance between them IS the azimuth and their magnitude is the level.
 * Everything stays within +-90 degrees because that is the whole of a stereo
 * image: a hard-left voice reaches the left bus and no further. */
void VoiceAzimuthGain(int32_t volL, int32_t volR, float* azDeg, float* gain)
{
    const float l = fabsf((float)volL);
    const float r = fabsf((float)volR);
    const float sum = l + r;

    if (sum <= 0.0f)
    {
        *azDeg = 0.0f;
        *gain  = 0.0f;
        return;
    }

    *azDeg = ((r - l) / sum) * 90.0f;
    *gain  = (l > r ? l : r) / 32767.0f;
    if (*gain > 1.0f)
        *gain = 1.0f;
}

/* Spread one voice across the two buses either side of it, so a voice sweeping
 * across the stage crossfades instead of stepping between speakers. */
void AccumulateVoice(const int16_t* mono, int frames, float azDeg, float gain)
{
    if (gain <= 0.0f)
        return;

    int lo = 0;
    while (lo < kDryBuses - 2 && kDryAzimuthsDeg[lo + 1] < azDeg)
        ++lo;
    const int hi = lo + 1;

    const float span = kDryAzimuthsDeg[hi] - kDryAzimuthsDeg[lo];
    float t = span > 0.0f ? (azDeg - kDryAzimuthsDeg[lo]) / span : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    /* Constant-power crossfade: a linear one dips about 3dB as a voice passes
     * between two buses, which is audible on a slow pan. */
    const float gLo = cosf(t * 1.57079632679f) * gain;
    const float gHi = sinf(t * 1.57079632679f) * gain;

    float* dstLo = g_busAccum[lo].data();
    float* dstHi = g_busAccum[hi].data();
    for (int i = 0; i < frames; ++i)
    {
        const float s = (float)mono[i];
        dstLo[i] += s * gLo;
        dstHi[i] += s * gHi;
    }
}

void AccumulateStereoBed(const int16_t* interleaved, int frames, int busL, int busR)
{
    float* dl = g_busAccum[busL].data();
    float* dr = g_busAccum[busR].data();
    for (int i = 0; i < frames; ++i)
    {
        dl[i] += (float)interleaved[i * 2 + 0];
        dr[i] += (float)interleaved[i * 2 + 1];
    }
}

/* The real speaker ring, per layout, sorted by azimuth. In direct mode a
 * voice is panned between its two neighbouring SPEAKERS rather than across
 * the front-arc buses: the buses only span +-90, so nothing could ever be
 * placed behind the listener, and that is most of what positional audio is
 * for. LFE is not on the ring -- nothing here is bass-managed. */
struct Spk { float az; int ch; };

/* FL FR BL BR */
const Spk kRingQuad[] = { { -135.0f, 2 }, { -45.0f, 0 }, { 45.0f, 1 }, { 135.0f, 3 } };
/* FL FR FC LFE BL BR */
const Spk kRing51[]   = { { -110.0f, 4 }, { -30.0f, 0 }, { 0.0f, 2 }, { 30.0f, 1 }, { 110.0f, 5 } };
/* FL FR FC LFE BL BR SL SR */
const Spk kRing71[]   = { { -150.0f, 4 }, { -90.0f, 6 }, { -30.0f, 0 }, { 0.0f, 2 },
                          { 30.0f, 1 }, { 90.0f, 7 }, { 150.0f, 5 } };

const Spk* g_ring      = NULL;
int        g_ringCount = 0;

/* Direct mode accumulates straight into speakers. */
std::vector<float> g_spkAccum[8];

void PanToSpeakers(const int16_t* mono, int frames, float azDeg, float gain)
{
    if (gain <= 0.0f || g_ringCount <= 0)
        return;

    while (azDeg < -180.0f) azDeg += 360.0f;
    while (azDeg >  180.0f) azDeg -= 360.0f;

    /* Neighbouring pair on the ring, wrapping across the back. */
    int   lo = g_ringCount - 1;
    int   hi = 0;
    float loAz = g_ring[lo].az - 360.0f;
    float hiAz = g_ring[hi].az;
    for (int i = 0; i < g_ringCount - 1; ++i)
    {
        if (azDeg >= g_ring[i].az && azDeg <= g_ring[i + 1].az)
        {
            lo = i; hi = i + 1;
            loAz = g_ring[lo].az; hiAz = g_ring[hi].az;
            break;
        }
    }
    if (azDeg > g_ring[g_ringCount - 1].az)
    {
        lo = g_ringCount - 1; hi = 0;
        loAz = g_ring[lo].az; hiAz = g_ring[hi].az + 360.0f;
    }

    const float span = hiAz - loAz;
    float t = span > 0.0f ? (azDeg - loAz) / span : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    const float gLo = cosf(t * 1.57079632679f) * gain;
    const float gHi = sinf(t * 1.57079632679f) * gain;
    float* dLo = g_spkAccum[g_ring[lo].ch].data();
    float* dHi = g_spkAccum[g_ring[hi].ch].data();
    for (int i = 0; i < frames; ++i)
    {
        const float v = (float)mono[i];
        dLo[i] += v * gLo;
        dHi[i] += v * gHi;
    }
}

void AddToSpeaker(const int16_t* interleaved, int frames, int stride, int offset, int ch)
{
    float* d = g_spkAccum[ch].data();
    for (int i = 0; i < frames; ++i)
        d[i] += (float)interleaved[i * stride + offset];
}

/* Bus indices, named so the mapping below reads as the layout it describes. */
enum
{
    kBusL90 = 0, kBusL45, kBusC, kBusR45, kBusR90,
    kBusWetL, kBusWetR,
    kBusCdL, kBusCdR
};

/* -3dB, for a bus that has to land between two speakers. */
const float kHalfPower = 0.70710678f;

void MixDirect(float master)
{
    const int ch  = g_directChannels;
    int16_t*  dst = g_directScratch.data();

    for (int i = 0; i < kBlockFrames; ++i)
    {
        for (int k = 0; k < ch; ++k)
        {
            float v = g_spkAccum[k][i] * master;
            if (v > 32767.0f)  v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            dst[i * ch + k] = (int16_t)v;
        }
    }
}

void RenderBlock(void)
{
    SPUCore::SplitOutput split;
    memset(&split, 0, sizeof(split));

    for (int v = 0; v < PsyX::kNumVoices; ++v)
    {
        g_voiceBuf[v].assign(kBlockFrames, 0);
        split.voiceMono[v] = g_voiceBuf[v].data();
    }
    g_wetBuf.assign((size_t)kBlockFrames * 2, 0);
    g_cdBuf.assign((size_t)kBlockFrames * 2, 0);
    split.wetLR = g_wetBuf.data();
    split.cdLR  = g_cdBuf.data();

    if (g_xaPump)
        g_xaPump(g_xaPumpUser, kBlockFrames);

    SDL_LockMutex(g_coreMutex);
    /* No stereo destination: that downmix is exactly what this replaces. */
    g_core->RenderFrames(NULL, kBlockFrames, &split);
    SDL_UnlockMutex(g_coreMutex);

    if (g_directOut)
    {
        for (int k = 0; k < g_directChannels; ++k)
            memset(g_spkAccum[k].data(), 0, sizeof(float) * (size_t)kBlockFrames);

        for (int v = 0; v < PsyX::kNumVoices; ++v)
        {
            float az, gain;
            int   azQ12 = 0;

            VoiceAzimuthGain(split.panL[v], split.panR[v], &az, &gain);
            /* The emitter true bearing when the game supplied one: the L/R
             * balance it would otherwise be derived from only spans the front
             * arc. Gain still comes from the louder side, since the balance
             * attenuation is already baked into those volumes. */
            if (PsyX_SPUSoftware_VoiceAzimuth(v, &azQ12))
                az = (float)azQ12 * (360.0f / 4096.0f);

            PanToSpeakers(g_voiceBuf[v].data(), kBlockFrames, az, gain);
        }

        /* Reverb return behind the listener, CD/XA as a front bed. */
        {
            const int bl = (g_directChannels == 4) ? 2 : 4;
            const int br = (g_directChannels == 4) ? 3 : 5;
            AddToSpeaker(g_wetBuf.data(), kBlockFrames, 2, 0, bl);
            AddToSpeaker(g_wetBuf.data(), kBlockFrames, 2, 1, br);
            AddToSpeaker(g_cdBuf.data(),  kBlockFrames, 2, 0, 0);
            AddToSpeaker(g_cdBuf.data(),  kBlockFrames, 2, 1, 1);
        }
    }
    else
    {
    for (int b = 0; b < kTotalBuses; ++b)
        memset(g_busAccum[b].data(), 0, sizeof(float) * (size_t)kBlockFrames);

    for (int v = 0; v < PsyX::kNumVoices; ++v)
    {
        float az, gain;
        VoiceAzimuthGain(split.panL[v], split.panR[v], &az, &gain);
        AccumulateVoice(g_voiceBuf[v].data(), kBlockFrames, az, gain);
    }

    AccumulateStereoBed(g_wetBuf.data(), kBlockFrames, kDryBuses, kDryBuses + 1);
    AccumulateStereoBed(g_cdBuf.data(), kBlockFrames, kDryBuses + kWetBuses,
                        kDryBuses + kWetBuses + 1);
    }

    /* Master volume last, matching where the hardware applies it. */
    const float mvRaw = (float)(split.masterL > split.masterR ? split.masterL : split.masterR)
                      / 32767.0f;
    const float master = mvRaw <= 0.0f ? 0.0f : (mvRaw > 1.0f ? 1.0f : mvRaw);

    if (g_directOut)
    {
        MixDirect(master);
        return;
    }

    for (int b = 0; b < kTotalBuses; ++b)
    {
        const float* srcBuf = g_busAccum[b].data();
        int16_t* dst = g_bus[b].scratch.data();
        for (int i = 0; i < kBlockFrames; ++i)
        {
            float s = srcBuf[i] * master;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            dst[i] = (int16_t)s;
        }
    }
}

int SDLCALL PumpThread(void*)
{
    while (g_running)
    {
        if (g_directOut)
        {
            ALint processed = 0;

            alGetSourcei(g_directSource, AL_BUFFERS_PROCESSED, &processed);
            while (processed-- > 0)
            {
                ALuint done = 0;
                alSourceUnqueueBuffers(g_directSource, 1, &done);
                if (done && g_directFreeCount < kQueueDepth)
                    g_directFree[g_directFreeCount++] = done;
            }

            if (g_directFreeCount == 0)
            {
                SDL_Delay(2);
                continue;
            }

            RenderBlock();

            {
                ALuint buf   = g_directFree[--g_directFreeCount];
                ALint  state = 0;

                alBufferData(buf, g_directFormat, g_directScratch.data(),
                             (ALsizei)(kBlockFrames * g_directChannels * sizeof(int16_t)),
                             kRate);
                alSourceQueueBuffers(g_directSource, 1, &buf);
                alGetSourcei(g_directSource, AL_SOURCE_STATE, &state);
                if (state != AL_PLAYING)
                    alSourcePlay(g_directSource);
            }
            continue;
        }

        int freeMin = kQueueDepth;

        for (int b = 0; b < kTotalBuses; ++b)
        {
            ALint processed = 0;

            alGetSourcei(g_bus[b].source, AL_BUFFERS_PROCESSED, &processed);
            while (processed-- > 0)
            {
                ALuint done = 0;
                alSourceUnqueueBuffers(g_bus[b].source, 1, &done);
                if (done && g_bus[b].freeCount < kQueueDepth)
                    g_bus[b].free[g_bus[b].freeCount++] = done;
            }
            if (g_bus[b].freeCount < freeMin)
                freeMin = g_bus[b].freeCount;
        }

        if (freeMin == 0)
        {
            SDL_Delay(2);
            continue;
        }

        RenderBlock();

        /* Every bus is queued the same number of frames on the same pass, so
         * they consume at the same rate and stay locked together for the whole
         * session -- which is the reason for buses instead of 24 sources. */
        for (int b = 0; b < kTotalBuses; ++b)
        {
            ALuint buf   = g_bus[b].free[--g_bus[b].freeCount];
            ALint  state = 0;

            alBufferData(buf, AL_FORMAT_MONO16, g_bus[b].scratch.data(),
                         (ALsizei)(kBlockFrames * sizeof(int16_t)), kRate);
            alSourceQueueBuffers(g_bus[b].source, 1, &buf);
            alGetSourcei(g_bus[b].source, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING)
                alSourcePlay(g_bus[b].source);
        }
    }
    return 0;
}
} // namespace

bool PsyX_SPUSpatial_Start(PsyX::SPUCore* core, SDL_mutex* coreMutex, int speakerMode)
{
    if (g_active || !core)
        return false;

    g_dev = alcOpenDevice(NULL);
    if (!g_dev)
    {
        eprintwarn("[SPATIAL] no OpenAL device; staying on the stereo sink\n");
        return false;
    }

    /* An explicit layout is requested through ALC, exactly as the legacy
     * renderer does it. auto passes NO output-mode attribute on purpose: the
     * attribute unconditionally overrides the user alsoft.ini, so auto has to
     * stay out of the way and let the device report its own layout. */
    ALCint attrs[5];
    int    na = 0;
    attrs[na++] = ALC_FREQUENCY;
    attrs[na++] = kRate;
    if (speakerMode >= 1 && speakerMode <= 5 &&
        alcIsExtensionPresent(g_dev, "ALC_SOFT_output_mode"))
    {
        attrs[na++] = ALC_OUTPUT_MODE_SOFT;
        attrs[na++] = SpeakersToAlcOutputMode(speakerMode);
    }
    attrs[na] = 0;

    g_prevCtx = alcGetCurrentContext();
    g_ctx = alcCreateContext(g_dev, attrs);
    if (!g_ctx || !alcMakeContextCurrent(g_ctx))
    {
        if (g_ctx) alcDestroyContext(g_ctx);
        alcCloseDevice(g_dev);
        g_dev = NULL; g_ctx = NULL;
        eprintwarn("[SPATIAL] could not create an OpenAL context\n");
        return false;
    }

    /* Trust only what came back. */
    g_achievedSpeakers = 1;
    g_surroundActive   = 0;
    if (alcIsExtensionPresent(g_dev, "ALC_SOFT_output_mode"))
    {
        ALCint alcMode = 0;
        alcGetIntegerv(g_dev, ALC_OUTPUT_MODE_SOFT, 1, &alcMode);
        g_achievedSpeakers = AlcOutputModeToSpeakers(alcMode);
        g_surroundActive   = g_achievedSpeakers == 2 || g_achievedSpeakers == 3 ||
                             g_achievedSpeakers == 4;
    }
    {
        static const char* const kNames[] = { "auto", "stereo", "quad", "5.1", "7.1", "hrtf" };
        eprintinfo("[SPATIAL] speaker layout: %s (requested %s)%s\n",
                   kNames[g_achievedSpeakers],
                   kNames[(speakerMode >= 0 && speakerMode <= 5) ? speakerMode : 0],
                   g_surroundActive ? " [surround routing active]" : "");
        /* A layout the device would not give us is the one failure that
         * otherwise looks like the setting being ignored: the sink runs, the
         * game sounds fine, and only the extra speakers are missing. Say so,
         * and name the fallback that has its own layout handling. */
        if (speakerMode >= 2 && speakerMode <= 4 && !g_surroundActive)
        {
            eprintwarn("[SPATIAL] %s was requested but the device gave %s. Check the\n"
                       "          Windows speaker configuration and alsoft.ini, or set\n"
                       "          spu_renderer = legacy to use the OpenAL renderer.\n",
                       kNames[(speakerMode >= 0 && speakerMode <= 5) ? speakerMode : 0],
                       kNames[g_achievedSpeakers]);
        }
    }


    /* Real speakers get discrete channels; stereo and HRTF keep the panner. */
    switch (g_achievedSpeakers)
    {
    case 2: g_directOut = 1; g_directChannels = 4; g_directFormat = AL_FORMAT_QUAD16;
            g_ring = kRingQuad; g_ringCount = 4; break;
    case 3: g_directOut = 1; g_directChannels = 6; g_directFormat = AL_FORMAT_51CHN16;
            g_ring = kRing51;   g_ringCount = 5; break;
    case 4: g_directOut = 1; g_directChannels = 8; g_directFormat = AL_FORMAT_71CHN16;
            g_ring = kRing71;   g_ringCount = 7; break;
    default: g_directOut = 0; g_directChannels = 0; g_directFormat = 0; break;
    }
    /* The format tokens are compile-time defines, so their presence proves
     * nothing about this runtime. alGetEnumValue only resolves a token NAME,
     * which OpenAL Soft knows either way, so it never reported a miss -- and a
     * build without the extension would have reached alBufferData and got
     * silence, which is what this fallback exists to avoid. */
    if (g_directOut && !alIsExtensionPresent("AL_EXT_MCFORMATS"))
    {
        eprintwarn("[SPATIAL] multichannel formats unavailable; using positioned sources\n");
        g_directOut = 0;
    }

    eprintinfo("[SPATIAL] output stage: %s\n",
               g_directOut ? "discrete speaker channels (no panning)"
                           : "positioned mono sources (OpenAL panning)");

    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListenerf(AL_GAIN, 1.0f);
    {
        const ALfloat orient[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
        alListenerfv(AL_ORIENTATION, orient);
    }

    if (g_directOut)
    {
        alGenSources(1, &g_directSource);
        alGenBuffers(kQueueDepth, g_directBuffers);
        for (int q = 0; q < kQueueDepth; ++q)
            g_directFree[q] = g_directBuffers[q];
        g_directFreeCount = kQueueDepth;
        g_directScratch.assign((size_t)kBlockFrames * g_directChannels, 0);
        for (int k = 0; k < g_directChannels; ++k)
            g_spkAccum[k].assign(kBlockFrames, 0.0f);
        /* A multichannel buffer is not spatialised, but say so explicitly so
         * no distance or doppler model can touch it. */
        alSourcei(g_directSource, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(g_directSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(g_directSource, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcef(g_directSource, AL_GAIN, 1.0f);
        for (int b = 0; b < kTotalBuses; ++b)
            g_busAccum[b].assign(kBlockFrames, 0.0f);
    }
    else
    for (int b = 0; b < kTotalBuses; ++b)
    {
        alGenSources(1, &g_bus[b].source);
        alGenBuffers(kQueueDepth, g_bus[b].buffers);
        for (int q = 0; q < kQueueDepth; ++q)
            g_bus[b].free[q] = g_bus[b].buffers[q];
        g_bus[b].freeCount = kQueueDepth;
        g_bus[b].scratch.assign(kBlockFrames, 0);
        g_busAccum[b].assign(kBlockFrames, 0.0f);

        float az;
        if (b < kDryBuses)
            az = kDryAzimuthsDeg[b];
        else if (b < kDryBuses + kWetBuses)
            az = kWetAzimuthsDeg[b - kDryBuses];
        else
            az = kCdAzimuthsDeg[b - kDryBuses - kWetBuses];
        PlaceSource(g_bus[b].source, az);
    }

    g_core = core;
    g_coreMutex = coreMutex;
    g_running = 1;
    g_thread = SDL_CreateThread(PumpThread, "PsyX SPU spatial", NULL);
    if (!g_thread)
    {
        g_running = 0;
        PsyX_SPUSpatial_Stop();
        return false;
    }

    g_active = 1;
    eprintf("[SPATIAL] software SPU through OpenAL: %d dry buses (front arc), %d reverb (rear), %d CD\n",
            kDryBuses, kWetBuses, kCdBuses);
    return true;
}

void PsyX_SPUSpatial_Stop(void)
{
    if (g_running)
    {
        g_running = 0;
        if (g_thread)
        {
            SDL_WaitThread(g_thread, NULL);
            g_thread = NULL;
        }
    }

    if (g_directSource)
    {
        alSourceStop(g_directSource);
        alSourcei(g_directSource, AL_BUFFER, 0);
        alDeleteSources(1, &g_directSource);
        g_directSource = 0;
        alDeleteBuffers(kQueueDepth, g_directBuffers);
        memset(g_directBuffers, 0, sizeof(g_directBuffers));
    }
    g_directOut       = 0;
    g_directChannels  = 0;
    g_directFormat    = 0;
    g_directFreeCount = 0;

    for (int b = 0; b < kTotalBuses; ++b)
    {
        if (g_bus[b].source)
        {
            alSourceStop(g_bus[b].source);
            alSourcei(g_bus[b].source, AL_BUFFER, 0);
            alDeleteSources(1, &g_bus[b].source);
            g_bus[b].source = 0;
            alDeleteBuffers(kQueueDepth, g_bus[b].buffers);
            memset(g_bus[b].buffers, 0, sizeof(g_bus[b].buffers));
            g_bus[b].freeCount = 0;
        }
    }

    if (g_ctx)
    {
        alcMakeContextCurrent(g_prevCtx);
        alcDestroyContext(g_ctx);
        g_ctx = NULL;
    }
    if (g_dev)
    {
        alcCloseDevice(g_dev);
        g_dev = NULL;
    }
    g_core = NULL;
    g_coreMutex = NULL;
    g_active = 0;
}

int PsyX_SPUSpatial_Active(void)
{
    return g_active;
}

int PsyX_SPUSpatial_AchievedSpeakers(void)
{
    return g_achievedSpeakers;
}

int PsyX_SPUSpatial_SurroundActive(void)
{
    return g_surroundActive;
}

void PsyX_SPUSpatial_SetXaPump(void (*pump)(void* user, int frames), void* user)
{
    g_xaPump = pump;
    g_xaPumpUser = user;
}
