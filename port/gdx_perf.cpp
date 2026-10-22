// G-Diffuser — lightweight frame-time telemetry. See gdx_perf.h for the contract.

#include "gdx_perf.h"
#include "gdx_dev_gates.h"
#include "port_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace gdx {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kSpikeThresholdMs = 25.0; // ~1.5 dropped 60Hz frames — worth an immediate line
constexpr int kSummaryWindowFrames = 600;  // ~10 s at 60 fps

// libultraship/src/fast/Fast3dWindow.cpp calls gdx_perf_sub_begin/end with these IDs as bare
// integers because it cannot include this header. These pins turn a renumbering of GdxPerfSub
// without a matching Fast3dWindow edit into a compile error instead of a silently mislabelled
// measurement — and a mislabelled perf number is worse than none, because it gets acted on.
static_assert(GDX_PERF_SUB_GUI == 3, "Fast3dWindow.cpp GDX_PERF_SUB_GUI_ID must match");
static_assert(GDX_PERF_SUB_SFRAME == 4, "Fast3dWindow.cpp GDX_PERF_SUB_SFRAME_ID must match");
static_assert(GDX_PERF_SUB_IRUN == 5, "Fast3dWindow.cpp GDX_PERF_SUB_IRUN_ID must match");
static_assert(GDX_PERF_SUB_EFRAME == 6, "Fast3dWindow.cpp GDX_PERF_SUB_EFRAME_ID must match");

const char* const kPhaseNames[PerfPhaseCount] = {
    "events", "input", "gametick", "guistart", "dispatch", "ticks", "present", "pacer",
};

struct PerfState {
    bool enabled = false;

    Clock::time_point frameStart{};
    Clock::time_point phaseStart[PerfPhaseCount]{};
    double phaseMs[PerfPhaseCount]{};       // current frame
    double phaseAccumMs[PerfPhaseCount]{};  // summary window accumulation

    // Main-thread-only; no lock needed.
    Clock::time_point subStart[GDX_PERF_SUB_COUNT]{};
    double subMs[GDX_PERF_SUB_COUNT]{};      // current frame
    double subAccumMs[GDX_PERF_SUB_COUNT]{}; // summary window accumulation
    std::thread::id mainThread{};            // captured at PerfFrameBegin; sub timers guard against it

    std::vector<double> frameTotals; // summary window frame totals (ms)
    int spikeCount = 0;

    // Guarded by mtx: the audio thread writes at ~200 Hz.
    std::mutex mtx;
    std::vector<double> audioTicks;

    // Written from any thread; drained by the summary.
    std::atomic<uint32_t> counts[GDX_PERF_COUNT_COUNT]{};
};

// Clamped at 0 in case a seam's wall-clock straddles the phase boundary by a hair.
double subLogicMs(double gametickMs, const double* sub) {
    double logic = gametickMs - sub[GDX_PERF_SUB_XLATE] - sub[GDX_PERF_SUB_RUN] - sub[GDX_PERF_SUB_MIRROR];
    return logic > 0.0 ? logic : 0.0;
}

PerfState& state() {
    static PerfState s; // holds a mutex — must be constructed in place, never copied
    static const bool initialized = [] {
        // Reserve unconditionally (a few tens of KB) so enabling telemetry from the Dev Tools menu
        // mid-session never allocates on the frame loop's first measured frame.
        s.frameTotals.reserve(kSummaryWindowFrames);
        s.audioTicks.reserve(2048);
        return true;
    }();
    (void) initialized;
    return s;
}

double toMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// Sorts in place, so callers pass a scratch copy. Summary path only — never per frame.
double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

void emitSummary(PerfState& s) {
    std::vector<double> totals = s.frameTotals;
    std::sort(totals.begin(), totals.end());
    const double p50 = percentile(totals, 0.50);
    const double p95 = percentile(totals, 0.95);
    const double p99 = percentile(totals, 0.99);
    const double mx = totals.empty() ? 0.0 : totals.back();

    const double n = totals.empty() ? 1.0 : static_cast<double>(totals.size());
    char phases[256];
    size_t off = 0;
    for (int i = 0; i < PerfPhaseCount; ++i) {
        off += static_cast<size_t>(std::snprintf(phases + off, sizeof(phases) - off, "%s=%.2f ",
                                                 kPhaseNames[i], s.phaseAccumMs[i] / n));
        if (off >= sizeof(phases)) {
            break;
        }
    }

    double aP95 = 0.0, aMax = 0.0;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        std::vector<double> at = s.audioTicks;
        s.audioTicks.clear();
        std::sort(at.begin(), at.end());
        aP95 = percentile(at, 0.95);
        aMax = at.empty() ? 0.0 : at.back();
    }

    double subMean[GDX_PERF_SUB_COUNT];
    for (int i = 0; i < GDX_PERF_SUB_COUNT; ++i) {
        subMean[i] = s.subAccumMs[i] / n;
    }
    const double logicMean = subLogicMs(s.phaseAccumMs[PerfGameTick] / n, subMean);

    const uint32_t mapsReparse = s.counts[GDX_PERF_COUNT_MAPS_REPARSE].exchange(0, std::memory_order_relaxed);
    const uint32_t mapsUnmapped = s.counts[GDX_PERF_COUNT_MAPS_UNMAPPED].exchange(0, std::memory_order_relaxed);

    // run[...] is the breakdown INSIDE run, not extra time on top of it. Read it per TICK, not per
    // pass: each term is already summed over the M sub-frames the tick presented, which is the
    // number that has to fit in the 16.68 ms budget.
    gdx_port_logf("[GDX perf] summary frames=%zu p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms "
                  "spikes=%d | mean: %s| sub: logic=%.2f xlate=%.2f run=%.2f mirror=%.2f "
                  "| run[gui=%.2f sframe=%.2f irun=%.2f eframe=%.2f] "
                  "| gfxrun=%.2f bridge=%.2f decomp=%.2f setup=%.2f post=%.2f fbmirror=%.2f "
                  "| audio p95=%.2fms max=%.2fms | maps[reparse=%u unmapped=%u]\n",
                  totals.size(), p50, p95, p99, mx, s.spikeCount, phases, logicMean,
                  subMean[GDX_PERF_SUB_XLATE], subMean[GDX_PERF_SUB_RUN], subMean[GDX_PERF_SUB_MIRROR],
                  subMean[GDX_PERF_SUB_GUI], subMean[GDX_PERF_SUB_SFRAME], subMean[GDX_PERF_SUB_IRUN],
                  subMean[GDX_PERF_SUB_EFRAME], subMean[GDX_PERF_SUB_GFXRUN],
                  // bridge = what gdx_gfx_run spends OUTSIDE translation and the sub-frame burst;
                  // decomp = the actual game frame. Separate owners, so never one "logic" figure.
                  subMean[GDX_PERF_SUB_GFXRUN] - subMean[GDX_PERF_SUB_XLATE] - subMean[GDX_PERF_SUB_RUN],
                  (s.phaseAccumMs[PerfGameTick] / n) - subMean[GDX_PERF_SUB_GFXRUN],
                  subMean[GDX_PERF_SUB_SETUP], subMean[GDX_PERF_SUB_POST],
                  subMean[GDX_PERF_SUB_FBMIRROR], aP95, aMax, mapsReparse, mapsUnmapped);

    s.frameTotals.clear();
    s.spikeCount = 0;
    std::memset(s.phaseAccumMs, 0, sizeof(s.phaseAccumMs));
    std::memset(s.subAccumMs, 0, sizeof(s.subAccumMs));
}

} // namespace

bool PerfEnabled() {
    return state().enabled;
}

void PerfFrameBegin() {
    PerfState& s = state();
    // Re-latch the gate ONCE per frame, here, before any PerfPhaseBegin runs. Every other entry
    // point tests the latched flag, so a mid-frame toggle can never leave a Begin without its End
    // — the whole frame is measured or none of it is.
    const bool wasEnabled = s.enabled;
    s.enabled = gdx_dev_gate(GDX_GATE_PERF) != 0;
    if (s.enabled && !wasEnabled) {
        gdx_port_logf("[GDX perf] telemetry enabled (spike threshold %.0f ms, summary every %d frames)\n",
                      kSpikeThresholdMs, kSummaryWindowFrames);
        // Counts gathered while disabled would inflate the first window.
        for (std::atomic<uint32_t>& count : s.counts) {
            count.store(0, std::memory_order_relaxed);
        }
    }
    if (!s.enabled) {
        return;
    }
    s.frameStart = Clock::now();
    s.mainThread = std::this_thread::get_id();
    std::memset(s.phaseMs, 0, sizeof(s.phaseMs));
    std::memset(s.subMs, 0, sizeof(s.subMs));
}

void PerfPhaseBegin(PerfPhase p) {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    s.phaseStart[p] = Clock::now();
}

void PerfPhaseEnd(PerfPhase p) {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    s.phaseMs[p] += toMs(Clock::now() - s.phaseStart[p]);
}

void PerfFrameEnd() {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    const double total = toMs(Clock::now() - s.frameStart);
    for (int i = 0; i < PerfPhaseCount; ++i) {
        s.phaseAccumMs[i] += s.phaseMs[i];
    }
    for (int i = 0; i < GDX_PERF_SUB_COUNT; ++i) {
        s.subAccumMs[i] += s.subMs[i];
    }
    s.frameTotals.push_back(total);

    // The pacer/present phases legitimately wait for vsync, so the deliberate waits come out of
    // the spike test (but stay in the reported breakdown) — otherwise every frame is a spike.
    const double waitMs = s.phaseMs[PerfPresent] + s.phaseMs[PerfPacer];
    const double workMs = total - waitMs;
    if (workMs > kSpikeThresholdMs) {
        ++s.spikeCount;
        const double logic = subLogicMs(s.phaseMs[PerfGameTick], s.subMs);
        gdx_port_logf("[GDX perf] SPIKE work=%.1fms total=%.1fms: events=%.1f input=%.1f "
                      "gametick=%.1f guistart=%.1f dispatch=%.1f ticks=%.1f present=%.1f pacer=%.1f "
                      "| sub: logic=%.1f xlate=%.1f run=%.1f mirror=%.1f\n",
                      workMs, total, s.phaseMs[PerfEvents], s.phaseMs[PerfInput],
                      s.phaseMs[PerfGameTick], s.phaseMs[PerfGuiStart], s.phaseMs[PerfDispatch],
                      s.phaseMs[PerfTicks], s.phaseMs[PerfPresent], s.phaseMs[PerfPacer],
                      logic, s.subMs[GDX_PERF_SUB_XLATE], s.subMs[GDX_PERF_SUB_RUN],
                      s.subMs[GDX_PERF_SUB_MIRROR]);
    }

    if (s.frameTotals.size() >= static_cast<size_t>(kSummaryWindowFrames)) {
        emitSummary(s);
    }
}

void PerfAudioTick(double ms) {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.audioTicks.size() < 100000) { // hard cap; cleared every summary window
        s.audioTicks.push_back(ms);
    }
}

} // namespace gdx

// C-callable sub-phase seams (see gdx_perf.h). The main-thread guard exists because the sub timers
// are unsynchronized: the game fiber and gdx_gfx_run's seams run on the host thread, so a stray
// off-thread call is skipped rather than allowed to race them.
extern "C" void gdx_perf_sub_begin(int id) {
    gdx::PerfState& s = gdx::state();
    if (!s.enabled || id < 0 || id >= GDX_PERF_SUB_COUNT) {
        return;
    }
    if (s.mainThread != std::this_thread::get_id()) {
        return;
    }
    s.subStart[id] = gdx::Clock::now();
}

extern "C" void gdx_perf_sub_end(int id) {
    gdx::PerfState& s = gdx::state();
    if (!s.enabled || id < 0 || id >= GDX_PERF_SUB_COUNT) {
        return;
    }
    if (s.mainThread != std::this_thread::get_id()) {
        return;
    }
    s.subMs[id] += gdx::toMs(gdx::Clock::now() - s.subStart[id]);
}

extern "C" void gdx_perf_count(int id) {
    if (id < 0 || id >= GDX_PERF_COUNT_COUNT) {
        return;
    }
    gdx::state().counts[id].fetch_add(1, std::memory_order_relaxed);
}
