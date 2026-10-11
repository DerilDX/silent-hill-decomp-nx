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
 *   1. Pc_DarkHarry_Update()  - game_main.c, once per frame
 *   2. Pc_DarkHarry_Draw()    - game_sys_states.c, beside the player's own draw
 *   3. g_PcCharaGhostDraw     - bodyprog_80055028.c, the two chara addPrim sites
 *   4. dark_harry* keys       - pc_config.{h,c} + config.cfg
 *   5. AFKEGG                 - pc_console_cmd.c, a debug trigger
 *   6. g_PcMouseLookTravel    - game_main.c, one line beside the only
 *                               SDL_GetRelativeMouseState gameplay calls (a
 *                               second reader there would eat the camera's
 *                               own mouse motion)
 * Removing the feature = delete this file, pc_dark_harry.c, those hooks.
 *
 * It borrows two things while he is on screen and hands both back on every
 * exit: the radio's second proximity voice (Sfx_RadioStaticLoop, which rises
 * from barely audible to full as he closes) and g_cfg_brightness (the scene
 * darkens as he closes). Neither is written anywhere else in here.
 *
 * Gated on g_PcConfig.darkHarry, and additionally restricted to the maps listed
 * in DARK_HARRY_MAPS (currently map2_s00, the streets). */

/* Once per frame from the main loop. Does nothing at all while the feature is
 * off, out of its map, or the player is not in normal gameplay control. */
void Pc_DarkHarry_Update(void);

/* Draw him, if he is here. Called with the other world draws; poses his own
 * skeleton and renders it see-through. Does nothing when he is not present. */
void Pc_DarkHarry_Draw(void);

/* 1 while the apparition exists. */
int Pc_DarkHarry_Active(void);

/* Put him there right now, ignoring the AFK clock. The AFKEGG console
 * command, for testing without standing still for five minutes. Returns NULL
 * if he is now here, otherwise why not, in words fit to print. */
const char* Pc_DarkHarry_Spawn(void);

/* Drop the apparition and restart the clock. Safe to call at any time. */
void Pc_DarkHarry_Reset(void);

#endif /* SH_PC_PORT */
#endif /* PC_DARK_HARRY_H */
