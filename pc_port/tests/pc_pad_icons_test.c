/* pc_pad_icons_test.c - gamepad icon mapping, cells, text fallback and art
 * detection. */
/* NDEBUG is set by the RelWithDebInfo build; this test IS its assertions. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_pad_icons.h"

static void test_pad_icons_map(void)
{
    assert(PadIcons_ForBind("a") == PI_CROSS);
    assert(PadIcons_ForBind("y") == PI_TRIANGLE);
    assert(PadIcons_ForBind("lefttrigger") == PI_L2);
    assert(PadIcons_ForBind("back") == PI_SELECT);
    assert(PadIcons_ForBind("dpleft") == PI_DPAD_LEFT);
    assert(PadIcons_ForBind("touchpad") == -1);
    assert(PadIcons_ForBind(NULL) == -1);
}

static void test_pad_icons_uv(void)
{
    int u = -1, v = -1;

    PadIcons_UV(PI_START, &u, &v);
    assert(u == 244 && v == 176);
    PadIcons_UV(PI_DPAD_UP, &u, &v);
    assert(u == 244 && v == 192);
    PadIcons_UV(PI_LS_LEFT, &u, &v);
    assert(u == 228 && v == 48);
    PadIcons_UV(PI_L2, &u, &v);
    assert(u == 244 && v == 0);
}

static void test_pad_icons_fallback(void)
{
    assert(strcmp(PadIcons_Fallback(PI_DPAD_UP, "dpup"), "D-PAD_U") == 0);
    assert(strcmp(PadIcons_Fallback(PI_LS_RIGHT, NULL), "LS_R") == 0);
    assert(strcmp(PadIcons_Fallback(-1, "touchpad"), "Touchpad") == 0);
    assert(strcmp(PadIcons_Fallback(-1, "paddle1"), "Paddle1") == 0);
    assert(PadIcons_Fallback(-1, NULL)[0] == '\0');
}

static void test_pad_icons_fallback_short(void)
{
    assert(strcmp(PadIcons_FallbackShort(PI_DPAD_UP, "dpup"), "DP_U") == 0);
    assert(strcmp(PadIcons_FallbackShort(PI_DPAD_RIGHT, NULL), "DP_R") == 0);
    assert(strcmp(PadIcons_FallbackShort(PI_DPAD_DOWN, NULL), "DP_D") == 0);
    assert(strcmp(PadIcons_FallbackShort(PI_DPAD_LEFT, "dpleft"), "DP_L") == 0);
    assert(strcmp(PadIcons_FallbackShort(PI_LS_UP, NULL), "LS_U") == 0);
    assert(strcmp(PadIcons_FallbackShort(PI_TRIANGLE, "y"), "Triangle") == 0);
    assert(strcmp(PadIcons_FallbackShort(-1, "touchpad"), "Touchpad") == 0);
    assert(PadIcons_FallbackShort(-1, NULL)[0] == '\0');
}

static void put_opaque(unsigned char* rgba, int w, int x, int y)
{
    rgba[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] = 255;
}

static void test_pad_icons_has_art(void)
{
    int            cu, cv, du, dv, scale;
    unsigned char* rgba;

    PadIcons_UV(PI_CIRCLE, &cu, &cv);
    PadIcons_UV(PI_DPAD_UP, &du, &dv);
    for (scale = 1; scale <= 2; scale++)
    {
        int w = 128 * scale, h = 256 * scale;

        rgba = (unsigned char*)calloc((size_t)w * (size_t)h, 4);
        assert(rgba != NULL);
        put_opaque(rgba, w, (cu - 128 + 11) * scale + (scale - 1), (cv + 11) * scale + (scale - 1));
        assert(PadIcons_CellHasArt(rgba, w, h, cu, cv) == 1);
        assert(PadIcons_CellHasArt(rgba, w, h, du, dv) == 0);
        free(rgba);
    }
    assert(PadIcons_CellHasArt(NULL, 128, 256, cu, cv) == 0);

    /* Not a 128x256 multiple: refuse rather than read a mis-scaled cell. */
    rgba = (unsigned char*)calloc(100 * 200, 4);
    assert(rgba != NULL);
    memset(rgba, 255, 100 * 200 * 4);
    assert(PadIcons_CellHasArt(rgba, 100, 200, cu, cv) == 0);
    free(rgba);
}

static void test_pad_icons_labels(void)
{
    assert(strcmp(PadIcons_SetLabel(PAD_ICONS_ORIGINAL), "Original") == 0);
    assert(strcmp(PadIcons_SetLabel(PAD_ICONS_XBOX), "Xbox") == 0);
    assert(strcmp(PadIcons_SetLabel(PAD_ICONS_NINTENDO), "Nintendo") == 0);
    assert(strcmp(PadIcons_SetLabel(7), "Original") == 0);
}

int main(void)
{
    test_pad_icons_map();
    test_pad_icons_uv();
    test_pad_icons_fallback();
    test_pad_icons_fallback_short();
    test_pad_icons_has_art();
    test_pad_icons_labels();
    printf("pc_pad_icons_test: all passed\n");
    return 0;
}
