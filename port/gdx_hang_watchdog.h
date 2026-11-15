/* port/gdx_hang_watchdog.h -- the hang watchdog: evidence for a freeze the crash handler never sees.
 * Copyright (c) Zorkats
 *
 * A freeze raises no exception, so the crash handler (n64_sched.c) stays silent and the log just stops. The
 * frame loop beats this module once a frame; a thread of its own watches the beats. When none has arrived for
 * GDX_HANG_SECONDS it writes, once per freeze:
 *
 *   - a "[hang]" header line, and the main thread's stack as "[hang]   #n module+0xRVA" lines, which the
 *     release .map resolves (Windows x64 only; elsewhere the header line alone);
 *   - a minidump next to the executable, gdiffuser-hang-<UTC time>.dmp: thread stacks and the module list,
 *     no heap (Windows only), at most GDX_HANG_MAX_DUMPS a session;
 *   - a "[hang] resumed" line if the frames come back, so a stall (a window drag, a slow load) can be told
 *     from a real freeze.
 *
 * The lines go to gdiffuser-crash.txt whatever the log settings, and to gdiffuser-run.log when that is open.
 * The watchdog stays off while a debugger is attached, and the CVar gdx.HangWatchdog (default 1) turns it off.
 *
 * The decision of when a freeze starts and ends is the pure part below, gdx_hang_watch_step, driven by the
 * beat count and a clock the caller passes in, so it is tested without a thread or a real wait.
 */

#ifndef GDX_HANG_WATCHDOG_H
#define GDX_HANG_WATCHDOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_HANG_ARM_BEATS 120 /* the beats before the watchdog arms, so the boot loads never count */
#define GDX_HANG_SECONDS 5.0   /* how long without a beat is a freeze */
#define GDX_HANG_MAX_DUMPS 3   /* the minidumps one session writes, at most */
#define GDX_HANG_MAX_FRAMES 48 /* the stack frames one freeze logs, at most */

/* What one step of the watch saw. */
enum {
    GDX_HANG_NONE = 0,    /* nothing new */
    GDX_HANG_FROZE = 1,   /* a freeze just started: write the evidence now */
    GDX_HANG_RESUMED = 2, /* the beats came back after a freeze */
};

typedef struct GdxHangWatch {
    unsigned long long lastBeats; /* the beat count the last step saw */
    double lastChange;            /* when the beat count last moved, in the caller's seconds */
    int started;                  /* a step has run, so lastChange means something */
    int frozen;                   /* a freeze has been reported and has not ended */
    int freezes;                  /* the freezes reported this session */
} GdxHangWatch;

void gdx_hang_watch_reset(GdxHangWatch* watch);

/* One look at the beats: beats is the frame loop's count so far, now the time in seconds. Returns one of the
   GDX_HANG_* values. On GDX_HANG_RESUMED, *stalled (when not NULL) is how long the freeze lasted. */
int gdx_hang_watch_step(GdxHangWatch* watch, unsigned long long beats, double now, double* stalled);

/* Whether the freeze just reported may write a minidump: the first GDX_HANG_MAX_DUMPS of a session do. */
int gdx_hang_watch_may_dump(const GdxHangWatch* watch);

/* The log lines, each ending in "\n". They return what snprintf returns. The header names the freeze's number in the session and how long it has lasted. */
int gdx_hang_format_header(char* out, size_t cap, int freeze, double seconds);
int gdx_hang_format_frame(char* out, size_t cap, int index, const char* module, unsigned long long rva);
int gdx_hang_format_resumed(char* out, size_t cap, int freeze, double seconds);

/* The runtime, gdx_hang_watchdog_thread.c. start runs once, on the main thread, before the frame loop; it
   does nothing when the CVar or a debugger says so. beat runs once a frame and is one atomic add. */
void gdx_hang_watchdog_start(int enabled);
void gdx_hang_watchdog_beat(void);

#ifdef __cplusplus
}
#endif

#endif
