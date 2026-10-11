/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "Dark Harry" easter egg. See pc_dark_harry.h for what it is and how to
 * remove it. Nothing in here is original-game behaviour. */
#include "pc_dark_harry.h"

#include "game.h"

#include <psyq/libgte.h>

#include "bodyprog/anim.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/items.h"
#include "bodyprog/map/map.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/screen/screen_data.h" /* g_DeltaTime */
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/view/vc_util.h"
#include "maps/characters/harry.h"
#include "pc_config.h"
#include "sh_log.h"

extern void func_8003DA9C(e_CharaId charaId, GsCOORDINATE2* boneCoords, s32 arg2, q3_12 timer, s32 arg4);
extern int  g_PcCharaGhostDraw;

/* Where he is allowed to appear. The fog exteriors are the point of the thing;
 * indoors there is no "edge of visibility" to fade out of. The streets, where
 * the player has had the radio for a long time. */
static const s8 DARK_HARRY_MAPS[] = {
    MapIdx_MAP2_S00,
};

/* Walk speed in world units per second. Far below Harry's walk: the effect is
 * noticing the shape is nearer than it was, not watching something approach. */
#define DARK_HARRY_SPEED Q12(0.35f)

/* Close enough to touch. Harry's own collision radius is 0.3. */
#define DARK_HARRY_REACH Q12(0.9f)

/* Give up if he cannot reach the player (geometry between them, player on a
 * ledge) rather than trailing around for the rest of the session. */
#define DARK_HARRY_MAX_STALK_SEC 120

/* Radio attenuation the moment he appears. The scale is backwards: 0 is full
 * volume and 255 is silence, so this is "barely there", and it walks down to 0
 * as he closes. */
#define DARK_HARRY_RADIO_FAINT 232

/* Brightness multiplier when he is on top of the player. Not zero -- the fade
 * is meant to read as the light going out of the scene, not as a black screen
 * that looks like the game died. */
#define DARK_HARRY_DARKEN_MIN 0.12f

/* Dismissal is measured at the INPUT DEVICES, never at Harry.
 *
 * Watching his position, his body rotation and the camera yaw is what made the
 * egg nearly impossible to trigger. The game has an AFK look-around of its own
 * (PlayerState_Idle, player_control.c:5729): ten seconds of standing still and
 * Harry shifts his weight and glances about, and the camera code may re-seed
 * g_TpsCamYaw from his body. Every proxy for "the player did something" moves
 * by itself at exactly the moment the player has finally stood still long
 * enough -- so the egg dismissed itself, and the clock could never climb past
 * those ten seconds.
 *
 * Buttons, sticks and mouse travel are the causes. Position stays only as a
 * backstop for something that moves him without being the player (a console
 * command bound to a key), at a tolerance far wider than any drift. */
#define DARK_HARRY_MOVE_TOL  Q12(1.0f) /* world units */
#define DARK_HARRY_MOUSE_TOL 40        /* pixels of travel; a desk knock is a few */
#define DARK_HARRY_STICK_DZ  24        /* of 128; the same deadzone the camera uses */

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

static q19_12 s_afk;       /* seconds of no input */
static q19_12 s_stalk;     /* seconds since he appeared */
static int    s_armed;     /* 0 until the player has had control for a frame */
static q19_12 s_spawnDist; /* how far out he started, for the approach curve */
static q19_12 s_dist;      /* current distance to the player */

/* Effects he owns while present, each restored on the way out. */
static int   s_radioVoice;  /* 1 = we started the static loop */
static int   s_brightHeld;  /* 1 = s_brightBase holds the player's own setting */
static float s_brightBase;

/* Input baseline, retaken whenever the clock resets. */
static s32      s_baseX, s_baseZ;
static unsigned s_baseMouse;

int Pc_DarkHarry_Active(void)
{
    return s_present;
}

/* Hand back the radio and the brightness. Called from every exit so he cannot
 * leave the screen dark or the static running. */
static void DarkHarry_EffectsClear(void)
{
    extern float g_cfg_brightness;

    if (s_radioVoice)
    {
        Sd_SfxStop(Sfx_RadioStaticLoop);
        s_radioVoice = 0;
    }

    if (s_brightHeld)
    {
        g_cfg_brightness = s_brightBase;
        s_brightHeld     = 0;
    }
}

static void DarkHarry_Despawn(void)
{
    DarkHarry_EffectsClear();
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

/* NULL when he is allowed to be here: normal gameplay, on one of his maps,
 * player alive and actually driving. Anything scripted (cutscene, message box,
 * inventory, a door, a death) names itself instead, which keeps the clock from
 * running when the player is not standing still of their own accord. The
 * reason is what AFKEGG reports and what the log records when he vanishes;
 * this path used to drop him in total silence, which is how the bug below hid
 * behind three rounds of "he just disappears". */
static const char* DarkHarry_Ineligible(void)
{
    u32 state;
    int i;

    if (!g_PcConfig.darkHarry)
        return "dark_harry is off in the config";
    if (g_GameWork.gameState != GameState_InGame)
        return "not in gameplay";
    if (g_SysWork.sysState != SysState_Gameplay)
        return "a menu or a script owns the scene";
    if (g_SavegamePtr == NULL)
        return "no savegame";

    for (i = 0; i < (int)(sizeof(DARK_HARRY_MAPS) / sizeof(DARK_HARRY_MAPS[0])); i++)
    {
        if (g_SavegamePtr->mapIdx == DARK_HARRY_MAPS[i])
            break;
    }
    if (i == (int)(sizeof(DARK_HARRY_MAPS) / sizeof(DARK_HARRY_MAPS[0])))
        return "wrong map";

    if (g_SysWork.playerWork.player.health <= Q12(0.0f))
        return "the player is already dead";

    /* PlayerState_Idle is the game's OWN AFK look-around, not a scripted
     * takeover: after ten seconds of standing still Harry starts glancing
     * around (player_control.c:5729), and he stays in that state until the
     * player does something. Rejecting it capped this feature's clock at those
     * same ten seconds -- the egg could never reach its own AFK time by
     * standing still, and AFKEGG refused from the exact state the player is in
     * when they type it. It is the best proof there is that nobody is driving.
     *
     * The death still works from it: the health<=0 branch at the end of
     * Player_ReceiveDamage excludes only the death and grab states. */
    state = (u32)g_SysWork.playerWork.extra.state;
    if (state != PlayerState_None && state != PlayerState_Idle)
        return "the player is not in normal control";

    return NULL;
}

static void DarkHarry_BaselineSet(void)
{
    extern unsigned g_PcMouseLookTravel;

    const s_SubCharacter* pl = &g_SysWork.playerWork.player;

    s_baseX     = pl->position.vx;
    s_baseZ     = pl->position.vz;
    s_baseMouse = g_PcMouseLookTravel;
}

/* Deflection from CENTRE past the deadzone, not drift from a baseline: a stick
 * at rest never leaves the deadzone however worn it is, and a deliberate push
 * always does. 128 is centre on these u8 axes. */
static int DarkHarry_StickPushed(u8 axis)
{
    s32 d = (s32)axis - 128;

    return d > DARK_HARRY_STICK_DZ || d < -DARK_HARRY_STICK_DZ;
}

/* NULL = the player has not touched anything, otherwise a short name for what
 * they touched (for the dismissal log line). */
static const char* DarkHarry_InputSeen(void)
{
    extern unsigned g_PcMouseLookTravel;

    const s_SubCharacter*     pl = &g_SysWork.playerWork.player;
    const s_AnalogController* an = &g_Controller0->analogController;
    u32 btns = (u32)g_Controller0->heldBtnFlags | (u32)g_Controller0->clickedBtnFlags;

    if (btns != 0)
        return "button";

    /* Look input is analog and never reaches the button flags. */
    if (DarkHarry_StickPushed(an->leftX)  || DarkHarry_StickPushed(an->leftY) ||
        DarkHarry_StickPushed(an->rightX) || DarkHarry_StickPushed(an->rightY))
        return "stick";

    /* Mouse look moves no stick and no button, and the camera yaw is no proxy
     * for it -- the game writes that itself. The travel counter only ever
     * grows on real motion, and the look code freezes it while the console has
     * focus, so typing AFKEGG cannot dismiss him. */
    if (g_PcMouseLookTravel - s_baseMouse > DARK_HARRY_MOUSE_TOL)
        return "mouse";

    if (ABS(pl->position.vx - s_baseX) > DARK_HARRY_MOVE_TOL ||
        ABS(pl->position.vz - s_baseZ) > DARK_HARRY_MOVE_TOL)
        return "move";

    return NULL;
}

const char* Pc_DarkHarry_Spawn(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    q19_12                posX, posZ;
    s32                   dx, dz;
    const char*           why;

    if (s_present)
        return "he is already here";
    /* Also covers AFKEGG, so the command can report honestly instead of
     * placing him somewhere the next frame would take him away from. */
    why = DarkHarry_Ineligible();
    if (why != NULL)
        return why;

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

    dx = pl->position.vx - s_pos.vx;
    dz = pl->position.vz - s_pos.vz;

    s_present   = 1;
    s_posed     = 0;
    s_stalk     = Q12(0.0f);
    s_dist      = SquareRoot12((s32)((((s64)dx * dx) + ((s64)dz * dz)) >> 12));
    s_spawnDist = s_dist;

    /* A continue, taken the moment he appears, so the death he is walking
     * toward costs the player nothing but the walk back. */
    g_GameWork.autosave = g_GameWork.savegame;

    SH_LOG("[DARKHARRY] appeared at (%d,%d,%d), player at (%d,%d,%d), %d units out",
           FP_FROM(s_pos.vx, Q12_SHIFT), FP_FROM(s_pos.vy, Q12_SHIFT), FP_FROM(s_pos.vz, Q12_SHIFT),
           FP_FROM(pl->position.vx, Q12_SHIFT), FP_FROM(pl->position.vy, Q12_SHIFT),
           FP_FROM(pl->position.vz, Q12_SHIFT), FP_FROM(s_spawnDist, Q12_SHIFT));

    return NULL;
}

/* He has reached the player.
 *
 * NOT the Split Head instant death: that state animates out of the boss map's
 * own anim table (field_38[D_800AF220]) and ends on one exact keyframe in it,
 * which a map carrying no row for it can never reach -- and in map0_s00 it
 * routes to the scripted wake-up in the cafe instead of a game over. He walked
 * up, the state was set, and nothing happened. Zeroing health hands over to
 * the death every enemy in the game uses, which works on every map: the scream,
 * the collapse and the Game Over screen are the real ones. */
static void DarkHarry_Take(void)
{
    s_SubCharacter* pl = &g_SysWork.playerWork.player;

    SH_LOG("[DARKHARRY] reached the player - death");
    DarkHarry_Despawn();
    s_afk = Q12(0.0f);

    /* 66 is the attack that routes to the drowning variant; anything stale in
     * here would pick the wrong death. */
    pl->attackReceived = NO_VALUE;
    pl->health         = Q12(0.0f);

    /* The scream. The normal death has no voice of its own -- the cry a dying
     * player hears belongs to the hit that killed them, and nothing hit them
     * here -- so he died silently. This is the clip the Split Head death plays
     * (player_control.c, PlayerState_InstantDeath). */
    SD_Call(Sfx_XaAudio635);
}

/* 0 at the distance he appeared from, Q12(1.0) at arm's length. */
static q19_12 DarkHarry_Closeness(void)
{
    q19_12 span = s_spawnDist - DARK_HARRY_REACH;
    q19_12 gone;

    if (span <= Q12(0.5f))
        return Q12(1.0f);

    gone = s_spawnDist - s_dist;
    if (gone <= Q12(0.0f))
        return Q12(0.0f);

    return CLAMP((s32)(((s64)gone * Q12(1.0f)) / span), Q12(0.0f), Q12(1.0f));
}

/* The radio hears him and the light goes out of the scene, both tied to the
 * same closeness so they arrive together. */
static void DarkHarry_EffectsUpdate(void)
{
    extern float g_cfg_brightness;

    q19_12 closeness = DarkHarry_Closeness();

    /* Nothing to pick him up if the radio is off or not found yet, which is
     * why he was moved to a map the player reaches it on. */
    if (g_SavegamePtr->itemToggleFlags & ItemToggleFlag_RadioOn)
    {
        s32 att = DARK_HARRY_RADIO_FAINT - (s32)(((s64)DARK_HARRY_RADIO_FAINT * closeness) >> 12);

        /* The static loop rather than the interference loop: npc_main fills its
         * two radio slots in order, so the second one is free unless two real
         * enemies are already in range. Sd_SfxAttributesUpdate restarts a
         * stopped radio voice by itself, so this both starts and drives it. */
        Sd_SfxAttributesUpdate(Sfx_RadioStaticLoop, Vc_StereoBalanceGet(&s_pos),
                               (u8)CLAMP(att, 0, 255), 0);
        s_radioVoice = 1;
    }

    if (!s_brightHeld)
    {
        s_brightBase = g_cfg_brightness;
        s_brightHeld = 1;
    }
    g_cfg_brightness = s_brightBase *
                       (1.0f - ((float)closeness / 4096.0f) * (1.0f - DARK_HARRY_DARKEN_MIN));
}

static void DarkHarry_Approach(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    s32 dx   = pl->position.vx - s_pos.vx;
    s32 dz   = pl->position.vz - s_pos.vz;
    s32 yaw  = ratan2(dx, dz);
    s32 step = (s32)(((s64)DARK_HARRY_SPEED * g_DeltaTime) >> 12);

    s_dist = SquareRoot12((s32)((((s64)dx * dx) + ((s64)dz * dz)) >> 12));
    if (s_dist <= DARK_HARRY_REACH)
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

    DarkHarry_EffectsUpdate();
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
    const char* why;

    why = DarkHarry_Ineligible();
    if (why != NULL)
    {
        /* Leaving gameplay at all takes him with it, so he can never be left
         * standing in a cutscene or survive a map change. Logged only when he
         * was actually here, so this is one line per appearance and never a
         * per-frame probe. */
        if (s_present)
            SH_LOG("[DARKHARRY] gone: %s", why);

        DarkHarry_Despawn();
        s_afk   = Q12(0.0f);
        s_armed = 0;
        /* Keep the input baseline current so re-entering gameplay does not
         * read the position jump as "the player moved". */
        DarkHarry_BaselineSet();
        return;
    }

    /* One eligible frame before the clock may start, so the frame control is
     * handed back (door, cutscene end) is never counted as idle. */
    if (!s_armed)
    {
        s_armed = 1;
        s_afk   = Q12(0.0f);
        DarkHarry_BaselineSet();
        return;
    }

    why = DarkHarry_InputSeen();
    if (why != NULL)
    {
        if (s_present)
        {
            SH_LOG("[DARKHARRY] dismissed by input (%s)", why);
            DarkHarry_Despawn();
        }
        s_afk = Q12(0.0f);
        DarkHarry_BaselineSet();
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
