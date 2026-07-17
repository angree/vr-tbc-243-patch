#pragma once

// fix #63-instr: per-eye D3D9 command recorder.
//
// The game's IDirect3DDevice9 is our proxy wrapper (cIDirect3DDevice9), so every
// SetRenderTarget / Clear / SetRenderState / DrawIndexedPrimitive the engine issues
// passes through our methods. We record one full frame's command stream per eye pass
// into a fixed buffer and dump it from OnPaint, to compare the left-eye pass (missing
// terrain + sky) against the right-eye pass at the raw D3D call level.
//
// Shared by game_extras.cpp (owns the buffer + arms + dumps) and
// cIDirect3DDevice9.cpp (the recording call sites). Definitions live in game_extras.cpp.

struct RecEntry { unsigned char eye; unsigned char op; unsigned int a; unsigned int b; };

// op codes:
//  1 = SetRenderTarget       (a = surface ptr)
//  2 = SetDepthStencilSurface(a = surface ptr)
//  3 = Clear                 (a = flags,  b = color)
//  4 = SetRenderState        (a = state,  b = value)  -- only Z/fog states (see RecMaybeRS)
//  5 = DrawIndexedPrimitive  (a = current stage0 texture ptr, b = PrimCount)
//  6 = DrawPrimitive         (a = current stage0 texture ptr, b = PrimCount)
//  7 = SetTexture stage0     (a = texture ptr)  -- also updates g_recCurTex
//  8 = SetViewport           (a = X<<16|Y, b = Width<<16|Height)

static const int REC_CAP = 20000;

// fix #71: while the warm-up world pass runs, our proxy swallows draw calls (returns
// D3D_OK without forwarding) - the engine still does all CPU-side traversal/state work
// (which is what primes the per-frame sky/far completion), but the D3D9 draw submission
// cost disappears. States/textures ARE still forwarded to keep the device state machine
// consistent for the real eye passes.
extern volatile int          g_warmupSkipDraws;

// Nameplate text-bar: while the name-draw window is armed (g_nameBarTid == render
// thread), the wrapper's SetTexture(stage 0) captures the font-atlas texture so the
// weld code can scan the '#' glyph once for a fully-opaque texel (game_extras.cpp).
extern volatile unsigned long g_nameBarTid;
extern void* volatile         g_nameBarTex0;

// fix #75: TRUE OVERBRIGHT for the moused-over / targeted unit. Character models draw
// with pixel shaders, so texture-stage tricks (MODULATE2X/4X) are ignored. Instead we
// identify the highlighted model ENGINE-SIDE: the per-batch pre-draw hook (sub_70D150 in
// game_extras.cpp) checks whether the current model carries a non-zero ambient-add
// (model+0x1AC/1B0/1B4) and sets g_hlBoost accordingly. When g_hlBoost is set, our
// proxied DrawIndexedPrimitive re-draws that mesh ADDITIVELY (SRC=ONE/DEST=ONE) a few
// times on top of itself, which is output-merger blending and works regardless of the
// pixel shader = genuine overbright. CM2Scene::Draw (sub_711550) clears g_hlBoost at its
// entry and exit so the boost can't leak onto terrain/WMO/UI draws. These are defined in
// game_extras.cpp; the proxy reads them.
//
// Change 1/2: the additive re-draw is now COLORED. The arm hook (sub_70D150) captures the
// highlighted model's own ambient tint into g_hlBoostColor and picks the pass count by
// highlight type (mouseover vs target) into g_hlBoostPasses. The proxy blends each extra
// pass with SRCBLEND=BLENDFACTOR (the tint) / DESTBLEND=ONE, so it adds shaderOutput x tint
// = a strongly colored glow instead of washing toward white.
extern volatile int          g_fix75_Highlight;  // 1 = highlight overbright enabled
extern volatile float        g_hlMouseColor[3];  // mouseover tint (RGB, may exceed 1.0)
extern volatile float        g_hlMouseBright;    // mouseover additive strength (1..5 -> passes)
extern volatile float        g_hlTargetColor[3]; // target tint (RGB, may exceed 1.0)
extern volatile float        g_hlTargetBright;   // target additive strength (1..5 -> passes)
extern volatile int          g_hlBoost;          // 1 = current model batch is highlighted -> additive re-draw
extern volatile float        g_hlBoostColor[3];  // tint of the currently-armed highlighted model (from its ambient)
extern volatile int          g_hlBoostPasses;    // additive passes for the current model (by highlight type)

extern volatile int          g_recEye;    // current eye pass: 0=L, 1=R, -1 = not a recorded world pass
extern volatile int          g_recArmed;  // 1 = record the current frame's two world eye passes
extern volatile unsigned int g_recCurTex; // fix #63-diff: last stage-0 texture bound (tagged onto each draw)
extern RecEntry              g_rec[REC_CAP];
extern volatile long         g_recCount;

// -------------------------------------------------------------------------------------
// fix #78-instr: FULL-FIDELITY per-eye command-stream dump (DIAGNOSTIC ONLY).
//
// Purpose: record every D3D9 Set*/Draw* call the engine issues during the LEFT world
// pass (j==0) and the RIGHT world pass (j==1) of a single frame, with full arguments,
// into ./vr_version/cmddump.txt. This lets us verify offline whether the two eyes issue
// an IDENTICAL command stream modulo the camera matrices (SetTransform VIEW/PROJECTION +
// RT/DS binds) - de-risking a future "record-left / replay-right" single-traversal
// rewrite. It changes NO rendering behavior: capture is a pure side-channel.
//
// Arming (one-shot): OnPaint's 2s poll sees ./vr_version/cmddump_trigger.txt, deletes it
// and sets g_cmdDumpArm=1. StartRender (msub_495410) then opens a capture window right
// around each eye's world walk (sub_494F30) - AFTER the mod's own RT/viewport binds and
// BEFORE the mod's own crosshair/plate draws, so only the engine's world stream is caught.
// The window sets g_cmdDumpActive=1 (+ g_cmdDumpTid = render thread) so the proxy appends;
// it is cleared right after the walk. After the right pass, CmdDump_Finish() writes the
// summary and clears g_cmdDumpArm (self-disarm). When g_cmdDumpArm/g_cmdDumpActive are 0
// the proxy call sites are a single predicted-not-taken branch - the forward path is
// byte-for-byte identical to before.
extern volatile int           g_cmdDumpArm;     // 1 = capture this frame's L+R world passes (one-shot)
extern volatile int           g_cmdDumpActive;  // 1 = inside a world-walk capture window (proxy appends)
extern volatile int           g_cmdDumpEye;     // 0=L, 1=R while a window is open
extern volatile unsigned long g_cmdDumpTid;     // render thread id captured when the window opened

// Window control (called by msub_495410 in game_extras.cpp).
void CmdDump_BeginPass(int eye);   // open the window for eye 0/1 (right before the world walk)
void CmdDump_EndPass(int eye);     // close the window (right after the walk, before mod draws)
void CmdDump_Finish();             // after both passes: write the [cmddump] done line, disarm

// Capture call sites (called by the proxy device methods, guarded by g_cmdDumpActive).
// All use plain scalar / opaque-pointer types so this header stays dependency-free.
void CmdDump_SetTransform(unsigned int state, const void* matrix);           // 16 floats
void CmdDump_SetRenderTarget(unsigned int index, const void* surface);
void CmdDump_SetDepthStencilSurface(const void* surface);
void CmdDump_SetViewport(const void* viewport);                              // D3DVIEWPORT9*
void CmdDump_SetStreamSource(unsigned int stream, const void* vb, unsigned int offset, unsigned int stride);
void CmdDump_SetIndices(const void* ib);
void CmdDump_SetFVF(unsigned int fvf);
void CmdDump_SetVertexDeclaration(const void* decl);
void CmdDump_SetVertexShader(const void* vs);                               // also tracks "VS bound"
void CmdDump_SetPixelShader(const void* ps);
void CmdDump_SetVertexShaderConstantF(unsigned int start, const float* data, unsigned int count); // first 8 floats
void CmdDump_SetTexture(unsigned int stage, const void* tex);
void CmdDump_DrawPrimitive(unsigned int primType, unsigned int start, unsigned int primCount);
void CmdDump_DrawIndexedPrimitive(unsigned int primType, int baseVtx, unsigned int minIdx,
                                  unsigned int numVtx, unsigned int startIdx, unsigned int primCount);
void CmdDump_DrawPrimitiveUP(unsigned int primType, unsigned int primCount);
void CmdDump_DrawIndexedPrimitiveUP(unsigned int primType, unsigned int numVtx, unsigned int primCount);

// -------------------------------------------------------------------------------------
// SINGLE-PASS STEREO: record the LEFT world pass's D3D9 command stream and replay it for
// the RIGHT eye, substituting only the projection (register c2) and the right-eye render
// targets. This skips the engine's entire right-eye world walk. Behind g_singlePassStereo
// (live via vr_version/singlepass.txt; default 0 -> the whole path is dormant).
//
// Proven from a live L+R command capture: the two world streams are identical op-for-op
// except the projection uploaded via SetVertexShaderConstantF start=2 (register c2). The
// ONLY per-eye difference is the off-axis _31 term (float index 2), which is sign-negated
// for the right eye. So replay copies each recorded c2 upload and negates data[2]; a
// non-projection c2 upload carries data[2]==0, so the negation is a harmless no-op there.
//
// The proxy device (cIDirect3DDevice9.cpp) only *calls* these functions while g_spsRecording
// (or g_spsCapC2). Their bodies + the record buffer live in game_extras.cpp. Single render
// thread, so no locking. When g_spsRecording/g_spsCapC2 are 0 every proxy capture site is a
// single predicted-not-taken load and the forward path is byte-for-byte the two-pass original.
extern volatile int g_spsRecording;   // 1 = capture the current (left) world pass for replay
extern volatile int g_spsCapC2;       // 1 = capture the first c2 upload per eye (calibration frame)

// Capture entry points (invoked by the proxy device methods, guarded by g_spsRecording).
// Plain scalar / opaque-pointer types keep this header dependency-free (COM ptrs as void*).
void Sps_RecTransform(unsigned int state, const void* matrix);                 // 16 floats
void Sps_RecSetRT(unsigned int index, void* surface);
void Sps_RecSetDS(void* surface);
void Sps_RecViewport(const void* vp);                                          // D3DVIEWPORT9*
void Sps_RecStreamSrc(unsigned int stream, void* vb, unsigned int offset, unsigned int stride);
void Sps_RecIndices(void* ib);
void Sps_RecFVF(unsigned int fvf);
void Sps_RecVDecl(void* decl);
void Sps_RecVS(void* vs);
void Sps_RecPS(void* ps);
void Sps_RecVSConstF(unsigned int start, const float* data, unsigned int count);
void Sps_RecPSConstF(unsigned int start, const float* data, unsigned int count);
void Sps_RecTexture(unsigned int stage, void* tex);
void Sps_RecRS(unsigned int state, unsigned int value);
void Sps_RecSampler(unsigned int sampler, unsigned int type, unsigned int value);
void Sps_RecDrawPrim(unsigned int type, unsigned int start, unsigned int primCount);
void Sps_RecDrawIdxPrim(unsigned int type, int baseVtx, unsigned int minIdx,
                        unsigned int numVtx, unsigned int startIdx, unsigned int primCount);
void Sps_RecDrawPrimUP(unsigned int type, unsigned int primCount, const void* verts, unsigned int stride);
void Sps_RecDrawIdxPrimUP(unsigned int type, unsigned int minVtx, unsigned int numVtx,
                          unsigned int primCount, const void* indices, unsigned int idxFmt,
                          const void* vtxData, unsigned int stride);
// Calibration: capture the first c2 upload for the current eye (uses g_recEye) into the
// per-eye calibration slot, so the negate-index-2 substitution can be verified against the
// engine's own right-eye c2 on the one two-pass frame taken when single-pass is enabled.
void Sps_CaptureFirstC2(unsigned int start, const float* data, unsigned int count);

// Append one entry. Trivial guarded array write; single render thread so no locking.
inline void RecAppend(unsigned char op, unsigned int a, unsigned int b)
{
    if (g_recArmed != 1) return;
    int eye = g_recEye;
    if (eye < 0) return;
    long idx = g_recCount;
    if (idx >= REC_CAP) return;
    g_rec[idx].eye = (unsigned char)eye;
    g_rec[idx].op  = op;
    g_rec[idx].a   = a;
    g_rec[idx].b   = b;
    g_recCount = idx + 1;
}

// SetRenderState is very high-volume; only record the Z-enable + fog states we care
// about, to keep the per-frame command volume sane.
inline void RecMaybeRS(unsigned int state, unsigned int value)
{
    switch (state)
    {
    case 7:   // D3DRS_ZENABLE
    case 27:  // D3DRS_ALPHABLENDENABLE
    case 28:  // D3DRS_FOGENABLE
    case 34:  // D3DRS_FOGCOLOR
    case 35:  // D3DRS_FOGTABLEMODE
    case 36:  // D3DRS_FOGSTART
    case 37:  // D3DRS_FOGEND
    case 48:  // D3DRS_RANGEFOGENABLE
        RecAppend(4, state, value);
        break;
    default:
        break;
    }
}
