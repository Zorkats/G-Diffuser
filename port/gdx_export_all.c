/* port/gdx_export_all.c -- what "Export everything" puts in its zip (gdx_export_all.h). */

#include "gdx_export_all.h"

#include <stdio.h>
#include <string.h>

static int lower(int c) {
    return ((c >= 'A') && (c <= 'Z')) ? (c - 'A' + 'a') : c;
}

/* ASCII case-insensitive compare of n bytes. */
static int same_n(const char* a, const char* b, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (lower((unsigned char) a[i]) != lower((unsigned char) b[i])) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return 1;
}

static int same(const char* a, size_t aLen, const char* b) {
    return (strlen(b) == aLen) && same_n(a, b, aLen);
}

static int starts_with(const char* s, size_t sLen, const char* prefix) {
    const size_t n = strlen(prefix);

    return (sLen >= n) && same_n(s, prefix, n);
}

static int ends_with(const char* s, size_t sLen, const char* suffix) {
    const size_t n = strlen(suffix);

    return (sLen >= n) && same_n(s + sLen - n, suffix, n);
}

/* A relative '/' path with no empty, "." or ".." segment, no backslash and no drive. */
static int path_ok(const char* p) {
    const char* seg = p;

    if ((p == NULL) || (p[0] == '\0') || (p[0] == '/') || (strchr(p, '\\') != NULL) || (strchr(p, ':') != NULL)) {
        return 0;
    }
    for (;;) {
        const char* slash = strchr(seg, '/');
        const size_t len = (slash != NULL) ? (size_t) (slash - seg) : strlen(seg);

        if ((len == 0) || ((len == 1) && (seg[0] == '.')) || ((len == 2) && (seg[0] == '.') && (seg[1] == '.'))) {
            return 0;
        }
        if (slash == NULL) {
            return 1;
        }
        seg = slash + 1;
    }
}

static int is_rom_or_junk(const char* name, size_t len) {
    static const char* const kSuffixes[] = { ".z64", ".n64", ".v64", ".ndd", ".d64", ".rom", ".lock", ".bak", ".log" };
    /* Built from the ROM or shipped with the game, wherever a player moved them. */
    static const char* const kNames[] = { "fzerox.o2r", "fzerox-disk.o2r", "generic.o2r", "n64ddipl.o2r",
                                          "gdiffuser.o2r" };
    size_t i;

    for (i = 0; i < sizeof(kSuffixes) / sizeof(kSuffixes[0]); i++) {
        if (ends_with(name, len, kSuffixes[i])) {
            return 1;
        }
    }
    for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (same(name, len, kNames[i])) {
            return 1;
        }
    }
    return 0;
}

/* gp_resume.bin, death_race_records.bin and their per-hack copies (main.cpp names them). */
static int is_record_file(const char* name, size_t len) {
    if (same(name, len, "gp_resume.bin") || same(name, len, "death_race_records.bin")) {
        return 1;
    }
    return (starts_with(name, len, "gp_resume-hack-") || starts_with(name, len, "death_race_records-hack-")) &&
           ends_with(name, len, ".bin");
}

static unsigned classify_top(const char* name, size_t len) {
    if (same(name, len, "course-elements.gdxs") || same(name, len, "course-forks.gbs") ||
        same(name, len, "course-meta.gms") || same(name, len, "course-settings.gks")) {
        return GDX_EXPORT_TRACKS;
    }
    if (same(name, len, "profiles.gps") || is_record_file(name, len)) {
        return GDX_EXPORT_PLAYER;
    }
    if (same(name, len, "gdiffuser.cfg.json") || same(name, len, "imgui.ini") || same(name, len, "palette.txt") ||
        same(name, len, "machine_stats.txt")) {
        return GDX_EXPORT_SETTINGS;
    }
    if (same(name, len, "profile-keys.gpk")) {
        return GDX_EXPORT_KEYS;
    }
    return 0;
}

/* saves/<diskName>.gdd.recovery and saves/<diskName>.gdd.recovery.snapshots hold track recovery and snapshots. */
static int is_track_recovery_folder(const char* name, size_t len) {
    return ends_with(name, len, ".gdd.recovery") || ends_with(name, len, ".gdd.recovery.snapshots");
}

unsigned gdx_export_classify(const char* relPath) {
    const char* slash;
    const char* rest;
    const char* base;
    size_t firstLen;
    size_t baseLen;

    if (!path_ok(relPath)) {
        return 0;
    }
    base = strrchr(relPath, '/');
    base = (base != NULL) ? base + 1 : relPath;
    baseLen = strlen(base);
    if (is_rom_or_junk(base, baseLen)) {
        return 0;
    }
    slash = strchr(relPath, '/');
    if (slash == NULL) {
        return classify_top(relPath, baseLen);
    }
    firstLen = (size_t) (slash - relPath);
    rest = slash + 1;

    if (same(relPath, firstLen, "saves")) {
        const char* second = strchr(rest, '/');

        if (second == NULL) {
            if (ends_with(base, baseLen, ".sav") || same(base, baseLen, "achievements.txt")) {
                return GDX_EXPORT_PLAYER;
            }
            return ends_with(base, baseLen, ".gdd") ? GDX_EXPORT_TRACKS : 0;
        }
        return is_track_recovery_folder(rest, (size_t) (second - rest)) ? GDX_EXPORT_TRACKS : 0;
    }
    if (same(relPath, firstLen, "ghosts")) {
        if (strchr(rest, '/') != NULL) {
            return 0;
        }
        return (ends_with(base, baseLen, ".gdg") || same(base, baseLen, "selection.gds")) ? GDX_EXPORT_PLAYER : 0;
    }
    if (same(relPath, firstLen, "exports")) {
        const char* second = strchr(rest, '/');

        if (second != NULL) {
            return 0; /* exports/machines/ holds captures of the game's own machine models */
        }
        if (starts_with(base, baseLen, GDX_EXPORT_FILE_PREFIX)) {
            return 0;
        }
        return (ends_with(base, baseLen, ".gdxc") || ends_with(base, baseLen, ".gdxp") ||
                ends_with(base, baseLen, ".gdxm"))
                   ? GDX_EXPORT_TRACKS
                   : 0;
    }
    if (same(relPath, firstLen, "mods")) {
        const char* second = strchr(rest, '/');

        if ((second != NULL) && same(rest, (size_t) (second - rest), "~romhacks")) {
            return 0;
        }
        return GDX_EXPORT_SETTINGS;
    }
    if (same(relPath, firstLen, "music")) {
        return GDX_EXPORT_SETTINGS;
    }
    return 0;
}

int gdx_export_folder_wanted(const char* relPath) {
    const char* slash;
    const char* rest;
    size_t firstLen;

    if ((relPath != NULL) && (relPath[0] == '\0')) {
        return 1;
    }
    if (!path_ok(relPath)) {
        return 0;
    }
    slash = strchr(relPath, '/');
    firstLen = (slash != NULL) ? (size_t) (slash - relPath) : strlen(relPath);
    rest = (slash != NULL) ? slash + 1 : NULL;

    if (same(relPath, firstLen, "saves")) {
        const char* second;

        if (rest == NULL) {
            return 1;
        }
        second = strchr(rest, '/');
        return is_track_recovery_folder(rest, (second != NULL) ? (size_t) (second - rest) : strlen(rest));
    }
    if (same(relPath, firstLen, "ghosts")) {
        return rest == NULL;
    }
    if (same(relPath, firstLen, "exports")) {
        return rest == NULL;
    }
    if (same(relPath, firstLen, "mods")) {
        const char* second;

        if (rest == NULL) {
            return 1;
        }
        second = strchr(rest, '/');
        return !same(rest, (second != NULL) ? (size_t) (second - rest) : strlen(rest), "~romhacks");
    }
    return same(relPath, firstLen, "music");
}

const char* gdx_export_group_name(unsigned group) {
    switch (group) {
        case GDX_EXPORT_TRACKS:
            return "Tracks and snapshots";
        case GDX_EXPORT_PLAYER:
            return "Profiles, saves, records and ghosts";
        case GDX_EXPORT_SETTINGS:
            return "Settings and mods";
        case GDX_EXPORT_KEYS:
            return "Signing keys";
        default:
            return "";
    }
}

int gdx_export_file_name(int year, int month, int day, int hour, int minute, int second, char* out, size_t outCap) {
    int n;

    if ((year < 1970) || (year > 9999) || (month < 1) || (month > 12) || (day < 1) || (day > 31) || (hour < 0) ||
        (hour > 23) || (minute < 0) || (minute > 59) || (second < 0) || (second > 60) || (out == NULL)) {
        return -1;
    }
    n = snprintf(out, outCap, GDX_EXPORT_FILE_PREFIX "%04d%02d%02d-%02d%02d%02d.zip", year, month, day, hour, minute,
                 second);
    if ((n < 0) || ((size_t) n >= outCap)) {
        if (outCap > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return n;
}
