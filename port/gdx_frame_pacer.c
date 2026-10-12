// gdx_frame_pacer.c -- wall-clock frame pacer for the host loop.
//
// WHY THE DEFAULT IS PLATFORM-SPECIFIC
// ------------------------------------
// The host loop advances the simulation by exactly one VI tick per iteration and its cadence
// comes from w->EndFrame() -> Interpreter::EndFrame() -> GfxWindowBackend*::SwapBuffersBegin().
// Both shipped backends contain a software limiter targeting mTargetFps (60), but they are NOT
// equally reliable (the default is set in port/main.cpp via CVarRegisterInteger before the menu
// registers this CVar, so a persisted user toggle still wins):
//
//   - Windows / DXGI-DX11 : gfx_dxgi.cpp SwapBuffersBegin() -- SetWaitableTimer coarse sleep to
//       ~1.5 ms before a 1e9/60 ns deadline, then a YieldProcessor spin, then Present(vsync).
//       Holds 60 fps reliably with VSync on or off, so this pacer stays OFF on Windows.
//
//   - Linux / SDL2-GL : gfx_sdl2.cpp SyncFramerateWithTime() sleeps with a *relative*
//       nanosleep(left) that has no busy-wait backstop and is NOT retried on EINTR. A single
//       signal makes it return early having slept only part of `left`; when that happens every
//       frame the loop free-runs at whatever the panel / VSync provides -- 144 Hz on the ROG
//       Ally, i.e. ~2.4x game speed. This pacer is therefore ON by default there: its absolute-
//       deadline, EINTR-retrying wait holds the true 59.94 Hz field rate regardless.
//
// WHEN ENABLED
// ------------
// Targets the true N64 NTSC field rate (60 / 1.001 ~= 59.94 Hz), which is marginally slower than
// the built-in 60.00 Hz limiter, so THIS pacer becomes the binding constraint and the built-in one
// turns into a no-op for the frame. The two do not fight; the slower schedule simply wins.
//
// VSYNC INTERACTION -- keep VSync OFF when this is ON. With VSync on, Present() also blocks on the
// display refresh, rounding each present onto the refresh grid (2.4 refreshes per frame at 144 Hz)
// and beating against this fixed 59.94 Hz schedule. That judder is a property of VSync at a
// non-integer-multiple refresh and exists for 60 fps content regardless of this module; a
// wall-clock pacer on top only makes it more visible.
//
// timeBeginPeriod() is deliberately NOT used, so no winmm link dependency is added; the
// high-resolution waitable timer plus the spin backstop already hit the deadline accurately,
// matching libultraship's own approach.

#include "gdx_frame_pacer.h"

#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h> // clock_gettime / clock_nanosleep (POSIX monotonic pacer backend)
#endif

#include "port_log.h"

// Declared locally -- the same minimal-include boundary idiom port/input_bridge.c uses -- so this
// C TU does not pull libultraship's C++ console-variable header.
extern int CVarGetInteger(const char* name, int defaultValue);

// Per-tick truth from port/n64_gfx_bridge.cpp, NOT the raw
// gEnhancements.Graphics.FrameInterpolation CVar. main.cpp's per-tick interpOn also forces the
// classic single-present branch (which calls gdx_frame_pacer_tick()) while an EK editor (Course
// Edit / Create Machine) is active, even though the CVar itself stays 1. Reading the raw CVar here
// made those editor ticks self-unarm this pacer AND get no interpolation pacing either -- neither
// mechanism ran, so the loop free-ran. This returns the active flag main.cpp already committed for
// THIS tick via gdx_gfx_interp_tick_config, so editor ticks (active=0) fall through to normal
// FramePacing and genuine interpolation ticks (active=1) still no-op this pacer.
extern int gdx_gfx_interp_tick_active(void);

// Target: N64 NTSC field rate = 60 / 1.001 Hz. Frame interval = 1.001 / 60 s.
// Expressed as a rational scale of the QPC frequency: ticks = freq * 1001 / 60000.
#define GDX_PACER_INTERVAL_NUM 1001
#define GDX_PACER_INTERVAL_DEN 60000

// If we fall more than this many whole frames behind the schedule (a hitch: menu
// stall, window drag, alt-tab, debugger breakpoint), re-anchor to "now" instead
// of replaying the missed frames. This clamps catch-up bursts.
#define GDX_PACER_MAX_LAG_FRAMES 4

// Fallback used only if the CVar was never registered (it normally is, before the frame loop first
// ticks). Mirrors the platform default set in main.cpp.
#ifdef _WIN32
#define GDX_PACER_CVAR_DEFAULT 0
#else
#define GDX_PACER_CVAR_DEFAULT 1
#endif

#ifdef _WIN32

// Some SDKs predate the high-resolution waitable-timer flag (Win10 1803+).
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static int sInitDone = 0;     // one-time init guard
static int sUsable = 0;       // 0 if QPC unavailable -> pacer degrades to a no-op
static LONGLONG sFreq = 0;    // QPC frequency (ticks/second)
static LONGLONG sIntervalTicks = 0;   // one frame in QPC ticks
static LONGLONG sSpinMarginTicks = 0; // coarse-sleep undershoot margin (~1.5 ms)
static LONGLONG sNextDeadline = 0;    // absolute QPC target for the next boundary; 0 = unarmed
static HANDLE sTimer = NULL;          // waitable timer for the coarse sleep

static void gdx_frame_pacer_init(void) {
    LARGE_INTEGER freq;
    sInitDone = 1;

    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        sUsable = 0; // no monotonic clock -> never pace
        return;
    }
    sFreq = freq.QuadPart;
    sIntervalTicks = sFreq * GDX_PACER_INTERVAL_NUM / GDX_PACER_INTERVAL_DEN;
    if (sIntervalTicks <= 0) {
        sUsable = 0;
        return;
    }
    // ~1.5 ms expressed in QPC ticks (freq * 0.0015 = freq * 3 / 2000).
    sSpinMarginTicks = sFreq * 3 / 2000;

    sTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (sTimer == NULL) {
        sTimer = CreateWaitableTimerW(NULL, FALSE, NULL); // auto-reset
    }
    // A NULL sTimer is not fatal: the tick path then relies on the spin loop alone (correct, just
    // busier).

    sUsable = 1;
}

// QPC ticks -> the 100 ns units SetWaitableTimer expects. `ticks` is at most a few frame intervals
// (~1.7e5 at 10 MHz), so ticks * 1e7 stays well within int64 range.
static LONGLONG gdx_ticks_to_100ns(LONGLONG ticks) {
    return ticks * 10000000LL / sFreq;
}

void gdx_frame_pacer_tick(void) {
    LARGE_INTEGER now;
    LONGLONG remaining;

    // Frame interpolation and frame pacing are opposed pacing owners. When interpolation drove
    // THIS tick, the host paces the SIM to the logic deadline (port/main.cpp) and presents run
    // VSync-paced inside the sub-frame loop; throttling here as well would fight the accumulator
    // and starve the sub-frame budget. Unarm so a later disable re-baselines cleanly.
    if (gdx_gfx_interp_tick_active() != 0) {
        sNextDeadline = 0;
        return;
    }

    // Live read every frame so the menu toggle takes effect immediately.
    if (CVarGetInteger("gEnhancements.Graphics.FramePacing", GDX_PACER_CVAR_DEFAULT) == 0) {
        // Unarm the schedule so a later enable starts from a fresh baseline rather than firing a
        // burst of catch-up frames.
        sNextDeadline = 0;
        return;
    }

    if (!sInitDone) {
        gdx_frame_pacer_init();
    }
    if (!sUsable) {
        return; // no usable clock -> behave like OFF
    }

    QueryPerformanceCounter(&now);

    if (sNextDeadline == 0) {
        // First paced frame (fresh, or first after a re-enable): set the baseline and do not
        // sleep, so no special-casing of an uninitialised previous timestamp is needed.
        sNextDeadline = now.QuadPart + sIntervalTicks;
        gdx_port_logf("[pacer] FramePacing ON: target ~59.94 Hz (N64 NTSC 60/1.001), "
                      "interval %lld QPC ticks. Recommend VSync OFF.\n",
                      (long long)sIntervalTicks);
        return;
    }

    remaining = sNextDeadline - now.QuadPart;

    // Big stall: we are many frames behind (hitch/pause). Re-anchor; do not catch up.
    if (remaining < -(sIntervalTicks * GDX_PACER_MAX_LAG_FRAMES)) {
        sNextDeadline = now.QuadPart + sIntervalTicks;
        return;
    }

    // At or past the deadline: this frame's own work already spent the budget. Advance by whole
    // intervals until the schedule is back in the future, keeping the long-run average locked to
    // the target rate.
    if (remaining <= 0) {
        do {
            sNextDeadline += sIntervalTicks;
        } while (sNextDeadline <= now.QuadPart);
        return;
    }

    // Coarse sleep to ~1.5 ms short of the deadline, then spin the remainder.
    {
        LONGLONG sleep_ticks = remaining - sSpinMarginTicks;
        if (sleep_ticks > 0 && sTimer != NULL) {
            LARGE_INTEGER due;
            due.QuadPart = -gdx_ticks_to_100ns(sleep_ticks); // negative = relative
            if (SetWaitableTimer(sTimer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(sTimer, INFINITE);
            }
        }
        // Also covers a coarse-sleep overshoot, in which case this exits immediately.
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= sNextDeadline) {
                break;
            }
            YieldProcessor();
        }
    }

    // Advance from the absolute schedule, not from "now", so per-frame jitter averages out.
    sNextDeadline += sIntervalTicks;

    // Guard against unbounded drift if a spin somehow overshot by many frames.
    if (sNextDeadline < now.QuadPart - sIntervalTicks * GDX_PACER_MAX_LAG_FRAMES) {
        sNextDeadline = now.QuadPart + sIntervalTicks;
    }
}

#else // !_WIN32

// ---------------------------------------------------------------------------------------------
// POSIX backend. Same schedule shape as the Windows path (absolute deadlines advanced by whole
// frame intervals, re-anchor on a big stall), but no coarse-sleep-plus-spin is needed: an
// absolute clock_nanosleep(TIMER_ABSTIME) sleeps to the exact deadline and is re-armed on EINTR.
// ---------------------------------------------------------------------------------------------

#define GDX_PACER_NS_PER_SEC 1000000000LL

static int sPosixInitDone = 0;
static int sPosixUsable = 0;
static int64_t sPosixIntervalNs = 0; // one N64 NTSC field in nanoseconds (1.001/60 s)
static int64_t sPosixNextDeadlineNs = 0; // absolute CLOCK_MONOTONIC target; 0 = unarmed

static int64_t gdx_monotonic_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * GDX_PACER_NS_PER_SEC + (int64_t)ts.tv_nsec;
}

static void gdx_frame_pacer_init_posix(void) {
    struct timespec res;
    sPosixInitDone = 1;

    // No monotonic clock -> degrade to a no-op, like the Windows path.
    if (clock_gettime(CLOCK_MONOTONIC, &res) != 0) {
        sPosixUsable = 0;
        return;
    }
    // Frame interval = 1.001/60 s expressed in ns: 1e9 * 1001 / 60000.
    sPosixIntervalNs = GDX_PACER_NS_PER_SEC * GDX_PACER_INTERVAL_NUM / GDX_PACER_INTERVAL_DEN;
    if (sPosixIntervalNs <= 0) {
        sPosixUsable = 0;
        return;
    }
    sPosixUsable = 1;
}

static void gdx_sleep_until_ns(int64_t deadlineNs) {
#ifdef __APPLE__
    /* macOS lacks clock_nanosleep; compute remaining time and use nanosleep,
       retrying on EINTR to match the Linux path's guarantee. */
    for (;;) {
        int64_t now = gdx_monotonic_now_ns();
        int64_t remaining = deadlineNs - now;
        if (remaining <= 0) {
            return;
        }
        struct timespec req;
        req.tv_sec = (time_t)(remaining / GDX_PACER_NS_PER_SEC);
        req.tv_nsec = (long)(remaining % GDX_PACER_NS_PER_SEC);
        if (nanosleep(&req, NULL) == 0) {
            return;
        }
        if (errno != EINTR) {
            return;
        }
    }
#else
    struct timespec abs;
    abs.tv_sec = (time_t)(deadlineNs / GDX_PACER_NS_PER_SEC);
    abs.tv_nsec = (long)(deadlineNs % GDX_PACER_NS_PER_SEC);
    // Re-arm on EINTR so a signal cannot cut the wait short -- the whole reason this pacer exists
    // on Linux (see the file header).
    for (;;) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
        if (rc == 0) {
            return;
        }
        if (rc != EINTR) {
            return; // unexpected error: give up waiting rather than spin
        }
    }
#endif
}

void gdx_frame_pacer_tick(void) {
    int64_t now;
    int64_t remaining;

    // See the Windows path above: interpolation owns pacing only on the ticks it actually drove.
    if (gdx_gfx_interp_tick_active() != 0) {
        sPosixNextDeadlineNs = 0;
        return;
    }

    if (CVarGetInteger("gEnhancements.Graphics.FramePacing", GDX_PACER_CVAR_DEFAULT) == 0) {
        sPosixNextDeadlineNs = 0; // re-anchor on a later enable
        return;
    }

    if (!sPosixInitDone) {
        gdx_frame_pacer_init_posix();
    }
    if (!sPosixUsable) {
        return;
    }

    now = gdx_monotonic_now_ns();

    if (sPosixNextDeadlineNs == 0) {
        sPosixNextDeadlineNs = now + sPosixIntervalNs;
        gdx_port_logf("[pacer] FramePacing ON: target ~59.94 Hz (N64 NTSC 60/1.001), "
                      "interval %lld ns (POSIX clock_nanosleep). Recommend VSync OFF.\n",
                      (long long)sPosixIntervalNs);
        return;
    }

    remaining = sPosixNextDeadlineNs - now;

    // Big stall (hitch/pause): re-anchor, do not replay missed frames.
    if (remaining < -(sPosixIntervalNs * GDX_PACER_MAX_LAG_FRAMES)) {
        sPosixNextDeadlineNs = now + sPosixIntervalNs;
        return;
    }

    // At or past the deadline: this frame already spent the budget. Advance by whole intervals
    // until the schedule is back in the future, keeping the long-run average on target.
    if (remaining <= 0) {
        do {
            sPosixNextDeadlineNs += sPosixIntervalNs;
        } while (sPosixNextDeadlineNs <= now);
        return;
    }

    gdx_sleep_until_ns(sPosixNextDeadlineNs);
    sPosixNextDeadlineNs += sPosixIntervalNs;

    now = gdx_monotonic_now_ns();
    if (sPosixNextDeadlineNs < now - sPosixIntervalNs * GDX_PACER_MAX_LAG_FRAMES) {
        sPosixNextDeadlineNs = now + sPosixIntervalNs;
    }
}

#endif // _WIN32
