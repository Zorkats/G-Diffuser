// G-Diffuser — lightweight frame-time telemetry (GDX_PERF=1).
//
// Turns "I feel slowdowns sometimes" into attributed numbers from a normal play session. Emits an
// immediate "[GDX perf] SPIKE" line with THAT frame's per-phase breakdown whenever a frame exceeds
// the threshold, plus a "[GDX perf] summary" line every ~10 s with frame-time p50/p95/p99/max,
// mean per-phase milliseconds, and the audio thread's tick p95/max over the same window.
//
// A Bucket A developer gate (port/gdx_dev_gates.h), so it is also togglable live from
// F1 > Dev Tools > Developer gates > Scheduling. The flag is re-latched from the gate once per
// frame in PerfFrameBegin, never mid-frame, so a toggle can never leave a phase Begin without its
// End.
//
// The lines only reach gdiffuser-run.log if the file sink is open — enable "Write gdiffuser-run.log"
// (or GDX_LOG=1 / GDX_TRACE=1) alongside it.
#pragma once

// ---------------------------------------------------------------------------------------------
// Second-level (sub-phase) breakdown of the game frame — C-callable so C translation units
// (n64_vi.c / n64_sched.c) and the C++ gfx bridge can share the same seams.
//
// The game's fibers do NOT run inside gdx_dispatch(): posting the VI retrace message in
// gdx_vi_tick() (main.cpp's "gametick" phase) wakes the Main scheduler thread and the cooperative
// scheduler dispatches the game fiber RIGHT THERE (osSendMesg -> osStartThread ->
// __osDispatchThread when __osRunningThread == NULL). The whole game frame — game logic AND the
// synchronous gfx-task submission — therefore executes inside the "gametick" phase, which is why
// these sub-timers exist at all.
//
// "logic" is not a seam; it is derived as gametick - (xlate + run + mirror). All three seams live
// on the host/main thread, so the timers need no lock; a main-thread guard skips any stray
// off-thread call rather than corrupt state.
#ifdef __cplusplus
extern "C" {
#endif

enum GdxPerfSub {
    GDX_PERF_SUB_XLATE = 0, // DL translation (N64DisplayListAdapter::ConvertRoot in gdx_gfx_run)
    GDX_PERF_SUB_RUN,       // the whole DrawAndRunGraphicsCommands call, summed over all sub-frames
    GDX_PERF_SUB_MIRROR,    // GdxUpdateFrameMirror (persistent frame-mirror refresh)
    // Breakdown INSIDE run (Fast3dWindow::DrawAndRunGraphicsCommands): nested within RUN, not
    // additional to it (gui + sframe + irun + eframe ~= run). They separate the three reasons a
    // sub-frame pass can be expensive -- re-rendering the game, rebuilding the whole ImGui frame,
    // or blocking on the present -- which have completely different fixes.
    // Deliberately placed AFTER MIRROR: `logic` is derived by subtracting XLATE + RUN + MIRROR by
    // explicit index, so appending here cannot corrupt it.
    GDX_PERF_SUB_GUI,    // Gui::StartDraw + Gui::EndDraw (a complete ImGui frame, per pass)
    GDX_PERF_SUB_SFRAME, // Interpreter::StartFrame (framebuffer/aspect setup, per pass)
    GDX_PERF_SUB_IRUN,   // Interpreter::Run (the display-list re-execution itself, per pass)
    GDX_PERF_SUB_EFRAME, // Interpreter::EndFrame (SwapBuffers; includes any vsync/latency block)
    // The WHOLE of gdx_gfx_run, so "logic" stops absorbing bridge work that is not game logic:
    // only ConvertRoot was timed (as xlate), so the texture-cache drain, wide-cache sweep, RGBA16
    // range clears, persistent-allocation reset and frame-mirror refresh all got reported as decomp
    // time. With this seam, bridge overhead = gfxrun - xlate - run and real decomp logic =
    // gametick - gfxrun. Nested (it contains xlate and run), so it is excluded from the logic
    // subtraction.
    GDX_PERF_SUB_GFXRUN,
    // Halves of the bridge overhead. setup is everything before translation begins (segment
    // binding, cache sweeps, endianness probe, texture-cache drain); post is everything after the
    // sub-frame burst (frame flags, RGBA16 range clears, persistent-allocation reset, mirror,
    // diagnostics). Both nested inside gfxrun.
    GDX_PERF_SUB_SETUP,
    GDX_PERF_SUB_POST,
    // The end-of-task framebuffer mirror loop, which copies every N64 framebuffer targeted as CIMG
    // anywhere in the task -- distinct from GDX_PERF_SUB_MIRROR, which times only
    // GdxUpdateFrameMirror.
    GDX_PERF_SUB_FBMIRROR,
    GDX_PERF_SUB_COUNT
};

void gdx_perf_sub_begin(int id);
void gdx_perf_sub_end(int id);

// Event counters, reported as totals per summary window. Thread-safe and cheap enough to call
// whether or not telemetry is enabled; counts from before enabling are discarded.
enum GdxPerfCount {
    GDX_PERF_COUNT_MAPS_REPARSE = 0, // POSIX memory-map snapshot parses (a full /proc/self/maps read)
    GDX_PERF_COUNT_MAPS_UNMAPPED,    // snapshot misses answered as unmapped without a re-parse
    GDX_PERF_COUNT_COUNT
};

void gdx_perf_count(int id);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
namespace gdx {

enum PerfPhase {
    PerfEvents = 0, // HandleEvents (SDL pump, drop events, window messages)
    PerfInput,      // controller poll + aspect tick + audio notify + mouse/nav (cheap, no game work)
    PerfGameTick,   // gdx_vi_tick: posts retrace -> runs the Main game fiber (game logic + gfx submit)
    PerfGuiStart,   // Gui::StartDraw + Window::StartFrame + deferred wake drain
    PerfDispatch,   // gdx_dispatch: residual fibers only (normally ~0; the frame ran in gametick)
    PerfTicks,      // savestate tick + disk-save tick
    PerfPresent,    // vi present fallback + Gui::EndDraw + Window::EndFrame (includes vsync wait)
    PerfPacer,      // optional frame pacer sleep
    PerfPhaseCount
};

bool PerfEnabled();

void PerfFrameBegin();
void PerfPhaseBegin(PerfPhase p);
void PerfPhaseEnd(PerfPhase p);
void PerfFrameEnd();

// Called from the dedicated audio thread with one tick's duration. Thread-safe.
void PerfAudioTick(double ms);

} // namespace gdx
#endif // __cplusplus
