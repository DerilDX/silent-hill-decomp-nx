#ifndef PSYX_SPUAL_EXT_H
#define PSYX_SPUAL_EXT_H

#include "PsyX_SPUAL.h"

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern int PsyX_SPUAL_AllocAt(u_int addr, int size);
extern int PsyX_SPUAL_SetTransferMode(int mode);
extern void PsyX_SPUAL_SetCommonAttr(SpuCommonAttr* psxAttrib);
extern void PsyX_SPUAL_GetCommonAttr(SpuCommonAttr* psxAttrib);
extern void PsyX_SPUAL_GetReverbModeParam(SpuReverbAttr* attr);
extern int PsyX_SPUAL_ClearReverbWorkArea(void);

extern void PsyX_SPUAL_ConfigureOutput(int backend, int mode, int rate, int bitPerfect);
extern int PsyX_SPUAL_ConfigureRenderer(
	int renderer, int highPrecisionClip, int modernClip, int modernDither);

/* Spatial output (software backend only): run the core through OpenAL
 * placement instead of taking its stereo downmix. */
extern void PsyX_SPUAL_ConfigureSpatial(int enable, int speakers);

extern int PsyX_SPUAL_PushXaFrames(
	const short* samples, u_int frames, int sourceRate, int channels);
extern void PsyX_SPUAL_ResetXa(void);
extern void PsyX_SPUAL_FinishXa(void);
extern int PsyX_SPUAL_IsXaDrained(void);
extern void PsyX_SPUAL_SetXaMasterGain(double gain);
extern void PsyX_SPUAL_SetXaPaused(int paused);
extern u_int PsyX_SPUAL_GetQueuedXaFrames(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif
