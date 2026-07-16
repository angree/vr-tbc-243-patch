#include "game_extras.h"
#include "cShaderData.h"
#include "cRenderObject.h"
#include "steamVR.h"
#include "structures.h"
#include "cIDirect3DDevice9.h"
#include "../include/tbc_addresses.h"
#include "../include/tbc_structures.h"
#include "rec_probe.h"  // fix #63-instr: per-eye D3D9 command recorder (shared with the proxy device)
#include <float.h>  // fix #61: _finite() for the pre-pass cull camera-sanity guard
#include <stdio.h>  // fix #63-instr: sprintf_s for the command dump hex formatting
#include <cmath>    // aim-assist: sqrtf/tanf/cosf/sinf/fabsf for the multi-ray gaze offsets
#include <fstream>  // Change 3: unified vr_version/vr_config.cfg parser
#include <sstream>  // Change 3: std::istringstream line tokenizing
#include <string>   // Change 3: std::string / std::getline

extern bool doLog;
extern std::stringstream logError;

// SEH helper from seh_helpers.cpp — scans memory for valid IDirect3DDevice9 pointer
extern "C" IDirect3DDevice9* SEH_ScanForDevice(void* ecx, int maxOffset, int* foundOffset);

extern std::ofstream ofOut;  // declared in proxydll.cpp; used by InitDetours logging
#define HOOK_LOG(name) do { ofOut << "[hook:" name "]" << std::endl; ofOut.flush(); } while(0)

#define M_PI		3.14159265358979323846f
float Deg2Rad = M_PI / 180.0f;
float Rad2Deg = 180.0f / M_PI;

const std::string g_VR_PATH = "./vr_version/";

stDX11 devDX11;
int curEye = 0;
// fix #63-instr: per-eye D3D9 command recorder storage (declared extern in rec_probe.h).
volatile int          g_recEye = -1;    // 0=L, 1=R, -1 = not a recorded world pass
volatile int          g_recArmed = 0;   // 1 = record this frame's two world eye passes
volatile unsigned int g_recCurTex = 0;  // fix #63-diff: last stage-0 texture bound (tagged onto each draw)
volatile int          g_warmupSkipDraws = 0;  // fix #71: proxy swallows draws while the warm-up pass runs
RecEntry              g_rec[REC_CAP];
volatile long         g_recCount = 0;
// fix #77: the fix #63 command recorder is a DEBUG tool. Its periodic DumpRec (every
// ~1200 frames) writes thousands of lines to the log on the render thread, which in a
// heavy instance scene spiked frame CPU to ~14ms -> missed the 72Hz deadline -> SteamVR
// reprojected ~half the frames (felt like a few fps in the headset while the flat mirror
// stayed 72). OFF for normal play; set to 1 only when actively diagnosing eye passes.
volatile int          g_recDiag = 0;
static int g_fix56_camHits = 0;  // fix #56 temp instrumentation: UpdateCameraFn hits, reset each OnPaint frame
// fix #58-instr: sky pipeline diagnostics. All touched only on the render thread.
static int g_skyDbg[2][5] = { { 0 } };   // per-eye snapshot [eye][vis,ovr,zone,dn,sky], captured before each world pass
// fix #59: deferred one-frame-batch purge. Flip to false to fall back to fix-#56 behaviour.
// fix #61: defaulted OFF — the deferred purge proved ineffective and must not interact
// with the synchronous pre-pass cull. Code kept in place, just disabled by this flag.
static bool g_fix59_DeferPurge = false;
// fix #61: re-run the engine's world-scene cull synchronously right before the stereo
// eye loop, so both eyes render from a fully-built visible-area list.
// fix #63: DEFAULT OFF — did not fix the missing-terrain left eye; disabled while we
// record the raw per-eye command stream instead. Code kept in place.
static bool g_fix61_PrePassCull = false;
// fix #62: invalidate the engine's render-target slot cache per eye pass, so a mid-scene
// effect pass that binds+restores via the engine can't leave the wrong target bound.
// fix #63: DEFAULT OFF — this invalidation caused a crash in the engine's RenderTargetSet
// change path (crash dump stack 0x5A48F3 -> 0x599093). Code kept in place, just disabled.
static bool g_fix62_RTCacheInvalidate = false;
// fix #62 proof probe: per-eye [eye] result of GetRenderTarget(0) at pass end. 1 = the
// bound RT still equals our eye surface (ok); 0 = something rebound it (HIJACKED).
static int g_rtProbe[2] = { -1, -1 };
// fix #65 experiment: at the END of the stereo body (after the overlay loop, before the
// hook returns) rebind the LEFT eye as the active D3D target (RT+depth) AND repoint the
// engine's curBackBuffer slot (gxDev+0x3A08 color / +0x3A0C depth) at the left-eye
// surfaces, mirroring the j=0 per-eye setup exactly. Rationale: the engine does extra
// rendering AFTER this hook and before Present each frame; today the RIGHT eye is still
// bound at hook exit, so that free rendering (sky/far terrain) all lands in the right
// eye — which is why the right eye looks complete and the left eye shows the honest
// incomplete output of the hooked passes. If the hypothesis holds, binding LEFT at exit
// should SWAP the missing-sky/terrain defect to the right eye. Flag-gated.
static bool g_fix65_LeftBoundAtExit = false;
// TEST: skip the RIGHT eye world pass entirely (j==1). If the LEFT eye then renders complete
// (sky+terrain), the second pass's engine scene rebuild is proven to overwrite the first pass's
// in-flight per-frame geometry. Right eye will be stale/black during the test - intentional.
static bool g_test_SkipRightEye = false;
// fix #67: throwaway world render before the eye loop - the first world traversal of a frame
// always produces incomplete sky/far content (async completion lands mid-frame); warming up
// makes the left eye the second, complete render. Tiny viewport keeps GPU cost near zero; CPU
// walk cost remains.
static bool g_fix67_WarmupPass = true;  // fix #70 probe build - warm-up disabled to expose the pass-1 failure
// fix #70 probe: sky-state probe arm. OnPaint sets this true for ONE frame every 600
// frames; the DNSkyPass hook (0x6E5DC0) then logs the decisive sky globals for both eyes
// of that single frame, then clears it. Rate-limits the probe to 2-3 lines per arm.
volatile bool g_skyProbeArm = false;
// fix #69: remove the forced scene rebuilds we arm every frame (*(int*)tesi = 1). Upstream
// upstream never arms this flag; the engine arms its own once per frame, and the warm-up pass
// (fix #67) already consumes that arm. Our extra arms force a THIRD/redundant full scene
// rebuild each frame - pure CPU cost on a CPU-bound frame. When true, both the warm-up arm
// and the fix #66 j==0 arm are skipped (A/B testable). When false, both arm as before.
static bool g_fix69_NoForcedRebuild = false;
// A/B TEST: false = yesterday's flawed submit (no render pose attached, stepped rotation).
static bool g_fix68_PoseSubmit = true;
// fix #74: write the per-eye projection also into GxDev+0xF0C (the INPUT proj slot)
// so WorldToScreen-projected unit names / combat text / nameplates get true-depth
// per-eye disparity. If head-turn culling pop-in appears, flip this off (option B:
// detour WorldToScreen 0x4AC810 instead).
static bool g_fix74_TextProjFix = false;  // OFF: option A corrupts culling (+0xF0C read by cull path 0x6986F0/0x5BFE90) -> characters below a line + player vanish. Confirm-test then move to option B (detour 0x4AC810).
// fix #74 option B: detour WorldToScreen (0x4AC810). It projects unit names /
// nameplates / combat text through the cached matrix at WF+0x3F8 (= view*proj,
// built from the MONO proj@+0xF0C). We transiently overwrite WF+0x3F8 with
// view*perEyeProj for the duration of the call, then restore it. This gives the
// text true per-eye depth WITHOUT touching +0xF0C, so the culling path (which
// reads +0xF0C, not WF+0x3F8) is left mono and characters stay visible.
static bool g_fix74b_TextProjDetour = true;
// fix #74 option B — DEPTH COMPRESSION to kill cluster cross-eye. Each label
// fuses at its unit's TRUE depth (correct stereo), but two labels at different
// depths that are screen-adjacent get false-matched between the eyes -> the
// "cross-eyed" percept (confirmed by disasm: TBC 2.4.3 has NO nameplate
// declutter/stacking; this is pure stereo physics, not a bug). Cure: blend each
// label's per-eye screen position toward the cyclopean CENTER position:
//     pos = center + k*(perEye - center)
// k=1.0 -> full per-eye depth (inert, == raw option B); k=0.0 -> flat (all labels
// at screen depth, zero disparity, never cross-eyed). ~0.4-0.6 = gentle depth,
// no cross-eye. Live-tunable via vr/nameplate_depth.txt (one float 0..1), re-read
// in OnPaint every ~1s so it can be dialed in WITHOUT a rebuild.
volatile float g_nameplateDepthScale = 0.5f;
// fix #74B flat mode: constant per-eye horizontal correction applied to the RIGHT
// eye's nameplate position (added to outXY[0]). In flat mode both eyes share one
// projection so the right eye's labels are offset from the right-eye world by the
// constant frustum-asymmetry Δc; a single constant here cancels it (or sets a
// comfortable fused depth) for ALL labels at once, without disturbing the now-
// consistent overlap stacking. Live-tunable via vr/nameplate_xshift.txt.
volatile float g_flatXShift = 0.0f;
// fix #74C Y correction: uniform vertical lift applied to EVERY nameplate's final
// position (both eyes equally, so the per-eye depth disparity is untouched). Raises
// the label off the character so a distant (perspective-shrunk) unit isn't covered
// by the fixed-screen-size plate. Live-tunable via vr/nameplate_yshift.txt. Units =
// same normalized screen space as the SetPoint position; sign found by testing.
volatile float g_nameplateYShift = 0.013f;   // user-calibrated default (2026-07-11); tune via file
// fix #75: amplify the unit mouseover/target HIGHLIGHT (too subtle in VR for gaze
// targeting). Engine adds an RGB color to the model's ambient (model+0x1AC/1B0/1B4,
// floats 0..1) when a unit is moused-over (SetHighlight arg 1) or targeted (arg 0);
// the source color 0xE18F7C is dim and time-of-day dependent. We override it after
// the engine's SetHighlight (0x625450). Values >1.0 = strong overbright (very visible).
// Live-tunable via the unified vr/upstream.cfg (mouseover_color / target_color, etc.).
volatile int g_fix75_Highlight = 1;   // non-static + volatile: shared with the proxy via rec_probe.h
// Change 2: separate tint + brightness for mouseover vs target.
volatile float g_hlMouseColor[3]  = { 1.2f, 0.15f, 0.15f };  // bright red-ish mouseover glow
volatile float g_hlMouseBright    = 4.0f;   // mouseover additive strength (1..5 -> passes)
volatile float g_hlTargetColor[3] = { 0.15f, 1.0f, 0.15f };  // bright green target glow
volatile float g_hlTargetBright   = 4.0f;   // target additive strength (1..5 -> passes)
volatile int g_hlBoost = 0;         // set by the per-batch pre-draw hook; read by the proxy for additive re-draw
// Change 1: tint + pass count of the currently-armed highlighted model, for the proxy's colored glow.
volatile float g_hlBoostColor[3]  = { 0.0f, 0.0f, 0.0f };
volatile int   g_hlBoostPasses    = 1;
float g_centerCamPosGame[3] = { 0, 0, 0 }; // cyclopean (no eye-offset) camera position, game coords, for the center-reference call
volatile bool  g_centerCamValid = false;

// VR aim crosshair (reticle) drawn at the center of each eye. The player aims
// with head-gaze, so a fixed "+" at the middle of each eye's render target marks
// where the view points. Live-tunable via vr_version/vr_config.cfg (crosshair / crosshair_size
// / crosshair_color), parsed in OnPaint alongside the other keys.
volatile int   g_crosshair        = 1;                 // on/off
volatile float g_crosshairSize    = 8.0f;              // half-length in eye-RT pixels
volatile float g_crosshairColor[3] = { 1.0f, 1.0f, 1.0f };
// Fine horizontal nudge (pixels) applied per eye to align the reticle onto the
// gaze center. The VR frustum is asymmetric, so the geometric RT center is not the
// visual center; the principal point handles most of it, this trims the rest.
volatile float g_crosshairOffset  = 0.0f;              // -500..500 px, live-tunable (horizontal convergence, per-eye)
volatile float g_crosshairYOffset = 0.0f;              // -500..500 px, live-tunable (vertical shift, both eyes)
// VR aim assist (multi-ray gaze targeting). The center gaze ray is exact; if it
// misses a unit, we fan out a few angularly-offset rays (rings) around it and take
// the first that hits a unit. This widens hover/target selection so aiming with the
// head is forgiving. Live-tunable via vr_version/vr_config.cfg (aim_assist / aim_rings /
// aim_spread_deg / aim_samples), parsed in OnPaint alongside the other keys.
volatile int   g_aimAssist    = 1;                     // on/off
volatile int   g_aimRings     = 2;                     // number of rings (0..4); 0 disables
volatile float g_aimSpreadDeg = 2.0f;                  // angular step per ring, degrees (ring k at k*spread)
volatile int   g_aimSamples   = 6;                     // probe points per ring (3..16)
// The center MUST be identical for the left and right eye passes, otherwise the
// blend diverges instead of scaling disparity (v1 bug: captured per-pass, the game
// camera differed between passes -> cross-eye everywhere). Arm this ONCE per frame
// in StartRender; the first fnUpdateCameraHMD of the frame captures the center and
// disarms, so both eye passes reuse the SAME center.
volatile bool  g_centerCaptureArmed = false;
// FLAT-CONSISTENT mode reference: the engine's per-eye nameplate overlap-stacking
// (which pushes clustered labels apart, decided from each eye's 2D positions)
// diverges between eyes and cannot be fixed by depth-scaling. To kill it we give
// BOTH eyes an IDENTICAL nameplate position: snapshot ONE view*proj cache on the
// first detour call of the frame and reuse it (with the shared center camera) for
// both eyes -> stacking gets identical inputs -> identical layout -> no cross-eye.
float g_flatCache[16] = { 0 };
volatile bool  g_flatValid = false;
// fix #74c: kill nameplate cluster cross-eye at the LAYOUT layer. The engine renders
// the world once per eye and runs Blizzard's SmartScreenRects overlap declutter INSIDE
// each eye pass, so a cluster of nameplates gets pushed apart DIFFERENTLY in the left
// and right eye -> stereo cross-eye. Cure: let the LEFT eye keep the engine's decluttered
// layout untouched, and make the RIGHT eye REUSE that same left-eye layout, shifted per
// plate by that plate's TRUE per-eye disparity (right raw projection - left raw projection).
// We bracket the nameplate apply loop (0x611260, single caller 0x4AE38C, nameplate-exclusive)
// with g_inPlateApply so we only touch the plates' own CLayoutFrame::SetPoint (0x433000)
// calls; all other UI SetPoints pass straight through. The plate pointer is stable across
// both eye passes (plates persist per unit), so we key a per-frame table on it. Live-toggle.
static bool g_fix74c_CopyLayout = true;
volatile bool g_inPlateApply = false;   // true only while the nameplate apply loop runs
// Nameplate occlusion: WorldToScreen (0x4AC810) computes the anchor's true per-eye
// depth d = clip.z/clip.w through the SAME view*perEyeProj the world rendered with.
// The unit-text committer (0x6110F0) pairs that depth with unit+0x1130, whose node is
// the same stable pointer msub_433000 sees as plate = layoutFrame-0x14. SetPoint can
// therefore look the depth up by exact node identity, without a temporal (x,y) join.
// It writes d into CLayoutFrame::m_layoutDepth before the original SetPoint resizes
// the plate and before UI regions are coalesced into a CFrameStrataNode batch. The
// field is restored after this eye's world-list draw. Lookup misses use safe near
// depth (legacy on-top behavior); write/state anomalies leave the engine draw path.
struct PlateRec { void* plate; float finalX, finalY, rawAx, rawAy;
                  float depth[2]; bool depthValid[2];
                  float savedLayoutDepth[2], appliedLayoutDepth[2];
                  bool eyeSeen[2], layoutDepthApplied[2]; };
static const int PLATE_MAX = 128;       // a scene has at most a few dozen plates
static PlateRec g_plateLayout[PLATE_MAX];
static int g_plateLayoutCount = 0;       // filled during the left pass, cleared each frame
volatile int g_nameplateOcclusion = 1;  // cvar vrNameplateOcclusion: 1 = occlude, 0 = legacy overlay
volatile float g_nameplateZForce = -1.0f; // DEBUG live key nameplate_zforce: >=0 forces all matched plate z to this value
volatile int g_nameplateZMode = 1;        // DEBUG live key nameplate_zmode: 0=no z-test, 1=z-buffer, 2=w-buffer (engine may render world with USEW)
volatile float g_nameplateZBias = 0.0f;   // live key nameplate_zbias: subtract from bar z (pull nearer)
volatile int g_nameplateTextBar = 1;      // live key textbar.txt: 1 = append an ASCII health bar as a
                                          // second line of the engine-drawn unit NAME (occludes natively,
                                          // no own geometry); 0 = legacy world-space quad experiment

// ENGINE-CONSISTENT plate depth. The engine itself projects every plate node each
// frame: WorldToScreen 0x4AC810 (position math) immediately followed by the committer
// 0x6110F0 (stores screen x/y on the node). We take the depth from the SAME
// WorldToScreen call that produced the on-screen position (same world position, same
// view*proj cache the engine used), then the committer hook pairs it with the node
// pointer (node == the plate frame msub_433000 sees; identity was runtime-proven).
// The table is keyed by node pointer with UPDATE-IN-PLACE and is never reset, so no
// eye/frame ordering can starve it; a reused node address is corrected by that
// node's own next commit before it is drawn again.
struct PlateDepthEnt { void* node; float d; };
static const int PLATE_DEPTH_MAX = 256;
static PlateDepthEnt g_plateDepth[PLATE_DEPTH_MAX];
static int g_plateDepthCount = 0;
static struct { float d; bool valid; } g_lastW2S = { 0, false }; // render thread only
static bool LookupPlateDepthByNode(void* plate, float* dOut);

// Per-eye WORLD view/proj snapshot for the world-space plate draw. Captured inside
// msub_4AC810's VR branch — the one moment per eye where the view stack verifiably
// holds this eye's world matrices (the same source the engine's own name draw uses).
// The end-of-eye-pass draw must not read the live stack (it may hold UI matrices by
// then), so it uses this snapshot. One matrix pair per eye; refreshed every pass.
static float g_wsView[2][16], g_wsProj[2][16];
static bool g_wsMtxValid[2] = { false, false };
// Camera world position per eye (captured with the matrices in msub_4AC810): the
// engine renders CAMERA-RELATIVE (the view has no translation), so absolute unit
// coordinates must be relativized before the view*proj transform.
static float g_wsCamPos[2][3];
static bool g_wsCamValid[2] = { false, false };
// DIAG: where does the world-space plate draw stop?
static unsigned g_dbgMtxCap = 0, g_dbgWsUsedFB = 0, g_dbgWsNoMtx = 0;
static unsigned g_dbgWsNoUnit = 0, g_dbgWsStale = 0, g_dbgWsClip = 0;
static unsigned g_dbgWsAbs = 0, g_dbgWsRel = 0;   // which coordinate mode landed on-screen
static float g_dbgLastT2 = 0;
static float g_dbgWsNdc[3] = { 0, 0, 0 };         // last chosen-mode anchor NDC (sanity)

// Unit-text node type discriminators (vtable at node+0), disasm-verified on 8606:
// nameplate nodes (NamePlateFrame.cpp ctor 0x7BD7B0) vs floating-combat-text nodes
// (CombatText vtable installed at 0x52BB63). Both node kinds live on the same global
// list 0xBA4BA4 and both reach the apply loop's SetPoint, so the vtable check is what
// keeps FCT and ctor-time child SetPoints out of the plate layout/occlusion tables.
static const uintptr_t TBC_NAMEPLATE_NODE_VTABLE = 0x008F7CE0;
static const uintptr_t TBC_COMBATTEXT_NODE_VTABLE = 0x008B4F08;

// TBC 8606 frame layout facts (disasm-verified):
//   * msub_433000 receives the embedded CLayoutFrame at plate+0x14 (runtime-verified).
//   * CLayoutFrame+0x58 = m_layoutScale; +0x5C..+0x68 = the CACHED LAYOUT RECT.
//     2.4.3 has NO per-frame depth field (Blizzard added one only in Cataclysm), so
//     depth must be injected into the drawn vertices, not into the frame (see the
//     flush-time z patch msub_42F390).
static const int TBC_SIMPLEFRAME_LAYOUT_OFFSET = 0x14; // VERIFIED by msub_433000/runtime
// Plate node render structure (for the flush-time z patch), all disasm-verified:
//   flushed batches (strata chain) hold elements: batch+0xC = element count,
//   batch+0x10 = element array (stride 0x3C); elem+0x10 = vertex count, elem+0x14 =
//   POINTER to the region's corner cache at region+0xD0 (XYZ stride 0xC, z 3rd float);
//   region+0x94 = owning frame (CSimpleRegion::SetParent 0x431540 writes it).
//   node+0x384 = plate's status-bar child frame, node+0x388 = cast bar frame.
static const int TBC_PLATE_BARFRAME_OFFSET = 0x384;
static const int TBC_PLATE_CASTBAR_OFFSET = 0x388;

// Diagnostics are render-thread only and emitted by the existing [occl] 2-second log.
static int g_plateOcclSeen = 0, g_plateOcclFrameMatched = 0;
static int g_plateOcclDepthMatched = 0, g_plateOcclDepthApplied = 0;
static int g_plateOcclNoDepth = 0, g_plateOcclNearForced = 0;
static int g_plateOcclWriteRejected = 0;
static int g_plateOcclDraws = 0, g_plateOcclReadyAtDraw = 0;
static int g_plateOcclLostBeforeDraw = 0, g_plateOcclDataFallback = 0;
static int g_plateOcclStateFallback = 0;
// DIAG: engagement probe — does the nameplate apply loop (0x611260) run this frame,
// at which curEye, and does it record any plates? Pinpoints why occlusion may not engage.
static int g_occlApplyFires = 0, g_occlApplyEye0 = 0, g_occlCountAfterApply = 0;
// DIAG: engine-depth pipeline probe — W2S depth stashes, committer fires/pairs,
// node-table size, plate lookups, vtable-filtered SetPoints, and z-patched batches.
static unsigned g_dbgStash = 0, g_dbgCommitFires = 0, g_dbgPaired = 0;
static unsigned g_dbgLookOk = 0, g_dbgLookMiss = 0, g_dbgVtRej = 0;
static unsigned g_dbgZBatchPatched = 0;   // strata batches whose vertex z we rewrote (legacy path, now 0)
static float g_dbgZLastPatched = 0.0f;    // last depth value written into vertices (legacy path)
// WORLD-SPACE REDESIGN diagnostics (printed by the [occl] 2-second window):
// hidden = engine plate quads suppressed at strata flush (vertex count zeroed),
// wsDraws = our own world-space plate draws in the name pipeline slot,
// wsGatherFail = name entries whose unit/plate/matrix resolution faulted (SEH).
static unsigned g_dbgHiddenElems = 0;
static unsigned g_dbgWsDraws = 0;
static unsigned g_dbgWsGatherFail = 0;
static bool g_plateOcclIncomplete[2] = { false, false };
static float g_plateOcclSamples[2] = { 0.0f, 0.0f };
static int g_plateOcclSampleCount = 0;

// DEPTH DELIVERY (disasm-verified 8606): UI frames in 2.4.3 have NO depth field —
// every world-strata quad's vertex z is a literal 0.0 written by the corner updater
// 0x42D640 into the region's corner cache (region+0xD0, 4 x XYZ). The batch flush
// 0x42F390 copies those corners to the GPU by POINTER at draw time. So the only
// reliable way to give a plate a real z is to overwrite the corner-cache z values
// AT FLUSH TIME (msub_42F390 below), after any layout rebuild and right before the
// GPU copy. Record here only computes and stores the depth; nothing is written.
static void RecordPlateOcclusionResult(PlateRec& r, void* /*layoutFrame*/, int eye,
                                       void* plate)
{
    if (eye != 0 && eye != 1) return;
    ++g_plateOcclSeen;
    r.eyeSeen[eye] = true;
    r.depthValid[eye] = LookupPlateDepthByNode(plate, &r.depth[eye]);
    r.layoutDepthApplied[eye] = r.depthValid[eye]; // "will be z-patched at flush"
    if (r.depthValid[eye]) {
        ++g_plateOcclDepthMatched;
        ++g_plateOcclDepthApplied;
        if (g_plateOcclSampleCount < 2)
            g_plateOcclSamples[g_plateOcclSampleCount++] = r.depth[eye];
    } else {
        // No depth -> vertices keep the engine's z = 0.0 -> the plate draws on top
        // (legacy behavior). Inherently safe: nothing can be hidden by mistake.
        ++g_plateOcclNoDepth;
        ++g_plateOcclNearForced;
    }
}

// Stash the depth of the WorldToScreen call that just produced a plate's on-screen
// position, using the SAME world position and the SAME view*proj cache the engine
// used for that call. This is the engine's own math — always consistent with what
// is drawn, in every pass and mode. The immediately following committer (0x6110F0)
// pairs it with the node pointer.
static inline void StashW2SDepth(const float* worldPos, const float* m, int ok)
{
    g_lastW2S.valid = false;
    if (!ok || !worldPos || !m) return;
    __try {
        float z = worldPos[0]*m[2] + worldPos[1]*m[6] + worldPos[2]*m[10] + m[14];
        float w = worldPos[0]*m[3] + worldPos[1]*m[7] + worldPos[2]*m[11] + m[15];
        if (w > 0.0001f) {
            float d = z / w;
            if (_finite(d) && d >= 0.0f && d <= 1.0f) {
                g_lastW2S.d = d;
                g_lastW2S.valid = true;
                ++g_dbgStash;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastW2S.valid = false;
    }
}

// Exact pointer-equality lookup in the update-in-place node table.
static bool LookupPlateDepthByNode(void* plate, float* dOut)
{
    if (!plate || !dOut) return false;
    for (int i = 0; i < g_plateDepthCount; ++i) {
        if (g_plateDepth[i].node == plate) {
            *dOut = g_plateDepth[i].d;
            ++g_dbgLookOk;
            return true;
        }
    }
    ++g_dbgLookMiss;
    return false;
}
// fix #69 timing accumulators (milliseconds). Each frame msub_495410 adds its four measured
// section times here; OnPaint prints the 600-frame average and resets. File scope so the
// StartRender hook (writer) and the OnPaint hook (reader/printer) share them.
static double g_time69_warmupSum = 0, g_time69_LSum = 0, g_time69_RSum = 0, g_time69_UISum = 0;
static int g_time69_frames = 0;
// fix #58/#59 probe: per-eye batch-chain diagnostics [eye][pre,post,dirty,purge], captured on the render thread.
static int g_batchDbg[2][4] = { { 0 } };
int g_wsrHits[3] = { 0, 0, 0 };          // WorldSceneRender hits this frame, tagged by curEye (0=L,1=R,2=UI)
int g_skyPassHits[3] = { 0, 0, 0 };      // DNSkyPass hits this frame, tagged by curEye
// fix #60-instr: per-eye hit counters for three candidate world-render sub-passes
// called after the sky pass inside WorldSceneRender (one is the terrain renderer).
int g_wsr75A0Hits[3] = { 0, 0, 0 };      // sub_6975A0 hits this frame, tagged by curEye
int g_wsr99C0Hits[3] = { 0, 0, 0 };      // sub_6999C0 hits this frame, tagged by curEye
int g_wsr5840Hits[3] = { 0, 0, 0 };      // sub_695840 hits this frame, tagged by curEye
// fix #61-instr: confirmation counters. 0x699F60 (visible-list fill primitive) and
// 0x70B4A0 (CMap terrain render) also run OUTSIDE our eye passes (engine UPDATE phase,
// where curEye may be stale) so these numbers only mean something alongside the raw
// visible-list head/tail snapshots below.
int g_fillHits[3] = { 0, 0, 0 };         // 0x699F60 visible-list fill hits, tagged by curEye
int g_cmapHits[3] = { 0, 0, 0 };         // 0x70B4A0 CMap terrain render hits, tagged by curEye
static int g_visListDbg[2][2] = { { 0 } };  // per-eye raw [head,tail] of 0xDA81C0/0xDA81C4, captured before each eye i-loop
IDirect3DDevice9* devDX9 = nullptr;
int textureIndex = 0;
// fix #64-instr: on-demand eye-texture pixel dump. A trigger file (./vr_version/dump_eyes.txt),
// checked every 2s in OnPaint, sets g_dumpEyesRequested. The stereo StartRender hook then
// saves both world eye render targets twice (pre-UI = *_A, post-UI = *_B), consumes the
// request and sets g_dumpEyesReadyPending. OnPaint then saves the ready/display slot
// (*_ready) once g_readyIndex is valid and clears the pending flag.
volatile bool g_dumpEyesRequested   = false;  // set by trigger, consumed by StartRender
volatile bool g_dumpEyesReadyPending = false; // set by StartRender, consumed by OnPaint
//simpleXR* sxr = new simpleXR(true);
simpleVR* svr = new simpleVR(false);

stScreenLayout screenLayout = stScreenLayout();
POINT hmdBufferSize = { 0, 0 };
POINT uiBufferSize = { 0, 0 };

stBasicTexture BackBuffer11[6] = { stBasicTexture(), stBasicTexture(), stBasicTexture(), stBasicTexture(), stBasicTexture(), stBasicTexture() };
stBasicTexture DepthBuffer11[6] = { stBasicTexture(), stBasicTexture(), stBasicTexture(), stBasicTexture(), stBasicTexture(), stBasicTexture() };

stBasicTexture9 BackBuffer[6] = { stBasicTexture9(), stBasicTexture9(), stBasicTexture9(), stBasicTexture9(), stBasicTexture9(), stBasicTexture9() };
stBasicTexture9 DepthBuffer[6] = { stBasicTexture9(), stBasicTexture9(), stBasicTexture9(), stBasicTexture9(), stBasicTexture9(), stBasicTexture9() };

//stBasicTexture9 mainRenderBuffer = stBasicTexture9();
//stBasicTexture9 mainDepthBuffer = stBasicTexture9();

stBasicTexture9 uiRender = stBasicTexture9();
stBasicTexture9 uiRenderMask = stBasicTexture9();
stBasicTexture9 uiRenderCheck = stBasicTexture9();
stBasicTexture9 uiRenderCheckSystem = stBasicTexture9();
stBasicTexture9 uiDepth = stBasicTexture9();
stBasicTexture9 cursor = stBasicTexture9();

struct RayTarget
{
    unsigned int targetIDA;
    unsigned int targetIDB;
    Vector3 intersectPoint;
    float intersectDepth;
    Vector3 intersectFrom;
    Vector3 intersectTo;
};

bool isRunningAsAdmin = false;
bool isPossessing = false;
std::stringstream outStream;

bool doOcclusion = false;  // fix #45: UI panel is a HUD — always on top. true let the (nearer) player character draw over the (farther) panel. Toggle keybind still at line ~3097.
stObjectManager* gPlayerObj = nullptr;

float gRotation = 0;
float gCamRotation = 0;
float vRotationOffset = 0;
float hRotationStickOffset = 0;
float maxRadRot = 2 * M_PI;
bool resetPlayerAnimCounter = false;
DWORD origBackBuffer = 0;
DWORD origDepthBuffer = 0;
DWORD curBackBufferLoc = 0;
IDirect3DSurface9* mainBackBuffer = nullptr;

bool printRoute = false;
bool isOverUI = false;
int indent = 0;
POINT wtfSize = { 0, 0 };

ShaderData vsTexture = VertexShaderTexture();
ShaderData psTexture = PixelShaderTexture();
ShaderData psMouseDot = PixelShaderWithMouseDot();
ShaderData psMask = PixelShaderMask();

RenderObject curvedUI = nullptr;
RenderObject maskedUI = nullptr;
RenderObject cursorUI = nullptr;



XMMATRIX matProjection[2] = { XMMatrixIdentity(), XMMatrixIdentity() };
XMMATRIX matEyeOffset[2] = { XMMatrixIdentity(), XMMatrixIdentity() };
XMMATRIX matHMDPos = XMMatrixIdentity();
XMMATRIX matController[2] = { XMMatrixIdentity(), XMMatrixIdentity() };
XMMATRIX matControllerPalm[2] = { XMMatrixIdentity(), XMMatrixIdentity() };
XMMATRIX cameraMatrix = XMMatrixIdentity();
XMMATRIX cameraMatrixIPD = XMMatrixIdentity();
XMMATRIX cameraMatrixGame = XMMatrixIdentity();
XMMATRIX zeroScale = XMMatrixScaling(0.00001f, 0.00001f, 0.00001f);
XMMATRIX before = {
         0, 0,-1, 0,
        -1, 0, 0, 0,
         0, 1, 0, 0,
         0, 0, 0, 1,
};
XMMATRIX after = {
         0,-1, 0, 0,
         0, 0, 1, 0,
        -1, 0, 0, 0,
         0, 0, 0, 1,
};
IDirect3DTexture9* hiddenTexture;

//----
// Config settings
//----
// BAKED 2026-07-12 from the working vr/config.txt values (config.txt no longer read). The 4 uiOffset* are DEFAULTS - overridden live by vr_version/vr_config.cfg (screen_size/screen_distance/screen_height/screen_depth).
bool cfg_snapRotateX = false;
bool cfg_snapRotateY = true;
float cfg_snapRotateAmountX = 45.0f;
float cfg_snapRotateAmountY = 15.0f;
float cfg_uiOffsetScale = 1.3f;
float cfg_uiOffsetZ = -100.0f;
float cfg_uiOffsetY = -20.0f;
float cfg_uiOffsetD = -0.94055f;
// UI/game screen panel curvature. Read at startup (applied when the curved-UI geometry
// is built). +1.0 = concave (original look, DEFAULT), 0 = flat, -1.0 = convex; the
// magnitude sets how strong the curve is. Used by RenderCurvedUI.cpp.
volatile float g_screenCurve = 1.0f;
// world_scale: apparent size of the whole 3D world in VR (live-tunable). 1.0 = normal,
// <1 = miniature "toybox" (e.g. 0.25 = 25% size), >1 = giant world. Applied in
// fnUpdateCameraHMD by scaling the eye baseline (IPD) + head-tracking translation by 1/S.
volatile float g_worldScale = 0.7f;
// When a WoW addon pushes any vr* value through the SetCVar bridge, the addon
// becomes the source of truth: the 2s vr_config.cfg re-read below is disabled so
// it can no longer overwrite (clobber) the value the addon just set.
volatile bool g_addonConfigActive = false;
int cfg_flyingMountID = 0;
int cfg_groundMountID = 0;
int cfg_hmdOnward = 0;
int cfg_uiMultiplier = 1;
int cfg_gameMultiplier = 1;
bool cfg_disableControllers = true;    // CRITICAL: true keeps the VR mouse cursor visible (false scales it to zero)
bool cfg_showBodyFPS = true;
inputController input = {}; //{ { 0, 0, 0, 0, 0, 0, 0, 0, 0 } };

//----
// DEBUG STUFF
//----
struct debugCounter
{
    int ida = 0;
    int idb = 0;
    int idc = 0;
    int idd = 0;
    int ide = 0;

    debugCounter()
    {
        ida = 0;
        idb = 0;
        idc = 0;
        idd = 0;
        ide = 0;
    }

    void update(int itemCount, int counter)
    {
        int value = 0;
        value = counter;
        counter = value / itemCount;
        ida = value % itemCount;

        value = counter;
        counter = value / itemCount;
        idb = value % itemCount;

        value = counter;
        counter = value / itemCount;
        idc = value % itemCount;

        value = counter;
        counter = value / itemCount;
        idd = value % itemCount;
    }
};

//----
// Game Declarations
//----
//void(__thiscall* CGWorldFrameM__OnWorldUpdate)(void*) = (void(__thiscall*)(void*))0x004FA5F0; // TODO_TBC
//void(__thiscall* CGWorldFrameM__Render)(void*) = (void(__thiscall*)(void*))0x004F8EA0; // TODO_TBC
void(__thiscall* CalculateForwardMovement)(int, int) = (void(__thiscall*)(int, int))TBC_CalculateForwardMovement;
void(__thiscall* CGMovementInfo__SetFacing)(int, float) = (void(__thiscall*)(int, float))TBC_CGMovementInfo_SetFacing;
// TBC unifies SetControlBit/UnsetControlBit/UpdatePlayer into a single dispatcher; for now keep the upstream call shape and route to the closest equivalents:
void(__thiscall* CGInputControl__SetControlBit)(int, int, int) = (void(__thiscall*)(int, int, int))TBC_CGInputControl_SetControlBit_DISPATCHER;
int (__thiscall* CGInputControl__UnsetControlBit)(int, int, int, int) = (int(__thiscall*)(int, int, int, int))TBC_CGInputControl_UnsetControlBit;
void(__thiscall* CGInputControl__UpdatePlayer)(int, int, int) = (void(__thiscall*)(int, int, int))TBC_CGInputControl_UpdatePlayer;
//void(__thiscall* CGCamera__ZoomIn)(int, float, int, float) = (void(__thiscall*)(int, float, int, float))0x005FF950; // TODO_TBC
//void(__thiscall* CGCamera__ZoomOut)(int, float, int, float) = (void(__thiscall*)(int, float, int, float))0x005FFA60; // TODO_TBC
//void(__thiscall* CameraUpdateX)(int, float) = (void(__thiscall*)(int, float))0x005FE5F0; // TODO_TBC
//void(__thiscall* CameraUpdateY)(int, float) = (void(__thiscall*)(int, float))0x005FFC20; // TODO_TBC
//bool(__thiscall* IsFallingSwimmingFlying)(int) = (bool(__thiscall*)(int))0x006EABA0; // TODO_TBC
void (*CastSpell)(int, int, int, int, int) = (void (*)(int, int, int, int, int))TBC_CastSpell;

bool(__thiscall* RayIntersect)(int, float, float, Vector3*, Vector3*) = (bool(__thiscall*)(int, float, float, Vector3*, Vector3*))TBC_TODO_RayIntersect;
bool(__thiscall* WorldClickIntersect)(int, Vector3*, Vector3*, unsigned int, RayTarget*) = (bool(__thiscall*)(int, Vector3*, Vector3*, unsigned int, RayTarget*))TBC_WorldClickIntersect;

float (*EnsureProperRadians)(float) = (float (*)(float))TBC_EnsureProperRadians;
int (*CGWorldFrame__GetActiveCamera)() = (int (*)())TBC_CGWorldFrame_GetActiveCamera;

// TBC port fix #15: in TBC 0x004AB5B0 is a __thiscall member — it reads
// [ecx+0x732C], so it needs the CGWorldFrame* as `this`. The upstream call
// (plain cdecl, garbage ECX) crashed at the login screen (2026-07-09 23:14
// dump: ACCESS_VIOLATION @ 0x004AB5B5 reading 0x732C, ECX=0). Always go
// through this wrapper: no world frame yet -> no camera -> return 0.
static int GetActiveCameraSafe()
{
    int worldFrame = *(int*)TBC_g_WorldFrame;
    if (!worldFrame) return 0;
    typedef int(__thiscall* tGetActiveCamera)(int);
    return ((tGetActiveCamera)TBC_CGWorldFrame_GetActiveCamera)(worldFrame);
}

// TBC port fix #16: Step-2 gates (restored — the 2026-04-30 rescue builds lost
// the "morning v1" gating). All three bodies write to engine structs at WotLK
// offsets (dxLoc+0x164.., GxDevicePtr+0xFC8 memcpy, curBackBufferLoc=NULL) and
// crashed frame #0 (2026-07-09 23:18 dump: pointer replaced by float 1.0f).
// Enable ONE at a time after verifying the TBC offsets in Ghidra.
static bool g_step2_StereoStartRender = true;   // fix #35 EXPERIMENTAL: all offsets disasm-verified 2026-07-10
static bool g_step2_InjectProjMatrix  = true;   // fix #35 EXPERIMENTAL: anchored at this+0xF4C
// TBC diag: first-hits entry trace for every detour, to pinpoint which hook is
// last alive before an in-world hang/crash (wild-jump EIP 0x448D3A18 pattern).
#define MSUB_TRACE(name) do { static int _th = 0; if (_th < 3) { ofOut << "[trace] " << name << " #" << _th << std::endl; ofOut.flush(); } _th++; } while(0)

static bool g_step2_BoneHacks         = false;  // head-bone hide / boneLookup — uses UNVERIFIED TBC offsets (pModelContainer chain)
// fix #27 DIAGNOSTIC: HMD->game-camera injection suspected of corrupting the
// engine's culling (objects vanish at distance, fps depends on view direction,
// persistent slowdowns). OFF for one test run — head rotation won't work.
static bool g_camHMDInject            = true;   // fix #29: re-enabled without the +0x40 write

// TBC port fix #21: D3D9 Reset handling. Our default-pool resources (render
// targets, shared textures) must be released before IDirect3DDevice9::Reset and
// recreated after, or Reset blocks forever (suspected cause of the world-entry
// freeze: frames stop, adapter-mode probe, then silence with 0% CPU).
bool g_vrResourcesLive = false;
bool g_vrSuspended = false;
bool g_eyeTexFilled[6] = { false, false, false, false, false, false };  // fix #23c
//int (*ClntObjMgrGetActivePlayer)() = (int(*)())0x004D3790; // TODO_TBC
stObjectManager*(*ClntObjMgrObjectPtr)(unsigned int, unsigned int, unsigned int, const char*, unsigned int) = (stObjectManager * (*)(unsigned int, unsigned int, unsigned int, const char*, unsigned int))TBC_ClntObjMgrObjectPtr;
stObjectManager*(*ClntObjMgrGetActivePlayerObj)() = (stObjectManager*(*)())TBC_ClntObjMgrGetActivePlayerObj;
//int (*CMovementStatus__Read)(int, int) = (int(*)(int, int))0x004F4D40; // TODO_TBC

void (__thiscall* Matrix44Translate)(int, int) = (void (__thiscall*)(int, int))0x004C1B30; // TODO_TBC

//bool (*lua_ChangeActionBarPage)(int) = (bool (*)(int))0x005A7F60; // TODO_TBC
//int  (*lua_ToggleRun)() = (int (*)())0x005FAAE0; // TODO_TBC
//void (*lua_RunBinding)(char*, char*) = (void(*)(char*, char*))0x0055FAD0; // TODO_TBC
void (*lua_TargetNearestEnemy)(int*) = (void(*)(int*))TBC_lua_TargetNearestEnemy;
void (*lua_Dismount)() = (void(*)())TBC_lua_Dismount;
//bool (*lua_IsMounted)(int) = (bool(*)(int))0x006125A0; // TODO_TBC

//void (*lua_MoveView)() = (void(*)())0x005FF000; // TODO_TBC
//void (*lua_CameraOrSelectOrMoveStart)() = (void(*)())0x005FC6C0; // TODO_TBC
//void (*lua_CameraOrSelectOrMoveStop)(int*) = (void(*)(int*))0x005FC730; // TODO_TBC
//void (*lua_TurnOrActionStart)() = (void(*)())0x005FC610; // TODO_TBC
//void (*lua_TurnOrActionStop)() = (void(*)())0x005FC680; // TODO_TBC


void RunFrameUpdateController();
void RunFrameUpdateSetCursor();  // fix #40


XMVECTOR GetAngles(XMMATRIX source)
{
    float thetaX, thetaY, thetaZ = 0.0f;
    thetaX = std::asin(source._32);

    if (thetaX < (M_PI / 2))
    {
        if (thetaX > (-M_PI / 2))
        {
            thetaZ = std::atan2(-source._12, source._22);
            thetaY = std::atan2(-source._31, source._33);
        }
        else
        {
            thetaZ = -std::atan2(-source._13, source._11);
            thetaY = 0;
        }
    }
    else
    {
        thetaZ = std::atan2(source._13, source._11);
        thetaY = 0;
    }
    return { thetaX, thetaY, thetaZ, 0 };
}

bool IsElevated()
{
    bool fRet = false;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize))
        {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if (hToken)
    {
        CloseHandle(hToken);
    }
    return fRet;
}

int searchAddress(std::vector<std::vector<int>> addressList)
{
    int addressCount = addressList.size();
    // TBC port: bail out if root is zero (TODO unresolved)
    if (addressCount == 0 || addressList[0][0] == 0)
        return 0;
    for (int i = 0; i < addressCount; i++)
        if (i == 0)
            addressList[i][1] = *(int*)(addressList[i][0]);
        else if (addressList[i - 1][1] > 0)
            addressList[i][1] = *(int*)(addressList[i - 1][1] + addressList[i][0]);

    return (addressList[addressCount - 1][1]) ? addressList[addressCount - 1][1] : 0;
}

stObjectManager* objManagerGetActiveObject()
{
    std::vector<std::vector<int>> charObj = {
            { (int)TBC_TODO_g_ObjMgrChainRoot, 0 }, // 3.3.5: 0xCD87A8 — possess detection (probably skippable in TBC)
            { 0x34, 0 },
            { 0x24, 0 },
            { 0x77C, 0 },
            { 0x150, 0 }
    };
    return (stObjectManager*)searchAddress(charObj);
}


stObjectManager* objManagerGetTargetObj()
{
    // Game Target
    int idA = *(int*)0x00BD07B0; // TODO_TBC
    int idB = *(int*)0x00BD07B4; // TODO_TBC
    if (idA)
        return ClntObjMgrObjectPtr(idA, idB, 8, ".\\GameUI.cpp", 0x774);
    return 0;
}

stObjectManager* objManagerGetMouseoverObj()
{
    // Mouseover
    int worldFrame = *(int*)TBC_g_WorldFrame;
    if (worldFrame)
    {
        int idA = *(int*)(worldFrame + 0x2C8);
        int idB = *(int*)(worldFrame + 0x2CC);
        if (idA)
            return ClntObjMgrObjectPtr(idA, idB, 1, ".\\GameUI.cpp", 0xFFFF);
    }
    return 0;
}





int cfg_tID = 0;

void CreateTextures(ID3D11Device* devDX11, IDirect3DDevice9* devDX9, POINT textureSize, POINT textureSizeUI)
{
    extern std::ofstream ofOut;
    HRESULT result = S_OK;
    ofOut << "[ct] entry textureSize=" << textureSize.x << "x" << textureSize.y << " UI=" << textureSizeUI.x << "x" << textureSizeUI.y << std::endl; ofOut.flush();
    for (int i = 0; i < 6; i++)
    {
        ofOut << "[ct] iter " << i << " BackBuffer11.Create ..." << std::endl; ofOut.flush();
        BackBuffer11[i].SetWidthHeight(textureSize.x, textureSize.y);
        bool b11 = BackBuffer11[i].Create(devDX11, false, true, false, true);
        std::string e11 = BackBuffer11[i].GetErrors();
        ofOut << "[ct] iter " << i << " BackBuffer11 ok=" << b11 << " sh=" << BackBuffer11[i].pSharedHandle << " errs='" << e11.c_str() << "'" << std::endl; ofOut.flush();
        if (!b11) logError << e11;

        ofOut << "[ct] iter " << i << " BackBuffer.Create ..." << std::endl; ofOut.flush();
        BackBuffer[i].SetWidthHeight(textureSize.x, textureSize.y);
        BackBuffer[i].pSharedHandle = BackBuffer11[i].pSharedHandle;
        bool b9 = BackBuffer[i].Create(devDX9, false, true, false, true);
        std::string e9 = BackBuffer[i].GetErrors();
        ofOut << "[ct] iter " << i << " BackBuffer ok=" << b9 << " errs='" << e9.c_str() << "'" << std::endl; ofOut.flush();
        if (!b9) logError << e9;


        // TBC port fix: do NOT share depth buffers. D3D9 CreateDepthStencilSurface
        // does not officially accept a shared handle from D3D11 D24_UNORM_S8_UINT —
        // works on WotLK due to driver leniency, corrupts heap on TBC. VRCompositor
        // doesn't need shared depth; we can use independent D3D9 + D3D11 depth.
        ofOut << "[ct] iter " << i << " DepthBuffer11.Create (non-shared) ..." << std::endl; ofOut.flush();
        DepthBuffer11[i].SetWidthHeight(textureSize.x, textureSize.y);
        DepthBuffer11[i].textureDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        DepthBuffer11[i].textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        DepthBuffer11[i].textureDesc.MiscFlags = 0;  // override default SHARED — depth cannot be shared safely
        bool d11 = DepthBuffer11[i].Create(devDX11, false, false, true, false);
        std::string ed11 = DepthBuffer11[i].GetErrors();
        ofOut << "[ct] iter " << i << " DepthBuffer11 ok=" << d11 << " errs='" << ed11.c_str() << "'" << std::endl; ofOut.flush();
        if (!d11) logError << ed11;

        ofOut << "[ct] iter " << i << " DepthBuffer.Create (non-shared) ..." << std::endl; ofOut.flush();
        DepthBuffer[i].SetWidthHeight(textureSize.x, textureSize.y);
        DepthBuffer[i].pSharedHandle = nullptr;  // don't use D3D11 depth handle
        DepthBuffer[i].renderFormat = D3DFMT_D24X8;
        bool d9 = DepthBuffer[i].Create(devDX9, false, false, true, false);
        std::string ed9 = DepthBuffer[i].GetErrors();
        ofOut << "[ct] iter " << i << " DepthBuffer ok=" << d9 << " errs='" << ed9.c_str() << "'" << std::endl; ofOut.flush();
        if (!d9) logError << ed9;
    }
    ofOut << "[ct] all 6 iterations done, moving to UI textures ..." << std::endl; ofOut.flush();

    ofOut << "[ct] uiRender.Create ..." << std::endl; ofOut.flush();
    uiRender.SetWidthHeight(textureSizeUI.x, textureSizeUI.y);
    if(!uiRender.Create(devDX9, false, true, false, false))
        logError << uiRender.GetErrors();
    ofOut << "[ct] uiRender done" << std::endl; ofOut.flush();

    ofOut << "[ct] uiRenderMask.Create ..." << std::endl; ofOut.flush();
    uiRenderMask.SetWidthHeight(textureSizeUI.x / 4, textureSizeUI.y / 4);
    if (!uiRenderMask.Create(devDX9, false, true, false, false))
        logError << uiRenderMask.GetErrors();
    ofOut << "[ct] uiRenderMask done" << std::endl; ofOut.flush();

    ofOut << "[ct] uiRenderCheck.Create ..." << std::endl; ofOut.flush();
    uiRenderCheck.SetWidthHeight(1, 1);
    if (!uiRenderCheck.Create(devDX9, false, true, false, false))
        logError << uiRenderCheck.GetErrors();
    ofOut << "[ct] uiRenderCheck done" << std::endl; ofOut.flush();

    //----
    // Requires SystemMem
    //----
    ofOut << "[ct] uiRenderCheckSystem CreateTexture ..." << std::endl; ofOut.flush();
    uiRenderCheckSystem.creationType = 1;
    result = devDX9->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &uiRenderCheckSystem.pTexture, NULL);
    if (SUCCEEDED(result))
        uiRenderCheckSystem.CreateShaderResourceView(devDX9);
    ofOut << "[ct] uiRenderCheckSystem done hr=" << std::hex << result << std::dec << std::endl; ofOut.flush();


    ofOut << "[ct] uiDepth.Create ..." << std::endl; ofOut.flush();
    uiDepth.SetWidthHeight(textureSizeUI.x, textureSizeUI.y);
    uiDepth.renderFormat = D3DFMT_D24X8;
    if (!uiDepth.Create(devDX9, false, false, true, false))
        logError << uiDepth.GetErrors();
    ofOut << "[ct] uiDepth done" << std::endl; ofOut.flush();


    //----
    // Cursor icons
    //----
    std::string cursorPath = g_VR_PATH + "pointer.png";
    ofOut << "[ct] cursor.CreateFromFile (" << cursorPath.c_str() << ") ..." << std::endl; ofOut.flush();
    if (!cursor.CreateFromFile(devDX9, false, false, false, cursorPath.c_str()))
        logError << cursor.GetErrors();
    ofOut << "[ct] cursor done" << std::endl; ofOut.flush();

    ofOut << "[ct] CreateTextures complete" << std::endl; ofOut.flush();
}

void DestroyTextures()
{
    cursor.Release();

    uiDepth.Release();
    uiRenderCheckSystem.Release();
    uiRenderCheck.Release();
    uiRenderMask.Release();
    uiRender.Release();

    for (int i = 0; i < 6; i++)
    {
        BackBuffer11[i].Release();
        BackBuffer[i].Release();

        DepthBuffer11[i].Release();
        DepthBuffer[i].Release();
    }    
}


bool CreateShaders(IDirect3DDevice9* devDX9)
{
    vsTexture.CompileShaderFromString(devDX9);
    psTexture.CompileShaderFromString(devDX9);
    psMouseDot.CompileShaderFromString(devDX9);
    psMask.CompileShaderFromString(devDX9);

    return true;
}

void DestroyShaders()
{
    vsTexture.Release();
    psTexture.Release();
    psMouseDot.Release();
    psMask.Release();
}


bool CreateBuffers(IDirect3DDevice9* devDX9, POINT textureSizeUI)
{
    curvedUI = RenderCurvedUI(devDX9);
    curvedUI.SetShadersLayout(vsTexture.Layout, vsTexture.VS, psMouseDot.PS);


    maskedUI = RenderMaskUI(devDX9);
    maskedUI.SetShadersLayout(vsTexture.Layout, vsTexture.VS, psMask.PS);

    cursorUI = RenderSquare(devDX9);
    cursorUI.SetShadersLayout(vsTexture.Layout, vsTexture.VS, psTexture.PS);



    
    


    //----
    // Back of hand squares
    //----
    return true;
}

void DestroyBuffers()
{
    curvedUI.Release();
    maskedUI.Release();
    cursorUI.Release();
}

// fix #21: called by the device wrapper around IDirect3DDevice9::Reset.
// Release all our default-pool D3D9 resources before the reset, recreate after.
void VR_PreReset()
{
    ofOut << "[hook] VR_PreReset: resourcesLive=" << g_vrResourcesLive << std::endl; ofOut.flush();
    if (g_vrResourcesLive)
    {
        extern CRITICAL_SECTION g_vrCS; extern bool g_vrCSInit; extern volatile LONG g_readyIndex;
        if (g_vrCSInit) EnterCriticalSection(&g_vrCS);
        g_readyIndex = -1;
        g_vrResourcesLive = false;
        DestroyTextures();
        DestroyBuffers();
        for (int i = 0; i < 6; i++) g_eyeTexFilled[i] = false;  // fix #23c: refill after recreate
        g_vrSuspended = true;
        if (g_vrCSInit) LeaveCriticalSection(&g_vrCS);
        ofOut << "[hook] VR_PreReset: textures+buffers released, VR suspended" << std::endl; ofOut.flush();
    }
}

void VR_PostReset(long hr)
{
    ofOut << "[hook] VR_PostReset: hr=0x" << std::hex << hr << std::dec << " suspended=" << g_vrSuspended << std::endl; ofOut.flush();
    if (g_vrSuspended && hr == 0)
    {
        CreateBuffers(devDX9, uiBufferSize);
        CreateTextures(devDX11.dev, devDX9, hmdBufferSize, uiBufferSize);
        g_vrResourcesLive = true;
        g_vrSuspended = false;
        ofOut << "[hook] VR_PostReset: buffers+textures recreated, VR resumed" << std::endl; ofOut.flush();
    }
}

//----
// TBC port fix #25: dedicated submit thread. Submitting from the game loop tied
// HMD updates to the GAME frame rate; when it dropped below 72fps in-world the
// runtime extrapolated frames ("motion smoothing"/ASW) and painted black bands
// during head turns (user-verified: no bands on the fast login screen, bands
// in-world). This thread waits on compositor cadence and re-submits the latest
// COMPLETE frame, so the compositor always has a fresh real frame.
// fix #55: SUPERSEDED — the thread is no longer started. Re-submitting the same
// stale frame at 72Hz AND calling WaitGetPoses at 72Hz broke the compositor's
// pose pairing (it assumes each Submit was rendered with the pose from the
// app's most recent WaitGetPoses). Submit/WaitGetPoses/SetFramePose now run
// once per GAME frame in the OnPaint hook (upstream order). The thread and
// StartVRSubmitThread stay compiled for easy rollback.
//----
CRITICAL_SECTION g_vrCS;
bool g_vrCSInit = false;
volatile LONG g_readyIndex = -1;     // last fully mirrored ring index (-1 = none)
volatile bool g_submitRun = false;
HANDLE g_submitThread = nullptr;

// fix #68: per-ring-slot render pose. The HMD pose that rendered a slot travels
// WITH the slot, so the 72Hz submit thread can attach it to that slot's Submit
// (Submit_TextureWithPose). The compositor then computes the correct warp delta
// (render pose -> current display pose) and head rotation stays smooth even
// though the game thread no longer blocks on WaitGetPoses. ALL of g_slotPose,
// g_slotPoseValid, g_pendingPose, g_pendingPoseValid and g_readyIndex are read
// and written only under g_vrCS.
static vr::HmdMatrix34_t g_slotPose[3];
static bool g_slotPoseValid[3] = { false, false, false };
// Pose captured in the OnPaint hook (the pose SetFramePose produced that this
// frame was rendered with). MirrorGameFrameToEyes copies it into g_slotPose[]
// for the slot it publishes.
static vr::HmdMatrix34_t g_pendingPose;
static bool g_pendingPoseValid = false;
// fix #68 rollback switch. false = 72Hz thread submits each slot WITH its render
// pose (fix #68). true = fix #55 behavior (Submit + WaitGetPoses on the game
// thread every frame — correct pose pairing but quantizes fps to 36/24).
static bool g_fix55_GameThreadSubmit = false;

// fix #73: one iteration's pose+submit body, extracted so it can run under SEH
// (the crash dumps 17:53/17:54 show the thread faulting inside this path during
// an instance loading screen - a resource-teardown race dereferenced a dead
// pointer). Called with g_vrCS HELD.
static void SubmitIterationBody()
{
    svr->SetFramePose();
    LONG idx = g_readyIndex;
    // fix #68: re-submit the latest complete slot at 72Hz, attaching the
    // pose the slot was actually rendered with (g_slotPose[idx]) via
    // Submit_TextureWithPose. The compositor uses that render pose to warp
    // to the current display pose, so head rotation stays smooth without
    // the game thread ever blocking on WaitGetPoses. If the slot has no
    // valid pose yet (e.g. mono mode, or before the first SetFramePose),
    // fall back to a plain Submit_Default so the eye still shows.
    if (idx >= 0)
    {
        // fix #73: explicit liveness checks - skip the frame if any texture
        // pointer is gone (teardown race during loading screens).
        if (!BackBuffer11[idx].pTexture || !BackBuffer11[idx + 3].pTexture) return;
        // A/B TEST (user-requested): g_fix68_PoseSubmit=false restores yesterday's
        // flawed mechanism (plain Submit, no render pose -> stepped rotation).
        if (g_fix68_PoseSubmit && g_slotPoseValid[idx])
            svr->Render(BackBuffer11[idx].pTexture, DepthBuffer11[idx].pTexture,
                        BackBuffer11[idx + 3].pTexture, DepthBuffer11[idx + 3].pTexture,
                        &g_slotPose[idx]);
        else
            svr->Render(BackBuffer11[idx].pTexture, DepthBuffer11[idx].pTexture,
                        BackBuffer11[idx + 3].pTexture, DepthBuffer11[idx + 3].pTexture);
    }
}

extern "C" int SEH_GuardedCall(void (*fn)(void));  // seh_helpers.cpp (fix #73)

static DWORD WINAPI VRSubmitThreadProc(LPVOID)
{
    ofOut << "[svrthread] submit thread started" << std::endl; ofOut.flush();
    static int s_sehTrips = 0;
    while (g_submitRun)
    {
        if (!(svr->isEnabled() && g_vrResourcesLive)) { Sleep(10); continue; }
        svr->WaitGetPoses();               // paces this loop to HMD cadence (72Hz)
        EnterCriticalSection(&g_vrCS);
        if (svr->isEnabled() && g_vrResourcesLive)
        {
            // fix #73: SEH net - a faulting iteration is logged and skipped
            // instead of crashing the whole game (ERROR #132 in d3d9.dll).
            if (!SEH_GuardedCall(SubmitIterationBody))
            {
                if (s_sehTrips < 20)
                {
                    s_sehTrips++;
                    ofOut << "[svrthread] EXCEPTION in submit iteration - skipped (" << s_sehTrips << ")" << std::endl; ofOut.flush();
                }
            }
        }
        LeaveCriticalSection(&g_vrCS);
    }
    ofOut << "[svrthread] submit thread stopped" << std::endl; ofOut.flush();
    return 0;
}

void StartVRSubmitThread()
{
    if (!g_vrCSInit) { InitializeCriticalSection(&g_vrCS); g_vrCSInit = true; }
    if (!g_submitThread)
    {
        g_submitRun = true;
        g_submitThread = CreateThread(NULL, 0, VRSubmitThreadProc, NULL, 0, NULL);
        ofOut << "[hook] StartVRSubmitThread: handle=" << (void*)g_submitThread << std::endl; ofOut.flush();
    }
}



XMMATRIX DxToGame(XMMATRIX matrix)
{
    return ((before * matrix) * after);
}

XMMATRIX GameToDx(XMMATRIX matrix)
{
    return ((after * matrix) * before);
}

XMMATRIX GetGameCamera(int camAddress, bool convert = false)
{
    if (camAddress)
    {
        XMMATRIX camMatrix = {
            *(float*)(camAddress + 0x14),
            *(float*)(camAddress + 0x18),
            *(float*)(camAddress + 0x1C),
            0.f,

            *(float*)(camAddress + 0x20),
            *(float*)(camAddress + 0x24),
            * (float*)(camAddress + 0x28),
            0.f,

            * (float*)(camAddress + 0x2C),
            * (float*)(camAddress + 0x30),
            * (float*)(camAddress + 0x34),
            0.f,

            * (float*)(camAddress + 0x08),
            * (float*)(camAddress + 0x0C),
            * (float*)(camAddress + 0x10),
            1.f
        };
        if (convert)
            return GameToDx(camMatrix);
        else
            return camMatrix;
    }
    else
        return XMMatrixIdentity();
}

void SetGameCamera(int camAddress, XMMATRIX camMatrix, bool convert = false)
{
    if (camAddress)
    {
        if (convert)
            camMatrix = DxToGame(camMatrix);

        *(float*)(camAddress + 0x08) = camMatrix(3, 0);
        *(float*)(camAddress + 0x0C) = camMatrix(3, 1);
        *(float*)(camAddress + 0x10) = camMatrix(3, 2);

        *(float*)(camAddress + 0x14) = camMatrix(0, 0);
        *(float*)(camAddress + 0x18) = camMatrix(0, 1);
        *(float*)(camAddress + 0x1C) = camMatrix(0, 2);
        *(float*)(camAddress + 0x20) = camMatrix(1, 0);
        *(float*)(camAddress + 0x24) = camMatrix(1, 1);
        *(float*)(camAddress + 0x28) = camMatrix(1, 2);
        *(float*)(camAddress + 0x2C) = camMatrix(2, 0);
        *(float*)(camAddress + 0x30) = camMatrix(2, 1);
        *(float*)(camAddress + 0x34) = camMatrix(2, 2);
    }
}

void fnUpdateCameraController(int camAddress)
{
    if (camAddress)
    {
        //*(float*)(camLoc + TBC_Camera::yaw) += hRotationOffset;
        // fix #28: was raw WotLK +0x120; TBC pitch is +0x108 (tbc_structures.h)
        *(float*)(camAddress + TBC_Camera::pitch) += vRotationOffset;
        //*(float*)(camLoc + 0x124) = 0;

        vRotationOffset = 0;
    }
}

// world_scale (S): apparent world size in VR. S<1 = miniature "toybox" world (you feel like
// a giant), S>1 = giant world. We scale the per-eye baseline (IPD) AND the head-tracking
// translation by 1/S TOGETHER (matched scaling avoids the "swimmy world" / nausea). The game
// camera, movement, FOV, culling and projection are untouched -> gameplay is identical.
static inline XMMATRIX ScaleTranslation(XMMATRIX m, float k)
{ m._41 *= k; m._42 *= k; m._43 *= k; return m; }

void fnUpdateCameraHMD(int camAddress)
{
    if (camAddress)
    {
        const float invS = (g_worldScale > 0.01f) ? (1.0f / g_worldScale) : 1.0f;
        cameraMatrixGame = GetGameCamera(camAddress, false);
        cameraMatrix = GameToDx(cameraMatrixGame);

        XMVECTOR angles = GetAngles(cameraMatrix);
        XMMATRIX horizonLockMatirx = XMMatrixRotationAxis({ 1, 0, 0, 0 }, angles.vector4_f32[0]);
        cameraMatrix = horizonLockMatirx * cameraMatrix;
        cameraMatrixGame = DxToGame(cameraMatrix);

        // fix #74B: capture the cyclopean (no eye-offset) camera position, in game
        // coords, for the nameplate depth-compression center reference. This is the
        // curEye==2 camera (matHMDPos*cameraMatrix) run through the same DxToGame the
        // engine applies via SetGameCamera, so its translation matches cam+0x8/0xC/0x10.
        if (g_centerCaptureArmed)
        {
            XMMATRIX centerGame = DxToGame(ScaleTranslation(matHMDPos, invS) * cameraMatrix);
            g_centerCamPosGame[0] = centerGame._41;
            g_centerCamPosGame[1] = centerGame._42;
            g_centerCamPosGame[2] = centerGame._43;
            g_centerCamValid = true;
            g_centerCaptureArmed = false;    // one capture per frame -> both eyes share this center
        }

        cameraMatrixIPD = XMMatrixIdentity();
        if (curEye == 0 || curEye == 1)
            cameraMatrixIPD = ScaleTranslation(matEyeOffset[curEye] * matHMDPos, invS);
        else
            cameraMatrixIPD = ScaleTranslation(matHMDPos, invS);
        cameraMatrixIPD *= cameraMatrix;

        SetGameCamera(camAddress, cameraMatrixIPD, true);

        // fix #43 REVERTED (2026-07-10): writing yaw/pitch fields made the engine
        // rebuild the camera from them, fighting the injected matrix — eyes
        // desynced (one high one low) and turning bled into vertical motion.
        // It did not fix the bottom terrain cut either. Terrain culling needs a
        // proper disasm investigation (separate mechanism), not guesses.

        // fix #38: upstream's camera+0x40 write DECODED — in 3.3.5 +0x40 is the
        // camera FOV used by CULLING; writing 2*PI disables culling entirely so
        // nothing pops when the head turns (render FOV comes from the injected
        // projection anyway). In TBC the fov field is +0x38 (2.4.3 CGCamera:
        // 0x8 pos, 0x14 matrix, 0x38 fov). Writing to +0x40 (a different TBC
        // field) caused the old catastrophic slowdowns — fix #29 removed it.
        // fix #41/#42: culling FOV written to the TBC camera fov field (+0x38).
        // 2*PI broke LOD (holes/few fps); 2.0 rad left a visible "ellipse" (the
        // culling cone) with sky/ground vanishing outside it — the Quest's
        // DIAGONAL fov is ~123deg. Live-tunable via vr/cull_fov.txt (radians,
        // 1.0..6.28, re-read every 2s), default 2.4 (~137deg).
        if (g_step2_InjectProjMatrix)
        {
            static float s_cullFov = 3.0f;  // fix #50: real fov now (was near-clip)
            static DWORD s_lastCullTick = 0;
            DWORD cn = GetTickCount();
            if (cn - s_lastCullTick > 2000)
            {
                s_lastCullTick = cn;
                FILE* cf = nullptr;
                if (fopen_s(&cf, "./vr_version/cull_fov.txt", "r") == 0 && cf)
                {
                    float v = 0;
                    if (fscanf_s(cf, "%f", &v) == 1 && v >= 1.0f && v <= 6.29f && v != s_cullFov)
                    {
                        s_cullFov = v;
                        ofOut << "[hook] cull_fov.txt -> " << v << " rad" << std::endl; ofOut.flush();
                    }
                    fclose(cf);
                }
            }
            // fix #50: AGENT FINDING — +0x38 is NEAR CLIP, not fov! Writing a big
            // value there pushed the near plane to metres away (holes, corrupted
            // depth). The real fov field is +0x40 (identical to WotLK; GetFov
            // clamps to 3.1241 rad ~179deg). +0x44 is aspect: writing 1.0 (square)
            // raises the VERTICAL fov share from 60% (4:3) to 70.7% -> less
            // top/bottom cut. cull_fov.txt now tunes actual fov (default 3.0).
            *(float*)(camAddress + 0x40) = s_cullFov;
            *(float*)(camAddress + 0x44) = 1.0f;
        }
    }
}


void UpdateCharacterAnimation_post(stObjectManager* playerObj)
{
    int cameraAddress = GetActiveCameraSafe();
    if (!cameraAddress) return;  // fix #15: no world/camera yet (login screen)
    if (!g_step2_BoneHacks) return;  // fix #19: body below writes bone matrices via UNVERIFIED TBC offsets (ptrBonePos)
    XMMATRIX camRaw = GetGameCamera(cameraAddress, false);

    if (!isPossessing && cameraAddress && *(float*)(cameraAddress + TBC_Camera::zoomLevel) == 0)  // fix #28: was raw WotLK +0x118
    {
        int headId = boneLookup.Get("Head");
        int headParentId = boneLookup.parentList[headId];
        XMMATRIX newHead = *(XMMATRIX*)(playerObj->pModelContainer->ptrBonePos + (0x40 * headParentId));
        *(XMMATRIX*)(playerObj->pModelContainer->ptrBonePos + (0x40 * headParentId)) = newHead * zeroScale;
        for (int childId : boneLookup.allChildren[headParentId])
            *(XMMATRIX*)(playerObj->pModelContainer->ptrBonePos + (0x40 * childId)) = newHead * zeroScale;
    }
}

int IntersectGround(RayTarget* rayTarget)
{
    rayTarget->targetIDA = 0;
    rayTarget->targetIDB = 0;
    rayTarget->intersectPoint = { 0, 0, 0 };
    rayTarget->intersectDepth = 0;
    rayTarget->intersectFrom = { 0, 0, 0 };
    rayTarget->intersectTo = { 0, 0, 0 };

    int worldFrame = SAFE_READ_INT(TBC_g_WorldFrame, 0);
    int worldPanel = SAFE_READ_INT(TBC_TODO_g_WorldPanel, 0);
    if (worldPanel && worldFrame)
    {
        float gvFOV = SAFE_READ_FLOAT(TBC_TODO_g_FOV_vertical, 0.785f);   // default ~45deg
        float ghFOV = SAFE_READ_FLOAT(TBC_TODO_g_FOV_horizontal, 1.047f); // default ~60deg
        float panelMouseCoordPercentX = *(float*)(worldPanel + 0x1224);
        float panelMouseCoordPercentY = *(float*)(worldPanel + 0x1228);
        float screenMouseCoordPercentX = gvFOV * panelMouseCoordPercentX;
        float screenMouseCoordPercentY = ghFOV * panelMouseCoordPercentY;

        Vector3 point1 = { 0, 0, 0 };
        Vector3 point2 = { 0, 0, 0 };
        bool intersect = RayIntersect(worldFrame, screenMouseCoordPercentX, screenMouseCoordPercentY, &point1, &point2);
        if (intersect)
        {
            rayTarget->intersectFrom = { point1.x, point1.y, point1.z };
            rayTarget->intersectTo = { point2.x, point2.y, point2.z };
            //int intersectType = WorldClickIntersect(worldFrame, &point1, &point2, 0x5C, rayTarget);
            int retData = WorldClickIntersect(worldFrame, &point1, &point2, 0, rayTarget);
            return (rayTarget->intersectDepth > 0) ? true : false;
        }
    }
    return 0;
}

// Mouse to world ray caluclations
void (*sub_4BF0F0)(float, float, Vector3*, Vector3*) = (void (*)(float, float, Vector3*, Vector3*))TBC_sub_MouseToWorldRay;
void (msub_4BF0F0)(float a, float b, Vector3* c, Vector3* d)
{
    MSUB_TRACE("MouseToWorldRay");
    XMMATRIX rayMatrix = XMMatrixIdentity();
    // world_scale: do NOT scale the gaze pick-ray. Targeting is by gaze DIRECTION (scale-
    // invariant); scaling the origin shifted the hit point differently per scale (user saw
    // the hotspot move between 0.1 and 0.7). Raw head pose keeps targeting consistent.
    if (cfg_disableControllers)
        rayMatrix = (matHMDPos * after);
    else
        rayMatrix = (matController[1] * after);
    XMVECTOR origin = { rayMatrix._41, rayMatrix._42, rayMatrix._43 };
    XMVECTOR frwd = { rayMatrix._31, rayMatrix._32, rayMatrix._33 };
    XMVECTOR norm = XMVector3Normalize(frwd);
    XMVECTOR end = origin + (norm * -1000.0f);

    origin = XMVector4Transform(origin, cameraMatrixGame);
    end = XMVector4Transform(end, cameraMatrixGame);

    c->x = origin.vector4_f32[0];
    c->y = origin.vector4_f32[1];
    c->z = origin.vector4_f32[2];

    d->x = end.vector4_f32[0];
    d->y = end.vector4_f32[1];
    d->z = end.vector4_f32[2];
}

// CGWorldFrame::TraceMouseRay — VR aim assist (multi-ray gaze targeting).
// The center ray is the real VR aim ray (built upstream in msub_4BF0F0). If it does
// not land on a unit, fan out a few angularly-offset rays in rings around it and take
// the first that hits a unit. Widens hover/target selection so head-aiming is forgiving.
// Disasm-verified 2.4.3 build 8606: thiscall, ecx=this, 4 stack args (origin, end,
// flags, out), ret 0x10. Pure query -> 0=nothing, 1=terrain/world, 2=object.
int (__thiscall* sub_4AEB10)(void*, void*, void*, unsigned, void*) =
    (int(__thiscall*)(void*, void*, void*, unsigned, void*))TBC_sub_TraceMouseRay;   // (this, origin, end, flags, out)
int __fastcall msub_4AEB10(void* thisWF, void* /*edx*/, void* origin, void* end, unsigned flags, void* out)
{
    // 1) center ray = the real VR aim ray (already built upstream).
    int r = sub_4AEB10(thisWF, origin, end, flags, out);
    if (r == 2) return 2;                                   // center already on a unit
    if (!g_aimAssist || g_aimRings <= 0) return r;

    float* o = (float*)origin;   // C3Vector
    float* e = (float*)end;
    float dir[3] = { e[0]-o[0], e[1]-o[1], e[2]-o[2] };
    float len = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    if (len < 1e-4f) return r;
    float inv = 1.0f/len; dir[0]*=inv; dir[1]*=inv; dir[2]*=inv;

    // perpendicular basis. WoW up = +Z; if dir is near-vertical use +Y.
    float up[3] = {0,0,1};
    if (fabsf(dir[2]) > 0.99f) { up[0]=0; up[1]=1; up[2]=0; }
    // u = normalize(cross(dir, up))
    float u[3] = { dir[1]*up[2]-dir[2]*up[1], dir[2]*up[0]-dir[0]*up[2], dir[0]*up[1]-dir[1]*up[0] };
    float ul = sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]); if (ul < 1e-4f) return r;
    u[0]/=ul; u[1]/=ul; u[2]/=ul;
    // v = cross(u, dir)  (unit)
    float v[3] = { u[1]*dir[2]-u[2]*dir[1], u[2]*dir[0]-u[0]*dir[2], u[0]*dir[1]-u[1]*dir[0] };

    const float DEG2RAD = 3.14159265f/180.0f;
    int rings = g_aimRings; if (rings > 4) rings = 4;
    int samples = g_aimSamples; if (samples < 3) samples = 3; if (samples > 16) samples = 16;
    unsigned pflags = flags & 0x18;                         // units + players only

    // Strict priority, exactly as intended: the center already missed (above). Now walk
    // rings from the INNERMOST outward. The FIRST ring that has ANY unit wins - wider
    // rings are never consulted after that. Within that winning ring we pick the NEAREST
    // unit (not the first one by angle), which is what stops it from grabbing a far unit
    // off to the side when a closer one is right there.
    unsigned char probe[0x30];
    for (int ring = 1; ring <= rings; ++ring) {
        float tanT = tanf(g_aimSpreadDeg * ring * DEG2RAD);
        bool  found = false;
        float bestDist = 3.0e38f;
        float bestEnd[3] = { 0.0f, 0.0f, 0.0f };
        for (int s = 0; s < samples; ++s) {
            float phi = 6.2831853f * s / samples;
            float cf = cosf(phi), sf = sinf(phi);
            float d2[3] = {
                dir[0] + tanT*(u[0]*cf + v[0]*sf),
                dir[1] + tanT*(u[1]*cf + v[1]*sf),
                dir[2] + tanT*(u[2]*cf + v[2]*sf) };
            float d2l = sqrtf(d2[0]*d2[0]+d2[1]*d2[1]+d2[2]*d2[2]);
            if (d2l < 1e-4f) continue;
            float di = 1.0f/d2l; d2[0]*=di; d2[1]*=di; d2[2]*=di;
            float end2[3] = { o[0]+d2[0]*len, o[1]+d2[1]*len, o[2]+d2[2]*len };
            int ri = sub_4AEB10(thisWF, origin, end2, pflags, probe);
            if (ri == 2) {
                // approx hit position = first 3 floats of the trace output; keep the
                // nearest one in this ring. If that layout guess is off it only reshuffles
                // which of the ring's hits we pick - correctness is guaranteed because the
                // winner is re-traced straight into the real `out` below.
                float* hp = (float*)probe;
                float dx = hp[0]-o[0], dy = hp[1]-o[1], dz = hp[2]-o[2];
                float hd = dx*dx + dy*dy + dz*dz;
                if (hd < bestDist) {
                    bestDist = hd;
                    bestEnd[0] = end2[0]; bestEnd[1] = end2[1]; bestEnd[2] = end2[2];
                    found = true;
                }
            }
        }
        if (found) {
            // Re-run the winning ray straight into the caller's buffer so the engine gets
            // a COMPLETE, correct hit result (no partial 0x18 copy over stale center data).
            sub_4AEB10(thisWF, origin, bestEnd, pflags, out);
            return 2;
        }
    }
    return r;                                               // nothing found -> keep center result
}

// SetClientMouseResetPoint
void (*sub_869DB0)() = (void(*)())TBC_sub_SetClientMouseResetPoint;
void (msub_869DB0)()
{
    MSUB_TRACE("SetClientMouseResetPoint");
    sub_869DB0();
}

// Calculate Window Size
void (*sub_684D70)(int, int, int) = (void (*)(int, int, int))TBC_sub_CalculateWindowSize;
void (msub_684D70)(int a, int b, int c)
{
    MSUB_TRACE("CalcWindowSize");
    //if(doLog) logError << "CalcWindowSize Pre : " << *(int*)(c + 0x0) << " : " << *(int*)(c + 0x4) << " : " << *(int*)(c + 0x8) << " : " << *(int*)(c + 0xC) << std::endl;

    RECT sizePre = { *(int*)(c + 0x0), *(int*)(c + 0x4), *(int*)(c + 0x8), *(int*)(c + 0xC) };

    sub_684D70(a, b, c);
    
    *(int*)(c + 0x0) = 0;
    *(int*)(c + 0x4) = 0;
    *(int*)(c + 0x8) = *(int*)(c + 0x0) + sizePre.right;
    *(int*)(c + 0xc) = *(int*)(c + 0x4) + sizePre.bottom;

    //if (doLog) logError << "CalcWindowSize Post : " << *(int*)(c + 0x0) << " : " << *(int*)(c + 0x4) << " : " << *(int*)(c + 0x8) << " : " << *(int*)(c + 0xC) << std::endl;
}

//----
// Create Window
//----
void(msub_6A08D0_pre)()
{
    // TBC port fix #11: upstream called svr->PreloadVR() here (WotLK had no live
    // VR session yet at window creation). PreloadVR does VR_Init + VR_Shutdown —
    // and since OUR session is already live from DllMain, that VR_Shutdown KILLED
    // the scene-app registration ~1s after start (vrclient log: "VR_Init a second
    // time ... VR_Shutdown called"). SteamVR Direct Mode then broke the game's
    // device creation -> "no 3D accelerator" -> game exits. This was the missing
    // half of the 2026-04-30 regression. Query the LIVE session instead.
    ofOut << "[hook] msub_6A08D0_pre (CreateWindow): NOT calling PreloadVR (would VR_Shutdown live session)" << std::endl; ofOut.flush();
    if (svr->isEnabled()) {
        bool ok = svr->RefreshBufferSizeFromLive();
        ofOut << "[hook]   RefreshBufferSizeFromLive -> " << (ok ? "OK" : "FAILED") << std::endl; ofOut.flush();
    }

    hmdBufferSize = svr->GetBufferSize();
    ofOut << "[hook]   hmdBufferSize=" << hmdBufferSize.x << "x" << hmdBufferSize.y << std::endl; ofOut.flush();
    if (hmdBufferSize.x == 0) {
        hmdBufferSize.x = 1844;
        hmdBufferSize.y = 1844;
    }

    // ui_sharpness / world_sharpness live in vr_config.cfg but are read HERE (startup),
    // not in the ~2s live loop: the render textures are sized once below
    // (CreateBuffers/CreateTextures) and never rebuilt, so these two only take effect on
    // a game RESTART. Same "key = value" / '#'-comment format as the live parser.
    {
        std::ifstream mcfg("./vr_version/vr_config.cfg");
        std::string mline;
        while (std::getline(mcfg, mline))
        {
            size_t h = mline.find('#');
            if (h != std::string::npos) mline.erase(h);
            for (size_t i = 0; i < mline.size(); ++i)
                if (mline[i] == '=') mline[i] = ' ';
            std::istringstream mss(mline);
            std::string mkey;
            if (!(mss >> mkey)) continue;
            int mv;
            if (mkey == "ui_sharpness" && (mss >> mv) && mv >= 1 && mv <= 3) {
                cfg_uiMultiplier = mv;
                ofOut << "[cfg] ui_sharpness = " << mv << " (startup)" << std::endl; ofOut.flush();
            }
            else if (mkey == "world_sharpness" && (mss >> mv) && mv >= 1 && mv <= 3) {
                cfg_gameMultiplier = mv;
                ofOut << "[cfg] world_sharpness = " << mv << " (startup)" << std::endl; ofOut.flush();
            }
            else if (mkey == "screen_curve") {
                float cv;
                if ((mss >> cv) && cv >= -3.0f && cv <= 3.0f) {
                    g_screenCurve = cv;
                    ofOut << "[cfg] screen_curve = " << cv << " (startup)" << std::endl; ofOut.flush();
                }
            }
        }
    }

    hmdBufferSize.x *= cfg_gameMultiplier;
    hmdBufferSize.y *= cfg_gameMultiplier;

    //if (doLog) logError << "CreateWindow Pre : " << *(float*)((int)ecx + 0x16C) << " : " << *(float*)((int)ecx + 0x170) << std::endl;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
}

void(msub_6A08D0_post)(void* ecx)
{
    // TBC port fix #12: upstream wrote wtfSize into the engine object at WotLK
    // offsets +0x16C/+0x170/+0x17C/+0x180 and read the HWND from +0x3968.
    // In TBC those offsets belong to DIFFERENT fields — we were corrupting the
    // engine's own state right before it creates the D3D9 device (suspected
    // cause of "no 3D accelerator" with no CreateDevice call ever made).
    // Disabled: log-only until the real TBC offsets are found via Ghidra.
    ofOut << "[hook] msub_6A08D0_post (CreateWindow post): SKIPPING WotLK struct writes (+0x16C..+0x180) and HWND@+0x3968 resize" << std::endl; ofOut.flush();
}

// Create Window
void(__thiscall* sub_68EBB0)(void*, int) = (void(__thiscall*)(void*, int))TBC_sub_CreateWindow;
void(__fastcall msub_68EBB0)(void* ecx, void* edx, int a)
{
    msub_6A08D0_pre();
    sub_68EBB0(ecx, a);
    msub_6A08D0_post(ecx);
}

void(__thiscall* sub_6A08D0)(void*, int) = (void(__thiscall*)(void*, int))TBC_TODO_sub_CreateWindowEx;
void(__fastcall msub_6A08D0)(void* ecx, void* edx, int a)
{
    msub_6A08D0_pre();
    sub_6A08D0(ecx, a);
    msub_6A08D0_post(ecx);
}

//----
// Create DX Device
//----
void msub_6A2040_pre(int a)
{
    ofOut << "[hook] msub_6A2040_pre (CreateDxDev) ecx_arg=0x" << std::hex << a << std::dec << std::endl; ofOut.flush();
    if (a) {
        // TBC diag: dump the first 12 dwords of the arg struct to locate the real
        // width/height fields in the TBC layout (WotLK had them at +0x14/+0x18).
        for (int i = 0; i <= 0x2C; i += 4) {
            ofOut << "[hook]   arg+0x" << std::hex << i << " = 0x" << *(unsigned int*)(a + i) << std::dec << " (" << *(int*)(a + i) << ")" << std::endl;
        }
        ofOut.flush();
    }
    if (a) {
        // TBC port fix #13: in TBC the CreateDevice arg struct has width@+0x10,
        // height@+0x14 (verified by the dword dump above: 1024 / 768 with
        // gxResolution 1024x768). WotLK had them at +0x14/+0x18.
        wtfSize.x = *(int*)(a + 0x10);
        wtfSize.y = *(int*)(a + 0x14);
        ofOut << "[hook]   raw wtfSize from a+0x10/+0x14 = " << wtfSize.x << "x" << wtfSize.y << std::endl; ofOut.flush();
        // Sanity-clamp stays as a fallback.
        if (wtfSize.x < 320 || wtfSize.x > 7680 || wtfSize.y < 240 || wtfSize.y > 4320) {
            ofOut << "[hook]   wtfSize looks bogus, falling back to 1280x720" << std::endl; ofOut.flush();
            wtfSize.x = 1280;
            wtfSize.y = 720;
        }
    } else {
        wtfSize.x = 1280;
        wtfSize.y = 720;
    }
}

void msub_6A2040_post(void* ecx, bool* retVal)
{
    ofOut << "[hook] msub_6A2040_post ecx=0x" << std::hex << (DWORD)ecx << std::dec << std::endl; ofOut.flush();
    // TBC port fix: VR was already initialized in DllMain (proxydll.cpp::InitInstance).
    // Calling StartVR() again here triggers Recenter() on a live session and
    // crashes inside vrclient.dll.
    // TBC port fix #8: do NOT retry StartVR() here either. VR_Init inside the
    // CreateDxDevice hook blocks indefinitely when SteamVR/HMD isn't ready
    // (verified: 2026-07-07 run hung on exactly this call — log ends here, no
    // crash dump). If the DllMain pre-init failed, run this session without VR.
    if (!svr->isEnabled()) {
        ofOut << "[hook]   svr not connected from DllMain — NOT retrying StartVR here (it deadlocks mid-CreateDevice). Running without VR this session." << std::endl; ofOut.flush();
    } else {
        ofOut << "[hook]   svr already connected from DllMain, skipping re-init" << std::endl; ofOut.flush();
    }
    ofOut << "[hook]   isEnabled=" << svr->isEnabled() << std::endl; ofOut.flush();
    if (svr->HasErrors())
        logError << svr->GetErrors();

    if (svr->isEnabled())
    {
        //----
        // TBC port: WotLK offsets +0x397C / +0x3968 / +0x3B3C in CGxDevice are
        // wrong in TBC. Use D3D9 API (version-stable) to get device/HWND/buffers.
        //----
        extern cIDirect3DDevice9Ex* glIDirect3DDevice9Ex;
        extern cIDirect3DDevice9*   glIDirect3DDevice9;

        IDirect3DDevice9* dev = glIDirect3DDevice9Ex
            ? (IDirect3DDevice9*)glIDirect3DDevice9Ex
            : (IDirect3DDevice9*)glIDirect3DDevice9;

        if (!dev) {
            // TBC port fix #10: the SEH memory-scan fallback is GONE — it crashed
            // the game twice (2026-04-30 13:07 and 2026-07-09 22:49, ACCESS_VIOLATION
            // inside d3d9.dll while scanning). A NULL proxy device means the game
            // escaped our proxy (QueryInterface leak — fixed as #9 in cIDirect3D9.cpp).
            // If it ever happens again: run flat, don't scan, don't crash.
            ofOut << "[hook]   no proxy device (game bypassed the proxy?!) - aborting VR setup, running flat" << std::endl; ofOut.flush();
            return;
        }
        devDX9 = dev;

        D3DDEVICE_CREATION_PARAMETERS dcp = {};
        if (dev->GetCreationParameters(&dcp) == S_OK) {
            screenLayout.hwnd = dcp.hFocusWindow;
        }
        screenLayout.width  = wtfSize.x;
        screenLayout.height = wtfSize.y;
        screenLayout.haveLayout = true;
        ofOut << "[hook]   HWND=0x" << std::hex << (DWORD)screenLayout.hwnd
              << " size=" << std::dec << screenLayout.width << "x" << screenLayout.height << std::endl; ofOut.flush();

        // config.txt removed 2026-07-12 - cfg_* values are baked constants (see top of file)

        //----
        // Save original back/depth buffers via D3D9 API
        //----
        IDirect3DSurface9* bb = nullptr;
        IDirect3DSurface9* db = nullptr;
        HRESULT hbb = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        HRESULT hdb = dev->GetDepthStencilSurface(&db);
        if (FAILED(hbb) || !bb) {
            ofOut << "[hook]   GetBackBuffer failed (hr=0x" << std::hex << hbb << std::dec << ") - aborting VR setup" << std::endl; ofOut.flush();
            return;
        }
        origBackBuffer  = (int)bb;
        origDepthBuffer = (int)db;
        // fix #35: curBackBufferLoc = CGxDeviceD3d current-RT/depth slot pair,
        // TBC +0x3A08/+0x3A0C (3.3.5: +0x3B3C/+0x3B40), disasm-verified. The
        // stereo StartRender writes the per-eye surfaces through it.
        curBackBufferLoc = *(int*)(TBC_g_GxDevicePtr) + 0x3A08;
        ofOut << "[hook]   curBackBufferLoc = gxDev+0x3A08 = 0x" << std::hex << curBackBufferLoc << std::dec << std::endl; ofOut.flush();

        D3DSURFACE_DESC bDesc;
        bb->GetDesc(&bDesc);
        ofOut << "[hook]   BackBuffer " << bDesc.Width << "x" << bDesc.Height
              << " fmt=" << bDesc.Format << std::endl; ofOut.flush();
        //logError << "BackBuffer:" << std::endl;
        //logError << bDesc.Width << "x" << bDesc.Height << std::endl;
        //logError << "f:" << bDesc.Format << " : p:" << bDesc.Pool << " : t:" << bDesc.Type << " : u:" << bDesc.Usage << std::endl;

        if (origDepthBuffer) {
            ((IDirect3DSurface9*)origDepthBuffer)->GetDesc(&bDesc);
            ofOut << "[hook]   DepthBuffer " << bDesc.Width << "x" << bDesc.Height << " fmt=" << bDesc.Format << std::endl; ofOut.flush();
        } else {
            ofOut << "[hook]   no depth buffer (skipping desc)" << std::endl; ofOut.flush();
        }

        // TBC port: skip D3D11/VR buffer setup for now to verify game can load.
        // CreateShaders/CreateBuffers/CreateTextures depend on hmdBufferSize from
        // simpleVR which may not be properly populated in TBC layout. Re-enable
        // once we confirm the game reaches login screen with VR registered.
        // STEP 1A: D3D11 device init (verified OK)
        ofOut << "[hook]   [step1a] devDX11.createDevice() ..." << std::endl; ofOut.flush();
        devDX11.createDevice();
        std::string dx11errs = devDX11.GetErrors();
        ofOut << "[hook]   [step1a] devDX11.createDevice returned, errs='" << dx11errs.c_str() << "'" << std::endl; ofOut.flush();
        logError << dx11errs;

        // STEP 1B: compile embedded HLSL shaders (verified OK)
        ofOut << "[hook]   [step1b] CreateShaders(devDX9) ..." << std::endl; ofOut.flush();
        CreateShaders(devDX9);
        ofOut << "[hook]   [step1b] CreateShaders returned" << std::endl; ofOut.flush();

        // STEP 1C: build UI render geometry (verified OK)
        uiBufferSize = { screenLayout.width * cfg_uiMultiplier, screenLayout.height * cfg_uiMultiplier };
        ofOut << "[hook]   [step1c] uiBufferSize=" << uiBufferSize.x << "x" << uiBufferSize.y << " (screen " << screenLayout.width << "x" << screenLayout.height << " * uiMultiplier " << cfg_uiMultiplier << ")" << std::endl; ofOut.flush();
        ofOut << "[hook]   [step1c] CreateBuffers(devDX9, uiBufferSize) ..." << std::endl; ofOut.flush();
        CreateBuffers(devDX9, uiBufferSize);
        ofOut << "[hook]   [step1c] CreateBuffers returned" << std::endl; ofOut.flush();

        // STEP 1D: shared D3D9/D3D11 textures for VR submit.
        //
        // History of attempts to get hmdBufferSize at this point in TBC's flow:
        //   (a) Call svr->PreloadVR() — DEADLOCKS. PreloadVR does VR_Init+VR_Shutdown
        //       and a second VR_Init on a live scene-app freezes WoW (OpenVR issue #1719).
        //   (b) Call openVRSession->GetRecommendedRenderTargetSize on the cached pointer
        //       from DllMain's StartVR — CRASHES inside vrclient.dll with NULL deref.
        //       Likely a vrserver-side state isn't ready for query inside the D3D9
        //       CreateDevice hook (SteamVR Direct Mode is mid-handshake right here).
        //
        // Resolution: skip dynamic query entirely. Use a sensible fallback HMD per-eye
        // size. Real HMD size can be refreshed later (e.g. before first frame submit)
        // when SteamVR Direct Mode handshake is complete.
        //
        // CreateTextures allocates 24 shared D3D9/D3D11 textures (6 BackBuffer11
        // + 6 BackBuffer + 6 DepthBuffer11 + 6 DepthBuffer) at this size. In a 32-bit
        // process every MB matters — 1832x1920 fails with ntdll heap NULL deref
        // @ 0x77A07B71 (verified 00:55 crash). Try 1024x1024 to confirm OOM
        // hypothesis. If this works, we'll later resize to native HMD per-eye
        // before first VR submit when Direct Mode handshake is complete.
        // fix #33: with LAA (4GB) we can afford native-HMD-size eye textures.
        // 1024x1024 was an OOM guard from the 2GB era; the mirrored 1024x768
        // game image was DOWNSCALED into ~563x407 and then blown up by the
        // compositor — visibly blurry vs the desktop. Use the live HMD size
        // (RefreshBufferSizeFromLive filled it in the CreateWindow hook).
        POINT liveSize = svr->GetBufferSize();
        if (liveSize.x >= 512 && liveSize.x <= 4096 && liveSize.y >= 512 && liveSize.y <= 4096)
        {
            // fix #44: 75% of native — stereo renders each eye at this size and
            // full native (2212x2448 x2) starves the GPU: game fps << 72Hz and
            // head rotation judders (rotation is baked at GAME fps).
            hmdBufferSize.x = (LONG)(liveSize.x * 0.75f);
            hmdBufferSize.y = (LONG)(liveSize.y * 0.75f);
        }
        else
        {
            hmdBufferSize.x = 1024;
            hmdBufferSize.y = 1024;
        }
        ofOut << "[hook]   [step1d] hmdBufferSize fallback=" << hmdBufferSize.x << "x" << hmdBufferSize.y << " uiBufferSize=" << uiBufferSize.x << "x" << uiBufferSize.y << " (cfg_gameMultiplier=" << cfg_gameMultiplier << " ignored in fallback)" << std::endl; ofOut.flush();
        ofOut << "[hook]   [step1d] CreateTextures(devDX11.dev, devDX9, hmdBufferSize, uiBufferSize) ..." << std::endl; ofOut.flush();
        CreateTextures(devDX11.dev, devDX9, hmdBufferSize, uiBufferSize);
        ofOut << "[hook]   [step1d] CreateTextures returned" << std::endl; ofOut.flush();
        g_vrResourcesLive = true;  // fix #21: Reset handler may now release/recreate these
        // fix #68: thread restored, now submits WITH render pose. The 72Hz thread
        // is back (it decouples HMD refresh from game fps again), but each Submit
        // now carries the slot's true render pose (Submit_TextureWithPose), so the
        // compositor reprojects correctly and head rotation stays smooth — the
        // problem fix #55 solved by moving Submit onto the game thread (which
        // quantized fps to 36/24). StartVRSubmitThread also inits the CS.
        if (!g_vrCSInit) { InitializeCriticalSection(&g_vrCS); g_vrCSInit = true; }
        StartVRSubmitThread();  // fix #25 thread + fix #68 pose-attached submit
        svr->monoSubmit = !g_step2_StereoStartRender;  // fix #35: stereo renders per-eye asymmetric -> full bounds

        // TBC port: WotLK NOP patches at 0x97044C/0x97044D ("delete epic code")
        // are NOT valid for 2.4.3 — that VA is unrelated data in TBC binary.
        // Writing NOP there CORRUPTS HEAP and causes random crashes in ntdll
        // (RtlGetNtGlobalFlags etc.) much later. Disabled.
        ofOut << "[hook]   SKIPPING WotLK NOP patches at 0x97044C/D (would corrupt TBC heap)" << std::endl; ofOut.flush();

        isRunningAsAdmin = IsElevated();

        // TBC port: skip controller action manifest registration for now —
        // setActiveJSON crashes because vr::VRInput() is a per-translation-unit
        // singleton that hasn't been resolved in this .cpp yet (VR_Init was in
        // simpleVR.cpp, this is game_extras.cpp+steamVR.cpp). User plays with
        // keyboard+mouse anyway. Re-enable later once we wire up lazy resolve.
        ofOut << "[hook]   SKIPPING setActiveJSON / setActionHandlesGame (no VR controllers in this build)" << std::endl; ofOut.flush();
        /*
        if (*retVal) { *retVal = setActiveJSON(g_VR_PATH + "actions.json"); }
        if (*retVal) { *retVal = setActionHandlesGame(&input); }
        */
    }
    ofOut << "[hook] msub_6A2040_post DONE" << std::endl; ofOut.flush();
}


bool(__thiscall* sub_6904D0)(void*, int) = (bool(__thiscall*)(void*, int))TBC_sub_CreateDxDevice;
bool(__fastcall msub_6904D0)(void* ecx, void* edx, int a)
{
    HOOK_LOG("CreateDxDev_legacy_enter");
    msub_6A2040_pre(a);
    bool retVal = sub_6904D0(ecx, a);
    HOOK_LOG("CreateDxDev_legacy_orig_returned");
    msub_6A2040_post(ecx, &retVal);
    HOOK_LOG("CreateDxDev_legacy_exit");
    return retVal;
}

// Create DX Device
bool(__thiscall* sub_6A2040)(void*, int) = (bool(__thiscall*)(void*, int))TBC_TODO_sub_CreateDxDeviceEx;
bool(__fastcall msub_6A2040)(void* ecx, void* edx, int a)
{
    msub_6A2040_pre(a);
    bool retVal = sub_6A2040(ecx, a);
    msub_6A2040_post(ecx, &retVal);
    return retVal;
}
//----
// Close DX Device
//----
void msub_6A1F40_pre()
{
    // fix #24b: the engine calls CloseDxDevice during NORMAL STARTUP too (tearing
    // down a probe device before creating the real one) — upstream's StopVR()
    // here killed the fresh VR session 0.7s in (vrclient: VR_Shutdown; HMD fell
    // back to Home). Only release OUR graphics resources, and only if they exist;
    // NEVER stop the VR session — the next CreateDxDevice re-runs Step 1 on it.
    ofOut << "[hook] CloseDxDevice_pre: resourcesLive=" << g_vrResourcesLive << std::endl; ofOut.flush();
    if (svr->isEnabled() && g_vrResourcesLive)
    {
        if (g_vrCSInit) EnterCriticalSection(&g_vrCS);
        g_readyIndex = -1;
        g_vrResourcesLive = false;
        DestroyTextures();
        DestroyBuffers();
        DestroyShaders();
        devDX11.Release();
        for (int i = 0; i < 6; i++) g_eyeTexFilled[i] = false;
        if (g_vrCSInit) LeaveCriticalSection(&g_vrCS);
        ofOut << "[hook] CloseDxDevice_pre: VR resources released (session kept alive)" << std::endl; ofOut.flush();
    }
}

void msub_6A1F40_post()
{
    //if (doLog) logError << "-- Close DX Device" << std::endl;
}

void(__thiscall* sub_6903B0)(void*) = (void(__thiscall*)(void*))TBC_sub_CloseDxDevice;
void(__fastcall msub_6903B0)(void* ecx, void* edx)
{
    msub_6A1F40_pre();
    sub_6903B0(ecx);
    msub_6A1F40_post();
}

void(__thiscall* sub_6A1F40)(void*) = (void(__thiscall*)(void*))TBC_TODO_sub_CloseDxDeviceEx;
void(__fastcall msub_6A1F40)(void* ecx, void* edx)
{
    msub_6A1F40_pre();
    sub_6A1F40(ecx);
    msub_6A1F40_post();
}

// Begin SceneSetup
void(__thiscall* sub_6A73E0)(void*) = (void(__thiscall*)(void*))TBC_sub_BeginSceneSetup;
void(__fastcall msub_6A73E0)(void* ecx, void* edx)
{
    MSUB_TRACE("BeginSceneSetup");
	sub_6A73E0(ecx);
}

// End SceneSetup
void(__thiscall* sub_6A7540)(void*) = (void(__thiscall*)(void*))TBC_sub_EndSceneSetup;
void(__fastcall msub_6A7540)(void* ecx, void* edx)
{
    MSUB_TRACE("EndSceneSetup");
	sub_6A7540(ecx);
}

// fix #23b: mirror the game frame into next-submit eye textures HERE, at
// Present time, when the frame is COMPLETE by definition. Copying at the end
// of the OnPaint hook caught a partially rendered frame (the engine issues
// more draw batches after OnPaint) — flickering black bands in the lower half.
void MirrorGameFrameToEyes()
{
    if (!(svr->isEnabled() && g_vrResourcesLive && devDX9)) return;

    // fix #35: in stereo mode the eyes were already rendered by StartRender —
    // no mono copy. Just publish the finished ring slot for the submit thread.
    if (g_step2_StereoStartRender)
    {
        if (g_vrCSInit) EnterCriticalSection(&g_vrCS);
        // fix #35b: companion view — in stereo the engine no longer draws to the
        // real back buffer (monitor went black). Show the left eye on the monitor.
        IDirect3DSurface9* bb = nullptr;
        if (devDX9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb) == S_OK && bb)
        {
            devDX9->StretchRect(BackBuffer[textureIndex].pShaderResource, NULL, bb, NULL, D3DTEXF_LINEAR);
            bb->Release();
        }
        // fix #68: the pose that rendered this slot (captured in OnPaint) travels
        // with the slot so the submit thread attaches it to this slot's Submit.
        g_slotPose[textureIndex] = g_pendingPose;
        g_slotPoseValid[textureIndex] = g_pendingPoseValid;
        g_readyIndex = textureIndex;
        textureIndex = ((textureIndex + 1) % 3);
        if (g_vrCSInit) LeaveCriticalSection(&g_vrCS);
        return;
    }

    // fix #26: cap mirror rate to ~72Hz. Uncapped (300+fps at login) the game
    // hammered the shared surfaces with copies while the compositor held
    // references to recently submitted slots — driver sync stalls spiralled
    // down to 1-8 fps on an RTX 5060 Ti. More than HMD rate is wasted anyway.
    static DWORD s_lastMirrorTick = 0;
    DWORD now = GetTickCount();
    if (now - s_lastMirrorTick < 12) return;
    s_lastMirrorTick = now;

    IDirect3DSurface9* pBackBuffer = nullptr;
    devDX9->GetBackBuffer(NULL, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
    if (!pBackBuffer) return;

    // fix #30: live-tunable screen size. vr/mono_screen.txt holds one number
    // (0.2 .. 1.0 = fraction of the union FOV width), re-read every 2s so the
    // user can resize the virtual screen WHILE playing. Default 0.55.
    static float s_monoScale = 0.55f;
    static DWORD s_lastCfgTick = 0;
    DWORD cfgNow = GetTickCount();
    if (cfgNow - s_lastCfgTick > 2000)
    {
        s_lastCfgTick = cfgNow;
        FILE* f = nullptr;
        if (fopen_s(&f, "./vr_version/mono_screen.txt", "r") == 0 && f)
        {
            float v = 0;
            if (fscanf_s(f, "%f", &v) == 1 && v >= 0.2f && v <= 1.0f && v != s_monoScale)
            {
                s_monoScale = v;
                for (int i = 0; i < 6; i++) g_eyeTexFilled[i] = false;  // refill borders for new rect
                ofOut << "[hook] mono_screen.txt -> scale=" << v << std::endl; ofOut.flush();
            }
            fclose(f);
        }
    }
    const float MONO_SCALE = s_monoScale;
    RECT dst;
    float tx = svr->unionTanX, ty = svr->unionTanY;
    // fix #25b: NO full-texture fallback. The first Present ran before the submit
    // thread computed unionTan; a full-screen copy landed in ring slot 0 and its
    // border survived the fill-once optimization — flickering image fragments
    // around the letterbox every ~3rd frame. Until the tans are known, skip.
    if (!(tx > 0 && ty > 0)) { pBackBuffer->Release(); return; }
    {
        // fix #34: use the REAL game aspect from the back buffer instead of a
        // hardcoded 3/4 — with gxResolution 1280x720 the screen still showed 4:3.
        float gameAspectHW = 0.75f;
        D3DSURFACE_DESC bbd;
        if (pBackBuffer->GetDesc(&bbd) == S_OK && bbd.Width > 0)
            gameAspectHW = (float)bbd.Height / (float)bbd.Width;
        float wFrac = MONO_SCALE;
        float hFrac = MONO_SCALE * gameAspectHW * (tx / ty);
        if (hFrac > 1.0f) hFrac = 1.0f;
        LONG w = (LONG)(hmdBufferSize.x * wFrac);
        LONG h = (LONG)(hmdBufferSize.y * hFrac);
        dst.left = (hmdBufferSize.x - w) / 2;  dst.top = (hmdBufferSize.y - h) / 2;
        dst.right = dst.left + w;              dst.bottom = dst.top + h;
    }

    // fix #23c: ColorFill ONLY ONCE per texture. The compositor re-reads the
    // last submitted texture at 72Hz while the game runs slower; with a ring of
    // 3 the per-frame ColorFill repainted a texture the compositor was actively
    // displaying -> the flickering black bands WERE our own black brush caught
    // mid-stroke (worse when fps dropped: turning, heavy areas). The letterbox
    // border never changes, so fill it once and only copy the game rect per frame.
    if (g_vrCSInit) EnterCriticalSection(&g_vrCS);
    IDirect3DSurface9* eyeSurf[2] = { BackBuffer[textureIndex].pShaderResource, BackBuffer[textureIndex + 3].pShaderResource };
    int eyeIdx[2] = { textureIndex, textureIndex + 3 };
    HRESULT hrl = S_OK, hrr = S_OK;
    for (int e = 0; e < 2; e++)
    {
        if (!g_eyeTexFilled[eyeIdx[e]])
        {
            devDX9->ColorFill(eyeSurf[e], NULL, D3DCOLOR_XRGB(0, 0, 0));
            g_eyeTexFilled[eyeIdx[e]] = true;
        }
        HRESULT hr = devDX9->StretchRect(pBackBuffer, NULL, eyeSurf[e], &dst, D3DTEXF_LINEAR);
        if (e == 0) hrl = hr; else hrr = hr;
    }
    pBackBuffer->Release();
    // fix #23: kick the command buffer so copies land before next frame's Submit
    static IDirect3DQuery9* s_flushQ = nullptr;
    if (!s_flushQ) devDX9->CreateQuery(D3DQUERYTYPE_EVENT, &s_flushQ);
    if (s_flushQ) { s_flushQ->Issue(D3DISSUE_END); s_flushQ->GetData(NULL, 0, D3DGETDATA_FLUSH); }
    // fix #68: attach the render pose (captured in OnPaint) to the published slot.
    g_slotPose[textureIndex] = g_pendingPose;
    g_slotPoseValid[textureIndex] = g_pendingPoseValid;
    // fix #25: publish this slot for the submit thread, move to the next one.
    g_readyIndex = textureIndex;
    textureIndex = ((textureIndex + 1) % 3);
    if (g_vrCSInit) LeaveCriticalSection(&g_vrCS);
    static int mHits = 0;
    if (mHits++ < 3) { ofOut << "[hook] mirror(at-Present) gameBB->eyes[" << g_readyIndex << "] dst=(" << dst.left << "," << dst.top << "," << dst.right << "," << dst.bottom << ") hrL=0x" << std::hex << hrl << " hrR=0x" << hrr << std::dec << std::endl; ofOut.flush(); }
}

// Present Scene
void(__thiscall* sub_6A7610)(void*) = (void(__thiscall*)(void*))TBC_sub_PresentScene;
void(__fastcall msub_6A7610)(void* ecx, void* edx)
{
    MSUB_TRACE("PresentScene");
    MirrorGameFrameToEyes();  // fix #23b: frame is complete right before Present
    // Render:
    // End Scene
    // Begin Scene - Mouse
    // End Scene   - Mouse
    // Begin Scene
    sub_6A7610(ecx);
}

// fix #64-instr: save one D3D9 render-target surface to a 24-bit BMP file, so we can
// visually inspect what pixels an eye texture actually contains. It copies the GPU
// render target down into a system-memory surface (GetRenderTargetData), reads the
// pixels (LockRect) and writes a bottom-up 24-bit BMP. The source is assumed 32-bit
// BGRA (D3DFMT_A8R8G8B8 — how the eye textures are created). Any D3D failure is logged
// with its HRESULT and skipped; it never throws or crashes the game. When the surface is
// wider than 2048 px it downsamples by 2 so the files stay small.
static void DumpSurfaceToBMP(IDirect3DSurface9* surf, const char* path)
{
    if (!surf || !devDX9) { ofOut << "[dump] null surface/device for " << path << std::endl; ofOut.flush(); return; }

    D3DSURFACE_DESC desc;
    HRESULT hr = surf->GetDesc(&desc);
    if (FAILED(hr)) { ofOut << "[dump] GetDesc 0x" << std::hex << hr << std::dec << " " << path << std::endl; ofOut.flush(); return; }

    IDirect3DSurface9* sysSurf = nullptr;
    hr = devDX9->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sysSurf, NULL);
    if (FAILED(hr) || !sysSurf) { ofOut << "[dump] CreateOffscreenPlainSurface 0x" << std::hex << hr << std::dec << " " << path << std::endl; ofOut.flush(); return; }

    hr = devDX9->GetRenderTargetData(surf, sysSurf);
    if (FAILED(hr)) { ofOut << "[dump] GetRenderTargetData 0x" << std::hex << hr << std::dec << " " << path << std::endl; ofOut.flush(); sysSurf->Release(); return; }

    D3DLOCKED_RECT lr;
    hr = sysSurf->LockRect(&lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) { ofOut << "[dump] LockRect 0x" << std::hex << hr << std::dec << " " << path << std::endl; ofOut.flush(); sysSurf->Release(); return; }

    int step = (desc.Width > 2048) ? 2 : 1;
    int outW = (int)desc.Width / step;
    int outH = (int)desc.Height / step;
    int rowStride = ((outW * 3) + 3) & ~3;  // BMP rows padded to a 4-byte boundary

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
    {
        ofOut << "[dump] fopen failed " << path << std::endl; ofOut.flush();
        sysSurf->UnlockRect(); sysSurf->Release(); return;
    }

    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    bfh.bfType    = 0x4D42;  // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize    = bfh.bfOffBits + rowStride * outH;
    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = outW;
    bih.biHeight      = outH;   // positive height = bottom-up rows
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage   = rowStride * outH;
    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);

    std::vector<unsigned char> row(rowStride, 0);
    const unsigned char* base = (const unsigned char*)lr.pBits;
    for (int y = 0; y < outH; y++)
    {
        int srcY = (outH - 1 - y) * step;                        // bottom-up
        const unsigned char* srcRow = base + (size_t)srcY * lr.Pitch;
        for (int x = 0; x < outW; x++)
        {
            const unsigned char* px = srcRow + (size_t)(x * step) * 4;  // 32-bit BGRA
            row[x * 3 + 0] = px[0];  // B
            row[x * 3 + 1] = px[1];  // G
            row[x * 3 + 2] = px[2];  // R
        }
        fwrite(row.data(), rowStride, 1, f);
    }

    fclose(f);
    sysSurf->UnlockRect();
    sysSurf->Release();
    ofOut << "[dump] wrote " << path << " (" << outW << "x" << outH << ")" << std::endl; ofOut.flush();
}







// Should Render Char
void(__thiscall* sub_6E0840)(void*, int, int, int) = (void(__thiscall*)(void*, int, int, int))TBC_sub_ShouldRenderChar;
void(__fastcall msub_6E0840)(void* ecx_, void* edx_, int a, int b, int c)
{
    MSUB_TRACE("ShouldRenderChar");
    // fix #19: alpha4 is one of the 5 UNVERIFIED TBC struct offsets — writing
    // 255 through it corrupts the character object in-world. Gated.
    if (!g_step2_BoneHacks) {
        sub_6E0840(ecx_, a, b, c);
        return;
    }
    int showHidePlayer = 0;
    if (curEye == 0 || curEye == 1) {
        showHidePlayer = 1;
    }
    ((stObjectManager*)ecx_)->alpha4 = 255;

    int cameraAddress = GetActiveCameraSafe();
    if (cameraAddress)
    {
        float zoomLevel = *(float*)(cameraAddress + TBC_Camera::zoomLevel);  // 3.3.5: +0x118, TBC: +0x100
        if (zoomLevel == 0 && showHidePlayer == 1 && !cfg_showBodyFPS)
            SAFE_WRITE_DWORD(TBC_TODO_g_HidePlayerFlag, FALSE);
        else
            SAFE_WRITE_DWORD(TBC_TODO_g_HidePlayerFlag, TRUE);
    }
    sub_6E0840(ecx_, a, b, c);
}

// Update Freelook Camera
void(__thiscall* sub_5FF530)(void*) = (void(__thiscall*)(void*))TBC_sub_UpdateFreelookCamera;
void(__fastcall msub_5FF530)(void* ecx, void* edx)
{
    MSUB_TRACE("UpdateFreelookCamera");
    if (g_camHMDInject) fnUpdateCameraController((int)ecx);
    sub_5FF530(ecx);
}

// Update Camera Fn
void(__thiscall* sub_606F90)(void*, int, int) = (void(__thiscall*)(void*, int, int))TBC_sub_UpdateCameraFn;
void(__fastcall msub_606F90)(void* ecx, void* edx, int a, int b)
{
    MSUB_TRACE("UpdateCameraFn");
    g_fix56_camHits++;  // fix #56 temp instrumentation
    sub_606F90(ecx, a, b);
    if (g_camHMDInject) fnUpdateCameraHMD((int)ecx);
}

// Slows animation value (frame timing?)
void (*sub_77EFF0)(int, float) = (void (*)(int, float))TBC_sub_SlowsAnimation;
void (msub_77EFF0)(int a, float b)
{
    MSUB_TRACE("SlowsAnim");
    // fix #69: 3 world traversals with warm-up active - animations advance per traversal.
    // Upstream halved dt for TWO world passes per frame; with the warm-up pass there are
    // three, so scale by 1/3 while it is active, otherwise keep the original /2.
    b = b * (g_fix67_WarmupPass ? (1.0f / 3.0f) : 0.5f);
    sub_77EFF0(a, b);
}

// Dynamic model animations
void(__thiscall* sub_82F0F0)(void*, int, int, int, int, int) = (void(__thiscall*)(void*, int, int, int, int, int))TBC_sub_DynamicModelAnimations;
void(__fastcall msub_82F0F0)(void* ecx, void* edx, int a, int b, int c, int d, int e)
{
    MSUB_TRACE("DynamicAnim");
    // fix #72 REVERTED: skipping animation during warm-up made characters vanish -
    // the engine animates each model ONCE per frame at its first traversal encounter
    // (= during the warm-up), so skipping there skips it for the whole frame.
    sub_82F0F0(ecx, a, b, c, d, e);

    if (gPlayerObj && gPlayerObj->pModelContainer == ecx)
        UpdateCharacterAnimation_post(gPlayerObj);
}

// Update Model Proj
void(__thiscall* sub_6A9B40)(void*, int) = (void(__thiscall*)(void*, int))TBC_sub_UpdateModelProj;
void(__fastcall msub_6A9B40)(void* ecx, void* edx, int a)
{
    MSUB_TRACE("UpdateModelProj");
    // fix #16: memcpy of the projection matrix to GxDevicePtr+0xFC8 uses a WotLK
    // offset — in TBC it overwrote pointers with 1.0f floats (23:18 crash). Gated.
    if (svr->isEnabled() && g_step2_InjectProjMatrix)
    {
        sub_6A9B40(ecx, a);

        // fix #32 (STEP B of stereo): TBC stores the projection matrix at
        // this+0xF0C — verified by disasm of 0x5AD8E0 (lea ecx,[ebx+0xF0C];
        // call matrix-copy). 3.3.5 had +0xFC8. Use the hook's own `this`
        // instead of the global for robustness.
        int projMatrixAddr = (int)ecx + 0xF4C;  // fix #35: FINAL proj matrix slot (0xF0C was the INPUT copy; 3.3.5 final: +0xFC8)

        if (curEye == 0 || curEye == 1) {
            if (*(float*)(projMatrixAddr + 0x3C) == 0) {
                XMMATRIX matProj = matProjection[curEye];
                //matProj._33 = uiOffsetD;// -0.938f;
                //matProj._34 = -0.06f;
                matProj._31 *= -1; matProj._32 *= -1; matProj._33 *= -1; matProj._34 *= -1;
                matProj._43 = *(float*)(projMatrixAddr + 0x38);

                memcpy((void*)projMatrixAddr, &matProj._11, 64);
                devDX9->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&matProj._11);

                // fix #74 (option A): also write the per-eye projection into the
                // INPUT slot at this+0xF0C. CGWorldFrame caches viewStackTop x
                // proj@+0xF0C (0x4AFA82 -> WF+0x3F8) and WorldToScreen (0x4AC810)
                // projects unit names / combat text / nameplates through it. Until
                // now +0xF0C held the engine's mono fov-3.0/aspect-1.0 matrix, so
                // text disparity never matched the world -> unfusable double vision.
                // WorldToScreen uses only the x/y/w rows, so the same matProj is
                // correct here. Risk (culling consumers of +0xF0C) -> flag-gated.
                if (g_fix74_TextProjFix)
                    memcpy((void*)((int)ecx + 0xF0C), &matProj._11, 64);
            }
        }
    }
    else
    {
        sub_6A9B40(ecx, a);
    }
}

// fix #74 option B helper: row-major 4x4 multiply, out = A * B (row-vector
// convention: v' = v * A * B). Matches the engine's WF+0x3F8 = view * proj.
static inline void Mat4Mul_rowmajor(const float* A, const float* B, float* out)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += A[i * 4 + k] * B[k * 4 + j];
            out[i * 4 + j] = s;
        }
}

// WorldToScreen (0x4AC810) — fix #74 option B. `ecx` IS the CGWorldFrame (WF).
// WorldToScreen projects a world point through the cached view*proj at WF+0x3F8.
// The engine rebuilt that cache this pass from the MONO proj@+0xF0C (fov-3.0/
// aspect-1.0), so text disparity never matched the per-eye world -> double vision.
// We swap WF+0x3F8 to view*perEyeProj for the call only, then restore. +0xF0C is
// never touched, so the cull path stays mono and characters stay visible.
int(__thiscall* sub_4AC810)(void*, float*, float*, int) =
    (int(__thiscall*)(void*, float*, float*, int))TBC_sub_WorldToScreen;
int(__fastcall msub_4AC810)(void* ecx, void* edx, float* worldPos, float* outXY, int flag)
{
    if (!(g_fix74b_TextProjDetour && svr->isEnabled() && g_vrResourcesLive
          && g_step2_InjectProjMatrix && (curEye == 0 || curEye == 1))) {
        // Engine's own W2S (e.g. the per-frame plate walk outside the eye passes).
        // Depth is eye-independent, so stash it here too, with the exact cache the
        // engine used for this call (WF+0x3F8 = current view*proj).
        int r0 = sub_4AC810(ecx, worldPos, outXY, flag);
        if (g_nameplateOcclusion)
            StashW2SDepth(worldPos, (const float*)((char*)ecx + 0x3F8), r0);
        return r0;
    }

    int* gxDev = *(int**)0xD2A15C;            // CGxDevice singleton
    if (!gxDev)
        return sub_4AC810(ecx, worldPos, outXY, flag);

    int vsIdx = *(int*)((char*)gxDev + 0x1A7C);   // view-stack top index
    if (vsIdx < 0 || vsIdx > 63)
        return sub_4AC810(ecx, worldPos, outXY, flag);

    const float* view = (const float*)((char*)gxDev + 0x1A84 + 64 * vsIdx);
    const float* proj = (const float*)((char*)gxDev + 0xF4C);   // final per-eye proj (as rendered this pass)

    float* cache = (float*)((char*)ecx + 0x3F8);  // WF+0x3F8 = cached view*proj
    float savedCache[16];
    memcpy(savedCache, cache, 64);                // engine's mono view*proj (for cull/dirty-compare)

    // ALWAYS use the FULL per-eye projection (this eye's rendered projection). The
    // projection's off-center term is large and matches the world; leaving it intact
    // is what keeps the label from ever inverting relative to the world.
    float perEyeCache[16];
    Mat4Mul_rowmajor(view, proj, perEyeCache);
    // WORLD-SPACE NAMEPLATES: capture this eye's view/proj HERE. This exact pair
    // (stack-top view x CGxDevice+0xF4C proj) projected real plate world positions
    // to verified-healthy depths (0.98-0.999), i.e. THE VIEW HERE HAS WORLD
    // TRANSLATION — unlike the CM2Scene-entry view, which is camera-relative
    // (translation 0) and useless for world coordinates. curEye is 0/1 here.
    memcpy(g_wsView[curEye], view, 64);
    memcpy(g_wsProj[curEye], proj, 64);
    g_wsMtxValid[curEye] = true;
    ++g_dbgMtxCap;
    g_dbgLastT2 = view[12]*view[12] + view[13]*view[13] + view[14]*view[14];

    // Snapshot the first call's cache once per frame for the flat-consistent mode.
    if (!g_flatValid) { memcpy(g_flatCache, perEyeCache, 64); g_flatValid = true; }

    memcpy(cache, perEyeCache, 64);

    float k = g_nameplateDepthScale;
    int camObj = *(int*)((char*)ecx + 0x732C);    // WF+0x732C = active camera object
    // WORLD-SPACE NAMEPLATES: capture this eye's camera WORLD position too (before
    // any of the branches below temporarily modify it). The captured view has zero
    // translation (camera-relative rendering), so the plate draw needs the camera
    // position to relativize the units' absolute world coordinates.
    if (camObj) {
        memcpy(g_wsCamPos[curEye], (const void*)(camObj + 0x08), 12);
        g_wsCamValid[curEye] = true;
    }

    // Full depth (k>=1), center not ready, or no camera -> plain per-eye (option B).
    if (k >= 0.999f || !g_centerCamValid || !camObj) {
        int r = sub_4AC810(ecx, worldPos, outXY, flag);
        memcpy(cache, savedCache, 64);
        if (g_nameplateOcclusion) StashW2SDepth(worldPos, perEyeCache, r);
        return r;
    }

    float* camPos0 = (float*)(camObj + 0x08);
    float savedCamPos0[3];
    memcpy(savedCamPos0, camPos0, 12);

    // FLAT-CONSISTENT mode (k<=0): give BOTH eyes an IDENTICAL nameplate position
    // (shared flat cache + shared center camera). The engine's per-eye overlap
    // stacking then gets identical inputs in both eyes -> identical layout -> the
    // labels fuse (flat, at one depth) and can never cross-eye. This is the cure for
    // the per-eye stacking divergence that depth-scaling cannot touch.
    if (k <= 0.001f) {
        memcpy(cache, g_flatCache, 64);
        camPos0[0] = g_centerCamPosGame[0];
        camPos0[1] = g_centerCamPosGame[1];
        camPos0[2] = g_centerCamPosGame[2];
        int r = sub_4AC810(ecx, worldPos, outXY, flag);
        memcpy(camPos0, savedCamPos0, 12);
        memcpy(cache, savedCache, 64);
        // constant per-eye X correction: right eye only (labels share one projection,
        // so the right eye is offset from its world by a constant; cancel it here).
        if (r && curEye == 1) {
            static int s_flatLog = 0;
            if ((s_flatLog++ % 400) == 0) { ofOut << "[flatX] R outXY.x pre=" << (outXY[0]-g_flatXShift) << " post=" << outXY[0] << " shift=" << g_flatXShift << std::endl; ofOut.flush(); }
            outXY[0] += g_flatXShift;
        }
        if (g_nameplateOcclusion) StashW2SDepth(worldPos, perEyeCache, r); // TRUE depth even in flat mode
        return r;
    }

    // DEPTH COMPRESSION (correct method): the nameplate disparity is
    //   D = k*(f*baseline/Z)  +  Δc
    // where f*baseline/Z is the depth-dependent parallax and Δc is the large,
    // constant frustum-asymmetry (off-center lens) term. Δc must be kept intact
    // (it matches the world); we only scale the parallax by moving the CAMERA
    // toward the cyclopean center. So: keep the per-eye projection (above), and
    // interpolate ONLY the camera position:
    //   camPos_k = center + k*(perEyeCam - center)
    // k=1 -> per-eye (full depth), k->0 -> center (labels toward infinity). Single
    // call, cannot invert, monotone in k. (Reuses camPos0/savedCamPos0 from above.)
    float camPosK[3];
    for (int i = 0; i < 3; ++i)
        camPosK[i] = g_centerCamPosGame[i] + k * (savedCamPos0[i] - g_centerCamPosGame[i]);
    memcpy(camPos0, camPosK, 12);

    int r = sub_4AC810(ecx, worldPos, outXY, flag);

    memcpy(camPos0, savedCamPos0, 12);             // restore the per-eye camera position
    memcpy(cache, savedCache, 64);                 // restore engine mono cache
    if (g_nameplateOcclusion) StashW2SDepth(worldPos, perEyeCache, r); // TRUE depth, not compressed
    return r;
}

// Unit-text projection committer (0x6110F0), verified __thiscall / ret 8, single
// unit-text caller 0x615134. Runs right after the W2S call for the same node, in
// every walk (it fires even when its internal dead-band skips the position write).
// Pair the stashed W2S depth with the node pointer (unit+0x1130 — runtime-proven to
// equal the plate frame msub_433000 sees) using UPDATE-IN-PLACE: one entry per node,
// its depth refreshed by every walk, never reset. No eye gate — depth is
// eye-independent and the walk may run outside our eye passes (curEye==2).
void(__thiscall* sub_6110F0)(void*, float*, void*) =
    (void(__thiscall*)(void*, float*, void*))0x006110F0;
void __fastcall msub_6110F0(void* ecx, void* edx, float* xy, void* wf)
{
    bool w2sValid = g_lastW2S.valid; float w2sD = g_lastW2S.d;
    sub_6110F0(ecx, xy, wf);
    ++g_dbgCommitFires;
    if (w2sValid) {
        __try {
            void* node = ecx ? *(void**)((char*)ecx + 0x1130) : NULL;
            if (node) {
                int found = -1;
                for (int i = 0; i < g_plateDepthCount; ++i)
                    if (g_plateDepth[i].node == node) { found = i; break; }
                if (found >= 0)
                    g_plateDepth[found].d = w2sD;
                else if (g_plateDepthCount < PLATE_DEPTH_MAX) {
                    g_plateDepth[g_plateDepthCount].node = node;
                    g_plateDepth[g_plateDepthCount].d = w2sD;
                    ++g_plateDepthCount;
                }
                ++g_dbgPaired;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Fail-soft: a stale/destroyed unit simply gets no depth entry.
        }
    }
    g_lastW2S.valid = false;
}

// fix #74c Hook A: bracket the nameplate apply loop (0x611260). Verified __cdecl with
// 2 stack args (epilogue `mov esp,ebp; pop ebp; ret`; caller 0x4AE38C does `add esp,8`).
// arg0 = the worldframe/plate-list container, arg1 = 0 (unused by the body). Single caller,
// nameplate-exclusive, runs once per eye pass. We set g_inPlateApply around the original so
// only the plates' SetPoint calls are intercepted, and clear the per-frame layout table at
// the START of the left pass (curEye==0) so the right pass can replay it.
void(__cdecl* sub_611260)(void*, int) = (void(__cdecl*)(void*, int))0x00611260;
void __cdecl msub_611260(void* worldframe, int a1)
{
    if (g_fix74c_CopyLayout && curEye == 0) {
        g_plateLayoutCount = 0;                    // fresh layout table for this frame
        g_plateOcclIncomplete[0] = g_plateOcclIncomplete[1] = false;
    }
    int _applyEye = curEye;
    g_inPlateApply = true;
    sub_611260(worldframe, a1);
    g_inPlateApply = false;
    g_occlApplyFires++;
    if (_applyEye == 0) g_occlApplyEye0++;
    g_occlCountAfterApply = g_plateLayoutCount;
}

// fix #74c Hook B: intercept CLayoutFrame::SetPoint (0x433000). Verified __thiscall,
// ret 0x18 (6 stack dword args after ecx): (anchorPoint, relFrame, relativePoint, float x,
// float y, one). x/y are the FINAL post-declutter screen position. This is a hot general
// UI function, so the very first thing we do is a single bool check and pass straight
// through unless we're inside the nameplate apply loop. In-loop: plate = ecx - 0x14, and
// the CURRENT eye's raw pre-declutter projection is at plate+0x38C (x) / +0x390 (y).
//   left  (curEye==0): record {finalX=x, finalY=y, rawAx, rawAy}, call original unchanged.
//   right (curEye==1): reuse the left final pos + (this-eye raw - left raw) as the disparity.
//   right plate not seen on the left, or any other eye: call original unchanged.
int(__thiscall* sub_433000)(void*, int, void*, int, float, float, int) =
    (int(__thiscall*)(void*, int, void*, int, float, float, int))0x00433000;
int __fastcall msub_433000(void* ecx, void* edx, int anchorPoint, void* relFrame,
                           int relativePoint, float x, float y, int one)
{
    // FAST PASS-THROUGH for all non-nameplate UI SetPoints (single bool test).
    if (!(g_fix74c_CopyLayout && g_inPlateApply))
        return sub_433000(ecx, anchorPoint, relFrame, relativePoint, x, y, one);

    char* plate = (char*)ecx - TBC_SIMPLEFRAME_LAYOUT_OFFSET;
    float ax = 0.0f, ay = 0.0f;
    __try {
        // The apply loop walks ONE list holding both nameplate and combat-text nodes,
        // and SetPoint also fires for child frames. Only true nameplate nodes may enter
        // the layout/occlusion tables — everything else keeps the engine's SetPoint.
        if (*(uintptr_t*)plate != TBC_NAMEPLATE_NODE_VTABLE) {
            ++g_dbgVtRej;
            return sub_433000(ecx, anchorPoint, relFrame, relativePoint, x, y, one);
        }
        ax = *(float*)(plate + 0x38C);             // this eye's raw projected x
        ay = *(float*)(plate + 0x390);             // this eye's raw projected y
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Fail-soft: keep the engine's SetPoint when the plate vanished mid-apply.
        return sub_433000(ecx, anchorPoint, relFrame, relativePoint, x, y, one);
    }
    const float ys = g_nameplateYShift;            // uniform vertical lift (both eyes)
    // SOURCE-LEVEL HIDE (world-space redesign): shift the plate's layout rect far
    // off-screen at SetPoint time. The engine's layout resolver + corner updater
    // then propagate the off-screen rect into EVERY downstream batch representation
    // (the flush-time corner displacement provably missed the one that reaches the
    // screen). Applies in every pass — the visible plates render in the j==2 UI
    // pass. Reverts instantly when the toggle goes off (plates re-SetPoint every
    // frame in VR). Accepted v1 cost: the plate's CLICK rect moves with it.
    const float hideY = g_nameplateOcclusion ? -100000.0f : 0.0f;
    if (g_nameplateOcclusion) ++g_dbgHiddenElems;

    if (curEye == 0) {                             // LEFT: record the decluttered layout
        if (g_plateLayoutCount < PLATE_MAX) {
            PlateRec& r = g_plateLayout[g_plateLayoutCount++];
            r.plate = plate; r.finalX = x; r.finalY = y; r.rawAx = ax; r.rawAy = ay;
            r.depthValid[0] = r.depthValid[1] = false;
            r.eyeSeen[0] = r.eyeSeen[1] = false;
            r.layoutDepthApplied[0] = r.layoutDepthApplied[1] = false;
            RecordPlateOcclusionResult(r, ecx, 0, plate); // writes depth before SetPoint
        } else if (g_nameplateOcclusion) g_plateOcclIncomplete[0] = true;
        return sub_433000(ecx, anchorPoint, relFrame, relativePoint, x, y + ys + hideY, one);
    }

    if (curEye == 1) {                             // RIGHT: replay left layout + disparity
        for (int i = 0; i < g_plateLayoutCount; ++i) {
            if (g_plateLayout[i].plate == plate) {
                ++g_plateOcclFrameMatched;
                float nx = g_plateLayout[i].finalX + (ax - g_plateLayout[i].rawAx);
                float ny = g_plateLayout[i].finalY + (ay - g_plateLayout[i].rawAy);
                RecordPlateOcclusionResult(g_plateLayout[i], ecx, 1, plate);
                return sub_433000(ecx, anchorPoint, relFrame, relativePoint, nx, ny + ys + hideY, one);
            }
        }
        // Plate appeared only in the right pass: keep its engine position, but still
        // give it a right-eye depth so enabling Z for the shared list is safe.
        if (g_plateLayoutCount < PLATE_MAX) {
            PlateRec& r = g_plateLayout[g_plateLayoutCount++];
            r.plate = plate; r.finalX = x; r.finalY = y; r.rawAx = ax; r.rawAy = ay;
            r.depthValid[0] = r.depthValid[1] = false;
            r.eyeSeen[0] = r.eyeSeen[1] = false;
            r.layoutDepthApplied[0] = r.layoutDepthApplied[1] = false;
            RecordPlateOcclusionResult(r, ecx, 1, plate);
        } else if (g_nameplateOcclusion) g_plateOcclIncomplete[1] = true;
        return sub_433000(ecx, anchorPoint, relFrame, relativePoint, x, y + ys + hideY, one);
    }

    // curEye == 2 (UI) or anything else: position unchanged, but the hide shift
    // still applies — the engine lays plates out in this pass too.
    return sub_433000(ecx, anchorPoint, relFrame, relativePoint, x, y + hideY, one);
}

// fix #75: apply the correct highlight tint to a unit's model, chosen from the unit's
// live highlight BITS (unit+0xC0) with TARGET PRIORITY. A single unit can be both targeted
// and moused-over at once, but the model carries only ONE ambient-add color; SetHighlight
// fires once per bit and the last write wins, so we must derive the color from the actual
// bit state after the engine updates it, not from the single triggering bit. bit0 (value 1)
// = target, bit1 (value 2) = mouseover (matches the SetHighlight arg: bit==0 -> 1<<0,
// bit==1 -> 1<<1). Called at the END of BOTH SetHighlight and ClearHighlight, after the
// engine's original has updated unit+0xC0. It also owns the fullbright diffuse multiplier
// (+0x1A0/1A4/1A8): 2.0 while highlighted, reset to 1.0 when no bits remain. The model
// ambient it writes is the same "is this highlighted / which type" signal the per-batch arm
// hook (sub_70D150) reads, so per-type brightness follows automatically.
static void ApplyHighlightColor(void* unit)
{
    if (!unit) return;
    void* model = *(void**)((char*)unit + 0xE8);
    if (!model) model = *(void**)((char*)unit + 0xE4);
    if (!model) return;
    int bits = *(int*)((char*)unit + 0xC0);
    const volatile float* tint = nullptr;
    if (bits & 1) tint = g_hlTargetColor;        // TARGET priority
    else if (bits & 2) tint = g_hlMouseColor;    // mouseover only
    if (tint) {
        *(float*)((char*)model + 0x1AC) = tint[0];
        *(float*)((char*)model + 0x1B0) = tint[1];
        *(float*)((char*)model + 0x1B4) = tint[2];
        *(float*)((char*)model + 0x1A0) = 2.0f;   // fullbright base
        *(float*)((char*)model + 0x1A4) = 2.0f;
        *(float*)((char*)model + 0x1A8) = 2.0f;
    } else {
        *(float*)((char*)model + 0x1AC) = 0.0f;
        *(float*)((char*)model + 0x1B0) = 0.0f;
        *(float*)((char*)model + 0x1B4) = 0.0f;
        *(float*)((char*)model + 0x1A0) = 1.0f;
        *(float*)((char*)model + 0x1A4) = 1.0f;
        *(float*)((char*)model + 0x1A8) = 1.0f;
    }
}

// fix #75: CGUnit_C::SetHighlight (0x625450) — __thiscall(unit, int bit): bit 1 =
// mouseover, bit 0 = target. Engine writes a dim, time-of-day color into the unit's model
// ambient add (model+0x1AC/1B0/1B4). We call the original, then re-derive and overwrite the
// tint from the unit's full bit state (ApplyHighlightColor) so a target that is also being
// moused-over keeps its target color instead of being clobbered by the mouseover write.
void(__thiscall* sub_625450)(void*, int) = (void(__thiscall*)(void*, int))0x00625450;
void __fastcall msub_625450(void* ecx, void* edx, int bit)
{
    sub_625450(ecx, bit);                              // engine sets the bit + its dim color
    if (!g_fix75_Highlight || !ecx) return;
    ApplyHighlightColor(ecx);                          // re-derive tint from unit+0xC0 (target priority)
}

// fix #75: CGUnit_C::ClearHighlight (0x6253E0) — __thiscall(unit, int bit), ret 4
// (disasm-verified: mov edx,ecx / mov ecx,[ebp+8] / and [edx+0xC0],~(1<<bit) / ret 4).
// After the engine clears the bit, re-derive the tint from the remaining bits: if the unit
// is still highlighted by another source it takes that color, otherwise ApplyHighlightColor
// zeroes the ambient and resets the diffuse multiplier back to 1.0.
void(__thiscall* sub_6253E0)(void*, int) = (void(__thiscall*)(void*, int))0x006253E0;
void __fastcall msub_6253E0(void* ecx, void* edx, int bit)
{
    sub_6253E0(ecx, bit);                              // engine clears the bit + its ambient
    if (!g_fix75_Highlight || !ecx) return;
    ApplyHighlightColor(ecx);                          // re-derive tint from remaining unit+0xC0 bits
}

// fix #75: per-model-batch pre-draw ARM. sub_70D150 (__thiscall, ecx = batch-render ctx,
// no stack args) runs right before every model batch's DrawIndexedPrimitive calls, on the
// same thread, synchronously. The batch's current model is *(void**)(ctx + 0x3300). We set
// g_hlBoost = 1 iff that model carries a non-zero ambient-add (model+0x1AC/1B0/1B4) — which
// is true only for the moused-over / targeted unit (SetHighlight wrote the tint there;
// normal models default to 0.0 and ClearHighlight zeroes it). g_hlBoost is refreshed every
// batch, so it tracks exactly which batch is the highlighted one. Then call the original.
void(__thiscall* sub_70D150)(void*) = (void(__thiscall*)(void*))0x0070D150;
void __fastcall msub_70D150(void* ecx, void* edx)
{
    if (g_fix75_Highlight && ecx)
    {
        void* model = *(void**)((char*)ecx + 0x3300);
        float ar = model ? *(float*)((char*)model + 0x1AC) : 0.0f;
        float ag = model ? *(float*)((char*)model + 0x1B0) : 0.0f;
        float ab = model ? *(float*)((char*)model + 0x1B4) : 0.0f;
        if (model && (ar != 0.0f || ag != 0.0f || ab != 0.0f))
        {
            // Highlighted batch. Capture the model's own ambient tint for the proxy's
            // COLORED additive glow (Change 1), and pick the pass count by highlight type
            // (Change 2): match the ambient EXACTLY against the two type colors — reliable
            // because SetHighlight copies the global verbatim into the model ambient.
            g_hlBoostColor[0] = ar; g_hlBoostColor[1] = ag; g_hlBoostColor[2] = ab;
            float bright;
            if (ar == g_hlMouseColor[0] && ag == g_hlMouseColor[1] && ab == g_hlMouseColor[2])
                bright = g_hlMouseBright;                 // mouseover
            else if (ar == g_hlTargetColor[0] && ag == g_hlTargetColor[1] && ab == g_hlTargetColor[2])
                bright = g_hlTargetBright;                // target
            else
                bright = g_hlMouseBright;                 // non-zero but no match (e.g. both bits) -> mouseover
            int passes = (int)(bright + 0.5f) - 1;
            if (passes < 1) passes = 1;
            if (passes > 5) passes = 5;
            g_hlBoostPasses = passes;
            g_hlBoost = 1;
        }
        else g_hlBoost = 0;
    }
    sub_70D150(ecx);
}

// fix #75: DISARM bracket. sub_711550 = CM2Scene::Draw — disasm-verified
// int __thiscall(CM2Scene* this, int phase), sets eax=1, ret 4 (epilogue at 0x7115D3).
// Clear g_hlBoost at entry and exit so the additive boost can never leak onto terrain,
// WMO or UI draws that happen after the last model batch of the scene.
int(__thiscall* sub_711550)(void*, int) = (int(__thiscall*)(void*, int))0x00711550;
int __fastcall msub_711550(void* ecx, void* edx, int phase)
{
    g_hlBoost = 0;
    // (World-space plate matrix capture removed from here 2026-07-16: the view on
    // the stack at CM2Scene entry is CAMERA-RELATIVE — zero translation (runtime:
    // lastT2=0 always) — so it cannot transform world-coordinate positions. The
    // capture lives in msub_4AC810's VR branch, whose view*proj pair was validated
    // by the healthy plate depths.)
    int r = sub_711550(ecx, phase);
    g_hlBoost = 0;
    return r;
}

// The game draws its OWN software cursor (the WoW hand) via RenderMouse. In VR it
// leaks into a single eye's render target as a faint "ghost" cursor, because it is
// drawn while a per-eye render target is bound. We draw our own VR cursor
// (cursorUI / pointer.png) already, so suppress the game's. Set false to restore
// the game cursor (e.g. for debugging).
volatile bool g_suppressGameCursor = true;   // suppress the game's own cursor (ghost in one eye). Confirmed NOT the nameplate cause.

// Nameplate depth-occlusion. WoW 2.4.3 draws the world-strata frame list (nameplates)
// with depth effectively disabled, so plates render OVER everything. The draw bracket
// explicitly forces ZENABLE=TRUE, ZWRITEENABLE=FALSE, ZFUNC=LESSEQUAL and restores all
// three afterward. g_plateZTest is true only inside that bracket so both proxy device
// classes also rewrite any batch-time ZFUNC=ALWAYS back to LESSEQUAL.
volatile bool g_plateZTest = false;

// (Flush-time plate hiding removed 2026-07-16: the displaced corner caches were
// not the representation that reaches the screen. Plates are now hidden at the
// SOURCE — msub_433000 shifts their layout rect off-screen; see hideY there.)

// Render Mouse
void(__thiscall* sub_687A90)(void*) = (void(__thiscall*)(void*))TBC_sub_RenderMouse;
void(__fastcall msub_687A90)(void* ecx, void* edx)
{
    MSUB_TRACE("RenderMouse");
    if (g_suppressGameCursor) return;   // skip the game cursor -> no ghost in one eye
    sub_687A90(ecx);
}

// StartUI
void(__thiscall* sub_494F30)(int) = (void(__thiscall*)(int))TBC_sub_StartUI;
void(__fastcall msub_494F30)(void* ecx, void* edx)
{
    MSUB_TRACE("StartUI");
    sub_494F30((int)ecx);
}

// Start Render
void (*sub_4BEE60)(float*, float*, int) = (void (*)(float*, float*, int))TBC_sub_RenderViewport;  // fix #35: TBC dropped the 4th arg
void(__thiscall* sub_494EE0)(int, int) = (void(__thiscall*)(int, int))TBC_sub_StartRender2;

void(__thiscall* sub_495410)(void*) = (void(__thiscall*)(void*))TBC_sub_StartRender;

// fix #59: one-frame-batch purge routine. In the decompile FUN_0043c7a0(int* list, __fastcall)
// calls FUN_0043c720() with the list still live in ECX -> __thiscall(ecx=list). It walks each
// frame's batch chain and destroys the one-frame batches, then the caller clears list+4.
void(__thiscall* sub_43C720_purge)(int) = (void(__thiscall*)(int))0x0043C720;

// fix #61: engine world-scene cull entry (TBC_sub_WorldSceneCull = 0x00684AA0).
// __cdecl(C3Vector* pos, C3Vector* dir, C3Vector* aux) — three pushed float* (vec3).
// Re-run synchronously before the eye loop from the engine's own latched camera.
void(__cdecl* sub_WorldSceneCull)(float*, float*, float*) = (void(__cdecl*)(float*, float*, float*))0x00684AA0;

// VR aim crosshair: draw a filled "+" centered at (cx,cy) in eye-RT pixels using
// fixed-function pre-transformed (screen-space) colored quads, so it renders no
// matter what shaders the game currently has bound. Saves and restores every device
// state it touches. Called per world eye pass while that eye's RT + viewport are bound.
static void DrawCrosshair(float cx, float cy)
{
    if (!g_crosshair || !devDX9) return;
    float s = g_crosshairSize; if (s < 1.0f) s = 1.0f;
    float t = 2.0f;                                   // bar half-thickness (px)
    // inline clamp of each color channel to 0..1 (volatile array, so read to locals)
    float cr = g_crosshairColor[0], cg = g_crosshairColor[1], cb = g_crosshairColor[2];
    if (cr < 0.0f) cr = 0.0f; else if (cr > 1.0f) cr = 1.0f;
    if (cg < 0.0f) cg = 0.0f; else if (cg > 1.0f) cg = 1.0f;
    if (cb < 0.0f) cb = 0.0f; else if (cb > 1.0f) cb = 1.0f;
    DWORD col = D3DCOLOR_ARGB(255, (BYTE)(cr * 255), (BYTE)(cg * 255), (BYTE)(cb * 255));
    struct V { float x, y, z, rhw; DWORD c; };
    // horizontal bar and vertical bar as two quads (2 tris each)
    V quads[12] = {
        // horizontal: (cx-s,cy-t)-(cx+s,cy+t)
        {cx - s, cy - t, 0, 1, col}, {cx + s, cy - t, 0, 1, col}, {cx - s, cy + t, 0, 1, col},
        {cx + s, cy - t, 0, 1, col}, {cx + s, cy + t, 0, 1, col}, {cx - s, cy + t, 0, 1, col},
        // vertical: (cx-t,cy-s)-(cx+t,cy+s)
        {cx - t, cy - s, 0, 1, col}, {cx + t, cy - s, 0, 1, col}, {cx - t, cy + s, 0, 1, col},
        {cx + t, cy - s, 0, 1, col}, {cx + t, cy + s, 0, 1, col}, {cx - t, cy + s, 0, 1, col},
    };
    IDirect3DDevice9* d = devDX9;
    // save state
    DWORD oZ, oZW, oLIGHT, oCULL, oABE, oAT, oFOG, oCOP, oCA1, oAOP; IDirect3DBaseTexture9* oTex = nullptr;
    IDirect3DVertexShader9* oVS = nullptr; IDirect3DPixelShader9* oPS = nullptr; DWORD oFVF = 0;
    d->GetRenderState(D3DRS_ZENABLE, &oZ); d->GetRenderState(D3DRS_ZWRITEENABLE, &oZW);
    d->GetRenderState(D3DRS_LIGHTING, &oLIGHT); d->GetRenderState(D3DRS_CULLMODE, &oCULL);
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &oABE); d->GetRenderState(D3DRS_ALPHATESTENABLE, &oAT);
    d->GetRenderState(D3DRS_FOGENABLE, &oFOG);
    d->GetTextureStageState(0, D3DTSS_COLOROP, &oCOP); d->GetTextureStageState(0, D3DTSS_COLORARG1, &oCA1); d->GetTextureStageState(0, D3DTSS_ALPHAOP, &oAOP);
    d->GetTexture(0, (IDirect3DBaseTexture9**)&oTex); d->GetVertexShader(&oVS); d->GetPixelShader(&oPS); d->GetFVF(&oFVF);
    // set 2D state
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr); d->SetTexture(0, nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 4, quads, sizeof(V));
    // restore
    d->SetTexture(0, oTex); if (oTex) oTex->Release();
    d->SetVertexShader(oVS); if (oVS) oVS->Release(); d->SetPixelShader(oPS); if (oPS) oPS->Release();
    d->SetFVF(oFVF);
    d->SetRenderState(D3DRS_ZENABLE, oZ); d->SetRenderState(D3DRS_ZWRITEENABLE, oZW);
    d->SetRenderState(D3DRS_LIGHTING, oLIGHT); d->SetRenderState(D3DRS_CULLMODE, oCULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, oABE); d->SetRenderState(D3DRS_ALPHATESTENABLE, oAT);
    d->SetRenderState(D3DRS_FOGENABLE, oFOG);
    d->SetTextureStageState(0, D3DTSS_COLOROP, oCOP); d->SetTextureStageState(0, D3DTSS_COLORARG1, oCA1); d->SetTextureStageState(0, D3DTSS_ALPHAOP, oAOP);
}

// ============================================================================
// WORLD-SPACE NAMEPLATES (redesign 2026-07-16; spec:
// _codex_devnotes/NAMEPLATE_WORLDSPACE_REDESIGN.md). The engine's screen-space
// plate quads are hidden at strata flush (msub_42F390 above). The replacement
// plates are drawn INSIDE the 3D scene, in the NAME pipeline slot: CM2Scene::Draw's
// "callback" element case (0x70DBF0) flushes pending model batches and then calls
//   model->(+0x1DC)(model, model+0x2D4, model->(+0x1E0))
// MID-SCENE, PER EYE, with this eye's world view/proj active, and only for models
// the engine did NOT cull. The shared callback the name updater (0x6D55E0) installs
// is 0x6D5320; we detour it, run the original name draw unchanged, then draw our
// plate quads with a real depth test -> per-pixel occlusion, per-eye placement, and
// vanish-behind-walls all inherited for free.
//
// Verified signature of 0x6D5320 (disasm 8606): void, __cdecl, 3 stack args
// (model, arg=model+0x2D4, ctx=name entry). Proof: the dispatcher at 0x70DC1F does
// push ctx / push arg / push model / call eax / ADD ESP,0xC (caller cleanup), and
// 0x6D5320 itself ends in a plain `ret`. ctx holds the unit GUID at +0x10/+0x14,
// which the original re-resolves through ClntObjMgrObjectPtr (0x46B610) at 0x6D533C.
//
// KNOWN LIMITATION (v1, accepted): the callback only fires for units whose NAME is
// registered for drawing (updater 0x6D55E0 runs only for name-enabled units). Units
// with names off get no world-space plate while their engine plate is hidden. The
// user plays with names visible; do NOT force-register callbacks in v1.

// Engine name-lift helper 0x6D44D0: __cdecl(unit) -> float. Returns the unit's name
// anchor height (model attachment 0x12 height above the model origin, with a
// unit-scale fallback and internal clamping). The engine name draw 0x6D46B0
// multiplies the result by the double at 0x8B0AA0 (= 0.2) before adding it to the
// head-position z (disasm 0x6D472E/0x6D4916) — we mirror that exact math.
float(__cdecl* sub_6D44D0)(void*) = (float(__cdecl*)(void*))0x006D44D0;

// Everything the draw needs, gathered from client memory first. Plain PODs only:
// functions using __try must not contain C++ objects with destructors.
struct WsPlateData {
    float pos[3];        // bar anchor: unit head position + engine name lift
    float right[3];      // camera right in world space (view matrix column 0)
    float up[3];         // camera up in world space (view matrix column 1)
    float view[16];      // this eye's view matrix (row-vector convention, row-major)
    float proj[16];      // this eye's projection as rendered this pass
    float cam[3];        // this eye's camera WORLD position (for relativizing)
    bool camValid;
    float frac;          // health fraction 0..1
    DWORD fillColor;     // D3DCOLOR of the fill quad (reaction RGB + our alpha)
};

// Resolve plate node -> unit -> health/color/anchor for one plate, with THIS eye's
// snapshot matrices. (Reworked from the name-callback variant: the name slot only
// fires for units whose NAME is drawn, and the engine hides names for units that
// have an active plate — mutually exclusive, so the callback never covered plates.
// Runtime-proven: wsDraws≈0. The plate node list from the SetPoint hook covers
// exactly the live plates instead.)
// Returns false on ANY anomaly (nothing is drawn then) — fail-soft by design.
static bool GatherWsPlateData(char* node, int eye, WsPlateData* out)
{
    __try {
        if (!node || (eye != 0 && eye != 1) || !g_wsMtxValid[eye]) return false;
        if (*(uintptr_t*)node != TBC_NAMEPLATE_NODE_VTABLE) return false;
        unsigned lo = *(unsigned*)(node + 0x360);        // plate owner GUID lo
        unsigned hi = *(unsigned*)(node + 0x364);        // plate owner GUID hi
        if (!(lo | hi)) return false;
        char* unit = (char*)ClntObjMgrObjectPtr(lo, hi, 8, ".\\PlayerName.cpp", 0x64);
        if (!unit) { ++g_dbgWsNoUnit; return false; }
        if (*(char**)(unit + 0x1130) != node) { ++g_dbgWsStale; return false; }

        // Anchor: unit vtbl+0x18 = __thiscall(unit, C3Vector* out) — the same head
        // position call the engine name draw makes at 0x6D474B — plus the engine's
        // own name lift (helper * 0.2). Constant fallback if the helper misbehaves.
        typedef void(__thiscall* UnitPosFn)(void*, float*);
        UnitPosFn posFn = *(UnitPosFn*)(*(char**)unit + 0x18);
        if (!posFn) return false;
        out->pos[0] = out->pos[1] = out->pos[2] = 0.0f;
        posFn(unit, out->pos);
        float lift = (float)(sub_6D44D0(unit) * (*(double*)0x8B0AA0));
        if (!_finite(lift) || lift < 0.0f || lift > 50.0f)
            lift = (float)(*(double*)0x8C4CF8);          // 0.6667, plain constant
        out->pos[2] += lift;

        // Health fraction. Primary: the plate's own status-bar frame — CSimpleStatusBar
        // fields disasm-verified on 8606: +0x358 min, +0x35C max, +0x360 value (floats;
        // SetMinMax 0x7951B0 writes +0x358/+0x35C, SetValue 0x795240 clamps against
        // them and compares +0x360). Fallback: unit descriptor array at unit+0x120 —
        // HEALTH at +0x40 and MAXHEALTH at +0x58, disasm-verified in the engine's own
        // .\HealthBar.cpp handler at 0x7BDE50 (fild [desc+0x58] -> SetMinMax max,
        // fild [desc+0x40] -> SetValue).
        float frac = -1.0f;
        char* bar = *(char**)(node + TBC_PLATE_BARFRAME_OFFSET);
        if (bar) {
            float mn = *(float*)(bar + 0x358);
            float mx = *(float*)(bar + 0x35C);
            float v  = *(float*)(bar + 0x360);
            if (_finite(mn) && _finite(mx) && _finite(v) && mx > mn)
                frac = (v - mn) / (mx - mn);
        }
        if (frac < 0.0f) {
            int* desc = *(int**)(unit + 0x120);
            if (desc) {
                int hp  = desc[0x40 / 4];
                int mhp = desc[0x58 / 4];
                if (mhp > 0) frac = (float)hp / (float)mhp;
            }
        }
        if (!_finite(frac) || frac < 0.0f) frac = 1.0f;  // unknown -> full bar
        if (frac > 1.0f) frac = 1.0f;
        out->frac = frac;

        // Fill color: the plate refresh (0x7BD010) stores the reaction color at
        // node+0x394 as a CImVector {b,g,r,a} — the same byte layout as D3DCOLOR,
        // so the dword can be used directly. Keep its RGB, force our fill alpha.
        // If it reads black (not yet refreshed), fall back to green/yellow/red.
        DWORD rgb = *(DWORD*)(node + 0x394) & 0x00FFFFFF;
        if (rgb == 0)
            rgb = (frac > 0.5f) ? 0x0020C020 : (frac > 0.25f) ? 0x00E0C000 : 0x00D01010;
        out->fillColor = 0xD9000000 | rgb;               // alpha ~0.85

        // Matrices + billboard vectors from THIS EYE's snapshot (g_wsView/g_wsProj,
        // captured in msub_4AC810 while the view stack verifiably held this eye's
        // world matrices). The live stack cannot be trusted at end-of-pass time.
        // Row-vector convention (v' = v * V, see Mat4Mul_rowmajor use above): the
        // camera right/up axes in world space are the view matrix's first and second
        // COLUMNS: right = (V[0],V[4],V[8]), up = (V[1],V[5],V[9]).
        const float* V = g_wsView[eye];
        const float* P = g_wsProj[eye];
        for (int i = 0; i < 16; ++i) { out->view[i] = V[i]; out->proj[i] = P[i]; }
        out->camValid = g_wsCamValid[eye];
        if (out->camValid) { out->cam[0] = g_wsCamPos[eye][0];
                             out->cam[1] = g_wsCamPos[eye][1];
                             out->cam[2] = g_wsCamPos[eye][2]; }
        out->right[0] = V[0]; out->right[1] = V[4]; out->right[2] = V[8];
        out->up[0]    = V[1]; out->up[1]    = V[5]; out->up[2]    = V[9];
        // Defensive normalize (the engine's view basis is orthonormal already).
        for (int a = 0; a < 2; ++a) {
            float* w = (a == 0) ? out->right : out->up;
            float len = w[0]*w[0] + w[1]*w[1] + w[2]*w[2];
            if (!_finite(len) || len < 1e-6f) return false;
            len = sqrtf(len);
            w[0] /= len; w[1] /= len; w[2] /= len;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Fail-soft: a despawning unit/plate mid-read just skips this plate.
        ++g_dbgWsGatherFail;
        return false;
    }
}

// Project one point through VP (row-vector) and score how "on-screen" it is:
// max(|ndcx|,|ndcy|), heavily penalized when the depth is outside [0,1] and 1e9
// when the point is behind the camera or non-finite. Used to pick the coordinate
// mode (absolute vs camera-relative) that actually lands in the viewport.
static float WsProbeNdcScore(const float* VP, float wx, float wy, float wz, float* ndcOut)
{
    float clw = wx*VP[3] + wy*VP[7] + wz*VP[11] + VP[15];
    if (!(clw > 0.0001f)) return 1e9f;
    float inv = 1.0f / clw;
    float nx = (wx*VP[0] + wy*VP[4] + wz*VP[8]  + VP[12]) * inv;
    float ny = (wx*VP[1] + wy*VP[5] + wz*VP[9]  + VP[13]) * inv;
    float nz = (wx*VP[2] + wy*VP[6] + wz*VP[10] + VP[14]) * inv;
    if (!_finite(nx) || !_finite(ny) || !_finite(nz)) return 1e9f;
    if (ndcOut) { ndcOut[0] = nx; ndcOut[1] = ny; ndcOut[2] = nz; }
    float s = fabsf(nx);
    float a = fabsf(ny);
    if (a > s) s = a;
    if (nz < -0.1f || nz > 1.1f) s += 100.0f;
    return s;
}

// Draw the two camera-facing quads with OUR OWN raw D3D9 calls on the proxy device
// global devDX9. CRITICAL: the engine NEVER uses the fixed-function transform path
// (zero SetTransform call sites in the whole binary — it is 100% vertex-shader
// based), so an FFP XYZ draw depends on residual device state and was invisible in
// practice. Instead the vertices are transformed on the CPU with this eye's
// view*proj (the same math that produced verified-healthy plate depths) and drawn
// as PRE-TRANSFORMED XYZRHW quads — byte-for-byte the draw path the crosshair
// already proves works on this device. The z-test still applies to RHW vertices:
// ZENABLE=TRUE + ZWRITEENABLE=FALSE z-tests our pixels against the eye's scene
// depth buffer -> per-pixel occlusion, no depth pollution. Every touched state is
// saved/restored verbatim (keeps the engine's Gx state cache truthful).
static void DrawWsPlateQuads(const WsPlateData* p, float vpW, float vpH)
{
    IDirect3DDevice9* d = devDX9;
    if (!d || vpW < 1.0f || vpH < 1.0f) return;

    // Bar geometry v1: ~1.4 x 0.18 world units, bottom edge on the anchor, centered
    // horizontally. Fill inset by 0.02 on every side and scaled horizontally by the
    // health fraction from the LEFT edge.
    const float halfW = 0.7f, h = 0.18f, inset = 0.02f;
    const DWORD bgColor = 0x8C000000;                    // black, alpha ~0.55
    float fillX0 = -halfW + inset;
    float fillX1 = fillX0 + p->frac * (2.0f * (halfW - inset));
    if (fillX1 < fillX0) fillX1 = fillX0;

    // CPU transform: world corner -> clip via view*proj (row-vector convention,
    // same as Mat4Mul_rowmajor / StashW2SDepth) -> viewport pixels + z/w + 1/w.
    float VP[16];
    Mat4Mul_rowmajor(p->view, p->proj, VP);

    // COORDINATE MODE: the engine renders CAMERA-RELATIVE (the captured view has
    // zero translation — runtime lastT2=0), so absolute world coordinates come out
    // far off-screen. Probe the anchor in both modes and use whichever lands sanely
    // in NDC — self-correcting if some context ever supplies a translating view.
    float off[3] = { 0, 0, 0 };
    {
        float ndcA[3], ndcR[3];
        float sA = WsProbeNdcScore(VP, p->pos[0], p->pos[1], p->pos[2], ndcA);
        float sR = 1e9f;
        if (p->camValid)
            sR = WsProbeNdcScore(VP, p->pos[0] - p->cam[0], p->pos[1] - p->cam[1],
                                 p->pos[2] - p->cam[2], ndcR);
        if (sA >= 1e9f && sR >= 1e9f) { ++g_dbgWsClip; return; } // anchor unusable
        if (sR < sA) {
            off[0] = p->cam[0]; off[1] = p->cam[1]; off[2] = p->cam[2];
            ++g_dbgWsRel;
            g_dbgWsNdc[0] = ndcR[0]; g_dbgWsNdc[1] = ndcR[1]; g_dbgWsNdc[2] = ndcR[2];
        } else {
            ++g_dbgWsAbs;
            g_dbgWsNdc[0] = ndcA[0]; g_dbgWsNdc[1] = ndcA[1]; g_dbgWsNdc[2] = ndcA[2];
        }
    }

    struct WsVtx { float x, y, z, rhw; DWORD c; };       // D3DFVF_XYZRHW | DIFFUSE
    WsVtx q[12];
    const float R0 = p->right[0], R1 = p->right[1], R2 = p->right[2];
    const float U0 = p->up[0],    U1 = p->up[1],    U2 = p->up[2];
    // Local corner list: {X,Y} in bar space, color per corner (6 bg + 6 fill).
    const float cx[12] = { -halfW,  halfW, -halfW,  halfW,  halfW, -halfW,
                           fillX0,  fillX1, fillX0, fillX1, fillX1, fillX0 };
    const float cy[12] = { h, h, 0.0f, h, 0.0f, 0.0f,
                           h - inset, h - inset, inset, h - inset, inset, inset };
    for (int v = 0; v < 12; ++v) {
        float wx = p->pos[0] - off[0] + R0 * cx[v] + U0 * cy[v];
        float wy = p->pos[1] - off[1] + R1 * cx[v] + U1 * cy[v];
        float wz = p->pos[2] - off[2] + R2 * cx[v] + U2 * cy[v];
        float clx = wx*VP[0] + wy*VP[4] + wz*VP[8]  + VP[12];
        float cly = wx*VP[1] + wy*VP[5] + wz*VP[9]  + VP[13];
        float clz = wx*VP[2] + wy*VP[6] + wz*VP[10] + VP[14];
        float clw = wx*VP[3] + wy*VP[7] + wz*VP[11] + VP[15];
        if (!(clw > 0.0001f)) { ++g_dbgWsClip; return; } // behind the camera: skip plate
        float inv = 1.0f / clw;
        float ndcx = clx * inv, ndcy = cly * inv, ndcz = clz * inv;
        if (!_finite(ndcx) || !_finite(ndcy) || !_finite(ndcz)) { ++g_dbgWsClip; return; }
        if (ndcz < 0.0f) { ++g_dbgWsClip; return; }      // in front of the near plane
        q[v].x = (0.5f + 0.5f * ndcx) * vpW;
        q[v].y = (0.5f - 0.5f * ndcy) * vpH;
        float zz = (ndcz > 1.0f) ? 1.0f : ndcz;
        // DIAG (nameplate_zforce in [0,1]): override the bar's screen-space z.
        float zf = g_nameplateZForce;
        if (zf >= 0.0f && zf <= 1.0f) zz = zf;
        // Depth bias (nameplate_zbias, live): pull the bar slightly NEARER so it wins
        // the LESSEQUAL test in open air but still loses behind foreground geometry.
        zz -= g_nameplateZBias;
        if (zz < 0.0f) zz = 0.0f; if (zz > 1.0f) zz = 1.0f;
        q[v].z = zz;
        q[v].rhw = inv;
        q[v].c = (v < 6) ? bgColor : p->fillColor;
    }

    __try {
        // ---- save EVERY device state we touch (COM getters AddRef) ----
        IDirect3DVertexShader9* oVS = NULL; IDirect3DPixelShader9* oPS = NULL;
        IDirect3DBaseTexture9* oTex = NULL;
        IDirect3DVertexDeclaration9* oDecl = NULL;
        IDirect3DVertexBuffer9* oVB = NULL; UINT oVBOfs = 0, oVBStride = 0;
        DWORD oFVF = 0, oZ = 0, oZW = 0, oZF = 0, oABE = 0, oATE = 0, oSRC = 0,
              oDST = 0, oLIT = 0, oCULL = 0, oFOG = 0;
        DWORD oCOP = 0, oCA2 = 0, oAOP = 0, oAA2 = 0, oCOP1 = 0, oAOP1 = 0;
        d->GetVertexShader(&oVS); d->GetPixelShader(&oPS);
        d->GetFVF(&oFVF); d->GetVertexDeclaration(&oDecl);
        d->GetStreamSource(0, &oVB, &oVBOfs, &oVBStride); // DrawPrimitiveUP nulls stream 0
        d->GetTexture(0, &oTex);
        d->GetRenderState(D3DRS_ZENABLE, &oZ); d->GetRenderState(D3DRS_ZWRITEENABLE, &oZW);
        d->GetRenderState(D3DRS_ZFUNC, &oZF);
        d->GetRenderState(D3DRS_ALPHABLENDENABLE, &oABE); d->GetRenderState(D3DRS_ALPHATESTENABLE, &oATE);
        d->GetRenderState(D3DRS_SRCBLEND, &oSRC); d->GetRenderState(D3DRS_DESTBLEND, &oDST);
        d->GetRenderState(D3DRS_LIGHTING, &oLIT); d->GetRenderState(D3DRS_CULLMODE, &oCULL);
        d->GetRenderState(D3DRS_FOGENABLE, &oFOG);
        d->GetTextureStageState(0, D3DTSS_COLOROP, &oCOP); d->GetTextureStageState(0, D3DTSS_COLORARG2, &oCA2);
        d->GetTextureStageState(0, D3DTSS_ALPHAOP, &oAOP); d->GetTextureStageState(0, D3DTSS_ALPHAARG2, &oAA2);
        d->GetTextureStageState(1, D3DTSS_COLOROP, &oCOP1); d->GetTextureStageState(1, D3DTSS_ALPHAOP, &oAOP1);

        // ---- pre-transformed draw state (crosshair-proven path) ----
        d->SetVertexShader(NULL); d->SetPixelShader(NULL);
        d->SetTexture(0, NULL);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        // Depth test. FORCE ZENABLE=TRUE (readback proved our earlier zmode-based set
        // came out 0 = disabled, so the depth test was inert and bars always drew on
        // top). zmode only picks TRUE vs USEW now; 0 still means "no test" for the A/B.
        int zm = g_nameplateZMode;
        DWORD zEnableVal = (zm == 0) ? D3DZB_FALSE : (zm == 2) ? D3DZB_USEW : D3DZB_TRUE;
        d->SetRenderState(D3DRS_ZENABLE, zEnableVal);
        DWORD zAfter = 99; d->GetRenderState(D3DRS_ZENABLE, &zAfter);
        if (zAfter != zEnableVal) {          // set didn't stick -> try again, hard
            d->SetRenderState(D3DRS_ZENABLE, zEnableVal);
            d->GetRenderState(D3DRS_ZENABLE, &zAfter);
        }
        { static int s_zl = 0; if (s_zl < 2) { ++s_zl;
            ofOut << "[occlzset] zm=" << zm << " wanted=" << zEnableVal
                  << " readback=" << zAfter << std::endl; ofOut.flush(); } }
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_LIGHTING, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);   // no texture:
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);     // diffuse only
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        // ONE-TIME readback: did our depth states actually take on this device, and
        // is the scene depth still bound at the ACTUAL draw call? If ZENABLE reads 0
        // or no DS is bound here, the test is inert regardless of z (explains bars on
        // top even at forced 0.9995).
        {
            static int s_rb = 0;
            if (s_rb < 2) {
                ++s_rb;
                DWORD rze=9,rzf=9,rzw=9; IDirect3DSurface9* rds=NULL;
                d->GetRenderState(D3DRS_ZENABLE,&rze);
                d->GetRenderState(D3DRS_ZFUNC,&rzf);
                d->GetRenderState(D3DRS_ZWRITEENABLE,&rzw);
                HRESULT hr = d->GetDepthStencilSurface(&rds);
                IDirect3DSurface9* rrt=NULL; d->GetRenderTarget(0,&rrt);
                ofOut << "[occlrb] afterSet ZENABLE=" << rze << " ZFUNC=" << rzf
                      << " ZWRITE=" << rzw << " dsBound=" << (rds?1:0)
                      << " dsHR=0x" << std::hex << (unsigned)hr << std::dec
                      << " rtBound=" << (rrt?1:0)
                      << " q0z=" << q[0].z << " q0rhw=" << q[0].rhw
                      << std::endl; ofOut.flush();
                if (rds) rds->Release(); if (rrt) rrt->Release();
            }
        }
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 4, q, sizeof(WsVtx));
        ++g_dbgWsDraws;

        // ---- restore verbatim (keeps the engine's Gx state cache truthful) ----
        d->SetVertexShader(oVS); if (oVS) oVS->Release();
        d->SetPixelShader(oPS); if (oPS) oPS->Release();
        if (oDecl) { d->SetVertexDeclaration(oDecl); oDecl->Release(); }
        else if (oFVF) d->SetFVF(oFVF);
        d->SetStreamSource(0, oVB, oVBOfs, oVBStride); if (oVB) oVB->Release();
        d->SetTexture(0, oTex); if (oTex) oTex->Release();
        d->SetRenderState(D3DRS_ZENABLE, oZ); d->SetRenderState(D3DRS_ZWRITEENABLE, oZW);
        d->SetRenderState(D3DRS_ZFUNC, oZF);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, oABE); d->SetRenderState(D3DRS_ALPHATESTENABLE, oATE);
        d->SetRenderState(D3DRS_SRCBLEND, oSRC); d->SetRenderState(D3DRS_DESTBLEND, oDST);
        d->SetRenderState(D3DRS_LIGHTING, oLIT); d->SetRenderState(D3DRS_CULLMODE, oCULL);
        d->SetRenderState(D3DRS_FOGENABLE, oFOG);
        d->SetTextureStageState(0, D3DTSS_COLOROP, oCOP); d->SetTextureStageState(0, D3DTSS_COLORARG2, oCA2);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, oAOP); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, oAA2);
        d->SetTextureStageState(1, D3DTSS_COLOROP, oCOP1); d->SetTextureStageState(1, D3DTSS_ALPHAOP, oAOP1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Fail-soft: a failed draw must never crash; the plate simply isn't drawn.
    }
}

// Draw ALL live plates for this eye, called from the eye loop at the END of the
// eye's world rendering — the eye's render target and scene depth buffer are still
// bound and complete, so ZENABLE gives true per-pixel occlusion (including behind
// walls: wall pixels are nearer, the plate z-fails there). This replaced the
// 0x6D5320 name-callback injection: the engine hides NAMES for units that have an
// active PLATE, so the callback never fired for plated units (wsDraws≈0).
static void DrawAllWsPlates(int eye, float vpW, float vpH)
{
    if ((eye != 0 && eye != 1) || !devDX9) return;
    if (!g_wsMtxValid[eye]) { ++g_dbgWsNoMtx; return; }
    // ONE-TIME probe: what is the depth environment at OUR draw point? If no depth
    // surface is bound, or ZENABLE is off, hardware occlusion is impossible here and
    // that (not the z/w mode) is the real reason plates float on top of everything.
    static int s_probed = 0;
    if (s_probed < 2) {
        ++s_probed;
        __try {
            IDirect3DSurface9* ds = NULL;
            HRESULT hds = devDX9->GetDepthStencilSurface(&ds);
            DWORD ze = 0, zf = 0, zw = 0; D3DVIEWPORT9 vp = {0};
            devDX9->GetRenderState(D3DRS_ZENABLE, &ze);
            devDX9->GetRenderState(D3DRS_ZFUNC, &zf);
            devDX9->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
            devDX9->GetViewport(&vp);
            D3DSURFACE_DESC sd; sd.Width = sd.Height = 0; sd.Format = D3DFMT_UNKNOWN;
            if (ds) ds->GetDesc(&sd);
            ofOut << "[occlprobe] eye=" << eye
                  << " dsBound=" << (ds ? 1 : 0) << " hr=0x" << std::hex << (unsigned)hds << std::dec
                  << " dsW=" << sd.Width << " dsH=" << sd.Height
                  << " dsFmt=" << (unsigned)sd.Format
                  << " engineZENABLE=" << ze << " ZFUNC=" << zf << " ZWRITE=" << zw
                  << " vp=" << vp.Width << "x" << vp.Height
                  << " minZ=" << vp.MinZ << " maxZ=" << vp.MaxZ
                  << std::endl; ofOut.flush();
            if (ds) ds->Release();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    for (int i = 0; i < g_plateLayoutCount; ++i) {
        PlateRec& r = g_plateLayout[i];
        if (!r.plate) continue;
        WsPlateData wsd;
        if (GatherWsPlateData((char*)r.plate, eye, &wsd))
            DrawWsPlateQuads(&wsd, vpW, vpH);
    }
}

// NAME-SLOT draw (user's approach): when nameplates are OFF and NAMES are shown, the
// engine calls the per-unit name draw callback 0x6D5320 MID-SCENE, per eye, with the
// exact geometry matrices and the scene depth bound — the same environment where the
// engine's own names occlude correctly (hide behind the lamp). Drawing our health bar
// here inherits that correct occlusion. Signature (disasm 8606): void __cdecl(model,
// arg=model+0x2D4, ctx=name entry); ctx GUID at +0x10/+0x14.
static unsigned g_dbgNameDraws = 0;
static unsigned g_dbgNameAppends = 0;

// TEXT-BAR path (user's design v2): instead of drawing our own geometry, append an
// ASCII health bar ("####----") as an extra LINE of the engine's own floating unit
// name. Mechanics (disasm 8606, 0x6D46B0): the name text is NOT stored in the name
// entry — on rebuild the engine fetches it into a 0x400-byte stack buffer via unit
// vtbl+0xA0, runs it through the markup helper 0x42C850, and bakes a cached string
// object into ctx+0x08. A rebuild happens only when ctx+0x18 bit0 (text-dirty) is
// set or no cached object exists; '\n' produces extra lines natively (guild names
// use the same path). So: set the dirty bit when the health bucket changes, and
// append our bar inside a detour on 0x42C850, gated to this thread's name-draw
// window only. The engine then renders it with its normal world-space text path —
// correct occlusion for free.
volatile DWORD g_nameBarTid = 0;          // thread inside the name-draw window (read by SetTexture wrapper)
void* volatile g_nameBarTex0 = 0;         // font-atlas texture captured by the SetTexture wrapper
static char g_nameBarText[96];            // "\n" + bar characters, one-shot per window
volatile int g_nameBarColor = 1;          // live key barcolor.txt: 1 = per-vertex recolor in glyph hook
static float g_nameBarFrac = 1.0f;        // health fraction of the unit currently in the window
volatile int g_nameBarQuad = 1;           // live key barquad.txt: 1 = weld the 16 '#' glyphs into one solid rect
volatile float g_nameBarForceFrac = -1.0f;// live key barfrac.txt: 0..1 forces displayed fraction (test), -1 off
volatile int g_nameBarShow = 1;           // Ctrl+V in-game toggle: 1 = name + bar panel, 0 = names only
static DWORD g_nameBarUnitColor = 0xFF20D020;  // engine-picked name color of the armed unit (ctx+0x0C)

// Nameplate MODE (addon cvar vrNameplateMode, applies instantly):
//   0 = ORIGINAL: stock engine nameplates, drawn over everything (occlusion system off)
//   1 = 3D PLATES: world-space name panel skinned with the ORIGINAL nameplate texture
//       (falls back to the NewPlate frame until the engine texture loader is wired)
//   2 = NEWPLATE: our custom panel (beveled frame; mana/cast bars planned)
volatile int g_nameplateMode = 2;
static void ApplyNameplateMode(int m)
{
    if (m < 0) m = 0; if (m > 2) m = 2;
    g_nameplateMode = m;
    g_nameplateOcclusion = (m != 0) ? 1 : 0;  // 0: unhide engine plates + drop bar lines
    if (m != 0) g_nameplateTextBar = 1;       // both 3D modes live in the name pipeline
    ofOut << "[bar] nameplate mode = " << m << std::endl; ofOut.flush();
}

// Ctrl+V has no default binding in 2.4.3, so we claim it as the panel toggle. Polled
// (edge-triggered, ~50ms) from the name callback; only reacts while our game window
// has focus so typing Ctrl+V elsewhere cannot flip it.
static void NameBarHotkeyTick()
{
    static DWORD s_lastChk = 0;
    static DWORD s_lastToggle = 0;
    static int s_prevDown = 0;
    DWORD t = GetTickCount();
    if (t - s_lastChk < 50) return;
    s_lastChk = t;
    int down = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) &&
               ((GetAsyncKeyState('V') & 0x8000) != 0);
    // 400ms cooldown: the async key state bounces within one press (observed 5 toggles
    // from 2 presses), so one press must count once.
    if (down && !s_prevDown && t - s_lastToggle > 400) {
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            s_lastToggle = t;
            g_nameBarShow = g_nameBarShow ? 0 : 1;
            ofOut << "[bar] Ctrl+V -> bar panel " << (g_nameBarShow ? "ON" : "OFF")
                  << std::endl; ofOut.flush();
        }
    }
    s_prevDown = down;
}
#define NAMEBAR_SEGS 16
struct NameBarSlot { void* ctx; int bucket; };
static NameBarSlot g_nameBarSlots[128];

// 0x616FF0 = the unit "get display name" virtual (vtbl+0xA0, shared by Unit AND
// Player vtables 0x8C3358/0x8C5620): __thiscall (mask, char* out, int cap), fills the
// caller's buffer and returns the LOGICAL LINE count (0x6D46B0 multiplies it by the
// line height for the vertical anchor). Appending here — instead of post-processing —
// keeps that line count correct (+1 for our bar line).
typedef int(__fastcall* NameProvideFn)(void* unit, void* edx, unsigned mask, char* out, int cap);
NameProvideFn sub_616FF0 = (NameProvideFn)0x00616FF0;
int __fastcall msub_616FF0(void* unit, void* edx, unsigned mask, char* out, int cap)
{
    int lines = sub_616FF0(unit, edx, mask, out, cap);
    if (out && cap > 0 && g_nameBarText[0] && g_nameBarTid == GetCurrentThreadId()) {
        __try {
            size_t len = strlen(out);
            size_t add = strlen(g_nameBarText);
            if (len > 0 && len + add < (size_t)cap) {
                memcpy(out + len, g_nameBarText, add + 1);
                ++g_dbgNameAppends;
                ++lines;                               // our bar is one extra line
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_nameBarText[0] = 0;                          // one-shot: append once per window
    }
    return lines;
}

// Glyph-vertex copy 0x5C6F40 (__thiscall on a layout chunk; args dst, fontPage, srcOff,
// count; ret 0x10): writes the FINAL world-space glyph vertices of one string for one
// font-texture page into the frame's mapped vertex buffer (via the corner writer
// 0x5C5740). Stride 24 = float pos[3], D3DCOLOR, float uv[2]. It runs EVERY frame from
// the per-string draw 0x5C5D30 (the baked layout is cached, the copy is not), inside our
// armed name-draw window — so this is the one spot where the bar glyphs exist as raw
// vertices with engine-correct depth. Post-process IN PLACE: locate the 16 identical-UV
// '#' quads of the bar line, weld them into TWO solid rectangles (health fill + dark
// remainder) with per-vertex colors and all UVs collapsed onto the glyph's ink center
// (solid look), degenerate the leftover quads. No new draw calls, no state changes.
struct NameVert { float x, y, z; DWORD c; float u, v; };
typedef void(__fastcall* GlyphCopyFn)(void* chunk, void* edx, NameVert* dst, int page, int srcOff, int count);
GlyphCopyFn sub_5C6F40 = (GlyphCopyFn)0x005C6F40;
static unsigned g_dbgBarWelds = 0;
static unsigned g_dbgBarWeldFails = 0;
static int g_dbgBarDumpOnce = 1;

// One-time scan of the live font atlas for a FULLY OPAQUE texel inside the '#' glyph
// rect: the glyph-rect center is the hole between the '#' strokes (alpha < 1 -> the bar
// looked translucent), so find the max-alpha texel instead. The atlas is captured by the
// SetTexture wrapper (g_nameBarTex0, managed pool -> lockable read-only).
static bool  g_inkResolved = false;
static float g_inkU = 0.0f, g_inkV = 0.0f;

// TEXTURED FRAME experiment (bartex.txt, default 1): draw a real textured border quad
// around the panel with our OWN texture, as a separate draw right after the engine's
// per-string draw (0x5C5D30) — the batch transforms/states set by 0x5C65D0 are still
// bound there, so the quad lands in the same plane with the same depth behavior. The
// frame texture has a TRANSPARENT center (alpha-tested away -> no depth write -> no
// z-fight with the color fill underneath) and an opaque beveled ring. v1 texture is
// procedural; a user-painted file can replace it later.
// DEFAULT OFF: the first live test flickered between two heights — the FFP UP-draw used
// different transforms than the engine's string draw. Opt-in via bartex.txt while the
// explicit-SetTransform variant below is being validated on eye-dumps.
volatile int g_nameBarTexKnob = 0;        // live key bartex.txt
struct PanelBasis { float c0[3], R[3], U[3], r0, r1, u0, u1; };
static PanelBasis g_panel;
static volatile int g_panelValid = 0;
// LEVEL digit quads relocated onto the frame's plaque by the weld (BL,TL,BR,TR per
// digit); drawn right after the frame quad with a tiny depth bias toward the camera
// so they deterministically win against the coplanar plaque (no z-fight).
static NameVert g_digitVerts[8];
static int g_digitVertCount = 0;
static IDirect3DTexture9* g_barFrameTex = 0;
static unsigned g_dbgBarTexDraws = 0;

// ORIGINAL nameplate border texture through the ENGINE's own loader (mode 1).
// Recipe from the Codex reverse (runs/20260717_002430): TextureCreate 0x457FC0(path,
// flags 0x101, CStatus*, 0, 1, 1) -> CTexture* (cached+refcounted, keep for the
// session); TextureGetGxTex 0x454C50(ctex, wait=1, 0) -> CGxTex* (borrowed); ensure
// GPU realization via gx vtbl slot 0; IDirect3DTexture9* lives at CGxTex+0x38 (reread
// every use — device reset can replace it). The CStatus layout is copied from
// CSimpleTexture::SetTexture 0x42F98B: vtable 0x88CE84, cap 8, self-linked list, level.
typedef void*(__cdecl* Fn_TextureCreate)(const char*, unsigned, void*, int, int, int);
typedef void*(__cdecl* Fn_TextureGetGxTex)(void*, int, void*);
static void* g_origPlateCTex = 0;
static void* g_origPlateGxTex = 0;
static int   g_origPlateTried = 0;
// UV sub-rect of the actual border ART inside the texture canvas (the .blp has large
// transparent margins, so stretching 0..1 shifted the art off the panel). Filled by a
// one-time alpha-bounds scan of the realized texture.
static float g_origUV[4] = { 0, 0, 1, 1 };   // u0, v0, u1, v1 of the border ART
static int   g_origUVScanned = 0;
// Transparent HOLE inside the art, RELATIVE to the art bbox (0..1; v from art top).
// The health fill must live entirely inside this hole — any pixel painted both by the
// ring and by the fill z-fights (user-verified flicker).
static float g_origHoleRel[4] = { 0.03f, 0.12f, 0.97f, 0.88f };
static int   g_origHoleValid = 0;

static void ScanOrigPlateUV(IDirect3DTexture9* tex)
{
    if (g_origUVScanned || !tex) return;
    D3DSURFACE_DESC dsc;
    if (FAILED(tex->GetLevelDesc(0, &dsc))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return;
    int x0 = dsc.Width, y0 = dsc.Height, x1 = -1, y1 = -1;
    bool dxt3 = (dsc.Format == D3DFMT_DXT3);   // 4x4 blocks, explicit 4-bit alpha
    const BYTE* bits = (const BYTE*)lr.pBits;
    int pitch = lr.Pitch;
    D3DFORMAT fmt = dsc.Format;
    // alpha of texel (x,y); -1 = unsupported format
    #define TEXEL_A(x, y) ( \
        dxt3 ? ((((bits + ((y) / 4) * pitch + ((x) / 4) * 16)[(((y) % 4) * 4 + ((x) % 4)) / 2]) \
                 >> ((((y) % 4) * 4 + ((x) % 4)) & 1 ? 4 : 0)) & 0xF) * 17 : \
        fmt == D3DFMT_A8R8G8B8 ? (bits + (y) * pitch)[(x) * 4 + 3] : \
        fmt == D3DFMT_A8L8     ? (bits + (y) * pitch)[(x) * 2 + 1] : \
        fmt == D3DFMT_A4R4G4B4 ? ((((const WORD*)(bits + (y) * pitch))[(x)] >> 12) * 17) : -1 )
    for (int y = 0; y < (int)dsc.Height; ++y)
        for (int x = 0; x < (int)dsc.Width; ++x)
            if (TEXEL_A(x, y) > 32) {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
    if (x1 > x0 && y1 > y0) {
        // HOLE: expand from the art center while transparent (the ring is closed, the
        // level plaque sits at the far right and just shortens the hole there).
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        if (TEXEL_A(cx, cy) <= 32) {
            int hx0 = cx, hx1 = cx, hy0 = cy, hy1 = cy;
            while (hx0 > x0 && TEXEL_A(hx0 - 1, cy) <= 32) --hx0;
            while (hx1 < x1 && TEXEL_A(hx1 + 1, cy) <= 32) ++hx1;
            while (hy0 > y0 && TEXEL_A(cx, hy0 - 1) <= 32) --hy0;
            while (hy1 < y1 && TEXEL_A(cx, hy1 + 1) <= 32) ++hy1;
            float aw = (float)(x1 + 1 - x0), ah = (float)(y1 + 1 - y0);
            g_origHoleRel[0] = (hx0 - x0) / aw;      g_origHoleRel[1] = (hy0 - y0) / ah;
            g_origHoleRel[2] = (hx1 + 1 - x0) / aw;  g_origHoleRel[3] = (hy1 + 1 - y0) / ah;
            g_origHoleValid = 1;
            ofOut << "[barq] orig border hole rel=(" << g_origHoleRel[0] << "," << g_origHoleRel[1]
                  << ")-(" << g_origHoleRel[2] << "," << g_origHoleRel[3] << ")" << std::endl; ofOut.flush();
        }
    }
    #undef TEXEL_A
    tex->UnlockRect(0);
    if (x1 >= x0 && y1 >= y0) {
        g_origUV[0] = (float)x0 / dsc.Width;  g_origUV[1] = (float)y0 / dsc.Height;
        g_origUV[2] = (float)(x1 + 1) / dsc.Width; g_origUV[3] = (float)(y1 + 1) / dsc.Height;
        g_origUVScanned = 1;
        ofOut << "[barq] orig border art uv=(" << g_origUV[0] << "," << g_origUV[1]
              << ")-(" << g_origUV[2] << "," << g_origUV[3] << ") fmt=" << dsc.Format
              << " " << dsc.Width << "x" << dsc.Height << std::endl; ofOut.flush();
    } else if (dsc.Format == D3DFMT_DXT1 || dsc.Format == D3DFMT_DXT5) {
        // other compressed formats: keep full UV, log once
        g_origUVScanned = 1;
        ofOut << "[barq] orig border compressed fmt=" << dsc.Format << ", full uv kept" << std::endl; ofOut.flush();
    }
}

static IDirect3DTexture9* GetOriginalPlateTexture()
{
    char* gx = (char*)*(int**)0xD2A15C;
    if (!gx || !*(int*)(gx + 0xEE4)) return 0;         // Gx device not active yet
    if (!g_origPlateGxTex && !g_origPlateTried) {
        g_origPlateTried = 1;
        struct { void* vt; int cap; void* la; uintptr_t lb; int level; } st;
        st.vt = (void*)0x0088CE84; st.cap = 8;
        st.la = &st.la; st.lb = (uintptr_t)&st.la | 1; st.level = 0;
        void* ctex = ((Fn_TextureCreate)0x00457FC0)(
            "Interface\\Tooltips\\Nameplate-Border", 0x101, &st, 0, 1, 1);
        if (ctex && st.level < 2) {
            g_origPlateCTex = ctex;
            g_origPlateGxTex = ((Fn_TextureGetGxTex)0x00454C50)(ctex, 1, 0);
        }
        ofOut << "[barq] original plate tex ctex=" << g_origPlateCTex
              << " gxtex=" << g_origPlateGxTex << " status=" << st.level
              << std::endl; ofOut.flush();
    }
    if (!g_origPlateGxTex) return 0;
    typedef void(__thiscall* EnsureFn)(void*, void*);
    void** vt = *(void***)gx;
    ((EnsureFn)vt[0])(gx, g_origPlateGxTex);           // realize if needed (0x5AAF40)
    IDirect3DTexture9* tex = *(IDirect3DTexture9**)((char*)g_origPlateGxTex + 0x38);
    ScanOrigPlateUV(tex);
    return tex;
}

static void EnsureBarFrameTex()
{
    if (g_barFrameTex || !devDX9) return;
    const int W = 64, H = 16, RING = 2;
    if (FAILED(devDX9->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                     &g_barFrameTex, 0)) || !g_barFrameTex) { g_barFrameTex = 0; return; }
    D3DLOCKED_RECT lr;
    if (FAILED(g_barFrameTex->LockRect(0, &lr, NULL, 0))) { g_barFrameTex->Release(); g_barFrameTex = 0; return; }
    for (int y = 0; y < H; ++y) {
        DWORD* row = (DWORD*)((char*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < W; ++x) {
            int edge = x; if (W - 1 - x < edge) edge = W - 1 - x;
            if (y < edge) edge = y; if (H - 1 - y < edge) edge = H - 1 - y;
            if (edge >= RING) { row[x] = 0x00000000; continue; }      // transparent center
            bool lit = (y <= x) && (y <= (H - 1 - y));                // crude top-left bevel
            row[x] = (edge == 0) ? 0xFF000000 : (lit ? 0xFF6E6E6E : 0xFF2A2A2A);
        }
    }
    g_barFrameTex->UnlockRect(0);
    ofOut << "[barq] frame texture created" << std::endl; ofOut.flush();
}

static void DrawBarFrameTex()
{
    if (!devDX9) return;
    // Mode 1 = original nameplate border art; mode 2 (or loader failure) = NewPlate.
    IDirect3DTexture9* frameTex = 0;
    if (g_nameplateMode == 1) frameTex = GetOriginalPlateTexture();
    if (!frameTex) { EnsureBarFrameTex(); frameTex = g_barFrameTex; }
    if (!frameTex) return;
    IDirect3DDevice9* d = devDX9;
    IDirect3DBaseTexture9* prevTex = 0; d->GetTexture(0, &prevTex);
    IDirect3DVertexShader9* prevVS = 0; d->GetVertexShader(&prevVS);
    DWORD prevFVF = 0; d->GetFVF(&prevFVF);
    IDirect3DVertexBuffer9* prevVB = 0; UINT prevOff = 0, prevStride = 0;
    d->GetStreamSource(0, &prevVB, &prevOff, &prevStride);
    if (prevVS) d->SetVertexShader(NULL);
    // FFP transforms are NOT what the engine's shader path used — set them explicitly
    // from the live CGxDevice matrices (world=identity, view slot, projection +0xF4C)
    // and restore afterwards, otherwise the quad lands at a different height and
    // flickers against the welded bars (first live test).
    D3DMATRIX prevW, prevV, prevP;
    d->GetTransform(D3DTS_WORLD, &prevW);
    d->GetTransform(D3DTS_VIEW, &prevV);
    d->GetTransform(D3DTS_PROJECTION, &prevP);
    {
        char* gx = (char*)*(int**)0xD2A15C;
        if (!gx) { if (prevVS) { d->SetVertexShader(prevVS); prevVS->Release(); }
                   d->SetTexture(0, prevTex); if (prevTex) prevTex->Release();
                   if (prevVB) { d->SetStreamSource(0, prevVB, prevOff, prevStride); prevVB->Release(); }
                   return; }
        D3DMATRIX ident; memset(&ident, 0, sizeof(ident));
        ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
        int vsIdx = *(int*)(gx + 0x1A7C);
        if (vsIdx < 0 || vsIdx > 63) vsIdx = 0;
        d->SetTransform(D3DTS_WORLD, &ident);
        d->SetTransform(D3DTS_VIEW, (const D3DMATRIX*)(gx + 0x1A84 + 64 * vsIdx));
        d->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)(gx + 0xF4C));
    }
    d->SetTexture(0, frameTex);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    struct V { float x, y, z; DWORD c; float u, v; } q[4];
    const PanelBasis& p = g_panel;
    const float rr[4] = { p.r0, p.r0, p.r1, p.r1 };   // BL, TL, TR, BR (triangle fan)
    const float uu[4] = { p.u0, p.u1, p.u1, p.u0 };
    float a0 = 0, b0 = 0, a1 = 1, b1 = 1;             // texture sub-rect (art bounds)
    if (g_nameplateMode == 1 && g_origUVScanned) {
        a0 = g_origUV[0]; b0 = g_origUV[1]; a1 = g_origUV[2]; b1 = g_origUV[3];
    }
    const float tu[4] = { a0, a0, a1, a1 };
    const float tv[4] = { b1, b0, b0, b1 };
    for (int i = 0; i < 4; ++i) {
        q[i].x = p.c0[0] + p.R[0] * rr[i] + p.U[0] * uu[i];
        q[i].y = p.c0[1] + p.R[1] * rr[i] + p.U[1] * uu[i];
        q[i].z = p.c0[2] + p.R[2] * rr[i] + p.U[2] * uu[i];
        q[i].c = 0xFFFFFFFF;
        q[i].u = tu[i]; q[i].v = tv[i];
    }
    d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, q, sizeof(V));
    if (g_digitVertCount >= 4 && g_nameBarTex0) {
        // Level digits over the plaque: their UVs point into the FONT atlas (captured
        // by the SetTexture wrapper), and a hair of depth bias toward the camera makes
        // them deterministically beat the coplanar plaque instead of z-fighting.
        d->SetTexture(0, (IDirect3DBaseTexture9*)g_nameBarTex0);
        DWORD oldBias = 0;
        d->GetRenderState(D3DRS_DEPTHBIAS, &oldBias);
        // D3D9 DEPTHBIAS is multiplied by the minimum resolvable depth (2^-24 on a
        // 24-bit buffer), so a "small" float like -0.00006 rounds to nothing and the
        // digits z-fought the plaque into invisible speckle. -64 => ~-4e-6 real bias.
        float bias = -64.0f;
        d->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&bias);
        V dq[12]; int n = 0;
        for (int g = 0; g + 4 <= g_digitVertCount; g += 4) {
            const NameVert* s = g_digitVerts + g;     // BL, TL, BR, TR
            const int tri[6] = { 0, 1, 2, 2, 1, 3 };
            for (int t = 0; t < 6 && n < 12; ++t) {
                const NameVert& w = s[tri[t]];
                dq[n].x = w.x; dq[n].y = w.y; dq[n].z = w.z;
                dq[n].c = w.c; dq[n].u = w.u; dq[n].v = w.v; ++n;
            }
        }
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, dq, sizeof(V));
        d->SetRenderState(D3DRS_DEPTHBIAS, oldBias);
        static int s_dbgDigitDrawOnce = 1;
        if (s_dbgDigitDrawOnce) {
            s_dbgDigitDrawOnce = 0;
            ofOut << "[barq] digit draw verts=" << n
                  << " p0=(" << dq[0].x << "," << dq[0].y << "," << dq[0].z
                  << ") uv0=(" << dq[0].u << "," << dq[0].v << ")" << std::endl; ofOut.flush();
        }
        g_digitVertCount = 0;
    }
    else if (g_digitVertCount >= 4) {
        static int s_dbgNoTexOnce = 1;
        if (s_dbgNoTexOnce) { s_dbgNoTexOnce = 0;
            ofOut << "[barq] digit draw SKIPPED: no font tex" << std::endl; ofOut.flush(); }
        g_digitVertCount = 0;
    }
    d->SetTransform(D3DTS_WORLD, &prevW);
    d->SetTransform(D3DTS_VIEW, &prevV);
    d->SetTransform(D3DTS_PROJECTION, &prevP);
    d->SetFVF(prevFVF);
    if (prevVS) { d->SetVertexShader(prevVS); prevVS->Release(); }
    d->SetTexture(0, prevTex); if (prevTex) prevTex->Release();
    if (prevVB) { d->SetStreamSource(0, prevVB, prevOff, prevStride); prevVB->Release(); }
    if (++g_dbgBarTexDraws == 1) { ofOut << "[barq] first frame-tex draw" << std::endl; ofOut.flush(); }
}

typedef void(__fastcall* StrDrawFn)(void* str, void* edx);
StrDrawFn sub_5C5D30 = (StrDrawFn)0x005C5D30;
void __fastcall msub_5C5D30(void* str, void* edx)
{
    sub_5C5D30(str, edx);
    if (!g_panelValid) return;
    g_panelValid = 0;
    if (!g_nameBarTexKnob || g_nameBarTid != GetCurrentThreadId()) return;
    __try { DrawBarFrameTex(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void ResolveInkUV(float u0, float v0, float u1, float v1)
{
    if (g_inkResolved) return;
    IDirect3DBaseTexture9* bt = (IDirect3DBaseTexture9*)g_nameBarTex0;
    if (!bt || bt->GetType() != D3DRTYPE_TEXTURE) return;
    IDirect3DTexture9* tex = (IDirect3DTexture9*)bt;
    D3DSURFACE_DESC d;
    if (FAILED(tex->GetLevelDesc(0, &d))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return;
    int x0 = (int)(u0 * d.Width), x1 = (int)(u1 * d.Width);
    int y0 = (int)(v0 * d.Height), y1 = (int)(v1 * d.Height);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > (int)d.Width) x1 = d.Width; if (y1 > (int)d.Height) y1 = d.Height;
    int bestA = -1, bx = 0, by = 0;
    for (int y = y0; y < y1; ++y) {
        const BYTE* row = (const BYTE*)lr.pBits + y * lr.Pitch;
        for (int x = x0; x < x1; ++x) {
            int a = -1;
            switch (d.Format) {
            case D3DFMT_A8R8G8B8: a = row[x * 4 + 3]; break;
            case D3DFMT_A8L8:     a = row[x * 2 + 1]; break;
            case D3DFMT_A8:       a = row[x]; break;
            case D3DFMT_A4R4G4B4: a = (((const WORD*)row)[x] >> 12) * 17; break;
            default: break;
            }
            if (a > bestA) { bestA = a; bx = x; by = y; }
        }
    }
    tex->UnlockRect(0);
    if (bestA >= 0) {
        g_inkU = (bx + 0.5f) / (float)d.Width;
        g_inkV = (by + 0.5f) / (float)d.Height;
        g_inkResolved = true;
        ofOut << "[barq] ink texel alpha=" << bestA << " uv=(" << g_inkU << "," << g_inkV
              << ") fmt=" << d.Format << std::endl; ofOut.flush();
    }
}

static void WeldNameBarQuads(NameVert* v, int n)
{
    // Verified via the [barq] vertex dump (2026-07-16): glyphs are QUADS of 4 vertices
    // each, ordered BL, TL, BR, TR (world-space positions, ARGB color, atlas v grows
    // downward). The Gx draw converts the quad list to triangles itself.
    const int VPG = 4;
    if (n < VPG * NAMEBAR_SEGS || (n % VPG) != 0) { ++g_dbgBarWeldFails; return; }
    int groups = n / VPG;

    if (g_dbgBarDumpOnce) {                             // one-shot layout verification
        g_dbgBarDumpOnce = 0;
        ofOut << "[barq] copy n=" << n << " groups=" << groups << std::endl;
        for (int i = 0; i < 12 && i < n; ++i)
            ofOut << "[barq] v" << i << " p=(" << v[i].x << "," << v[i].y << "," << v[i].z
                  << ") c=" << std::hex << v[i].c << std::dec
                  << " uv=(" << v[i].u << "," << v[i].v << ")" << std::endl;
        ofOut.flush();
    }

    // Per-group UV signature (glyph rect in the atlas). The bar is 16 identical '#'
    // glyphs laid out as the LAST run of this page's stream.
    float sigU0[512], sigV0[512], sigU1[512], sigV1[512];
    if (groups > 512) { ++g_dbgBarWeldFails; return; }
    for (int g = 0; g < groups; ++g) {
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (int i = 0; i < VPG; ++i) {
            NameVert& w = v[g * VPG + i];
            if (w.u < u0) u0 = w.u; if (w.u > u1) u1 = w.u;
            if (w.v < v0) v0 = w.v; if (w.v > v1) v1 = w.v;
        }
        sigU0[g] = u0; sigV0[g] = v0; sigU1[g] = u1; sigV1[g] = v1;
    }
    // The bar line = a run of >=16 identical '#' sigs; up to 2 LEVEL digit glyphs may
    // follow it (equal adjacent digits like "77" form runs of 2, never 16).
    #define SIG_EQ(a, b) (fabsf(sigU0[a]-sigU0[b])<1e-6f && fabsf(sigV0[a]-sigV0[b])<1e-6f && \
                          fabsf(sigU1[a]-sigU1[b])<1e-6f && fabsf(sigV1[a]-sigV1[b])<1e-6f)
    int hEnd = -1;
    {
        int g = groups - 1;
        while (g >= 0) {
            int s = g;
            while (s > 0 && SIG_EQ(s, s - 1)) --s;
            if (g - s + 1 >= NAMEBAR_SEGS) { hEnd = g; break; }
            g = s - 1;
        }
    }
    #undef SIG_EQ
    int nDig = (hEnd >= 0) ? (groups - 1 - hEnd) : 99;
    if (hEnd < 0 || nDig > 2) { ++g_dbgBarWeldFails; return; }
    int first = hEnd - NAMEBAR_SEGS + 1;                // first bar glyph group
    NameVert* bar = v + first * VPG;
    int bn = NAMEBAR_SEGS * VPG;
    g_digitVertCount = 0;

    // Health fraction + colors.
    float frac = g_nameBarFrac;
    float ff = g_nameBarForceFrac;
    if (ff >= 0.0f && ff <= 1.0f) frac = ff;
    if (frac < 0.0f) frac = 0.0f; if (frac > 1.0f) frac = 1.0f;
    DWORD fillC = (frac > 0.5f) ? 0xFF20D020 : (frac > 0.25f) ? 0xFFE0C000 : 0xFFE02020;
    DWORD restC = 0xFF4A1010;   // dark red: the empty part must stay readable on dark walls
    if (g_nameplateMode == 1) {
        // Original-plate mode: the bar is tinted like the stock nameplate — by unit
        // REACTION, not health. The engine already picked that color for the NAME
        // (ctx+0x0C, captured at arm time), so reuse it; near-black empty part.
        fillC = g_nameBarUnitColor;
        restC = 0xFF161616;
    }

    if (!g_nameBarQuad) {                               // recolor-only mode (barquad=0)
        int fillGlyphs = (int)(frac * NAMEBAR_SEGS + 0.5f);
        for (int g = 0; g < NAMEBAR_SEGS; ++g)
            for (int i = 0; i < VPG; ++i)
                bar[g * VPG + i].c = g_nameBarColor ? ((g < fillGlyphs) ? fillC : restC)
                                                    : bar[g * VPG + i].c;
        ++g_dbgBarWelds;
        return;
    }

    // Plane axes of the text: R = along the bar (first->last glyph center),
    // U = vertical from one glyph's own extent.
    float c0[3] = { 0, 0, 0 }, c1[3] = { 0, 0, 0 };
    for (int i = 0; i < VPG; ++i) {
        c0[0] += bar[i].x; c0[1] += bar[i].y; c0[2] += bar[i].z;
        NameVert& w = bar[(NAMEBAR_SEGS - 1) * VPG + i];
        c1[0] += w.x; c1[1] += w.y; c1[2] += w.z;
    }
    for (int a = 0; a < 3; ++a) { c0[a] /= VPG; c1[a] /= VPG; }
    float R[3] = { c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2] };
    float rl = sqrtf(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
    if (!_finite(rl) || rl < 1e-5f) { ++g_dbgBarWeldFails; return; }
    R[0] /= rl; R[1] /= rl; R[2] /= rl;
    // U = component of (vert - center) of glyph 0 orthogonal to R, from the vert with
    // the biggest such offset (a top or bottom corner).
    float U[3] = { 0, 0, 0 }; float best = 0.0f;
    for (int i = 0; i < VPG; ++i) {
        float d[3] = { bar[i].x - c0[0], bar[i].y - c0[1], bar[i].z - c0[2] };
        float pr = d[0] * R[0] + d[1] * R[1] + d[2] * R[2];
        float o[3] = { d[0] - pr * R[0], d[1] - pr * R[1], d[2] - pr * R[2] };
        float ol = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
        if (ol > best) { best = ol; U[0] = o[0]; U[1] = o[1]; U[2] = o[2]; }
    }
    best = sqrtf(best);
    if (!_finite(best) || best < 1e-6f) { ++g_dbgBarWeldFails; return; }
    U[0] /= best; U[1] /= best; U[2] /= best;
    // The winning corner is randomly a top OR a bottom one, so U's sign is arbitrary —
    // and the "drop the panel below the name" offset then randomly went UP onto the
    // name. WoW world Z points up: force U upward so the offset is always downward.
    if (U[2] < 0.0f) { U[0] = -U[0]; U[1] = -U[1]; U[2] = -U[2]; }

    // Extents of the whole 16-glyph line in (R,U) coordinates around c0.
    float rMin = 1e9f, rMax = -1e9f, uMin = 1e9f, uMax = -1e9f;
    for (int i = 0; i < bn; ++i) {
        float d[3] = { bar[i].x - c0[0], bar[i].y - c0[1], bar[i].z - c0[2] };
        float pr = d[0] * R[0] + d[1] * R[1] + d[2] * R[2];
        float pu = d[0] * U[0] + d[1] * U[1] + d[2] * U[2];
        if (pr < rMin) rMin = pr; if (pr > rMax) rMax = pr;
        if (pu < uMin) uMin = pu; if (pu > uMax) uMax = pu;
    }
    // Widths of the trailing level digits, and re-centering: the digits widen the
    // centered text line, shifting the 16 hashes left of the unit — shift the panel
    // back right by half the digit width.
    float digW[2] = { 0, 0 };
    for (int dgi = 0; dgi < nDig; ++dgi) {
        NameVert* dg = v + (hEnd + 1 + dgi) * VPG;
        float mn = 1e9f, mx = -1e9f;
        for (int i = 0; i < VPG; ++i) {
            float dd[3] = { dg[i].x - c0[0], dg[i].y - c0[1], dg[i].z - c0[2] };
            float pr = dd[0] * R[0] + dd[1] * R[1] + dd[2] * R[2];
            if (pr < mn) mn = pr; if (pr > mx) mx = pr;
        }
        digW[dgi] = mx - mn;
    }
    float digShift = (digW[0] + digW[1]) * 0.5f;
    rMin += digShift; rMax += digShift;
    float rFill = rMin + (rMax - rMin) * frac;
    ResolveInkUV(sigU0[first], sigV0[first], sigU1[first], sigV1[first]);
    float inkU = g_inkResolved ? g_inkU : (sigU0[first] + sigU1[first]) * 0.5f;
    float inkV = g_inkResolved ? g_inkV : (sigV0[first] + sigV1[first]) * 0.5f;

    // Corner classes of a template glyph (preserve triangle structure and winding):
    // for each of the 6 verts of glyph 0, remember whether it is left/right, bottom/top.
    struct CC { bool right, top; } cls[6];
    {
        float grMin = 1e9f, grMax = -1e9f, guMin = 1e9f, guMax = -1e9f, gr[6], gu[6];
        for (int i = 0; i < VPG; ++i) {
            float d[3] = { bar[i].x - c0[0], bar[i].y - c0[1], bar[i].z - c0[2] };
            gr[i] = d[0] * R[0] + d[1] * R[1] + d[2] * R[2];
            gu[i] = d[0] * U[0] + d[1] * U[1] + d[2] * U[2];
            if (gr[i] < grMin) grMin = gr[i]; if (gr[i] > grMax) grMax = gr[i];
            if (gu[i] < guMin) guMin = gu[i]; if (gu[i] > guMax) guMax = gu[i];
        }
        float rMid = (grMin + grMax) * 0.5f, uMid = (guMin + guMax) * 0.5f;
        for (int i = 0; i < VPG; ++i) { cls[i].right = gr[i] > rMid; cls[i].top = gu[i] > uMid; }
    }
    // Panel look, N64-style: every rect is DISJOINT (coplanar overlapping quads
    // z-fight because each triangle interpolates its own depth — the user saw tearing).
    //   glyph 0 -> FILL   [rMin..rFill] x inner        (health color)
    //   glyph 1 -> EMPTY  [rFill..rMax] x inner        (dark red, touches fill, no overlap)
    //   glyph 2 -> border TOP    (full outer width strip above inner)
    //   glyph 3 -> border BOTTOM (full outer width strip below inner)
    //   glyph 4 -> border LEFT   (inner-height strip left of inner)
    //   glyph 5 -> border RIGHT  (inner-height strip right of inner)
    //   glyphs 6..15 -> collapsed.
    // Sizing (user feedback): narrower + flatter than the 16-char line and shifted DOWN
    // so the panel never touches the name text above it.
    float h = uMax - uMin;
    {
        float rc = (rMin + rMax) * 0.5f;
        float halfW = (rMax - rMin) * 0.5f * 0.72f;
        rMin = rc - halfW; rMax = rc + halfW;
        rFill = rMin + (rMax - rMin) * frac;
    }
    float uMid = (uMin + uMax) * 0.5f - h * 0.55f;  // drop below the glyph line
    float halfH = h * 0.40f;                        // 0.8x the glyph-line height
    float inU0 = uMid - halfH, inU1 = uMid + halfH;
    float bw = h * 0.22f;                           // border thickness
    const DWORD borderC = 0xFF000000;
    // With the textured frame active the color border strips are skipped — the frame
    // texture (drawn right after this string, transparent center) replaces them.
    int lastBorder = g_nameBarTexKnob ? 1 : 5;
    float fillR0 = rMin, fillR1 = rMax, fillU0 = inU0, fillU1 = inU1;
    if (g_nameBarTexKnob) {
        for (int a = 0; a < 3; ++a) { g_panel.c0[a] = c0[a]; g_panel.R[a] = R[a]; g_panel.U[a] = U[a]; }
        g_panel.r0 = rMin - bw; g_panel.r1 = rMax + bw;
        g_panel.u0 = inU0 - bw; g_panel.u1 = inU1 + bw;
        g_panelValid = 1;
        // The fill/empty rects must live entirely inside the frame art's transparent
        // HOLE — one pixel painted by both the ring and the fill z-fights (verified by
        // the user as flicker). Map the hole (relative to the art, v from top) into
        // panel space and inset it a few percent as a safety margin.
        float hr0, hr1, hv0, hv1;
        if (g_nameplateMode == 1 && g_origHoleValid) {
            hr0 = g_origHoleRel[0]; hv0 = g_origHoleRel[1];
            hr1 = g_origHoleRel[2]; hv1 = g_origHoleRel[3];
        } else {  // procedural frame: 2px ring on a 64x16 canvas
            hr0 = 2.0f / 64; hv0 = 2.0f / 16; hr1 = 62.0f / 64; hv1 = 14.0f / 16;
        }
        float fw = g_panel.r1 - g_panel.r0, fh = g_panel.u1 - g_panel.u0;
        fillR0 = g_panel.r0 + fw * hr0;
        fillR1 = g_panel.r0 + fw * hr1;
        fillU1 = g_panel.u1 - fh * hv0;          // art v grows downward, panel u upward
        fillU0 = g_panel.u1 - fh * hv1;
        float insH = (fillR1 - fillR0) * 0.015f;
        float insV = (fillU1 - fillU0) * 0.12f;
        fillR0 += insH; fillR1 -= insH; fillU0 += insV; fillU1 -= insV;
        // Relocate the level digits onto the plaque (right of the hole) and remove
        // them from the engine's own draw (they'd land right of the bar otherwise).
        if (g_nameplateMode == 1 && g_origHoleValid && nDig > 0) {
            float plaqL = g_panel.r0 + fw * g_origHoleRel[2];
            float plaqR = g_panel.r1 - fw * 0.02f;
            float pcx = (plaqL + plaqR) * 0.5f;
            float pcy = (g_panel.u0 + g_panel.u1) * 0.5f;
            NameVert* d0 = v + (hEnd + 1) * VPG;
            float hmn = 1e9f, hmx = -1e9f;
            for (int i = 0; i < VPG; ++i) {
                float dd[3] = { d0[i].x - c0[0], d0[i].y - c0[1], d0[i].z - c0[2] };
                float pu = dd[0] * U[0] + dd[1] * U[1] + dd[2] * U[2];
                if (pu < hmn) hmn = pu; if (pu > hmx) hmx = pu;
            }
            float srcH = hmx - hmn;
            float tgtH = fh * 0.42f;
            float sc = (srcH > 1e-5f) ? tgtH / srcH : 1.0f;
            float totW = (digW[0] + digW[1]) * sc;
            float x = pcx - totW * 0.5f;
            for (int dgi = 0; dgi < nDig; ++dgi) {
                int gg = hEnd + 1 + dgi;
                float w = digW[dgi] * sc;
                for (int i = 0; i < VPG && g_digitVertCount < 8; ++i) {
                    NameVert& out = g_digitVerts[g_digitVertCount++];
                    float pr = cls[i].right ? (x + w) : x;
                    float pu = cls[i].top ? (pcy + tgtH * 0.5f) : (pcy - tgtH * 0.5f);
                    out.x = c0[0] + R[0] * pr + U[0] * pu;
                    out.y = c0[1] + R[1] * pr + U[1] * pu;
                    out.z = c0[2] + R[2] * pr + U[2] * pu;
                    out.u = cls[i].right ? sigU1[gg] : sigU0[gg];
                    out.v = cls[i].top ? sigV0[gg] : sigV1[gg];
                    out.c = 0xFFFFE080;               // gold, like the stock level text
                }
                x += w;
                for (int i = 0; i < VPG; ++i) {       // degenerate the engine's copy
                    NameVert& w2 = v[gg * VPG + i];
                    w2.x = c0[0]; w2.y = c0[1]; w2.z = c0[2];
                }
            }
            static int s_dbgRelocOnce = 1;
            if (s_dbgRelocOnce) {
                s_dbgRelocOnce = 0;
                ofOut << "[barq] digits relocated n=" << nDig
                      << " verts=" << g_digitVertCount << std::endl; ofOut.flush();
            }
        }
    }
    rFill = fillR0 + (fillR1 - fillR0) * frac;
    for (int g = 0; g < NAMEBAR_SEGS; ++g) {
        float r0, r1, u0, u1; DWORD col;
        switch (g) {
        case 0: r0 = fillR0;    r1 = rFill;     u0 = fillU0;    u1 = fillU1;    col = fillC;   break;
        case 1: r0 = rFill;     r1 = fillR1;    u0 = fillU0;    u1 = fillU1;    col = restC;   break;
        case 2: r0 = rMin - bw; r1 = rMax + bw; u0 = inU1;      u1 = inU1 + bw; col = borderC; break;
        case 3: r0 = rMin - bw; r1 = rMax + bw; u0 = inU0 - bw; u1 = inU0;      col = borderC; break;
        case 4: r0 = rMin - bw; r1 = rMin;      u0 = inU0;      u1 = inU1;      col = borderC; break;
        case 5: r0 = rMax;      r1 = rMax + bw; u0 = inU0;      u1 = inU1;      col = borderC; break;
        default: r0 = r1 = rMin; u0 = u1 = uMin; col = 0; break;
        }
        if (g > 1 && g > lastBorder) { r0 = r1 = rMin; u0 = u1 = uMin; col = 0; }
        for (int i = 0; i < VPG; ++i) {
            NameVert& w = bar[g * VPG + i];
            float pr = cls[i].right ? r1 : r0;
            float pu = cls[i].top ? u1 : u0;
            w.x = c0[0] + R[0] * pr + U[0] * pu;
            w.y = c0[1] + R[1] * pr + U[1] * pu;
            w.z = c0[2] + R[2] * pr + U[2] * pu;
            w.u = inkU; w.v = inkV;
            if (g <= 5) w.c = col;
        }
    }
    ++g_dbgBarWelds;
}

void __fastcall msub_5C6F40(void* chunk, void* edx, NameVert* dst, int page, int srcOff, int count)
{
    sub_5C6F40(chunk, edx, dst, page, srcOff, count);
    if (g_nameBarTid != GetCurrentThreadId() || !dst || count <= 0) return;
    __try { WeldNameBarQuads(dst, count); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ++g_dbgBarWeldFails; }
}

// 0x613CE0 = the unit "should show floating name" virtual (vtbl+0xA4, __thiscall,
// one arg = name-type mask). At 0x613D5E/0x613D6B it returns 0 whenever the unit has
// an ACTIVE NAMEPLATE (unit+0x1130 / castbar unit+0x1134) — that is why names (and
// our bar) vanish in nameplate mode. In occlusion mode we hide the engine plates and
// the bar lives in the name, so: blank the two plate pointers for the duration of the
// check (restored immediately) to keep the name visible for plated units too.
typedef int(__fastcall* ShouldShowNameFn)(void* unit, void* edx, unsigned mask);
ShouldShowNameFn sub_613CE0 = (ShouldShowNameFn)0x00613CE0;
int __fastcall msub_613CE0(void* unit, void* edx, unsigned mask)
{
    if (g_nameplateOcclusion && g_nameplateTextBar && unit) {
        // Remove only the "active nameplate suppresses the name" rule by blanking the
        // plate pointers around the original call. Do NOT return 1 directly for plated
        // units: this virtual answers per name-TYPE (mask bit), and forcing 1 for every
        // mask registered the unit in SEVERAL name lists -> two stacked name strings
        // (one with the bar line, one without) z-fighting = the "bar blinks between two
        // heights" bug the user hit twice.
        __try {
            char* u = (char*)unit;
            int save0 = *(int*)(u + 0x1130);
            int save1 = *(int*)(u + 0x1134);
            *(int*)(u + 0x1130) = 0;
            *(int*)(u + 0x1134) = 0;
            int r = sub_613CE0(unit, edx, mask);
            *(int*)(u + 0x1130) = save0;
            *(int*)(u + 0x1134) = save1;
            return r;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return sub_613CE0(unit, edx, mask);
}

static NameBarSlot* NameBarFindSlot(void* ctx, bool insert)
{
    int h = (int)(((uintptr_t)ctx >> 4) & 127);
    for (int i = 0; i < 128; ++i) {
        NameBarSlot& s = g_nameBarSlots[(h + i) & 127];
        if (s.ctx == ctx) return &s;
        if (!s.ctx && insert) { s.ctx = ctx; s.bucket = -1; return &s; }
    }
    return 0;
}

// Returns true when the injection window was armed (caller must disarm after the
// original returns).
static bool PrepareNameBarInjection(char* ctx)
{
    NameBarHotkeyTick();
    if (!g_nameplateOcclusion || !g_nameBarShow) {
        // Bar just turned OFF (occlusion off, or Ctrl+V "names only" mode): force one
        // engine rebuild to drop our bar line from this entry.
        NameBarSlot* s = NameBarFindSlot(ctx, false);
        if (s) { *(BYTE*)(ctx + 0x18) |= 1; s->ctx = 0; s->bucket = -1; }
        return false;
    }
    unsigned lo = *(unsigned*)(ctx + 0x10);
    unsigned hi = *(unsigned*)(ctx + 0x14);
    if (!(lo | hi)) return false;
    char* unit = (char*)ClntObjMgrObjectPtr(lo, hi, 8, ".\\PlayerName.cpp", 0x64);
    if (!unit) return false;
    float frac = 1.0f;
    int* desc = *(int**)(unit + 0x120);
    if (desc) { int hp = desc[0x40 / 4], mhp = desc[0x58 / 4];
                if (mhp > 0) frac = (float)hp / (float)mhp; }
    if (!_finite(frac) || frac < 0.0f) frac = 1.0f; if (frac > 1.0f) frac = 1.0f;
    int bucket = (int)(frac * (float)NAMEBAR_SEGS + 0.5f);
    if (bucket < 0) bucket = 0; if (bucket > NAMEBAR_SEGS) bucket = NAMEBAR_SEGS;

    NameBarSlot* s = NameBarFindSlot(ctx, true);
    if (s && s->bucket != bucket) {
        s->bucket = bucket;
        *(BYTE*)(ctx + 0x18) |= 1;                     // text-dirty -> engine re-fetches + rebakes
    }
    // The bar TEXT is always 16 '#' — the visible fill/empty split and colors are applied
    // per-VERTEX in the glyph-copy hook (msub_5C6F40), which also welds the 16 glyph quads
    // into one solid rectangle. |c markup is DEAD here: the world-name renderer strips it
    // but applies one color per whole string (verified in-headset), so don't emit it.
    int k = 0;
    g_nameBarText[k++] = '\n';
    for (int i = 0; i < NAMEBAR_SEGS; ++i) g_nameBarText[k++] = '#';
    // LEVEL digits ride along in the same line as real glyphs; the weld relocates
    // their quads onto the frame's golden plaque. UNIT_FIELD_LEVEL sits at +0x88 from
    // the FULL descriptor; unit+0x120 points past the 0x18-byte object header, so it
    // is +0x70 here (health +0x40/+0x58 follow the same convention).
    if (desc) {
        int lvl = desc[0x70 / 4];
        if (lvl > 0 && lvl < 100) {
            if (lvl >= 10) g_nameBarText[k++] = (char)('0' + lvl / 10);
            g_nameBarText[k++] = (char)('0' + lvl % 10);
        }
    }
    g_nameBarText[k] = 0;
    g_nameBarFrac = frac;
    g_nameBarUnitColor = 0xFF000000 | (*(DWORD*)(ctx + 0x0C) & 0x00FFFFFF);
    g_nameBarTid = GetCurrentThreadId();
    return true;
}

void(__cdecl* sub_6D5320)(void*, void*, void*) = (void(__cdecl*)(void*, void*, void*))0x006D5320;
void __cdecl msub_6D5320(void* model, void* arg, void* ctx)
{
    bool armed = false;
    if (ctx && g_nameplateTextBar) {
        __try { armed = PrepareNameBarInjection((char*)ctx); }
        __except (EXCEPTION_EXECUTE_HANDLER) { armed = false; }
    }
    sub_6D5320(model, arg, ctx);                       // draw the engine name (with our bar line)
    if (armed) { g_nameBarTid = 0; g_nameBarText[0] = 0; ++g_dbgNameDraws; }
    if (g_nameplateTextBar) return;                    // text-bar mode: no own geometry at all
    if (!g_nameplateOcclusion || !devDX9 || !ctx) return;
    if (curEye != 0 && curEye != 1) return;
    __try {
        // Capture the LIVE matrices right here — mid-scene these are exactly the
        // matrices the geometry depth was written with, so our z matches the buffer.
        char* gx = (char*)*(int**)0xD2A15C;
        if (!gx) return;
        int vsIdx = *(int*)(gx + 0x1A7C);
        if (vsIdx < 0 || vsIdx > 63) return;
        memcpy(g_wsView[curEye], gx + 0x1A84 + 64 * vsIdx, 64);
        memcpy(g_wsProj[curEye], gx + 0xF4C, 64);
        g_wsMtxValid[curEye] = true;
        // Camera world position for the camera-relative transform.
        int cam = GetActiveCameraSafe();
        if (cam) { memcpy(g_wsCamPos[curEye], (const void*)(cam + 0x08), 12);
                   g_wsCamValid[curEye] = true; }

        unsigned lo = *(unsigned*)((char*)ctx + 0x10);
        unsigned hi = *(unsigned*)((char*)ctx + 0x14);
        if (!(lo | hi)) return;
        char* unit = (char*)ClntObjMgrObjectPtr(lo, hi, 8, ".\\PlayerName.cpp", 0x64);
        if (!unit) return;

        WsPlateData wsd;
        // head anchor + engine name lift
        typedef void(__thiscall* UnitPosFn)(void*, float*);
        UnitPosFn posFn = *(UnitPosFn*)(*(char**)unit + 0x18);
        if (!posFn) return;
        wsd.pos[0] = wsd.pos[1] = wsd.pos[2] = 0.0f;
        posFn(unit, wsd.pos);
        float lift = (float)(sub_6D44D0(unit) * (*(double*)0x8B0AA0));
        if (!_finite(lift) || lift < 0.0f || lift > 50.0f) lift = (float)(*(double*)0x8C4CF8);
        wsd.pos[2] += lift;
        // health fraction from the unit descriptor (V off -> no plate node)
        float frac = 1.0f;
        int* desc = *(int**)(unit + 0x120);
        if (desc) { int hp = desc[0x40 / 4], mhp = desc[0x58 / 4];
                    if (mhp > 0) frac = (float)hp / (float)mhp; }
        if (!_finite(frac) || frac < 0.0f) frac = 1.0f; if (frac > 1.0f) frac = 1.0f;
        wsd.frac = frac;
        DWORD rgb = (frac > 0.5f) ? 0x0020C020 : (frac > 0.25f) ? 0x00E0C000 : 0x00D01010;
        wsd.fillColor = 0xD9000000 | rgb;
        // matrices / billboard / camera
        const float* V = g_wsView[curEye]; const float* P = g_wsProj[curEye];
        for (int i = 0; i < 16; ++i) { wsd.view[i] = V[i]; wsd.proj[i] = P[i]; }
        wsd.camValid = g_wsCamValid[curEye];
        if (wsd.camValid) { wsd.cam[0]=g_wsCamPos[curEye][0]; wsd.cam[1]=g_wsCamPos[curEye][1]; wsd.cam[2]=g_wsCamPos[curEye][2]; }
        wsd.right[0]=V[0]; wsd.right[1]=V[4]; wsd.right[2]=V[8];
        wsd.up[0]=V[1]; wsd.up[1]=V[5]; wsd.up[2]=V[9];
        for (int a = 0; a < 2; ++a) { float* w = (a==0)?wsd.right:wsd.up;
            float len = w[0]*w[0]+w[1]*w[1]+w[2]*w[2];
            if (!_finite(len) || len < 1e-6f) return; len = sqrtf(len);
            w[0]/=len; w[1]/=len; w[2]/=len; }

        D3DVIEWPORT9 vp; if (FAILED(devDX9->GetViewport(&vp))) return;
        DrawWsPlateQuads(&wsd, (float)vp.Width, (float)vp.Height);
        ++g_dbgNameDraws;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// OBSOLETE legacy path (screen-space z-test bracket), disabled by this constant but
// kept compiled to avoid churn until the world-space plates are confirmed in-game.
static volatile bool g_useLegacyPlateZTest = false;

// Nameplate occlusion draw bracket. Depth reaches the pixels via the flush-time
// vertex-z patch (msub_42F390), which is active only while g_plateZTest is set here.
// Save all depth states, force a read-only LESSEQUAL test for the draw, then restore
// them verbatim. Unknown/unmatched frames keep engine z=0.0 and stay on top, so the
// z-tested draw can never hide anything by mistake; fall back to the plain engine
// draw only when there is no depth data at all or a device state call fails.
static void DrawWorldListPlatesWithDepth(int listPtr, int eye)
{
    if (!devDX9 || !listPtr || (eye != 0 && eye != 1)) {
        ++g_plateOcclStateFallback;
        sub_494F30(listPtr);
        return;
    }

    // Skip the z-tested draw only when NO plate has a computed depth (nothing to
    // occlude -> the plain draw is identical). Eye 1's own depths are computed inside
    // the wrapped call, so eye-0 results decide for both (depth is eye-independent).
    bool anyDepth = false;
    for (int i = 0; i < g_plateLayoutCount; ++i) {
        if (g_plateLayout[i].depthValid[0] || g_plateLayout[i].depthValid[1]) {
            anyDepth = true;
            ++g_plateOcclReadyAtDraw;
        }
    }
    if (!anyDepth) {
        ++g_plateOcclDataFallback;
        sub_494F30(listPtr);
        return;
    }

    IDirect3DDevice9* d = devDX9;
    DWORD savedZEnable = FALSE, savedZWrite = FALSE, savedZFunc = D3DCMP_LESSEQUAL;
    HRESULT getZE = d->GetRenderState(D3DRS_ZENABLE, &savedZEnable);
    HRESULT getZW = d->GetRenderState(D3DRS_ZWRITEENABLE, &savedZWrite);
    HRESULT getZF = d->GetRenderState(D3DRS_ZFUNC, &savedZFunc);
    if (FAILED(getZE) || FAILED(getZW) || FAILED(getZF)) {
        ++g_plateOcclStateFallback;
        sub_494F30(listPtr);
        return;
    }

    HRESULT setZE = d->SetRenderState(D3DRS_ZENABLE, TRUE);
    HRESULT setZW = d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    HRESULT setZF = d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    if (FAILED(setZE) || FAILED(setZW) || FAILED(setZF)) {
        // g_plateZTest is still false, so an old ZFUNC=ALWAYS restores exactly.
        d->SetRenderState(D3DRS_ZENABLE, savedZEnable);
        d->SetRenderState(D3DRS_ZWRITEENABLE, savedZWrite);
        d->SetRenderState(D3DRS_ZFUNC, savedZFunc);
        ++g_plateOcclStateFallback;
        sub_494F30(listPtr);
        return;
    }

    ++g_plateOcclDraws;
    g_plateZTest = true;  // DrawBatch's ZFUNC=ALWAYS is rewritten by base AND Ex proxies.
    sub_494F30(listPtr);
    g_plateZTest = false;

    HRESULT restoreZE = d->SetRenderState(D3DRS_ZENABLE, savedZEnable);
    HRESULT restoreZW = d->SetRenderState(D3DRS_ZWRITEENABLE, savedZWrite);
    HRESULT restoreZF = d->SetRenderState(D3DRS_ZFUNC, savedZFunc);
    if (FAILED(restoreZE) || FAILED(restoreZW) || FAILED(restoreZF))
        ++g_plateOcclStateFallback;
}

void(__fastcall msub_495410)(void* ecx, void* edx)
{
    static int srHits = 0;
    if (srHits < 3) { ofOut << "[hook] StartRender #" << srHits << " (stereo=" << g_step2_StereoStartRender << ")" << std::endl; ofOut.flush(); }
    srHits++;
    if (svr->isEnabled() && g_step2_StereoStartRender)
    {
        HRESULT result = S_OK;
        D3DVIEWPORT9 hViewport, uViewport, mViewport;
        XMMATRIX identityMatrix = XMMatrixIdentity();

        hViewport.X = 0;
        hViewport.Y = 0;
        hViewport.Width = hmdBufferSize.x;
        hViewport.Height = hmdBufferSize.y;
        hViewport.MinZ = 0.0f;
        hViewport.MaxZ = 1.0f;

        uViewport.X = 0;
        uViewport.Y = 0;
        uViewport.Width = uiBufferSize.x;
        uViewport.Height = uiBufferSize.y;
        uViewport.MinZ = 0.0f;
        uViewport.MaxZ = 1.0f;

        mViewport.X = 0;
        mViewport.Y = 0;
        mViewport.Width = uiBufferSize.x / 4;
        mViewport.Height = uiBufferSize.y / 4;
        mViewport.MinZ = 0.0f;
        mViewport.MaxZ = 1.0f;

        float a[] = { 0, 0, 0, 0 };
        float b[] = { 0, 0 };
        int esi = (int)ecx + 0x0CE0;  // fix #35: TBC frame list (3.3.5: +0xCE4)
        int dxLoc = *(int*)(TBC_g_GxDevicePtr);

        //(float*)((int)ecx + 0x44)
        //a[0] = 0.1f; // fov values?
        //a[1] = 0.1f;
        //a[2] = 0.15f;
        //a[3] = 0.88f;

        b[0] = 0.0f; // screen coord center x,y offset -1:1
        b[1] = 0.0f; 

        //sub_4BEE60(a, b, 0, 0);
        sub_4BEE60((float*)((int)ecx + 0x40), b, 0);  // fix #35: TBC fov array at +0x40 (3.3.5: +0x44), 3-arg call

        //----
        // Checks to see if were in the game world or ui
        //----
        int frameWorld = *(int*)(TBC_g_WorldFrame);

        // fix #74B: arm the once-per-frame center capture. The next camera update
        // (warm-up / left pass) captures the cyclopean center camera position; both
        // eyes reuse it, so the depth-compression baseline is identical per eye.
        g_centerCaptureArmed = true;
        g_flatValid = false;   // re-arm the once-per-frame flat-cache snapshot

        // fix #61: synchronous world-scene cull before the stereo eye loop.
        // The engine runs its world cull once per game frame in the UPDATE phase
        // (0x684AA0 via 0x6986F0), BEFORE this StartRender hook: it latches the
        // camera into 0xDA5B70 / 0xDA5B7C, clears the sky flags, then fills the
        // visible-area lists the terrain render consumes. That fill completes
        // asynchronously; when completion lands between our two eye passes, the
        // left eye renders from an incomplete list. Re-run the cull here from the
        // engine's own latched values (passing them is what the engine callsite
        // effectively does; the camAux copy mirrors the callsite's local copy) so
        // both eyes see a fully-built visible-area list. Gated by frameWorld
        // (in-world check) plus a CMap-present + finite-camera sanity check.
        if (g_fix61_PrePassCull && frameWorld)
        {
            int cmap = *(int*)0x00DA43EC;
            float* camPos = (float*)0x00DA5B70;
            float* camDir = (float*)0x00DA5B7C;
            bool camSane = _finite(camPos[0]) && _finite(camPos[1]) && _finite(camPos[2]) && (camPos[0] != 0.0f || camPos[1] != 0.0f || camPos[2] != 0.0f);
            if (cmap && camSane) {
                float camAux[3] = { camPos[0], camPos[1], camPos[2] };
                sub_WorldSceneCull(camPos, camDir, camAux);
            }
        }

        std::tuple<int, IDirect3DSurface9*, IDirect3DSurface9*, int, int, D3DVIEWPORT9, D3DCOLOR> bufferList[] = { // left eye, right eye, ui
            std::make_tuple(textureIndex + 0, BackBuffer[textureIndex + 0].pShaderResource, DepthBuffer[textureIndex + 0].pDepthStencilView, 0, 1, hViewport, D3DCOLOR_ARGB(0, 0, 0, 0)),
            std::make_tuple(textureIndex + 3, BackBuffer[textureIndex + 3].pShaderResource, DepthBuffer[textureIndex + 3].pDepthStencilView, 0, 1, hViewport, D3DCOLOR_ARGB(0, 0, 0, 0)),
            std::make_tuple(0, uiRender.pShaderResource, uiDepth.pDepthStencilView, 1, 9, uViewport, D3DCOLOR_ARGB(0, 0, 0, 0))
        };
        int tIndex = 0;
        IDirect3DSurface9* renderTarget = NULL;
        IDirect3DSurface9* depthBuffer = NULL;
        int frameStart = 0;
        int frameStop = 0;
        D3DVIEWPORT9 viewport;
        D3DCOLOR clearColor = D3DCOLOR_ARGB(0, 0, 0, 0);

        int start = 0;
        int stop = 3;

        // fix #59: remember the engine's per-frame purge arm (list+4) captured on the
        // left-eye pass, keyed by world list index i, so we can run it manually once
        // after the right-eye pass. World passes only cover i in [0,1), size for safety.
        int savedPurge[16] = { 0 };

        // fix #58-instr probe: count a scene list's one-frame-batch chain exactly like the
        // engine walk (array of frame items at list+0x14, count at list+8; chain head at
        // frame+0x118, pool at frame+0x110). Fully null-guarded and capped so a wrong
        // layout can only produce a bogus number, never a crash or hang.
        auto countBatches = [](int listPtr) -> int {
            int total = 0;
            if (!listPtr) return 0;
            int arr = *(int*)(listPtr + 0x14);
            int cnt = *(int*)(listPtr + 8);
            if (!arr || cnt <= 0 || cnt > 100000) return 0;
            for (int k = 0; k < cnt; k++) {
                int f = *(int*)(arr + k * 4);
                if (!f) continue;
                int pool = *(int*)(f + 0x110);
                unsigned u = *(unsigned*)(f + 0x118);
                int guard = 0;
                while (u && !(u & 1) && pool && guard < 100000) {
                    total++;
                    u = *(unsigned*)(pool + u + 4);
                    guard++;
                }
            }
            return total;
        };

        // fix #67: throwaway warm-up world render before the per-eye loop. The FIRST world
        // traversal of a game frame always comes out missing sky+far content (its async
        // completion lands mid-frame); the SECOND is always complete. Running one extra
        // throwaway world pass here makes the real left eye (j==0) effectively the second,
        // complete render. Modeled on the j==0 pass body but stripped of all diagnostics:
        // it binds the LEFT eye surfaces (overwritten immediately by the real left pass),
        // renders into a tiny 8x8 viewport (GPU cost near zero; the CPU list walk is what
        // primes the async completion), and is NOT recorded (g_recEye = -1).
        // fix #69 timing: measure the four render sections with QueryPerformanceCounter.
        // Low overhead - one QPC read per section boundary. Averaged over 600 frames in OnPaint.
        static LARGE_INTEGER s_qpcFreq = { 0 };
        if (s_qpcFreq.QuadPart == 0) QueryPerformanceFrequency(&s_qpcFreq);
        double secW = 0, secL = 0, secR = 0, secUI = 0;
        LARGE_INTEGER qA, qPrev;
        QueryPerformanceCounter(&qA);

        // fix #69: gate the warm-up pass on being in-world (frameWorld != 0). Out of the
        // world (login/loading) there is no world traversal to prime, so the extra pass is
        // pure waste there.
        if (g_fix67_WarmupPass && frameWorld)
        {
            curEye = 0;
            g_recEye = -1;  // do NOT record the warm-up pass

            int wIndex; IDirect3DSurface9* wRT; IDirect3DSurface9* wDepth;
            int wFrameStart; int wFrameStop; D3DVIEWPORT9 wViewportUnused; D3DCOLOR wClear;
            // bind the LEFT eye render target + depth (bufferList[0] values); its content
            // gets overwritten by the real left pass right after, so no scratch texture needed.
            std::tie(wIndex, wRT, wDepth, wFrameStart, wFrameStop, wViewportUnused, wClear) = bufferList[0];

            // TINY viewport: copy the j==0 viewport struct but force an 8x8 render area
            // (X=Y=0, MinZ/MaxZ as in the original hViewport).
            D3DVIEWPORT9 wViewport = hViewport;
            wViewport.X = 0;
            wViewport.Y = 0;
            wViewport.Width = 8;
            wViewport.Height = 8;

            // fix #35: TBC viewport block is at +0x168..0x184 (3.3.5: +0x164..0x180)
            *(float*)(dxLoc + 0x168) = 0;
            *(float*)(dxLoc + 0x16C) = 0;
            *(float*)(dxLoc + 0x170) = (float)wViewport.Height;
            *(float*)(dxLoc + 0x174) = (float)wViewport.Width;
            *(float*)(dxLoc + 0x178) = 0;
            *(float*)(dxLoc + 0x17C) = 0;
            *(float*)(dxLoc + 0x180) = (float)wViewport.Height;
            *(float*)(dxLoc + 0x184) = (float)wViewport.Width;

            *(int*)(curBackBufferLoc) = (int)wRT;
            *(int*)(curBackBufferLoc + 0x4) = (int)wDepth;
            result = devDX9->SetRenderTarget(0, wRT);
            result = devDX9->SetDepthStencilSurface(wDepth);
            result = devDX9->SetViewport(&wViewport);
            result = devDX9->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, wClear, 1.0f, 0);

            // WORLD lists only (same frameStart/frameStop as bufferList[0]).
            g_warmupSkipDraws = 1;  // fix #71: proxy swallows all draw calls for this walk (CPU-side engine work still runs)
            for (int i = wFrameStart; i < wFrameStop; i++)
            {
                int tBool = *(int*)((int)ecx + 0x80);  // fix #35: TBC (3.3.5: +0x7C)
                int tesi = *(int*)(esi + (i * 4));

                // fix #59: purge-suppress logic, mirrored EXACTLY from the j==0 eye pass.
                if (g_fix59_DeferPurge)
                {
                    if (i < 16) savedPurge[i] = *(int*)(tesi + 4);  // remember engine's arm on left eye
                    *(int*)(tesi + 4) = 0;                          // suppress purge for this eye pass
                }

                if (!g_fix69_NoForcedRebuild) *(int*)tesi = 1;  // fix #66 arm before warm-up pass; fix #69 skips the forced rebuild
                sub_494EE0(tesi, tBool == 0);
                sub_494F30(tesi);
            }
            g_warmupSkipDraws = 0;  // fix #71: real eye passes draw normally again
        }

        // fix #69 timing: warm-up section done (whether or not it ran); qPrev now marks the
        // start of the per-eye loop.
        QueryPerformanceCounter(&qPrev);
        secW = (double)(qPrev.QuadPart - qA.QuadPart) * 1000.0 / (double)s_qpcFreq.QuadPart;

        // Run Frames
        for (int j = start; j < stop; j++)
        {
            if (g_test_SkipRightEye && j == 1) continue;  // TEST: right eye world pass disabled
            curEye = j;
            g_recEye = (j <= 1) ? j : -1;  // fix #63-instr: record only the two world eye passes, not the UI pass (j==2)
            std::tie(tIndex, renderTarget, depthBuffer, frameStart, frameStop, viewport, clearColor) = bufferList[j];

            // fix #35: TBC viewport block is at +0x168..0x184 (3.3.5: +0x164..0x180)
            *(float*)(dxLoc + 0x168) = 0;
            *(float*)(dxLoc + 0x16C) = 0;
            *(float*)(dxLoc + 0x170) = (float)viewport.Height;
            *(float*)(dxLoc + 0x174) = (float)viewport.Width;
            *(float*)(dxLoc + 0x178) = 0;
            *(float*)(dxLoc + 0x17C) = 0;
            *(float*)(dxLoc + 0x180) = (float)viewport.Height;
            *(float*)(dxLoc + 0x184) = (float)viewport.Width;

            *(int*)(curBackBufferLoc) = (int)renderTarget;
            *(int*)(curBackBufferLoc + 0x4) = (int)depthBuffer;
            result = devDX9->SetRenderTarget(0, renderTarget);
            result = devDX9->SetDepthStencilSurface(depthBuffer);
            result = devDX9->SetViewport(&viewport);
            result = devDX9->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);

            // fix #62: we just bound this pass's render target DIRECTLY on the D3D device,
            // bypassing CGxDevice::RenderTargetSet (0x5A48B0). That leaves the engine's
            // RT slot cache (CGxDev+0x27FC/0x2800 slot0, +0x2808/0x280C slot1) pointing at
            // the target from the PREVIOUS pass. A mid-scene engine effect pass that
            // binds+restores via RenderTargetSet would then early-out on the restore and
            // leave the wrong target bound for the rest of this eye. Force a cache mismatch
            // (and mark the viewport dirty) so the next engine RenderTargetSet actually
            // issues the D3D call. Applies to ALL passes (j=0,1,2). We intentionally do NOT
            // touch +0x2804/+0x2810 (AddRef'd held refs — the engine releases those itself).
            if (g_fix62_RTCacheInvalidate) {
                int gxDev = *(int*)(TBC_g_GxDevicePtr);
                if (gxDev) {
                    *(int*)(gxDev + 0x27FC) = 0xFFFFFFFF;  // slot0 cached rtObj -> force mismatch
                    *(int*)(gxDev + 0x2800) = 0xFFFFFFFF;  // slot0 cached face
                    *(int*)(gxDev + 0x2808) = 0xFFFFFFFF;  // slot1 cached rtObj
                    *(int*)(gxDev + 0x280C) = 0xFFFFFFFF;  // slot1 cached face
                    *(int*)(gxDev + 0xEF0)  = 1;           // viewport dirty -> re-apply from +0xEF4 block at next flush
                }
            }

            // fix #58-instr: snapshot the sky-gating globals for this eye immediately
            // before its world render call. eye0 (left) is the pass rendering without
            // sky/far-scenery; comparing eye0 vs eye1 shows which gate flipped.
            if (j <= 1)
            {
                g_skyDbg[j][0] = *(int*)(TBC_g_SkyVisibleFlag);    // vis
                g_skyDbg[j][1] = *(int*)(TBC_g_AreaLightOverride); // ovr
                g_skyDbg[j][2] = *(int*)(TBC_g_CurrentZoneLight);  // zone
                g_skyDbg[j][3] = *(int*)(TBC_g_DNSkiesEnabled);    // dn
                g_skyDbg[j][4] = *(int*)(TBC_g_CurrentSkyObj);     // sky
                // fix #61-instr: raw visible-list head/tail as the engine left them
                // going into this eye's render. These are the meaningful signal (the
                // fill/cmap hit counters can include engine-update-phase hits).
                g_visListDbg[j][0] = *(int*)0x00DA81C0;  // head
                g_visListDbg[j][1] = *(int*)0x00DA81C4;  // tail
            }

            for (int i = frameStart; i < frameStop; i++)
            {
                int tBool = *(int*)((int)ecx + 0x80);  // fix #35: TBC (3.3.5: +0x7C)
                int tesi = *(int*)(esi + (i * 4));

                // fix #58-instr probe: snapshot the dirty (list+0) and purge (list+4) flags
                // exactly as the engine left them, BEFORE the fix #56 / fix #59 writes below,
                // and count the batch chain length going into the render call.
                if (j <= 1) {
                    g_batchDbg[j][2] = *(int*)tesi;        // D = dirty flag as read
                    g_batchDbg[j][3] = *(int*)(tesi + 4);  // P = purge flag as read
                    g_batchDbg[j][0] = countBatches(tesi); // pre
                }

                // fix #59: the engine arms the purge flag at list+4 once per game frame.
                // sub_494EE0 -> 0x43C7A0 purges on the FIRST pass that sees it, destroying the
                // one-frame batches (terrain chunks + sky) recorded by the previous pass so that
                // eye renders without them. Suppress the purge for BOTH eye passes, then run it
                // MANUALLY once after the right-eye pass has walked the chain. (Verified in the
                // loop: this world list index i is NOT re-processed by the UI pass j==2, so
                // restoring the flag would purge next frame's left eye -> manual purge instead.)
                if (g_fix59_DeferPurge && j <= 1)
                {
                    if (j == 0 && i < 16) savedPurge[i] = *(int*)(tesi + 4);  // remember engine's arm on left eye
                    *(int*)(tesi + 4) = 0;                                    // suppress purge for this eye pass
                }

                if (j == 0 && !g_fix69_NoForcedRebuild) *(int*)tesi = 1;  // fix #66: arm rebuild only before the FIRST eye pass - a rebuild during
                                              // pass 2 can overwrite dynamic buffers still referenced by pass 1's
                                              // in-flight draws (left-eye terrain/sky stomp)
                                              // (was fix #56: armed for BOTH eye passes j==0 left, j==1 right;
                                              // NOT the UI pass j==2)
                                              // fix #69: skipped entirely when g_fix69_NoForcedRebuild - the engine's own
                                              // once-per-frame arm (consumed by the warm-up pass) is enough; this extra
                                              // arm forced a redundant full scene rebuild each frame (CPU cost).
                sub_494EE0(tesi, tBool == 0);
                // WORLD-SPACE REDESIGN: the screen-space z-test bracket is DISABLED
                // (g_useLegacyPlateZTest=false; kept compiled, not deleted). Plates
                // are hidden at flush time (msub_42F390) and re-drawn world-space in
                // the name pipeline slot (msub_6D5320), which z-tests per pixel
                // without rewriting any engine state.
                if (g_useLegacyPlateZTest && g_nameplateOcclusion && j <= 1 && i == 0 && g_plateLayoutCount > 0) {
                    DrawWorldListPlatesWithDepth(tesi, j);
                } else {
                    sub_494F30(tesi);
                }
                if (j <= 1) g_batchDbg[j][1] = countBatches(tesi);  // post = chain length after the render call

                // fix #59: after the right-eye pass has walked the chain, run the purge manually
                // once for each world list the engine had armed, then leave list+4 cleared.
                if (g_fix59_DeferPurge && j == 1 && i < 16 && savedPurge[i] != 0)
                {
                    sub_43C720_purge(tesi);
                    *(int*)(tesi + 4) = 0;
                }
            }

            // WORLD-SPACE NAMEPLATES: draw all live plates for this eye now — after
            // every world list has rendered, the eye's scene depth buffer is complete
            // and still bound, so the plates' ZENABLE draw occludes per pixel.
            if (g_nameplateOcclusion && !g_nameplateTextBar && (j == 0 || j == 1))
                DrawAllWsPlates(j, (float)hViewport.Width, (float)hViewport.Height);

            // VR aim crosshair: draw the gaze-center reticle into THIS eye's render target
            // while it (and hViewport) are still bound. World eye passes only (j==0 left,
            // j==1 right); NOT the UI pass (j==2). The VR frustum is asymmetric, so the
            // geometric RT center is NOT the visual/gaze center: two offset crosses. Place
            // the reticle at the projection PRINCIPAL POINT derived from this eye's projection
            // matrix (row-3 negation the mod applies elsewhere cancels in _31/_34), plus a
            // small per-eye fine nudge (g_crosshairOffset).
            if (j == 0 || j == 1) {
                float w34 = matProjection[curEye]._34;
                float ndcx = (w34 != 0.0f) ? (matProjection[curEye]._31 / w34) : 0.0f;
                float ndcy = (w34 != 0.0f) ? (matProjection[curEye]._32 / w34) : 0.0f;
                float cx = (0.5f + 0.5f * ndcx) * hViewport.Width + (curEye == 0 ? g_crosshairOffset : -g_crosshairOffset);
                float cy = (0.5f - 0.5f * ndcy) * hViewport.Height + g_crosshairYOffset;
                DrawCrosshair(cx, cy);
            }

            // fix #62 proof probe: at the END of each eye's world pass, ask the D3D device
            // which render target is ACTUALLY bound to slot 0. If it still equals the eye
            // surface we bound going in (renderTarget), the engine never hijacked it during
            // the scene render (ok). If it differs, a mid-scene effect pass rebound it and
            // never restored our eye target (HIJACKED) — exactly the fix #62 failure mode.
            if (j <= 1 && devDX9) {
                IDirect3DSurface9* curRT = nullptr;
                if (SUCCEEDED(devDX9->GetRenderTarget(0, &curRT)) && curRT) {
                    g_rtProbe[j] = (curRT == renderTarget) ? 1 : 0;
                    curRT->Release();
                }
            }

            // fix #69 timing: record this pass's duration (world render calls only). j==0 -> L,
            // j==1 -> R, j==2 -> UI world pass (the overlay tail is added to secUI later).
            LARGE_INTEGER qNow; QueryPerformanceCounter(&qNow);
            double sec = (double)(qNow.QuadPart - qPrev.QuadPart) * 1000.0 / (double)s_qpcFreq.QuadPart;
            if (j == 0) secL = sec; else if (j == 1) secR = sec; else secUI = sec;
            qPrev = qNow;
        }

        g_recEye = -1;  // fix #63-instr: stop recording once the two world eye passes are done

        // fix #64-instr: dump point A = PRE-UI. Both world eye passes (j==0 left, j==1
        // right) are complete; the UI / cursor overlay has NOT been drawn yet. This shows
        // the raw stereo world render (terrain/sky) before anything is composited on top.
        if (g_dumpEyesRequested)
        {
            DumpSurfaceToBMP(BackBuffer[textureIndex].pShaderResource,     "./vr_version/eyedump_L_A.bmp");
            DumpSurfaceToBMP(BackBuffer[textureIndex + 3].pShaderResource, "./vr_version/eyedump_R_A.bmp");
        }

        devDX9->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        devDX9->SetRenderState(D3DRS_ZENABLE, TRUE);
        devDX9->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        //----
        // Renders masked ui
        //----
        result = devDX9->SetRenderTarget(0, uiRenderMask.pShaderResource);
        result = devDX9->SetDepthStencilSurface(NULL);
        result = devDX9->SetViewport(&mViewport);
        result = devDX9->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

        result = devDX9->SetVertexShaderConstantF(0, &identityMatrix._11, 4);
        result = devDX9->SetVertexShaderConstantF(4, &identityMatrix._11, 4);
        result = devDX9->SetVertexShaderConstantF(8, &identityMatrix._11, 4);
        result = devDX9->SetTexture(0, uiRender.pTexture);
        maskedUI.Render();
        //devDX9->StretchRect(uiRender.pShaderResource, NULL, uiRenderMask.pShaderResource, NULL, D3DTEXF_NONE);

        devDX9->SetFVF(NULL);
        devDX9->SetRenderState(D3DRS_LIGHTING, FALSE);
        devDX9->SetRenderState(D3DRS_ZENABLE, (doOcclusion) ? TRUE : FALSE);
        devDX9->SetRenderState(D3DRS_ZWRITEENABLE, (doOcclusion) ? TRUE : FALSE);
        devDX9->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        devDX9->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        devDX9->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        //devDX9->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        devDX9->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        devDX9->SetRenderState(D3DRS_ALPHAREF, 0x00);
        devDX9->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        devDX9->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        devDX9->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);


        XMMATRIX projectionMatrix = XMMatrixIdentity();
        XMMATRIX viewMatrix = XMMatrixIdentity();
        XMMATRIX worldMatrix = XMMatrixIdentity();

        if (cfg_disableControllers)
        {

            //----
            // Render cursor to to ui windows
            //----
            std::tie(tIndex, renderTarget, depthBuffer, frameStart, frameStop, viewport, clearColor) = bufferList[2];
            *(int*)(curBackBufferLoc) = (int)renderTarget;
            *(int*)(curBackBufferLoc + 0x4) = (int)depthBuffer;
            result = devDX9->SetRenderTarget(0, renderTarget);
            result = devDX9->SetDepthStencilSurface(depthBuffer);
            result = devDX9->SetViewport(&viewport);
            //result = devDX9->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 255, 255, 255), 1.0f, 0);

            //----
            // Renders mouseover cursor
            //----
            worldMatrix = cursorUI.GetObjectMatrix(false, true);
            result = devDX9->SetVertexShaderConstantF(0, &projectionMatrix._11, 4);
            result = devDX9->SetVertexShaderConstantF(4, &viewMatrix._11, 4);
            result = devDX9->SetVertexShaderConstantF(8, &worldMatrix._11, 4);
            result = devDX9->SetTexture(0, cursor.pTexture);
            cursorUI.Render();  // fix #38: this IS the VR cursor — ungated (fix #36 over-gated it)
            
            for (int i = 0; i < 2; i++)
            {
                //----
                // Render ui to game windows
                //----
                std::tie(tIndex, renderTarget, depthBuffer, frameStart, frameStop, viewport, clearColor) = bufferList[i];
                *(int*)(curBackBufferLoc) = (int)renderTarget;
                *(int*)(curBackBufferLoc + 0x4) = (int)depthBuffer;
                result = devDX9->SetRenderTarget(0, renderTarget);
                result = devDX9->SetDepthStencilSurface(depthBuffer);
                result = devDX9->SetViewport(&viewport);
                //result = devDX9->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 255, 255, 255), 1.0f, 0);

                XMMATRIX hmdData = matEyeOffset[i] * matHMDPos;
                projectionMatrix = XMMatrixTranspose(matProjection[i]);
                viewMatrix = XMMatrixTranspose(XMMatrixInverse(0, (hmdData)));
                worldMatrix = XMMatrixIdentity();
                projectionMatrix._33 = cfg_uiOffsetD;// -0.938f;
                //projectionMatrix._34 =  -0.06f;

                devDX9->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
                devDX9->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                devDX9->SetRenderState(D3DRS_ZENABLE, FALSE);

                devDX9->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

                devDX9->SetRenderState(D3DRS_ZENABLE, (doOcclusion) ? TRUE : FALSE);

                worldMatrix = curvedUI.GetObjectMatrix(false, true);
                result = devDX9->SetVertexShaderConstantF(0, &projectionMatrix._11, 4);
                result = devDX9->SetVertexShaderConstantF(4, &viewMatrix._11, 4);
                result = devDX9->SetVertexShaderConstantF(8, &worldMatrix._11, 4);
                result = devDX9->SetTexture(0, uiRender.pTexture);
                curvedUI.Render();

                devDX9->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            }
        }
        else
        {
            RunFrameUpdateController();

            //----
            // Render cursor to to ui windows
            //----
            std::tie(tIndex, renderTarget, depthBuffer, frameStart, frameStop, viewport, clearColor) = bufferList[2];
            *(int*)(curBackBufferLoc) = (int)renderTarget;
            *(int*)(curBackBufferLoc + 0x4) = (int)depthBuffer;
            result = devDX9->SetRenderTarget(0, renderTarget);
            result = devDX9->SetDepthStencilSurface(depthBuffer);
            result = devDX9->SetViewport(&viewport);
            //result = devDX9->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 255, 255, 255), 1.0f, 0);

            //----
            // Renders mouseover cursor
            //----
            worldMatrix = cursorUI.GetObjectMatrix(false, true);
            result = devDX9->SetVertexShaderConstantF(0, &projectionMatrix._11, 4);
            result = devDX9->SetVertexShaderConstantF(4, &viewMatrix._11, 4);
            result = devDX9->SetVertexShaderConstantF(8, &worldMatrix._11, 4);
            result = devDX9->SetTexture(0, cursor.pTexture);
            cursorUI.Render();  // fix #38: this IS the VR cursor — ungated (fix #36 over-gated it)

            for (int i = 0; i < 2; i++)
            {
                //----
                // Render ui to game windows
                //----
                std::tie(tIndex, renderTarget, depthBuffer, frameStart, frameStop, viewport, clearColor) = bufferList[i];
                *(int*)(curBackBufferLoc) = (int)renderTarget;
                *(int*)(curBackBufferLoc + 0x4) = (int)depthBuffer;
                result = devDX9->SetRenderTarget(0, renderTarget);
                result = devDX9->SetDepthStencilSurface(depthBuffer);
                result = devDX9->SetViewport(&viewport);
                //result = devDX9->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 255, 255, 255), 1.0f, 0);

                XMMATRIX hmdData = matEyeOffset[i] * matHMDPos;
                projectionMatrix = XMMatrixTranspose(matProjection[i]);
                viewMatrix = XMMatrixTranspose(XMMatrixInverse(0, (hmdData)));
                worldMatrix = XMMatrixIdentity();
                projectionMatrix._33 = cfg_uiOffsetD;// -0.938f;
                //projectionMatrix._34 =  -0.06f;

                devDX9->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
                devDX9->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

                devDX9->SetRenderState(D3DRS_ZENABLE, (doOcclusion) ? TRUE : FALSE);

                //----
                // Renders the ui
                //----
                    worldMatrix = curvedUI.GetObjectMatrix(false, true);
                    result = devDX9->SetVertexShaderConstantF(0, &projectionMatrix._11, 4);
                    result = devDX9->SetVertexShaderConstantF(4, &viewMatrix._11, 4);
                    result = devDX9->SetVertexShaderConstantF(8, &worldMatrix._11, 4);
                    result = devDX9->SetTexture(0, uiRender.pTexture);
                    curvedUI.Render();

                devDX9->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            }
        }

        devDX9->SetRenderState(D3DRS_ZENABLE, TRUE);
        devDX9->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

        // fix #64-instr: dump point B = POST-UI. This is the final eye content (world +
        // UI + cursor) for this frame — the same slot that gets published to the ready
        // index and submitted to the compositor. Consume the request here (so A and B are
        // from the SAME frame) and hand off to OnPaint for the ready-slot dump.
        if (g_dumpEyesRequested)
        {
            DumpSurfaceToBMP(BackBuffer[textureIndex].pShaderResource,     "./vr_version/eyedump_L_B.bmp");
            DumpSurfaceToBMP(BackBuffer[textureIndex + 3].pShaderResource, "./vr_version/eyedump_R_B.bmp");
            g_dumpEyesRequested = false;
            g_dumpEyesReadyPending = true;
            ofOut << "[dump] A+B eye textures saved (pre/post UI); ready-slot dump pending" << std::endl; ofOut.flush();
        }

        // fix #65 experiment (flag-gated): rebind the LEFT eye as the active target at the
        // very end of the stereo body, mirroring exactly what the j=0 per-eye setup does
        // (see lines ~2005-2008). This leaves the LEFT eye's render target + depth bound on
        // the device AND the engine's curBackBufferLoc slot pointing at the left-eye
        // surfaces, so the engine's free post-hook rendering (done after this hook, before
        // Present) lands in the LEFT eye instead of the RIGHT. Reuses the same j=0 locals
        // (bufferList[0] unpack). Null-guarded.
        if (g_fix65_LeftBoundAtExit && devDX9)
        {
            std::tie(tIndex, renderTarget, depthBuffer, frameStart, frameStop, viewport, clearColor) = bufferList[0];
            if (renderTarget)
            {
                *(int*)(curBackBufferLoc)       = (int)renderTarget;  // gxDev+0x3A08 = left color surface
                *(int*)(curBackBufferLoc + 0x4) = (int)depthBuffer;   // gxDev+0x3A0C = left depth surface
                devDX9->SetRenderTarget(0, renderTarget);
                devDX9->SetDepthStencilSurface(depthBuffer);
            }
        }

        // fix #69 timing: the overlay tail (masked UI + cursor/UI compositing) ran since qPrev
        // (end of the j==2 pass); fold it into the UI section, then accumulate all four sections
        // into the per-frame sums OnPaint averages and resets.
        LARGE_INTEGER qEnd; QueryPerformanceCounter(&qEnd);
        secUI += (double)(qEnd.QuadPart - qPrev.QuadPart) * 1000.0 / (double)s_qpcFreq.QuadPart;
        g_time69_warmupSum += secW;
        g_time69_LSum      += secL;
        g_time69_RSum      += secR;
        g_time69_UISum     += secUI;
        g_time69_frames++;
    }
    else
    {
        sub_495410(ecx);
    }
}

// OnPrint
void(__thiscall* sub_4A8720)() = (void(__thiscall*)())TBC_sub_OnPaint;
// fix #63-instr: format a 32-bit value as 8 uppercase hex digits (no 0x prefix), so
// surface/texture pointers in the dump match the pointers logged elsewhere.
static std::string hx8(unsigned int v)
{
    char buf[16];
    sprintf_s(buf, sizeof(buf), "%08X", v);
    return std::string(buf);
}

// fix #63-diff: dump a per-eye DIFFERENCE HISTOGRAM instead of the raw command stream.
// Runs on the render thread from OnPaint, after recording is disarmed, so g_rec is stable.
// The old raw stream capped at 150 lines/eye and never reached the terrain draws; both eyes
// looked identical in that prefix. Instead we aggregate the WHOLE frame:
//   [recsum2] per-eye DIP count + triangle total (sum of PrimitiveCount over DIP+DP),
//   [recdiff] per-texture (dips/tris) but ONLY where the two eyes differ (the smoking gun
//             for terrain a texture drawn in one eye and not the other), and
//   [recfog]  how many DIP draws ran with FOGENABLE on vs off, per eye.
static void DumpRec()
{
    long n = g_recCount;
    if (n > REC_CAP) n = REC_CAP;

    struct TexBin { unsigned tex; int dips; int tris; };
    static const int HCAP = 1024;
    static TexBin hist[2][HCAP];   // static: too large for the render-thread stack
    memset(hist, 0, sizeof(hist));
    int histN[2] = { 0, 0 };

    int totDip[2] = { 0, 0 };
    int totTris[2] = { 0, 0 };
    int fogOn[2] = { 0, 0 };
    int fogOff[2] = { 0, 0 };
    unsigned curFog[2] = { 0, 0 };   // running D3DRS_FOGENABLE value, tracked in stream order

    for (long k = 0; k < n; k++) {
        RecEntry& e = g_rec[k];
        if (e.eye > 1) continue;
        int eye = e.eye;
        if (e.op == 4) {                       // SetRenderState: track fog toggles in order
            if (e.a == 28) curFog[eye] = e.b;  // D3DRS_FOGENABLE
            continue;
        }
        if (e.op != 5 && e.op != 6) continue;  // only DrawIndexedPrimitive / DrawPrimitive

        unsigned tex = e.a;                    // current stage-0 texture, tagged at draw time
        int prim = (int)e.b;                   // PrimitiveCount

        totTris[eye] += prim;
        if (e.op == 5) {                       // DIP counts toward DIP total + fog trace
            totDip[eye]++;
            if (curFog[eye]) fogOn[eye]++; else fogOff[eye]++;
        }

        int f = -1;
        for (int t = 0; t < histN[eye]; t++) if (hist[eye][t].tex == tex) { f = t; break; }
        if (f < 0 && histN[eye] < HCAP) { f = histN[eye]++; hist[eye][f].tex = tex; hist[eye][f].dips = 0; hist[eye][f].tris = 0; }
        if (f >= 0) { if (e.op == 5) hist[eye][f].dips++; hist[eye][f].tris += prim; }
    }

    ofOut << "[rec] === fix #63 per-eye difference histogram (" << n << " entries";
    if (g_recCount > REC_CAP) ofOut << ", CAPPED at " << REC_CAP;
    ofOut << ") ===" << std::endl;

    ofOut << "[recsum2] eye=0 DIP=" << totDip[0] << " tris=" << totTris[0]
          << " | eye=1 DIP=" << totDip[1] << " tris=" << totTris[1]
          << "  (tris = sum PrimitiveCount over DIP+DP)" << std::endl;
    ofOut.flush();

    // Per-texture diff: print only textures whose (dips/tris) differ between the eyes.
    int lines = 0;
    for (int i = 0; i < histN[0] && lines < 80; i++) {
        unsigned tex = hist[0][i].tex;
        int j = -1;
        for (int t = 0; t < histN[1]; t++) if (hist[1][t].tex == tex) { j = t; break; }
        int Ld = hist[0][i].dips, Lt = hist[0][i].tris;
        int Rd = (j >= 0) ? hist[1][j].dips : 0;
        int Rt = (j >= 0) ? hist[1][j].tris : 0;
        if (Ld == Rd && Lt == Rt) continue;    // identical in both eyes -> skip
        ofOut << "[recdiff] tex=" << hx8(tex) << " L: " << Ld << "/" << Lt
              << " R: " << Rd << "/" << Rt;
        if (j < 0) ofOut << " ONLY-L";
        ofOut << std::endl;
        lines++;
    }
    for (int i = 0; i < histN[1] && lines < 80; i++) {   // eye-1-only textures
        unsigned tex = hist[1][i].tex;
        int j = -1;
        for (int t = 0; t < histN[0]; t++) if (hist[0][t].tex == tex) { j = t; break; }
        if (j >= 0) continue;                  // already covered by the eye-0 loop
        ofOut << "[recdiff] tex=" << hx8(tex) << " L: 0/0 R: "
              << hist[1][i].dips << "/" << hist[1][i].tris << " ONLY-R" << std::endl;
        lines++;
    }
    if (lines == 0) ofOut << "[recdiff] (no per-eye texture differences)" << std::endl;
    else if (lines >= 80) ofOut << "[recdiff] (capped at 80 lines)" << std::endl;
    ofOut.flush();

    ofOut << "[recfog] eye=0 fogON draws=" << fogOn[0] << " fogOFF draws=" << fogOff[0]
          << " | eye=1 fogON draws=" << fogOn[1] << " fogOFF draws=" << fogOff[1] << std::endl;
    ofOut.flush();

    // fix #65-instr: the RT / DS / CLR / VP event stream in ORDER, each tagged with the
    // eye and how many draws had already been issued in that eye at that point. Shows
    // exactly WHEN the engine rebinds targets / clears relative to the geometry draws,
    // so we can see whether a mid-frame rebind is eating one eye's terrain. Ops other
    // than 1/2/3/8 are skipped; draws (5/6) advance the per-eye draw counter.
    {
        int drawSeen[2] = { 0, 0 };
        int oplines = 0;
        for (long k = 0; k < n && oplines < 60; k++) {
            RecEntry& e = g_rec[k];
            if (e.eye > 1) continue;
            int eye = e.eye;
            if (e.op == 5 || e.op == 6) { drawSeen[eye]++; continue; }   // draw: advance counter, no print
            if (e.op != 1 && e.op != 2 && e.op != 3 && e.op != 8) continue;
            ofOut << "[recops] eye=" << eye << " @draw" << drawSeen[eye] << " ";
            if (e.op == 1)      ofOut << "RT a=" << hx8(e.a);
            else if (e.op == 2) ofOut << "DS a=" << hx8(e.a);
            else if (e.op == 3) ofOut << "CLR flags=" << e.a << " color=" << hx8(e.b);
            else                ofOut << "VP x=" << (e.a >> 16) << " y=" << (e.a & 0xFFFF)
                                      << " w=" << (e.b >> 16) << " h=" << (e.b & 0xFFFF);
            ofOut << std::endl;
            oplines++;
        }
        if (oplines >= 60) ofOut << "[recops] (capped)" << std::endl;
        ofOut.flush();
    }
}

void(__fastcall msub_4A8720)()
{
    static int opHits = 0;
    if (opHits < 5 || opHits % 600 == 0) { ofOut << "[hook] OnPaint #" << opHits << " isEnabled=" << svr->isEnabled() << std::endl; ofOut.flush(); }
    int fix56CamHits = g_fix56_camHits;  // fix #56 temp instrumentation: hits accumulated this frame (StartRender runs before OnPaint)
    g_fix56_camHits = 0;
    if (opHits % 600 == 0) { ofOut << "[fix56] UpdateCameraFn hits last frame = " << fix56CamHits << " (expect 2)" << std::endl; ofOut.flush(); }
    // fix #58-instr: snapshot + reset the sky counters every frame (StartRender ran
    // before OnPaint, so these reflect the frame just rendered); log once per 600.
    int wsrL = g_wsrHits[0], wsrR = g_wsrHits[1];
    int spL = g_skyPassHits[0], spR = g_skyPassHits[1];
    g_wsrHits[0] = g_wsrHits[1] = g_wsrHits[2] = 0;
    g_skyPassHits[0] = g_skyPassHits[1] = g_skyPassHits[2] = 0;
    // fix #60-instr: snapshot + reset the three WSR sub-pass counters each frame.
    int s75aL = g_wsr75A0Hits[0], s75aR = g_wsr75A0Hits[1];
    int s99cL = g_wsr99C0Hits[0], s99cR = g_wsr99C0Hits[1];
    int s584L = g_wsr5840Hits[0], s584R = g_wsr5840Hits[1];
    g_wsr75A0Hits[0] = g_wsr75A0Hits[1] = g_wsr75A0Hits[2] = 0;
    g_wsr99C0Hits[0] = g_wsr99C0Hits[1] = g_wsr99C0Hits[2] = 0;
    g_wsr5840Hits[0] = g_wsr5840Hits[1] = g_wsr5840Hits[2] = 0;
    // fix #61-instr: snapshot + reset the confirmation counters each frame.
    int fillL = g_fillHits[0], fillR = g_fillHits[1];
    int cmapL = g_cmapHits[0], cmapR = g_cmapHits[1];
    g_fillHits[0] = g_fillHits[1] = g_fillHits[2] = 0;
    g_cmapHits[0] = g_cmapHits[1] = g_cmapHits[2] = 0;
    if (opHits % 600 == 0) {
        ofOut << "[sky] eye0: vis=" << g_skyDbg[0][0] << " ovr=" << g_skyDbg[0][1]
              << " zone=" << g_skyDbg[0][2] << " dn=" << g_skyDbg[0][3] << " sky=" << g_skyDbg[0][4]
              << " | eye1: vis=" << g_skyDbg[1][0] << " ovr=" << g_skyDbg[1][1]
              << " zone=" << g_skyDbg[1][2] << " dn=" << g_skyDbg[1][3] << " sky=" << g_skyDbg[1][4]
              << " | wsr hits L/R=" << wsrL << "/" << wsrR
              << " skypass L/R=" << spL << "/" << spR << std::endl; ofOut.flush();
        ofOut << "[wsr2] 75A0 L/R=" << s75aL << "/" << s75aR
              << " 99C0 L/R=" << s99cL << "/" << s99cR
              << " 5840 L/R=" << s584L << "/" << s584R << std::endl; ofOut.flush();
        // fix #61: confirmation line. fill/cmap = per-eye hit counts of the visible-list
        // fill (0x699F60) and CMap terrain render (0x70B4A0) — note these can include
        // engine-update-phase hits. listL/listR = the raw visible-list head/tail
        // (0xDA81C0/0xDA81C4) as the engine left them going into each eye's render; if
        // the pre-pass cull works, both eyes should show a non-zero, matching list.
        ofOut << "[cull] fill L/R=" << fillL << "/" << fillR
              << " cmap L/R=" << cmapL << "/" << cmapR
              << " listL=" << g_visListDbg[0][0] << "/" << g_visListDbg[0][1]
              << " listR=" << g_visListDbg[1][0] << "/" << g_visListDbg[1][1]
              << " (fix61=" << g_fix61_PrePassCull << ")" << std::endl; ofOut.flush();
        // fix #62 proof: did the engine hijack our eye render target mid-scene? "ok" = the
        // RT bound at pass end still equals our eye surface; "HIJACKED" = it was rebound.
        ofOut << "[rt] endL=" << (g_rtProbe[0] == 1 ? "ok" : (g_rtProbe[0] == 0 ? "HIJACKED" : "n/a"))
              << " endR=" << (g_rtProbe[1] == 1 ? "ok" : (g_rtProbe[1] == 0 ? "HIJACKED" : "n/a"))
              << " (fix62=" << g_fix62_RTCacheInvalidate << ")" << std::endl; ofOut.flush();
    }
    // fix #58/#59 probe: batch-chain lengths per eye. pre = chain length going into the
    // render call, post = chain length after it; flags = dirty/purge (D/P) as the engine
    // left them before our writes. (fix59 shows whether the deferred purge is active.)
    if (opHits % 600 == 0) {
        ofOut << "[batch] L: pre=" << g_batchDbg[0][0] << " post=" << g_batchDbg[0][1]
              << " flags=" << g_batchDbg[0][2] << "/" << g_batchDbg[0][3]
              << " | R: pre=" << g_batchDbg[1][0] << " post=" << g_batchDbg[1][1]
              << " flags=" << g_batchDbg[1][2] << "/" << g_batchDbg[1][3]
              << " (fix59=" << g_fix59_DeferPurge << ")" << std::endl; ofOut.flush();
    }
    // fix #69: per-section CPU timing, averaged over the last ~600 frames. warmup = throwaway
    // world pass; L/R = the two eye world passes; UI = the j==2 UI world pass plus the overlay
    // compositing tail. Helps see where the CPU-bound frame time goes.
    if (opHits % 600 == 0) {
        int tf = (g_time69_frames > 0) ? g_time69_frames : 1;
        char tbuf[192];
        sprintf_s(tbuf, sizeof(tbuf),
            "[time] warmup=%.2fms L=%.2fms R=%.2fms UI=%.2fms (avg over last 600 frames)",
            g_time69_warmupSum / tf, g_time69_LSum / tf, g_time69_RSum / tf, g_time69_UISum / tf);
        ofOut << tbuf << std::endl; ofOut.flush();
        g_time69_warmupSum = g_time69_LSum = g_time69_RSum = g_time69_UISum = 0.0;
        g_time69_frames = 0;
    }
    // fix #63-instr: per-eye command-stream recorder. StartRender runs BEFORE this OnPaint,
    // so arming here records the NEXT frame's world passes; we dump on the following OnPaint.
    // Phase 599 (mod 1200) never collides with the %600 log blocks above.
    {
        static int recArmFrame = -1;
        if (g_recArmed == 1 && opHits != recArmFrame) {
            DumpRec();
            g_recArmed = 0;
            g_recCount = 0;
        }
        else if (g_recDiag && opHits % 1200 == 599) {   // fix #77: gated OFF for play
            g_recCount = 0;
            g_recEye = -1;
            g_recArmed = 1;
            recArmFrame = opHits;
        }
    }
    // fix #70 probe: arm the DNSkyPass sky-state log for ONE frame every 600 frames.
    // StartRender (which runs the sky passes) runs BEFORE this OnPaint, so arming here
    // takes effect for the NEXT frame's world passes; we disarm on the following OnPaint,
    // giving exactly one frame's sky passes (both eyes = 2-3 lines) per arm. Phase 300
    // avoids the %600==0 log blocks above and the %1200==599 recorder arm.
    if (g_skyProbeArm) g_skyProbeArm = false;            // armed last frame - StartRender consumed it, disarm
    else if (opHits % 600 == 300) g_skyProbeArm = true;  // arm for the next frame's sky passes
    opHits++;

    // fix #64-instr: on-demand eye-texture dump trigger. Every 2s check for the trigger
    // file ./vr_version/dump_eyes.txt; if present, delete it and arm a one-shot dump. StartRender
    // then writes the pre-UI (*_A) and post-UI (*_B) eye textures; the ready-slot dump
    // (*_ready) happens further below once g_readyIndex is valid.
    {
        static DWORD s_lastDumpTick = 0;
        DWORD dnow = GetTickCount();
        if (dnow - s_lastDumpTick > 2000)
        {
            s_lastDumpTick = dnow;
            FILE* df = nullptr;
            if (fopen_s(&df, "./vr_version/dump_eyes.txt", "r") == 0 && df)
            {
                fclose(df);
                remove("./vr_version/dump_eyes.txt");
                g_dumpEyesRequested = true;
                ofOut << "[dump] trigger detected -> arming eye-texture dump" << std::endl; ofOut.flush();
            }

            // DEBUG (ALWAYS read, even when the addon owns config): a one-line file
            // ./vr_version/zmode.txt holding a single integer selects the nameplate
            // depth-test mode live (0=no test, 1=z-buffer, 2=w-buffer), so occlusion
            // can be isolated in-game without a relogin.
            {
                std::ifstream zf("./vr_version/zmode.txt");
                int zv;
                if (zf >> zv && zv >= 0 && zv <= 2 && zv != g_nameplateZMode) {
                    g_nameplateZMode = zv;
                    ofOut << "[cfg] zmode.txt -> nameplate_zmode = " << zv << std::endl; ofOut.flush();
                }
                std::ifstream zff("./vr_version/zforce.txt");
                float fv;
                if (zff >> fv && fv >= -1.0f && fv <= 1.0f && fv != g_nameplateZForce) {
                    g_nameplateZForce = fv;
                    ofOut << "[cfg] zforce.txt -> nameplate_zforce = " << fv << std::endl; ofOut.flush();
                }
                std::ifstream zbf("./vr_version/zbias.txt");
                float bv;
                if (zbf >> bv && bv >= -0.1f && bv <= 0.1f && bv != g_nameplateZBias) {
                    g_nameplateZBias = bv;
                    ofOut << "[cfg] zbias.txt -> nameplate_zbias = " << bv << std::endl; ofOut.flush();
                }
                std::ifstream tbf("./vr_version/textbar.txt");
                int tv;
                if (tbf >> tv && tv >= 0 && tv <= 1 && tv != g_nameplateTextBar) {
                    g_nameplateTextBar = tv;
                    ofOut << "[cfg] textbar.txt -> nameplate_textbar = " << tv << std::endl; ofOut.flush();
                }
                std::ifstream bcf("./vr_version/barcolor.txt");
                int bcv;
                if (bcf >> bcv && bcv >= 0 && bcv <= 1 && bcv != g_nameBarColor) {
                    g_nameBarColor = bcv;
                    ofOut << "[cfg] barcolor.txt -> namebar_color = " << bcv << std::endl; ofOut.flush();
                }
                std::ifstream bqf("./vr_version/barquad.txt");
                int bqv;
                if (bqf >> bqv && bqv >= 0 && bqv <= 1 && bqv != g_nameBarQuad) {
                    g_nameBarQuad = bqv;
                    ofOut << "[cfg] barquad.txt -> namebar_quad = " << bqv << std::endl; ofOut.flush();
                }
                std::ifstream bff("./vr_version/barfrac.txt");
                float bfv;
                if (bff >> bfv && bfv >= -1.0f && bfv <= 1.0f && bfv != g_nameBarForceFrac) {
                    g_nameBarForceFrac = bfv;
                    ofOut << "[cfg] barfrac.txt -> namebar_forcefrac = " << bfv << std::endl; ofOut.flush();
                }
                std::ifstream btf("./vr_version/bartex.txt");
                int btv;
                if (btf >> btv && btv >= 0 && btv <= 1 && btv != g_nameBarTexKnob) {
                    g_nameBarTexKnob = btv;
                    ofOut << "[cfg] bartex.txt -> namebar_frametex = " << btv << std::endl; ofOut.flush();
                }
                std::ifstream bmf("./vr_version/barmode.txt");
                int bmv;
                if (bmf >> bmv && bmv >= 0 && bmv <= 2 && bmv != g_nameplateMode) {
                    ofOut << "[cfg] barmode.txt -> "; ofOut.flush();
                    ApplyNameplateMode(bmv);
                }
            }

            // Change 3: ONE unified live-tuning config file. Replaces the old six separate
            // .txt reads (nameplate_depth / nameplate_xshift / nameplate_yshift /
            // highlight_mouseover / highlight_target / highlight_bright). Re-read every ~2s
            // (this throttle). Format: "key = value" (also "key value"); '#' starts a
            // comment; blank/unknown lines are ignored. Only logs a key when its value
            // actually changes (like the old per-file logs). The file is kept, not deleted.
            if (!g_addonConfigActive) {
                std::ifstream cfg("./vr_version/vr_config.cfg");
                std::string line;
                while (std::getline(cfg, line))
                {
                    size_t hash = line.find('#');
                    if (hash != std::string::npos) line.erase(hash);      // strip comment
                    for (size_t i = 0; i < line.size(); ++i)
                        if (line[i] == '=') line[i] = ' ';                 // '=' acts as a separator
                    std::istringstream ss(line);
                    std::string key;
                    if (!(ss >> key)) continue;                           // blank line

                    if (key == "nameplate_depth")
                    {
                        float v;
                        if ((ss >> v) && v >= 0.0f && v <= 1.0f && v != g_nameplateDepthScale) {
                            g_nameplateDepthScale = v;
                            ofOut << "[cfg] nameplate_depth = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "nameplate_zmode")
                    {
                        int v;
                        if ((ss >> v) && v >= 0 && v <= 2 && v != g_nameplateZMode) {
                            g_nameplateZMode = v;
                            ofOut << "[cfg] nameplate_zmode = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "nameplate_zforce")
                    {
                        float v;
                        if ((ss >> v) && v >= -1.0f && v <= 1.0f && v != g_nameplateZForce) {
                            g_nameplateZForce = v;
                            ofOut << "[cfg] nameplate_zforce = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "nameplate_yshift")
                    {
                        float v;
                        if ((ss >> v) && v >= -2000.0f && v <= 2000.0f && v != g_nameplateYShift) {
                            g_nameplateYShift = v;
                            ofOut << "[cfg] nameplate_yshift = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "nameplate_xshift")
                    {
                        float v;
                        if ((ss >> v) && v >= -2000.0f && v <= 2000.0f && v != g_flatXShift) {
                            g_flatXShift = v;
                            ofOut << "[cfg] nameplate_xshift = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "mouseover_color")
                    {
                        float r, g, b;
                        if ((ss >> r >> g >> b) &&
                            r >= 0.0f && r <= 8.0f && g >= 0.0f && g <= 8.0f && b >= 0.0f && b <= 8.0f &&
                            (r != g_hlMouseColor[0] || g != g_hlMouseColor[1] || b != g_hlMouseColor[2])) {
                            g_hlMouseColor[0] = r; g_hlMouseColor[1] = g; g_hlMouseColor[2] = b;
                            ofOut << "[cfg] mouseover_color = " << r << " " << g << " " << b << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "mouseover_bright")
                    {
                        float v;
                        if ((ss >> v) && v >= 1.0f && v <= 5.0f && v != g_hlMouseBright) {
                            g_hlMouseBright = v;
                            ofOut << "[cfg] mouseover_bright = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "target_color")
                    {
                        float r, g, b;
                        if ((ss >> r >> g >> b) &&
                            r >= 0.0f && r <= 8.0f && g >= 0.0f && g <= 8.0f && b >= 0.0f && b <= 8.0f &&
                            (r != g_hlTargetColor[0] || g != g_hlTargetColor[1] || b != g_hlTargetColor[2])) {
                            g_hlTargetColor[0] = r; g_hlTargetColor[1] = g; g_hlTargetColor[2] = b;
                            ofOut << "[cfg] target_color = " << r << " " << g << " " << b << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "target_bright")
                    {
                        float v;
                        if ((ss >> v) && v >= 1.0f && v <= 5.0f && v != g_hlTargetBright) {
                            g_hlTargetBright = v;
                            ofOut << "[cfg] target_bright = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "crosshair")
                    {
                        int v;
                        if ((ss >> v) && (v == 0 || v == 1) && v != g_crosshair) {
                            g_crosshair = v;
                            ofOut << "[cfg] crosshair = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "crosshair_size")
                    {
                        float v;
                        if ((ss >> v) && v >= 1.0f && v <= 100.0f && v != g_crosshairSize) {
                            g_crosshairSize = v;
                            ofOut << "[cfg] crosshair_size = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "crosshair_color")
                    {
                        float r, g, b;
                        if ((ss >> r >> g >> b) &&
                            r >= 0.0f && r <= 1.0f && g >= 0.0f && g <= 1.0f && b >= 0.0f && b <= 1.0f &&
                            (r != g_crosshairColor[0] || g != g_crosshairColor[1] || b != g_crosshairColor[2])) {
                            g_crosshairColor[0] = r; g_crosshairColor[1] = g; g_crosshairColor[2] = b;
                            ofOut << "[cfg] crosshair_color = " << r << " " << g << " " << b << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "crosshair_offset")
                    {
                        float v;
                        if ((ss >> v) && v >= -500.0f && v <= 500.0f && v != g_crosshairOffset) {
                            g_crosshairOffset = v;
                            ofOut << "[cfg] crosshair_offset = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "crosshair_yoffset")
                    {
                        float v;
                        if ((ss >> v) && v >= -500.0f && v <= 500.0f && v != g_crosshairYOffset) {
                            g_crosshairYOffset = v;
                            ofOut << "[cfg] crosshair_yoffset = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "aim_assist")
                    {
                        int v;
                        if ((ss >> v) && (v == 0 || v == 1) && v != g_aimAssist) {
                            g_aimAssist = v;
                            ofOut << "[cfg] aim_assist = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "aim_rings")
                    {
                        int v;
                        if ((ss >> v) && v >= 0 && v <= 4 && v != g_aimRings) {
                            g_aimRings = v;
                            ofOut << "[cfg] aim_rings = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "aim_spread_deg")
                    {
                        float v;
                        if ((ss >> v) && v >= 0.1f && v <= 15.0f && v != g_aimSpreadDeg) {
                            g_aimSpreadDeg = v;
                            ofOut << "[cfg] aim_spread_deg = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "aim_samples")
                    {
                        int v;
                        if ((ss >> v) && v >= 3 && v <= 16 && v != g_aimSamples) {
                            g_aimSamples = v;
                            ofOut << "[cfg] aim_samples = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "screen_size")
                    {
                        float v;
                        if ((ss >> v) && v >= 0.1f && v <= 5.0f && v != cfg_uiOffsetScale) {
                            cfg_uiOffsetScale = v;
                            ofOut << "[cfg] screen_size = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "screen_distance")
                    {
                        float v;
                        if ((ss >> v) && v >= -1000.0f && v <= 1000.0f && v != cfg_uiOffsetZ) {
                            cfg_uiOffsetZ = v;
                            ofOut << "[cfg] screen_distance = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "screen_height")
                    {
                        float v;
                        if ((ss >> v) && v >= -1000.0f && v <= 1000.0f && v != cfg_uiOffsetY) {
                            cfg_uiOffsetY = v;
                            ofOut << "[cfg] screen_height = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "screen_depth")
                    {
                        float v;
                        if ((ss >> v) && v >= -2.0f && v <= 2.0f && v != cfg_uiOffsetD) {
                            cfg_uiOffsetD = v;
                            ofOut << "[cfg] screen_depth = " << v << std::endl; ofOut.flush();
                        }
                    }
                    else if (key == "world_scale")
                    {
                        float v;
                        if ((ss >> v) && v >= 0.1f && v <= 4.0f && v != g_worldScale) {
                            g_worldScale = v;
                            ofOut << "[cfg] world_scale = " << v << std::endl; ofOut.flush();
                        }
                    }
                }
            }
        }
    }

    gPlayerObj = nullptr;
    isPossessing = false;
    if (svr->isEnabled() && g_vrResourcesLive)  // fix #21: skip VR work while device is resetting
    {
        // fix #57: FOV overscan live-tuning. k>1 renders each eye wider than the
        // visible FOV and crops back on submit, giving compositor reprojection real
        // pixels past the edge instead of black. Live-tuned via vr/overscan.txt
        // (re-read every 2s, valid 1.0..1.5, no file / bad value = keep current).
        {
            static float s_overscan = 1.0f;
            static DWORD s_lastOvTick = 0;
            DWORD on = GetTickCount();
            if (on - s_lastOvTick > 2000)
            {
                s_lastOvTick = on;
                FILE* of = nullptr;
                if (fopen_s(&of, "./vr_version/overscan.txt", "r") == 0 && of)
                {
                    float v = 0;
                    if (fscanf_s(of, "%f", &v) == 1 && v >= 1.0f && v <= 1.5f && v != s_overscan)
                    {
                        s_overscan = v;
                        ofOut << "[hook] overscan.txt -> k=" << v << std::endl; ofOut.flush();
                    }
                    fclose(of);
                }
            }
            svr->overscan = s_overscan;
        }

        gPlayerObj = ClntObjMgrGetActivePlayerObj();

        // fix #31: auto-recenter on world enter. HMD yaw is measured from the
        // SteamVR zero set at session start, so after login the view pointed
        // wherever the head happened to face at launch ("character off to the
        // side"). Whichever way you look when the world loads becomes forward.
        static bool s_wasInWorld = false;
        if (gPlayerObj && !s_wasInWorld)
        {
            svr->Recenter();
            ofOut << "[hook] world entered -> Recenter (current head direction = forward)" << std::endl; ofOut.flush();
        }
        s_wasInWorld = (gPlayerObj != nullptr);

        //----
        // Active Character is not the active player
        // Possessed something else?
        //----
        // fix #76: this WotLK-only possess/vehicle check reads player field +0x1008 as a
        // pointer and dereferences +0x770 off it. In TBC 2.4.3 +0x1008 is NOT a pointer -
        // usually 0 (so the && short-circuited and it never crashed), but in Sethekk Halls
        // combat it held the int 1 -> *(int*)(1 + 0x770) read 0x771 -> ACCESS_VIOLATION on
        // the main render thread (no SEH there). Vehicles don't exist in TBC; the port note
        // tbc_structures.h TODO_possessTarget already says to stub this branch to false.
        // isPossessing then stays false = identical to every normal frame ever rendered.
        if (false)  // was: gPlayerObj && gPlayerObj->unknown11 && *(int*)(gPlayerObj->unknown11 + 0x770) != 0
        {
            stObjectManager* activeObj = objManagerGetActiveObject();
            if (gPlayerObj != activeObj)
            {
                isPossessing = true;
                gPlayerObj = activeObj;
            }
        }

        // fix #68: the 72Hz submit thread now does Submit + WaitGetPoses +
        // SetFramePose (each Submit carrying the slot's render pose). The game
        // thread must NOT call WaitGetPoses/Submit — that is what quantized fps in
        // fix #55. The pose matrices below (matProjection/matEyeOffset/matHMDPos)
        // are refreshed by the thread's SetFramePose and read here under g_vrCS.
        //
        // g_fix55_GameThreadSubmit (default false) keeps the fix #55 path compiled
        // for one-flag rollback.
        if (g_vrCSInit) EnterCriticalSection(&g_vrCS);
        if (g_fix55_GameThreadSubmit)
        {
            // --- fix #55 rollback path: Submit + WaitGetPoses on the game thread ---
            LONG idx = g_readyIndex;
            if (idx >= 0)
                svr->Render(BackBuffer11[idx].pTexture, DepthBuffer11[idx].pTexture,
                            BackBuffer11[idx + 3].pTexture, DepthBuffer11[idx + 3].pTexture);
            svr->WaitGetPoses();
            svr->SetFramePose();
        }

        // fix #68: capture the pose SetFramePose produced (thread-updated when the
        // fix #55 path is off) as the pose THIS frame renders with. It is copied
        // into g_slotPose[] by MirrorGameFrameToEyes for the slot it publishes, so
        // it travels with the frame to the thread's pose-attached Submit.
        {
            vr::HmdMatrix34_t rp;
            if (svr->GetHmdRawPose(&rp)) { g_pendingPose = rp; g_pendingPoseValid = true; }
            else { g_pendingPoseValid = false; }
        }

        matProjection[0] = (XMMATRIX)(svr->GetFramePose(poseType::Projection, 0)._m);
        matProjection[1] = (XMMATRIX)(svr->GetFramePose(poseType::Projection, 1)._m);

        matEyeOffset[0] = (XMMATRIX)(svr->GetFramePose(poseType::EyeOffset, 0)._m);
        matEyeOffset[1] = (XMMATRIX)(svr->GetFramePose(poseType::EyeOffset, 1)._m);

        matHMDPos = (XMMATRIX)(svr->GetFramePose(poseType::hmdPosition, -1)._m);
        matController[0] = (XMMATRIX)(svr->GetFramePose(poseType::LeftHand, -1)._m);
        matController[1] = (XMMATRIX)(svr->GetFramePose(poseType::RightHand, -1)._m);

        matControllerPalm[0] = (XMMATRIX)(svr->GetFramePose(poseType::LeftHandPalm, -1)._m);
        matControllerPalm[1] = (XMMATRIX)(svr->GetFramePose(poseType::RightHandPalm, -1)._m);
        if (g_vrCSInit) LeaveCriticalSection(&g_vrCS);

        // Nameplate-occlusion diagnostics (throttled ~2s). WORLD-SPACE REDESIGN:
        // "hidden" = engine plate quads suppressed at strata flush this window,
        // "wsDraws" = our world-space plate draws this window. Both > 0 together
        // is the engagement proof (engine plates gone AND replacements drawn).
        {
            static DWORD s_lastOcclLog = 0;
            DWORD onOccl = GetTickCount();
            if ((g_dbgHiddenElems > 0 || g_dbgWsDraws > 0 || g_dbgWsGatherFail > 0 ||
                 g_dbgNameDraws > 0 || g_dbgNameAppends > 0 ||
                 g_plateOcclDraws > 0 || g_plateOcclStateFallback > 0 ||
                 g_plateOcclDepthApplied > 0) && onOccl - s_lastOcclLog > 2000) {
                s_lastOcclLog = onOccl;
                ofOut << "[occl] hidden=" << g_dbgHiddenElems
                      << " wsDraws=" << g_dbgWsDraws << " nameDraws=" << g_dbgNameDraws
                      << " nameAppends=" << g_dbgNameAppends
                      << " barWelds=" << g_dbgBarWelds << " barWeldFails=" << g_dbgBarWeldFails
                      << " wsGatherFail=" << g_dbgWsGatherFail
                      << " mtxCap=" << g_dbgMtxCap
                      << " usedFB=" << g_dbgWsUsedFB
                      << " noMtx=" << g_dbgWsNoMtx
                      << " noUnit=" << g_dbgWsNoUnit
                      << " stale=" << g_dbgWsStale
                      << " clip=" << g_dbgWsClip
                      << " abs=" << g_dbgWsAbs
                      << " rel=" << g_dbgWsRel
                      << " ndc=" << g_dbgWsNdc[0] << "/" << g_dbgWsNdc[1] << "/" << g_dbgWsNdc[2]
                      << " lastT2=" << g_dbgLastT2
                      << " draws=" << g_plateOcclDraws
                      << " seen=" << g_plateOcclSeen
                      << " frameMatched=" << g_plateOcclFrameMatched
                      << " depthMatched=" << g_plateOcclDepthMatched
                      << " zPatched=" << g_dbgZBatchPatched
                      << " zLast=" << g_dbgZLastPatched
                      << " nodepth=" << g_plateOcclNoDepth
                      << " dataFallback=" << g_plateOcclDataFallback
                      << " stateFallback=" << g_plateOcclStateFallback;
                if (g_plateOcclSampleCount > 0)
                    ofOut << " sample0=" << g_plateOcclSamples[0];
                if (g_plateOcclSampleCount > 1)
                    ofOut << " sample1=" << g_plateOcclSamples[1];
                ofOut << std::endl; ofOut.flush();
                g_plateOcclSeen = g_plateOcclFrameMatched = 0;
                g_plateOcclDepthMatched = g_plateOcclDepthApplied = 0;
                g_plateOcclNoDepth = g_plateOcclNearForced = 0;
                g_plateOcclWriteRejected = 0;
                g_plateOcclDraws = g_plateOcclReadyAtDraw = 0;
                g_plateOcclLostBeforeDraw = g_plateOcclDataFallback = 0;
                g_plateOcclStateFallback = 0;
                g_plateOcclSampleCount = 0;
                g_dbgZBatchPatched = 0;
                g_dbgHiddenElems = 0;
                g_dbgWsDraws = 0; g_dbgNameDraws = 0; g_dbgNameAppends = 0;
                g_dbgBarWelds = 0; g_dbgBarWeldFails = 0;
                g_dbgWsGatherFail = 0;
                g_dbgMtxCap = 0; g_dbgWsUsedFB = 0; g_dbgWsNoMtx = 0;
                g_dbgWsNoUnit = 0; g_dbgWsStale = 0; g_dbgWsClip = 0;
                g_dbgWsAbs = 0; g_dbgWsRel = 0;
            }
        }

        // DIAG (unconditional, ~2s): proves whether the nameplate apply loop engages at all.
        {
            static DWORD s_lastZ = 0;
            DWORD nowZ = GetTickCount();
            if (nowZ - s_lastZ > 2000) {
                s_lastZ = nowZ;
                ofOut << "[occlz] applyFires=" << g_occlApplyFires
                      << " applyEye0=" << g_occlApplyEye0
                      << " countAfterApply=" << g_occlCountAfterApply
                      << " nameplateOccl=" << g_nameplateOcclusion
                      << " curEye=" << curEye << std::endl; ofOut.flush();
                g_occlApplyFires = 0; g_occlApplyEye0 = 0;
            }
        }

        // DIAG (unconditional, ~2s): engine-depth pipeline — W2S stashes, committer
        // fires/pairs, node-table size, and plate lookup hits/misses.
        {
            static DWORD s_lastD = 0;
            DWORD nowD = GetTickCount();
            if (nowD - s_lastD > 2000) {
                s_lastD = nowD;
                ofOut << "[occld] stash=" << g_dbgStash
                      << " commitFires=" << g_dbgCommitFires
                      << " paired=" << g_dbgPaired
                      << " tableN=" << g_plateDepthCount
                      << " lookOk=" << g_dbgLookOk
                      << " lookMiss=" << g_dbgLookMiss
                      << " vtRej=" << g_dbgVtRej
                      << " zForce=" << g_nameplateZForce
                      << std::endl; ofOut.flush();
                g_dbgStash = 0; g_dbgCommitFires = 0; g_dbgPaired = 0;
                g_dbgLookOk = 0; g_dbgLookMiss = 0; g_dbgVtRej = 0;
            }
        }

        // fix #64-instr: dump the ready/display slot's eye textures — the exact D3D9
        // eye surfaces that get submitted this frame. Runs after StartRender produced its
        // A/B dumps (g_dumpEyesReadyPending is only set there). g_readyIndex was published
        // by MirrorGameFrameToEyes for the slot StartRender just rendered.
        if (g_dumpEyesReadyPending)
        {
            LONG ridx = g_readyIndex;
            if (ridx >= 0)
            {
                DumpSurfaceToBMP(BackBuffer[ridx].pShaderResource,     "./vr_version/eyedump_L_ready.bmp");
                DumpSurfaceToBMP(BackBuffer[ridx + 3].pShaderResource, "./vr_version/eyedump_R_ready.bmp");
                g_dumpEyesReadyPending = false;
                ofOut << "[dump] eye textures saved" << std::endl; ofOut.flush();
            }
        }

        // fix #37: position the UI panel from config EVERY frame. Upstream set
        // curvedUI's matrix inside RunFrameUpdateController, which only runs in
        // a branch gated by the unresolved worldPanel global — it never executed,
        // so the panel kept its identity matrix and sat AT THE USER'S HEAD
        // ("tuz obok ryja"), ignoring all config knobs.
        {
            static DWORD s_lastUiCfgTick = 0;
            DWORD uiNow = GetTickCount();
            if (uiNow - s_lastUiCfgTick > 2000)
            {
                s_lastUiCfgTick = uiNow;
                // config.txt live-tune removed 2026-07-12 - uiOffset* now live-tuned via vr_version/vr_config.cfg (screen_size/distance/height/depth)
            }
            float uiAspect = (screenLayout.height > 0) ? ((float)screenLayout.width / (float)screenLayout.height) : 1.333f;
            XMMATRIX uiScaleM = XMMatrixScaling(cfg_uiOffsetScale, cfg_uiOffsetScale, cfg_uiOffsetScale);
            XMMATRIX uiZM = XMMatrixTranslation(0.0f, 0.0f, (cfg_uiOffsetZ / 100.0f));
            XMMATRIX uiYM = XMMatrixTranslation(0.0f, (cfg_uiOffsetY / 100.0f), 0.0f);
            curvedUI.SetObjectMatrix(XMMatrixScaling(uiAspect, 1, 1) * (uiScaleM * uiZM * uiYM));
            // fix #40: cursor quad positioning lived in the same dead branch as
            // the UI placement — without it the cursor was an unpositioned unit
            // quad (visible only right-of-center / right eye). Updates sprite UV
            // and follows the real mouse each frame. Dangerous tail is gated
            // behind !isOverUI which is always false here.
            RunFrameUpdateSetCursor();
        }

        // fix #19: gated — pModelContainer/p20Container are WotLK offsets, in TBC
        // unverified (tbc_structures.h TODO). With a player object loaded (in
        // world) this chain dereferences garbage.
        if (g_step2_BoneHacks && gPlayerObj && gPlayerObj->pModelContainer->p20Container->ptr20)
        {
            int boneCount = *(int*)(gPlayerObj->pModelContainer->p20Container->ptr20 + 0x2C);
            int boneOffset = *(int*)(gPlayerObj->pModelContainer->p20Container->ptr20 + 0x30);
            bool reset = boneLookup.Set(boneCount, boneOffset);
        }

        int worldPanel = SAFE_READ_INT(TBC_TODO_g_WorldPanel, 0);
        if (worldPanel)
        {
            isOverUI = true;
            int worldFrame = SAFE_READ_INT(TBC_g_WorldFrame, 0);
            int mouseOverUiElement = *(int*)(worldPanel + 0x78);
            if (worldFrame && mouseOverUiElement == worldFrame)
                isOverUI = false;
        }
        else
        {
            isOverUI = true;
        }

        //----
        // Update near/far clip
        //----
        SAFE_WRITE_FLOAT(TBC_TODO_g_NearClip, 0.06f);
        // SAFE_WRITE_FLOAT(TBC_TODO_g_FarClip, 1000.0f);  // optional

        svr->SetProjection({ SAFE_READ_FLOAT(TBC_TODO_g_NearClip, 0.06f),
                             SAFE_READ_FLOAT(TBC_TODO_g_FarClip,  1000.0f) });

        sub_4A8720();

        resetPlayerAnimCounter = true;


        // fix #23b: the mirror copy moved to MirrorGameFrameToEyes(), called from
        // the PresentScene hook where the frame is complete. Copying here (end of
        // the OnPaint hook) caught a PARTIAL frame — the engine issues more draw
        // batches after OnPaint — causing flickering black bands in the lower half.
    }
    else
    {
        sub_4A8720();
    }
    gPlayerObj = nullptr;
}

void SetMousePosition(HWND hwnd, int mouseX, int mouseY, bool forceMouse)
{
    HWND active = GetActiveWindow();
    POINT p = { 0, 0 };

    if (active == hwnd || forceMouse == true)
    {
        int mouseHold = *(int*)0x00D4156C; // TODO_TBC
        if (!isOverUI && mouseHold == 1)
        {
            p.x = *(int*)0x00D413EC; // TODO_TBC
            p.y = *(int*)0x00D413F0; // TODO_TBC
        }
        else
        {
            p.x = mouseX;
            p.y = mouseY;
            ClientToScreen(hwnd, &p);
        }

        SetCursorPos(p.x, p.y);
    }
}

void RunFrameUpdateSetCursor()
{
    float aspect = (float)screenLayout.width / (float)screenLayout.height;

    //----
    // Cursor
    //----
    static int currentCursorID = -9;
    int cursorID = *(int*)0x00CF5750; // fix #54: ORIGINAL upstream code restored, with the TBC cursor-id global (WotLK 0xC26DE8 -> TBC 0xCF5750, disasm-verified). Reads the live WoW cursor exactly like the working backup.
    if (cfg_disableControllers)
    {
        if (cursorID == 0 || cursorID == 1)
            cursorID = 28;
        else if (cursorID == 53)
            cursorID = 1;
    }
    else
    {
        if (cursorID == 1)
            cursorID = -1;
        else if (cursorID == 53)
            cursorID = 1;
    }

    if (currentCursorID != (cursorID - 1))
    {
        currentCursorID = (cursorID - 1);

        // Single-cursor image (no 26x2 atlas): use the WHOLE png as one cursor sprite,
        // so an original square cursor.png (transparent) maps directly and we ship no
        // copyrighted cursor atlas. cursorID is still tracked so the buffer only rebuilds
        // on change; the sprite itself is always the full texture.
        float uv[] = { 0.0f, 0.0f, 1.0f, 1.0f };

        std::vector<float> squareData = {
                 0, -1,  0,    uv[0], uv[3],
                 0,  0,  0,    uv[0], uv[1],
                 1,  0,  0,    uv[2], uv[1],
                 1, -1,  0,    uv[2], uv[3],
        };
        cursorUI.SetVertexBuffer(squareData, 5, true);
    }

    float x = screenLayout.width / 2;
    float y = screenLayout.height / 2;

    if (cfg_disableControllers)
    {
        POINT p = { 0, 0 };
        GetCursorPos(&p);
        ScreenToClient(screenLayout.hwnd, &p);
        x = ((float)p.x / screenLayout.width) * 2 - 1;
        y = -(((float)p.y / screenLayout.height) * 2 - 1);
        if (cursorID == 28)
            cursorID = -1;
    }
    XMMATRIX cursorScale = (cfg_disableControllers) ? XMMatrixScaling(0.075f / aspect, 0.075f, 0.075f) : zeroScale;
    XMMATRIX cursorOffset = XMMatrixTranslation(x, y, 0.0f);
    cursorUI.SetObjectMatrix(cursorScale * cursorOffset);
}

void RunFrameUpdateController()
{
    struct intersectLayout
    {
        RenderObject* item;
        bool* atUI;
        stScreenLayout* layout;
        std::vector<intersectPoint> intersection;
        std::vector<bool> interaction;
        unsigned int multiplier;
        bool updateDistance;
        bool forceMouse;
        bool fromCenter;
    };

    byte uiClear[4] = { 0, 0, 0, 0 };
    bool curvedUIAtUI = false;
 
    float aspect = (float)screenLayout.width / (float)screenLayout.height;
    POINT halfScreen = { screenLayout.width / 2, screenLayout.height / 2 };

    //----
    // Add all the interactable items to the intersect list
    //----
    std::list<intersectLayout> intersectList = std::list<intersectLayout>();
    XMMATRIX uiScaleMatrix = XMMatrixScaling(cfg_uiOffsetScale, cfg_uiOffsetScale, cfg_uiOffsetScale);
    XMMATRIX uiZMatrix = XMMatrixTranslation(0.0f, 0.0f, (cfg_uiOffsetZ / 100.0f));
    XMMATRIX moveMatrix = XMMatrixTranslation(0.0f, (cfg_uiOffsetY / 100.0f), 0.0f);
    XMMATRIX playerOffset = (uiScaleMatrix * uiZMatrix * moveMatrix);

        XMMATRIX aspectScaleMatrix = XMMatrixScaling(aspect, 1, 1);
        curvedUI.SetObjectMatrix(aspectScaleMatrix * playerOffset);// *(uiScaleMatrix* uiZMatrix* moveMatrix));
        
        //              item, atUI, layout, intersection, dist, multiplier, updateDistance, forceMouse, fromCenter;
        intersectList.push_back({ &curvedUI, &curvedUIAtUI, &screenLayout, std::vector<intersectPoint>(), std::vector<bool>(), 1, isOverUI, false, true });

    //----

    RunFrameUpdateSetCursor();
 
    //XMMATRIX rayMatrix = matController[1];
    XMMATRIX rayMatrix = (matController[1]);
    XMVECTOR origin = { rayMatrix._41, rayMatrix._42, rayMatrix._43 };
    XMVECTOR frwd = { rayMatrix._31, rayMatrix._32, rayMatrix._33 };
    XMVECTOR originS = origin + ((frwd * -1) * 0.1f);
    XMVECTOR norm = XMVector3Normalize(frwd);

    //----
    // Go though all interactable items and check to see if the ray interacts with something
    //----
    int maskWidth = uiBufferSize.x / 4;
    int maskHeight = uiBufferSize.y / 4;

    float dist = -9999;
    intersectLayout closest = intersectLayout();
    int closestIndex = -1;
    HRESULT result = S_OK;
    //isOverUI = false;
    for (std::list<intersectLayout>::iterator it = intersectList.begin(); it != intersectList.end(); ++it)
    {
        if (it->layout == nullptr || it->layout->haveLayout)
        {
            *(it->atUI) = it->item->RayIntersection(originS, norm, &it->intersection, it->interaction, &logError);
            if(*(it->atUI) == true)
            {
                for (int i = 0; i < it->intersection.size(); i++)
                {
                    bool isOverUIElement = true;
                    if (curvedUIAtUI)
                    {
                        POINT tmpPos = { it->intersection[i].point.vector4_f32[0] * maskWidth, it->intersection[i].point.vector4_f32[1] * maskHeight };
                        RECT fromBox = { tmpPos.x, tmpPos.y, tmpPos.x + 1, tmpPos.y + 1 };
                        result = devDX9->StretchRect(uiRenderMask.pShaderResource, &fromBox, uiRenderCheck.pShaderResource, NULL, D3DTEXF_NONE);
                        result = devDX9->GetRenderTargetData(uiRenderCheck.pShaderResource, uiRenderCheckSystem.pShaderResource);

                        D3DLOCKED_RECT lr;
                        ZeroMemory(&lr, sizeof(D3DLOCKED_RECT));
                        result = uiRenderCheckSystem.pShaderResource->LockRect(&lr, 0, D3DLOCK_READONLY);
                        byte* pixel = (byte*)lr.pBits;
                        result = uiRenderCheckSystem.pShaderResource->UnlockRect();

                        if (pixel[0] == 0) // not over ui element
                            isOverUIElement = false;
                    }

                    if (isOverUIElement && it->intersection[i].distance >= dist)
                    {
                        dist = it->intersection[i].distance;
                        closest = *it;
                        closestIndex = i;
                        //isOverUI = true;
                    }
                }
            }
        }
    }

    if (closest.item != nullptr && closestIndex >= 0)
    {
        HWND useHWND = 0;
        if (closest.layout != nullptr)
        {
            halfScreen.x = (closest.layout->width / 2);
            halfScreen.y = (closest.layout->height / 2);

            //----
            // converts uv (0.0->1.0) to screen coords | width/height
            //----
            closest.intersection[closestIndex].point.vector4_f32[0] = closest.intersection[closestIndex].point.vector4_f32[0] * closest.layout->width;
            closest.intersection[closestIndex].point.vector4_f32[1] = closest.intersection[closestIndex].point.vector4_f32[1] * closest.layout->height;

            //----
            // Changes anchor from top left corner to middle of screen
            //----
            if (closest.fromCenter)
            {
                closest.intersection[closestIndex].point.vector4_f32[0] = halfScreen.x + (closest.intersection[closestIndex].point.vector4_f32[0] - halfScreen.x);
                closest.intersection[closestIndex].point.vector4_f32[1] = halfScreen.y + (closest.intersection[closestIndex].point.vector4_f32[1] - halfScreen.y);
            }
            useHWND = closest.layout->hwnd;
        }

        SetMousePosition(useHWND, (int)closest.intersection[closestIndex].point.vector4_f32[0], (int)closest.intersection[closestIndex].point.vector4_f32[1], closest.forceMouse);
    }
    else// if (!curvedUIAtUI)
    {
        SetMousePosition(screenLayout.hwnd, halfScreen.x, halfScreen.y, false);
    }
}


// Skybox fix? - disable makes skybox work on all viewports but kills water
void(__thiscall* sub_6A38D0)(void*) = (void(__thiscall*)(void*))TBC_SKIP_sub_SkyboxFix;
void(__fastcall msub_6A38D0)(void* ecx, void* edx)
{
    static bool show = false;
    static bool doSBS = false;
    if (show) {
        sub_6A38D0(ecx);
    }
}

// GreyBoxes
void (*sub_796C10)(int, int, int, int, int) = (void (*)(int, int, int, int, int))TBC_SKIP_sub_GreyBoxes;
void (msub_796C10)(int a, int b, int c, int d, int e)
{
    //sub_796C10(a, b, c, d, e);
}

// fix #58-instr: two signature-agnostic counting detours. We do NOT know the exact
// calling convention / arg count of these engine subs, so a normal C hook that
// re-calls the original would risk the fix #22 stack-corruption class of bug.
// Instead each hook is a NAKED trampoline: increment g_*Hits[curEye], then a bare
// jmp to the Detours trampoline (which runs the saved prologue + jmps back into the
// real function). No call frame is formed, so ALL registers/stack args and the
// callee's own `ret N` convention are preserved perfectly. Only eax + flags are
// clobbered before the real function runs, and no calling convention passes an
// incoming argument in eax, so this is safe for any convention.
void (*sub_69A190)() = (void(*)())TBC_sub_WorldSceneRender;  // WorldSceneRender
__declspec(naked) void msub_69A190()
{
    __asm {
        mov  eax, curEye
        inc  dword ptr [g_wsrHits + eax*4]
        jmp  dword ptr [sub_69A190]
    }
}

void (*sub_6E5DC0)() = (void(*)())TBC_sub_DNSkyPass;  // DNSkyPass
// fix #70 probe: full C hook (DNSkyPass is cdecl, void, no args - a plain C hook is safe).
// (a) increments the per-eye hit counter (same as the old naked detour); (b) when armed by
// OnPaint (g_skyProbeArm, once every 600 frames for one frame), logs the decisive sky state
// as the engine has it AT the sky pass entry - this is the exact moment the earlier snapshots
// missed, since these globals are cleared/recomputed inside the world render pass; (c) calls
// the original. Both eyes of the armed frame emit one line each (2-3 lines per arm).
void msub_6E5DC0_full()
{
    int e = curEye;
    if (e >= 0 && e < 3)
        g_skyPassHits[e]++;
    if (g_skyProbeArm) {
        int   vis  = *(int*)0x00DA54B0;   // sky visible latch
        int   name = *(int*)0x00DA54AC;   // sky name/atlas ptr
        int   ovr  = *(int*)0x00DA5628;   // sky override
        int   dn   = *(int*)0x00E18F9C;   // day/night
        int   dc   = *(int*)0x00E18DC0;   // day-night controller
        int   s0   = *(int*)0x00E18E48;   // sky model slot 0
        float a0   = *(float*)0x00E18E4C; // slot 0 alpha
        int   s1   = *(int*)0x00E18E50;   // sky model slot 1
        float a1   = *(float*)0x00E18E5C; // slot 1 alpha
        float e78  = *(float*)0x00E18E78; // blend/time factor
        int   m    = (s0 != 0) ? *(int*)(s0 + 0x18) : 0;   // model ptr
        int   mf   = (m  != 0) ? *(int*)(m  + 0x10) : 0;   // model flags (bit0 = file loaded)
        char nbuf[32] = { 0 };
        if (name > 0x10000) {             // only deref if it looks like a real pointer
            const char* np = (const char*)name;
            for (int i = 0; i < 24 && np[i]; i++) nbuf[i] = np[i];
        }
        char sbuf[256];
        sprintf_s(sbuf, sizeof(sbuf),
            "[skyprobe] eye=%d vis=%d name=%08X(%s) ovr=%d dn=%d dc=%08X "
            "s0=%08X a0=%.3f s1=%08X a1=%.3f e78=%.3f m=%08X mf=%08X",
            e, vis, name, nbuf, ovr, dn, dc, s0, a0, s1, a1, e78, m, mf);
        ofOut << sbuf << std::endl; ofOut.flush();
    }
    sub_6E5DC0();
}

// fix #60-instr: identical safe naked counting detours on the three candidate
// world-render sub-passes called after the sky pass inside WorldSceneRender.
void (*sub_6975A0)() = (void(*)())0x006975A0;  // WSR sub-pass candidate
__declspec(naked) void msub_6975A0()
{
    __asm {
        mov  eax, curEye
        inc  dword ptr [g_wsr75A0Hits + eax*4]
        jmp  dword ptr [sub_6975A0]
    }
}

void (*sub_6999C0)() = (void(*)())0x006999C0;  // WSR sub-pass candidate
__declspec(naked) void msub_6999C0()
{
    __asm {
        mov  eax, curEye
        inc  dword ptr [g_wsr99C0Hits + eax*4]
        jmp  dword ptr [sub_6999C0]
    }
}

void (*sub_695840)() = (void(*)())0x00695840;  // WSR sub-pass candidate
__declspec(naked) void msub_695840()
{
    __asm {
        mov  eax, curEye
        inc  dword ptr [g_wsr5840Hits + eax*4]
        jmp  dword ptr [sub_695840]
    }
}

// fix #61-instr: same safe naked counting detours on the visible-list fill
// primitive and the CMap terrain render, to confirm the pre-pass cull actually
// re-fills the list before the eye loop. NOTE: both also fire in the engine
// UPDATE phase (curEye may be stale there), so read these alongside the raw
// head/tail snapshots in the [cull] log line.
void (*sub_699F60)() = (void(*)())0x00699F60;  // visible-list fill primitive
__declspec(naked) void msub_699F60()
{
    __asm {
        mov  eax, curEye
        inc  dword ptr [g_fillHits + eax*4]
        jmp  dword ptr [sub_699F60]
    }
}

void (*sub_70B4A0)() = (void(*)())0x0070B4A0;  // CMap terrain render
__declspec(naked) void msub_70B4A0()
{
    __asm {
        mov  eax, curEye
        inc  dword ptr [g_cmapHits + eax*4]
        jmp  dword ptr [sub_70B4A0]
    }
}

// ===========================================================================
// Addon -> mod live-config bridge (2026-07-13). A WoW addon calls
// SetCVar("vrXxx", value); we intercept any cvar name starting with "vr", apply
// it to the SAME globals the .cfg parser writes, and swallow the call so the game
// does not print "Couldn't find CVar named 'vrXxx'". One-way, live, no file.
// Verified addresses (client 2.4.3 build 8606): Lua glue SetCVar = 0x00472320
// (plus an identical clone at 0x004997A0 registered in a 2nd table -> hook both;
// whichever the live Lua state does not bind simply never fires). Reading args:
// FrameScript_GetText(L, luaIndex, 0) = 0x0072DFF0 returns a Lua arg as a string.
int(__cdecl* sub_472320)(void*) = (int(__cdecl*)(void*))0x00472320;
int(__cdecl* sub_4997A0)(void*) = (int(__cdecl*)(void*))0x004997A0;
const char*(__cdecl* g_FrameScript_GetText)(void*, int, int) =
    (const char*(__cdecl*)(void*, int, int))0x0072DFF0;

static void VR_ApplyCVar(const char* name, const char* val)
{
    if (!name || !val) return;
    if (_stricmp(name, "vrDumpEyes") == 0) {   // a command, not a setting: arm the 3D-screenshot eye dump
        g_dumpEyesRequested = true;
        ofOut << "[cvar] vrDumpEyes -> eye dump armed" << std::endl; ofOut.flush();
        return;                                 // do NOT set g_addonConfigActive for a one-shot command
    }
    const double d = atof(val);
    const int    i = atoi(val);
    #define VR_IS(n) (_stricmp(name, n) == 0)
    if      (VR_IS("vrWorldScale"))       { if (d >= 0.1  && d <= 4.0)      g_worldScale = (float)d; }
    else if (VR_IS("vrScreenSize"))       { if (d >= 0.1  && d <= 5.0)      cfg_uiOffsetScale = (float)d; }
    else if (VR_IS("vrScreenDistance"))   { if (d >= -1000.0 && d <= 1000.0) cfg_uiOffsetZ = (float)d; }
    else if (VR_IS("vrScreenHeight"))     { if (d >= -1000.0 && d <= 1000.0) cfg_uiOffsetY = (float)d; }
    else if (VR_IS("vrNameplateDepth"))   { if (d >= 0.0  && d <= 1.0)      g_nameplateDepthScale = (float)d; }
    else if (VR_IS("vrCrosshairSize"))    { if (d >= 1.0  && d <= 64.0)     g_crosshairSize = (float)d; }
    else if (VR_IS("vrCrosshairOffset"))  { if (d >= -500.0 && d <= 500.0)  g_crosshairOffset = (float)d; }
    else if (VR_IS("vrCrosshairYoffset")) { if (d >= -500.0 && d <= 500.0)  g_crosshairYOffset = (float)d; }
    else if (VR_IS("vrMouseoverBright"))  { if (d >= 1.0  && d <= 5.0)      g_hlMouseBright = (float)d; }
    else if (VR_IS("vrTargetBright"))     { if (d >= 1.0  && d <= 5.0)      g_hlTargetBright = (float)d; }
    else if (VR_IS("vrAimRings"))         { if (i >= 0    && i <= 4)        g_aimRings = i; }
    else if (VR_IS("vrAimSpreadDeg"))     { if (d >= 0.1  && d <= 15.0)     g_aimSpreadDeg = (float)d; }
    else if (VR_IS("vrAimSamples"))       { if (i >= 3    && i <= 16)       g_aimSamples = i; }
    else if (VR_IS("vrCrosshair"))        { g_crosshair = (i != 0) ? 1 : 0; }
    else if (VR_IS("vrAimAssist"))        { g_aimAssist = (i != 0) ? 1 : 0; }
    else if (VR_IS("vrNameplateOcclusion")) { ApplyNameplateMode((i != 0) ? 2 : 0); }  // legacy alias
    else if (VR_IS("vrNameplateMode"))      { ApplyNameplateMode(i); }                 // 0 orig, 1 3D, 2 NewPlate
    else if (VR_IS("vrNameplateZForce"))  { if (d >= -1.0 && d <= 1.0)      g_nameplateZForce = (float)d; }
    else return;  // unknown vr* name: ignore silently
    #undef VR_IS
    g_addonConfigActive = true;   // addon owns config now; stop the cfg-file clobber
    ofOut << "[cvar] " << name << " = " << val << std::endl; ofOut.flush();
}

static int VR_SetCVarBridge(void* L, int(__cdecl* orig)(void*))
{
    const char* name = g_FrameScript_GetText ? g_FrameScript_GetText(L, 1, 0) : nullptr;
    if (name && (name[0] == 'v' || name[0] == 'V') && (name[1] == 'r' || name[1] == 'R')) {
        const char* val = g_FrameScript_GetText(L, 2, 0);
        VR_ApplyCVar(name, val ? val : "0");
        return 0;  // consumed: no engine call, no "unknown CVar" error, 0 Lua results
    }
    return orig(L);  // not ours: run the real SetCVar (trampoline)
}
int(__cdecl msub_472320)(void* L) { return VR_SetCVarBridge(L, sub_472320); }
int(__cdecl msub_4997A0)(void* L) { return VR_SetCVarBridge(L, sub_4997A0); }

// TBC port: null-tolerant detour install. Each hook only attaches if its
// target address is non-zero. Logs which hooks were installed/skipped to
// vr/output.txt so we know the actual coverage at runtime.
//
// SAFE_DETOUR_ATTACH: install detour iff target is non-NULL, count, and log.
#define SAFE_DETOUR_ATTACH(target, replacement, label) \
    do { \
        if ((target) != nullptr) { \
            DetourAttach((PVOID*)&(target), (PVOID)(replacement)); \
            ofOut << "[detour:install] " label " @ 0x" << std::hex << (DWORD)(target) << std::dec << std::endl; \
            installedCount++; \
        } else { \
            ofOut << "[detour:skip-NULL] " label " (TODO_TBC address)" << std::endl; \
            skippedCount++; \
        } \
    } while (0)

#define SAFE_DETOUR_DETACH(target, replacement) \
    do { \
        if ((target) != nullptr) { \
            DetourDetach((PVOID*)&(target), (PVOID)(replacement)); \
        } \
    } while (0)

// TBC port: TEST_LEVEL controls which hooks attach.
//   0 = NO hooks (pure D3D9 proxy passthrough; verify game starts)
//   1 = only D3D9 lifecycle (CreateDxDev, BeginScene, EndScene, Present)
//   2 = level 1 + StartRender + OnPaint (mono image on HMD attempt)
//   3 = all hooks (full VR — may crash if struct offsets wrong)
#ifndef upstream_TEST_LEVEL
#define upstream_TEST_LEVEL 3
#endif

void InitDetours(HANDLE hModule)
{
    int installedCount = 0;
    int skippedCount = 0;
    ofOut << "=== InitDetours (TBC port, TEST_LEVEL=" << upstream_TEST_LEVEL << ") ===" << std::endl;

    if (upstream_TEST_LEVEL == 0) {
        ofOut << "Test level 0: no hooks installed - pure D3D9 proxy passthrough." << std::endl;
        ofOut.flush();
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    SAFE_DETOUR_ATTACH(sub_4BF0F0, msub_4BF0F0, "MouseToWorldRay");
    // RE-ENABLED 2026-07-12: earlier "kills the mouse cursor" was actually the wannabe
    // test folder being corrupted (a non-DLL file), NOT this hook - our DLL keeps the
    // mouse in the clean backup folder. Testing raycast in a clean folder now.
    SAFE_DETOUR_ATTACH(sub_4AEB10, msub_4AEB10, "AimAssistTrace");

    SAFE_DETOUR_ATTACH(sub_869DB0, msub_869DB0, "SetClientMouseResetPoint");
    SAFE_DETOUR_ATTACH(sub_684D70, msub_684D70, "CalcWindowSize");
    SAFE_DETOUR_ATTACH(sub_68EBB0, msub_68EBB0, "CreateWindow");
    SAFE_DETOUR_ATTACH(sub_6A08D0, msub_6A08D0, "CreateWindowEx");
    SAFE_DETOUR_ATTACH(sub_6904D0, msub_6904D0, "CreateDxDevice");
    SAFE_DETOUR_ATTACH(sub_6A2040, msub_6A2040, "CreateDxDeviceEx");
    // fix #24: CloseDxDevice re-enabled with the disasm-verified address 0x5A5640
    // (the old bindiff match 0x845F41 fired spuriously and killed VR — fix #14).
    SAFE_DETOUR_ATTACH(sub_6903B0, msub_6903B0, "CloseDxDevice");
    SAFE_DETOUR_ATTACH(sub_6A1F40, msub_6A1F40, "CloseDxDeviceEx");
    SAFE_DETOUR_ATTACH(sub_6A73E0, msub_6A73E0, "BeginSceneSetup");
    SAFE_DETOUR_ATTACH(sub_6A7540, msub_6A7540, "EndSceneSetup");
    SAFE_DETOUR_ATTACH(sub_6A7610, msub_6A7610, "PresentScene");

    // TBC port fix #22: ShouldRenderChar hook DISABLED. Its first in-world call
    // was always the last trace before a 0%-CPU hang or a wild-jump crash
    // (EIP=0x448D3A18) — the WotLK signature __thiscall(void*,int,int,int) does
    // not match TBC 0x5E8BF0, so the passthrough corrupts the stack on return.
    // The hook body was already gated (hide-player cosmetics only). Re-enable
    // after verifying the TBC signature in Ghidra.
    ofOut << "[detour:skip-BAD-SIG] ShouldRenderChar (stack corruption on passthrough - WotLK signature mismatch)" << std::endl; ofOut.flush();
    SAFE_DETOUR_ATTACH(sub_5FF530, msub_5FF530, "UpdateFreelookCamera");
    SAFE_DETOUR_ATTACH(sub_606F90, msub_606F90, "UpdateCameraFn");
    SAFE_DETOUR_ATTACH(sub_77EFF0, msub_77EFF0, "SlowsAnim");
    SAFE_DETOUR_ATTACH(sub_82F0F0, msub_82F0F0, "DynamicAnim");
    SAFE_DETOUR_ATTACH(sub_6A9B40, msub_6A9B40, "UpdateModelProj");
    SAFE_DETOUR_ATTACH(sub_4AC810, msub_4AC810, "WorldToScreen");   // fix #74 option B
    SAFE_DETOUR_ATTACH(sub_611260, msub_611260, "NameplateApplyLoop");  // fix #74c bracket
    SAFE_DETOUR_ATTACH(sub_433000, msub_433000, "LayoutFrameSetPoint"); // fix #74c intercept
    SAFE_DETOUR_ATTACH(sub_6110F0, msub_6110F0, "NameplateCommit");     // engine-depth pairing
    SAFE_DETOUR_ATTACH(sub_625450, msub_625450, "SetHighlight");        // fix #75 highlight
    SAFE_DETOUR_ATTACH(sub_6253E0, msub_6253E0, "ClearHighlight");      // fix #75 fullbright reset
    SAFE_DETOUR_ATTACH(sub_70D150, msub_70D150, "HL_BatchPreDraw");     // fix #75 additive arm
    SAFE_DETOUR_ATTACH(sub_711550, msub_711550, "HL_CM2SceneDraw");     // fix #75 additive disarm bracket
    SAFE_DETOUR_ATTACH(sub_6D5320, msub_6D5320, "NameDrawBar");         // world-space bar in the name slot
    SAFE_DETOUR_ATTACH(sub_616FF0, msub_616FF0, "NameTextAppend");      // append health bar line to name text
    SAFE_DETOUR_ATTACH(sub_613CE0, msub_613CE0, "NameShowWithPlate");   // keep names visible for plated units
    SAFE_DETOUR_ATTACH(sub_5C6F40, msub_5C6F40, "NameBarWeld");         // weld '#' glyphs into a solid bar quad
    SAFE_DETOUR_ATTACH(sub_5C5D30, msub_5C5D30, "NameBarFrameTex");     // textured frame after the string draw
    SAFE_DETOUR_ATTACH(sub_687A90, msub_687A90, "RenderMouse");
    SAFE_DETOUR_ATTACH(sub_494F30, msub_494F30, "StartUI");
    SAFE_DETOUR_ATTACH(sub_495410, msub_495410, "StartRender");
    SAFE_DETOUR_ATTACH(sub_4A8720, msub_4A8720, "OnPaint");

    SAFE_DETOUR_ATTACH(sub_6A38D0, msub_6A38D0, "SkyboxFix");
    SAFE_DETOUR_ATTACH(sub_796C10, msub_796C10, "GreyBoxes");

    // fix #58-instr: diagnostic counting detours (naked passthrough).
    SAFE_DETOUR_ATTACH(sub_69A190, msub_69A190, "WorldSceneRender[instr]");
    SAFE_DETOUR_ATTACH(sub_6E5DC0, msub_6E5DC0_full, "DNSkyPass[skyprobe70]");

    // fix #60-instr: count the three candidate WSR sub-passes after the sky pass.
    SAFE_DETOUR_ATTACH(sub_6975A0, msub_6975A0, "WSRsub_6975A0[instr]");
    SAFE_DETOUR_ATTACH(sub_6999C0, msub_6999C0, "WSRsub_6999C0[instr]");
    SAFE_DETOUR_ATTACH(sub_695840, msub_695840, "WSRsub_695840[instr]");

    // fix #61-instr: confirm the pre-pass cull re-fills the visible-area lists.
    SAFE_DETOUR_ATTACH(sub_699F60, msub_699F60, "VisListFill[instr]");
    SAFE_DETOUR_ATTACH(sub_70B4A0, msub_70B4A0, "CMapTerrainRender[instr]");

    // Addon -> mod live-config bridge (see VR_SetCVarBridge). Hook both SetCVar
    // glue copies; the one the live Lua state does not bind simply never fires.
    SAFE_DETOUR_ATTACH(sub_472320, msub_472320, "SetCVarBridge");
    SAFE_DETOUR_ATTACH(sub_4997A0, msub_4997A0, "SetCVarBridge2");

    LONG result = DetourTransactionCommit();
    ofOut << "=== InitDetours done: " << installedCount << " installed, "
          << skippedCount << " skipped (NULL TODO_TBC), commit="
          << ((result == NO_ERROR) ? "OK" : "FAIL") << " ===" << std::endl;
    ofOut.flush();

    if (result == NO_ERROR)
        OutputDebugString("detoured successfully started");
    return;
}


void ExitDetours()
{
    //if (doLog) logError << "-- ExitDetours Start" << std::endl;
    /*
    logError << "s:" << cfg_uiOffsetScale << " : z:" << cfg_uiOffsetZ << " : y:" << cfg_uiOffsetY << " : d:" << cfg_uiOffsetD << std::endl;
    
    for (unsigned int i = 0; i < uiViewGame.size(); i++)
    {
        logError << i << ": o: " << uiViewGame.at(i).offset.x << ", " << uiViewGame.at(i).offset.y << ", " << uiViewGame.at(i).offset.z << std::endl;
        logError << i << ": r: " << uiViewGame.at(i).rotation.x << ", " << uiViewGame.at(i).rotation.y << ", " << uiViewGame.at(i).rotation.z << std::endl;
        logError << i << ": s: " << uiViewGame.at(i).scale.x << ", " << uiViewGame.at(i).scale.y << ", " << uiViewGame.at(i).scale.z << std::endl;
    }
    */
    
    
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    SAFE_DETOUR_DETACH(sub_4BF0F0, msub_4BF0F0);
    SAFE_DETOUR_DETACH(sub_4AEB10, msub_4AEB10);  // aim assist re-enabled 2026-07-12
    SAFE_DETOUR_DETACH(sub_869DB0, msub_869DB0);
    SAFE_DETOUR_DETACH(sub_684D70, msub_684D70);
    SAFE_DETOUR_DETACH(sub_68EBB0, msub_68EBB0);
    SAFE_DETOUR_DETACH(sub_6A08D0, msub_6A08D0);
    SAFE_DETOUR_DETACH(sub_6904D0, msub_6904D0);
    SAFE_DETOUR_DETACH(sub_6A2040, msub_6A2040);
    SAFE_DETOUR_DETACH(sub_6903B0, msub_6903B0);  // fix #24: re-enabled
    SAFE_DETOUR_DETACH(sub_6A1F40, msub_6A1F40);
    SAFE_DETOUR_DETACH(sub_6A73E0, msub_6A73E0);
    SAFE_DETOUR_DETACH(sub_6A7540, msub_6A7540);
    SAFE_DETOUR_DETACH(sub_6A7610, msub_6A7610);
    // fix #22: ShouldRenderChar never attached - do not detach.
    SAFE_DETOUR_DETACH(sub_5FF530, msub_5FF530);
    SAFE_DETOUR_DETACH(sub_606F90, msub_606F90);
    SAFE_DETOUR_DETACH(sub_77EFF0, msub_77EFF0);
    SAFE_DETOUR_DETACH(sub_82F0F0, msub_82F0F0);
    SAFE_DETOUR_DETACH(sub_6A9B40, msub_6A9B40);
    SAFE_DETOUR_DETACH(sub_4AC810, msub_4AC810);
    SAFE_DETOUR_DETACH(sub_611260, msub_611260);  // fix #74c
    SAFE_DETOUR_DETACH(sub_433000, msub_433000);  // fix #74c
    SAFE_DETOUR_DETACH(sub_6110F0, msub_6110F0);  // engine-depth pairing
    SAFE_DETOUR_DETACH(sub_625450, msub_625450);  // fix #75
    SAFE_DETOUR_DETACH(sub_6253E0, msub_6253E0);  // fix #75
    SAFE_DETOUR_DETACH(sub_70D150, msub_70D150);  // fix #75 additive arm
    SAFE_DETOUR_DETACH(sub_711550, msub_711550);  // fix #75 additive disarm bracket
    SAFE_DETOUR_DETACH(sub_6D5320, msub_6D5320);  // world-space bar in the name slot
    SAFE_DETOUR_DETACH(sub_616FF0, msub_616FF0);  // append health bar line to name text
    SAFE_DETOUR_DETACH(sub_613CE0, msub_613CE0);  // keep names visible for plated units
    SAFE_DETOUR_DETACH(sub_5C6F40, msub_5C6F40);  // weld '#' glyphs into a solid bar quad
    SAFE_DETOUR_DETACH(sub_5C5D30, msub_5C5D30);  // textured frame after the string draw
    SAFE_DETOUR_DETACH(sub_687A90, msub_687A90);
    SAFE_DETOUR_DETACH(sub_494F30, msub_494F30);
    SAFE_DETOUR_DETACH(sub_495410, msub_495410);
    SAFE_DETOUR_DETACH(sub_4A8720, msub_4A8720);
    SAFE_DETOUR_DETACH(sub_6A38D0, msub_6A38D0);
    SAFE_DETOUR_DETACH(sub_796C10, msub_796C10);
    SAFE_DETOUR_DETACH(sub_69A190, msub_69A190);  // fix #58-instr
    SAFE_DETOUR_DETACH(sub_6E5DC0, msub_6E5DC0_full);  // fix #58-instr / #70 skyprobe
    SAFE_DETOUR_DETACH(sub_6975A0, msub_6975A0);  // fix #60-instr
    SAFE_DETOUR_DETACH(sub_6999C0, msub_6999C0);  // fix #60-instr
    SAFE_DETOUR_DETACH(sub_695840, msub_695840);  // fix #60-instr
    SAFE_DETOUR_DETACH(sub_699F60, msub_699F60);  // fix #61-instr
    SAFE_DETOUR_DETACH(sub_70B4A0, msub_70B4A0);  // fix #61-instr
    SAFE_DETOUR_DETACH(sub_472320, msub_472320);  // addon SetCVar bridge
    SAFE_DETOUR_DETACH(sub_4997A0, msub_4997A0);  // addon SetCVar bridge (clone)

    if (DetourTransactionCommit() == NO_ERROR)
        OutputDebugString("detoured successfully stopped");
    return;
}



void (*moveForwardStart)() = (void (*)())0x005FC200; // TODO_TBC
void (*moveForwardStop)() = (void (*)())0x005FC250; // TODO_TBC
void (*moveBackwardStart)() = (void (*)())0x005FC290; // TODO_TBC
void (*moveBackwardStop)() = (void (*)())0x005FC2E0; // TODO_TBC

void (*moveLeftStart)() = (void (*)())0x005FC440; // TODO_TBC
void (*moveLeftStop)() = (void (*)())0x005FC490; // TODO_TBC
void (*moveRightStart)() = (void (*)())0x005FC4D0; // TODO_TBC
void (*moveRightStop)() = (void (*)())0x005FC520; // TODO_TBC

void (*turnLeftStart)() = (void (*)())0x005FC320; // TODO_TBC
void (*turnLeftStop)() = (void (*)())0x005FC360; // TODO_TBC
void (*turnRightStart)() = (void (*)())0x005FC3B0; // TODO_TBC
void (*turnRightStop)() = (void (*)())0x005FC3F0; // TODO_TBC

void (*jumpOrAscendStart)() = (void (*)())0x005FBF80; // TODO_TBC
void (*jumpOrAscendStop)() = (void (*)())0x005FC0A0; // TODO_TBC
void (*sitOrDescendStart)() = (void (*)())0x0051B1D0; // TODO_TBC
void (*sitOrDescendStop)() = (void (*)())0x005FC140; // TODO_TBC


bool rightStickXCenter = false;
bool rightStickYCenter = false;

void setVerticalRotation(float rotation)
{
    int camera = GetActiveCameraSafe();
    if (gPlayerObj && gPlayerObj->ptrObjectData && camera)
        gPlayerObj->ptrObjectData->objPitch = rotation;
}

void setHorizontalRotation(float rotation, float camOffset, bool mouseHold)
{
    int camera = GetActiveCameraSafe();
    if (gPlayerObj && gPlayerObj->ptrObjectData && camera)
    {
        CGMovementInfo__SetFacing((int)gPlayerObj->ptrObjectData, rotation);
        *(float*)(camera + TBC_Camera::yaw) = -(rotation - camOffset);  // fix #28: was raw WotLK +0x11C
        if (mouseHold)
            *(float*)(camera + 0x11C) = camOffset;
    }
}

bool IsPlayerRunning()
{
    if (gPlayerObj && gPlayerObj->ptrObjectData)
        return ((gPlayerObj->ptrObjectData->MovementStatus & 0x100) == 0);
    return false;
}

float DiffObjFaceObj(stObjectManager* viewerObj, stObjectManager* targetObj)
{
    if (viewerObj && viewerObj->ptrObjectData && targetObj && targetObj->ptrObjectData)
    {
        Vector3 oV = viewerObj->ptrObjectData->objPos;
        Vector3 tV = targetObj->ptrObjectData->objPos;
        float curRotation = viewerObj->ptrObjectData->objRot;

        Vector3 distance;
        distance.x = tV.x - oV.x;
        distance.y = tV.y - oV.y;
        distance.z = tV.z - oV.z;

        float posRotation = EnsureProperRadians(std::atan2f(distance.y, distance.x));
        return posRotation;
        //if (std::fabs(posRotation - curRotation) > 0.5f)
        //    return posRotation - curRotation;
        //else
        //    return posRotation - gRotation;
    }
    return 0;
}
