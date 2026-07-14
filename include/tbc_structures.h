// =============================================================================
// tbc_structures.h
// =============================================================================
// upstream port to WoW 2.4.3 — engine struct layouts.
//
// This file defines:
//   - Math types (unchanged from upstream's stCommon.h — vanilla→present-day stable)
//   - BoneNameLookup (unchanged from upstream)
//   - Camera struct offsets (PARTIAL change between WotLK and TBC)
//   - CGUnit_C / Player struct offsets — MAJOR layout change in TBC
//   - M2 bone format constants — likely change
//
// Status legend:
//   [VERIFIED]  — confirmed for TBC by ≥1 public source (BB/KX/EM)
//   [LIKELY]    — vanilla/TBC/WotLK all agree, OR derived from layout logic
//   [TODO]      — needs reverse engineering on 2.4.3 Wow.exe
//   [DROPPED]   — upstream's WotLK layout that DOES NOT exist in TBC
//
// IMPORTANT: TBC's CGUnit_C is RADICALLY different from WotLK's. upstream's
// WotLK code reads `gPlayerObj->ptrObjectData->objPos` (a pointer to an
// inline-or-heap stObjectData substruct). In TBC, no such substruct exists —
// position, rotation, movement flags are direct fields on the unit at
// +0xBF0..+0xC8C. The port will require rewriting every `gPlayerObj->ptrObjectData->X`
// reference to `*(T*)((BYTE*)gPlayerObj + TBC_OFFSET_X)`.
// =============================================================================
#pragma once

#undef UNICODE
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <unordered_set>
#include <vector>
#include <map>
#include <string>


// =============================================================================
// 1. MATH TYPES — pulled in from upstream's stCommon.h (no version dependency)
// =============================================================================
// Vector3 and uMatrix are defined in stCommon.h.  Don't redefine them here —
// game_extras.cpp transitively includes stCommon.h via structures.h, and a
// duplicate definition would be a hard compile error (multiple-definition).
//
// If this header is ever consumed standalone (without structures.h before it),
// uncomment the include below. The relative path assumes the standard layout:
//   upstream_tbc_port/include/tbc_structures.h
//   upstream_tbc_port/vrtbc243/stCommon.h
//
// #include "../vrtbc243/stCommon.h"


// =============================================================================
// 2. CAMERA STRUCT — partial change WotLK → TBC
// =============================================================================
// upstream reads/writes 16 distinct offsets on the camera struct.
// emenzed/WoW-VR-mod has TBC values for some of them.
//
// Strategy: use named constants below. The TBC CGCamera struct shrunk between
// WotLK and TBC — yaw/pitch/zoom moved by -0x18 (e.g. WotLK 0x118→TBC 0x100).
// Position vectors (X/Y/Z at +0x08/+0x0C/+0x10) and the rotation 3x3 matrix
// at +0x14..+0x34 are STABLE since vanilla.

namespace TBC_Camera
{
    // ---- 2a. VERIFIED stable since vanilla ----
    constexpr int posX            = 0x08;   // [VERIFIED]  3.3.5 same      [EM][KX]
    constexpr int posY            = 0x0C;   // [VERIFIED]  3.3.5 same      [EM][KX]
    constexpr int posZ            = 0x10;   // [VERIFIED]  3.3.5 same      [EM][KX]
    constexpr int forwardX        = 0x14;   // [LIKELY]    3.3.5 same — rotation matrix row 0
    constexpr int forwardY        = 0x18;   // [LIKELY]    3.3.5 same
    constexpr int forwardZ        = 0x1C;   // [LIKELY]    3.3.5 same
    constexpr int rightX          = 0x20;   // [LIKELY]    3.3.5 same — rotation row 1
    constexpr int rightY          = 0x24;   // [LIKELY]    3.3.5 same
    constexpr int rightZ          = 0x28;   // [LIKELY]    3.3.5 same
    constexpr int upX             = 0x2C;   // [LIKELY]    3.3.5 same — rotation row 2
    constexpr int upY             = 0x30;   // [LIKELY]    3.3.5 same
    constexpr int upZ             = 0x34;   // [LIKELY]    3.3.5 same

    // ---- 2b. VERIFIED for TBC, DIFFERENT from WotLK ----
    constexpr int FOV             = 0x40;   // [VERIFIED]  [EM]              3.3.5 was used as "maxYaw" (+0x40 write of 2*PI). In TBC this slot is FOV — verify if upstream's "+= maxRadRot" write is still safe here.
    constexpr int fixFlag         = 0xAC;   // [VERIFIED]  [EM]              "Camera turning fix flag"
    constexpr int zoomLevel       = 0x100;  // [VERIFIED]  3.3.5: 0x118     [EM]
    constexpr int yaw             = 0x104;  // [VERIFIED]  3.3.5: 0x11C     [EM]   <-- writes from setHorizontalRotation
    constexpr int pitch           = 0x108;  // [VERIFIED]  3.3.5: 0x120     [EM]   <-- writes from fnUpdateCameraController
    constexpr int roll            = 0x10C;  // [VERIFIED]  [EM]              3.3.5 had this as `+0x124` (commented out)
    constexpr int playerX         = 0x190;  // [VERIFIED]  [EM]              "Player X from camera" — used internally by engine
    constexpr int playerY         = 0x194;  // [VERIFIED]  [EM]
    constexpr int playerZ         = 0x198;  // [VERIFIED]  [EM]
    constexpr int playerYaw       = 0x1A0;  // [VERIFIED]  [EM]

    // ---- 2c. UNKNOWN — upstream uses but emenzed/BB/KX don't list ----
    constexpr int TODO_zoomMirror = 0;      // 3.3.5: 0x1E8  WotLK had a "target zoom" mirror written together with zoomLevel.
                                            // TBC may not have this — if zoom flickers in port, drop the second write at game_extras.cpp:2729/2734/2942/2947
    constexpr int TODO_maxYaw     = 0;      // 3.3.5: 0x40   upstream writes 2*PI to camera+0x40 to remove yaw cap.
                                            // In TBC +0x40 is FOV. So either:
                                            //   (a) the maxYaw write was always wrong in upstream (writing FOV), or
                                            //   (b) TBC's maxYaw is at a different offset.
                                            // RECOMMEND: skip this write entirely in port — yaw cap was probably already non-existent.
}


// =============================================================================
// 3. CGUnit_C / Player struct — MAJOR layout change vs WotLK
// =============================================================================
// In WotLK (upstream's stObjectManager), there was a stObjectData substruct
// pointer at +0xD8 and an inline copy at +0x788. In TBC, fields are direct
// on CGUnit_C with completely different offsets.
//
// Use these named constants in the port instead of the upstream substruct.

namespace TBC_CGUnit
{
    // ---- 3a. VERIFIED CGObject_C base (pre-vanilla layout, stable) ----
    constexpr int descriptor              = 0x008;   // [VERIFIED]  [BB]    descriptor block pointer
    constexpr int objGuidLo               = 0x030;   // [VERIFIED]  [EM]    matches upstream 3.3.5 stObjectManager.objGuID @+0x30
                                                     //                     (upstream labels it differently; it's 8-byte GUID lo at +0x30, hi at +0x34)
    constexpr int summonedByGuid          = 0x030;   // [VERIFIED]  [BB]    "summoned by" GUID — same offset as objGuidLo? VERIFY
    constexpr int targetGuid              = 0x040;   // [VERIFIED]  [BB]    target GUID

    // ---- 3b. VERIFIED CGUnit_C fields (TBC) ----
    constexpr int health                  = 0x058;   // [VERIFIED]  [BB]    (WotLK was 0x60)
    constexpr int mana                    = 0x05C;   // [VERIFIED]  [BB]
    constexpr int rage                    = 0x060;   // [VERIFIED]  [BB]
    constexpr int energy                  = 0x068;   // [VERIFIED]  [BB]
    constexpr int maxHealth               = 0x070;   // [VERIFIED]  [BB]    (WotLK was 0x80)
    constexpr int maxMana                 = 0x074;   // [VERIFIED]  [BB]
    constexpr int level                   = 0x088;   // [VERIFIED]  [BB]    (WotLK was 0xD8)
    constexpr int factionId               = 0x08C;   // [VERIFIED]  [BB]
    constexpr int unitFlags               = 0x0B8;   // [VERIFIED]  [BB]
    constexpr int buffsBase               = 0x0C0;   // [VERIFIED]  [BB]
    constexpr int debuffsBase             = 0xE20;   // [VERIFIED]  [BB]
    constexpr int dynamicFlags            = 0x290;   // [VERIFIED]  [BB]
    constexpr int currentChanneling       = 0x240;   // [VERIFIED]  [BB]
    constexpr int currentSpellcast        = 0xF3C;   // [VERIFIED]  [BB]

    // ---- 3c. VERIFIED Player position/rotation/movement (TBC) ----
    // These supersede upstream's stObjectData::objPos / objRot / objPitch / MovementStatus
    // and the entire speed table (which in upstream was on +0x0814..+0x0830).

    constexpr int locationX               = 0xBF0;   // [VERIFIED]  [KX]    (WotLK: gPlayerObj.objectData.objPos.x = +0x798)
    constexpr int locationY               = 0xBF4;   // [VERIFIED]  [KX]
    constexpr int locationZ               = 0xBF8;   // [VERIFIED]  [KX]
    constexpr int locationOrientation     = 0xBFC;   // [VERIFIED]  [KX]    yaw      (WotLK: +0x7A8)
    constexpr int locationVerticalOrient  = 0xC00;   // [VERIFIED]  [KX]    pitch    (WotLK: +0x7AC, named objPitch)
    constexpr int setFacingPlayerOffset   = 0xBE0;   // [VERIFIED]  [BB]    LocalPlayer_SetFacingOffset — used as `this` for SetFacing
    constexpr int movementFlagsHard       = 0xC20;   // [VERIFIED]  [BB][KX] (WotLK: stObjectData.MovementStatus = +0x07CC)
                                                     //                     The "0x100 = walking" bit upstream checks IS NOT IN THE PUBLIC TBC TABLE.
                                                     //                     TBC's known values: 128=normal, 16=ghost(walk on water), 1=flying, 64=floating, 80=ghost flying...
                                                     //                     RECOMMEND: stub IsPlayerRunning() to return false and revisit with disasm.
    constexpr int movementFlagsEasy       = 0xC23;   // [VERIFIED]  [KX]
    constexpr int movementStartX          = 0xC28;   // [VERIFIED]  [KX]
    constexpr int movementStartY          = 0xC2C;   // [VERIFIED]  [KX]
    constexpr int waterHeight             = 0xC30;   // [VERIFIED]  [KX]
    constexpr int orientationPoint        = 0xC34;   // [VERIFIED]  [KX]
    constexpr int verticalOrientPoint     = 0xC38;   // [VERIFIED]  [KX]
    constexpr int movementData            = 0xC3C;   // [VERIFIED]  [KX]    double — high-res timestamp
    constexpr int forwardMovementAngle    = 0xC40;   // [VERIFIED]  [KX]
    constexpr int turningMovementAngle    = 0xC48;   // [VERIFIED]  [KX]
    constexpr int turnWhileMovingPerm     = 0xC54;   // [VERIFIED]  [KX]
    constexpr int jumpState               = 0xC5C;   // [VERIFIED]  [KX]
    constexpr int jumpStartZ              = 0xC60;   // [VERIFIED]  [KX]

    // ---- 3d. VERIFIED speed table (TBC) ----
    // Replaces upstream's stObjectData speed fields at +0x0814..+0x0830.
    constexpr int curSpeed                = 0xC68;   // [VERIFIED]  [KX]    (WotLK was at relative +0x8C / abs +0x814)
    constexpr int walkSpeed               = 0xC6C;   // [VERIFIED]  [KX]    default 2.5
    constexpr int runSpeedForward         = 0xC70;   // [VERIFIED]  [KX]    default 7
    constexpr int runSpeedBackward        = 0xC74;   // [VERIFIED]  [KX]    default 4.5
    constexpr int swimSpeedForward        = 0xC78;   // [VERIFIED]  [KX]    default 4.722...
    constexpr int swimSpeedBackward       = 0xC7C;   // [VERIFIED]  [KX]
    constexpr int flySpeedForward         = 0xC80;   // [VERIFIED]  [KX]    default 7  (yes, TBC has fly speeds — used by druid flight form)
    constexpr int flySpeedBackward        = 0xC84;   // [VERIFIED]  [KX]
    constexpr int turningSpeed            = 0xC88;   // [VERIFIED]  [KX]    default 3.14 (PI)
    constexpr int jumpHeight              = 0xC8C;   // [VERIFIED]  [KX]    default -7.955...

    // ---- 3e. VERIFIED player-only fields (TBC) ----
    constexpr int scale                   = 0x09C;   // [VERIFIED]  [KX]    default 1.0
    constexpr int backpackFirstItem       = 0xAE0;   // [VERIFIED]  [BB]    (WotLK was 0x5C8)
    constexpr int equipmentFirstItem      = 0x3068;  // [VERIFIED]  [BB]
    constexpr int faction                 = 0x26CC;  // [VERIFIED]  [KX]
    constexpr int emoteState              = 0x28E4;  // [VERIFIED]  [KX]
    constexpr int hunterTracking          = 0x3AC8;  // [VERIFIED]  [KX]
    constexpr int castingSpell            = 0xF40;   // [VERIFIED]  [KX]    default 0

    // ---- 3f. UNKNOWN for upstream's specific WotLK-flavored uses ----
    constexpr int TODO_pModelContainer    = 0;       // 3.3.5: stObjectManager.pModelContainer at +0x00B4
                                                     // CRITICAL — used everywhere in upstream for bone access.
                                                     // TBC equivalent likely +0x60..+0xC0; find by xref to M2 model load fns.
    constexpr int TODO_alpha4             = 0;       // 3.3.5: stObjectManager.alpha4 at +0x00CB
                                                     // Forced to 255 in sub_ShouldRenderChar replacement.
                                                     // TBC may have 4 alpha bytes at different position.
    constexpr int TODO_ptrObjectData      = 0;       // 3.3.5: stObjectManager.ptrObjectData at +0x00D8
                                                     // **DROPPED in TBC** — there is no separate stObjectData struct;
                                                     // pos/rot/movement are direct fields above.
                                                     // PORT: replace `gPlayerObj->ptrObjectData->objPos` with
                                                     //       `*(Vector3*)((BYTE*)gPlayerObj + TBC_CGUnit::locationX)`.
    constexpr int TODO_mount              = 0;       // 3.3.5: 0x09C0 (unused in upstream — declared only)
    constexpr int TODO_modelID            = 0;       // 3.3.5: 0x1A64 (unused in upstream — declared only)
    constexpr int TODO_possessTarget      = 0;       // 3.3.5: stObjectManager.unknown11 +0x1008, then +0x770
                                                     // Used to detect possession (line 1788). Vehicle system didn't
                                                     // really exist in TBC; stub the entire branch to false.
}


// =============================================================================
// 4. M2 BONE FORMAT CONSTANTS
// =============================================================================
// upstream iterates over the M2 bone descriptor array at
//   `(stModelContainer->p20Container->ptr20)+0x30` (the boneOffset)
// reading int keyBoneId at relative +0x00 and short parentBoneId at +0x08.
// Stride is 0x58 in WotLK (M2 v272). TBC uses M2 v264 — likely 0x44 stride.
//
// Per-bone matrix array at `stModelContainer->ptrBonePos` is XMMATRIX (0x40).

namespace TBC_M2
{
    // ---- 4a. LIKELY (M2 file format constants) ----
    constexpr int boneMatrixStride        = 0x40;    // [LIKELY]    XMMATRIX = 16 floats. Hasn't changed since vanilla.
    constexpr int boneDescriptorStride    = 0x44;    // [TODO]      VERIFY — WotLK M2 v272 was 0x58. TBC M2 v264 is most likely 0x44 (no `submeshes`/extra IK fields).
                                                     //             If wrong, reading parent walk in BoneNameLookup::Set will read into adjacent bones and corrupt the model. CRITICAL TO VERIFY.
    constexpr int bone_keyBoneIdOffset    = 0x00;    // [LIKELY]    int keyBoneId
    constexpr int bone_parentBoneIdOffset = 0x06;    // [TODO]      WotLK was 0x08 — verify. CM2Bone::nameId moved.
                                                     //             PARENT IS A short16. Reading at wrong offset = wrong parent = wrong IK chain.

    // ---- 4b. VERIFIED M2 keybone count differences ----
    // (Source: wowdev.wiki M2 page)
    constexpr int keyBoneCount_TBC        = 27;      // M2 v264: 27 keybones
    constexpr int keyBoneCount_WotLK      = 35;      // M2 v272: 35 keybones (added Hand* / finger / Wheel*)
    // upstream's BoneNameLookup has 39 entries but indexes them by keyBoneId — entries
    // with keyBoneId >= 27 will simply not populate in TBC, which is harmless.
    // The Hand/Elbow IK chain is derived dynamically from Thumb's parent walk, so
    // it works regardless of keybone count, AS LONG AS finger keybones exist
    // (they do — fingers were in vanilla).
}


// =============================================================================
// 5. RENDERER STATE STRUCT — anchored at TBC_g_GxDevicePtr (=0x00C71C24)
// =============================================================================
// upstream reads many offsets off this pointer for: viewport block, projection
// matrix slot, HWND, IDirect3DDevice9*, back/depth buffer slots.
//
// TBC's CGxDeviceD3d struct is heavily reorganized vs WotLK. Every offset
// in this section needs disasm verification. NONE of them are in public
// dumps for TBC.

namespace TBC_GxDevice
{
    // ---- 5a. UNKNOWN — disasm required ----
    constexpr int TODO_viewport_x1        = 0;       // 3.3.5: +0x164  float, write — viewport rect first pair
    constexpr int TODO_viewport_y1        = 0;       // 3.3.5: +0x168
    constexpr int TODO_viewport_h         = 0;       // 3.3.5: +0x16C  also written from msub_6A08D0_post on the window object (different struct)
    constexpr int TODO_viewport_w         = 0;       // 3.3.5: +0x170
    constexpr int TODO_viewport_x2        = 0;       // 3.3.5: +0x174  second pair
    constexpr int TODO_viewport_y2        = 0;       // 3.3.5: +0x178
    constexpr int TODO_viewport_h2        = 0;       // 3.3.5: +0x17C
    constexpr int TODO_viewport_w2        = 0;       // 3.3.5: +0x180
    constexpr int TODO_window_hwnd        = 0;       // 3.3.5: +0x3968  HWND
    constexpr int TODO_d3d_device         = 0;       // 3.3.5: +0x397C  IDirect3DDevice9*
    constexpr int TODO_origBackBuffer     = 0;       // 3.3.5: +0x3B3C  IDirect3DSurface9*
    constexpr int TODO_origDepthBuffer    = 0;       // 3.3.5: +0x3B40  IDirect3DSurface9*
    constexpr int TODO_proj_matrix_block  = 0;       // 3.3.5: +0xFC8   16 floats — the in-game projection matrix that upstream memcpy's the VR matrix into
    constexpr int TODO_proj_z_translation = 0;       // 3.3.5: +0xFC8 + 0x38   _43 of projection (preserved into VR matrix)
    constexpr int TODO_proj_w_predicate   = 0;       // 3.3.5: +0xFC8 + 0x3C   _44 used as gate ("if 0")
    constexpr int TODO_unknown_44         = 0;       // 3.3.5: +0x44    passed as first arg to sub_4BEE60 (FOV/clip data block)
    constexpr int TODO_unknown_7C         = 0;       // 3.3.5: +0x7C    "tBool" flag gating world-frame loop
    constexpr int TODO_frame_array_base   = 0;       // 3.3.5: +0x0CE4  base of world-frame pointer array iterated by sub_StartRender

    // Anchor strategy: in IDA, set EA = TBC_g_GxDevicePtr (0x00C71C24), follow
    // its dereference, find xrefs to D3D9 calls (CreateDevice, Reset, Present)
    // — those will reveal the device-pointer offset and the back-buffer slots.
    // The +0xFC8 projection matrix block can be found by finding xrefs to
    // d3dx9.dll function imports near matrix-multiply intrinsics.
}


// =============================================================================
// 6. WORLD FRAME / WORLD PANEL — partial knowledge
// =============================================================================
// upstream also reads offsets off [TBC_g_WorldFrame] (=0x00C6ECCC per emenzed)
// and [TBC_g_WorldPanel] (3.3.5 was 0x00B499A8, TBC unknown).

namespace TBC_WorldFrame
{
    constexpr int cameraOffset            = 0x732C;  // [VERIFIED]  [EM]   from worldFrame, dereferences to camera root.
                                                     //                    NOTE: this is an alternative anchor for camera — upstream uses
                                                     //                    CGWorldFrame__GetActiveCamera() (0x004AB5B0) instead.
    constexpr int TODO_mouseoverGuidLo    = 0;       // 3.3.5: +0x2C8
    constexpr int TODO_mouseoverGuidHi    = 0;       // 3.3.5: +0x2CC
}

namespace TBC_WorldPanel
{
    constexpr int TODO_mouseoverElement   = 0;       // 3.3.5: +0x78    pointer to UI element under cursor
    constexpr int TODO_mouseCoordPctX     = 0;       // 3.3.5: +0x1224
    constexpr int TODO_mouseCoordPctY     = 0;       // 3.3.5: +0x1228
}


// =============================================================================
// 7. BoneNameLookup — copy verbatim from upstream's structures.h
// =============================================================================
// This class is engine-agnostic. The bone names match WoTLK; in TBC the
// extra keybones (HandL/R, ElbowL/R, _NameMount, Wheel*) simply won't bind,
// which is harmless. The IK chain for hands is derived dynamically from
// the thumb's parent — works in TBC as long as finger keybones exist.
//
// Single change to verify: BoneNameLookup::Set() reads at stride 0x58. Change
// to TBC_M2::boneDescriptorStride (0x44 likely) once disasm confirms.
//
// (Code identical to upstream/upstream/vrtbc243/structures.h:10-192; not duplicated
//  here — just include the original file. Listed here for completeness of
//  the "what changed" map.)


// =============================================================================
// 8. PORT MIGRATION TABLE — the field-by-field translation guide
// =============================================================================
// When porting game_extras.cpp expressions, use this table:
//
// WotLK (3.3.5)                                    | TBC (2.4.3)
// --------------------------------------------------|----------------------------------------------------------
// gPlayerObj->ptrObjectData->objPos                 | *(Vector3*)((BYTE*)gPlayerObj + TBC_CGUnit::locationX)
// gPlayerObj->ptrObjectData->objRot                 | *(float*)  ((BYTE*)gPlayerObj + TBC_CGUnit::locationOrientation)
// gPlayerObj->ptrObjectData->objPitch (write)       | *(float*)  ((BYTE*)gPlayerObj + TBC_CGUnit::locationVerticalOrient) = X
// gPlayerObj->ptrObjectData->MovementStatus & 0x100 | TODO — disasm required; stub `IsPlayerRunning() return false;` until verified
// gPlayerObj->ptrObjectData->runSpeedForward etc.   | *(float*)((BYTE*)gPlayerObj + TBC_CGUnit::runSpeedForward) etc.
// gPlayerObj->pModelContainer                       | *(stModelContainer**)((BYTE*)gPlayerObj + TBC_CGUnit::TODO_pModelContainer)  -- TODO
// gPlayerObj->alpha4 = 255                          | *((BYTE*)gPlayerObj + TBC_CGUnit::TODO_alpha4) = 255  -- TODO
// CGMovementInfo__SetFacing((int)gPlayerObj->ptrObjectData, rad)  | SetFacing((BYTE*)gPlayerObj + TBC_CGUnit::setFacingPlayerOffset, rad)
//                                                                   ^^ WotLK passed ptrObjectData; TBC passes player+0xBE0 [BB]
// camera+0x118 (zoom)                               | camera + TBC_Camera::zoomLevel  (= 0x100)
// camera+0x11C (yaw)                                | camera + TBC_Camera::yaw        (= 0x104)
// camera+0x120 (pitch)                              | camera + TBC_Camera::pitch      (= 0x108)
// camera+0x40   (maxYaw cap write)                  | DROP — in TBC +0x40 is FOV; the cap-removal write is destructive
// camera+0x1E8  (zoom mirror)                       | DROP — likely doesn't exist in TBC; if zoom flickers, revisit
// *(int*)0x00B7436C  (worldFrame)                   | *(int*)TBC_g_WorldFrame
// *(int*)0x0C5DF88   (gxDevice)                     | *(int*)TBC_g_GxDevicePtr
//
// =============================================================================
// END OF FILE
// =============================================================================
