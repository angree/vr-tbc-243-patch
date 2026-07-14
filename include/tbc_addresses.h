// =============================================================================
// tbc_addresses.h
// =============================================================================
// upstream port to WoW 2.4.3 (TBC, Wow.exe build 8606, retail Oct 2011, 8,272,528 B)
//
// All values are absolute virtual addresses in the 0x00400000 image base.
// Both 3.3.5a and 2.4.3 retail Wow.exe use the same base — addresses are
// directly comparable RVAs. No ASLR.
//
// Status legend (in trailing comment):
//   [BB]   = DrewKestell/BloogBot (github.com/DrewKestell/BloogBot)
//   [KX]   = kynox WoW Offset Dumper output, preserved in sndcode/Arkstone
//   [EM]   = emenzed/WoW-VR-mod (Emenzed = the same person credited in
//            upstream README under "Help with finding memory addresses")
//   [???]  = unknown for 2.4.3, requires reverse engineering on Wow.exe 8606
//   [SKIP] = address whose only purpose was cosmetic / WotLK-only feature;
//            do not bother re-resolving for the port
//
// Cross-source conflicts are flagged inline.
// =============================================================================
#pragma once

// -----------------------------------------------------------------------------
// Null-tolerant memory access macros
// -----------------------------------------------------------------------------
// All TBC_TODO_* constants default to 0. Reading *(T*)0 = NULL deref = crash.
// Use these macros when the address may still be unresolved (TODO).
//
//   SAFE_READ_INT(addr, default)    — returns *(int*)addr  if addr != 0, else default
//   SAFE_READ_FLOAT(addr, default)  — returns *(float*)addr  if addr != 0, else default
//   SAFE_READ_DWORD(addr, default)  — returns *(DWORD*)addr  if addr != 0, else default
//   SAFE_WRITE_INT(addr, val)       — writes if addr != 0, no-op otherwise
//   SAFE_WRITE_FLOAT(addr, val)
//   SAFE_WRITE_DWORD(addr, val)
//
// Once a TODO_g_X global is resolved (TBC_TODO_g_X -> non-zero), the macro
// becomes a plain dereference at runtime — no perf cost beyond a branch.

#define SAFE_READ_INT(addr, def)     ((addr) != 0 ? *(int*)(addr)   : (def))
#define SAFE_READ_FLOAT(addr, def)   ((addr) != 0 ? *(float*)(addr) : (def))
#define SAFE_READ_DWORD(addr, def)   ((addr) != 0 ? *(DWORD*)(addr) : (def))
#define SAFE_WRITE_INT(addr, val)    do { if ((addr) != 0) *(int*)(addr)   = (val); } while (0)
#define SAFE_WRITE_FLOAT(addr, val)  do { if ((addr) != 0) *(float*)(addr) = (val); } while (0)
#define SAFE_WRITE_DWORD(addr, val)  do { if ((addr) != 0) *(DWORD*)(addr) = (val); } while (0)


// -----------------------------------------------------------------------------
// 0. Image base
// -----------------------------------------------------------------------------
constexpr unsigned int WOW_IMAGE_BASE_TBC   = 0x00400000;  // [verified — both clients]
constexpr unsigned int WOW_IMAGE_BASE_WOTLK = 0x00400000;


// =============================================================================
// 1. ENGINE API — function pointers upstream calls directly
// =============================================================================
// All addresses listed as raw uint; the user can wrap them with the typedefs
// they want in the port code (game_extras_tbc.cpp).
//
// Calling convention is preserved from upstream's 3.3.5 declaration. Don't change.
//
// Cross-reference column: file:line in original game_extras.cpp where the FP
// was declared (so you can match the typedef when porting).
//
// Format:
//   constexpr unsigned int TBC_<symbol> = 0x........;  // 3.3.5: 0x........  __<conv>  [src]  desc
// -----------------------------------------------------------------------------

// ---- 1a. VERIFIED for TBC (have source) -------------------------------------

constexpr unsigned int TBC_CGWorldFrame_GetActiveCamera   = 0x004AB5B0;  // 3.3.5: 0x004F5960  __cdecl     [KX][BB]  returns CGCamera*; the most-called engine fn in upstream
constexpr unsigned int TBC_ClntObjMgrGetActivePlayerObj   = 0x00402F40;  // 3.3.5: 0x004038F0  __cdecl     [KX]      returns local player as stObjectManager*
constexpr unsigned int TBC_ClntObjMgrObjectPtr            = 0x0046B610;  // 3.3.5: 0x004D4DB0  __cdecl     [KX]      (uint guidLo,uint guidHi,uint typeMask,const char*,uint) — typecheck variant matches upstream's 5-arg sig
                                                                         //                                          NOTE: BloogBot says 0x0046b520 for "GetObjectPtrFunPtr"; that is the no-typecheck variant. Use 0x0046B610.
constexpr unsigned int TBC_CGMovementInfo_SetFacing       = 0x007B9DE0;  // 3.3.5: 0x00989B70  __thiscall  [BB]      (this, float radians) — server-aware facing setter
constexpr unsigned int TBC_CGInputControl_SetControlBit_DISPATCHER = 0x005343A0;  // 3.3.5: 0x005FA170  __thiscall  [BB][KX]
                                                                         //                                          ⚠️ NOTE: TBC unifies Set/Unset into one DISPATCHER at this address.
                                                                         //                                          Signature: (this, uint bit, int isSet, eventTick, ?). Inside it calls:
                                                                         //                                          - 0x00533370 (Set helper, when isSet != 0)
                                                                         //                                          - 0x00533620 (Unset helper, when isSet == 0)
                                                                         //                                          - 0x00534160 (the actual SendMovementUpdate / UpdatePlayer, sim=0.76 confirmed)
                                                                         //                                          PORT: refactor upstream's SetControlBit/UnsetControlBit/UpdatePlayer trio
                                                                         //                                          to a single dispatcher call with isSet flag.
constexpr unsigned int TBC_SendMovementUpdate             = 0x0060D200;  // 3.3.5: ?           __thiscall  [BB]      upstream's CGInputControl::UpdatePlayer is most likely this — verify
constexpr unsigned int TBC_CastSpell                      = 0x006FC520;  // 3.3.5: 0x0080DA40  __cdecl     [BB]      Spell_C::CastSpell(spellId,...) — used for mount casting in upstream
constexpr unsigned int TBC_TODO_RayIntersect              = 0;           // 3.3.5: 0x004F6450  __thiscall  [???]
                                                                         //                                          ⚠️ CORRECTED: BloogBot's "IntersectFunPtr=0x006A37B0" is the 3D-world-ray
                                                                         //                                          collision API (= upstream's WorldClickIntersect), NOT this screen-to-world ray.
                                                                         //                                          upstream's RayIntersect takes (worldFrame, screenX, screenY, outNear, outFar)
                                                                         //                                          and is unique to upstream's mouse-pick semantics. Manual disasm required.
constexpr unsigned int TBC_WorldClickIntersect            = 0x006A37B0;  // 3.3.5: 0x004F9930  __thiscall  [BB]      3D world ray collision (BloogBot's "Intersect")
                                                                         //                                          Verified by Ghidra decompilation - takes (origin, dir, outHit, outHit2, flags, outInfo).
constexpr unsigned int TBC_sub_TraceMouseRay              = 0x004AEB10;  // __thiscall(this, C3Vector* origin, C3Vector* end, unsigned flags, HitRec* out), ret 0x10.
                                                                         //                                          Disasm-verified 2.4.3 build 8606: ecx=this, 4 stack args, ret 0x10.
                                                                         //                                          Pure ray query -> 0=nothing, 1=terrain/world, 2=object. Flags bits: 0x08 units,
                                                                         //                                          0x10 players, 0x40 corpses, 0x01/0x02 terrain/world. Used by VR aim-assist multi-ray gaze.
constexpr unsigned int TBC_FrameScript_Execute            = 0x00706C80;  // 3.3.5: ?           __cdecl     [BB][KX]  Lua_Dostring (FrameScript_Execute). Use this to replace ALL the per-function lua_* calls below.
constexpr unsigned int TBC_lua_Dismount                   = 0x00622490;  // 3.3.5: 0x0051D170  __cdecl     [BB]      direct binding for Dismount() — alternative is FrameScript_Execute("Dismount()")
constexpr unsigned int TBC_GetPlayerGuid                  = 0x00469DD0;  // 3.3.5: ?           __cdecl     [BB][KX]  ClntObjMgrGetActivePlayer — returns GUID, not object*. upstream doesn't use this directly but it's a useful anchor.

// ---- 1b. VERIFIED but currently UNUSED by upstream (kept for completeness) ------
// These were in upstream's commented-out declarations; if you re-enable any feature,
// the TBC equivalents are already known.

constexpr unsigned int TBC_lua_ToggleRun                  = 0;           // 3.3.5: 0x005FAAE0  [???]     not in public dumps
constexpr unsigned int TBC_lua_IsMounted                  = 0;           // 3.3.5: 0x006125A0  [???]     not in public dumps
constexpr unsigned int TBC_CGCamera_ZoomIn                = 0;           // 3.3.5: 0x005FF950  [???]     not used by upstream after move to direct +0x118 writes
constexpr unsigned int TBC_CGCamera_ZoomOut               = 0;           // 3.3.5: 0x005FFA60  [???]
constexpr unsigned int TBC_CameraUpdateX                  = 0;           // 3.3.5: 0x005FE5F0  [???]
constexpr unsigned int TBC_CameraUpdateY                  = 0;           // 3.3.5: 0x005FFC20  [???]
constexpr unsigned int TBC_IsFallingSwimmingFlying        = 0;           // 3.3.5: 0x006EABA0  [???]     not used (commented)


// ---- 1c. UNKNOWN for 2.4.3 — needs RE ---------------------------------------

constexpr unsigned int TBC_CalculateForwardMovement = 0x007BA120;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x0098B0E0  __thiscall  CGUnit_C::CalculateForwardMovement — used at game_extras.cpp:3186 to push movement vector
constexpr unsigned int TBC_CGInputControl_UnsetControlBit = 0x00533620;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x005FA450  __thiscall  In TBC, this MERGES with SetControlBit via dispatcher (see _DISPATCHER above with isSet=0).
constexpr unsigned int TBC_CGInputControl_UpdatePlayer         = 0x00534160;  // 3.3.5: 0x005FBBC0  __thiscall  [bindiff sim=0.76] Pumps queued bits to server. ⚠️ Verify in Ghidra.
//                                                                         //                                          Note: this is also called internally by the SetControlBit dispatcher.
//                                                                         //                                          May be redundant if you use the dispatcher API directly.
// (TBC_WorldClickIntersect now defined in 1a above — 0x006A37B0)
constexpr unsigned int TBC_EnsureProperRadians = 0x00460B30;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x004C5090  __cdecl     wraps angle to [-PI, PI] — INLINE-ABLE if you can't find it (`std::fmodf` + std::abs)
constexpr unsigned int TBC_lua_TargetNearestEnemy              = 0x004A7010;  // 3.3.5: 0x00525AD0  __cdecl  [bindiff sim=0.90] confident match.


// =============================================================================
// 2. DETOUR TARGETS — engine-internal sub_XXXXXX functions upstream hooks
// =============================================================================
// All 24 detoured subs from InitDetours (game_extras.cpp:2363-2393).
// The ENGINE side of the VR mod. Each address is the function upstream replaces.
//
// ALL ARE TODO for 2.4.3 — none of these subs have published TBC equivalents.
// Strategy: BinDiff/Diaphora 3.3.5a vs 2.4.3 in IDA; the comments below
// describe what each sub does so you can verify the match.
//
// "skip?" column: "yes" = the replacement does nothing or supresses the
// original entirely (you can simply not install the detour and behavior is
// identical). "no" = the replacement is load-bearing for VR.
// -----------------------------------------------------------------------------

// ---- 2a. CRITICAL detours (must port — these are what makes VR work) -------

constexpr unsigned int TBC_sub_StartRender                    = 0x0043CCE0;  // 3.3.5: 0x00495410  __thiscall(void*)  [bindiff norm_sim=0.99, anchored unique]
                                                                              //                                          The per-frame stereo loop. CRITICAL hook.
constexpr unsigned int TBC_sub_OnPaint                        = 0x00447560;  // 3.3.5: 0x004A8720  __thiscall()  [bindiff sim=0.65] medium confidence — VERIFY in Ghidra GUI.
constexpr unsigned int TBC_sub_UpdateModelProj = 0x005AD8E0;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x006A9B40  __thiscall(void*, int)         After original, overwrites engine's projection matrix at *([GxDevicePtr])+0xFC8 with per-eye HMD projection. LOAD-BEARING.
constexpr unsigned int TBC_sub_UpdateCameraFn = 0x0053E800;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x00606F90  __thiscall(void*, int, int)    After original, overwrites camera matrix with HMD pose + IPD. THE CENTRAL VR INJECTION. LOAD-BEARING.
// 2026-07-10 fix #22: SIGNATURE DIFFERS FROM 3.3.5! Disasm-verified: TBC version
// is __thiscall(this, int) with `ret 4` (ONE stack arg) and RETURNS bool in EAX
// (3.3.5 took 3 args, void, wrote result through arg3 pointer). Address is right,
// but the hook must be rewritten for this signature before re-enabling — calling
// it WotLK-style shifted ESP by 8 -> wild-jump crash EIP=0x448D3A18 / 0%-CPU hang.
// TBC globals inside: [this+0xF18] flags, [this+0x25D0], DAT_00D66B3C (3.3.5: 0xC9D540).
constexpr unsigned int TBC_sub_ShouldRenderChar = 0x005E8BF0;  // [disasm-verified addr, WRONG WotLK signature — hook disabled, see fix #22]  // 3.3.5: 0x006E0840
constexpr unsigned int TBC_sub_DynamicModelAnimations = 0x00718380;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x0082F0F0  __thiscall(void*, int, int, int, int, int) After original, scales head bone + descendants to zero in 1st person. Depends on bone descriptor stride (see tbc_structures.h)
constexpr unsigned int TBC_sub_StartUI                        = 0x0043C7F0;  // 3.3.5: 0x00494F30  __thiscall(int)  [bindiff sim=0.99] virtually identical decompilation
constexpr unsigned int TBC_sub_StartRender2                   = 0x0043C7A0;  // 3.3.5: 0x00494EE0  __thiscall(int, int)  [bindiff sim=0.97]
constexpr unsigned int TBC_sub_RenderViewport = 0x0045C2D0;  // [disasm/decomp-verified 2026-07-10; old bindiff 0x435EE0 was WRONG]  // 3.3.5: 0x004BEE60  TBC signature: __cdecl(float*, float*, int) — the 4th int arg was REMOVED in TBC

// ---- 2b. DEVICE/WINDOW lifecycle detours (must port — VR init/teardown) ----

constexpr unsigned int TBC_sub_CreateWindow = 0x005A4180;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x0068EBB0  __thiscall(void*, int)         Original CreateWindowEx wrapper. upstream uses pre/post hooks for DPI awareness + buffer sizing.
constexpr unsigned int TBC_TODO_sub_CreateWindowEx            = 0;  // 3.3.5: 0x006A08D0  __thiscall(void*, int)         Newer variant.  Anchor: s_GxDevWindow = 0x00E1F894 [KX]
constexpr unsigned int TBC_sub_CalculateWindowSize = 0x00599B70;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x00684D70  __cdecl(int, int, int)         Forces top-left positioning of the WoW window
constexpr unsigned int TBC_sub_CreateDxDevice = 0x005A56C0;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x006904D0  __thiscall(void*, int)         IDirect3DDevice9::CreateDevice wrapper. Anchor: GxDevicePtr = 0x00C71C24 [KX]
constexpr unsigned int TBC_TODO_sub_CreateDxDeviceEx          = 0;  // 3.3.5: 0x006A2040  __thiscall(void*, int)         Ex variant. Hosts CreateBuffers / SteamVR Start / addon copy / NOP patches.
// 2026-07-10 fix #24: bindiff match 0x00845F41 was WRONG (unaligned mid-function;
// the detour fired spuriously ~1.4s after start and killed the VR session — fix #14).
// Real address found by disasm (capstone on pure 2.4.3 exe): sits directly before
// CreateDxDevice like in 3.3.5, __thiscall(this), plain ret; body does virtual
// call [vt+0x10], releases COM members at +0x3A14/+0x3864 (call [vt+8] = Release)
// and nulls them — classic device teardown.
constexpr unsigned int TBC_sub_CloseDxDevice = 0x005A5640;  // [disasm-verified 2026-07-10]  // 3.3.5: 0x006903B0  __thiscall(void*)  Tears down VR stack
constexpr unsigned int TBC_TODO_sub_CloseDxDeviceEx           = 0;  // 3.3.5: 0x006A1F40  __thiscall(void*)              Ex variant

// ---- 2c. SCENE detours (mostly pass-through wrappers, but installed) -------

constexpr unsigned int TBC_sub_BeginSceneSetup = 0x005AB180;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x006A73E0  __thiscall(void*)              Pure pass-through (currently — could host enhancements)
constexpr unsigned int TBC_sub_EndSceneSetup = 0x005AB230;  // [bindiff:Implied Match, conf=verify]  // 3.3.5: 0x006A7540  __thiscall(void*)              Pure pass-through
constexpr unsigned int TBC_sub_PresentScene = 0x005AB260;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x006A7610  __thiscall(void*)              Pure pass-through
constexpr unsigned int TBC_sub_RenderMouse = 0x0059C190;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x00687A90  __thiscall(void*)              Pure pass-through
constexpr unsigned int TBC_sub_WorldToScreen = 0x004AC810;  // [disasm-verified 2.4.3]  __thiscall(WF, float* worldPos, float* outXY, int flag) -> bool. Projects a world point to screen coords for unit names/nameplates/floating combat text. Reads the cached view*proj at WF+0x3F8 (NOT +0xF0C live). fix #74 option B detours this to swap WF+0x3F8 to view*perEyeProj for the call only.
constexpr unsigned int TBC_sub_SetClientMouseResetPoint       = 0x00748890;  // 3.3.5: 0x00869DB0  __cdecl()  [bindiff sim=0.69] confident match
constexpr unsigned int TBC_sub_SlowsAnimation                 = 0x00681580;  // 3.3.5: 0x0077EFF0  __cdecl(int, float)  [bindiff sim=0.93] confident match
constexpr unsigned int TBC_sub_UpdateFreelookCamera = 0x005375E0;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x005FF530  __thiscall(void*)              Calls fnUpdateCameraController (writes camera +0x120 += vRotationOffset) before forwarding.
constexpr unsigned int TBC_sub_MouseToWorldRay = 0x0045C530;  // [bindiff:BSIM, conf=high]  // 3.3.5: 0x004BF0F0  __cdecl(float, float, Vector3*, Vector3*)   Wholesale-replaced (no original call). Substitutes controller-anchored ray for cursor ray.

// ---- 2d. SUPPRESSED detours — replacement does nothing -------------------

constexpr unsigned int TBC_SKIP_sub_SkyboxFix                 = 0;  // 3.3.5: 0x006A38D0  [SKIP]  VERIFIED ABSENT in 2.4.3 (2026-07-10 disasm) - deferred sky scissor (3.3.5 0x6A38D0) was added post-TBC; ghidriff match 0x5AB680 is actually the deferred SetIndices-apply - hooking/disabling it would break ALL indexed drawing. Permanent skip.
constexpr unsigned int TBC_SKIP_sub_GreyBoxes                 = 0;  // 3.3.5: 0x00796C10  [SKIP]  Replacement is empty-body. Skip the detour entirely.


// =============================================================================
// 3. MOVEMENT KEYBIND FAMILY — small stub fns called by controllers
// =============================================================================
// These are bind-handlers (one per binding name in WoW's keybind dispatcher).
// upstream calls them DIRECTLY rather than going through FrameScript.
//
// In upstream 3.3.5 they cluster around 0x005FBxxx-0x005FCxxx (one address per ~0x40
// bytes — they are tiny stub functions). On TBC the cluster will be at a
// different RVA but adjacent addresses. Strategy:
//   (a) find ONE of them (e.g. jumpOrAscendStart) by xref from the keybind
//       dispatch table at 0x0072DAE0+ [KX], OR by FrameScript_Execute calls
//       containing string "JUMP";
//   (b) the rest will be at +0x40, +0x80, ... offsets from there.
//
// Alternative for ALL of these: just call FrameScript_Execute("Jump()") /
// MoveForwardStart()) etc. via TBC_FrameScript_Execute. Slower (one extra
// engine layer) but trivial to port.
//
// "live?" column: yes = upstream actually calls this. no = declared but unused.
// -----------------------------------------------------------------------------

constexpr unsigned int TBC_TODO_jumpOrAscendStart   = 0;  // 3.3.5: 0x005FBF80  live  game_extras.cpp:2981
constexpr unsigned int TBC_TODO_jumpOrAscendStop    = 0;  // 3.3.5: 0x005FC0A0  live  game_extras.cpp:2983
constexpr unsigned int TBC_TODO_moveForwardStart    = 0;  // 3.3.5: 0x005FC200  live  game_extras.cpp:2656
constexpr unsigned int TBC_TODO_moveForwardStop     = 0;  // 3.3.5: 0x005FC250  live  game_extras.cpp:2663
constexpr unsigned int TBC_SKIP_moveBackwardStart   = 0;  // 3.3.5: 0x005FC290  no    declared, never invoked — drop
constexpr unsigned int TBC_SKIP_moveBackwardStop    = 0;  // 3.3.5: 0x005FC2E0  no
constexpr unsigned int TBC_SKIP_turnLeftStart       = 0;  // 3.3.5: 0x005FC320  no
constexpr unsigned int TBC_SKIP_turnLeftStop        = 0;  // 3.3.5: 0x005FC360  no
constexpr unsigned int TBC_SKIP_turnRightStart      = 0;  // 3.3.5: 0x005FC3B0  no
constexpr unsigned int TBC_SKIP_turnRightStop       = 0;  // 3.3.5: 0x005FC3F0  no
constexpr unsigned int TBC_SKIP_moveLeftStart       = 0;  // 3.3.5: 0x005FC440  no
constexpr unsigned int TBC_SKIP_moveLeftStop        = 0;  // 3.3.5: 0x005FC490  no
constexpr unsigned int TBC_SKIP_moveRightStart      = 0;  // 3.3.5: 0x005FC4D0  no
constexpr unsigned int TBC_SKIP_moveRightStop       = 0;  // 3.3.5: 0x005FC520  no
constexpr unsigned int TBC_SKIP_sitOrDescendStart   = 0;  // 3.3.5: 0x0051B1D0  no    out-of-range from neighbours — preserved from original
constexpr unsigned int TBC_SKIP_sitOrDescendStop    = 0;  // 3.3.5: 0x005FC140  no


// =============================================================================
// 4. RAW MEMORY GLOBALS
// =============================================================================
// Engine-side global variables upstream reads/writes directly via *(T*)0x........
//
// CONFLICT NOTE: emenzed and BloogBot disagree on what "worldFrame" means.
//   - emenzed wowmw.py:        worldFrame = 0x00C6ECCC      "world frame pointer"
//   - upstream 3.3.5:              0x00B7436C is a CWorldFrame*  (see comment table 3 in extraction)
//   - BloogBot has no direct equivalent.
// Until verified by disasm, treat 0x00C6ECCC as the candidate for TBC_g_WorldFrame
// and check for null-deref behavior on first run.
//
// Format: 3.3.5 addr → TBC addr   meaning
// -----------------------------------------------------------------------------

// ---- 4a. VERIFIED for TBC ---------------------------------------------------

constexpr unsigned int TBC_g_WorldFrame                = 0x00C6ECCC;  // 3.3.5: 0x00B7436C  [EM]    CGWorldFrame* — used as `this` for ray-cast and read of mouseover GUID at +0x2C8/+0x2CC
constexpr unsigned int TBC_g_ClientConnection          = 0x00D43318;  // 3.3.5: ?           [KX][EM] g_clientConnection root; objectManager chain = [0xD43318]+0x2218 [EM]
constexpr unsigned int TBC_g_IsInGame                  = 0x00BDB1AC;  // 3.3.5: ?           [EM]    bool — true when in game world
constexpr unsigned int TBC_g_PlayerGuid                = 0x00D68A00;  // 3.3.5: ?           [EM]    local player GUID (8 bytes)
// 2026-07-10: 0x00C71C24 was WRONG (only 15 reads, foreign vtables, not the device).
// True analog of 3.3.5 0x00C5DF88 proven by the device-factory pair (3.3.5 0x681290
// stores to 0xC5DF88; TBC 0x596100 stores to 0xD2A15C; 1818 reads).
// Verified CGxDeviceD3d offsets (3.3.5 -> TBC): projection matrix +0xFC8 -> +0xF4C;
// viewport floats +0x164..0x180 -> +0x168..0x184; HWND +0x3968 -> +0x3850;
// IDirect3DDevice9* +0x397C -> +0x3864; cur RT/depth +0x3B3C/+0x3B40 -> +0x3A08/+0x3A0C
constexpr unsigned int TBC_g_GxDevicePtr               = 0x00D2A15C;  // [disasm-verified 2026-07-10]  3.3.5: 0x00C5DF88
constexpr unsigned int TBC_g_GxDevWindow               = 0x00E1F894;  // 3.3.5: ?           [KX]    s_GxDevWindow — alternate anchor for window/HWND
constexpr unsigned int TBC_g_PlayerBase                = 0x00E29D28;  // 3.3.5: ?           [KX]    PlayerBase pointer (kynox dump). Note: this may be the same as `[g_ClientConnection]+0x2218+...` chain; verify.
constexpr unsigned int TBC_g_MapId                     = 0x00E18DB4;  // 3.3.5: ?           [KX][BB] current map id
constexpr unsigned int TBC_g_OsGetAsyncTimeMs          = 0x00749850;  // 3.3.5: ?           [KX]    timer fn — useful for perf measurements
constexpr unsigned int TBC_g_PlayerLocationX           = 0x00E18DF4;  // 3.3.5: ?           [KX]    local player X (alias of player base + 0xBF0)
constexpr unsigned int TBC_g_PlayerLocationY           = 0x00E18DF8;  // 3.3.5: ?           [KX]
constexpr unsigned int TBC_g_PlayerLocationZ           = 0x00E18DFC;  // 3.3.5: ?           [KX]
constexpr unsigned int TBC_g_PlayerRotation            = 0x00E18E24;  // 3.3.5: ?           [KX]    local player yaw

// ---- 4b. UNKNOWN for 2.4.3 (specific to upstream uses) --------------------------

constexpr unsigned int TBC_TODO_g_FOV_vertical         = 0;  // 3.3.5: 0x00AC0CB4  float — upstream scales mouse-pick into screen-space radians from this
constexpr unsigned int TBC_TODO_g_FOV_horizontal       = 0;  // 3.3.5: 0x00AC0CB8  float
constexpr unsigned int TBC_TODO_g_NearClip = 0x00BA991C;  // [disasm-verified 2026-07-10] float, default 0.1; cvar "nearclip" cb 0x6901D0
constexpr unsigned int TBC_TODO_g_FarClip = 0x00DA45F4;  // [disasm-verified 2026-07-10] float; WRITE via setter 0x682E80 (raw write skips refresh 0x6B4F50 + dirty flag 0xDFE764)
constexpr unsigned int TBC_TODO_g_EventTick            = 0;  // 3.3.5: 0x00B499A4  int — passed to SetControlBit family
constexpr unsigned int TBC_TODO_g_WorldPanel           = 0;  // 3.3.5: 0x00B499A8  CGWorldFrameUI* — has mouseover element ptr at +0x78 + mouse coords at +0x1224/+0x1228
constexpr unsigned int TBC_TODO_g_TargetGuidLo         = 0;  // 3.3.5: 0x00BD07B0  current target GUID lo
constexpr unsigned int TBC_TODO_g_TargetGuidHi         = 0;  // 3.3.5: 0x00BD07B4  current target GUID hi
constexpr unsigned int TBC_TODO_g_MountCount           = 0;  // 3.3.5: 0x00BE8E08  array length of mount spell ids
constexpr unsigned int TBC_TODO_g_MountArrayBase       = 0;  // 3.3.5: 0x00BE8E0C  pointer to mount-spell-id array (4 bytes/entry)
constexpr unsigned int TBC_g_InputControl              = 0x00C896BC;  // 3.3.5: 0x00C24954  [KX]   CInputControl* — used as `this` for SetControlBit
constexpr unsigned int TBC_TODO_g_ZoomSpeedOffset      = 0;  // 3.3.5: 0x00C24E58  declared but effectively unused — drop
constexpr unsigned int TBC_TODO_g_CursorId             = 0;  // 3.3.5: 0x00C26DE8  current cursor sprite id; the wotlk enum (incl. id 53) does not exist in TBC
constexpr unsigned int TBC_TODO_g_HidePlayerFlag       = 0;  // 3.3.5: 0x00C9D540  DWORD — written by sub_ShouldRenderChar to hide own char in 1st person
constexpr unsigned int TBC_TODO_g_StoredMouseX         = 0;  // 3.3.5: 0x00D413EC  saved cursor X during mouse-look
constexpr unsigned int TBC_TODO_g_StoredMouseY         = 0;  // 3.3.5: 0x00D413F0
constexpr unsigned int TBC_TODO_g_MouseHoldFlag        = 0;  // 3.3.5: 0x00D4156C  bool — right/middle mouse-look active
constexpr unsigned int TBC_TODO_g_ObjMgrChainRoot      = 0;  // 3.3.5: 0x00CD87A8  Pointer chain root for active-character via possess. Walk: [+0x34]→[+0x24]→[+0x77C]→[+0x150]


// =============================================================================
// 5. INLINE BINARY PATCHES
// =============================================================================
// upstream's only live byte-level patch.
//   3.3.5: 0x97044C/0x97044D set to 0x90 (NOP) — comment in code: "delete epic code"
//
// Most likely target: purple-glow effect or auto-loot popup at WotLK
// 0x970000 band. PURELY COSMETIC.
//
// PORT RECOMMENDATION: SKIP entirely. Re-find the equivalent later only if
// the cosmetic side-effect is ever identified and wanted.
// -----------------------------------------------------------------------------

constexpr unsigned int TBC_SKIP_NopPatch_DeleteEpicCode_a = 0;  // 3.3.5: 0x0097044C  byte → 0x90  [SKIP]
constexpr unsigned int TBC_SKIP_NopPatch_DeleteEpicCode_b = 0;  // 3.3.5: 0x0097044D  byte → 0x90  [SKIP]
// 3.3.5 had a dead local `DWORD overrideLocation = 0x0082FDF6;` — never written. Drop.


// =============================================================================
// 6. BONUS — useful TBC anchors not in upstream's original code
// =============================================================================
// Adressy z BloogBot/kynox które warto mieć pod ręką jak będziesz w IDA
// szukał detour targets. Można od nich tracerować xrefy.
// -----------------------------------------------------------------------------

constexpr unsigned int TBC_anchor_ObjectUpdate            = 0x00622520;  // [KX]  CGObject_C::Update — likely candidate for sub_UpdateModelProj parent or sibling
constexpr unsigned int TBC_anchor_ClntObjMgrEnumObjects   = 0x0046B3F0;  // [BB][KX]  EnumerateVisibleObjects — useful start for object-manager chain
constexpr unsigned int TBC_anchor_SetTarget               = 0x004A6690;  // [BB][KX]  SetTarget(guid) — anchor for "target" globals
constexpr unsigned int TBC_anchor_Lua_Reload              = 0x00401AE0;  // [KX]  Lua_Reload — useful anchor for the FrameScript dispatch
constexpr unsigned int TBC_anchor_LuaTable_Begin          = 0x0072DAE0;  // [KX]  beginning of internal lua_* function table (lua_gettop, lua_tonumber, ...) — runs to ~0x0072F710
constexpr unsigned int TBC_anchor_BroadcastEvent          = 0x00707850;  // [KX]  for FrameScript event dispatch
constexpr unsigned int TBC_anchor_DescriptorBlock_Object  = 0x00B95890;  // [KX]  s_objectDescriptors
constexpr unsigned int TBC_anchor_DescriptorBlock_Unit    = 0x00B95A48;  // [KX]  s_unitDescriptors
constexpr unsigned int TBC_anchor_DescriptorBlock_Player  = 0x00B96128;  // [KX]  s_playerDescriptors
constexpr unsigned int TBC_anchor_NoFallDamage            = 0x007BA4C0;  // [KX]  if you want it
constexpr unsigned int TBC_anchor_MountainClimbPatch_addr = 0x008C839B;  // [KX]  legendary patch site — not used by upstream but useful cheat-engine reference


// =============================================================================
// END OF FILE
// =============================================================================
//
// PORT WORKFLOW SUMMARY
// =====================
//   Phase A (verified — these compile and link today):
//       Section 1a: ~10 named functions
//       Section 4a: ~10 globals + GxDevicePtr (the SINGLE most important global)
//   Phase B (needs IDA disasm of 2.4.3 Wow.exe):
//       Section 1c: ~5 named functions
//       Section 2:  ~22 sub_XXXXXX (BinDiff vs 3.3.5 strongly recommended)
//       Section 3:  ~3 keybind functions actually used (jumpStart/Stop, moveForwardStart/Stop)
//                   + alternative: route through TBC_FrameScript_Execute
//       Section 4b: ~13 globals (most fragile: g_GxDevicePtr's offset block,
//                   already covered by Section 4a anchor)
//   Phase C (skip outright):
//       Section 1b: 7 unused commented-out fns
//       Section 2d: 2 suppressed detours (SkyboxFix, GreyBoxes)
//       Section 3 SKIP entries: 12 unused keybind fns
//       Section 5:  the NOP patches
// =============================================================================

// ---- 2026-07-10 disasm-verified culling/clip globals (agent report, MILESTONE 8) ----
constexpr unsigned int TBC_g_HorizonFarClip = 0x00DA45F8;  // float, default 2112; cvar "horizonfarclip"
constexpr unsigned int TBC_g_WorldFlags     = 0x00DA4510;  // dword; bit 0x20 = terrain horizon-occlusion culling (console handler 0x681D20)
constexpr unsigned int TBC_g_SceneViewObj   = 0x00BE1100;  // scene/view object used by StartRender; frustum-angle rect at +0x40..0x4C, valid bit +0x3C|1 (getter 0x4321F0)
// CGCamera (both versions identical low layout): +0x38 near, +0x3C far, +0x40 fov (diagonal rad, legal (0,pi)), +0x44 aspect
// TBC GxuXformCreateProjection = 0x5C0E80: fov >= pi -> silent no-op (SetLastError 0x57) = the "2pi disables culling" trick


// =============================================================================
// 7. TBC sky pipeline (disasm-verified 2026-07-10)
// =============================================================================
// The day/night sky + skybox draw path in 2.4.3, mapped by disasm against the
// 3.3.5a analogs. Used by the fix #58-instr diagnostic run to find which gate
// suppresses the sky/far-scenery in stereo pass 1 (left eye).
//
// Draw functions:
constexpr unsigned int TBC_sub_WorldSceneRender    = 0x0069A190;  // 3.3.5: 0x0079A870  gates whether the sky pass runs at all
constexpr unsigned int TBC_sub_DNSkyPass           = 0x006E5DC0;  // 3.3.5: 0x007F09B0  DayNight sky-pass wrapper
constexpr unsigned int TBC_sub_DNRenderOneSky      = 0x006E5CB0;  // 3.3.5: 0x007F08C0  renders one sky object
constexpr unsigned int TBC_sub_GxXformSetViewport  = 0x00596D50;  // 3.3.5: 0x00681F60  takes 6 floats (viewport min/max box)
//
// Gating globals (read by the instrumented run):
constexpr unsigned int TBC_g_SkyVisibleFlag        = 0x00DA54B0;  // 0 => sky pass skipped + black clear path taken
constexpr unsigned int TBC_g_SkyboxModelPtr        = 0x00DA54AC;  // 3.3.5: 0x00CD861C  current skybox model*
constexpr unsigned int TBC_g_AreaLightOverride     = 0x00DA5628;  // 3.3.5: 0x00CD8794  sky drawn only when == 0xF
constexpr unsigned int TBC_g_CurrentZoneLight      = 0x00DA5638;  // current zone light record
constexpr unsigned int TBC_g_DNSkiesEnabled        = 0x00E18F9C;  // 3.3.5: 0x00D38CCC  day/night skies on/off
constexpr unsigned int TBC_g_CurrentSkyObj         = 0x00E18E48;  // 3.3.5: 0x00D38B5C  active sky object*
//
// NOTE: the CGxDevice viewport floats live at +0xEF4..0xF08 (minX,maxX,minY,maxY,
// minZ,maxZ; 3.3.5: +0xF70..0xF84) - the sky wrapper resets the D3D viewport from
// these. Our per-eye viewport writes go to GxDev+0x168..0x184, which is a DIFFERENT
// block (see fix #35 in game_extras.cpp). Resetting one does not touch the other.
//
// ---- fix #61 (2026-07-10 disasm-verified): synchronous world-scene cull ----
// The engine's world cull entry runs once per game frame in the UPDATE phase,
// BEFORE our StartRender hook: via 0x6986F0 it latches the camera into the
// globals below, clears the sky flags, then traverses the world and fills the
// visible-area lists that the terrain render (0x70B4A0) consumes. That fill
// completes asynchronously; when it lands between our two eye passes, the left
// eye renders from an incomplete list. Re-running the cull synchronously right
// before the eye loop makes both eyes see a fully-built list.
constexpr unsigned int TBC_sub_WorldSceneCull = 0x00684AA0;  // cdecl(C3Vector* pos, C3Vector* dir, C3Vector* aux); callsite-verified 0x4AFA27; latches cam + clears sky flags + fills visible-area lists
constexpr unsigned int TBC_g_CamPosLatched    = 0x00DA5B70;  // filled by 0x6986F0 during engine cull (3 floats: pos)
constexpr unsigned int TBC_g_CamDirLatched    = 0x00DA5B7C;  // (3 floats: dir)
constexpr unsigned int TBC_g_CMapObjPtr       = 0x00DA43EC;  // CMap object; terrain render 0x70B4A0 is called with ecx=this
constexpr unsigned int TBC_g_VisAreaListHead  = 0x00DA81C0;  // visible-area list head (filled by 0x699F60)
constexpr unsigned int TBC_g_VisAreaListTail  = 0x00DA81C4;  // visible-area list tail

// ---- fix #62 (2026-07-10 disasm-verified ~95%): engine render-target slot cache ----
// CGxDevice::RenderTargetSet caches the currently-bound render target by object
// identity and EARLY-OUTS (no D3D call) when the cache already matches. Our stereo
// per-eye loop binds eye render targets DIRECTLY on the D3D device, so this engine
// cache goes stale. A mid-scene effect pass (e.g. water reflection 0x6940B0, called
// inside WorldSceneRender before sky/terrain) that binds+restores via the engine
// leaves the WRONG target bound for the rest of pass 1 — terrain+sky pixels land in
// the effect texture instead of the left eye. Fix: force a cache mismatch per eye
// pass so the next engine RenderTargetSet actually issues the D3D call.
//   Slot layout on CGxDevice:  +0x27FC slot0 rtObj / +0x2800 slot0 face
//                              +0x2808 slot1 rtObj / +0x280C slot1 face
//   Do NOT touch +0x2804 / +0x2810 — AddRef'd held refs; the engine's change path
//   releases them correctly, clobbering them leaks/corrupts refcounts.
constexpr unsigned int TBC_sub_RenderTargetSet   = 0x005A48B0;  // __thiscall(int index, void* rtObj, int face); slot cache early-out at 0x5A48D2
constexpr unsigned int TBC_CGxDev_rtSlotCache0   = 0x27FC;      // +0x2800 face; slot1 at +0x2808/+0x280C; do NOT touch +0x2804/+0x2810 (held refs)
constexpr unsigned int TBC_CGxDev_vpDirty        = 0xEF0;       // viewport dirty flag, flushed by 0x5AD780 from +0xEF4..0xF08
