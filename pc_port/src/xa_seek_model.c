/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Per-line voice start delay, emulating what the PlayStation's drive and the
 * game's own XA task pool cost before a voice became audible.
 *
 * On the console a voice line queues two sound tasks, Stop then Play
 * (Sd_XaAudioPlayTaskAdd), and the task pool runs one state per game frame:
 *   Sd_XaAudioStop: fade-out frames (volume -24 a frame) + mute + CdlPause +
 *                   cleanup;
 *   Sd_XaAudioPlay: init, CdlSetmode, filter prep, CdlSetfilter, LBA, CdlSeekL,
 *                   then CdlReadN once CdSync reports the seek done, then
 *                   enable audio.
 * The seek is the only variable part: how far the head travels from where it
 * last read. The PC port skips the whole state machine (XaPlayer_Play starts
 * instantly), so this reproduces its duration and the players hold the voice
 * start by it. Seek time follows the Mednafen/DuckStation model. */
#include "xa_player.h"
#include "pc_config.h"
#include "sh_log.h"
#include "common.h"
#include "main/fileinfo.h"
#include "bodyprog/sound/sound_system.h"
#include <SDL_timer.h>

extern int      g_PsxCdHeadLba;
extern unsigned g_PsxCdHeadEpoch;
extern s32      g_IntervalVBlanks;
extern int      GetVideoMode(void);

#define PSX_CPU_HZ        33868800.0
#define DISC_SECTORS      (72 * 60 * 75)
#define XA_SECTORS_PER_S  150 /* CdlModeSpeed: the voice stream reads at 2x */
#define PLAY_FIXED_FRAMES 7   /* Sd_XaAudioPlay states outside the seek wait */
#define STOP_FIXED_FRAMES 3   /* mute, CdlPause, cleanup */
#define DIALOG_NORMAL_MS  300 /* cutscene_line_gap_ms of the "Normal" preset */

static int      s_streamActive  = 0;
static int      s_streamStopped = 0;
static int      s_streamLba     = 0;
static unsigned s_streamEpoch   = 0;
static Uint32   s_streamStartMs = 0;
static Uint32   s_streamStopMs  = 0;
static int      s_xaVol         = 0;
static Uint32   s_stopEndMs     = 0;
static Uint32   s_lineDelayMs   = 0;

static double FrameMs(void)
{
    int vbl = g_IntervalVBlanks;

    if (vbl < 1) vbl = 2;
    if (vbl > 4) vbl = 4;
    return 1000.0 * vbl / (GetVideoMode() == 1 ? 50.0 : 60.0);
}

static int HeadLba(Uint32 nowMs)
{
    /* ReadN keeps the drive reading through the clip and its pad until the
     * stop task pauses it, so the head ends wherever the stream had reached. */
    if (s_streamActive && s_streamEpoch == g_PsxCdHeadEpoch)
    {
        Uint32 end = s_streamStopped ? s_streamStopMs : nowMs;

        if ((Sint32)(end - s_streamStartMs) <= 0)
            return s_streamLba;
        return s_streamLba + (int)(((Uint64)(end - s_streamStartMs) * XA_SECTORS_PER_S) / 1000u);
    }
    return g_PsxCdHeadLba;
}

static double SeekMs(int fromLba, int toLba)
{
    int    diff   = (toLba > fromLba) ? toLba - fromLba : fromLba - toLba;
    double cycles = (double)diff * PSX_CPU_HZ / DISC_SECTORS;

    if (cycles < 20000.0)
        cycles = 20000.0;
    /* Long seeks pay a fixed settle; short ones pay the restart from the
     * paused state the stop task left the drive in (double speed). */
    cycles += (diff >= 2250) ? PSX_CPU_HZ * 0.3 : 1237952.0;
    return cycles * 1000.0 / PSX_CPU_HZ;
}

void Pc_XaSeek_OnStopTask(void)
{
    const Uint32 nowMs   = SDL_GetTicks();
    const double frameMs = FrameMs();
    int          fades   = 0;
    int          vol     = s_xaVol;

    do
    {
        fades++;
        vol -= 24;
    } while (vol >= 2);

    if (s_streamActive && !s_streamStopped && s_streamEpoch == g_PsxCdHeadEpoch)
    {
        s_streamStopped = 1;
        s_streamStopMs  = nowMs + (Uint32)((fades + 1) * frameMs);
    }
    s_xaVol     = 0;
    s_stopEndMs = nowMs + (Uint32)((fades + STOP_FIXED_FRAMES) * frameMs);
}

void Pc_XaSeek_OnPlayTask(uint16_t xaIdx, int xaVol)
{
    const Uint32 nowMs   = SDL_GetTicks();
    const double frameMs = FrameMs();
    const int    gap     = g_PcConfig.cutsceneLineGapMs;
    int          target, head, waitFrames = 0;
    double       seek, delay;
    Uint32       stopLeft;

    s_lineDelayMs = 0;
    if (xaIdx >= 727 || g_XaItemData[xaIdx].xaFileIdx_0 == 0)
        return;

    target   = (int)g_FileXaLoc[g_XaItemData[xaIdx].xaFileIdx_0] + (int)g_XaItemData[xaIdx].sector_4;
    head     = HeadLba(nowMs);
    seek     = SeekMs(head, target);
    stopLeft = ((Sint32)(s_stopEndMs - nowMs) > 0) ? s_stopEndMs - nowMs : 0;

    /* CdlSeekL is issued one frame before the first CdSync poll. */
    if (seek > frameMs)
        waitFrames = (int)((seek - frameMs + frameMs - 1.0) / frameMs);

    delay = (double)stopLeft + (PLAY_FIXED_FRAMES + waitFrames) * frameMs;
    if (gap <= 0)
        delay = 0.0;
    else
        delay = delay * gap / DIALOG_NORMAL_MS;
    if (delay > 3000.0)
        delay = 3000.0;
    s_lineDelayMs = (Uint32)delay;

    s_streamActive  = 1;
    s_streamStopped = 0;
    s_streamLba     = target;
    s_streamEpoch   = g_PsxCdHeadEpoch;
    s_streamStartMs = nowMs + s_lineDelayMs;
    s_xaVol         = xaVol;

    SH_DBG("[XASEEK] xa=%u head=%d target=%d seek=%.0fms stopLeft=%ums frames=%d+%d delay=%ums",
           (unsigned)xaIdx, head, target, seek, (unsigned)stopLeft,
           PLAY_FIXED_FRAMES, waitFrames, (unsigned)s_lineDelayMs);
}

uint32_t Pc_XaSeek_LineDelayMs(void)
{
    uint32_t d = s_lineDelayMs;

    s_lineDelayMs = 0;
    return d;
}
