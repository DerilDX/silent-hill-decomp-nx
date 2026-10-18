/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_pad_icons.c - gamepad icon mapping, no renderer, no SDL, no game state.
 * See pc_pad_icons.h; the stateful half is pc_pad_icons_apply.c.
 */

#include <stddef.h>

#include "pc_pad_icons.h"

/* OPTION2.TIM sits at VRAM x 480, 32 cells into tpage 7 (x 448), so its
 * texel column 0 is tpage u 128. */
#define ATLAS_U0 128
#define ATLAS_W  128
#define ATLAS_H  256

/* SDL names follow the Xbox face layout (a = bottom), which is where the
 * PlayStation Cross sits. */
static const struct { const char* sdl; int icon; } s_binds[] = {
    { "lefttrigger", PI_L2 },        { "righttrigger", PI_R2 },
    { "leftshoulder", PI_L1 },       { "rightshoulder", PI_R1 },
    { "y", PI_TRIANGLE },            { "b", PI_CIRCLE },
    { "a", PI_CROSS },               { "x", PI_SQUARE },
    { "back", PI_SELECT },           { "leftstick", PI_L3 },
    { "rightstick", PI_R3 },         { "start", PI_START },
    { "dpup", PI_DPAD_UP },          { "dpright", PI_DPAD_RIGHT },
    { "dpdown", PI_DPAD_DOWN },      { "dpleft", PI_DPAD_LEFT },
};

static const char* const s_names[PI_COUNT] = {
    "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square",
    "Select", "L3", "R3", "Start",
    "D-PAD_U", "D-PAD_R", "D-PAD_D", "D-PAD_L",
    "LS_U", "LS_R", "LS_D", "LS_L",
};

static const char* const s_setLabels[PAD_ICONS_COUNT] = { "Original", "Xbox", "Nintendo" };

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int same_name(const char* a, const char* b)
{
    while (*a != '\0' && lower((unsigned char)*a) == lower((unsigned char)*b))
    {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int PadIcons_ForBind(const char* sdlName)
{
    int i;

    if (sdlName == NULL)
        return -1;
    for (i = 0; i < (int)(sizeof(s_binds) / sizeof(s_binds[0])); i++)
    {
        if (same_name(sdlName, s_binds[i].sdl))
            return s_binds[i].icon;
    }
    return -1;
}

void PadIcons_UV(int icon, int* u, int* v)
{
    int cu = 244, cv = 0;

    if (icon >= PI_L2 && icon <= PI_DPAD_LEFT)
        cv = icon * 16;
    else if (icon >= PI_LS_UP && icon < PI_COUNT)
    {
        cu = 228;
        cv = (icon - PI_LS_UP) * 16;
    }
    if (u != NULL)
        *u = cu;
    if (v != NULL)
        *v = cv;
}

/* Buttons with no PlayStation counterpart (guide, touchpad, paddles, misc)
 * print their SDL name, cut down to what FONT16 can draw. */
const char* PadIcons_Fallback(int icon, const char* sdlName)
{
    static char buf[16];
    size_t      n = 0;

    if (icon >= 0 && icon < PI_COUNT)
        return s_names[icon];
    if (sdlName == NULL)
        return "";
    for (; sdlName[n] != '\0' && n < sizeof(buf) - 1; n++)
    {
        int c = (unsigned char)sdlName[n];

        if (n == 0 && c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            c = '_';
        buf[n] = (char)c;
    }
    buf[n] = '\0';
    return buf;
}

const char* PadIcons_FallbackShort(int icon, const char* sdlName)
{
    static const char* const s_short[4] = { "DP_U", "DP_R", "DP_D", "DP_L" };

    if (icon >= PI_DPAD_UP && icon <= PI_DPAD_LEFT)
        return s_short[icon - PI_DPAD_UP];
    return PadIcons_Fallback(icon, sdlName);
}

int PadIcons_CellHasArt(const unsigned char* rgba, int w, int h, int u, int v)
{
    int sx, sy, x, y;

    if (rgba == NULL || w < ATLAS_W || h < ATLAS_H || w % ATLAS_W != 0 || h % ATLAS_H != 0)
        return 0;
    if (u < ATLAS_U0 || u + PAD_ICON_SIZE > ATLAS_U0 + ATLAS_W || v < 0 || v + PAD_ICON_SIZE > ATLAS_H)
        return 0;
    sx = w / ATLAS_W;
    sy = h / ATLAS_H;
    for (y = v * sy; y < (v + PAD_ICON_SIZE) * sy; y++)
    {
        const unsigned char* row = rgba + (size_t)y * (size_t)w * 4;

        for (x = (u - ATLAS_U0) * sx; x < (u - ATLAS_U0 + PAD_ICON_SIZE) * sx; x++)
        {
            if (row[(size_t)x * 4 + 3] != 0)
                return 1;
        }
    }
    return 0;
}

const char* PadIcons_SetLabel(int set)
{
    return (set >= 0 && set < PAD_ICONS_COUNT) ? s_setLabels[set] : s_setLabels[PAD_ICONS_ORIGINAL];
}
