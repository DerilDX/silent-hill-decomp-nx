/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "Dark Harry" easter egg. See pc_dark_harry.h for what it is and how to
 * remove it. Nothing in here is original-game behaviour. */
#include "pc_dark_harry.h"

#include "game.h"

#include "bodyprog/anim.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/map/map.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/screen/screen_data.h" /* g_DeltaTime */
#include "maps/characters/harry.h"
#include "pc_config.h"
#include "sh_log.h"

extern void func_8003DA9C(e_CharaId charaId, GsCOORDINATE2* boneCoords, s32 arg2, q3_12 timer, s32 arg4);
extern int  g_PcCharaGhostDraw;

/* Where he is allowed to appear. The fog exteriors are the point of the thing;
 * indoors there is no "edge of visibility" to fade out of. One map for now. */
static const s8 DARK_HARRY_MAPS[] = {
    MapIdx_MAP0_S00,
};

/* Walk speed in world units per second. Far below Harry's walk: the effect is
 * noticing the shape is nearer than it was, not watching something approach. */
#define DARK_HARRY_SPEED Q12(0.35f)

/* Close enough to touch. Harry's own collision radius is 0.3. */
#define DARK_HARRY_REACH Q12(0.9f)

/* Give up if he cannot reach the player (geometry between them, player on a
 * ledge) rather than trailing around for the rest of the session. */
#define DARK_HARRY_MAX_STALK_SEC 120

/* He is NOT an NPC.
 *
 * The obvious route -- take an NPC slot like the console SPAWN command -- does
 * not work for Harry: g_CharaAnimDataIdxs[Chara_Harry] is never set, because
 * the player's animation loads through the player's own path (FS_BUFFER_0 and
 * g_SysWork.playerBoneCoords) rather than into one of the NPC anim slots.
 * npc_main sees animLoaded == false and skips the slot with `continue`, so the
 * model is never drawn. The first build did exactly that: he spawned, walked
 * over and killed the player while being completely invisible.
 *
 * So he carries his own model state and bone coords and is drawn directly.
 * That also keeps him out of npc_main, collision and the spawn bookkeeping
 * entirely, which suits something that is not part of the game. */
#define DARK_HARRY_BONE_MAX 32

static s_Model       s_model;
static GsCOORDINATE2 s_bones[DARK_HARRY_BONE_MAX];
static VECTOR3       s_pos;
static SVECTOR       s_rot;
static int           s_present; /* 0 = not here */
static int           s_posed;   /* bones initialised for this appearance */

static q19_12 s_afk;   /* seconds of no input */
static q19_12 s_stalk; /* seconds since he appeared */
static int    s_armed; /* 0 until the player has had control for a frame */

/* Last-seen input state, for edge detection. */
static s32 s_prevPosX, s_prevPosZ;
static s16 s_prevRotY;
static s32 s_prevCamYaw;
static s32 s_prevStick;

int Pc_DarkHarry_Active(void)
{
    return s_present;
}

static void DarkHarry_Despawn(void)
{
    s_present = 0;
    s_posed   = 0;
    s_stalk   = Q12(0.0f);
}

void Pc_DarkHarry_Reset(void)
{
    DarkHarry_Despawn();
    s_afk   = Q12(0.0f);
    s_armed = 0;
}

/* Normal gameplay, on one of his maps, player alive and actually driving.
 * Anything scripted (cutscene, message box, inventory, a door, a death) fails
 * this, which keeps the clock from running when the player is not standing
 * still of their own accord. */
static int DarkHarry_Eligible(void)
{
    int i;

    if (!g_PcConfig.darkHarry)
        return 0;
    if (g_GameWork.gameState != GameState_InGame)
        return 0;
    if (g_SysWork.sysState != SysState_Gameplay)
        return 0;
    if (g_SavegamePtr == NULL)
        return 0;

    for (i = 0; i < (int)(sizeof(DARK_HARRY_MAPS) / sizeof(DARK_HARRY_MAPS[0])); i++)
    {
        if (g_SavegamePtr->mapIdx == DARK_HARRY_MAPS[i])
            break;
    }
    if (i == (int)(sizeof(DARK_HARRY_MAPS) / sizeof(DARK_HARRY_MAPS[0])))
        return 0;

    if (g_SysWork.playerWork.player.health <= Q12(0.0f))
        return 0;
    if (g_SysWork.playerWork.extra.state != PlayerState_None)
        return 0;

    return 1;
}

/* Any control touched at all, including looking around. Deliberately broad --
 * the brief is that ANY input dismisses him. */
static int DarkHarry_InputSeen(void)
{
    extern s32 g_TpsCamYaw;

    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    u32 held   = (u32)g_Controller0->heldBtnFlags | (u32)g_Controller0->clickedBtnFlags;
    /* Both sticks in one word: look input is analog and never reaches the
     * button flags, so without this a player nudging the camera reads as idle. */
    s32 stick  = ((s32)g_Controller0->analogController.leftX  << 24) |
                 ((s32)g_Controller0->analogController.leftY  << 16) |
                 ((s32)g_Controller0->analogController.rightX <<  8) |
                 ((s32)g_Controller0->analogController.rightY);
    /* Mouse look moves no stick; it lands here. */
    s32 camYaw = g_TpsCamYaw;

    int moved   = (pl->position.vx != s_prevPosX) || (pl->position.vz != s_prevPosZ);
    int turned  = (pl->rotation.vy != s_prevRotY) || (camYaw != s_prevCamYaw);
    int nudged  = (stick != s_prevStick);
    int pressed = (held != 0);

    s_prevPosX   = pl->position.vx;
    s_prevPosZ   = pl->position.vz;
    s_prevRotY   = pl->rotation.vy;
    s_prevCamYaw = camYaw;
    s_prevStick  = stick;

    return pressed || moved || turned || nudged;
}

void Pc_DarkHarry_Spawn(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    q19_12                posX, posZ;

    if (s_present)
        return;

    if (g_PcConfig.darkHarryX != 0 || g_PcConfig.darkHarryZ != 0)
    {
        /* A fixed landmark, which is the point: he belongs to a place in the
         * map rather than to wherever the player happens to be facing. Config
         * is in hundredths of a world unit. */
        posX = (q19_12)(((s64)g_PcConfig.darkHarryX * Q12(1.0f)) / 100);
        posZ = (q19_12)(((s64)g_PcConfig.darkHarryZ * Q12(1.0f)) / 100);
    }
    else
    {
        /* No landmark configured yet: straight ahead at the fog wall, so the
         * feature still does something. */
        q19_12 dist = Q12(1.0f) * g_PcConfig.darkHarryDistance;

        posX = pl->position.vx + (s32)(((s64)dist * Math_Sin(pl->rotation.vy)) >> 12);
        posZ = pl->position.vz + (s32)(((s64)dist * Math_Cos(pl->rotation.vy)) >> 12);
    }

    memset(&s_model, 0, sizeof(s_model));
    s_model.charaId      = Chara_Harry;
    s_model.controlState = 0;
    s_model.stateStep    = 0;
    s_model.anim.status  = ANIM_STATUS(HarryAnim_Still, false);
    s_model.anim.flags  |= AnimFlag_Visible | AnimFlag_Unlocked;
    s_model.anim.time    = Q12(0.0f);

    s_pos.vx = posX;
    s_pos.vz = posZ;
    Collision_SurfaceGet(&surf, posX, posZ);
    s_pos.vy = surf.groundHeight;

    s_rot.vx = 0;
    s_rot.vz = 0;
    s_rot.vy = (s16)((pl->rotation.vy + 0x800) & 0xFFF); /* facing the player */

    s_present = 1;
    s_posed   = 0;
    s_stalk   = Q12(0.0f);

    /* A continue, taken the moment he appears, so the death he is walking
     * toward costs the player nothing but the walk back. */
    g_GameWork.autosave = g_GameWork.savegame;

    SH_LOG("[DARKHARRY] appeared at (%d,%d,%d), player at (%d,%d,%d)",
           FP_FROM(s_pos.vx, Q12_SHIFT), FP_FROM(s_pos.vy, Q12_SHIFT), FP_FROM(s_pos.vz, Q12_SHIFT),
           FP_FROM(pl->position.vx, Q12_SHIFT), FP_FROM(pl->position.vy, Q12_SHIFT),
           FP_FROM(pl->position.vz, Q12_SHIFT));
}

/* He has reached the player. Hand over to the game's own instant death -- the
 * one the Split Head bite uses -- so the scream, the fade and the game over
 * are the real ones rather than an imitation. */
static void DarkHarry_Take(void)
{
    s_SubCharacter* pl    = &g_SysWork.playerWork.player;
    s_PlayerExtra*  extra = &g_SysWork.playerWork.extra;

    SH_LOG("[DARKHARRY] reached the player - instant death");
    DarkHarry_Despawn();
    s_afk = Q12(0.0f);

    pl->attackReceived = NO_VALUE;
    Player_ExtraStateSet(pl, extra, PlayerState_InstantDeath);
}

static void DarkHarry_Approach(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    s32 dx   = pl->position.vx - s_pos.vx;
    s32 dz   = pl->position.vz - s_pos.vz;
    s32 yaw  = ratan2(dx, dz);
    s32 step = (s32)(((s64)DARK_HARRY_SPEED * g_DeltaTime) >> 12);
    s32 distSq;

    /* Squared compare: one multiply pair and no square root. */
    distSq = (s32)((((s64)dx * dx) + ((s64)dz * dz)) >> 12);
    if (distSq <= (s32)(((s64)DARK_HARRY_REACH * DARK_HARRY_REACH) >> 12))
    {
        DarkHarry_Take();
        return;
    }

    s_rot.vy  = (s16)(yaw & 0xFFF);
    s_pos.vx += (s32)(((s64)step * Math_Sin(yaw)) >> 12);
    s_pos.vz += (s32)(((s64)step * Math_Cos(yaw)) >> 12);

    /* Follow the ground rather than the collision system: he is not solid, and
     * a floating or sunken figure breaks the effect instantly. */
    Collision_SurfaceGet(&surf, s_pos.vx, s_pos.vz);
    s_pos.vy = surf.groundHeight;
}

void Pc_DarkHarry_Draw(void)
{
    s_AnmHeader* hdr = (s_AnmHeader*)FS_BUFFER_0;
    if (!s_present || hdr == NULL)
        return;

    if (!s_posed)
    {
        /* Harry's skeleton laid out in OUR coords, not the player's, so posing
         * him cannot disturb the player's own pose. */
        if ((int)hdr->boneCount > DARK_HARRY_BONE_MAX)
        {
            SH_LOG("[DARKHARRY] skeleton needs %d bones, room for %d - not drawing",
                   (int)hdr->boneCount, DARK_HARRY_BONE_MAX);
            DarkHarry_Despawn();
            return;
        }
        Anim_BoneInit(hdr, s_bones);
        s_posed = 1;
    }

    /* Pose him from Harry's OWN anim table, which is exactly what
     * Player_AnimUpdate does for the player: his idle lives in the player's
     * bank, not in the NPC "statue" idle list (that one is for the human
     * cutscene actors and has no Harry entry). Playback writes bones 1+, so
     * the root placement below is still ours to do. */
    {
        s_AnimInfo* info = &HARRY_BASE_ANIM_INFOS[s_model.anim.status];

        if (info->playbackFunc != NULL)
            info->playbackFunc(&s_model, hdr, s_bones, info);
        else
            Anim_BoneUpdate(hdr, s_bones, 0, 0, Q12(0.0f));
    }

    Math_RotMatrixZxyNegGte(&s_rot, &s_bones->coord);
    s_bones->coord.t[0] = Q12_TO_Q8(s_pos.vx);
    s_bones->coord.t[1] = Q12_TO_Q8(s_pos.vy);
    s_bones->coord.t[2] = Q12_TO_Q8(s_pos.vz);
    s_bones->flg        = 0;

    /* See-through, for this draw only. */
    g_PcCharaGhostDraw = 1;
    func_8003DA9C(Chara_Harry, s_bones, 1, 0, 0);
    g_PcCharaGhostDraw = 0;
}

void Pc_DarkHarry_Update(void)
{
    if (!DarkHarry_Eligible())
    {
        /* Leaving gameplay at all takes him with it, so he can never be left
         * standing in a cutscene or survive a map change. */
        DarkHarry_Despawn();
        s_afk   = Q12(0.0f);
        s_armed = 0;
        /* Keep the input baseline current so re-entering gameplay does not
         * read the position jump as "the player moved". */
        (void)DarkHarry_InputSeen();
        return;
    }

    /* One eligible frame before the clock may start, so the frame control is
     * handed back (door, cutscene end) is never counted as idle. */
    if (!s_armed)
    {
        s_armed = 1;
        s_afk   = Q12(0.0f);
        (void)DarkHarry_InputSeen();
        return;
    }

    if (DarkHarry_InputSeen())
    {
        if (s_present)
        {
            SH_LOG("[DARKHARRY] dismissed by input");
            DarkHarry_Despawn();
        }
        s_afk = Q12(0.0f);
        return;
    }

    if (!s_present)
    {
        s_afk += g_DeltaTime;
        if (s_afk >= Q12(1.0f) * g_PcConfig.darkHarryAfkSec)
            Pc_DarkHarry_Spawn();
        return;
    }

    s_stalk += g_DeltaTime;
    if (s_stalk >= Q12((float)DARK_HARRY_MAX_STALK_SEC))
    {
        SH_LOG("[DARKHARRY] gave up after %d s", DARK_HARRY_MAX_STALK_SEC);
        DarkHarry_Despawn();
        s_afk = Q12(0.0f);
        return;
    }

    DarkHarry_Approach();
}
