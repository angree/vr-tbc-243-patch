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
