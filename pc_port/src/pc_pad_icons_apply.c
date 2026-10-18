/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_pad_icons_apply.c - puts the chosen gamepad icon set over the OPTION2.TIM
 * button atlas as a hi-res override. Runs on the game thread, after the atlas
 * upload (fsqueue_3.c) and when the set is switched on the Controls screen.
 */

#include <stdio.h>
#include <stdlib.h>

#include "hires_override.h"
#include "pc_config.h"
#include "pc_pad_icons.h"
#include "sh_log.h"

/* g_ControllerButtonAtlasImg: pixels at (480,0) 32x256 cells, CLUT (464,0). */
#define ATLAS_VRAM_X 480
#define ATLAS_VRAM_Y 0
#define ATLAS_VRAM_W 32
#define ATLAS_VRAM_H 256
#define ATLAS_CLUT_X 464
#define ATLAS_CLUT_Y 0
#define ATLAS_BPP    4

static const char* const s_files[PAD_ICONS_COUNT] = {
    "gamedata/icons/pad_original.png",
    "gamedata/icons/pad_xbox.png",
    "gamedata/icons/pad_nintendo.png",
};

/* Until a set is applied the disc atlas is what draws. */
static int s_hasArt[PI_COUNT] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

static unsigned char* read_file(const char* path, unsigned int* outSize)
{
    FILE*          f = fopen(path, "rb");
    long           n;
    unsigned char* buf;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (n > 0) ? (unsigned char*)malloc((size_t)n) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outSize = (unsigned int)n;
    return buf;
}

static void UseDiscArt(void)
{
    int i;

    HiresOverride_DropVramRect(ATLAS_VRAM_X, ATLAS_VRAM_Y, ATLAS_VRAM_W, ATLAS_VRAM_H,
                               ATLAS_CLUT_X, ATLAS_CLUT_Y, ATLAS_BPP);
    for (i = 0; i < PI_COUNT; i++)
        s_hasArt[i] = i <= PI_START;
}

void PadIcons_Apply(void)
{
    int            set  = g_PcConfig.gamepadIcons;
    const char*    path;
    unsigned char* file;
    unsigned char* rgba = NULL;
    unsigned int   size = 0;
    int            w = 0, h = 0, i, cells = 0;

    if (set < 0 || set >= PAD_ICONS_COUNT)
        set = PAD_ICONS_ORIGINAL;
    path = s_files[set];

    file = read_file(path, &size);
    if (file == NULL)
    {
        UseDiscArt();
        SH_DBG("[PADICONS] %s set: %s missing, using the disc icons", PadIcons_SetLabel(set), path);
        return;
    }
    if (HiresOverride_DecodeToRGBA(file, size, &rgba, &w, &h) != 0 || rgba == NULL)
    {
        free(file);
        UseDiscArt();
        SH_DBG("[PADICONS] %s set: %s does not decode, using the disc icons", PadIcons_SetLabel(set), path);
        return;
    }
    free(file);

    /* The override stretches the image over the whole atlas, so anything but
     * a uniform multiple of 128x256 would put every icon off its cell. */
    if (w % 128 != 0 || h % 256 != 0 || w / 128 != h / 256 || w == 0 ||
        HiresOverride_RegisterRGBA(path, rgba, w, h, ATLAS_VRAM_X, ATLAS_VRAM_Y, ATLAS_VRAM_W,
                                   ATLAS_VRAM_H, ATLAS_CLUT_X, ATLAS_CLUT_Y, ATLAS_BPP) != 0)
    {
        free(rgba);
        UseDiscArt();
        SH_DBG("[PADICONS] %s set: %s is %dx%d or failed to register (needs 128x256 or a 2x/4x multiple), using the disc icons",
               PadIcons_SetLabel(set), path, w, h);
        return;
    }

    for (i = 0; i < PI_COUNT; i++)
    {
        int u, v;

        PadIcons_UV(i, &u, &v);
        s_hasArt[i] = PadIcons_CellHasArt(rgba, w, h, u, v);
        cells += s_hasArt[i];
    }
    free(rgba);
    SH_DBG("[PADICONS] %s set: %s %dx%d, %d of %d icons have art", PadIcons_SetLabel(set), path, w,
           h, cells, PI_COUNT);
}

int PadIcons_HasArt(int icon)
{
    return (icon >= 0 && icon < PI_COUNT) ? s_hasArt[icon] : 0;
}

void PadIcons_Step(int dir)
{
    char value[8];

    g_PcConfig.gamepadIcons =
        ((g_PcConfig.gamepadIcons + (dir < 0 ? -1 : 1)) % PAD_ICONS_COUNT + PAD_ICONS_COUNT) %
        PAD_ICONS_COUNT;
    snprintf(value, sizeof(value), "%d", g_PcConfig.gamepadIcons);
    PcConfig_SaveKeyValue("gamepad_icons", value);
    PadIcons_Apply();
}
