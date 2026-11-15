/* port/gdx_hang_watchdog.c -- when a freeze starts and ends, and its log lines. See gdx_hang_watchdog.h.
 * Copyright (c) Zorkats
 *
 * Pure C: no thread, no clock and no platform header, so the unit test links this file alone. The thread
 * that drives it is gdx_hang_watchdog_thread.c.
 */

#include "gdx_hang_watchdog.h"

#include <stdio.h>

void gdx_hang_watch_reset(GdxHangWatch* watch) {
    watch->lastBeats = 0;
    watch->lastChange = 0.0;
    watch->started = 0;
    watch->frozen = 0;
    watch->freezes = 0;
}

int gdx_hang_watch_step(GdxHangWatch* watch, unsigned long long beats, double now, double* stalled) {
    if (!watch->started || (beats != watch->lastBeats)) {
        int resumed = watch->started && watch->frozen;

        if (resumed && (stalled != NULL)) {
            *stalled = now - watch->lastChange;
        }
        watch->started = 1;
        watch->lastBeats = beats;
        watch->lastChange = now;
        watch->frozen = 0;
        return resumed ? GDX_HANG_RESUMED : GDX_HANG_NONE;
    }
    /* No beat since the last step. Before the loop has run GDX_HANG_ARM_BEATS frames the game is still
       booting, and a boot load that takes long is not a freeze. */
    if (watch->frozen || (beats < GDX_HANG_ARM_BEATS) || (now - watch->lastChange < GDX_HANG_SECONDS)) {
        return GDX_HANG_NONE;
    }
    watch->frozen = 1;
    watch->freezes++;
    return GDX_HANG_FROZE;
}

int gdx_hang_watch_may_dump(const GdxHangWatch* watch) {
    return watch->frozen && (watch->freezes >= 1) && (watch->freezes <= GDX_HANG_MAX_DUMPS);
}

int gdx_hang_format_header(char* out, size_t cap, int freeze, double seconds) {
    return snprintf(out, cap, "[hang] FREEZE %d: no frame for %.1f s, main thread stack follows\n", freeze,
                    seconds);
}

int gdx_hang_format_frame(char* out, size_t cap, int index, const char* module, unsigned long long rva) {
    return snprintf(out, cap, "[hang]   #%d %s+0x%llX\n", index, (module != NULL) ? module : "?", rva);
}

int gdx_hang_format_resumed(char* out, size_t cap, int freeze, double seconds) {
    return snprintf(out, cap, "[hang] FREEZE %d ended: frames resumed after %.1f s\n", freeze, seconds);
}
