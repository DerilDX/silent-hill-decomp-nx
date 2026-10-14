# User report, 2026-10-14: ten issues, organised

Original report was one block of prose. Each item below keeps the user's own wording
in the first line, then what the port does about it. Status key: FIXED (code changed
this session, needs user test), OPEN (cause found, fix pending or needs input),
KNOWN (previously investigated, no PC fault found yet), NEW (not yet investigated).

| # | Area | Item | Status |
|---|------|------|--------|
| 1 | Combat | Split Head (first boss) melee attack: rumble but no flinch, no damage | FIXED |
| 2 | Render | Dahlia first encounter close-up: wall textures missing (no HD pack) | NEW |
| 3 | Audio | Lisa encounter FMV has no music; audio odd at the start of every FMV | NEW |
| 4 | Cutscene | Sewer fence drop before Twinfeeler: fence falls first, then Harry | NEW |
| 5 | Enemies | Rompers hop over things they should not, sometimes out of view | KNOWN |
| 6 | Render | Sewer water: distracting flashlight reflection, Classic mode too | NEW |
| 7 | Render | Projected shadows vanish in menus, pickups, doors, examine | OPEN |
| 8 | Cutscene | Original pauses between dialogue lines gone; wants a launcher option | FIXED |
| 9 | Render | Incubator to Incubus switch effect looks broken | NEW |
| 10 | Render | After final boss: Harry runs on black floor; town shot has no road | NEW |

## 1. Split Head melee does no damage (FIXED)

Split Head's body and charge hits are not a hitbox attack. Every frame the boss computes how
far its segments pushed into Harry since the previous frame and feeds that residual through a
dead zone, a quantiser and the flinch and damage thresholds in `player.c`. All of those were
tuned for a 30 Hz tick. At 240 fps the per-frame residual is one eighth as large, so a full
charge fell under the flinch threshold and a walk fell inside the dead zone. Damage was still
applied, but as a tiny per-frame amount, and the damage rumble fires for any non-zero amount.
That is exactly "rumble, no animation, no health loss".

Fix: `split_head.c` rescales the residual to one 30 Hz tick before the dead zone and
quantiser; `player.c` scales the accumulator and the applied damage back by the frame
fraction so per-second totals match the PSX, and keys the rumble to the per-tick amount.
Other bosses use the attack table and were never affected.

## 5. Romper hops (KNOWN, no PC fault found 2026-09-06)

Read in full before: the hop has no line-of-sight gate, an airborne enemy clears any step
lower than its apex plus 0.5 m, and hop height follows the map's absolute floor height.
All of that is the original formula. "Jumped so high I could not see them" is new data.
The build prints one `[ROMPER-HOP]` line per launch and landing; a log from a session where
it happens, plus the map name, is what is needed to go further.

## 7. Shadows vanish in menus, pickups, doors, examine (OPEN)

Deliberate gate: shadows only run while game state is in-game, system state is gameplay
and no screen fade is active. It was added because the shadow pre-pass produced a white
flash on room-load frames. Investigation in progress to narrow the gate.

## 8. Dialogue pauses (FIXED, launcher 2026.10.14.1)

The PSX paused between voiced lines while the CD seeked to the next clip; the PC loads
instantly. The port already had a minimum-gap setting (`cutscene_line_gap_ms`, default
300 ms, config-file only). The launcher now exposes it as "Dialog Pause": Off (0),
Short (300, default), Medium (700), Long (1200). The tooltip says longer values can push
voices late in tightly timed scenes.
