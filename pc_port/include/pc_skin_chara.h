/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_SKIN_CHARA_H
#define PC_SKIN_CHARA_H

/* Skinned glTF character replacements.
 *
 * A character whose model file has a sibling gamedata/load/<folder>/<NAME>.glb
 * (or .gltf) is drawn from that skinned mesh instead of its rigid ILM parts:
 * every vertex follows the game's own bone matrices through its glTF skin
 * weights, so joints bend smoothly and the model needs no cutting into parts.
 * The skin's joints are matched to the game's bones by name (Mixamo names,
 * with or without the "mixamorig:" prefix and Sketchfab "_NN" suffixes), and
 * the model's bind pose -- T-pose, A-pose, anything -- is retargeted onto the
 * character's rest pose at load. Textures stay full resolution, one per
 * material. */

#include "game.h"

typedef struct _Skeleton s_Skeleton;
typedef struct PcModernDrawBinding PcModernDrawBinding;

#ifdef __cplusplus
extern "C" {
#endif

/* Draw handles carry this bit so the modern-mesh renderer seam can route them. */
#define PC_SKIN_HANDLE_FLAG 0x80000000u

/* Returns the replacement for the character that owns `skel`, or NULL to draw
 * it the stock way. */
void* Pc_SkinChara_Begin(s_Skeleton* skel);
/* Every visible part of the skeleton, with the matrices the stock drawer would
 * have used. Also runs that bone's lighting. */
void  Pc_SkinChara_Bone(void* chara, int boneIdx, MATRIX* worldMat, MATRIX* viewMat);
void  Pc_SkinChara_End(void* chara, void* ot, int shift, int hideHead);

int   Pc_SkinChara_PrepareDraw(u32 handle, PcModernDrawBinding* binding);

#ifdef __cplusplus
}
#endif

#endif
