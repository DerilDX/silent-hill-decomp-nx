#ifndef PSYX_SPUSPATIAL_H
#define PSYX_SPUSPATIAL_H

/* Spatialised output for the software SPU.
 *
 * The software SPU models the PlayStation's synthesis and reverb exactly, but
 * the hardware is a STEREO device, so its renderer mixes all 24 voices down to
 * L/R and its output interface is literally an interleaved stereo buffer. That
 * left surround users choosing between accurate audio and more than two
 * speakers: the legacy OpenAL backend gives them a speaker layout but only an
 * approximate reverb, and this one gives them the real reverb in stereo.
 *
 * This module removes the choice. The core still synthesises and still runs its
 * own reverb -- nothing about the emulation changes -- but instead of taking its
 * stereo downmix we take the per-voice taps, place each voice in a speaker field
 * from its own PSX pan, and let OpenAL render that field to whatever layout the
 * user actually has.
 *
 * Voices are summed into a handful of fixed-direction buses rather than getting
 * a source each. Twenty-four independently streamed sources drift the moment one
 * of them underruns, and recovering it is audible; a fixed set of buses never
 * starts or stops, so every stream stays sample-aligned for the whole session.
 *
 * The dry field stays in FRONT of the listener because that is where a stereo
 * console image belongs, and the reverb return is sent to the rear pair. That is
 * what the extra speakers are for, and it is the one part of the mix that is
 * genuinely ambient rather than positional. */

struct SDL_mutex;

namespace PsyX { class SPUCore; }

/* speakerMode matches PsyX_SPUAL_SetOutputMode: 0 auto, 1 stereo, 2 quad,
 * 3 5.1, 4 7.1, 5 HRTF. Returns false if OpenAL is unavailable, in which case
 * the caller should keep using the ordinary stereo sink. */
bool PsyX_SPUSpatial_Start(PsyX::SPUCore* core, SDL_mutex* coreMutex, int speakerMode);
void PsyX_SPUSpatial_Stop(void);
int  PsyX_SPUSpatial_Active(void);

/* The layout the device ACTUALLY gave us, in the same 0..5 ids, and whether
 * that layout has real surround channels. Measured from ALC, not echoed back
 * from the request, because a 5.1 ask on a stereo endpoint degrades silently.
 * Meaningless unless PsyX_SPUSpatial_Active(). */
int  PsyX_SPUSpatial_AchievedSpeakers(void);
int  PsyX_SPUSpatial_SurroundActive(void);

/* Pending XA/CD frames are pushed by the owner before each render block. */
void PsyX_SPUSpatial_SetXaPump(void (*pump)(void* user, int frames), void* user);

#endif
