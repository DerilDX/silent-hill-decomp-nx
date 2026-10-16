/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_freecam_hud.c - controls panel shown while the free camera is on.
 *
 * Self-contained GL in the same arrangement as pc_ra_toast.c (own program, VAO,
 * textures, full state save/restore), drawn from the dbg_overlay post-capture
 * hook so it never lands in the scene the camera is filming. H hides it.
 */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "sh_log.h"
#include "pc_config.h"
#include "stb_truetype.h"

extern int   g_DebugCamEnabled;
extern int   g_PcFreeCamHudHidden;
extern int   g_PcFreeCamSteady;
extern int   g_PcFreeCamCruise;
extern float g_PcFreeCamSpeedMul;

static GLuint s_prog, s_vao, s_vbo, s_texWhite, s_texText;
static GLint  s_locTex, s_locColor;
static int    s_glReady;
static int    s_texW, s_texH;
static char   s_bakedText[1024];
static int    s_bakedPx;

static stbtt_fontinfo s_font;
static int            s_fontOk, s_fontTried;

static GLuint fch_make_shader(GLenum type, const char* src)
{
    GLuint      sh = glCreateShader(type);
    GLint       ok = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };
    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[FREECAMHUD] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint fch_upload(const unsigned char* rgba, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

static void fch_gl_init(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    v_uv = a_uv;\n"
        "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(u_tex, v_uv) * u_color;\n"
        "}\n";
    static const unsigned char white[4] = { 255, 255, 255, 255 };
    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0;

    s_glReady = -1;
    vs = fch_make_shader(GL_VERTEX_SHADER, vs_src);
    fs = fch_make_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    s_locTex   = glGetUniformLocation(s_prog, "u_tex");
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    /* Called inside Draw's save/restore, but PsyCross caches its bindings, so
     * hand back exactly what was bound (see pc_ra_toast.c toast_gl_init). */
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);

    s_texWhite = fch_upload(white, 1, 1);
    s_glReady  = 1;
}

static void fch_quad(GLuint tex, float x0, float yTop, float x1, float yBot, float r, float g, float b, float a)
{
    float v[6][4] = {
        { x0, yTop, 0.0f, 0.0f }, { x0, yBot, 0.0f, 1.0f }, { x1, yTop, 1.0f, 0.0f },
        { x1, yTop, 1.0f, 0.0f }, { x0, yBot, 0.0f, 1.0f }, { x1, yBot, 1.0f, 1.0f },
    };
    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* Reads the font off disk; game-loop only (pc_ra_toast.c Pc_RaToast_Preload
 * explains why a read from the draw hook can wedge a load). */
void Pc_FreeCamHud_Preload(void)
{
    static const char* paths[] = {
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    int i;

    if (s_fontTried)
        return;
    s_fontTried = 1;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        FILE*          f = fopen(paths[i], "rb");
        long           n;
        unsigned char* data;
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = (n > 0) ? (unsigned char*)malloc((size_t)n) : NULL;
        if (data && fread(data, 1, (size_t)n, f) == (size_t)n &&
            stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            /* stbtt keeps pointers into the buffer for the font's lifetime. */
            fclose(f);
            s_fontOk = 1;
            return;
        }
        free(data);
        fclose(f);
    }
    SH_DBG("[FREECAMHUD] no usable font - controls panel disabled");
}

#define FCH_ROWS 9

/* Rows are "key\tdescription"; the key column is right-aligned so the
 * descriptions line up. White glyphs, dark outline, straight alpha. */
static GLuint fch_bake(const char* rows[FCH_ROWS], int count, float px, int* outW, int* outH)
{
    const float scale   = stbtt_ScaleForPixelHeight(&s_font, px);
    const int   outline = px >= 22.0f ? 2 : 1;
    const float gap     = px * 0.8f;
    float       keyW = 0.0f, descW = 0.0f, lineH = px * 1.25f;
    int         asc, desc, lgap, W, H, i, x, y;
    unsigned char *cov, *rgba, *scratch;
    GLuint      tex;

    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &lgap);
    for (i = 0; i < count; i++)
    {
        const char* tab = strchr(rows[i], '\t');
        float       w[2] = { 0.0f, 0.0f };
        const char* p;
        int         col = 0;
        for (p = rows[i]; *p; p++)
        {
            int adv, lsb;
            if (*p == '\t') { col = 1; continue; }
            stbtt_GetCodepointHMetrics(&s_font, (unsigned char)*p, &adv, &lsb);
            w[col] += adv * scale;
        }
        if (tab == NULL) { w[1] = w[0]; w[0] = 0.0f; }
        if (w[0] > keyW) keyW = w[0];
        if (w[1] > descW) descW = w[1];
    }
    W = (int)(keyW + gap + descW) + 2 * outline + 4;
    H = (int)(lineH * count) + 2 * outline + 4;
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096)
        return 0;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)(px * 3) * (size_t)(px * 3) + 16);
    if (!cov || !rgba || !scratch)
    {
        free(cov); free(rgba); free(scratch);
        return 0;
    }

    for (i = 0; i < count; i++)
    {
        const char* tab   = strchr(rows[i], '\t');
        const float baseY = outline + 2 + lineH * i + asc * scale;
        float       penX;
        const char* p;

        /* Key column right-aligned against the gap; header rows span both. */
        if (tab)
        {
            float w = 0.0f;
            for (p = rows[i]; p < tab; p++)
            {
                int adv, lsb;
                stbtt_GetCodepointHMetrics(&s_font, (unsigned char)*p, &adv, &lsb);
                w += adv * scale;
            }
            penX = outline + 2 + keyW - w;
        }
        else
            penX = outline + 2;

        for (p = rows[i]; *p; p++)
        {
            int cp = (unsigned char)*p, gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
            if (*p == '\t')
            {
                penX = outline + 2 + keyW + gap;
                continue;
            }
            stbtt_GetCodepointBitmapBox(&s_font, cp, scale, scale, &gx0, &gy0, &gx1, &gy1);
            gw = gx1 - gx0;
            gh = gy1 - gy0;
            if (gw > 0 && gh > 0 && gw < px * 3 && gh < px * 3)
            {
                stbtt_MakeCodepointBitmap(&s_font, scratch, gw, gh, gw, scale, scale, cp);
                for (sy = 0; sy < gh; sy++)
                {
                    int dy = (int)baseY + gy0 + sy;
                    if (dy < 0 || dy >= H) continue;
                    for (sx = 0; sx < gw; sx++)
                    {
                        int           dx = (int)penX + gx0 + sx;
                        unsigned char v  = scratch[sy * gw + sx];
                        if (dx < 0 || dx >= W) continue;
                        if (v > cov[dy * W + dx]) cov[dy * W + dx] = v;
                    }
                }
            }
            stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
            penX += adv * scale;
        }
    }

    for (y = 0; y < H; y++)
    {
        for (x = 0; x < W; x++)
        {
            unsigned char best = 0;
            int           ox, oy;
            float         aT, aO, a;
            for (oy = -outline; oy <= outline; oy++)
            {
                if (y + oy < 0 || y + oy >= H) continue;
                for (ox = -outline; ox <= outline; ox++)
                {
                    if (x + ox < 0 || x + ox >= W) continue;
                    if (cov[(y + oy) * W + x + ox] > best) best = cov[(y + oy) * W + x + ox];
                }
            }
            aT = cov[y * W + x] / 255.0f;
            aO = best / 255.0f;
            a  = aT + aO * (1.0f - aT);
            rgba[(y * W + x) * 4 + 0] = rgba[(y * W + x) * 4 + 1] = rgba[(y * W + x) * 4 + 2] =
                (a > 0.0f) ? (unsigned char)(255.0f * aT / a) : 0;
            rgba[(y * W + x) * 4 + 3] = (unsigned char)(255.0f * a);
        }
    }

    tex   = fch_upload(rgba, W, H);
    *outW = W;
    *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

void Pc_FreeCamHud_Draw(void)
{
    extern int g_PcQuickOptionsActive;
    extern int g_PcConsoleInputActive;
    GLint      vp[4];
    float      vpW, vpH, margin, pad, left, bottom;
    int        px, i;
    char       text[1024];
    char       hdr[64], cruise[64];
    const char* rows[FCH_ROWS];
    GLint      prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint      prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint      prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean  prevBlend, prevDepth, prevCull;

    if (!g_DebugCamEnabled || g_PcFreeCamHudHidden || g_GameWork.gameState != GameState_InGame ||
        g_PcQuickOptionsActive || g_PcConsoleInputActive || !s_fontOk)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW = (float)vp[2];
    vpH = (float)vp[3];

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    if (!s_glReady)
        fch_gl_init();
    if (s_glReady != 1)
        goto restore;

    snprintf(hdr, sizeof(hdr), "FREE CAMERA      speed x%.2f%s", g_PcFreeCamSpeedMul,
             g_PcFreeCamSteady ? "   STEADY" : "");
    snprintf(cruise, sizeof(cruise), ",\tcruise control: %s", g_PcFreeCamCruise ? "ON" : "off");
    rows[0]  = hdr;
    rows[1]  = "W A S D\tmove";
    rows[2]  = "E / Q\tup / down";
    rows[3]  = "Mouse\tlook";
    rows[4]  = "Wheel\tchange speed";
    rows[5]  = "Shift / Ctrl\tfast / slow (hold)";
    rows[6]  = "Space\tsteady: straight line, slow and smooth (hold)";
    rows[7]  = cruise;
    rows[8]  = "H\thide this panel";

    px = (int)(0.022f * vpH);
    if (px < 12) px = 12;
    text[0] = '\0';
    for (i = 0; i < FCH_ROWS; i++)
    {
        strncat(text, rows[i], sizeof(text) - strlen(text) - 2);
        strncat(text, "\n", sizeof(text) - strlen(text) - 1);
    }
    if (!s_texText || px != s_bakedPx || strcmp(text, s_bakedText) != 0)
    {
        if (s_texText)
            glDeleteTextures(1, &s_texText);
        s_texText = fch_bake(rows, FCH_ROWS, (float)px, &s_texW, &s_texH);
        s_bakedPx = px;
        strncpy(s_bakedText, text, sizeof(s_bakedText) - 1);
    }
    if (!s_texText)
        goto restore;

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glUniform1i(s_locTex, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* PsyCross can leave GL_FUNC_REVERSE_SUBTRACT behind (pc_ra_toast.c). */
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

#define NX(p_) (-1.0f + 2.0f * (p_) / vpW)
#define NY(p_) (-1.0f + 2.0f * (p_) / vpH)
    margin = 0.03f * vpH;
    pad    = 0.012f * vpH;
    left   = margin;
    bottom = margin;
    fch_quad(s_texWhite, NX(left), NY(bottom + s_texH + 2 * pad), NX(left + s_texW + 2 * pad), NY(bottom),
             0.0f, 0.0f, 0.0f, 0.45f);
    fch_quad(s_texText, NX(left + pad), NY(bottom + pad + s_texH), NX(left + pad + s_texW), NY(bottom + pad),
             1.0f, 1.0f, 1.0f, 0.95f);
#undef NX
#undef NY

restore:
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glUseProgram((GLuint)prevProg);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

/* FPS counter (quick menu Debug Actions > FPS Display). Shares the panel's GL
 * objects and font; counts the frames this hook sees, which is one per
 * presented frame, and refreshes the number twice a second. */
static GLuint s_texFps;
static int    s_fpsW, s_fpsH, s_fpsPx;
static char   s_fpsBaked[32];

void Pc_FpsHud_Draw(void)
{
    static Uint64 s_winStart  = 0;
    static int    s_winFrames = 0;
    static int    s_fps       = 0;
    const Uint64  now         = SDL_GetPerformanceCounter();
    const Uint64  freq        = SDL_GetPerformanceFrequency();
    const int     corner      = g_PcConfig.fpsDisplay;
    GLint         vp[4];
    float         vpW, vpH, margin, pad, x, y, boxW, boxH;
    int           px;
    char          text[32];
    const char*   rows[1];
    GLint      prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint      prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint      prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean  prevBlend, prevDepth, prevCull;

    if (corner < 1 || corner > 4)
    {
        s_winStart = 0;
        return;
    }

    if (s_winStart == 0)
    {
        s_winStart  = now;
        s_winFrames = 0;
    }
    s_winFrames++;
    if (now - s_winStart >= freq / 2)
    {
        s_fps       = (int)((double)s_winFrames * (double)freq / (double)(now - s_winStart) + 0.5);
        s_winStart  = now;
        s_winFrames = 0;
    }
    if (!s_fontOk)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW = (float)vp[2];
    vpH = (float)vp[3];

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    if (!s_glReady)
        fch_gl_init();
    if (s_glReady != 1)
        goto restore;

    snprintf(text, sizeof(text), "%d FPS", s_fps);
    px = (int)(0.026f * vpH);
    if (px < 12) px = 12;
    if (!s_texFps || px != s_fpsPx || strcmp(text, s_fpsBaked) != 0)
    {
        if (s_texFps)
            glDeleteTextures(1, &s_texFps);
        rows[0]  = text;
        s_texFps = fch_bake(rows, 1, (float)px, &s_fpsW, &s_fpsH);
        s_fpsPx  = px;
        strncpy(s_fpsBaked, text, sizeof(s_fpsBaked) - 1);
    }
    if (!s_texFps)
        goto restore;

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glUniform1i(s_locTex, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

#define NX(p_) (-1.0f + 2.0f * (p_) / vpW)
#define NY(p_) (-1.0f + 2.0f * (p_) / vpH)
    margin = 0.02f * vpH;
    pad    = 0.008f * vpH;
    boxW   = s_fpsW + 2 * pad;
    boxH   = s_fpsH + 2 * pad;
    x = (corner == 2 || corner == 4) ? vpW - margin - boxW : margin;
    y = (corner == 3 || corner == 4) ? margin : vpH - margin - boxH; /* bottom edge, GL y-up */
    fch_quad(s_texWhite, NX(x), NY(y + boxH), NX(x + boxW), NY(y), 0.0f, 0.0f, 0.0f, 0.45f);
    fch_quad(s_texFps, NX(x + pad), NY(y + pad + s_fpsH), NX(x + pad + s_fpsW), NY(y + pad),
             1.0f, 1.0f, 1.0f, 0.95f);
#undef NX
#undef NY

restore:
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glUseProgram((GLuint)prevProg);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}
