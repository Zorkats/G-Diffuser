// port/gdx_interp.h — matrix frame-interpolation math + per-tick snap state.
//
// A GfxPool double-buffer lerp driven by port/n64_gfx_bridge.cpp's G_MTX scratch-slot
// indirection: the bridge reroutes every pool-span modelview matrix into a stable scratch slot
// and refills it with LerpMtx(prev, cur, t) before each sub-frame replay.
//
// PRIME DIRECTIVE: interpolation is render-only. Nothing here ever writes back into game logic.
//
// No-op unless GDX_INTERP_P1 is set. Standalone C++ (no decomp/LUS headers in the interface) so
// it links into the G-Diffuser executable beside the bridge.

#ifndef GDX_INTERP_H
#define GDX_INTERP_H

#include <cstddef>
#include <cstdint>

// --- P3: discontinuity cut epoch ------------------------------------------------------------
// A hard discontinuity — race start/restart, respawn, retire/knockout, a camera hard-cut, or a
// mode/screen transition — makes the PREVIOUS keyframe meaningless for EVERY on-screen entity, so
// the whole frame must SNAP (t=1) for one tick instead of streaking from stale poses. Every such
// site bumps a single process-global epoch:
//   * gdx_interp_mark_cut(void)    — C linkage with NO string dependency, so the PORT-gated
//                                    decomp shims (racer.c, camera.c, ...) stay true one-liners.
//   * gdx_interp_mark_cut_src(tag) — for port-side hooks that carry a label.
// The bridge consumes the epoch once per rendered tick (gdx_interp::CutPendingForThisTick) and, on
// a changed epoch, forces every pool matrix that tick to snap.
extern "C" void gdx_interp_mark_cut(void);
extern "C" void gdx_interp_mark_cut_src(const char* tag);

namespace gdx_interp {

// --- Activation surface ----------------------------------------------------------------------
// P1 is the in-bridge, env-gated diagnostic mode (GDX_INTERP_P1), separate from the shipping
// user-facing toggle gEnhancements.Graphics.FrameInterpolation that drives P2's host-side
// sub-frame loop (see P2HostActive below). Values:
//   unset / "0"      -> Off (bit-exact stock path)
//   "mid" / "half"   -> presented (2nd) pass renders the t=0.5 midpoint frame
//   a float in (0,1) -> presented pass renders at that fixed t
enum class P1Mode { Off, Mid, Half, Numeric };

struct P1Config {
    bool enabled;   // any P1 mode active (GDX_INTERP_P1 set to a non-"0" value)
    P1Mode mode;
    float presentT; // t used for the presented (2nd) replay pass; 0.5 for Mid/Half
};

// Read GDX_INTERP_P1 exactly once; cached for the process lifetime.
const P1Config& P1();

// --- N64 Mtx <-> float --------------------------------------------------------------------
// Pool matrices are HOST-built (decomp guMtxF2L output: little-endian s15.16, hi/lo split — the
// bridge only reroutes the !isBig pool-span path). These reproduce libultra guMtxL2F / guMtxF2L
// bit-for-bit on the raw 16 host int32 words, so a round-trip is exact and a t=1 lerp is
// byte-transparent (P0 invariant). `mtx` points at 64 readable bytes.
void MtxToF(const void* mtx, float mf[4][4]);
void MtxFromF(const float mf[4][4], void* mtx);

// --- Per-element float lerp (matrix lerp, NOT slerp) ---------------------------------------
// Mirrors SoH interpolate_mtxf: res[i][j] = w*prev[i][j] + t*cur[i][j], w = 1-t. Decomposed-input
// interpolation was rejected — it caused rolling artifacts in SoH. Inter-tick rotation deltas at
// 60 Hz are tiny, and hard cuts are handled by the snap rules (referenced-set + teleport) below
// and by P3's gdx_interp_mark_cut(), not by better math. At t=1 the output equals `cur`
// byte-for-byte. prev/cur/out each point at 64 bytes; out may alias neither prev nor cur.
void LerpMtx(const void* prev, const void* cur, float t, void* out);

// --- Snap rule: translation-magnitude teleport (belt-and-suspenders) ----------------------
// True if the camera-space translation delta between prev and cur exceeds kTeleportThreshold.
// A coarse guard for teleports/cuts of entities present in BOTH ticks that P3's event shims do
// not cover yet; normal per-tick motion is far below the threshold.
extern const float kTeleportThreshold;
bool TranslationTeleport(const void* prev, const void* cur);

// --- Snap rule: rotation-consistency mispair guard ------------------------------------------
// True when the basis ROTATION between prev and cur differs by more than ~60 degrees. The
// translation teleport above cannot catch a mispairing between two objects that sit close
// together -- adjacent track chunks pass the 300-unit guard silently while their orientations
// are unrelated. Comparing normalized basis rows closes that hole: legit per-tick rotation is
// well under 15 deg/tick even at the fastest spin attack, while a mispaired pair differs by an
// arbitrary angle. Like the translation guard this is a mitigation, not the stable-key fix.
bool RotationTeleport(const void* prev, const void* cur);

// Gate for RotationTeleport at the reroute site. Env pin GDX_INTERP_ROT_SNAP forces it on for
// an A/B; the CVar carries the shipping default. Default OFF until owner-validated per the
// perfection plan's Slice-3 data gate; flip the CVar default once confirmed in a race.
bool RotationSnapActive();

// --- Pairing-quality measurement ------------------------------------------------------------
// Magnitude of the prev->cur translation delta for a slot that PAIRED (offset referenced in both
// ticks, under the teleport threshold) — the number that separates a correct pairing from a silent
// mispairing.
//
// Slot identity is the GfxPool BYTE OFFSET (n64_gfx_bridge.cpp GdxP0RerouteMtx:
// offset = origPtr - mCurPoolBase, then prevPtr = mPrevPoolBase + offset), which is only valid
// while the pool layout is stable frame to frame. The pool fills in draw-submission order, so when
// the camera turns and the visible set changes (track-chunk cull, objects entering/leaving), offset
// N holds a DIFFERENT logical object than last tick and the lerp runs between two unrelated
// transforms. kTeleportThreshold cannot reliably catch that: adjacent track chunks sit far closer
// together than any teleport, so a mispaired floor chunk can pass the guard silently.
//
// Reported per tick so a camera sweep can be compared against a straight: if the delta population
// grows a fat tail exactly when the view rotates, byte-offset identity is the defect and the fix is
// a stable key (Starship keys on the destination Mtx* instead).
float TranslationDelta(const void* prev, const void* cur);

// --- Snap rule: referenced-set tracking (spawn/despawn) ------------------------------------
// The port cannot observe which pool slots game code wrote without instrumenting the decomp, so
// it tracks the set of pool OFFSETS referenced by each tick's display list instead. A slot whose
// offset was NOT referenced last tick has a stale/absent previous keyframe -> snap (t=1) for that
// slot only, which covers spawn/despawn purely port-side.
//
// Lifecycle runs ONCE PER RENDERED TICK -- which is NOT once per gdx_gfx_run. The game submits 2-6
// GFX tasks per 60 Hz tick and gdx_gfx_run executes per task, so bracketing these calls inside it
// compared each task's offsets against the PREVIOUS TASK's set rather than the previous tick's:
// slots that should lerp snapped, and a fully-snapped task tripped the sub-frame loop's
// `degenerate` check into rendering every pass at t=1. Both calls are driven from the real tick
// boundary (gGdxInterpNewTick in n64_gfx_bridge.cpp, armed by gdx_gfx_interp_tick_config).
//   BeginTick()  -> clears the current-tick offset set.
//   NoteReferencedOffset(off) -> records `off` for this tick; returns true iff it was in the
//                                PREVIOUS tick's set (i.e. a usable prev keyframe exists).
//                                Called by EVERY task in the tick, accumulating into one set.
//   CommitTick() -> promotes the current set to "previous". Invoked at the START of the next tick,
//                   immediately before BeginTick, so nothing has to identify the tick's last task.
void BeginTick();
bool NoteReferencedOffset(uint32_t offset);
void CommitTick();

// --- P3 cut consume ---------------------------------------------------------------------------
// True iff a cut fired since the PREVIOUS call — a consume-once edge, so WHO calls it and HOW
// OFTEN is load-bearing. Called exactly once per rendered tick, by the tick's FIRST gfx task, and
// latched for every later task in that tick (gGdxInterpCutThisTick in n64_gfx_bridge.cpp). Calling
// it per task instead let the first task eat the cut while the rest saw false, so half a frame
// snapped and the other half lerped across the cut. Cut sites fire during the game-logic half of a
// dispatch, which precedes that same dispatch's gfx-task submission (where BeginTick runs), so a
// cut is observed on the tick it happens.
bool CutPendingForThisTick();

// --- Dual-pool resolution ------------------------------------------------------------------
// The two GfxPools are D_8024DCE0[2]; the current one is selected by D_800DCCFC parity and its
// host base is what Segment_SetPhysicalAddress(1, gGfxPool) stored in gSegments[1]. The previous
// tick's copy of the same matrix lives at the SAME offset in the sibling pool. Returns the sibling
// base, or 0 if the passed base does not match the parity-selected pool — the caller then skips
// the lerp for this tick. All decomp GfxPool-layout knowledge lives here.
uintptr_t PrevPoolBase(uintptr_t curPoolBase);

// --- P4: pool-quiescence guard ---------------------------------------------------------------
// The raw GfxPool double-buffer parity (D_800DCCFC & 1). The toggle (D_800DCCFC ^= 1) happens
// ONLY in the NEXT tick's Gfx_InitBuffer, which runs inside gdx_dispatch and therefore CANNOT run
// during the bridge's sub-frame present loop (that loop wraps StartFrame/EndFrame only). The
// bridge latches this before the loop and re-checks it after: an unchanged value confirms both
// pools stayed quiescent across the whole replay window.
int PoolParity();

// --- P2 activation: host-driven main-loop render/logic decoupling -------------------------
// True when the decoupled sub-frame present loop is active this process. Two sources:
//   * the integer CVar gEnhancements.Graphics.FrameInterpolation != 0, read LIVE so the menu
//     toggle takes effect between ticks (same idiom as gEnhancements.Graphics.FramePacing), OR
//   * the env override GDX_INTERP_P2 set to a non-"0" value — a test hook, cached for the process.
// When active, port/n64_gfx_bridge.cpp reuses the P1 dual-pool lerp machinery (P2 is OR-ed into
// the adapter's P1-enable) and port/main.cpp drives M sub-frame presents per 60 Hz logic tick.
bool P2HostActive();

// --- Camera/projection interpolation ------------------------------------------------------
// True when G_MTX_PROJECTION pool matrices should ALSO be rerouted through the interpolation
// scratch. Precedence: the env override GDX_INTERP_CAMERA (non-"0", cached for the process), then
// the integer CVar gEnhancements.Graphics.InterpolateCamera (read live). The Bucket B dev gate of
// the same name is a dev-build force-on and is OR-ed in by the bridge.
//
// ON BY DEFAULT, unlike the other interpolation switches: race.c loads the COMBINED
// projection*view camera with G_MTX_PROJECTION (race.c:250, and earlier at background.c:1083), and
// course.c emits no gSPMatrix at all -- course geometry is world-space vertices viewed through that
// camera. Excluding projection therefore froze BOTH the camera and the entire track at 60 Hz,
// leaving racer models smoothed against a static world.
//
// Element-wise lerp of the combined matrix is the FALLBACK path, and it is NOT exact. Only the
// projection factors out: P is identical across a tick pair and Camera_CalculateProjectionViewMtx
// (camera.c:949) is linear in V, so lerp(P*V0, P*V1) = P*lerp(V0,V1). The view matrix is what does
// not survive. In this codebase's row-vector convention V = [R ; t] with t = -eye*R --
// Matrix_SetLookAt writes m[3][j] as -eye dotted with column j of R, never the eye itself
// (math.c:1018, :1032, :1046) -- so row 3 is the eye ALREADY ROTATED BY R. Lerping it blends a
// product whose two factors BOTH move across the tick, and the implied camera position
//     eye_implied(t) = -lerp(t0,t1) * lerp(R0,R1)^T   !=   lerp(eye0, eye1)
// bows off the straight line between the two true eye positions by about
// (delta-rotation x |eye|). `eye` is a raw world coordinate thousands of units from the origin, so
// the rotation-blend error is multiplied by a multi-thousand-unit lever arm into a WORLD
// TRANSLATION error: zero at t=1, largest mid-tick, so the whole scene slides off and snaps back
// at every tick boundary.
//
// This is NOT the approximation the racer modelview matrices already accept. A model matrix's
// lever arm is object-local, so the identical rotation blend only slightly mis-orients a small
// object; on the camera the same expression is scaled by the distance to the world origin. Same
// math, categorically different magnitude -- equating the two is the error. It shows as an
// afterimage of the player machine during a SIDE attack and not during a spin attack:
// driftAttackForce is 5.0f for side (racer.c:3831) against 0.5f for spin (racer.c:3860), a 10x
// harder camera swing and so a 10x bow. Turning this switch off removes the ghost but re-freezes
// camera and track, which is why the fallback still ships rather than the default flipping to off.
//
// The correct path does not lerp the matrix at all. Snapshot the camera POSE per tick
// (eye/at/up/fov/near/far/fovScale/frustrumCenter), interpolate the pose, and rebuild through the
// stock chain -- Matrix_SetFrustrum -> Matrix_SetLookAt -> Camera_CalculateProjectionViewMtx
// (camera.c:949) -> Camera_MatrixToMtx (camera.c:976) -- which is exactly what
// Camera_UpdateProjectionViewMtx (camera.c:1206, sole writer of GfxPool::unk_20208) runs. Because
// it is the same chain, a rebuild at t=1 MUST come out bit-identical to the pool matrix; that
// equality is the gate, and a rebuild that fails it drops back to the element-wise lerp.
bool CameraInterpActive();

// --- Racer-basis interpolation fixes ------------------------------------------------------
// Same env-pin / CVar-default split as CameraInterpActive: the env var forces ON for a rebuild-free
// A/B, the CVar carries the shipping default (registered in gdx_menu.cpp, read live so a console
// toggle applies on the next tick). Resolved here rather than in the bridge because CVarGetInteger
// links in this TU -- the bridge's earlier getenv-only versions of these gates were unreachable by
// users.
//
// RigidBasisActive: rescale element-wise-lerped per-racer basis rows back to rigid length.
// Measured without it: rowlen 0.0818 at t=0.5 against exactly 0.10000 at t=1 -- an 18% mid-tick
// collapse that squashes and shears the model for a sub-frame.
// Env pin GDX_INTERP_ROT_FIX; CVar gEnhancements.Graphics.InterpRigidBasis.
bool RigidBasisActive();
// BasisJumpFixActive: freeze the rotation of a per-racer matrix's previous keyframe across a side
// attack's two model-basis discontinuities (racer.c:4556), so the lerp does not smear the jump.
// Env pin GDX_INTERP_BASIS_FIX; CVar gEnhancements.Graphics.InterpBasisJump.
bool BasisJumpFixActive();

} // namespace gdx_interp

#endif // GDX_INTERP_H
