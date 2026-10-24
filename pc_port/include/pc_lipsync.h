/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_LIPSYNC_H
#define PC_LIPSYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lip sync from the voice stream itself. The XA players feed each line's PCM
 * as they decode it, ahead of playback; the loudness envelope drives a mouth,
 * and a pitch vote over the line says whether the speaker is male or female,
 * which is all the game tells us about who is talking. */
void Pc_Lip_Feed(const int16_t* pcm, int frames, int rate, int channels);
void Pc_Lip_Stop(void);
void Pc_Lip_Hold(int hold);

/* Mouth opening 0..1 at the current play position, or -1 when no voice is
 * playing. *female is 1 or 0 once the line's pitch is known, -1 before. */
float Pc_Lip_Level(int* female);

#ifdef __cplusplus
}
#endif

#endif
