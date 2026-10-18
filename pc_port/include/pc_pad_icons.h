/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_pad_icons.h - switchable gamepad icon sets drawn from the OPTION2.TIM
 * button atlas (tpage 7, 4bpp, CLUT (464,0)). pc_pad_icons.c is the pure
 * mapping; pc_pad_icons_apply.c loads gamedata/icons/pad_<set>.png over the
 * atlas. Cell layout for artists: pc_port/docs/PadIcons.md.
 */
#ifndef PC_PAD_ICONS_H
#define PC_PAD_ICONS_H

typedef enum { PAD_ICONS_ORIGINAL, PAD_ICONS_XBOX, PAD_ICONS_NINTENDO, PAD_ICONS_COUNT } e_PadIconSet;

typedef enum
{
    PI_L2, PI_R2, PI_L1, PI_R1, PI_TRIANGLE, PI_CIRCLE, PI_CROSS, PI_SQUARE,
    PI_SELECT, PI_L3, PI_R3, PI_START,                     /* disc art, u 244, v = id * 16 */
    PI_DPAD_UP, PI_DPAD_RIGHT, PI_DPAD_DOWN, PI_DPAD_LEFT, /* u 244, v 192..240 (blank on disc) */
    PI_LS_UP, PI_LS_RIGHT, PI_LS_DOWN, PI_LS_LEFT,         /* u 228, v 0..48 (new column) */
    PI_COUNT
} e_PadIcon;

#define PAD_ICON_SIZE 12

int         PadIcons_ForBind(const char* sdlName);
void        PadIcons_UV(int icon, int* u, int* v);
const char* PadIcons_Fallback(int icon, const char* sdlName);
/* Compact stand-in for a cell too narrow for PadIcons_Fallback: "DP_x" for
 * the d-pad, otherwise the same text. Same static buffer rule. */
const char* PadIcons_FallbackShort(int icon, const char* sdlName);
int         PadIcons_CellHasArt(const unsigned char* rgba, int w, int h, int u, int v);
int         PadIcons_HasArt(int icon);
const char* PadIcons_SetLabel(int set);
void        PadIcons_Step(int dir);
void        PadIcons_Apply(void);

#endif
