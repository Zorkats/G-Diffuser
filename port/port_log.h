#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#include <limits.h>         /* PATH_MAX */
#endif
#endif

/* The per-frame trace breadcrumbs sprinkled through the decomp sources (gdx_ck(), gdx_seg_log(),
   ...) each cost three OS calls per line on the GAME thread, so GDX_TRACE gates them without
   touching any decomp call site. Debug traces ON, Release OFF; GDX_TRACE overrides both ways.
   Error-class logging (crash handler, boot one-shots) calls gdx_port_logf directly and is
   always on. */
/* GDX_DIAG_VERBOSE gates the high-frequency per-frame diagnostic families, silent by default in
   both configs. Defined and cached in n64_sched.c; declared here AND in global.h so both the C++
   bridge and decomp C can call it. */
#ifdef __cplusplus
extern "C" {
#endif
int gdx_diag_verbose(void);
#ifdef __cplusplus
}
#endif

// The logging gates below follow the Bucket D policy in gdx_dev_gates.h: CVar is the persisted
// preference, adopted at startup ahead of essentially all boot logging; an env var overrides for
// that run only and is never written back.
#include "gdx_dev_gates.h"

static inline int gdx_trace_enabled(void) {
    return gdx_dev_gate(GDX_GATE_TRACE);
}

// OFF by default: a normal play session must not silently create a log file. Read LIVE rather
// than latched, so ticking the box mid-session starts the log; gdx_port_write_log opens the file
// the first time this returns non-zero, and lines already emitted are gone.
static inline int gdx_log_file_enabled(void) {
    return gdx_dev_gate(GDX_GATE_LOG_FILE) || gdx_dev_gate(GDX_GATE_TRACE) ||
           gdx_dev_gate(GDX_GATE_DIAG_VERBOSE) || gdx_dev_gate(GDX_GATE_DIAG_UNLOCK);
}

// Exe-relative, matching the saves convention in sram_buffer.cpp; falls back to a CWD-relative
// bare filename. Shared by both sinks below so they cannot disagree on where the files land.
static inline const char* gdx_exe_relative_path(char* outPath, size_t outCap, const char* fileName) {
#ifdef _WIN32
    {
        DWORD n = GetModuleFileNameA(NULL, outPath, (DWORD)outCap);
        if (n > 0 && n < outCap) {
            char* slash = strrchr(outPath, '\\');
            if (slash != NULL) {
                slash[1] = '\0';
                if (strlen(outPath) + strlen(fileName) < outCap) {
                    strcat(outPath, fileName);
                    return outPath;
                }
            }
        }
    }
#elif defined(__APPLE__)
    {
        uint32_t bufSize = (uint32_t)outCap;
        if (_NSGetExecutablePath(outPath, &bufSize) == 0) {
            /* Resolve symlinks so the dirname is the real app location */
            char resolved[PATH_MAX];
            if (realpath(outPath, resolved) != NULL &&
                strlen(resolved) < outCap) {
                strcpy(outPath, resolved);
            }
            char* slash = strrchr(outPath, '/');
            if (slash != NULL &&
                (size_t)(slash - outPath) + 1 + strlen(fileName) < outCap) {
                strcpy(slash + 1, fileName);
                return outPath;
            }
        }
    }
#else
    {
        ssize_t rl = readlink("/proc/self/exe", outPath, outCap - 1);
        if (rl > 0 && (size_t)rl < outCap) {
            outPath[rl] = '\0';
            char* slash = strrchr(outPath, '/');
            if (slash != NULL &&
                (size_t)(slash - outPath) + 1 + strlen(fileName) < outCap) {
                strcpy(slash + 1, fileName);
                return outPath;
            }
        }
    }
#endif
    if (strlen(fileName) < outCap) {
        strcpy(outPath, fileName);
    } else if (outCap > 0) {
        outPath[0] = '\0';
    }
    return outPath;
}

static inline const char* gdx_log_file_path(char* outPath, size_t outCap) {
    return gdx_exe_relative_path(outPath, outCap, "gdiffuser-run.log");
}

static inline const char* gdx_crash_report_path(char* outPath, size_t outCap) {
    return gdx_exe_relative_path(outPath, outCap, "gdiffuser-crash.txt");
}

// Holds the file handle open instead of fopen/fclose per call: the Debug CRT's internal dynamic
// buffer deallocates on a stack frame the fiber scheduler may have switched out, which corrupts
// the heap when this is called from the GFX fiber.
static inline void gdx_port_write_log(const char* message) {
    if (message == NULL) {
        return;
    }

    if (gdx_port_log_tap != NULL) {
        gdx_port_log_tap(message);
    }

#ifdef _WIN32
    OutputDebugStringA(message);
    // GUI-subsystem builds have no console, so the file sink is the only way to get boot
    // diagnostics off a user's machine.
    if (gdx_log_file_enabled()) {
        static HANDLE sLogFile = INVALID_HANDLE_VALUE;
        static int sTriedOpen = 0;
        if (!sTriedOpen) {
            sTriedOpen = 1;
            char logPath[MAX_PATH];
            gdx_log_file_path(logPath, sizeof(logPath));
            sLogFile = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (sLogFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(sLogFile, message, (DWORD)lstrlenA(message), &written, NULL);
        }
    }
    fputs(message, stderr);
    fflush(stderr);
#else
    if (gdx_log_file_enabled()) {
        static FILE* sLogFile = NULL;
        static int sTriedOpen = 0;
        if (!sTriedOpen) {
            sTriedOpen = 1;
            char logPath[4096];
            gdx_log_file_path(logPath, sizeof(logPath));
            if (logPath[0] != '\0') {
                sLogFile = fopen(logPath, "ab");
            }
        }
        if (sLogFile != NULL) {
            fputs(message, sLogFile);
            fflush(sLogFile);
        }
    }
    fputs(message, stderr);
    fflush(stderr);
#endif
}

// Deliberately bypasses gdx_log_file_enabled(): field testers set no diagnostic gates, so without
// this a crash leaves zero artifacts on disk. Called only from the crash handler (n64_sched.c),
// which can run on a fiber whose stack is mid-switch — raw CreateFileA/WriteFile (or open/write)
// only, same CRT-FILE* hazard as gdx_port_write_log.
static inline void gdx_crash_report_write(const char* message) {
    if (message == NULL || message[0] == '\0') {
        return;
    }
#ifdef _WIN32
    {
        char path[MAX_PATH];
        HANDLE file;
        gdx_crash_report_path(path, sizeof(path));
        file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, message, (DWORD) lstrlenA(message), &written, NULL);
            CloseHandle(file);
        }
    }
#else
    {
        char path[4096];
        int fd;
        gdx_crash_report_path(path, sizeof(path));
        if (path[0] != '\0') {
            fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                write(fd, message, strlen(message));
                close(fd);
            }
        }
    }
#endif
}

static inline void gdx_port_vlogf(const char* fmt, va_list args) {
    char buffer[2048];
    size_t prefixLen = 0;
#ifdef _WIN32
    /* Millisecond wall clock so lines can be correlated with frame timing and external events. */
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = snprintf(buffer, sizeof(buffer),
                         "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                         st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        if (n > 0 && (size_t)n < sizeof(buffer)) {
            prefixLen = (size_t)n;
        }
    }
#endif
    vsnprintf(buffer + prefixLen, sizeof(buffer) - prefixLen, fmt, args);
    buffer[sizeof(buffer) - 1] = '\0';
    gdx_port_write_log(buffer);
}

static inline void gdx_port_logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    gdx_port_vlogf(fmt, args);
    va_end(args);
}
