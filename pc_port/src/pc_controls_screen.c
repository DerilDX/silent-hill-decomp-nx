/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_controls_screen.c - native Controls (rebind) screen. See
 * pc_controls_screen.h. pc_bind_panel.c (GL, Quick Options) is the
 * behavioural reference for activate, clear, capture and reset.
 */
#include "game.h"

#include <stdio.h>
#include <string.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "dbg_overlay.h"
#include "lang_text.h"
#include "pc_bind_core.h"
#include "pc_bind_input.h"
#include "pc_config.h"
#include "pc_controls_screen.h"
#include "pc_native_menu.h"
#include "pc_native_menu_draw.h"
#include "pc_pad_icons.h"
#include "sh_log.h"

extern int g_DebugThirdPersonCam;

/* Options draws only y 0..224 of the 240-line text space. */
#define CS_ICONS_Y      4
#define CS_RULE1_Y      23
#define CS_HEAD_Y       25
#define CS_RULE2_Y      42
#define CS_ARROW_UP_Y   49
#define CS_LIST_Y       56
#define CS_PITCH        16
#define CS_VISIBLE      8
#define CS_ARROW_DOWN_Y 189
#define CS_RULE3_Y      198
#define CS_RESET_Y      202
#define CS_CONFIRM_Y    96

#define CS_RULE_X0      4
#define CS_RULE_X1      316
#define CS_TEXT_X       28
#define CS_VALUE_RX     300
#define CS_LABEL_RX     129
#define CS_HEAD_FUNC_X  70
#define CS_HL_X0        2
#define CS_HL_X1        316
#define CS_ARROW_X      160
#define CS_ROW_H        16
#define CS_ICON_DY      2

/* After the text (NM_OT_TEXT 8), so each icon's DR_TPAGE cannot touch a glyph. */
#define CS_OT_ICON      7

#define CS_RGB_WHITE    128
#define CS_RGB_DIM      64

/* Widths from the FONT16 kerning table: the widest English label
 * ("Change_Camera", 129) starts at x 0; "Space" (48) fits both keyboard
 * cells; pad cells take "LS_U" (41) and "DP_U" (43), PAD2 ends at x 318.
 * Advances carry the glyph's own right gap, so cells abut. */
static const int s_colX[BC_COLS] = { 134, 182, 230, 275 };
static const int s_colW[BC_COLS] = { 48, 48, 43, 43 };

enum { CS_FOCUS_ICONS, CS_FOCUS_LIST, CS_FOCUS_RESET };
enum { CS_HIT_NONE, CS_HIT_ICONS, CS_HIT_ROW, CS_HIT_RESET, CS_HIT_YES, CS_HIT_NO };

static int       s_open;
static int       s_focus;
static int       s_col, s_wantCol;
static int       s_listen;
static NmScroll  s_list;
static NmConfirm s_confirm;
static BindNav   s_nav;

static void cs_beep(int sfxId)
{
    Sd_PlaySfx((u16)sfxId, 0, 64);
}

static int cs_cell_exists(int row, int col)
{
    const BcRow* r = BindCore_Row(row);

    return r != NULL && col >= 0 && col < BC_COLS && r->key[col] != NULL;
}

static int cs_stick_cell(int row, int col)
{
    const BcRow* r = BindCore_Row(row);

    return r != NULL && (r->flags & BCF_STICK) && col >= BC_PAD1 && r->key[col] == NULL;
}

/* The nearest cell of this row that exists, preferring the left side. */
static int cs_resolve_col(int row, int want)
{
    int d;

    if (cs_cell_exists(row, want))
        return want;
    for (d = 1; d < BC_COLS; d++)
    {
        if (cs_cell_exists(row, want - d))
            return want - d;
        if (cs_cell_exists(row, want + d))
            return want + d;
    }
    return BC_KB1;
}

static void cs_focus_list(int row)
{
    s_focus = CS_FOCUS_LIST;
    NmScroll_Select(&s_list, row);
    s_col = cs_resolve_col(s_list.sel, s_wantCol);
}

static void cs_move_row(int dir)
{
    const int last = BindCore_RowCount() - 1;

    switch (s_focus)
    {
        case CS_FOCUS_ICONS:
            if (dir > 0)
                cs_focus_list(0);
            else
                s_focus = CS_FOCUS_RESET;
            break;

        case CS_FOCUS_LIST:
            if (s_list.sel + dir < 0)
                s_focus = CS_FOCUS_ICONS;
            else if (s_list.sel + dir > last)
                s_focus = CS_FOCUS_RESET;
            else
                cs_focus_list(s_list.sel + dir);
            break;

        default:
            if (dir > 0)
                s_focus = CS_FOCUS_ICONS;
            else
                cs_focus_list(last);
            break;
    }
}

static void cs_move_col(int dir)
{
    int c = s_col;
    int i;

    for (i = 0; i < BC_COLS - 1; i++)
    {
        c = (c + dir + BC_COLS) % BC_COLS;
        if (cs_cell_exists(s_list.sel, c))
            break;
    }
    if (cs_cell_exists(s_list.sel, c) && c != s_col)
    {
        s_col     = c;
        s_wantCol = c;
        cs_beep(Sfx_MenuMove);
    }
}

static void cs_activate(void)
{
    char toast[96];
    int  row = s_list.sel;

    if (!BindCore_CellUsable(row, s_col) || (s_col >= BC_PAD1 && !BindInput_PadConnected()))
    {
        cs_beep(Sfx_MenuError);
        return;
    }
    cs_beep(Sfx_MenuConfirm);
    BindCore_FormatRebindToast(row, s_col, toast, sizeof(toast));
    DbgOverlay_ToastLine(toast);
    s_listen = 1;
    BindCapture_Begin(row, s_col);
}

static void cs_clear(void)
{
    if (!BindCore_CellUsable(s_list.sel, s_col))
    {
        cs_beep(Sfx_MenuError);
        return;
    }
    BindCore_Set(s_list.sel, s_col, "NONE");
    cs_beep(Sfx_MenuCancel);
}

static void cs_open_reset(void)
{
    cs_beep(Sfx_MenuConfirm);
    NmConfirm_Open(&s_confirm);
}

static int cs_value_left(void)
{
    return CS_VALUE_RX - Nm_TextWidth(PadIcons_SetLabel(g_PcConfig.gamepadIcons));
}

static int cs_in_row(int my, int y)
{
    return my >= y && my < y + CS_ROW_H;
}

static int cs_hit(const NmInput* in, int* outRow, int* outCol)
{
    int i, c;

    *outRow = -1;
    *outCol = -1;
    if (s_confirm.open)
    {
        if (!cs_in_row(in->mouseY, CS_CONFIRM_Y + 24) || in->mouseX < 80 || in->mouseX >= 240)
            return CS_HIT_NONE;
        return (in->mouseX < 160) ? CS_HIT_YES : CS_HIT_NO;
    }
    if (cs_in_row(in->mouseY, CS_ICONS_Y))
        return CS_HIT_ICONS;
    if (cs_in_row(in->mouseY, CS_RESET_Y))
        return CS_HIT_RESET;
    if (in->mouseY < CS_LIST_Y)
        return CS_HIT_NONE;
    i = (in->mouseY - CS_LIST_Y) / CS_PITCH;
    if (i >= s_list.visible || s_list.top + i >= s_list.count)
        return CS_HIT_NONE;
    *outRow = s_list.top + i;
    for (c = 0; c < BC_COLS; c++)
    {
        int x0 = s_colX[c] - 2;
        int x1 = (c + 1 < BC_COLS) ? s_colX[c + 1] - 2 : s_colX[c] + s_colW[c] + 2;

        if (in->mouseX >= x0 && in->mouseX < x1)
            *outCol = c;
    }
    return CS_HIT_ROW;
}

static void cs_mouse(const NmInput* in)
{
    int row, col, hit;

    if (!in->mouseOk || !(in->mouseMoved || in->mouseLeft || in->mouseRight))
        return;
    hit = cs_hit(in, &row, &col);

    if (in->mouseMoved)
    {
        int oldFocus = s_focus, oldSel = s_list.sel, oldCol = s_col;

        if (hit == CS_HIT_ICONS)
            s_focus = CS_FOCUS_ICONS;
        else if (hit == CS_HIT_RESET)
            s_focus = CS_FOCUS_RESET;
        else if (hit == CS_HIT_ROW)
        {
            if (cs_cell_exists(row, col))
                s_wantCol = col;
            cs_focus_list(row);
        }
        if (s_focus != oldFocus || (s_focus == CS_FOCUS_LIST && (s_list.sel != oldSel || s_col != oldCol)))
            cs_beep(Sfx_MenuMove);
    }

    if (in->mouseLeft)
    {
        if (hit == CS_HIT_ICONS)
        {
            int left = cs_value_left();

            s_focus = CS_FOCUS_ICONS;
            PadIcons_Step((in->mouseX < left && in->mouseX >= left - 16) ? -1 : 1);
            cs_beep(Sfx_MenuMove);
        }
        else if (hit == CS_HIT_RESET)
        {
            s_focus = CS_FOCUS_RESET;
            cs_open_reset();
        }
        else if (hit == CS_HIT_ROW && cs_cell_exists(row, col))
        {
            s_wantCol = col;
            cs_focus_list(row);
            cs_activate();
        }
        else if (hit == CS_HIT_ROW && cs_stick_cell(row, col))
        {
            cs_beep(Sfx_MenuError);
        }
    }
    else if (in->mouseRight && hit == CS_HIT_ROW)
    {
        if (cs_cell_exists(row, col))
        {
            s_wantCol = col;
            cs_focus_list(row);
            cs_clear();
        }
        else if (cs_stick_cell(row, col))
        {
            cs_beep(Sfx_MenuError);
        }
    }
}

static void cs_confirm_update(const int pressed[BN_COUNT], const NmInput* mouse)
{
    NmInput         in;
    NmConfirmResult res;
    int             row, col;

    memset(&in, 0, sizeof(in));
    in.left    = pressed[BN_LEFT];
    in.right   = pressed[BN_RIGHT];
    in.confirm = pressed[BN_OK];
    in.cancel  = pressed[BN_BACK];

    if (mouse->mouseOk && (mouse->mouseMoved || mouse->mouseLeft))
    {
        int hit = cs_hit(mouse, &row, &col);

        if (hit == CS_HIT_YES || hit == CS_HIT_NO)
        {
            int yes = (hit == CS_HIT_YES);

            if (s_confirm.yes != yes)
                cs_beep(Sfx_MenuMove);
            s_confirm.yes = yes;
            in.confirm |= mouse->mouseLeft;
        }
    }
    if (in.left || in.right)
        cs_beep(Sfx_MenuMove);

    res = NmConfirm_Update(&s_confirm, &in);
    if (res == NM_CONFIRM_YES)
    {
        cs_beep(Sfx_MenuConfirm);
        BindCore_ResetScheme();
    }
    else if (res == NM_CONFIRM_NO)
    {
        cs_beep(Sfx_MenuCancel);
    }
}

void Pc_ControlsScreen_Open(void)
{
    BindCore_SetScheme(g_DebugThirdPersonCam ? 1 : 0);
    NmScroll_Init(&s_list, BindCore_RowCount(), CS_VISIBLE);
    s_focus   = CS_FOCUS_LIST;
    s_col     = BC_KB1;
    s_wantCol = BC_KB1;
    s_listen  = 0;
    memset(&s_confirm, 0, sizeof(s_confirm));
    BindNav_Reset(&s_nav);
    s_open = 1;
    SH_DBG("[BINDS] controls screen open: %s scheme", BindCore_Scheme() ? "altcam" : "classic");
}

int Pc_ControlsScreen_IsOpen(void)
{
    return s_open;
}

int Pc_ControlsScreen_Update(void)
{
    int     pressed[BN_COUNT];
    NmInput in;

    if (!s_open)
        return 0;

    if (s_listen)
    {
        const char* got = BindCapture_Update();

        if (got != NULL)
        {
            s_listen = 0;
            if (got[0] == '\0')
            {
                cs_beep(Sfx_MenuCancel);
            }
            else
            {
                BindCore_Set(s_list.sel, s_col, got);
                cs_beep(Sfx_MenuConfirm);
            }
            /* The captured press must not also navigate. */
            BindNav_Reset(&s_nav);
        }
        return 1;
    }

    BindNav_Poll(&s_nav, pressed);
    NmInput_FromGame(&in);

    if (s_confirm.open)
    {
        cs_confirm_update(pressed, &in);
        return 1;
    }

    if (pressed[BN_BACK])
    {
        cs_beep(Sfx_MenuCancel);
        s_open = 0;
        SH_DBG("[BINDS] controls screen closed");
        return 0;
    }

    if (in.wheel != 0)
    {
        NmScroll_Wheel(&s_list, in.wheel);
        if (s_focus == CS_FOCUS_LIST)
            s_col = cs_resolve_col(s_list.sel, s_wantCol);
    }
    cs_mouse(&in);
    if (s_listen || s_confirm.open)
        return 1;

    if (pressed[BN_UP] || pressed[BN_DOWN])
    {
        cs_move_row(pressed[BN_UP] ? -1 : 1);
        cs_beep(Sfx_MenuMove);
    }
    if (pressed[BN_LEFT] || pressed[BN_RIGHT])
    {
        int dir = pressed[BN_LEFT] ? -1 : 1;

        if (s_focus == CS_FOCUS_ICONS)
        {
            PadIcons_Step(dir);
            cs_beep(Sfx_MenuMove);
        }
        else if (s_focus == CS_FOCUS_LIST)
        {
            cs_move_col(dir);
        }
    }

    if (pressed[BN_OK])
    {
        if (s_focus == CS_FOCUS_ICONS)
        {
            PadIcons_Step(1);
            cs_beep(Sfx_MenuMove);
        }
        else if (s_focus == CS_FOCUS_LIST)
        {
            cs_activate();
        }
        else
        {
            cs_open_reset();
        }
    }
    else if (pressed[BN_CLEAR])
    {
        if (s_focus == CS_FOCUS_LIST)
            cs_clear();
        else
            cs_beep(Sfx_MenuError);
    }
    return 1;
}

/* ---- draw ---------------------------------------------------------------- */

/* A translated string too wide for the space is cut; a fitting one is drawn
 * by its key so Gfx_StringDraw still localizes it. */
static const char* cs_fit(const char* key, int maxW, char* out, size_t outSize)
{
    const char* t = Pc_LangMenuText(key);

    if (Pc_LangMenuTextWidth(t) <= maxW)
        return key;
    Nm_FitEllipsis(t, out, outSize, maxW, Pc_LangMenuTextWidth);
    return out;
}

static void cs_icon_draw(int icon, int x, int y, int color)
{
    GsOT_TAG*  ot    = &g_OrderingTable2[g_ActiveBufferIdx].org[CS_OT_ICON];
    SPRT*      prim  = (SPRT*)GsOUT_PACKET_P;
    DR_TPAGE*  tpage = (DR_TPAGE*)(prim + 1);
    int        py    = (g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED) ? (y - 112) * 2 : (y - 112);
    int        u, v, r, g, b;

    switch (color)
    {
        case StringColorId_Gold:     r = 160; g = 128; b = 64; break;
        case StringColorId_Red:      r = 128; g = 0;   b = 0;  break;
        case StringColorId_DarkGrey: r = g = b = CS_RGB_DIM;   break;
        default:                     r = g = b = CS_RGB_WHITE; break;
    }
    PadIcons_UV(icon, &u, &v);

    setSprt(prim);
    setRGB0(prim, r, g, b);
    setXY0(prim, x - 160, py);
    setWH(prim, PAD_ICON_SIZE, PAD_ICON_SIZE);
    setUV0(prim, u, v);
    setClut(prim, g_ControllerButtonAtlasImg.clutX, g_ControllerButtonAtlasImg.clutY);
    addPrim(ot, prim);

    setDrawTPage(tpage, 0, 1, getTPageN(0, 0, 7, 0));
    addPrim(ot, tpage);
    GsOUT_PACKET_P = (PACKET*)(tpage + 1);
}

static void cs_pad_draw(int icon, const char* sdlName, int col, int y, int color)
{
    char name[NM_LINE_MAX], fit[NM_LINE_MAX];

    if (icon >= 0 && PadIcons_HasArt(icon))
    {
        cs_icon_draw(icon, s_colX[col], y + CS_ICON_DY, color);
        return;
    }
    Nm_SanitizeGlyphs(PadIcons_Fallback(icon, sdlName), name, sizeof(name), 0);
    if (Nm_TextWidth(name) > s_colW[col])
        Nm_SanitizeGlyphs(PadIcons_FallbackShort(icon, sdlName), name, sizeof(name), 0);
    Nm_TextDraw(s_colX[col], y, NM_TEXT_TALL, color, cs_fit(name, s_colW[col], fit, sizeof(fit)));
}

/* The left-stick and d-pad icons for a Move row's direction. */
static int cs_stick_icon(int row, int col)
{
    static const struct { const char* key; int ls, dpad; } dirs[] = {
        { "key_up", PI_LS_UP, PI_DPAD_UP },         { "key_down", PI_LS_DOWN, PI_DPAD_DOWN },
        { "key_left", PI_LS_LEFT, PI_DPAD_LEFT },   { "key_right", PI_LS_RIGHT, PI_DPAD_RIGHT },
    };
    const char* key = BindCore_Row(row)->key[BC_KB1];
    int         i;

    for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++)
    {
        if (strcmp(key, dirs[i].key) == 0)
            return (col == BC_PAD1) ? dirs[i].ls : dirs[i].dpad;
    }
    return -1;
}

static void cs_cell_draw(int row, int col, int y, int selected)
{
    char        name[NM_LINE_MAX], clean[NM_LINE_MAX], fit[NM_LINE_MAX];
    const char* v;
    int         color;

    if (!cs_cell_exists(row, col))
    {
        if (cs_stick_cell(row, col))
            cs_pad_draw(cs_stick_icon(row, col), NULL, col, y, StringColorId_DarkGrey);
        return;
    }
    if (selected && s_listen)
    {
        Nm_TextDraw(s_colX[col], y, NM_TEXT_TALL, StringColorId_Gold, "...");
        return;
    }

    if (!BindCore_RowEnabled(row))
        color = selected ? StringColorId_Gold : StringColorId_DarkGrey;
    else if (BindCore_IsDuplicate(row, col))
        color = StringColorId_Red;
    else
        color = selected ? StringColorId_Gold : StringColorId_White;

    v = BindCore_Value(row, col);
    if (BindCore_Unbound(v))
    {
        Nm_TextDraw(s_colX[col], y, NM_TEXT_TALL, color, "-");
        return;
    }
    if (col >= BC_PAD1)
    {
        cs_pad_draw(PadIcons_ForBind(v), v, col, y, color);
        return;
    }
    BindCore_DisplayKey(v, name, sizeof(name));
    Nm_SanitizeGlyphs(name, clean, sizeof(clean), 0);
    Nm_TextDraw(s_colX[col], y, NM_TEXT_TALL, color, cs_fit(clean, s_colW[col], fit, sizeof(fit)));
}

static void cs_row_draw(int row, int y)
{
    const int   selRow = (s_focus == CS_FOCUS_LIST && row == s_list.sel);
    const int   color  = BindCore_RowEnabled(row) ? StringColorId_White : StringColorId_DarkGrey;
    char        label[NM_LINE_MAX], fit[NM_LINE_MAX];
    const char* shown;
    int         c;

    Nm_SanitizeGlyphs(BindCore_Row(row)->label, label, sizeof(label), 0);
    /* Bare "Map" is the PC Options start-map row's key, which Japanese words
     * as "start map". The leading space makes this row a key of its own and
     * draws nothing in a right-aligned label. */
    if (strcmp(label, "Map") == 0)
        snprintf(label, sizeof(label), "_Map");
    shown = cs_fit(label, CS_LABEL_RX, fit, sizeof(fit));
    Nm_TextDrawRight(CS_LABEL_RX, y, NM_TEXT_TALL, color, shown);
    Nm_TextDraw(CS_LABEL_RX, y, NM_TEXT_TALL, color, ":");

    for (c = 0; c < BC_COLS; c++)
        cs_cell_draw(row, c, y, selRow && c == s_col);

    if (selRow)
        Nm_HighlightDraw(CS_HL_X0, CS_HL_X1, y + 14);
}

static void cs_bullet_row_draw(int y, int selected, const char* label)
{
    Nm_BulletDraw(CS_TEXT_X - NM_BULLET_TEXT_DX, y, selected);
    Nm_TextDraw(CS_TEXT_X, y, NM_TEXT_TALL, StringColorId_White, label);
    if (selected)
        Nm_HighlightDraw(CS_TEXT_X - NM_BULLET_TEXT_DX - 1, CS_TEXT_X + Nm_TextWidth(label) + 4, y + 14);
}

void Pc_ControlsScreen_Draw(void)
{
    const char* setLabel;
    int         kbSel, padSel, i;

    if (!s_open)
        return;

    Nm_BlackBackgroundDraw();

    if (s_confirm.open)
    {
        NmConfirm_Draw(&s_confirm, "Reset_controls?", CS_CONFIRM_Y);
        return;
    }

    cs_bullet_row_draw(CS_ICONS_Y, s_focus == CS_FOCUS_ICONS, "Gamepad_Icons");
    setLabel = PadIcons_SetLabel(g_PcConfig.gamepadIcons);
    Nm_TextDrawRight(CS_VALUE_RX, CS_ICONS_Y, NM_TEXT_TALL, StringColorId_White, setLabel);
    Nm_ValueArrowsDraw(cs_value_left(), CS_VALUE_RX, CS_ICONS_Y);

    kbSel  = (s_focus == CS_FOCUS_LIST && s_col <= BC_KB2);
    padSel = (s_focus == CS_FOCUS_LIST && s_col >= BC_PAD1);
    Nm_RuleDraw(CS_RULE_X0, CS_RULE_X1, CS_RULE1_Y, 0);
    Nm_TextDrawCentered(CS_HEAD_FUNC_X, CS_HEAD_Y, NM_TEXT_TALL, StringColorId_LightGrey, "Function");
    Nm_TextDrawCentered((s_colX[BC_KB1] + s_colX[BC_KB2] + s_colW[BC_KB2]) / 2, CS_HEAD_Y, NM_TEXT_TALL,
                        kbSel ? StringColorId_Gold : StringColorId_LightGrey, "Keyboard");
    Nm_TextDrawCentered((s_colX[BC_PAD1] + s_colX[BC_PAD2] + s_colW[BC_PAD2]) / 2, CS_HEAD_Y, NM_TEXT_TALL,
                        padSel ? StringColorId_Gold : StringColorId_LightGrey, "Controller");
    Nm_RuleDraw(CS_RULE_X0, CS_RULE_X1, CS_RULE2_Y, 0);

    for (i = s_list.top; i < s_list.count && i < s_list.top + s_list.visible; i++)
        cs_row_draw(i, CS_LIST_Y + ((i - s_list.top) * CS_PITCH));

    if (s_list.count > s_list.visible)
    {
        Nm_ScrollArrowDraw(CS_ARROW_X, CS_ARROW_UP_Y, 1, NmScroll_CanUp(&s_list));
        Nm_ScrollArrowDraw(CS_ARROW_X, CS_ARROW_DOWN_Y, 0, NmScroll_CanDown(&s_list));
    }

    Nm_RuleDraw(CS_RULE_X0, CS_RULE_X1, CS_RULE3_Y, 0);
    cs_bullet_row_draw(CS_RESET_Y, s_focus == CS_FOCUS_RESET, "Reset");
}
