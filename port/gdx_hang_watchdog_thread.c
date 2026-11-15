/* port/gdx_hang_watchdog_thread.c -- the hang watchdog's thread, its stack capture and its minidump.
 * Copyright (c) Zorkats
 *
 * See gdx_hang_watchdog.h. The thread looks at the frame loop's beat count four times a second and hands it
 * to gdx_hang_watch_step, which decides when a freeze starts and ends.
 *
 * While the main thread is suspended this thread only reads registers and unwinds: no heap, no CRT stream
 * and no dbghelp call, because the frozen thread may hold the lock any of those would wait on. That is why
 * the walk is RtlVirtualUnwind rather than StackWalk64. Every line reaches gdiffuser-crash.txt by raw file
 * writes (gdx_crash_report_write) before the minidump is attempted, and gdiffuser-run.log last, since its
 * stderr copy takes a CRT lock the frozen thread may hold.
 */

#include "gdx_hang_watchdog.h"
#include "port_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "Dbghelp.lib")
#else
#include <pthread.h>
#include <time.h>
#endif

#define GDX_HANG_POLL_MS 250
#define GDX_HANG_LINE 192

/* The lines of one report, kept so the run log gets them after the crash file and the dump. */
static char sLines[GDX_HANG_MAX_FRAMES + 4][GDX_HANG_LINE];
static int sLineCount;

static void hang_line(const char* line) {
    gdx_crash_report_write(line);
    if (sLineCount < (int) (sizeof(sLines) / sizeof(sLines[0]))) {
        snprintf(sLines[sLineCount++], GDX_HANG_LINE, "%s", line);
    }
}

static void hang_flush_run_log(void) {
    int i;

    for (i = 0; i < sLineCount; i++) {
        gdx_port_logf("%s", sLines[i]);
    }
    sLineCount = 0;
}

#ifdef _WIN32

static volatile LONG64 sBeats;
static HANDLE sMainThread;

void gdx_hang_watchdog_beat(void) {
    InterlockedIncrement64(&sBeats);
}

static unsigned long long hang_beats(void) {
    return (unsigned long long) InterlockedCompareExchange64(&sBeats, 0, 0);
}

static double hang_now(void) {
    return (double) GetTickCount64() / 1000.0;
}

/* The main thread's return addresses, innermost first. Suspends it for the walk only. */
static int hang_capture(DWORD64* pcs, int cap) {
    int count = 0;
    CONTEXT context;

    if (SuspendThread(sMainThread) == (DWORD) -1) {
        return 0;
    }
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;
#if defined(_M_X64)
    __try {
        if (GetThreadContext(sMainThread, &context)) {
            while ((count < cap) && (context.Rip != 0)) {
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION function;

                pcs[count++] = context.Rip;
                function = RtlLookupFunctionEntry(context.Rip, &imageBase, NULL);
                if (function != NULL) {
                    PVOID handlerData = NULL;
                    DWORD64 establisher = 0;

                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                     &establisher, NULL);
                } else {
                    /* A leaf function: its return address is on top of the stack. Read without faulting,
                       since a corrupt stack pointer must not end the report. */
                    DWORD64 ret = 0;

                    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID) (uintptr_t) context.Rsp, &ret,
                                           sizeof(ret), NULL)) {
                        break;
                    }
                    context.Rip = ret;
                    context.Rsp += sizeof(ret);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A corrupt frame ends the walk; what was captured stands. */
    }
#else
    (void) pcs;
    (void) cap; /* not x64: the walk is x64 only, so the report has the header line alone */
#endif
    ResumeThread(sMainThread);
    return count;
}

/* The module an address belongs to, as its file name without the folder, and the address's offset in it. */
static void hang_module_of(DWORD64 pc, char* name, size_t cap, unsigned long long* rva) {
    HMODULE module = NULL;
    char path[MAX_PATH];
    const char* base;

    snprintf(name, cap, "?");
    *rva = (unsigned long long) pc;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR) (uintptr_t) pc, &module)) {
        return;
    }
    *rva = (unsigned long long) (pc - (DWORD64) (uintptr_t) module);
    if (GetModuleFileNameA(module, path, sizeof(path)) == 0) {
        return;
    }
    base = strrchr(path, '\\');
    snprintf(name, cap, "%s", (base != NULL) ? base + 1 : path);
}

/* A minidump next to the executable: thread stacks and the module list, no heap. Its file name only, never
   the folder, goes in the log. */
static void hang_dump(void) {
    SYSTEMTIME st;
    char name[64];
    char path[MAX_PATH];
    char line[GDX_HANG_LINE];
    HANDLE file;
    BOOL written;

    GetSystemTime(&st);
    snprintf(name, sizeof(name), "gdiffuser-hang-%04u%02u%02u-%02u%02u%02u.dmp", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    gdx_exe_relative_path(path, sizeof(path), name);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        snprintf(line, sizeof(line), "[hang]   minidump %s could not be created (error %lu)\n", name,
                 (unsigned long) GetLastError());
        hang_line(line);
        return;
    }
    written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                (MINIDUMP_TYPE) (MiniDumpNormal | MiniDumpWithThreadInfo), NULL, NULL, NULL);
    CloseHandle(file);
    snprintf(line, sizeof(line), written ? "[hang]   minidump %s written\n" : "[hang]   minidump %s failed\n",
             name);
    hang_line(line);
}

static void hang_report(const GdxHangWatch* watch, double seconds) {
    DWORD64 pcs[GDX_HANG_MAX_FRAMES];
    char line[GDX_HANG_LINE];
    char module[96];
    unsigned long long rva;
    int count;
    int i;

    gdx_hang_format_header(line, sizeof(line), watch->freezes, seconds);
    hang_line(line);
    count = hang_capture(pcs, GDX_HANG_MAX_FRAMES);
    for (i = 0; i < count; i++) {
        hang_module_of(pcs[i], module, sizeof(module), &rva);
        gdx_hang_format_frame(line, sizeof(line), i, module, rva);
        hang_line(line);
    }
    if (gdx_hang_watch_may_dump(watch)) {
        hang_dump();
    }
    hang_flush_run_log();
}

static DWORD WINAPI hang_thread(LPVOID unused) {
    GdxHangWatch watch;
    int reported = 0;

    (void) unused;
    gdx_hang_watch_reset(&watch);
    for (;;) {
        double stalled = 0.0;
        double now = hang_now();
        int seen = gdx_hang_watch_step(&watch, hang_beats(), now, &stalled);

        if (seen == GDX_HANG_FROZE) {
            /* A debugger's breakpoint stops frames too; that is no freeze to report, nor its end. */
            reported = !IsDebuggerPresent();
            if (reported) {
                hang_report(&watch, now - watch.lastChange);
            }
        } else if ((seen == GDX_HANG_RESUMED) && reported) {
            char line[GDX_HANG_LINE];

            reported = 0;
            gdx_hang_format_resumed(line, sizeof(line), watch.freezes, stalled);
            hang_line(line);
            hang_flush_run_log();
        }
        Sleep(GDX_HANG_POLL_MS);
    }
}

void gdx_hang_watchdog_start(int enabled) {
    static int sStarted = 0;
    HANDLE thread;

    if (!enabled || sStarted) {
        return;
    }
    sStarted = 1;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sMainThread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        gdx_port_logf("[hang] watchdog off: the main thread handle could not be opened (error %lu)\n",
                      (unsigned long) GetLastError());
        return;
    }
    thread = CreateThread(NULL, 64 * 1024, hang_thread, NULL, 0, NULL);
    if (thread == NULL) {
        gdx_port_logf("[hang] watchdog off: its thread could not start (error %lu)\n",
                      (unsigned long) GetLastError());
        return;
    }
    CloseHandle(thread);
    gdx_port_logf("[hang] watchdog on: a %.0f s freeze writes the main thread stack\n", GDX_HANG_SECONDS);
}

#else /* not Windows: the header line alone, no stack and no dump */

static unsigned long long sBeats;

void gdx_hang_watchdog_beat(void) {
    __atomic_fetch_add(&sBeats, 1ULL, __ATOMIC_RELAXED);
}

static unsigned long long hang_beats(void) {
    return __atomic_load_n(&sBeats, __ATOMIC_RELAXED);
}

static double hang_now(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static void* hang_thread(void* unused) {
    GdxHangWatch watch;
    struct timespec poll = { 0, GDX_HANG_POLL_MS * 1000000L };

    (void) unused;
    gdx_hang_watch_reset(&watch);
    for (;;) {
        char line[GDX_HANG_LINE];
        double stalled = 0.0;
        double now = hang_now();
        int seen = gdx_hang_watch_step(&watch, hang_beats(), now, &stalled);

        if (seen == GDX_HANG_FROZE) {
            gdx_hang_format_header(line, sizeof(line), watch.freezes, now - watch.lastChange);
            hang_line(line);
            hang_flush_run_log();
        } else if (seen == GDX_HANG_RESUMED) {
            gdx_hang_format_resumed(line, sizeof(line), watch.freezes, stalled);
            hang_line(line);
            hang_flush_run_log();
        }
        nanosleep(&poll, NULL);
    }
    return NULL;
}

void gdx_hang_watchdog_start(int enabled) {
    static int sStarted = 0;
    pthread_t thread;

    if (!enabled || sStarted) {
        return;
    }
    sStarted = 1;
    if (pthread_create(&thread, NULL, hang_thread, NULL) != 0) {
        gdx_port_logf("[hang] watchdog off: its thread could not start\n");
        return;
    }
    pthread_detach(thread);
    gdx_port_logf("[hang] watchdog on: a %.0f s freeze writes a [hang] line\n", GDX_HANG_SECONDS);
}

#endif
