# Prop Physics & Wind-Driven Foliage: Feasibility & Options

Status: **RESEARCH ONLY. Nothing is implemented.** Don't start coding until we've picked an option.
Date: 2026-09-26

## What was asked

1. Small props (chairs, potted plants and the like) should react physically: slide or tip
   over when Harry or an enemy runs into them.
2. Each scene has a wind setting that almost nothing uses. Tree billboards and plants
   should sway with it.
3. Both must be optional and off by default. The best case needs no asset edits. If edits
   turn out to be required, players do that setup themselves.

## TL;DR

- **The engine is friendlier to this than expected.** Every map prop is drawn from its own
  per-instance `MATRIX`. The game can draw any named model at any pose, lighting follows
  the world matrix, and small-prop collision is already a separate primitive (a
  **cylinder**) with its own enable gate. We don't have to bake or cut any geometry to move a prop.
- **The one real unknown is data, not code:** are chairs and plants their *own* model
  instances inside the `.IPD` chunks, or merged into the room mesh? The TrenchBroom doc
  says "props are instanced", but nobody has made a per-prop list. **We need a local
  data survey before choosing** (see [Next step](#next-step-a-data-survey)).
- **Wind foliage is the cheap, low-risk half.** Billboard trees can sway with a small
  change inside `Gfx_BillboardDraw` (no physics library needed). 3D plant models can
  sway through a shear on their instance matrix. The scene "wind" really is the snow
  drift speed, so it's zero in most places. We need an ambient-breeze source too.
- **Recommended library: [Box3D](https://github.com/erincatto/box3d)** (MIT, pure C17,
  MinGW-supported, triangle meshes/convex hulls/sleeping built in). It matches our C
  codebase and vendors like `third_party/lua`. It's v0.1, so the fallback is **Jolt
  Physics** via the **JoltC** wrapper (mature, C++). Hide either one behind a small
  `pc_physics.h` so we can swap them.
- **Recommended first scope: "soft" props.** Props get shoved by Harry and enemies but
  never block or change Harry's movement. That keeps gameplay, demo playback and save
  files identical to stock. Props that block Harry would be a separate, later opt-in.

---

## What the engine gives us (findings)

### 1. Map props are instances with their own transform

`include/bodyprog/formats/ipd.h`:

```c
typedef struct _IpdModelInstance {
    s_ModelHeader* modelHdr;   // resolved by name from the chunk's LM or the *_GLB.PLM
    MATRIX         mat;        // chunk-local transform (Q12 rotation, Q8 translation)
} s_IpdModelInstance;
```

Each chunk has `modelBuffers[]`, and each buffer has `modelInstances[]`. Model headers
carry an 8-char `name` (`s_ModelHeader::name`), so every instance can be identified by
name. `Ipd_ChunkDraw` (`src/bodyprog/gfx/bodyprog_80040B74.c`) copies `curBufC->mat`
into `modelCoord.workm`, adds the cell offset and draws. There are three copies of that
loop (debug/whole-map at :3404, the PC interior path at :3440, the original PSX PVS path
at :3472), and each needs the same hook.

**What this means:** to move a prop we swap in a different matrix right there. No mesh
editing, no re-baking.

### 2. The engine can already draw a named model at any pose

`WorldGfx_ObjectAdd(model, pos, rot)` (`src/bodyprog/gfx/world_draw.c`) looks up a
model by name in the loaded chunk LMs and the item LM, then draws it at an arbitrary
position and rotation. Map scripts use it for keys, doors, the `CHAIR_HI`/`ISU_HIDE` objects
and so on. It's a second hook option: hide the static instance and re-submit the prop
as a world object. For stock chunk props, hooking the instance matrix (finding 1) is
simpler.

### 3. Lighting follows the matrix

`func_80057090` transforms the light/flashlight vectors into model space through
`worldMat` (`func_80057228`). A rotated or tipped prop is lit correctly by the
flashlight without extra work.

### 4. Collision is 2.5D, and small props use cylinders

`s_IpdCollisionData` has two primitive kinds (`src/bodyprog/collision/collision.c`):

| Kind | Table | Shape | Enable gate |
|---|---|---|---|
| Edge ("subcell") | `subcells` | Line between two `splitVertices`, a floor surface on each side; `0xFF` = none = wall | `func_8006B318` (:1341), 4-bit category vs `state->flags` |
| Obstacle | `ptr_18` | **Cylinder**: `offset` = centre, `field_8` = radius | `func_8006C3D4` (:2062), category `field_0_8` vs `state->flags` |

A potted plant, pillar or bin is almost certainly a `ptr_18` cylinder. Box-like props
(tables, desks) would be a closed loop of edges with no surface inside.

Both test functions start with a single early `return false`. A PC-only
"disabled element" bitmask per chunk (≤256 elements each, so 32 bytes) slots in right
next to it. We **can't** reuse the game's category bits: they're shared with other
walls and the story uses them (e.g. the school valve puzzle).

The existing collision visualizer (`'` key, `CollVis_*` in `pc_port/src/dbg_overlay.c`)
already draws both edges and cylinders, so we can check what a given prop has.

### 5. The scene "wind" is really the snow drift speed

`s_MapOverlayHdr::windSpeedX/Z` (`include/bodyprog/map/map.h:518`) point to
`g_Particle_SpeedX/Z` in all 43 map headers. `src/maps/particle.c` sets them:

- Non-zero only for `SnowType_LightWindy`/`HeavyWindy`, and only while the snow
  system reports wind. They ramp up to 200..800 with random ±8 jitter and ramp back to 0
  otherwise.
- Readers: snow particles (`particle.c`) and one smoke/steam effect drifter
  (`bodyprog_8005E0DC.c:2771`, `:2965`).

So interiors and non-snow exteriors always read 0. Foliage sway needs an **ambient
breeze** of its own, and the snow wind can add to it when present.

### 6. Tree and bush billboards are a small point cloud

The IPD model buffer's `field_10` list places billboards: `pad 0` calls
`Gfx_BillboardDraw(1, …)` and `pad 1` calls `Gfx_BillboardDraw(2, …)`. Each type is a fixed clump of
sprites from `D_800AE204[]` (`item_screens_3.c`): type 1 = 12 sprites stacked
vertically (a tree), type 2 = 5 low sprites (a bush). Each sprite is one point
(`RotTransPers`) that becomes a screen-space diamond quad
(`bodyprog_80055028.c:5088`).

To make them sway, offset each sprite's point sideways before projecting it. Scale the
offset by the sprite's height (`positionY`, where −Y is up) and use a per-tree phase
from its world XZ. That's a copy into a local `SVECTOR`; the shared table is never
modified.

### 7. The port already renders through the CPU/GTE path

There's no separate modern world-mesh renderer to keep in sync. `pc_modern_mesh.c` is
items only. Matrix changes are enough for any backend (GL, ANGLE D3D11, Vulkan).

---

## The big unknown: how are props stored in the data?

There are three possible outcomes. Only a data survey can tell us which applies,
and it may differ per area:

| Case | What it looks like | Consequence |
|---|---|---|
| **A. Own instance** | The chair is its own `s_IpdModelInstance` (e.g. a shared `CHAIR` mesh placed 4× in a classroom) | Fully doable with **no asset edits** |
| **B. Merged mesh** | The chair's polys are part of a room mesh (`ROOM01`) | Can't be moved without editing assets. A player can split it into its own instance with the existing TrenchBroom pipeline (`ipd2map.py` → `map2ipd.py`, loose-file override). The compiler already emits rigid moves as instances |
| **C. Global PLM** | The instance points to the `*_GLB.PLM` (`isGlobalPlm`) | Same as A. The mesh is shared across the area |

Hints so far: the TrenchBroom doc says "props are instanced… several copies share one
mesh", and the scripts use names like `CHAIR_HI` and `ISU_HIDE`. So Case A is likely
common. Small clutter in older areas could still be merged (Case B).

---

## Options: props

### How props are chosen

1. **Name whitelist (recommended).** Ship `physics_props.cfg` with lines like
   `CHAIR01 mass=6 shape=hull friction=0.6`. The data survey tells us which names to
   list. Players can edit or extend it, and it works for modded chunks too.
2. **Automatic heuristic.** Treat an instance as a prop when its bounding box is small
   (e.g. < 1.2 × 1.2 × 1.5 units), it isn't the chunk shell, and its mesh is shared by
   more than one instance. Good for discovery, too risky as the only rule: it would catch
   door frames, light fixtures and wall trims.
3. **Hybrid.** Use the heuristic to generate a candidate list in the survey, then
   whitelist entries by hand.

### How much the physics affects gameplay

| Tier | Behaviour | Gameplay change | Notes |
|---|---|---|---|
| **1. Cosmetic / soft (recommended first)** | Harry and enemies are kinematic capsules in the physics world. Props get shoved and tip over; Harry's movement is never changed | **None.** Harry's movement, demo playback (`MISC/*.DAT`) and saves stay identical to stock | Harry keeps colliding with nothing where the prop was, so the prop's baked cylinder/edge loop must be disabled or he stops at an invisible prop. A pinned prop can clip into Harry |
| 2. Blocking | As tier 1, plus heavy or pinned props push back on Harry through the PC hook in the chara collision response | Yes: movement changes | Must be forced off during demo playback. A separate opt-in |
| 3. Full | Enemies path around props, bullets knock them | Yes | Enemy AI pathing ignores props, so this would need AI changes. Not recommended |

Bullet impacts are a cheap add-on even in tier 1. When a weapon fires, cast one ray
into the physics world only, so the shot never changes what the game hits.

### Handing collision over

When a prop becomes physical:

1. **Find its baked collision.** At chunk load, match `ptr_18` cylinders whose centre
   is inside the prop's XZ footprint, and closed edge loops fully inside it (with a small
   margin).
2. **Mask it** in the PC per-chunk bitmask (finding 4). Chosen option: at load, or
   only once the prop has moved more than ~0.1 units from home. The second is safer,
   because an untouched prop still blocks exactly like stock.
3. The prop's physics body now does the colliding.

Where no baked collision is found (Harry already walks through the prop in stock), there's nothing to mask.

### The static world the props collide with

| Source | Pros | Cons |
|---|---|---|
| **Render meshes of the non-prop instances** in the loaded chunks → triangle mesh | Exact floors and furniture tops, so a knocked chair can land on a table | Needs the `s_Primitive` → triangle decode (belek666's `sh_ipd2obj` and our `ipd2map` already do it) |
| **IPD collision extruded**: edges → vertical quads, surfaces → floor planes | Keeps props inside the space the player can walk | Coarse vertically; no tabletops |
| **Both (recommended)** | Render mesh for shape; extruded collision walls as invisible barriers | A little more setup per chunk, and cheap at runtime |

Build this per chunk on load and destroy it on unload, alongside `Ipd_Init` and the chunk slot LRU.

### Persistence

Keep prop state for the session only, keyed by `(ipd file idx, buffer idx, instance
idx)`. A chair you knock over stays down if you leave and come back. It resets on
restart and **never goes in the save file**. Props that scripts drive (keys, doors, anything
drawn via `WorldGfx_ObjectAdd`) stay out of the whitelist, since game logic tracks those.

---

## Options: physics library

Project license is GPL-3.0-or-later. All of these are compatible.

| Library | License | Language | Fit | Notes |
|---|---|---|---|---|
| **[Box3D](https://github.com/erincatto/box3d)** | MIT | **C17** | Best fit | By Erin Catto (Box2D). Released June 2026. Convex hulls, capsules, triangle meshes, height fields, sleeping, contact events, character mover, deterministic. MSVC/MinGW/GCC/Clang, no dependencies. **v0.1, young API** |
| **[Jolt Physics](https://github.com/jrouwe/JoltPhysics)** + **[JoltC](https://amerkoleci-joltc.mintlify.app/)** | MIT | C++17 (+C wrapper) | Strong fallback | Mature (Horizon Forbidden West, Death Stranding 2). MSYS2 ships a MinGW package. Adds C++ to a mostly C build (we already link some C++) |
| Bullet 3 | zlib | C++ | OK | Mature but in maintenance mode. Its C API is aimed at robotics |
| ReactPhysics3D / ODE | zlib / BSD+LGPL | C++ / C | Weaker | Nothing we need that the two above don't do better |
| Hand-written mini solver | — | C | Not recommended | Boxes/hulls against a triangle mesh with stable stacking is weeks of work that Box3D already does |

**Recommendation:** Box3D, vendored under `pc_port/third_party/box3d` and built as a static
lib the way `lua_static` is, behind a small `pc_physics.h` (world create/destroy, add
static mesh, add body, step, query transform, ray cast) so we can drop in Jolt if Box3D's
early API churns.

**Timing:** use a fixed 60 Hz step and interpolate transforms for the renderer, as
`Task_HighFPS_Keyframe_Audit.md` does. Physics must never read `g_DeltaTime` directly.
A room has maybe 5 to 40 bodies, which is negligible cost.

---

## Options: wind and foliage

### Wind source

- `wind = sceneWind + ambientBreeze`
- `sceneWind` = `*g_MapOverlayHdr.windSpeedX/Z` (snow drift), scaled down.
- `ambientBreeze` = a slow noise-driven gust vector. Default to exteriors
  (`g_Map.isExterior`) only, with a strength slider. Interiors get none unless the player
  opts in (an open window has no data behind it).

### What sways

| Target | Technique | Cost / risk |
|---|---|---|
| **Billboard trees and bushes** | Per-sprite offset before `RotTransPers` in `Gfx_BillboardDraw`, scaled by the sprite's height and phased by tree position | Very low. One function, no physics library |
| **3D plant models** (bushes, hanging plants, grass tufts that are real meshes) | A shear in world space around the instance's ground height, applied to the instance matrix: `x' = x + k·(groundY − y)`. Only vertices above the ground move | Low. Matrix math only. Needs a plant name whitelist (same survey) |
| Physics props | Wind force on very light bodies (paper, cans) | Optional. Mostly unrealistic indoors |
| Snow | Already uses wind | — |

This half needs no physics library, so it can ship on its own first.

---

## Gating (the "stock unchanged" guarantee)

Follow the pattern in `pc_modern_mesh.c`:

- `config.cfg`: `physics_props = 0` (default), `physics_foliage_sway = 0` (default),
  `physics_wind_strength`, `physics_props_block_player = 0` (tier 2, later).
- When a flag is 0, the hook's first check returns and nothing is allocated,
  loaded or logged. The physics world is never created.
- Forced off during demo playback and FMV/DMS cutscenes where a prop could clip into a
  scripted character. The last point needs checking per cutscene. Soft props are
  low-risk because nothing is scripted around them.
- Quick Options toggle for both.

---

## Risks and open questions

- **Case B props** (merged meshes) can't be done without asset edits. That becomes the
  player-side TrenchBroom split, documented as optional.
- **Fixed-camera PVS:** exterior chunks cull model buffers by the player's subcell
  (`modelOrderList`). A prop shoved a long way might pop out of view in exteriors. Minor,
  and the PC interior path already skips that culling.
- **Enemies:** their collision cylinders (`field_D4.radius_0`, `field_D8` offsets) give us
  kinematic capsules for free. They won't avoid props.
- **Map DLLs:** the hooks are all in `bodyprog`, not in map overlays, so no per-DLL work.
- **Whitelisting pickups by mistake:** script-driven models must be excluded. The survey
  should flag any name that also appears in `WorldObject_*` calls.

---

## Next step: a data survey

This needs a local session with the game data. It adds no gameplay:

1. A debug console command (e.g. `physdump`) that, for every loaded chunk, logs each
   instance: chunk file, buffer/instance index, model name, local-vs-global LM, world
   AABB, triangle count, how many instances share its mesh, and any `ptr_18` cylinders or
   closed edge loops inside its footprint. Also count the billboards per chunk.
2. Walk through a few representative areas: school classrooms, hospital, Cafe 5to2,
   a street, and the Otherworld. Collect the logs.
3. From those logs: decide Case A/B per area, draft the whitelist, and confirm the
   cylinder theory for plants.

After that, the suggested build order is: (1) billboard sway, (2) 3D plant shear,
(3) Box3D vendored + soft props in one room, (4) collision handover + persistence,
(5) optional bullet impulses, (6) optional blocking tier.
