/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lang_quick.h"

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "lang_pack.h"
#include "lang_ru.h"
#include "lang_text.h"
#include "pc_config.h"

typedef struct {
    const char* en;
    const char* tr[4]; /* de, fr, es, it -- UTF-8 */
} s_QuickTranslation;

/* PAL columns for the QUICK. strings. The PC Options rows these overlays also
 * show come from lang_menu.c's s_MenuTr instead, so the two menus agree. */
static const s_QuickTranslation s_QuickTr[] = {
#include "lang_quick_pal.inc"
};

typedef struct {
    const char* en;
    const char* ja;
    const char* zh;
} s_QuickCjk;

/* The Japanese and Chinese packs store the game font's own bytes (Shift-JIS,
 * kuten), which these UTF-8 overlays cannot draw, and a Japanese disc's own two
 * languages have no pack at all -- so their overlay text is compiled in. */
#include "lang_quick_cjk.inc"

enum { QL_ENGLISH = 0, QL_RUSSIAN, QL_PACK, QL_PAL, QL_JA, QL_ZH };

int Pc_LangQuickScript(void)
{
    if (Pc_RuActive())
        return LANG_QUICK_SCRIPT_LATIN;
    if (Pc_LangPackActive())
    {
        int font = Pc_LangPackFont();

        return (font == LANG_PACK_FONT_SJIS)    ? LANG_QUICK_SCRIPT_JAPANESE
             : (font == LANG_PACK_FONT_CHINESE) ? LANG_QUICK_SCRIPT_CHINESE
                                                : LANG_QUICK_SCRIPT_LATIN;
    }
    if (g_GameRegion == Region_JPN)
    {
        return (g_PcConfig.jpLanguage == JP_LANG_JAPANESE) ? LANG_QUICK_SCRIPT_JAPANESE
             : (g_PcConfig.jpLanguage == JP_LANG_CHINESE)  ? LANG_QUICK_SCRIPT_CHINESE
                                                           : LANG_QUICK_SCRIPT_LATIN;
    }
    return LANG_QUICK_SCRIPT_LATIN;
}

int Pc_LangQuickFontPaths(const char** out, int max)
{
    /* Oswald carries Latin and Cyrillic, but no font the port ships has a kana
     * or a hanzi. A CJK face is far too large to ship for an overlay and every
     * Windows install already has one, so those go FIRST when the script needs
     * them -- the Chinese faces for Chinese, since a Japanese one lacks most
     * simplified forms -- with the Latin faces after, so a machine without any
     * CJK face still draws the English instead of nothing. */
    static const char* const ja[] = {
        "C:/Windows/Fonts/YuGothM.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    };
    static const char* const zh[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    };
    static const char* const latin[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    int script = Pc_LangQuickScript();
    int n      = 0;
    int i;

    if (script == LANG_QUICK_SCRIPT_JAPANESE)
        for (i = 0; i < (int)(sizeof(ja) / sizeof(ja[0])) && n < max; i++)
            out[n++] = ja[i];
    else if (script == LANG_QUICK_SCRIPT_CHINESE)
        for (i = 0; i < (int)(sizeof(zh) / sizeof(zh[0])) && n < max; i++)
            out[n++] = zh[i];
    for (i = 0; i < (int)(sizeof(latin) / sizeof(latin[0])) && n < max; i++)
        out[n++] = latin[i];
    return n;
}

static const char* CjkLookup(const s_QuickCjk* tbl, int count, const char* key, int zh)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (tbl[i].en[0] == key[0] && strcmp(tbl[i].en, key) == 0)
            return zh ? tbl[i].zh : tbl[i].ja;
    }
    return NULL;
}

/* Mirrors Pc_LangMenuText's order, so an overlay never disagrees with the
 * menu drawn behind it. */
static int QuickLang(void)
{
    int lang = g_PcConfig.language;

    switch (Pc_LangQuickScript())
    {
        case LANG_QUICK_SCRIPT_JAPANESE: return QL_JA;
        case LANG_QUICK_SCRIPT_CHINESE:  return QL_ZH;
        default:                         break;
    }
    if (Pc_RuActive())
        return QL_RUSSIAN;
    if (Pc_LangPackActive())
        return QL_PACK;
    if (g_GameRegion == Region_JPN)
        return QL_ENGLISH;
    if ((g_GameRegion == Region_EUR || (g_GameRegion == Region_USA && Pc_FanTextActive())) &&
        lang >= 1 && lang <= 4)
    {
        return QL_PAL;
    }
    return QL_ENGLISH;
}

#define RING_COUNT 8
#define RING_SIZE  512

static char* RingNext(void)
{
    static char s_Ring[RING_COUNT][RING_SIZE];
    static int  s_Slot;

    s_Slot = (s_Slot + 1) % RING_COUNT;
    return s_Ring[s_Slot];
}

unsigned int Pc_LangUtf8Next(const char** pp)
{
    const unsigned char* p = (const unsigned char*)*pp;
    unsigned int         c = *p++;

    if (c >= 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    }
    else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80)
    {
        c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
        p += 2;
    }
    else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80)
    {
        c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
        p += 1;
    }

    *pp = (const char*)p;
    return c;
}

static int Utf8Put(unsigned int cp, char* out, int room)
{
    if (cp < 0x80 && room >= 1)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800 && room >= 2)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && room >= 3)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    return 0;
}

/* Menu-dialect text -> what the overlay draws: '_' is the menu font's space,
 * and the PAL table is Latin-1. */
static const char* FromMenu(const char* s, int latin1)
{
    char* out = RingNext();
    int   n   = 0;

    while (*s != '\0' && n < RING_SIZE - 4)
    {
        unsigned int cp;

        if (*s == '_')
        {
            out[n++] = ' ';
            s++;
            continue;
        }
        if (*s == '\x01')
        {
            s++;
            continue;
        }
        if (latin1)
            cp = (unsigned char)*s++;
        else
            cp = Pc_LangUtf8Next(&s);
        n += Utf8Put(cp, out + n, RING_SIZE - 1 - n);
    }
    out[n] = '\0';
    return out;
}

const char* Pc_LangQuickMenu(const char* us)
{
    const char* tr = NULL;
    int         latin1 = 0;

    if (us == NULL)
        return "";

    switch (QuickLang())
    {
        case QL_RUSSIAN: tr = Pc_RuMenuUtf8(us); break;
        case QL_PACK:
            tr = Pc_LangPackMenuUtf8(us);
            if (tr == NULL)
            {
                tr     = Pc_LangMenuPal(us, -1); /* a de/fr/es/it pack's column */
                latin1 = (tr != NULL);
            }
            break;
        case QL_PAL:     tr = Pc_LangMenuPal(us, -1); latin1 = 1; break;
        case QL_JA:
        case QL_ZH:
        {
            const char* cjk = CjkLookup(s_MenuCjkTr, (int)(sizeof(s_MenuCjkTr) / sizeof(s_MenuCjkTr[0])),
                                        us, QuickLang() == QL_ZH);
            if (cjk != NULL)
                return cjk;
            break;
        }
        default:         break;
    }
    return FromMenu(tr ? tr : us, tr ? latin1 : 0);
}

/* The compiled de/fr/es/it overlay text, by column rather than by the retail
 * language number, so a pack of the same name reads it too (Pc_LangPalColumn).
 * The pack carries the story and the items; this text was written for the PAL
 * disc and is the same language, so there is nothing to duplicate. */
static const char* QuickPal(const char* en)
{
    int col = Pc_LangPalColumn();
    int i;

    if (col < 0)
        return NULL;

    for (i = 0; i < (int)(sizeof(s_QuickTr) / sizeof(s_QuickTr[0])); i++)
    {
        if (s_QuickTr[i].en[0] == en[0] && strcmp(s_QuickTr[i].en, en) == 0)
            return s_QuickTr[i].tr[col];
    }
    return NULL;
}

const char* Pc_LangQuick(const char* en)
{
    int i;

    if (en == NULL)
        return "";

    switch (QuickLang())
    {
        case QL_PACK:
        {
            char key[160];
            int  n = snprintf(key, sizeof(key), "QUICK.");
            const char* tr;

            for (i = 0; en[i] != '\0' && n < (int)sizeof(key) - 1; i++)
                key[n++] = (en[i] == ' ') ? '_' : (en[i] == '=') ? '-' : en[i];
            key[n] = '\0';
            tr = Pc_LangPackUtf8(key);
            if (tr != NULL && tr[0] != '\0')
                return tr;
            /* Not in the pack: a de/fr/es/it pack still has a column. */
            tr = QuickPal(en);
            return (tr != NULL) ? tr : en;
        }
        case QL_PAL:
        {
            const char* tr = QuickPal(en);

            return (tr != NULL) ? tr : en;
        }
        case QL_JA:
        case QL_ZH:
        {
            const char* tr = CjkLookup(s_QuickCjkTr, (int)(sizeof(s_QuickCjkTr) / sizeof(s_QuickCjkTr[0])),
                                       en, QuickLang() == QL_ZH);

            return (tr != NULL) ? tr : en;
        }
        default:
            return en;
    }
}

const char* Pc_LangQuickFill(const char* en, const char* placeholder, const char* value)
{
    const char* tmpl = Pc_LangQuick(en);
    const char* at   = strstr(tmpl, placeholder);
    char*       out  = RingNext();
    size_t      phLen = strlen(placeholder);

    if (at == NULL)
    {
        tmpl = en;
        at   = strstr(tmpl, placeholder);
        if (at == NULL)
        {
            snprintf(out, RING_SIZE, "%s", en);
            return out;
        }
    }
    snprintf(out, RING_SIZE, "%.*s%s%s", (int)(at - tmpl), tmpl, value ? value : "", at + phLen);
    return out;
}

static unsigned int UpperCp(unsigned int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 0x20;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return c - 0x20;
    /* Latin Extended-A pairs the cases in two parities around U+0138/U+0149. */
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c : c - 1;
    if (c >= 0x430 && c <= 0x44F)
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45F)
        return c - 0x50;
    return c;
}

void Pc_LangUtf8Upper(const char* in, char* out, int outSize)
{
    int n = 0;

    if (outSize <= 0)
        return;
    while (in != NULL && *in != '\0' && n < outSize - 4)
        n += Utf8Put(UpperCp(Pc_LangUtf8Next(&in)), out + n, outSize - 1 - n);
    out[n] = '\0';
}
