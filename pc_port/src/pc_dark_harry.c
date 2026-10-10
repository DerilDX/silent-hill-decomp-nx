/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "Dark Harry" easter egg. See pc_dark_harry.h for what it is and how to
 * remove it. Nothing in here is original-game behaviour. */
#include "pc_dark_harry.h"

#include "game.h"

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

/* Where he is allowed to appear. The fog exteriors are the point of the thing;
 * indoors there is no "edge of visibility" to fade out of. One map for now. */
static const s8 DARK_HARRY_MAPS[] = {
    MapIdx_MAP0_S00,
};

/* Walk speed, in world units per second. Deliberately far below Harry's walk:
 * the whole effect is noticing that the shape is nearer than it was. */
#define DARK_HARRY_SPEED Q12(0.35f)

/* How close he has to get to take you. Harry's own collision radius is 0.3, so
 * this is "close enough to touch" without needing the collision system. */
#define DARK_HARRY_REACH Q12(0.9f)

/* Give up and reset if he somehow cannot reach the player (geometry in the way,
 * player on a ledge). Without this he would trail around forever. */
#define DARK_HARRY_MAX_STALK_SEC 120

static int    s_slot = -1;      /* NPC slot he occupies, -1 = not present */
static q19_12 s_afk;            /* seconds of no input */
static q19_12 s_stalk;          /* seconds since he appeared */
static int    s_armed;          /* 0 until the player has been in control once */

/* Last-seen input state, for edge detection. */
static u32    s_prevHeld;
static s32    s_prevPosX, s_prevPosZ;
static s16    s_prevRotY;
static s32    s_prevCamYaw;
static s32    s_prevStick;

int Pc_DarkHarry_Active(void)
{
    return s_slot >= 0;
}

static void DarkHarry_Despawn(void)
{
    if (s_slot >= 0)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[s_slot];

        CLEAR_FLAG(&g_SysWork.npcFlags, s_slot);
        CLEAR_FLAG(g_SysWork.field_228C, s_slot);
        memset(npc, 0, sizeof(*npc));
        npc->model.charaId = Chara_None;
        s_slot = -1;
    }
    s_stalk = Q12(0.0f);
}

void Pc_DarkHarry_Reset(void)
{
    DarkHarry_Despawn();
    s_afk   = Q12(0.0f);
    s_armed = 0;
}

/* Normal gameplay, on one of his maps, player alive and actually driving.
 * Anything scripted (cutscene, message box, inventory, a door, a death) fails
 * this, which is what keeps the clock from running when the player is not
 * really standing still of their own accord. */
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

    /* Mid-reaction, mid-cutscene-pose or any non-idle state: not AFK, busy. */
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
    u32  held   = (u32)g_Controller0->heldBtnFlags | (u32)g_Controller0->clickedBtnFlags;
    /* Both sticks in one word: look input is analog and never reaches the
     * button flags, so without this a player nudging the camera would still
     * count as idle. */
    s32  stick  = ((s32)g_Controller0->analogController.leftX  << 24) |
                  ((s32)g_Controller0->analogController.leftY  << 16) |
                  ((s32)g_Controller0->analogController.rightX <<  8) |
                  ((s32)g_Controller0->analogController.rightY);
    /* Mouse look does not move a stick either; it lands here. */
    s32  camYaw = g_TpsCamYaw;

    int  moved   = (pl->position.vx != s_prevPosX) || (pl->position.vz != s_prevPosZ);
    int  turned  = (pl->rotation.vy != s_prevRotY) || (camYaw != s_prevCamYaw);
    int  nudged  = (stick != s_prevStick);
    int  pressed = (held != 0);

    s_prevHeld   = held;
    s_prevPosX   = pl->position.vx;
    s_prevPosZ   = pl->position.vz;
    s_prevRotY   = pl->rotation.vy;
    s_prevCamYaw = camYaw;
    s_prevStick  = stick;

    return pressed || moved || turned || nudged;
}

/* Out at the fog wall, dead ahead, so he is a silhouette resolving out of it
 * rather than something that popped into an empty street. */
static void DarkHarry_Spawn(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    q19_12                dist, posX, posZ;
    s32                   sn, cs;
    int                   i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        if (g_SysWork.npcs[i].model.charaId == Chara_None)
            break;
    }
    if (i >= NPC_COUNT_MAX)
        return; /* room is full of real monsters; he can wait */

    /* His model is Harry's, which is always resident, so unlike a console
     * spawn there is nothing to stream and no "not in this map" case. */
    sn   = Math_Sin(pl->rotation.vy);
    cs   = Math_Cos(pl->rotation.vy);
    dist = Q12((float)1.0f) * g_PcConfig.darkHarryDistance;
    posX = pl->position.vx + (s32)(((s64)dist * sn) >> 12);
    posZ = pl->position.vz + (s32)(((s64)dist * cs) >> 12);

    memset(&g_SysWork.npcs[i], 0, sizeof(s_SubCharacter));
    g_SysWork.npcs[i].model.charaId      = Chara_Harry;
    g_SysWork.npcs[i].model.controlState = 0;
    g_SysWork.npcs[i].model.stateStep    = 0;
    g_SysWork.npcs[i].field_40           = (s8)i;
    g_SysWork.npcs[i].position.vx        = posX;
    g_SysWork.npcs[i].position.vz        = posZ;
    Collision_SurfaceGet(&surf, posX, posZ);
    g_SysWork.npcs[i].position.vy        = surf.groundHeight;
    /* Facing the player from the moment he exists. */
    g_SysWork.npcs[i].rotation.vy        = (s16)((pl->rotation.vy + 0x800) & 0xFFF);
    g_SysWork.npcs[i].model.anim.status  = ANIM_STATUS(HarryAnim_Still, false);
    g_SysWork.npcs[i].model.anim.flags  |= AnimFlag_Visible;
    /* Nothing may collide with him or hit him: he is not a monster, and a
     * monster walking into him would shove him off his approach. */
    g_SysWork.npcs[i].collision.state    = CharaCollisionState_Ignore;

    SET_FLAG(&g_SysWork.npcFlags, i);
    SET_FLAG(g_SysWork.field_228C, i);
    {
        /* Same disclaimer as a console spawn: no savegame identity, so killing
         * or clearing him must not dead-flag a real spawn row. */
        extern unsigned char g_PcNpcDebugSpawned[];
        g_PcNpcDebugSpawned[i] = 1;
    }

    s_slot  = i;
    s_stalk = Q12(0.0f);

    /* A continue, taken the moment he appears, so the death he is walking
     * toward costs the player nothing but the walk back. */
    g_GameWork.autosave = g_GameWork.savegame;

    SH_LOG("[DARKHARRY] appeared in npc[%d] at %d units, map %d",
           i, (int)g_PcConfig.darkHarryDistance, (int)g_SavegamePtr->mapIdx);
}

/* He has reached the player. Hand over to the game's own instant death -- the
 * same one the Split Head bite uses -- so the scream, the fade and the game
 * over are the real ones rather than an imitation. */
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
    s_SubCharacter*       me = &g_SysWork.npcs[s_slot];
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    s32 dx    = pl->position.vx - me->position.vx;
    s32 dz    = pl->position.vz - me->position.vz;
    s32 yaw   = ratan2(dx, dz);
    s32 step  = (s32)(((s64)DARK_HARRY_SPEED * g_DeltaTime) >> 12);
    s32 distSq;

    /* Squared compare keeps this to one multiply pair and no square root. */
    distSq = (s32)((((s64)dx * dx) + ((s64)dz * dz)) >> 12);
    if (distSq <= (s32)(((s64)DARK_HARRY_REACH * DARK_HARRY_REACH) >> 12))
    {
        DarkHarry_Take();
        return;
    }

    me->rotation.vy = (s16)(yaw & 0xFFF);
    me->position.vx += (s32)(((s64)step * Math_Sin(yaw)) >> 12);
    me->position.vz += (s32)(((s64)step * Math_Cos(yaw)) >> 12);

    /* Follow the ground rather than the collision system: he is not solid, and
     * a floating or sunken figure breaks the effect instantly. */
    Collision_SurfaceGet(&surf, me->position.vx, me->position.vz);
    me->position.vy = surf.groundHeight;

    me->model.anim.status = ANIM_STATUS(HarryAnim_Still, false);
    me->model.anim.flags |= AnimFlag_Visible;
}

void Pc_DarkHarry_Update(void)
{
    if (!DarkHarry_Eligible())
    {
        /* Leaving gameplay at all takes him with it, so he can never be left
         * standing in a cutscene or survive a map change. */
        if (s_slot >= 0)
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
        /* The brief: he vanishes the instant the player does anything, and the
         * wait starts over from zero before he can return. */
        if (s_slot >= 0)
        {
            SH_LOG("[DARKHARRY] dismissed by input");
            DarkHarry_Despawn();
        }
        s_afk = Q12(0.0f);
        return;
    }

    if (s_slot < 0)
    {
        s_afk += g_DeltaTime;
        if (s_afk >= Q12((float)1.0f) * g_PcConfig.darkHarryAfkSec)
            DarkHarry_Spawn();
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
