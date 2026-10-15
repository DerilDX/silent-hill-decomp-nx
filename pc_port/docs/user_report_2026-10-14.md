# User report, 2026-10-14: ten issues, organised

Original report was one block of prose. Each item below keeps the user's own wording
in the first line, then what the port does about it. Status key: FIXED (code changed
this session, needs user test), OPEN (cause found, fix pending or needs input),
KNOWN (previously investigated, no PC fault found yet), NEW (not yet investigated).

| # | Area | Item | Status |
|---|------|------|--------|
| 1 | Combat | Split Head (first boss) melee attack: rumble but no flinch, no damage | FIXED |
| 2 | Render | Dahlia first encounter close-up: wall textures missing (no HD pack) | DROPPED (camera framing shows too much vertically; user call) |
| 3 | Audio | Lisa encounter FMV has no music; audio odd at the start of every FMV | FIXED (start) / OPEN (music) |
| 4 | Cutscene | Sewer fence drop before Twinfeeler: fence falls first, then Harry | FIXED |
| 5 | Enemies | Rompers hop over things they should not, sometimes out of view | KNOWN |
| 6 | Render | Sewer water: distracting flashlight reflection, Classic mode too | FIXED (needs test) |
| 7 | Render | Projected shadows vanish in menus, pickups, doors, examine | FIXED |
| 8 | Cutscene | Original pauses between dialogue lines gone; wants a launcher option | FIXED (reworked as seek delay) |
| 9 | Render | Incubator to Incubus switch effect looks broken | DROPPED (effect shown from the wrong angle; user call) |
| 10 | Render | After final boss: Harry runs on black floor; town shot has no road | FIXED |

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

## 3. FMV audio (start glitch FIXED, Lisa music OPEN)

Start glitch: the disc STR player opened the SDL device on the first audio sector and
unpaused it at once, then fed it one sector per video frame. The queue held 0 to 1 sectors
while the device pulled 108 ms callbacks, so every movie opened with underrun silence gaps.
The device was also never closed, so each movie leaked one device (SDL caps at 16) and the
previous movie's queued tail bled into the next scene.

Fix in `fmv_player.cpp`: decode four frames ahead of the presentation clock (the STR
interleaves its audio with the video, so this fills the queue ahead), hold the device paused
until the first frame is shown so audio time zero matches video time zero, and pause, clear
and close the device when the movie ends or is skipped.

Lisa music: the STR for that movie (M7_01536) carries a single XA channel and its audio is
not static; the quiet music bed is in the file. Two candidates remain, both decided by the
user's log right after `playing FMV (file=2061`:
- `[FMV] SDL_OpenAudioDevice failed` means the device leak above hit the 16-device cap and
  the movie played silent. The close fix above resolves that.
- `[FMV] Container override: gamedata/fmv/M7_01536.mp4` means an FMV mod was playing, and
  the missing music is that file's audio track, not the port.

## 4. Sewer fence drop (FIXED)

Not a cutscene. The grating in map4_s03 is a world object; Harry's ride down is plain
player physics with the grating's collision surface removed. The grating's vertical motion
is delta-time scaled and matched Harry at any frame rate, but its hinge swing accumulated
once per call with no delta-time term and was damped to zero after half a second, so the
final angle was "how many calls fit in 0.5 s": about 39 degrees on the PSX, 86 degrees at
240 fps. The grating flipped flat before Harry's fall was visible.

Fix in `map4_s03.c`: the hinge is stepped at the PSX 30 Hz cadence with the PSX per-step
gravity, so the swing matches the original at any frame rate.

## 5. Romper hops (KNOWN, no PC fault found 2026-09-06)

Read in full before: the hop has no line-of-sight gate, an airborne enemy clears any step
lower than its apex plus 0.5 m, and hop height follows the map's absolute floor height.
All of that is the original formula. "Jumped so high I could not see them" is new data.
The build prints one `[ROMPER-HOP]` line per launch and landing; a log from a session where
it happens, plus the map name, is what is needed to go further.

## 6. Sewer water reflection (OPEN, needs a screenshot)

Not a regression in the water code: `water.c` is unchanged since July. What changed is that
on 2026-07-27 the flashlight's reflective octagon on the water (an eight-sided dark disc
ahead of Harry that turns with him, two opaque layers plus two additive ones) was restored in
all flashlight modes after being hidden the day before. Its colours are hardcoded, which is
why Classic mode does not change it. On PSX the water surface's caustic texels with the STP
bit clear paint opaque over that disc; the PC adds them instead, so the dark disc stays
fully visible under the lit water and reads as a hard-edged hole.

Options: draw the octagon's two opaque layers semi-transparent and additive (warmth only,
no occlusion), or drop them and keep the dark outer ring, or the engine-faithful route of
honouring STP=0 texels as opaque on semi-transparent textured prims in PsyCross. A screenshot
decides which prim the user is seeing: an eight-sided dark disc ahead of Harry that rotates
with him, or a bright patch directly under him that brightens as he faces the camera (that
one is the beam highlight the user approved in July).

## 7. Shadows vanish in menus, pickups, doors, examine (FIXED)

Two layered causes. The master gate only allowed shadows in the plain gameplay and pause
sub-states with no fade, so every live-rendered frame outside those (the whole of an examine
message, the item-pickup tick that gets frozen behind the item, both door fades, the first
tick of the status and map screens) rendered without shadows. Separately, the shadow depth
pre-pass ran once per ordering table, and the UI table has no casters, so every frame ended
with the shadow map cleared. The shader was already allowed to sample the "last" map on
gated frames, but that map was always empty.

The white flash the gate was written against (2026-07-05) came from the pass leaving GL
blending disabled, so the subtractive full-screen fade tile drew as opaque white; the
blend tracker was fixed on 2026-07-20 but the gate was never revisited.

Fix: the gate is now the positive "world was submitted this tick" flag; the pre-pass opens
lazily on the first caster so a casterless batch neither clears the map nor touches GL
state; and the scissor-state tracker now applies the requested state instead of toggling
the previous one (a real leak after the pass's sentinel reset).

## 8. Dialogue pauses (FIXED, launcher 2026.10.14.1)

The PSX paused between voiced lines while the CD seeked to the next clip; the PC loads
instantly. The port already had a minimum-gap setting (`cutscene_line_gap_ms`, default
300 ms, config-file only). The launcher now exposes it as "Dialog Pause": Off (0),
Short (300, default), Medium (700), Long (1200). The tooltip says longer values can push
voices late in tightly timed scenes.

## 10. Ending floor black, town road missing (OPEN, needs a log)

Both are world objects, not procedural prims: `URA_HIDE` and `UNDER_HI` (the otherworld
run floor) and `REAL_HID` (the town road), three scrolling 20-unit tiles placed around
(100, 0, -140) by `func_800E98EC` in map7_s03_3.c and switched on per ending step. Their
geometry lives only in chunk ER02FC (cell 2,-4). A world object resolves its model through
the chunk at the player's cell on interior maps, and the PC drops any world object whose
chunk has no resident textures (the June gate in world_draw.c that stopped far props drawing
with garbage pages). The player is moved to (140, -100), cell (3,-3), for the town shot.

Candidates, in order:
- The object resolves but its chunk is loaded-but-untextured, so the draw gate drops it
  every frame. The log prints `[WOBJ-TEX] 'REAL_HID'` only when it is drawn.
- The object never resolves (`[WOBJ] find-fail name='REAL_HID'`, printed once per name).
- The 2026-09-17 change that limits map7_s03 to drawing exactly the player's cell. It
  gates chunk geometry, not world objects, so it is the least likely of the three.

What settles it in one step: a SilentHill.log from a run through the ending (grep for
`WOBJ`, `IPD-INIT` and `playing FMV`) plus a screenshot of the running segment.

## 10, revisited (FIXED 2026-10-15, commit 2e239d3c4)

The ending-session log settled it: cell (2,-4), the arena chunk that holds the run floor
and the town road, sat in chunk slot 1, which still pointed into the shared PSX RAM slice
under FS_BUFFER_18/20. The ending streams its DMS files there, so the chunk bytes became
garbage, every reformat retry failed (`[IPDREF] HIT-MISMATCH` each frame, `isLoaded=0`),
and the three world objects reported `find-fail`. Collision was gone too, which is why
Harry seemed to run on nothing. Every chunk slot now owns a heap buffer.

## 8, revisited (FIXED 2026-10-15, commit b8defebbb)

The end-of-line hold was the wrong model: it moved the page advance, which is why it
needed a caveat. The PSX pause is a CD seek at the START of each voice line: the subtitle
appears, the voice begins a beat later, and the scene runs on its authored timers. The
setting now delays only the start of playback; subtitle and scene timing are untouched.
Launcher presets: Off / Short / Normal / Long = 0 / 150 / 300 / 600 ms, Normal default.

## 6, revisited (OPEN)

Screenshot received. The circular reflection is the water.c octagon: four prims with
vertex colours of 4 to 24 out of 255, which on the PSX is almost invisible over black
water (the user's PSX reference shows only the soft purple feedback patch). On the PC it
renders as three flat concentric bands at roughly 60 to 80 brightness, so something in the
PC path brightens untextured gouraud prims that carry view-space depth, or loses the
gouraud gradient. Not resolved by reading; needs a rendered measurement (VOIDPROBE-style
dump of that frame) before changing anything.

## New: model flicker and Cybil shrinking for a frame in the ending

Reported with the ending replay. No lines in the log point at it (no [COORD] guard hits).
Not investigated yet.

## 6, resolved (2026-10-15, PsyCross 5a729ca, main 7a4a5a572)

The water surface is a palette trick. The game composites two scrolled copies of a 4-bit wave
texture into a 32x32 VRAM tile, then the water polygons sample that tile back as a 4-bit
texture through a different palette, so every 16-bit colour word turns into four palette
indices. The look only comes out right when the words are bit-exact PSX GPU output. On the
PC the tile went through the GL offscreen path (shader modulation, filtering, dither, an RGBA
readback and a repack whose STP bit was set pseudo-randomly by an operator-precedence bug),
so the water came back as mid-grey noise, and the flashlight's dark octagon underneath showed
through as flat bands because additive blending ignored the STP bit that makes the PSX draw
those texels solid.

Fix: the tile is now composited in software with the GPU's own arithmetic, additive
primitives follow the STP rule (set = add, clear = solid) the way the 50/50 mode already did,
and the pack bug is fixed. Measured from the screenshot: the band brightness steps matched the
octagon's authored colours exactly, and the scene's black floor of 36 is the brightness 1.3
setting on the quantized output, not a rendering fault.
