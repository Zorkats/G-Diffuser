#include "sram_buffer.h"
#include "gdx_hackmods.h" // per-hack save isolation: gdx_hackmod_active_save_basename
#include "port_log.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cwchar>
#else
#include <cstdlib>
#include <unistd.h>   // readlink, access
#include <sys/stat.h> // mkdir
#include <cerrno>
#endif

// Host-side persistence for the game's cart-SRAM save (settings, course/death-race
// records, cup completion, character engine tuning, single-slot player/staff ghost).
// Host .cpp TU (G-Diffuser exe target, not the gdiffuser_game decomp object library),
// so the CRT is available here. decomp/src/overlays/ovl_i2/save.c only ever calls the
// three C-linkage entry points below; it never sees fopen/FILE*.
extern "C" {

static uint8_t s_sramBuffer[GDX_SRAM_SIZE];
static bool s_initialized = false;

// Windows: <exedir>/saves/fzerox.sav. POSIX: <cwd>/saves/fzerox.sav, matching the 64DD
// disk-save sidecar (disk_savefile.cpp) and the ghost I/O (gdx_ghost_io.c) so every save
// artifact shares one per-user directory rather than the often read-only exe directory.
// Each platform's gdx_sram_path migrates its legacy save on first resolution, and only
// when no current-convention save exists, so an already-migrated one is never clobbered.
#ifdef _WIN32
static bool gdx_sram_path(wchar_t* outPath, size_t outCapChars) {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return false;
    }

    wchar_t* slash = std::wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return false;
    }
    slash[1] = L'\0';

    // The active hack mod (if any) owns its own save file, so a hack's cup completion never
    // lands in the stock progression. gdx_hackmod_active_save_basename is latched at boot.
    wchar_t fileName[GDX_HACKMOD_SAVE_MAX];
    {
        const char* narrow = gdx_hackmod_active_save_basename();
        size_t i = 0;
        for (; narrow[i] != '\0' && i + 1 < GDX_HACKMOD_SAVE_MAX; i++) {
            fileName[i] = static_cast<wchar_t>(static_cast<unsigned char>(narrow[i]));
        }
        fileName[i] = L'\0';
    }
    const wchar_t* savesDir = L"saves\\";
    if (wcslen(exePath) + wcslen(savesDir) + wcslen(fileName) >= outCapChars) {
        return false;
    }
    wcscpy_s(outPath, outCapChars, exePath);
    wcscat_s(outPath, outCapChars, L"saves");
    if (!CreateDirectoryW(outPath, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    wcscat_s(outPath, outCapChars, L"\\");
    wcscat_s(outPath, outCapChars, fileName);

    wchar_t legacyPath[MAX_PATH * 2] = {};
    if (wcslen(exePath) + wcslen(fileName) < sizeof(legacyPath) / sizeof(legacyPath[0])) {
        wcscpy_s(legacyPath, sizeof(legacyPath) / sizeof(legacyPath[0]), exePath);
        wcscat_s(legacyPath, sizeof(legacyPath) / sizeof(legacyPath[0]), fileName);
        if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(legacyPath) != INVALID_FILE_ATTRIBUTES) {
            if (MoveFileExW(legacyPath, outPath, MOVEFILE_WRITE_THROUGH)) {
                gdx_port_logf("[sram] migrated legacy %s into saves\\ folder\n", gdx_hackmod_active_save_basename());
            }
        }
    }
    return true;
}
#else
// POSIX resolves CWD-relative rather than exe-relative because the exe directory is
// read-only inside an AppImage, /opt or Flatpak deployment, where a flush would silently
// fail and lose the save. Matches disk_savefile.cpp's savesDirectory() ("." as the exe dir
// on POSIX, mkdir "./saves" 0755) and the ghost I/O convention.

// rename(2) is atomic within a filesystem; a cross-device EXDEV falls back to a byte copy
// plus unlink. Callers only invoke this when dstPath does not yet exist, so an existing
// current-convention save is never overwritten.
static bool gdx_sram_move_file(const char* srcPath, const char* dstPath) {
    if (rename(srcPath, dstPath) == 0) {
        return true;
    }
    if (errno != EXDEV) {
        return false;
    }

    FILE* in = fopen(srcPath, "rb");
    if (in == nullptr) {
        return false;
    }
    FILE* out = fopen(dstPath, "wb");
    if (out == nullptr) {
        fclose(in);
        return false;
    }
    char buf[8192];
    size_t rd;
    bool ok = true;
    while ((rd = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, rd, out) != rd) {
            ok = false;
            break;
        }
    }
    if (ferror(in) != 0) {
        ok = false;
    }
    if (fflush(out) != 0) {
        ok = false;
    }
    if (fclose(out) != 0) {
        ok = false;
    }
    fclose(in);
    if (!ok) {
        remove(dstPath); // don't leave a half-written destination behind
        return false;
    }
    remove(srcPath); // best-effort: the copy is the source of truth now
    return true;
}

static bool gdx_sram_path(char* outPath, size_t outCap) {
    // See the Windows branch: the active hack mod owns its own save file.
    const char* fileName = gdx_hackmod_active_save_basename();
    const char* savesDir = "saves";

    if (strlen(savesDir) + 1 + strlen(fileName) >= outCap) {
        return false;
    }
    if (mkdir(savesDir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    strcpy(outPath, savesDir);
    strcat(outPath, "/");
    strcat(outPath, fileName);

    // Older POSIX builds wrote exe-relative <exedir>/saves/fzerox.sav. Migrate that in, but
    // ONLY when no CWD-relative save exists yet, so an already-migrated (or freshly written)
    // save is never clobbered.
    if (access(outPath, F_OK) != 0) {
        char exePath[4096];
        ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (n > 0) {
            exePath[n] = '\0';
            char* slash = strrchr(exePath, '/');
            if (slash != nullptr) {
                slash[1] = '\0'; // keep trailing separator
                char legacyPath[4096 + 32];
                if (strlen(exePath) + strlen("saves/") + strlen(fileName) < sizeof(legacyPath)) {
                    strcpy(legacyPath, exePath);
                    strcat(legacyPath, "saves/");
                    strcat(legacyPath, fileName);
                    if (access(legacyPath, F_OK) == 0 && gdx_sram_move_file(legacyPath, outPath)) {
                        gdx_port_logf("[sram] migrated legacy exe-relative %s into CWD saves/ folder\n",
                                      gdx_hackmod_active_save_basename());
                    }
                }
            }
        }
    }
    return true;
}
#endif

void gdx_sram_init(void) {
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    memset(s_sramBuffer, 0, sizeof(s_sramBuffer));

#ifdef _WIN32
    wchar_t path[MAX_PATH * 2] = {};
    if (!gdx_sram_path(path, sizeof(path) / sizeof(path[0]))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; starting with a fresh save.\n");
        return;
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) {
        gdx_port_logf("[sram] no existing %s; starting fresh (first boot creates it on first write).\n",
                      gdx_hackmod_active_save_basename());
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != (long) GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] %s size mismatch (%ld bytes, expected %u); starting with a fresh save.\n",
                      gdx_hackmod_active_save_basename(), sz, GDX_SRAM_SIZE);
        fclose(f);
        return;
    }

    if (fread(s_sramBuffer, 1, GDX_SRAM_SIZE, f) != GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: failed reading %s; starting with a fresh save.\n",
                      gdx_hackmod_active_save_basename());
        memset(s_sramBuffer, 0, sizeof(s_sramBuffer));
    } else {
        gdx_port_logf("[sram] loaded %u bytes from %s\n", GDX_SRAM_SIZE, gdx_hackmod_active_save_basename());
    }
    fclose(f);
#else
    char path[4096] = {};
    if (!gdx_sram_path(path, sizeof(path))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; starting with a fresh save.\n");
        return;
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        gdx_port_logf("[sram] no existing %s; starting fresh (first boot creates it on first write).\n",
                      gdx_hackmod_active_save_basename());
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != (long) GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] %s size mismatch (%ld bytes, expected %u); starting with a fresh save.\n",
                      gdx_hackmod_active_save_basename(), sz, GDX_SRAM_SIZE);
        fclose(f);
        return;
    }

    if (fread(s_sramBuffer, 1, GDX_SRAM_SIZE, f) != GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: failed reading %s; starting with a fresh save.\n",
                      gdx_hackmod_active_save_basename());
        memset(s_sramBuffer, 0, sizeof(s_sramBuffer));
    } else {
        gdx_port_logf("[sram] loaded %u bytes from %s\n", GDX_SRAM_SIZE, gdx_hackmod_active_save_basename());
    }
    fclose(f);
#endif
}

static void gdx_sram_flush(void) {
#ifdef _WIN32
    wchar_t path[MAX_PATH * 2] = {};
    if (!gdx_sram_path(path, sizeof(path) / sizeof(path[0]))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; save not persisted.\n");
        return;
    }

    // Stage to fzerox.sav.tmp and MoveFileEx-replace, so a crash mid-write cannot truncate
    // or half-write the live save -- the previous fzerox.sav stays intact until the temp
    // is complete.
    wchar_t tempPath[MAX_PATH * 2] = {};
    if (wcslen(path) + 4 >= sizeof(tempPath) / sizeof(tempPath[0])) {
        gdx_port_logf("[sram] WARNING: save path too long for temp file; save not persisted.\n");
        return;
    }
    wcscpy_s(tempPath, sizeof(tempPath) / sizeof(tempPath[0]), path);
    wcscat_s(tempPath, sizeof(tempPath) / sizeof(tempPath[0]), L".tmp");

    FILE* f = nullptr;
    if (_wfopen_s(&f, tempPath, L"wb") != 0 || f == nullptr) {
        gdx_port_logf("[sram] WARNING: failed to open %s.tmp for writing; save not persisted.\n",
                      gdx_hackmod_active_save_basename());
        return;
    }
    bool ok = fwrite(s_sramBuffer, 1, GDX_SRAM_SIZE, f) == GDX_SRAM_SIZE;
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[sram] WARNING: failed writing %s.tmp; save not persisted.\n",
                      gdx_hackmod_active_save_basename());
        _wremove(tempPath);
        return;
    }
    if (!MoveFileExW(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        gdx_port_logf("[sram] WARNING: could not replace %s; save not persisted.\n",
                      gdx_hackmod_active_save_basename());
        _wremove(tempPath);
        return;
    }
#else
    char path[4096] = {};
    if (!gdx_sram_path(path, sizeof(path))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; save not persisted.\n");
        return;
    }

    // Stage to fzerox.sav.tmp and rename over the live file: rename(2) is atomic within a
    // filesystem, so a crash mid-write cannot truncate the real save.
    char tempPath[4096 + 8] = {};
    if (strlen(path) + 4 >= sizeof(tempPath)) {
        gdx_port_logf("[sram] WARNING: save path too long for temp file; save not persisted.\n");
        return;
    }
    strcpy(tempPath, path);
    strcat(tempPath, ".tmp");

    FILE* f = fopen(tempPath, "wb");
    if (f == nullptr) {
        gdx_port_logf("[sram] WARNING: failed to open %s.tmp for writing; save not persisted.\n",
                      gdx_hackmod_active_save_basename());
        return;
    }
    bool ok = fwrite(s_sramBuffer, 1, GDX_SRAM_SIZE, f) == GDX_SRAM_SIZE;
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[sram] WARNING: failed writing %s.tmp; save not persisted.\n",
                      gdx_hackmod_active_save_basename());
        remove(tempPath);
        return;
    }
    if (rename(tempPath, path) != 0) {
        gdx_port_logf("[sram] WARNING: could not replace %s; save not persisted.\n",
                      gdx_hackmod_active_save_basename());
        remove(tempPath);
        return;
    }
#endif
}

void gdx_sram_read(unsigned int offset, void* dst, unsigned int size) {
    gdx_sram_init(); // load order is not guaranteed: Sram_Init() may not have run yet
    if (dst == nullptr) {
        return;
    }
    if ((size_t) offset + (size_t) size > GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: read out of range (offset=%u size=%u); returning zeros.\n", offset, size);
        memset(dst, 0, size);
        return;
    }
    memcpy(dst, s_sramBuffer + offset, size);
}

void gdx_sram_write(unsigned int offset, const void* src, unsigned int size) {
    gdx_sram_init();
    if (src == nullptr) {
        return;
    }
    if ((size_t) offset + (size_t) size > GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: write out of range (offset=%u size=%u); ignored.\n", offset, size);
        return;
    }
    memcpy(s_sramBuffer + offset, src, size);
    gdx_sram_flush(); // write-through: no debounce needed at 32KB / event-driven call frequency
}

} // extern "C"
