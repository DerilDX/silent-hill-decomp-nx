/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Skinned glTF character replacements -- see pc_skin_chara.h.
 *
 * Load: the skin's joints map onto game bones by name; each game bone then
 * gets a retarget transform taking the model's bind pose onto the character's
 * rest pose (bone direction swung onto the model's, joint moved onto the
 * model's joint, segment length scaled to the game's), so every vertex is
 * stored as up to four bone-local positions.
 *
 * Frame: the skeleton drawer hands over each bone's view matrix; vertices are
 * blended from those, projected like the GTE, lit by the game's own per-bone
 * light setup, and emitted as one modern-mesh packet per material. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "inline_no_dmpsx.h"
#include <psyq/gtemac.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/chara/chara_model.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/formats/lm.h"
#include "main/fileinfo.h"

#include "cgltf.h"

#include "PsyX/PsyX_render.h"
#include "psx/gtereg.h"
#include "psx/libgpu.h"
#include "psyq/libgs.h"
#include "hires_override.h"
#include "pc_config.h"
#include "pc_modern_mesh.h"
#include "pc_modern_vertex.h"
#include "pc_skin_chara.h"
#include "sh_log.h"

extern FILE* Pc_LooseFOpen(const char* path, const char* mode);
extern float PGXP_GetSzMax(void);
extern void  PsyX_NoteItemDepthSz(unsigned int sz);
extern void  func_80057228(MATRIX* mat, s32 alpha, SVECTOR* arg2, VECTOR3* arg3);
extern s_WorldEnvWork g_WorldEnvWork;

#define PC_SKIN_BONES     18
#define PC_SKIN_MAX_CHARA 8
#define PC_SKIN_MAX_MATS  32
#define PC_SKIN_INFL      4
/* Characters sit at func_800571D0(field_B_1 = 0)'s OT base. */
#define PC_SKIN_OT_BASE   2

typedef struct
{
    s16   R[9];  /* bone -> model rotation, Q12, row-major */
    s16   T[3];  /* joint, PSX model space */
    float C[3];  /* rest-pose centroid of the bone's stock geometry */
} PcSkinRestBone;

/* HERO rest pose: HB_BASE.ANM keyframe 0, the pose the ILM converter exports
 * (HERO.ilmmeta.json restPose per part; centroids from that same HERO.obj). */
#define HERO_SOLE_Y (-9.4f)
static const PcSkinRestBone s_heroRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, -32, -96, 32, 4064, -64, 96, 64, 4064 }, { 0, -279, 0 }, { 0.5f, -322.1f, -6.4f } },
    { { 4036, -148, 124, 130, 4006, 383, -126, -385, 4012 }, { 0, -391, -6 }, { 1.2f, -407.5f, 7.7f } },
    { { 131, -44, 4031, -764, 3949, 94, -3950, -765, 128 }, { 0, -359, -2 }, { -35.7f, -357.2f, -7.1f } },
    { { 2822, -2809, -505, 273, -470, 3965, -2803, -2832, -144 }, { -40, -360, -4 }, { -46.5f, -339.9f, -8.0f } },
    { { 3100, -2487, 173, 196, 509, 3920, -2445, -3084, 525 }, { -47, -310, -6 }, { -50.9f, -285.7f, -5.2f } },
    { { 2027, -3378, 171, 357, 405, 3889, -3334, -2027, 520 }, { -48, -249, 4 }, { -46.5f, -227.0f, 10.4f } },
    { { -124, 77, 4038, -511, 4010, -92, -4015, -507, -129 }, { 0, -359, -2 }, { 33.7f, -357.3f, -7.3f } },
    { { 3882, 985, 75, 52, -544, 3974, 974, -3859, -535 }, { 39, -360, -4 }, { 41.1f, -339.9f, -11.6f } },
    { { 3720, 1419, 136, -333, 414, 3954, 1379, -3720, 496 }, { 39, -310, -11 }, { 44.3f, -285.6f, -12.0f } },
    { { 3928, 237, -456, 397, 490, 3919, 262, -3926, 468 }, { 44, -249, -2 }, { 44.5f, -226.3f, 2.9f } },
    { { -4064, 32, 192, 64, 4064, 64, -192, 96, -4064 }, { 0, -279, 0 }, { 0.4f, -259.8f, -4.7f } },
    { { -4008, -435, 157, -415, 4011, -159, -137, -151, -4039 }, { -16, -236, 0 }, { -22.9f, -186.8f, -9.7f } },
    { { -3977, -380, 256, -412, 3815, -1125, -136, -1124, -3845 }, { -26, -146, -4 }, { -32.4f, -93.0f, -27.2f } },
    { { -3708, -6, 1410, -175, 3934, -512, -1398, -514, -3687 }, { -36, -44, -43 }, { -44.1f, -26.4f, -37.9f } },
    { { 4044, -31, 63, 4, 4033, -36, -62, 60, 4045 }, { 16, -236, 1 }, { 17.4f, -186.4f, -5.4f } },
    { { 4012, -38, 58, 3, 3974, 437, -62, -415, 3988 }, { 15, -146, 2 }, { 15.7f, -89.3f, -11.1f } },
    { { 3873, -188, 843, 134, 3959, 20, -862, 30, 3893 }, { 13, -39, -18 }, { 19.1f, -22.1f, -9.4f } },
};

/* Where each bone's segment ends: a child joint, the midpoint of two (the
 * pelvis), or -1 for an end bone measured to its geometry's centroid. */
static const s8 s_childA[PC_SKIN_BONES] = { -1, 2, -1, 4, 5, 6, -1, 8, 9, 10, -1, 12, 13, 14, -1, 16, 17, -1 };
static const s8 s_childB[PC_SKIN_BONES] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 15, -1, -1, -1, -1, -1, -1 };
/* Limb segments follow the game's lengths. */
static const u8 s_scaled[PC_SKIN_BONES] = { 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0 };
/* Limb joints move onto the game's, so elbows and knees bend where the
 * animation bends them and a held weapon lands in the hand. The torso, head,
 * clavicles and feet only turn about the game's pivots: their joints sit at
 * different heights on a modern rig (Mixamo's Hips is the pelvis, the game's
 * is the waist), and moving them there lifted the pelvis, stretched the neck
 * and stood the feet off the floor. */
static const u8 s_pinned[PC_SKIN_BONES] = { 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0 };

/* Mixamo base name -> game bone. The game's "L" parts sit at -X, which is the
 * model's own right side, so Mixamo "Right" drives them. */
static const struct { const char* name; s8 bone; } s_jointMap[] = {
    { "Hips", 11 }, { "Spine", 1 }, { "Spine1", 1 }, { "Spine2", 1 }, { "Spine3", 1 },
    { "Neck", 2 }, { "Neck1", 2 }, { "Head", 2 }, { "HeadTop_End", 2 },
    { "RightShoulder", 3 }, { "RightArm", 4 }, { "RightForeArm", 5 }, { "RightHand", 6 },
    { "LeftShoulder", 7 }, { "LeftArm", 8 }, { "LeftForeArm", 9 }, { "LeftHand", 10 },
    { "RightUpLeg", 12 }, { "RightLeg", 13 }, { "RightFoot", 14 }, { "RightToeBase", 14 }, { "RightToe_End", 14 },
    { "LeftUpLeg", 15 }, { "LeftLeg", 16 }, { "LeftFoot", 17 }, { "LeftToeBase", 17 }, { "LeftToe_End", 17 },
};

typedef struct
{
    float l[PC_SKIN_INFL][3]; /* bone-local position per influence */
    float w[PC_SKIN_INFL];
    u8    bone[PC_SKIN_INFL];
    u8    n;
    u8    dom;
    SVECTOR gteNormal;        /* dominant-bone-local, the ILM's inward sign */
} PcSkinVert;

typedef struct
{
    u32   v[3];
    float uv[3][2];
} PcSkinTri;

typedef struct
{
    PcSkinTri*      tris;
    int             triCount;
    unsigned char*  rgba;
    int             w, h;
    u32             texture;
    GrModernVertex* out[2];
    int             outCount[2];
} PcSkinMat;

typedef struct
{
    char        name[16];
    int         state; /* 0 untried, 1 ready, -1 none */
    int         vertCount;
    PcSkinVert* verts;
    int         matCount;
    PcSkinMat   mats[PC_SKIN_MAX_MATS];
    u32*        byBone[PC_SKIN_BONES];
    int         byBoneCount[PC_SKIN_BONES];

    /* frame state */
    float       rv[PC_SKIN_BONES][9];
    float       tv[PC_SKIN_BONES][3];
    u8          have[PC_SKIN_BONES];
    u8          lit[PC_SKIN_BONES];
    float       rw[PC_SKIN_BONES][9]; /* bone -> world, for the ragdoll */
    float       tw[PC_SKIN_BONES][3];
    int         isPlayer;
    CVECTOR*    color;
    float*      pos;  /* view x,y,z */
    float*      scr;  /* screen x,y */
    u8*         ok;
} PcSkinChara;

static PcSkinChara s_chara[PC_SKIN_MAX_CHARA];
static int         s_charaCount;

/* ---------------------------------------------------------------- math */

static void M3_FromQ12(const s16* q, float* m)
{
    int i;
    for (i = 0; i < 9; i++)
        m[i] = q[i] / 4096.0f;
}

static void M3_Mul(const float* a, const float* b, float* o)
{
    int r, c;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

static void M3_MulTV(const float* m, const float* v, float* o) /* m^T v */
{
    o[0] = m[0] * v[0] + m[3] * v[1] + m[6] * v[2];
    o[1] = m[1] * v[0] + m[4] * v[1] + m[7] * v[2];
    o[2] = m[2] * v[0] + m[5] * v[1] + m[8] * v[2];
}

static void M3_MulV(const float* m, const float* v, float* o)
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

static float V3_Len(const float* v)
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* Shortest rotation taking direction a onto direction b. */
static void M3_Align(const float* a0, const float* b0, float* m)
{
    float a[3], b[3], v[3], c, s, k;
    float la = V3_Len(a0), lb = V3_Len(b0);
    int i;

    for (i = 0; i < 9; i++)
        m[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    if (la < 1e-6f || lb < 1e-6f)
        return;
    for (i = 0; i < 3; i++)
    {
        a[i] = a0[i] / la;
        b[i] = b0[i] / lb;
    }
    v[0] = a[1] * b[2] - a[2] * b[1];
    v[1] = a[2] * b[0] - a[0] * b[2];
    v[2] = a[0] * b[1] - a[1] * b[0];
    c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    s = V3_Len(v);
    if (s < 1e-6f)
        return; /* parallel; antiparallel never occurs between a rest bone and its model twin */
    k = (1.0f - c) / (s * s);
    m[0] = 1 - k * (v[1] * v[1] + v[2] * v[2]); m[1] = -v[2] + k * v[0] * v[1];      m[2] = v[1] + k * v[0] * v[2];
    m[3] = v[2] + k * v[0] * v[1];              m[4] = 1 - k * (v[0] * v[0] + v[2] * v[2]); m[5] = -v[0] + k * v[1] * v[2];
    m[6] = -v[1] + k * v[0] * v[2];             m[7] = v[0] + k * v[1] * v[2];      m[8] = 1 - k * (v[0] * v[0] + v[1] * v[1]);
}

/* Column-major glTF 4x4 inverse of a rigid-plus-scale bind matrix; only the
 * translation of the inverse is needed (the joint's bind position). */
static int M4_InvTranslation(const float* m, float* t)
{
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    float inv[9];
    float tx = m[12], ty = m[13], tz = m[14];

    if (fabsf(det) < 1e-12f)
        return 0;
    inv[0] = (e * i - f * h) / det; inv[1] = (c * h - b * i) / det; inv[2] = (b * f - c * e) / det;
    inv[3] = (f * g - d * i) / det; inv[4] = (a * i - c * g) / det; inv[5] = (c * d - a * f) / det;
    inv[6] = (d * h - e * g) / det; inv[7] = (b * g - a * h) / det; inv[8] = (a * e - b * d) / det;
    t[0] = -(inv[0] * tx + inv[1] * ty + inv[2] * tz);
    t[1] = -(inv[3] * tx + inv[4] * ty + inv[5] * tz);
    t[2] = -(inv[6] * tx + inv[7] * ty + inv[8] * tz);
    return 1;
}

/* ---------------------------------------------------------------- loading */

static int Skin_JointBone(const char* full)
{
    char base[64];
    const char* s = strrchr(full, ':');
    size_t n, i;

    s = s ? s + 1 : full;
    snprintf(base, sizeof(base), "%s", s);
    /* Sketchfab appends "_<index>" to every node name. */
    n = strlen(base);
    i = n;
    while (i > 0 && base[i - 1] >= '0' && base[i - 1] <= '9')
        i--;
    if (i > 0 && i < n && base[i - 1] == '_')
        base[i - 1] = '\0';
    for (i = 0; i < sizeof(s_jointMap) / sizeof(s_jointMap[0]); i++)
        if (strcmp(base, s_jointMap[i].name) == 0)
            return s_jointMap[i].bone;
    if (strncmp(base, "RightHand", 9) == 0)
        return 6;
    if (strncmp(base, "LeftHand", 8) == 0)
        return 10;
    return -1;
}

static cgltf_result Skin_FileRead(const struct cgltf_memory_options* mem, const struct cgltf_file_options* file,
                                  const char* path, cgltf_size* size, void** data)
{
    FILE* f = Pc_LooseFOpen(path, "rb");
    long n;
    void* bytes;

    (void)mem;
    (void)file;
    if (f == NULL)
        return cgltf_result_file_not_found;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes = (n > 0) ? malloc((size_t)n) : NULL;
    if (bytes == NULL || fread(bytes, 1, (size_t)n, f) != (size_t)n)
    {
        free(bytes);
        fclose(f);
        return cgltf_result_io_error;
    }
    fclose(f);
    *size = (cgltf_size)n;
    *data = bytes;
    return cgltf_result_success;
}

static void Skin_FileRelease(const struct cgltf_memory_options* mem, const struct cgltf_file_options* file, void* data)
{
    (void)mem;
    (void)file;
    free(data);
}

static void Skin_LoadImage(PcSkinMat* mat, const cgltf_material* m, const char* gltfPath)
{
    const cgltf_texture* tex;
    const cgltf_image* img;
    unsigned char* bytes = NULL;
    cgltf_size size = 0;
    int owned = 0;

    if (m == NULL || !m->has_pbr_metallic_roughness)
        return;
    tex = m->pbr_metallic_roughness.base_color_texture.texture;
    if (tex == NULL || (img = tex->image) == NULL)
        return;
    if (img->buffer_view != NULL && img->buffer_view->buffer->data != NULL)
    {
        bytes = (unsigned char*)img->buffer_view->buffer->data + img->buffer_view->offset;
        size = img->buffer_view->size;
    }
    else if (img->uri != NULL && strncmp(img->uri, "data:", 5) != 0)
    {
        char path[512], uri[256];
        const char* slash = strrchr(gltfPath, '/');
        void* data = NULL;
        cgltf_options opt;

        snprintf(uri, sizeof(uri), "%s", img->uri);
        cgltf_decode_uri(uri);
        snprintf(path, sizeof(path), "%.*s%s", slash ? (int)(slash - gltfPath + 1) : 0, gltfPath, uri);
        memset(&opt, 0, sizeof(opt));
        if (Skin_FileRead(&opt.memory, &opt.file, path, &size, &data) != cgltf_result_success)
        {
            SH_DBG("[SKIN] texture %s not found", path);
            return;
        }
        bytes = (unsigned char*)data;
        owned = 1;
    }
    if (bytes != NULL && size > 0 && size < 0xffffffffu &&
        HiresOverride_DecodeToRGBA(bytes, (unsigned)size, &mat->rgba, &mat->w, &mat->h) != 0)
    {
        SH_DBG("[SKIN] texture for material %s did not decode (PNG only)", m->name ? m->name : "?");
        mat->rgba = NULL;
    }
    if (owned)
        free(bytes);
}

static int Skin_Load(PcSkinChara* ch, const char* path)
{
    cgltf_options opt;
    cgltf_data* data = NULL;
    const cgltf_skin* skin;
    float jointPos[128][3];
    s8 jointBone[128];
    float minY = 1e30f, maxY = -1e30f, scale, offX, offY, offZ;
    float hipsPos[3] = { 0, 0, 0 };
    int hipsFound = 0;
    float armY = 0.0f;
    int armCount = 0;
    size_t ni, pi, j;
    int totalVerts = 0;
    float anchor[PC_SKIN_BONES][3];
    int anchorSet[PC_SKIN_BONES];
    float dirSum[PC_SKIN_BONES][3];
    int dirCount[PC_SKIN_BONES];
    float R[PC_SKIN_BONES][9], axis[PC_SKIN_BONES][3], k[PC_SKIN_BONES];
    float* gpos = NULL;
    float* gnrm = NULL;
    int b;

    memset(&opt, 0, sizeof(opt));
    opt.file.read = Skin_FileRead;
    opt.file.release = Skin_FileRelease;
    if (cgltf_parse_file(&opt, path, &data) != cgltf_result_success)
        return 0;
    if (cgltf_load_buffers(&opt, data, path) != cgltf_result_success || data->skins_count == 0)
    {
        SH_DBG("[SKIN] %s: buffers missing or no skin", path);
        cgltf_free(data);
        return 0;
    }
    skin = &data->skins[0];
    if (skin->joints_count == 0 || skin->joints_count > 128 || skin->inverse_bind_matrices == NULL)
    {
        SH_DBG("[SKIN] %s: skin has %u joints / no inverse bind matrices", path, (unsigned)skin->joints_count);
        cgltf_free(data);
        return 0;
    }

    /* Joints: game bone by name, falling back to the nearest mapped ancestor. */
    for (j = 0; j < skin->joints_count; j++)
    {
        float ibm[16];
        const cgltf_node* n = skin->joints[j];
        int bone = -1;

        cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibm, 16);
        if (!M4_InvTranslation(ibm, jointPos[j]))
            jointPos[j][0] = jointPos[j][1] = jointPos[j][2] = 0.0f;
        for (; n != NULL && bone < 0; n = n->parent)
            bone = n->name ? Skin_JointBone(n->name) : -1;
        jointBone[j] = (s8)bone;
        if (skin->joints[j]->name != NULL)
        {
            int own = Skin_JointBone(skin->joints[j]->name);
            const cgltf_node* par = skin->joints[j]->parent;
            if ((own == 4 || own == 8) && (par == NULL || par->name == NULL || Skin_JointBone(par->name) != own))
            {
                armY += jointPos[j][1];
                armCount++;
            }
        }
        if (!hipsFound && skin->joints[j]->name && Skin_JointBone(skin->joints[j]->name) == 11)
        {
            memcpy(hipsPos, jointPos[j], sizeof(hipsPos));
            hipsFound = 1;
        }
    }
    if (!hipsFound)
    {
        SH_DBG("[SKIN] %s: no Hips joint -- not a humanoid rig this loader knows", path);
        cgltf_free(data);
        return 0;
    }

    /* Count vertices and measure height. */
    for (ni = 0; ni < data->nodes_count; ni++)
    {
        const cgltf_node* node = &data->nodes[ni];
        if (node->mesh == NULL || node->skin != skin)
            continue;
        for (pi = 0; pi < node->mesh->primitives_count; pi++)
        {
            const cgltf_primitive* p = &node->mesh->primitives[pi];
            size_t a;
            for (a = 0; a < p->attributes_count; a++)
                if (p->attributes[a].type == cgltf_attribute_type_position)
                {
                    const cgltf_accessor* acc = p->attributes[a].data;
                    totalVerts += (int)acc->count;
                    if (acc->has_min && acc->has_max)
                    {
                        if (acc->min[1] < minY) minY = acc->min[1];
                        if (acc->max[1] > maxY) maxY = acc->max[1];
                    }
                }
        }
    }
    if (totalVerts == 0 || maxY <= minY || armCount == 0 || armY / armCount <= minY)
    {
        SH_DBG("[SKIN] %s: no skinned geometry", path);
        cgltf_free(data);
        return 0;
    }

    /* glTF (x, y, z) -> PSX model space: Y down, the same mirror the ILM
     * converter's OBJs use; soles on the character's soles, hips over its hips,
     * and sized by SHOULDER height rather than overall height. A modern head
     * and neck are taller than the PSX ones, so height-matching left the arms
     * a few inches low and pinning them raised the shoulders into peaks; at
     * shoulder height the wrists already sit where the game holds weapons. */
    scale = (HERO_SOLE_Y - (float)s_heroRest[4].T[1]) / (armY / armCount - minY);
    offX = -hipsPos[0] * scale;
    offZ = -hipsPos[2] * scale;
    offY = HERO_SOLE_Y + minY * scale;
#define TO_GAME(src, dst) do { (dst)[0] = (src)[0] * scale + offX; (dst)[1] = -(src)[1] * scale + offY; (dst)[2] = (src)[2] * scale + offZ; } while (0)

    ch->verts = (PcSkinVert*)calloc((size_t)totalVerts, sizeof(PcSkinVert));
    gpos = (float*)calloc((size_t)totalVerts * 3, sizeof(float));
    gnrm = (float*)calloc((size_t)totalVerts * 3, sizeof(float));
    if (ch->verts == NULL || gpos == NULL || gnrm == NULL)
        goto fail;

    /* Geometry. */
    {
        int vbase = 0;
        for (ni = 0; ni < data->nodes_count; ni++)
        {
            const cgltf_node* node = &data->nodes[ni];
            if (node->mesh == NULL || node->skin != skin)
                continue;
            for (pi = 0; pi < node->mesh->primitives_count; pi++)
            {
                const cgltf_primitive* p = &node->mesh->primitives[pi];
                const cgltf_accessor *aPos = NULL, *aNrm = NULL, *aUv = NULL, *aJ = NULL, *aW = NULL;
                size_t a, vc, ic, t;
                int mi;
                PcSkinMat* mat;

                if (p->type != cgltf_primitive_type_triangles)
                    continue;
                for (a = 0; a < p->attributes_count; a++)
                {
                    const cgltf_attribute* at = &p->attributes[a];
                    if (at->type == cgltf_attribute_type_position) aPos = at->data;
                    else if (at->type == cgltf_attribute_type_normal) aNrm = at->data;
                    else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) aUv = at->data;
                    else if (at->type == cgltf_attribute_type_joints && at->index == 0) aJ = at->data;
                    else if (at->type == cgltf_attribute_type_weights && at->index == 0) aW = at->data;
                }
                if (aPos == NULL || aJ == NULL || aW == NULL)
                    continue;
                vc = aPos->count;

                /* Material slot. */
                mi = -1;
                for (a = 0; a < (size_t)ch->matCount; a++)
                    if ((const void*)ch->mats[a].out[1] == (const void*)p->material)
                        mi = (int)a;
                if (mi < 0)
                {
                    if (ch->matCount >= PC_SKIN_MAX_MATS)
                        continue;
                    mi = ch->matCount++;
                    /* out[1] doubles as the material key until frame buffers exist. */
                    ch->mats[mi].out[1] = (GrModernVertex*)(void*)p->material;
                    Skin_LoadImage(&ch->mats[mi], p->material, path);
                }
                mat = &ch->mats[mi];

                for (t = 0; t < vc; t++)
                {
                    PcSkinVert* sv = &ch->verts[vbase + t];
                    float pos[3], nrm[3] = { 0, 1, 0 }, w[4];
                    cgltf_uint jt[4];
                    int q, r;

                    cgltf_accessor_read_float(aPos, t, pos, 3);
                    if (aNrm != NULL)
                        cgltf_accessor_read_float(aNrm, t, nrm, 3);
                    cgltf_accessor_read_uint(aJ, t, jt, 4);
                    cgltf_accessor_read_float(aW, t, w, 4);
                    TO_GAME(pos, &gpos[(vbase + t) * 3]);
                    gnrm[(vbase + t) * 3 + 0] = nrm[0];
                    gnrm[(vbase + t) * 3 + 1] = -nrm[1];
                    gnrm[(vbase + t) * 3 + 2] = nrm[2];
                    sv->n = 0;
                    for (q = 0; q < 4; q++)
                    {
                        int bone = (jt[q] < skin->joints_count) ? jointBone[jt[q]] : -1;
                        if (w[q] <= 0.0f)
                            continue;
                        if (bone < 0)
                            bone = 11;
                        for (r = 0; r < sv->n; r++)
                            if (sv->bone[r] == bone)
                                break;
                        if (r == sv->n)
                        {
                            sv->bone[r] = (u8)bone;
                            sv->w[r] = 0.0f;
                            sv->n++;
                        }
                        sv->w[r] += w[q];
                    }
                    if (sv->n == 0)
                    {
                        sv->n = 1;
                        sv->bone[0] = 11;
                        sv->w[0] = 1.0f;
                    }
                    {
                        float sum = 0.0f;
                        for (r = 0; r < sv->n; r++) sum += sv->w[r];
                        sv->dom = 0;
                        for (r = 0; r < sv->n; r++)
                        {
                            sv->w[r] /= sum;
                            if (sv->w[r] > sv->w[sv->dom]) sv->dom = (u8)r;
                        }
                        sv->dom = sv->bone[sv->dom];
                    }
                }

                ic = p->indices ? p->indices->count : vc;
                mat->tris = (PcSkinTri*)realloc(mat->tris, sizeof(PcSkinTri) * (size_t)(mat->triCount + ic / 3));
                if (mat->tris == NULL)
                    goto fail;
                for (t = 0; t + 2 < ic; t += 3)
                {
                    PcSkinTri* tri = &mat->tris[mat->triCount++];
                    float cu = 0.0f, cv = 0.0f;
                    int c;
                    for (c = 0; c < 3; c++)
                    {
                        size_t idx = p->indices ? cgltf_accessor_read_index(p->indices, t + c) : t + c;
                        tri->v[c] = (u32)(vbase + idx);
                        tri->uv[c][0] = tri->uv[c][1] = 0.0f;
                        if (aUv != NULL)
                            cgltf_accessor_read_float(aUv, idx, tri->uv[c], 2);
                        cu += tri->uv[c][0];
                        cv += tri->uv[c][1];
                    }
                    /* Tiled UVs (the coat at v 2..3) wrap per triangle: the modern
                     * sampler clamps, which turned them into black patches. */
                    cu = floorf(cu / 3.0f);
                    cv = floorf(cv / 3.0f);
                    for (c = 0; c < 3; c++)
                    {
                        tri->uv[c][0] -= cu;
                        tri->uv[c][1] -= cv;
                    }
                }
                vbase += (int)vc;
            }
        }
        ch->vertCount = vbase;
    }
    for (b = 0; b < ch->matCount; b++)
        ch->mats[b].out[1] = NULL;

    /* Anchors: each bone's root-most mapped joint. */
    memset(anchorSet, 0, sizeof(anchorSet));
    for (j = 0; j < skin->joints_count; j++)
    {
        int bone = skin->joints[j]->name ? Skin_JointBone(skin->joints[j]->name) : -1;
        const cgltf_node* par = skin->joints[j]->parent;
        int parBone = -1;
        for (; par != NULL && parBone < 0; par = par->parent)
            parBone = par->name ? Skin_JointBone(par->name) : -1;
        if (bone >= 0 && parBone != bone && !anchorSet[bone])
        {
            TO_GAME(jointPos[j], anchor[bone]);
            anchorSet[bone] = 1;
        }
    }
    for (b = 1; b < PC_SKIN_BONES; b++)
        if (!anchorSet[b])
        {
            SH_DBG("[SKIN] %s: no joint drives game bone %d", path, b);
            goto fail;
        }

    /* Model-side direction of end bones: centroid of the geometry they own. */
    memset(dirSum, 0, sizeof(dirSum));
    memset(dirCount, 0, sizeof(dirCount));
    for (j = 0; j < (size_t)ch->vertCount; j++)
    {
        int d = ch->verts[j].dom;
        dirSum[d][0] += gpos[j * 3]; dirSum[d][1] += gpos[j * 3 + 1]; dirSum[d][2] += gpos[j * 3 + 2];
        dirCount[d]++;
    }

    /* Retarget: R = swing(rest dir -> model dir) * restR; joint -> model joint. */
    for (b = 0; b < PC_SKIN_BONES; b++)
    {
        const PcSkinRestBone* rb = &s_heroRest[b];
        float restR[9], swing[9], gd[3], md[3], gEnd[3], mEnd[3], lg, lm;
        int i;

        M3_FromQ12(rb->R, restR);
        k[b] = 1.0f;
        axis[b][0] = 1.0f; axis[b][1] = axis[b][2] = 0.0f;
        if (b == 0)
        {
            memcpy(R[b], restR, sizeof(restR));
            anchor[0][0] = anchor[0][1] = anchor[0][2] = 0.0f;
            continue;
        }
        if (s_childA[b] >= 0)
        {
            const PcSkinRestBone* ca = &s_heroRest[(int)s_childA[b]];
            for (i = 0; i < 3; i++)
            {
                gEnd[i] = ca->T[i];
                mEnd[i] = anchor[(int)s_childA[b]][i];
            }
            if (s_childB[b] >= 0)
                for (i = 0; i < 3; i++)
                {
                    gEnd[i] = 0.5f * (gEnd[i] + s_heroRest[(int)s_childB[b]].T[i]);
                    mEnd[i] = 0.5f * (mEnd[i] + anchor[(int)s_childB[b]][i]);
                }
        }
        else
        {
            for (i = 0; i < 3; i++)
            {
                gEnd[i] = rb->C[i];
                mEnd[i] = dirCount[b] ? dirSum[b][i] / dirCount[b] : anchor[b][i];
            }
        }
        for (i = 0; i < 3; i++)
        {
            gd[i] = gEnd[i] - rb->T[i];
            md[i] = mEnd[i] - anchor[b][i];
        }
        M3_Align(gd, md, swing);
        M3_Mul(swing, restR, R[b]);
        lg = V3_Len(gd);
        lm = V3_Len(md);
        if (s_scaled[b] && lm > 1e-3f && s_childA[b] >= 0)
        {
            float mdu[3];
            k[b] = lg / lm;
            if (k[b] < 0.5f) k[b] = 0.5f;
            if (k[b] > 2.0f) k[b] = 2.0f;
            for (i = 0; i < 3; i++) mdu[i] = md[i] / lm;
            M3_MulTV(R[b], mdu, axis[b]);
        }
    }

    for (b = 1; b < PC_SKIN_BONES; b++)
        if (!s_pinned[b])
        {
            anchor[b][0] = s_heroRest[b].T[0];
            anchor[b][1] = s_heroRest[b].T[1];
            anchor[b][2] = s_heroRest[b].T[2];
        }

    /* Bone-local positions per influence, lighting normal per vertex. */
    for (j = 0; j < (size_t)ch->vertCount; j++)
    {
        PcSkinVert* sv = &ch->verts[j];
        float d[3], nl[3], len;
        int q;

        for (q = 0; q < sv->n; q++)
        {
            int bb = sv->bone[q];
            float l[3], s;
            d[0] = gpos[j * 3] - anchor[bb][0];
            d[1] = gpos[j * 3 + 1] - anchor[bb][1];
            d[2] = gpos[j * 3 + 2] - anchor[bb][2];
            M3_MulTV(R[bb], d, l);
            s = (k[bb] - 1.0f) * (l[0] * axis[bb][0] + l[1] * axis[bb][1] + l[2] * axis[bb][2]);
            sv->l[q][0] = l[0] + s * axis[bb][0];
            sv->l[q][1] = l[1] + s * axis[bb][1];
            sv->l[q][2] = l[2] + s * axis[bb][2];
        }
        M3_MulTV(R[sv->dom], &gnrm[j * 3], nl);
        len = V3_Len(nl);
        if (len < 1e-6f) { nl[0] = 0; nl[1] = -1; nl[2] = 0; len = 1; }
        sv->gteNormal.vx = (s16)(-nl[0] / len * 4064.0f);
        sv->gteNormal.vy = (s16)(-nl[1] / len * 4064.0f);
        sv->gteNormal.vz = (s16)(-nl[2] / len * 4064.0f);
    }
#undef TO_GAME

    for (b = 0; b < PC_SKIN_BONES; b++)
    {
        ch->byBone[b] = (u32*)malloc(sizeof(u32) * (size_t)(dirCount[b] + 1));
        ch->byBoneCount[b] = 0;
    }
    for (j = 0; j < (size_t)ch->vertCount; j++)
    {
        int d = ch->verts[j].dom;
        ch->byBone[d][ch->byBoneCount[d]++] = (u32)j;
    }
    ch->color = (CVECTOR*)calloc((size_t)ch->vertCount, sizeof(CVECTOR));
    ch->pos = (float*)calloc((size_t)ch->vertCount * 3, sizeof(float));
    ch->scr = (float*)calloc((size_t)ch->vertCount * 2, sizeof(float));
    ch->ok = (u8*)calloc((size_t)ch->vertCount, 1);
    for (b = 0; b < ch->matCount; b++)
    {
        ch->mats[b].out[0] = (GrModernVertex*)calloc((size_t)ch->mats[b].triCount * 3, sizeof(GrModernVertex));
        ch->mats[b].out[1] = (GrModernVertex*)calloc((size_t)ch->mats[b].triCount * 3, sizeof(GrModernVertex));
    }

    SH_DBG("[SKIN] %s: %d verts, %d materials, %u joints, scale %.1f", path, ch->vertCount, ch->matCount,
           (unsigned)skin->joints_count, scale);
    for (b = 1; b < PC_SKIN_BONES; b++)
        SH_DBG("[SKIN]   bone %2d anchor (%.0f %.0f %.0f) k %.2f", b, anchor[b][0], anchor[b][1], anchor[b][2], k[b]);
    free(gpos);
    free(gnrm);
    cgltf_free(data);
    return 1;

fail:
    free(gpos);
    free(gnrm);
    cgltf_free(data);
    return 0;
}

/* ---------------------------------------------------------------- frame */

static int Skin_ModelPath(const s_LmHeader* lm, char* name, size_t nameSize, char* path, size_t pathSize)
{
    s_CharaModel* models[CHARA_GROUP_COUNT + 1];
    int i;

    models[0] = &g_WorldGfxWork.harryModel;
    for (i = 0; i < CHARA_GROUP_COUNT; i++)
        models[i + 1] = &g_WorldGfxWork.charaModels[i];
    for (i = 0; i <= CHARA_GROUP_COUNT; i++)
    {
        const s_CharaModel* cm = models[i];
        char file[32], folder[16];
        const char* f;
        char* dot;
        size_t len;
        s32 idx;

        if (cm->lmHdr != lm || cm->charaId >= Chara_Count)
            continue;
        idx = CHARA_FILE_INFOS[cm->charaId].modelFileIdx;
        if (idx <= 0 || idx >= FS_FILE_COUNT)
            return 0;
        Fs_GetFileName(file, idx);
        dot = strrchr(file, '.');
        if (dot != NULL)
            *dot = '\0';
        f = g_FilePaths[g_FileTable[idx].pathIdx];
        if (f[0] == '\\')
            f++;
        len = strlen(f);
        while (len > 0 && f[len - 1] == '\\')
            len--;
        if (len >= sizeof(folder))
            len = sizeof(folder) - 1;
        memcpy(folder, f, len);
        folder[len] = '\0';
        snprintf(name, nameSize, "%s", file);
        snprintf(path, pathSize, "gamedata/load/%s/%s", folder, file);
        return 1;
    }
    return 0;
}

void* Pc_SkinChara_Begin(s_Skeleton* skel)
{
    const s_ModelHeader* hdr;
    const s_LmHeader* lm = NULL;
    char name[16], base[96], path[112];
    PcSkinChara* ch = NULL;
    int i;

    if (!g_PcConfig.allowLooseFiles || skel == NULL || skel->bones_4 == NULL)
        return NULL;
    hdr = skel->bones_4->bone.modelInfo.modelHdr;
    if (hdr == NULL)
        return NULL;
    if (g_WorldGfxWork.harryModel.lmHdr != NULL &&
        hdr >= g_WorldGfxWork.harryModel.lmHdr->modelHdrs &&
        hdr < g_WorldGfxWork.harryModel.lmHdr->modelHdrs + g_WorldGfxWork.harryModel.lmHdr->modelCount)
        lm = g_WorldGfxWork.harryModel.lmHdr;
    for (i = 0; lm == NULL && i < CHARA_GROUP_COUNT; i++)
    {
        const s_LmHeader* c = g_WorldGfxWork.charaModels[i].lmHdr;
        if (c != NULL && hdr >= c->modelHdrs && hdr < c->modelHdrs + c->modelCount)
            lm = c;
    }
    if (lm == NULL || !Skin_ModelPath(lm, name, sizeof(name), base, sizeof(base)))
        return NULL;
    /* Only HERO's rest pose is tabled so far. */
    if (strcmp(name, "HERO") != 0)
        return NULL;

    for (i = 0; i < s_charaCount; i++)
        if (strcmp(s_chara[i].name, name) == 0)
            ch = &s_chara[i];
    if (ch == NULL)
    {
        if (s_charaCount >= PC_SKIN_MAX_CHARA)
            return NULL;
        ch = &s_chara[s_charaCount++];
        memset(ch, 0, sizeof(*ch));
        snprintf(ch->name, sizeof(ch->name), "%s", name);
    }
    if (ch->state == 0)
    {
        ch->state = -1;
        snprintf(path, sizeof(path), "%s.glb", base);
        if (Skin_Load(ch, path))
            ch->state = 1;
        else
        {
            snprintf(path, sizeof(path), "%s.gltf", base);
            if (Skin_Load(ch, path))
                ch->state = 1;
        }
    }
    if (ch->state != 1)
        return NULL;
    memset(ch->have, 0, sizeof(ch->have));
    memset(ch->lit, 0, sizeof(ch->lit));
    ch->isPlayer = (lm == g_WorldGfxWork.harryModel.lmHdr);
    return ch;
}

void Pc_SkinChara_Bone(void* chara, int boneIdx, MATRIX* worldMat, MATRIX* viewMat)
{
    PcSkinChara* ch = (PcSkinChara*)chara;
    s_GteScratchData* sd = PSX_SCRATCH_ADDR(0);
    int i, r, c;

    if (ch == NULL || boneIdx < 0 || boneIdx >= PC_SKIN_BONES || ch->have[boneIdx])
        return;
    for (r = 0; r < 3; r++)
    {
        for (c = 0; c < 3; c++)
            ch->rv[boneIdx][r * 3 + c] = viewMat->m[r][c] / 4096.0f;
        ch->tv[boneIdx][r] = (float)viewMat->t[r];
    }
    for (r = 0; r < 3; r++)
    {
        for (c = 0; c < 3; c++)
            ch->rw[boneIdx][r * 3 + c] = worldMat->m[r][c] / 4096.0f;
        ch->tw[boneIdx][r] = (float)worldMat->t[r];
    }
    ch->have[boneIdx] = 1;

    /* The stock part's own lighting prelude (func_80057090 -> func_8005A21C),
     * with the shader owning fog as there. */
    if (worldMat != NULL && g_WorldEnvWork.field_0 != 0)
        func_80057228(worldMat, g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);
    switch (g_WorldEnvWork.field_0)
    {
        case 0:
            func_8005A42C(sd, 256 << 4);
            break;
        case 1:
            func_8005A478(sd, 256 << 4);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;
        case 2:
            func_8005A838(sd, 256 << 4);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;
    }
    for (i = 0; i < ch->byBoneCount[boneIdx]; i++)
    {
        u32 v = ch->byBone[boneIdx][i];
        if (g_WorldEnvWork.field_0 == 0)
        {
            ch->color[v] = sd->field_3D8;
        }
        else
        {
            CVECTOR in = { 0, 0, 0, 0 };
            gte_ldrgb(&in);
            gte_ldv0(&ch->verts[v].gteNormal);
            gte_ncs();
            gte_strgb(&ch->color[v]);
        }
    }
    ch->lit[boneIdx] = 1;
}

static u8 Skin_Fog(s32 z)
{
    s32 fa;

    if (!g_WorldEnvWork.isFogEnabled)
        return 0;
    if (z < (1 << g_WorldEnvWork.fog.depthShift))
    {
        s32 idx = (z << 7) >> g_WorldEnvWork.fog.depthShift;
        if (idx < 0) idx = 0;
        if (idx > 127) idx = 127;
        fa = g_WorldEnvWork.fogRamp[idx];
    }
    else
    {
        fa = 255;
    }
    fa = fa * 16 + g_WorldEnvWork.fog.intensity;
    if (fa > 0x1000) fa = 0x1000;
    if (fa < 0) fa = 0;
    return (u8)((fa * 127 + 2048) >> 12);
}


/* ---------------------------------------------------------------- ragdoll
 *
 * Config `ragdoll`: when the player's replacement dies, the pose it died in
 * becomes a particle skeleton -- joints plus hand, foot and head tips, a rigid
 * torso, stiff wrists and ankles -- integrated with Verlet under gravity onto
 * the floor Harry stood on, and every bone is rebuilt from it. It starts with
 * the animation's own motion plus a push along the last hit's knockback. World
 * units, Y down, 256 to the metre. Walls are not collided with. */

enum
{
    RD_WAIST, RD_CHEST, RD_NECK, RD_HEAD,
    RD_SHL, RD_ELL, RD_WRL, RD_HANDL,
    RD_SHR, RD_ELR, RD_WRR, RD_HANDR,
    RD_HIPL, RD_KNL, RD_ANL, RD_TOEL,
    RD_HIPR, RD_KNR, RD_ANR, RD_TOER,
    RD_COUNT
};

#define RD_MAX_STICKS 64
#define RD_GRAVITY    (9.8f * 256.0f)
#define RD_RADIUS     6.0f
/* Fixed step: Verlet velocity is per step, so it must not follow the frame rate. */
#define RD_STEP       (1.0f / 120.0f)

/* Particle -> (bone, tip?) it is read from. */
static const s8 s_rdBone[RD_COUNT] = { 1, 3, 2, 2, 4, 5, 6, 6, 8, 9, 10, 10, 12, 13, 14, 14, 15, 16, 17, 17 };
static const u8 s_rdTip[RD_COUNT]  = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 };

/* Bone -> joint particle, end particle, parent bone (-1: a torso frame). */
static const s8 s_rdJoint[PC_SKIN_BONES] = { -1, RD_WAIST, RD_NECK, RD_CHEST, RD_SHL, RD_ELL, RD_WRL, RD_CHEST, RD_SHR, RD_ELR, RD_WRR,
                                              RD_WAIST, RD_HIPL, RD_KNL, RD_ANL, RD_HIPR, RD_KNR, RD_ANR };
static const s8 s_rdEnd[PC_SKIN_BONES]   = { -1, -1, RD_HEAD, RD_SHL, RD_ELL, RD_WRL, RD_HANDL, RD_SHR, RD_ELR, RD_WRR, RD_HANDR,
                                              -1, RD_KNL, RD_ANL, RD_TOEL, RD_KNR, RD_ANR, RD_TOER };
static const s8 s_rdParent[PC_SKIN_BONES] = { -1, -1, 1, 1, 3, 4, 5, 1, 7, 8, 9, -1, 11, 12, 13, 11, 15, 16 };

typedef struct
{
    int   active;
    float p[RD_COUNT][3], pp[RD_COUNT][3];
    u8    sa[RD_MAX_STICKS], sb[RD_MAX_STICKS];
    float len[RD_MAX_STICKS];
    int   stickCount;
    float floorY;
    float R0[PC_SKIN_BONES][9];
    float torso0[9], hip0[9];
    float dir0[PC_SKIN_BONES][3];
    float anim[RD_COUNT][3];
    float animPrev[RD_COUNT][3];
    float animDt;
    int   haveAnim;
    float hit[3];
    float hitAmount;
    float acc;
} PcRagdoll;

static PcRagdoll s_rd;

static void RD_TipLocal(int bone, float* out)
{
    const PcSkinRestBone* rb = &s_heroRest[bone];
    float R[9], d[3];
    M3_FromQ12(rb->R, R);
    d[0] = rb->C[0] - rb->T[0];
    d[1] = rb->C[1] - rb->T[1];
    d[2] = rb->C[2] - rb->T[2];
    M3_MulTV(R, d, out);
}

static void RD_Points(const PcSkinChara* ch, float out[RD_COUNT][3])
{
    int i;
    for (i = 0; i < RD_COUNT; i++)
    {
        int b = s_rdBone[i];
        if (s_rdTip[i])
        {
            float l[3], w[3];
            RD_TipLocal(b, l);
            M3_MulV(ch->rw[b], l, w);
            out[i][0] = w[0] + ch->tw[b][0];
            out[i][1] = w[1] + ch->tw[b][1];
            out[i][2] = w[2] + ch->tw[b][2];
        }
        else
        {
            out[i][0] = ch->tw[b][0];
            out[i][1] = ch->tw[b][1];
            out[i][2] = ch->tw[b][2];
        }
    }
}

/* Orthonormal frame, columns: a, b made perpendicular to a, a x b. */
static void RD_Frame(const float* from, const float* to, const float* l, const float* r, float* F)
{
    float a[3], b[3], c[3], la, lb, d;
    int i;
    for (i = 0; i < 3; i++) { a[i] = to[i] - from[i]; b[i] = r[i] - l[i]; }
    la = V3_Len(a); if (la < 1e-6f) la = 1;
    for (i = 0; i < 3; i++) a[i] /= la;
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    for (i = 0; i < 3; i++) b[i] -= d * a[i];
    lb = V3_Len(b); if (lb < 1e-6f) lb = 1;
    for (i = 0; i < 3; i++) b[i] /= lb;
    c[0] = a[1] * b[2] - a[2] * b[1];
    c[1] = a[2] * b[0] - a[0] * b[2];
    c[2] = a[0] * b[1] - a[1] * b[0];
    for (i = 0; i < 3; i++) { F[i * 3] = a[i]; F[i * 3 + 1] = b[i]; F[i * 3 + 2] = c[i]; }
}

static void RD_Stick(int a, int b)
{
    if (s_rd.stickCount >= RD_MAX_STICKS)
        return;
    s_rd.sa[s_rd.stickCount] = (u8)a;
    s_rd.sb[s_rd.stickCount] = (u8)b;
    s_rd.len[s_rd.stickCount] = 0.0f;
    s_rd.stickCount++;
}

static void RD_Start(const PcSkinChara* ch)
{
    static const u8 torso[] = { RD_WAIST, RD_CHEST, RD_NECK, RD_SHL, RD_SHR, RD_HIPL, RD_HIPR };
    float vel[3] = { 0, 0, 0 }, speed, hl;
    int i, j, b;

    memset(s_rd.p, 0, sizeof(s_rd.p));
    s_rd.stickCount = 0;
    for (i = 0; i < (int)sizeof(torso); i++)
        for (j = i + 1; j < (int)sizeof(torso); j++)
            RD_Stick(torso[i], torso[j]);
    RD_Stick(RD_NECK, RD_HEAD); RD_Stick(RD_CHEST, RD_HEAD);
    RD_Stick(RD_SHL, RD_ELL); RD_Stick(RD_ELL, RD_WRL); RD_Stick(RD_WRL, RD_HANDL); RD_Stick(RD_ELL, RD_HANDL);
    RD_Stick(RD_SHR, RD_ELR); RD_Stick(RD_ELR, RD_WRR); RD_Stick(RD_WRR, RD_HANDR); RD_Stick(RD_ELR, RD_HANDR);
    RD_Stick(RD_HIPL, RD_KNL); RD_Stick(RD_KNL, RD_ANL); RD_Stick(RD_ANL, RD_TOEL); RD_Stick(RD_KNL, RD_TOEL);
    RD_Stick(RD_HIPR, RD_KNR); RD_Stick(RD_KNR, RD_ANR); RD_Stick(RD_ANR, RD_TOER); RD_Stick(RD_KNR, RD_TOER);

    RD_Points(ch, s_rd.p);
    for (i = 0; i < s_rd.stickCount; i++)
    {
        float d[3];
        for (j = 0; j < 3; j++) d[j] = s_rd.p[s_rd.sb[i]][j] - s_rd.p[s_rd.sa[i]][j];
        s_rd.len[i] = V3_Len(d);
    }
    s_rd.floorY = (float)Q12_TO_Q8(g_SysWork.playerWork.player.position.vy) - RD_RADIUS;

    /* Push along the last hit's knockback, harder for heavier hits, mostly on
     * the upper body and slightly upward so the body leaves its feet. */
    hl = sqrtf(s_rd.hit[0] * s_rd.hit[0] + s_rd.hit[2] * s_rd.hit[2]);
    speed = (1.0f + 3.0f * (s_rd.hitAmount / 30.0f)) * 256.0f;
    if (speed > 4.5f * 256.0f) speed = 4.5f * 256.0f;
    for (i = 0; i < RD_COUNT; i++)
    {
        float w = (i <= RD_HEAD || i == RD_SHL || i == RD_SHR) ? 1.0f : 0.45f;
        float v[3] = { 0, 0, 0 };
        if (s_rd.haveAnim && s_rd.animDt > 0.0f)
            for (j = 0; j < 3; j++) v[j] = (s_rd.anim[i][j] - s_rd.animPrev[i][j]) / s_rd.animDt;
        if (hl > 1e-6f)
        {
            v[0] += s_rd.hit[0] / hl * speed * w;
            v[2] += s_rd.hit[2] / hl * speed * w;
            v[1] -= 0.6f * 256.0f * w;
        }
        for (j = 0; j < 3; j++)
            s_rd.pp[i][j] = s_rd.p[i][j] - v[j] * RD_STEP;
    }
    (void)vel;

    RD_Frame(s_rd.p[RD_WAIST], s_rd.p[RD_NECK], s_rd.p[RD_SHL], s_rd.p[RD_SHR], s_rd.torso0);
    {
        float mid[3];
        for (j = 0; j < 3; j++) mid[j] = 0.5f * (s_rd.p[RD_HIPL][j] + s_rd.p[RD_HIPR][j]);
        RD_Frame(s_rd.p[RD_WAIST], mid, s_rd.p[RD_HIPL], s_rd.p[RD_HIPR], s_rd.hip0);
    }
    for (b = 0; b < PC_SKIN_BONES; b++)
    {
        memcpy(s_rd.R0[b], ch->rw[b], sizeof(s_rd.R0[b]));
        if (s_rdEnd[b] >= 0)
            for (j = 0; j < 3; j++)
                s_rd.dir0[b][j] = s_rd.p[(int)s_rdEnd[b]][j] - s_rd.p[(int)s_rdJoint[b]][j];
    }
    s_rd.acc = 0.0f;
    s_rd.active = 1;
}

static void RD_Step(float dt)
{
    int i, j, it;
    for (i = 0; i < RD_COUNT; i++)
    {
        for (j = 0; j < 3; j++)
        {
            float x = s_rd.p[i][j];
            float v = (x - s_rd.pp[i][j]) * 0.995f;
            s_rd.pp[i][j] = x;
            s_rd.p[i][j] = x + v + ((j == 1) ? RD_GRAVITY * dt * dt : 0.0f);
        }
    }
    for (it = 0; it < 10; it++)
    {
        for (i = 0; i < s_rd.stickCount; i++)
        {
            float* a = s_rd.p[s_rd.sa[i]];
            float* b = s_rd.p[s_rd.sb[i]];
            float d[3], l, f;
            for (j = 0; j < 3; j++) d[j] = b[j] - a[j];
            l = V3_Len(d);
            if (l < 1e-6f)
                continue;
            f = 0.5f * (l - s_rd.len[i]) / l;
            for (j = 0; j < 3; j++)
            {
                a[j] += d[j] * f;
                b[j] -= d[j] * f;
            }
        }
        for (i = 0; i < RD_COUNT; i++)
            if (s_rd.p[i][1] > s_rd.floorY)
            {
                s_rd.p[i][1] = s_rd.floorY;
                /* Ground friction: bleed the sliding velocity. */
                s_rd.pp[i][0] += (s_rd.p[i][0] - s_rd.pp[i][0]) * 0.35f;
                s_rd.pp[i][2] += (s_rd.p[i][2] - s_rd.pp[i][2]) * 0.35f;
            }
    }
}

/* Rebuild the bones' world matrices from the particles, then hand them to the
 * skinning as view matrices through this frame's camera (any bone's animated
 * view * inverse(world)). */
static void RD_Pose(PcSkinChara* ch)
{
    float V[9], Vt[3], torso[9], hip[9], F[9], T[9];
    float Rw[PC_SKIN_BONES][9], tw[PC_SKIN_BONES][3], delta[PC_SKIN_BONES][9];
    int b, j, ref = 1;

    /* camera: view = V * world */
    {
        float Rt[9];
        for (j = 0; j < 9; j++) Rt[j] = ch->rw[ref][(j % 3) * 3 + j / 3];
        M3_Mul(ch->rv[ref], Rt, V);
        M3_MulV(V, ch->tw[ref], Vt);
        for (j = 0; j < 3; j++) Vt[j] = ch->tv[ref][j] - Vt[j];
    }

    RD_Frame(s_rd.p[RD_WAIST], s_rd.p[RD_NECK], s_rd.p[RD_SHL], s_rd.p[RD_SHR], F);
    for (j = 0; j < 9; j++) T[j] = s_rd.torso0[(j % 3) * 3 + j / 3];
    M3_Mul(F, T, torso);
    {
        float mid[3];
        for (j = 0; j < 3; j++) mid[j] = 0.5f * (s_rd.p[RD_HIPL][j] + s_rd.p[RD_HIPR][j]);
        RD_Frame(s_rd.p[RD_WAIST], mid, s_rd.p[RD_HIPL], s_rd.p[RD_HIPR], F);
    }
    for (j = 0; j < 9; j++) T[j] = s_rd.hip0[(j % 3) * 3 + j / 3];
    M3_Mul(F, T, hip);

    for (b = 1; b < PC_SKIN_BONES; b++)
    {
        float* d = delta[b];
        if (b == 1)
            memcpy(d, torso, sizeof(torso));
        else if (b == 11)
            memcpy(d, hip, sizeof(hip));
        else
        {
            const float* pd = delta[(int)s_rdParent[b]];
            float carried[3], now[3], sw[9];
            M3_MulV(pd, s_rd.dir0[b], carried);
            for (j = 0; j < 3; j++)
                now[j] = s_rd.p[(int)s_rdEnd[b]][j] - s_rd.p[(int)s_rdJoint[b]][j];
            M3_Align(carried, now, sw);
            M3_Mul(sw, pd, d);
        }
        M3_Mul(d, s_rd.R0[b], Rw[b]);
        for (j = 0; j < 3; j++)
            tw[b][j] = s_rd.p[(int)s_rdJoint[b]][j];
    }
    for (b = 1; b < PC_SKIN_BONES; b++)
    {
        float t[3];
        if (!ch->have[b])
            continue;
        M3_Mul(V, Rw[b], ch->rv[b]);
        M3_MulV(V, tw[b], t);
        for (j = 0; j < 3; j++)
            ch->tv[b][j] = t[j] + Vt[j];
    }
}

static void Skin_Ragdoll(PcSkinChara* ch)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float dt = g_DeltaTime / 4096.0f;
    int i;

    if (!g_PcConfig.ragdoll || !ch->isPlayer || !ch->have[1] || !ch->have[11])
    {
        s_rd.active = 0;
        return;
    }
    if (pl->health > Q12(0.0f))
    {
        /* Alive: remember the last hit and how the pose is moving, for the
         * moment it stops being alive. */
        if (pl->damage.position.vx != 0 || pl->damage.position.vy != 0 || pl->damage.position.vz != 0)
        {
            s_rd.hit[0] = (float)pl->damage.position.vx;
            s_rd.hit[1] = (float)pl->damage.position.vy;
            s_rd.hit[2] = (float)pl->damage.position.vz;
        }
        if (pl->damage.amount > 0)
            s_rd.hitAmount = pl->damage.amount / 4096.0f;
        if (dt > 0.0f)
        {
            memcpy(s_rd.animPrev, s_rd.anim, sizeof(s_rd.anim));
            RD_Points(ch, s_rd.anim);
            s_rd.animDt = dt;
            s_rd.haveAnim = (s_rd.haveAnim < 2) ? s_rd.haveAnim + 1 : 2;
        }
        s_rd.active = 0;
        return;
    }
    if (!s_rd.active)
    {
        if (s_rd.haveAnim < 2)
            s_rd.haveAnim = 0;
        RD_Start(ch);
    }
    if (dt > 1.0f / 15.0f)
        dt = 1.0f / 15.0f;
    s_rd.acc += dt;
    for (i = 0; s_rd.acc >= RD_STEP && i < 16; i++)
    {
        RD_Step(RD_STEP);
        s_rd.acc -= RD_STEP;
    }
    RD_Pose(ch);
}

void Pc_SkinChara_End(void* chara, void* otp, int shift, int hideHead)
{
    PcSkinChara* ch = (PcSkinChara*)chara;
    GsOT* ot = (GsOT*)otp;
    float offX = 0.0f, offY = 0.0f, szMax;
    float h = (float)C2_H, ofx = (float)C2_OFX / 65536.0f, ofy = (float)C2_OFY / 65536.0f;
    float minZ = 1e30f, maxZ = 0.0f;
    int buf = g_ActiveBufferIdx & 1;
    int v, m, bucket;

    if (ch == NULL || ot == NULL)
        return;
    Skin_Ragdoll(ch);
    PsyX_GetDrawEnvOffset(&offX, &offY);
    for (v = 0; v < ch->vertCount; v++)
    {
        const PcSkinVert* sv = &ch->verts[v];
        float p[3] = { 0, 0, 0 }, ws = 0.0f;
        int q;

        for (q = 0; q < sv->n; q++)
        {
            int b = sv->bone[q];
            float o[3];
            if (!ch->have[b])
                continue;
            M3_MulV(ch->rv[b], sv->l[q], o);
            p[0] += sv->w[q] * (o[0] + ch->tv[b][0]);
            p[1] += sv->w[q] * (o[1] + ch->tv[b][1]);
            p[2] += sv->w[q] * (o[2] + ch->tv[b][2]);
            ws += sv->w[q];
        }
        ch->ok[v] = 0;
        if (ws < 0.5f)
            continue;
        p[0] /= ws; p[1] /= ws; p[2] /= ws;
        if (p[2] < 16.0f || p[2] > 65535.0f)
            continue;
        ch->pos[v * 3] = p[0]; ch->pos[v * 3 + 1] = p[1]; ch->pos[v * 3 + 2] = p[2];
        ch->scr[v * 2] = ofx + p[0] * h / p[2] + offX;
        ch->scr[v * 2 + 1] = ofy + p[1] * h / p[2] + offY;
        if (!ch->lit[sv->dom])
            ch->color[v].r = ch->color[v].g = ch->color[v].b = 128;
        ch->ok[v] = 1;
        if (p[2] < minZ) minZ = p[2];
        if (p[2] > maxZ) maxZ = p[2];
    }
    if (maxZ <= 0.0f)
        return;
    PsyX_NoteItemDepthSz((unsigned)maxZ);
    szMax = PGXP_GetSzMax();
    if (szMax < 1.0f)
        szMax = 1.0f;

    bucket = PC_SKIN_OT_BASE + ((((int)minZ) >> shift) >> 2);
    if (bucket < 1)
        bucket = 1;
    if (bucket > ORDERING_TABLE_SIZE - 1)
        bucket = ORDERING_TABLE_SIZE - 1;

    for (m = 0; m < ch->matCount; m++)
    {
        PcSkinMat* mat = &ch->mats[m];
        GrModernVertex* out = mat->out[buf];
        int t, n = 0;

        mat->outCount[buf] = 0;
        if (out == NULL)
            continue;
        for (t = 0; t < mat->triCount; t++)
        {
            const PcSkinTri* tri = &mat->tris[t];
            int c;
            if (!ch->ok[tri->v[0]] || !ch->ok[tri->v[1]] || !ch->ok[tri->v[2]])
                continue;
            if (hideHead && ch->verts[tri->v[0]].dom == 2 && ch->verts[tri->v[1]].dom == 2 &&
                ch->verts[tri->v[2]].dom == 2)
                continue;
            for (c = 0; c < 3; c++)
            {
                u32 vi = tri->v[c];
                GrModernVertex* o = &out[n++];
                float z = ch->pos[vi * 3 + 2];
                float nz = 1.0f - 2.0f * z / szMax;
                float u = tri->uv[c][0], vv = tri->uv[c][1];

                if (nz < -1.0f) nz = -1.0f;
                if (nz > 1.0f) nz = 1.0f;
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                if (vv < 0.0f) vv = 0.0f;
                if (vv > 1.0f) vv = 1.0f;
                memset(o, 0, sizeof(*o));
                o->x = ch->scr[vi * 2];
                o->y = ch->scr[vi * 2 + 1];
                o->z = nz;
                o->u = u * 256.0f;
                o->v = vv * 256.0f;
                o->r = ch->color[vi].r;
                o->g = ch->color[vi].g;
                o->b = ch->color[vi].b;
                o->a = 255;
                o->fog = (char)Skin_Fog((s32)z);
                o->ppx = o->x;
                o->ppy = o->y;
                o->ppw = z;
                o->vsx = ch->pos[vi * 3];
                o->vsy = ch->pos[vi * 3 + 1];
                o->vsz = z;
            }
        }
        mat->outCount[buf] = n;
        if (n < 3)
            continue;
        {
            DR_PSYX_MODERN_MESH* packet = (DR_PSYX_MODERN_MESH*)GsOUT_PACKET_P;
            setlen(packet, 2);
            packet->code[0] = 0xB3000000;
            packet->code[1] = PC_SKIN_HANDLE_FLAG | ((u32)(ch - s_chara) << 8) | ((u32)buf << 7) | (u32)m;
            addPrim(&ot->org[bucket], packet);
            GsOUT_PACKET_P = (PACKET*)((unsigned char*)packet + sizeof(*packet));
        }
    }
}

int Pc_SkinChara_PrepareDraw(u32 handle, PcModernDrawBinding* binding)
{
    int ci = (int)((handle >> 8) & 0xFF), buf = (int)((handle >> 7) & 1), m = (int)(handle & 0x7F);
    PcSkinChara* ch;
    PcSkinMat* mat;

    if (ci >= s_charaCount)
        return 0;
    ch = &s_chara[ci];
    if (ch->state != 1 || m >= ch->matCount)
        return 0;
    mat = &ch->mats[m];
    if (mat->outCount[buf] < 3)
        return 0;
    if (mat->texture == 0)
    {
        static const unsigned char white[4] = { 255, 255, 255, 255 };
        mat->texture = (mat->rgba != NULL) ? HiresOverride_CreateTextureRGBA(mat->rgba, mat->w, mat->h)
                                           : HiresOverride_CreateTextureRGBA(white, 1, 1);
        if (mat->texture == 0)
            return 0;
    }
    Pc_ModernVertex_Upload(mat->out[buf], mat->outCount[buf]);
    binding->textureId = mat->texture;
    binding->texFormat = TF_32_BIT_RGBA;
    binding->vertexCount = mat->outCount[buf];
    binding->nativeWidth = binding->nativeHeight = 256;
    return 1;
}
