# Skinned Character Replacements (glTF)

Replace a character with any rigged, skinned model, such as a remake model or a
Mixamo-rigged character, by dropping one `.glb` file into the game data. The game drives
your model from its own skeleton every frame:

- joints bend smoothly through your skin weights
- every material keeps its full-resolution texture
- surfaces hide each other per pixel

There is no cutting into parts, no fitting in Blender and no converter step.

This replaces the older rigid-part workflow in [Model_Modding_Guide.md](Model_Modding_Guide.md)
for whole-character swaps. Use that guide only when you want to edit the original PSX model.

**Supported characters:** Harry (`HERO`) for now.

## Quick start

1. Get a rigged humanoid model (see [Getting a rig](#getting-a-rig)). Mixamo, Blender,
   XPS, Unreal, 3ds Max Biped and VRoid bone names all work.
2. In Blender, **File > Export > glTF 2.0**, format **glTF Binary (.glb)**, with the
   settings under [Exporting](#exporting).
3. Save it as `gamedata/load/CHARA/HERO.glb`.
4. Make sure `allow_loose_files = 1` is in `config.cfg`.
5. For a female character, also create `gamedata/load/CHARA/HERO.ini` containing
   `female=1` (see [Female characters](#female-characters)).
6. Start the game. Harry is now your model.

To go back to the original Harry, delete or rename `HERO.glb`. The replacement takes
priority over any `HERO.ILM` in the same folder, so you do not need to remove that.

## What the model needs

| Requirement | Details |
|---|---|
| Format | glTF 2.0 with **one skin**. Use `.glb`. A `.gltf` with its `.bin` and textures next to it also works. |
| Skeleton | Humanoid, with the bones named in a common convention (see [Bones the game uses](#bones-the-game-uses)). The `mixamorig:` prefix and Sketchfab `_01` style suffixes are fine. |
| Pose | Any bind pose: T-pose, A-pose or arms down. The game converts it. |
| Facing | glTF standard: Y up, facing +Z. This is what Blender exports by default. |
| Size | Any. The model is scaled so its **shoulders** sit at Harry's shoulder height, feet on the floor. |
| Weights | Up to 4 bones per vertex, as glTF allows. |
| Textures | **PNG**, base colour only. JPEG is not decoded. Embed them in the `.glb`. |
| Polygons | Up to about 87,000 triangles per material. |

### Bones the game uses

Your rig must have at least these joints. Fingers, toes, extra spine joints, twist
bones and anything else are fine: they follow the nearest bone listed here.

| Game bone | Mixamo joint(s) |
|---|---|
| Pelvis | `Hips` |
| Chest | `Spine`, `Spine1`, `Spine2` |
| Head | `Neck`, `Head` |
| Clavicles | `LeftShoulder`, `RightShoulder` |
| Upper arms | `LeftArm`, `RightArm` |
| Forearms | `LeftForeArm`, `RightForeArm` |
| Hands | `LeftHand`, `RightHand` (and all finger joints) |
| Thighs | `LeftUpLeg`, `RightUpLeg` |
| Shins | `LeftLeg`, `RightLeg` |
| Feet | `LeftFoot`, `RightFoot`, `LeftToeBase`, `RightToeBase` |

Left and right are the **character's own** left and right, as Mixamo names them.

**Other naming conventions** work without renaming. A bone is matched by its side
(`L`, `R`, `Left`, `Right` as a separate word, or a `Left`/`Right`/`l`/`r` prefix) and a
body-part word in its name:

| Game bone | Words matched |
|---|---|
| Pelvis | `pelvis`, `hips` |
| Chest | `spine`, `chest`, `torso`, `abdomen` |
| Head | `neck`, `head`, and face joints (`eye`, `jaw`, `face`) |
| Clavicles | `clavicle`, `collar`, `shoulder` |
| Upper arms | `upperarm`, `upper_arm`, `shldr`, or the first `arm` joint below the clavicle |
| Forearms | `forearm`, `lowerarm`, `elbow`, or an `arm` joint below another `arm` joint |
| Hands | `hand`, `wrist`, `palm`, and finger words (`finger`, `thumb`, `index`, ...) |
| Thighs | `thigh`, `upleg`, `upperleg`, or the first `leg` joint |
| Shins | `calf`, `shin`, `knee`, `lowerleg`, or the second `leg` joint |
| Feet | `foot`, `toe`, `ball`, `ankle`, or the third `leg` joint and below |

So `upper_arm.L` (Blender), `Arm.001.L` / `Arm.002.L` (XPS), `upperarm_l` (Unreal),
`Bip01 L UpperArm` (3ds Max) and `J_Bip_L_UpperArm` (VRoid) are all recognised. Joints
that match nothing, such as hair or skirt bones, follow their parent.

## Getting a rig

**Already rigged with Mixamo names** (many Sketchfab and game-rip models): use it as is.

**Not rigged:**
1. Export the model from Blender as FBX or OBJ.
2. Upload it to [Mixamo](https://www.mixamo.com), place the markers and let it auto-rig.
3. Download it as **FBX, T-pose, without skin animation**.
4. Import the FBX into Blender. Check that each piece moves with its bones by rotating a
   few bones in Pose mode, then undo.

**Rigged with other bone names:** usually works as is (see the second table above).
If the log says `no Hips joint` or `no joint drives game bone N`, rename the bones it
missed to the Mixamo names. In Blender, select the armature and rename the bones in the
Outliner. The vertex groups follow automatically.

## Exporting

**File > Export > glTF 2.0**:

- **Format:** glTF Binary (`.glb`)
- **Include:** Selected Objects. Select the armature and every mesh that belongs to the
  character, and nothing else: no lights, no cameras.
- **Data > Mesh:** Apply Modifiers **off**, UVs **on**, Normals **on**. Blender cannot
  export shape keys with Apply Modifiers on, and the Armature modifier does not need
  applying: the skin is exported from the armature either way. If you use another
  modifier, such as Mirror or Subdivision, apply it by hand before adding shape keys.
- **Data > Shape Keys:** **on** if you want blinking (see below)
- **Data > Armature / Skinning:** **on**
- **Animation:** **off**. The game animates the model.
- **Material > Images:** keep PNG. If your textures are DDS, TGA or JPEG, convert
  them to PNG in Blender first: Image Editor > Image > Save As > PNG.

Keep the armature unscaled if you can. Applying scale with **Ctrl+A > All Transforms**
on the armature and the meshes, before exporting, avoids surprises.

## How your model is fitted to Harry

The game never changes your mesh. It works out how to map your skeleton onto Harry's
when the model loads:

- **Arms and legs** bend at Harry's elbows, wrists, hips and knees, so the animation
  bends them in the right place and a held weapon lands in the hand. The upper and
  lower segments are stretched or squashed along their length to match Harry's.
- **Torso, head, clavicles and feet** keep your model's own shape and only turn about
  Harry's pivots, so the neck, waist and feet are not stretched.
- **Shoulders** keep your model's own width. Modern bodies are broader than Harry's,
  so the upper arm stays out at your shoulder and the forearm angles in to reach
  Harry's wrist.
- **Hips and knees** keep your model's own spacing side to side.

### Proportions

Harry's PSX skeleton is not modern anatomy. It has a short neck, a high waist pivot
and a stance with the feet apart. A model that looks right in Blender can look
slightly off in game:

- **Waist too wide or thick:** in Edit mode, select the waist and hip loops and scale
  them in X and Y. Leave the armature alone.
- **Legs look bowed:** Harry's own stance has the feet apart and the left leg slightly
  back, and your model copies it. Slimming the lower legs and turning the feet in
  slightly in Edit mode helps.
- **Arms sink into a bulky coat at the hips:** Harry's hands hang close to his
  thighs. Slim the coat's lower sides and pockets in Edit mode.
- **Arms too long or short:** do not resize the arm bones. The game already matches
  Harry's arm lengths. Reshape the sleeves instead.

Change the mesh, not the bones. Moving joints changes where your model bends, but
not where Harry's animation bends.

## Hair and other cutouts

Texels with alpha below 50% are not drawn, so hair cards, eyelashes and lace need
an **alpha channel in the PNG itself**. Some ripped models paint hair strands on
solid black and rely on a separate opacity map. Those show as black squares in
game until you add the alpha:

1. Open the hair texture in an image editor such as GIMP.
2. Add an alpha channel and make the black background transparent. In GIMP:
   **Colors > Color to Alpha**, colour black.
3. Save it as PNG, then load that PNG in the hair material in Blender. Connect its
   **Alpha** output to the Principled BSDF **Alpha** input, so the exporter keeps it.

The cut is hard-edged. Soft, blended alpha is not supported.

## Blinking

Add a shape key that closes both eyes and name it `blink`. `eyeBlink`, `eyesClosed`, or
a left and right pair such as `eyeBlink_L`/`eyeBlink_R` also work. The game blinks it
every 2 to 6 seconds, and the eyes stay shut when the ragdoll takes over. Export with
**Shape Keys** on.

Mouth keys (`jawOpen`, `mouthOpen`, `viseme_aa`) are recognised and loaded, but lip sync
does not drive them yet.

## Ragdoll on death

Add `ragdoll = 1` to `config.cfg`. When Harry dies, a skinned replacement goes limp
and falls, pushed along the last hit he took. The original model always plays its
normal death animation.

## Female characters

Harry's own cries, grunts and breathing are raised in pitch for female characters, the
same as when you play as Cybil or Lisa. The game cannot tell from the mesh, so put a
text file named after the model next to it, `gamedata/load/CHARA/HERO.ini`:

```
female=1
```

The amount is the `female_voice_pitch` setting in `config.cfg` (percent, default 140,
`0` turns it off). Cutscene dialogue is recorded audio and keeps Harry's voice.

## Troubleshooting

`SilentHill.log` has a `[SKIN]` line for everything the loader decides:

| Log line | Meaning |
|---|---|
| `[SKIN] gamedata/load/CHARA/HERO.glb: N verts, M materials, ...` | Loaded. The lines after it list each bone's fitted joint. |
| `no Hips joint` | No bone is named `Hips` or `Pelvis`. Rename the pelvis bone to `Hips`. |
| `no joint drives game bone N` | One of the required joints in the table is missing. |
| `buffers missing or no skin` | Exported without the armature, or the `.bin` is missing next to a `.gltf`. |
| `texture ... did not decode (PNG only)` | Convert that texture to PNG. The material draws white until you do. |
| No `[SKIN]` line at all | `allow_loose_files` is off, or the file is not named after the model (`HERO.glb`). |

## Limitations

- Harry only for now. Other characters need their skeleton tables added.
- The hand holding an item closes into a fist, with every finger bent the same way.
  This needs a finger chain of three joints under each hand: the index finger when it is
  named (`LeftHandIndex1` to `3`), otherwise a middle finger. It bends past that knuckle
  in the direction the finger already curls, so a dead straight finger gets no grip.
  The game does not pose individual fingers.
- Shape keys drive blinking. A rig that blinks with eyelid bones instead does not
  blink in game.
- The replacement does not cast flashlight shadows yet.
- The ragdoll collides with the floor, not walls.

## Licences

Respect the model's licence. Many Sketchfab models are CC-BY and need the author
credited wherever you share the mod.
