/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_DARK_HARRY_H
#define PC_DARK_HARRY_H

#ifdef SH_PC_PORT

/* "Dark Harry" — an easter egg that is NOT in the original game.
 *
 * Stand still outdoors in the fog long enough and a translucent copy of Harry
 * fades in at the edge of visibility and starts walking at you. Touch any
 * control and he is gone. Let him reach you and he kills you.
 *
 * DELIBERATELY SELF-CONTAINED so it can be pulled out again. Everything lives
 * in pc_dark_harry.c; outside it there are only these, each a single line:
 *   1. Pc_DarkHarry_Update()        — game_main.c, once per frame
 *   2. Pc_DarkHarry_IsGhost(slot)   — world_draw.c, to draw the copy see-through
 *   3. g_PcCharaGhostDraw           — bodyprog_80055028.c, the two chara addPrim sites
 *   4. dark_harry* keys             — pc_config.{h,c} + config.cfg
 *   5. AFKEGG                       - pc_console_cmd.c, a debug trigger
 * Removing the feature = delete this file, pc_dark_harry.c, those hooks.
 *
 * Gated on g_PcConfig.darkHarry, and additionally restricted to the maps listed
 * in DARK_HARRY_MAPS (currently map0_s00 only). */

/* Once per frame from the main loop. Does nothing at all while the feature is
 * off, out of its map, or the player is not in normal gameplay control. */
void Pc_DarkHarry_Update(void);

/* 1 while the apparition exists. The chara draw pairs this with "a Harry draw
 * that is not the player's" to spot him, which is exact because he is the only
 * other Harry that can be in the scene. */
/* Draw him, if he is here. Called with the other world draws; poses his own
 * skeleton and renders it see-through. Does nothing when he is not present. */
void Pc_DarkHarry_Draw(void);

int Pc_DarkHarry_Active(void);

/* Put him there right now, ignoring the AFK clock. The AFKEGG console
 * command, for testing without standing still for five minutes. */
void Pc_DarkHarry_Spawn(void);

/* Drop the apparition and restart the clock. Safe to call at any time. */
void Pc_DarkHarry_Reset(void);

#endif /* SH_PC_PORT */
#endif /* PC_DARK_HARRY_H */
