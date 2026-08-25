// port/gdx_interp.cpp — matrix frame-interpolation math + per-tick snap state.
// See gdx_interp.h for the architecture context. Render-only: nothing in this file writes back
// into game logic.

#include "gdx_interp.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

// Included here rather than in the header: gdx_interp.h's standalone contract keeps its interface
// free of any port/decomp/LUS dependency.
#include "port_log.h"

// =============================================================================================
// P3 cut epoch. Global scope with C linkage so the one-line PORT-gated decomp shims reach it
// without any C++/namespace surface. atomic is defensive: today every producer (decomp game
// fibers) and the single consumer (the gfx bridge) run on one cooperatively-scheduled thread, but
// a relaxed atomic costs nothing and keeps the counter well-defined if that ever changes.
// =============================================================================================
static std::atomic<std::uint32_t> g_cutEpoch{0};

static void GdxInterpMarkCutImpl(const char* tag) {
    const char* t = (tag != nullptr && tag[0] != '\0') ? tag : "decomp";
    // The bump is UNCONDITIONAL so the consumer's first tick after a toggle-on still snaps; the
    // discontinuity sites call this whether or not interpolation is enabled.
    const std::uint32_t ep = g_cutEpoch.fetch_add(1, std::memory_order_relaxed) + 1u;

    // Gate the LOG, not the bump, so normal-play logs stay clean. Rate-limited because a shim that
    // ends up in a hot path would otherwise spam 60 lines/s: first few always, whenever the tag
    // changes (distinct events stay visible), otherwise a heartbeat every 60 cuts.
    if (!gdx_interp::P1().enabled && !gdx_interp::P2HostActive()) {
        return;
    }
    static std::uint32_t sLastLoggedEpoch = 0;
    static char sLastTag[32] = {0};
    const bool firstFew = (ep <= 8u);
    const bool tagChanged = (std::strncmp(sLastTag, t, sizeof(sLastTag)) != 0);
    const bool heartbeat = (ep - sLastLoggedEpoch) >= 60u;
    if (firstFew || tagChanged || heartbeat) {
        sLastLoggedEpoch = ep;
        std::strncpy(sLastTag, t, sizeof(sLastTag) - 1);
        sLastTag[sizeof(sLastTag) - 1] = '\0';
        gdx_port_logf("[interp-p3] cut epoch=%u source=%s\n", static_cast<unsigned>(ep), t);
    }
}

extern "C" void gdx_interp_mark_cut(void) {
    GdxInterpMarkCutImpl(nullptr);
}
extern "C" void gdx_interp_mark_cut_src(const char* tag) {
    GdxInterpMarkCutImpl(tag);
}

namespace gdx_interp {

bool CutPendingForThisTick() {
    static std::uint32_t sLastSeen = 0;
    const std::uint32_t cur = g_cutEpoch.load(std::memory_order_relaxed);
    const bool changed = (cur != sLastSeen);
    sLastSeen = cur;
    return changed;
}


// =============================================================================================
// std::getenv can miss variables set after CRT init on some Windows configurations, so prefer
// GetEnvironmentVariableA there — same idiom as GdxInterpP0Enabled in n64_gfx_bridge.cpp. `buf` is
// caller-owned storage sized for the longest value this module parses; returns nullptr if the
// variable is unset (or too long to fit `buf` on Windows).
// =============================================================================================
static const char* GdxGetEnvVarWinAware(const char* name, char* buf, size_t bufSize) {
#ifdef _WIN32
    const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(bufSize));
    return (n > 0 && n < bufSize) ? buf : nullptr;
#else
    (void)buf;
    (void)bufSize;
    return std::getenv(name);
#endif
}

// =============================================================================================
// Activation surface — parse GDX_INTERP_P1 once.
// =============================================================================================
static P1Config ParseP1() {
    P1Config c{false, P1Mode::Off, 0.5f};

    char envBuf[64] = {0};
    const char* v = GdxGetEnvVarWinAware("GDX_INTERP_P1", envBuf, sizeof(envBuf));
    if (v == nullptr || v[0] == '\0' || (v[0] == '0' && v[1] == '\0')) {
        return c;
    }

    if (std::strcmp(v, "mid") == 0) {
        return P1Config{true, P1Mode::Mid, 0.5f};
    }
    if (std::strcmp(v, "half") == 0) {
        return P1Config{true, P1Mode::Half, 0.5f};
    }

    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end != v && d > 0.0 && d < 1.0) {
        float t = static_cast<float>(d);
        if (t > 0.999f) {
            t = 0.999f; // never let the presented sub-frame reach live state
        }
        return P1Config{true, P1Mode::Numeric, t};
    }

    return P1Config{true, P1Mode::Mid, 0.5f};
}

const P1Config& P1() {
    static const P1Config cfg = ParseP1();
    return cfg;
}

// =============================================================================================
// N64 Mtx <-> float. Bit-for-bit libultra guMtxL2F / guMtxF2L on 16 host int32 words.
//   words[0..7]  = integer half (s15.16 high words), words[8..15] = fraction half (low words).
// Pool matrices are host-built (decomp guMtxF2L output, host byte order), so reading the 64 bytes
// as native int32 matches the decomp's own native reads exactly.
// =============================================================================================
static constexpr float kFix32ToF = 1.0f / 65536.0f;
static constexpr float kFToFix32 = 65536.0f;

void MtxToF(const void* mtx, float mf[4][4]) {
    const int32_t* w = reinterpret_cast<const int32_t*>(mtx);
    const uint32_t* intw = reinterpret_cast<const uint32_t*>(w);
    const uint32_t* fracw = reinterpret_cast<const uint32_t*>(w + 8);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 2; j++) {
            const int idx = i * 2 + j;
            const uint32_t A = intw[idx];
            const uint32_t F = fracw[idx];
            const uint32_t e1 = (A & 0xFFFF0000u) | ((F >> 16) & 0xFFFFu);
            const uint32_t e2 = ((A << 16) & 0xFFFF0000u) | (F & 0xFFFFu);
            mf[i][j * 2]     = static_cast<float>(static_cast<int32_t>(e1)) * kFix32ToF;
            mf[i][j * 2 + 1] = static_cast<float>(static_cast<int32_t>(e2)) * kFix32ToF;
        }
    }
}

void MtxFromF(const float mf[4][4], void* mtx) {
    uint32_t* intw = reinterpret_cast<uint32_t*>(mtx);
    uint32_t* fracw = reinterpret_cast<uint32_t*>(intw + 8);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 2; j++) {
            const int idx = i * 2 + j;
            const int32_t e1 = static_cast<int32_t>(mf[i][j * 2] * kFToFix32);
            const int32_t e2 = static_cast<int32_t>(mf[i][j * 2 + 1] * kFToFix32);
            intw[idx]  = (static_cast<uint32_t>(e1) & 0xFFFF0000u) |
                         ((static_cast<uint32_t>(e2) >> 16) & 0xFFFFu);
            fracw[idx] = ((static_cast<uint32_t>(e1) << 16) & 0xFFFF0000u) |
                         (static_cast<uint32_t>(e2) & 0xFFFFu);
        }
    }
}

// =============================================================================================
// Per-element float lerp — SoH interpolate_mtxf.
// =============================================================================================
void LerpMtx(const void* prev, const void* cur, float t, void* out) {
    float pf[4][4];
    float cf[4][4];
    float of[4][4];
    MtxToF(prev, pf);
    MtxToF(cur, cf);
    const float w = 1.0f - t;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            of[i][j] = w * pf[i][j] + t * cf[i][j];
        }
    }
    MtxFromF(of, out);
}

// =============================================================================================
// Translation-magnitude teleport snap (belt-and-suspenders).
// Threshold in camera-space units per tick. Authoritative cut coverage is P3's
// gdx_interp_mark_cut(); this catches gross jumps that no event shim has been wired for yet.
//
// 300 is measured, not guessed. Slot identity is a GfxPool byte offset (n64_gfx_bridge.cpp
// GdxP0RerouteMtx), so when the pool layout shifts -- the visible set changes as the camera turns
// and the track-chunk cull admits different geometry -- offset N pairs against a DIFFERENT object
// than last tick, and the lerp runs between two unrelated transforms. That is the floor flicker,
// and it is why the flicker tracks camera angle. The [interp-pair] probe measured the two
// populations in a Mute City race; they separate cleanly, with nothing in between:
//   correctly paired windows : per-tick max 0-203 units
//   mispairing bursts        : per-tick max 495, 610, 762, 1206, 1740  (223 events / 41893 pairings)
// 300 sits above the clean population with margin and below the lowest observed mispairing, and
// still leaves large headroom over real motion: a machine at 3000 km/h -- the game's speed
// ceiling -- covers roughly 140 units per tick. Erring toward snapping is the right asymmetry: a
// snapped slot costs ONE imperceptible tick of non-interpolation, a mispaired slot is a visible
// flicker.
//
// This is a MITIGATION, not the architectural fix -- byte-offset identity is still the wrong key,
// and a mispairing between two objects closer than 300 units apart is still silent. The real fix
// is a stable per-object key (Starship records the destination Mtx* instead).
const float kTeleportThreshold = 300.0f;

float TranslationDelta(const void* prev, const void* cur) {
    float pf[4][4];
    float cf[4][4];
    MtxToF(prev, pf);
    MtxToF(cur, cf);
    // Translation lives in row 3 (guTranslateF writes mf[3][0..2]).
    const float dx = cf[3][0] - pf[3][0];
    const float dy = cf[3][1] - pf[3][1];
    const float dz = cf[3][2] - pf[3][2];
    return sqrtf((dx * dx) + (dy * dy) + (dz * dz));
}

bool TranslationTeleport(const void* prev, const void* cur) {
    return TranslationDelta(prev, cur) > kTeleportThreshold;
}

// =============================================================================================
// Rotation-consistency mispair guard. cos(60 deg): correctly paired slots rotate far under
// 15 deg/tick (fastest spin attack), unrelated objects differ by an arbitrary angle, so the
// populations separate as widely as the translation ones do around kTeleportThreshold.
// Only rows 0/1 are compared -- row 2 is derived and adds no information for rigid bases, and
// degenerate (zero-length) rows skip rather than snap: a degenerate basis is not evidence of
// mispairing, and snapping every such slot would freeze legit effects geometry.
const float kRotationTeleportCosine = 0.5f;

bool RotationTeleport(const void* prev, const void* cur) {
    float pf[4][4];
    float cf[4][4];
    MtxToF(prev, pf);
    MtxToF(cur, cf);
    for (int r = 0; r < 2; r++) {
        const float px = pf[r][0], py = pf[r][1], pz = pf[r][2];
        const float cx = cf[r][0], cy = cf[r][1], cz = cf[r][2];
        const float plen = sqrtf((px * px) + (py * py) + (pz * pz));
        const float clen = sqrtf((cx * cx) + (cy * cy) + (cz * cz));
        if (plen <= 0.0f || clen <= 0.0f) {
            continue;
        }
        const float dot = ((px * cx) + (py * cy) + (pz * cz)) / (plen * clen);
        if (dot < kRotationTeleportCosine) {
            return true;
        }
    }
    return false;
}

// =============================================================================================
// Referenced-set tracking. Graphics-thread only; no locking.
// =============================================================================================
static std::unordered_set<uint32_t>& CurSet() {
    static std::unordered_set<uint32_t> s;
    return s;
}
static std::unordered_set<uint32_t>& PrevSet() {
    static std::unordered_set<uint32_t> s;
    return s;
}

void BeginTick() {
    CurSet().clear();
}

bool NoteReferencedOffset(uint32_t offset) {
    const bool wasPresent = (PrevSet().find(offset) != PrevSet().end());
    CurSet().insert(offset);
    return wasPresent;
}

void CommitTick() {
    PrevSet().swap(CurSet());
}

// =============================================================================================
// Dual-pool resolution. All GfxPool-layout knowledge lives here.
// =============================================================================================
extern "C" {
extern unsigned char D_8024DCE0[]; // decomp: GfxPool D_8024DCE0[2]; addressed as raw bytes here
extern int D_800DCCFC;             // decomp: s32 double-buffer parity toggle
}

// The N64 struct-comment GfxPool size from decomp/include/sys.h, selected on EXPANSION_KIT. This
// is NOT the host stride: on a 64-bit host sizeof(Gfx) doubles (pointer-width w1), inflating
// gfxBuffer[13313] by 0x1A008, so the real pool is 0x50738 rather than 0x36730. Kept only as the
// reference value the one-time stride log reports against.
#ifdef EXPANSION_KIT
static constexpr size_t kGfxPoolSize = 0x36730;
#else
static constexpr size_t kGfxPoolSize = 0x2C6F0;
#endif

// port/decomp_port.c compiles WITH the real GfxPool type, so this is the authoritative stride.
// Using the N64 constant instead put every modelview matrix outside GdxP0MtxInPoolSpan's bound:
// PrevPoolBase returned 0 forever, every slot snapped to t=1, and the sub-frame loop paid full
// cost while rendering the disabled path.
extern "C" size_t gdx_gfxpool_sizeof(void);

// Latched once. Logs the N64-vs-host delta the first time so the discrepancy stays visible without
// disabling the feature. 0 only if the ground-truth query fails, which PrevPoolBase treats as
// "no lerp".
static size_t GfxPoolStride() {
    static const size_t stride = [] {
        const size_t real = gdx_gfxpool_sizeof();
        if (real != kGfxPoolSize) {
            gdx_port_logf("[interp] GfxPool host stride 0x%zX (N64 struct-comment size 0x%X) — "
                          "using host sizeof for dual-pool lerp\n",
                          real, static_cast<unsigned>(kGfxPoolSize));
        }
        return real;
    }();
    return stride;
}

uintptr_t PrevPoolBase(uintptr_t curPoolBase) {
    const size_t stride = GfxPoolStride();
    if (stride == 0) {
        return 0; // ground-truth query failed: degrade safely (every slot snaps to cur)
    }
    const int parity = D_800DCCFC & 1;
    const uintptr_t arrayBase = reinterpret_cast<uintptr_t>(&D_8024DCE0[0]);
    const uintptr_t expectedCur = arrayBase + static_cast<uintptr_t>(parity) * stride;
    if (curPoolBase != expectedCur) {
        // gSegments[1] does not match the parity-selected pool: the layout assumption is off, or
        // the pool moved. Disable the dual-pool lerp for this tick.
        return 0;
    }
    return arrayBase + static_cast<uintptr_t>(parity ^ 1) * stride;
}

int PoolParity() {
    return D_800DCCFC & 1;
}

// =============================================================================================
// P2 activation. CVar bridge declared locally (same minimal-include idiom as
// port/gdx_frame_pacer.c and port/input_bridge.c) so this standalone TU does not pull the LUS
// C++ console-variable header. CVarGetInteger is linked from libultraship in the same target.
// =============================================================================================
extern "C" int CVarGetInteger(const char* name, int defaultValue);

// Env overrides are test hooks: cached for the process lifetime so they are never re-read per call
// and never written back to the user's config.
static bool P2EnvOverride() {
    static const bool on = [] {
        char envBuf[8] = {0}; // only v[0]/v[1] are inspected -- same idiom as GDX_INTERP_P0's buf
        const char* v = GdxGetEnvVarWinAware("GDX_INTERP_P2", envBuf, sizeof(envBuf));
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

bool P2HostActive() {
    if (P2EnvOverride()) {
        return true;
    }
    // Live read so the menu toggle applies on the next tick, exactly like FramePacing.
    return CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) != 0;
}

static bool CameraEnvOverride() {
    static const bool on = [] {
        char envBuf[8] = {0};
        const char* v = GdxGetEnvVarWinAware("GDX_INTERP_CAMERA", envBuf, sizeof(envBuf));
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

bool CameraInterpActive() {
    if (CameraEnvOverride()) {
        return true;
    }
    // Live read so the menu toggle applies on the next tick. Default 1: see gdx_interp.h for why
    // this one ships on while FrameInterpolation itself defaults off.
    return CVarGetInteger("gEnhancements.Graphics.InterpolateCamera", 1) != 0;
}

static bool RigidBasisEnvOverride() {
    static const bool on = [] {
        char envBuf[8] = {0};
        const char* v = GdxGetEnvVarWinAware("GDX_INTERP_ROT_FIX", envBuf, sizeof(envBuf));
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

bool RigidBasisActive() {
    if (RigidBasisEnvOverride()) {
        return true;
    }
    return CVarGetInteger("gEnhancements.Graphics.InterpRigidBasis", 0) != 0;
}

static bool BasisJumpEnvOverride() {
    static const bool on = [] {
        char envBuf[8] = {0};
        const char* v = GdxGetEnvVarWinAware("GDX_INTERP_BASIS_FIX", envBuf, sizeof(envBuf));
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

bool BasisJumpFixActive() {
    if (BasisJumpEnvOverride()) {
        return true;
    }
    return CVarGetInteger("gEnhancements.Graphics.InterpBasisJump", 0) != 0;
}

static bool RotationSnapEnvOverride() {
    static const bool on = [] {
        char envBuf[8] = {0};
        const char* v = GdxGetEnvVarWinAware("GDX_INTERP_ROT_SNAP", envBuf, sizeof(envBuf));
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

bool RotationSnapActive() {
    if (RotationSnapEnvOverride()) {
        return true;
    }
    return CVarGetInteger("gEnhancements.Graphics.InterpRotSnap", 0) != 0;
}

} // namespace gdx_interp
