// G-Diffuser — 64DD disk image loader (Expansion Kit). Host-side counterpart of
// port/n64_leo.c: owns the CRT file I/O the gdiffuser_game target cannot include.
#include "port_log.h"
#include "rom_buffer.h"
#include "gdx_extract_launch.h"
#include "gen/EkTranslatedOverrides.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h> // readlink
#endif

extern "C" {
unsigned char* gdx_disk_buffer = nullptr;
unsigned int gdx_disk_size = 0;

// 64DD IPL/drive ROM image (user-supplied): holds the built-in kanji/ANK font the EK
// text renderers read through LeoGetKAdr/LeoGetAAdr + DDROM_FONT_START.
unsigned char* gdx_ddipl_buffer = nullptr;
unsigned int gdx_ddipl_size = 0;

void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize);
// port/gdx_ek_strings.c: copies the translated disk's own English into the overlay
// string symbols. Declared here because those are C TUs with no header of their own.
void gdx_ek_strings_apply(const unsigned char* disk, unsigned long long diskSize);
// port/disk_savefile.cpp: init fingerprints the pristine image and loads any existing
// sidecar; apply replays the saved dirty ranges over it.
void gdx_disk_save_init(const char* diskName, const unsigned char* pristine, unsigned int size);
void gdx_disk_save_apply(unsigned char* buffer);
// The same CRC-64/XZ implementation disk_savefile.cpp fingerprints with, reused below for
// EK disk-variant detection rather than carrying a second CRC-64 table.
unsigned long long gdx_disk_crc64(const unsigned char* data, unsigned long long length);
// port/gdx_ek_disk_overrides.c: overwrites three garbled I8 glyphs in the translated disk's
// re-authored Create-Machine label sub-block. No-op for Course Edit.
void gdx_ek_disk_overrides_apply(void);
void gdx_leo_on_disk_loaded(const unsigned char* disk);

// 64DD boot-logo source texture (136x39 RGBA16), zero-filled by the EK asset generator at
// build time and populated from the loaded disk image by gdx_ek_assets_fill(). sys_main.c's
// func_806F33D0 CPU-blits it straight into the VI framebuffer with no RDP task, which is what
// the texel swap in gdx_disk_finalize exists for. Declared here to avoid decomp headers.
extern unsigned short D_80769DF0[];

static void gdx_dir_of(const char* path, char* outDir, size_t outSize);
static void gdx_exe_dir(char* outDir, size_t outSize);

// port/AssetLoader.cpp: copies min(fileSize, outSize) bytes of a mounted o2r entry into `out`,
// returns 1 on success.
extern "C" int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

// Frozen 64DD IPL geometry: the drive-ROM font block starts at 0xA0000 inside a 0x140000-byte
// image. The port allocates that full size and places the font block at its real offset, so every
// consumer's guard (fontAddr >= 0xA0000 && fontAddr + 0x80 <= gdx_ddipl_size) and LeoGetKAdr's
// arithmetic keep holding unchanged.
#define GDX_DDROM_FONT_START 0xA0000u
#define GDX_DDIPL_LOGICAL_SIZE 0x140000u
#define GDX_DDIPL_FONT_BLOCK_BYTES (GDX_DDIPL_LOGICAL_SIZE - GDX_DDROM_FONT_START) // 0xA0000

// The archived font slice is already normalized to big-endian by the gdx-extract `ipl` step, so
// no swap happens here. Returns false when n64ddipl.o2r is absent; the raw-file fallback carries.
static bool gdx_ddipl_load_from_archive(void) {
    unsigned char* buf = static_cast<unsigned char*>(malloc(GDX_DDIPL_LOGICAL_SIZE));
    if (buf == nullptr) {
        return false;
    }
    memset(buf, 0, GDX_DDIPL_LOGICAL_SIZE);
    size_t copied = 0;
    if (!GDiffuser_LoadArchiveFileBytes("ipl/font_block", buf + GDX_DDROM_FONT_START,
                                        GDX_DDIPL_FONT_BLOCK_BYTES, &copied) ||
        copied != GDX_DDIPL_FONT_BLOCK_BYTES) {
        free(buf);
        return false;
    }
    gdx_ddipl_buffer = buf;
    gdx_ddipl_size = GDX_DDIPL_LOGICAL_SIZE;
    gdx_port_logf("[leo] 64DD IPL font block served from n64ddipl.o2r (%u bytes at 0x%X)\n",
                  GDX_DDIPL_FONT_BLOCK_BYTES, GDX_DDROM_FONT_START);
    return true;
}

// Game.DdIplPath from gdx_firstboot.cfg. Both directories are probed because in installed mode
// the process CWD is the data dir, while the dev/portable layout keeps the cfg next to the exe.
static bool gdx_firstboot_ipl_path(char* outPath, size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    outPath[0] = '\0';
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    const char* dirs[] = { "", exeDir }; // "" == current working directory
    for (const char* dir : dirs) {
        char cfgPath[1200];
        snprintf(cfgPath, sizeof(cfgPath), "%sgdx_firstboot.cfg", dir);
        FILE* cf = fopen(cfgPath, "rb");
        if (cf == nullptr) {
            continue;
        }
        char line[2048];
        bool found = false;
        while (fgets(line, sizeof(line), cf) != nullptr) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            const char* kv = "Game.DdIplPath=";
            const size_t kvLen = strlen(kv);
            if (strncmp(line, kv, kvLen) == 0 && line[kvLen] != '\0') {
                strncpy(outPath, line + kvLen, outSize - 1);
                outPath[outSize - 1] = '\0';
                found = true;
                break;
            }
        }
        fclose(cf);
        if (found) {
            return true;
        }
    }
    return false;
}

// Tries the firstboot-recorded path first, then the known dump names next to the exe, the chosen
// ROM, and the working directory. A bare CWD-relative open silently loses the drive-ROM font
// whenever the launch CWD is not the data dir. Returns an open FILE* (caller closes) or null.
static FILE* gdx_ddipl_open_raw(char* chosenPath, size_t chosenSize) {
    if (chosenSize > 0) {
        chosenPath[0] = '\0';
    }
    char recorded[1024] = {};
    if (gdx_firstboot_ipl_path(recorded, sizeof(recorded)) && recorded[0] != '\0') {
        FILE* f = fopen(recorded, "rb");
        if (f != nullptr) {
            if (chosenSize > 0) {
                strncpy(chosenPath, recorded, chosenSize - 1);
                chosenPath[chosenSize - 1] = '\0';
            }
            return f;
        }
    }
    // Both known dump filenames are probed in each directory: a dev-tree boot has no
    // firstboot-recorded path, so this loop is the only chance to find the US-prototype dump.
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    char romDir[1024] = {};
    gdx_dir_of(gdx_rom_path, romDir, sizeof(romDir));
    const char* dirs[] = { exeDir, romDir, "" };
    const char* names[] = { "N64DDIPLROM.n64", "64DD_IPL_US_MJR.n64" };
    for (const char* dir : dirs) {
        for (const char* name : names) {
            char path[1200];
            snprintf(path, sizeof(path), "%s%s", dir, name);
            FILE* f = fopen(path, "rb");
            if (f != nullptr) {
                if (chosenSize > 0) {
                    strncpy(chosenPath, path, chosenSize - 1);
                    chosenPath[chosenSize - 1] = '\0';
                }
                return f;
            }
        }
    }
    return nullptr;
}

// Raw-file fallback. IPL dumps circulate as z64 (big-endian), v64 (16-bit swapped) or n64
// (32-bit swapped); the first byte of the PI header is 0x80 in native order, which is what the
// detection below keys off to normalize them.
static void gdx_ddipl_load_from_raw(void) {
    char chosen[1200] = {};
    FILE* f = gdx_ddipl_open_raw(chosen, sizeof(chosen));
    if (f == nullptr) {
        gdx_port_logf("[leo] no N64DDIPLROM.n64 (and no n64ddipl.o2r); drive-ROM font stays blank\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x100000) { // sanity: IPL ROM dumps are 4MB
        fclose(f);
        gdx_port_logf("[leo] %s too small (%ld bytes); ignored\n", chosen, sz);
        return;
    }
    unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
    if (buf == nullptr || fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    if (buf[0] == 0x80) {
        // native big-endian
    } else if (buf[1] == 0x80) {
        for (long i = 0; i + 1 < sz; i += 2) { // v64: swap 16-bit pairs
            unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
        }
    } else if (buf[3] == 0x80) {
        for (long i = 0; i + 3 < sz; i += 4) { // n64: reverse 32-bit words
            unsigned char t0 = buf[i], t1 = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2];
            buf[i + 2] = t1; buf[i + 3] = t0;
        }
    } else {
        gdx_port_logf("[leo] %s: unrecognized byte order (first bytes %02X %02X %02X %02X); using as-is\n",
                      chosen, buf[0], buf[1], buf[2], buf[3]);
    }
    gdx_ddipl_buffer = buf;
    gdx_ddipl_size = static_cast<unsigned int>(sz);
    gdx_port_logf("[leo] 64DD IPL ROM loaded from %s (%ld bytes)\n", chosen, sz);
}

// Archive first, so a completed setup can delete the user's IPL ROM file.
static void gdx_ddipl_load(void) {
    if (gdx_ddipl_buffer != nullptr) {
        return;
    }
    if (gdx_ddipl_load_from_archive()) {
        return;
    }
    gdx_ddipl_load_from_raw();
}

// Country code from the standard N64 header (offset 0x3E), only readable once the z64 magic
// word confirms native byte order. gdx_init_rom() does not normalize v64/n64 dumps, so those
// answer "not confidently Japanese" and keep the translated-first disk preference.
static bool gdx_rom_is_japanese(void) {
    if (gdx_rom_buffer == nullptr || gdx_rom_size < 0x40) {
        return false;
    }
    if (gdx_rom_buffer[0] != 0x80 || gdx_rom_buffer[1] != 0x37) {
        return false;
    }
    return gdx_rom_buffer[0x3E] == 'J';
}

// Directory (including trailing separator) of a file path, or "" when it has none. Both
// separators are handled: ROM paths arrive from a Windows picker or a hand-typed FZEROX_ROM.
static void gdx_dir_of(const char* path, char* outDir, size_t outSize) {
    outDir[0] = '\0';
    if (path == nullptr || path[0] == '\0' || outSize == 0) {
        return;
    }
    const char* slash = strrchr(path, '\\');
    const char* fwdSlash = strrchr(path, '/');
    if (fwdSlash != nullptr && (slash == nullptr || fwdSlash > slash)) {
        slash = fwdSlash;
    }
    if (slash == nullptr) {
        return;
    }
    size_t len = static_cast<size_t>(slash - path) + 1; // keep the separator
    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(outDir, path, len);
    outDir[len] = '\0';
}

static void gdx_exe_dir(char* outDir, size_t outSize) {
    outDir[0] = '\0';
    if (outSize == 0) {
        return;
    }
#ifdef _WIN32
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return;
    }
    slash[1] = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, outDir, static_cast<int>(outSize), nullptr, nullptr);
#else
    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0) {
        return;
    }
    exePath[n] = '\0';
    char* slash = strrchr(exePath, '/');
    if (slash == nullptr) {
        return;
    }
    slash[1] = '\0'; // keep trailing separator
    if (strlen(exePath) >= outSize) {
        return;
    }
    strcpy(outDir, exePath);
#endif
}

// Canonical retail/translated EK image size, and the .gdd save key for archive-sourced loads.
// gdx_firstboot.cpp copies every disk source to this leaf name, so an archive built from the
// managed copy replays saves under the same key the file path would have produced.
// INVARIANT: .gdd keying is by canonical leaf name, never by path.
#define GDX_DISK_EXACT_BYTES 64931840u
static const char* const kDiskArchiveSaveKey = "baserom.translated.ek.ndd";

// Deletion-gate verdict: 1 only after a boot both reconstructed the disk from fzerox-disk.o2r
// and proved SHA-256(reconstructed) == the managed-copy sha. The Data & Files panel offers
// deletion only on a passed verdict; nothing here ever deletes a user file.
static int s_diskArchiveVerified = 0;

// Loaded 64DD disk-layout variant, decided from the pristine image's CRC64 in gdx_disk_finalize.
// 0 = JP retail layout, and the safe default for an unrecognized disk since gdx_ek_assets_fill's
// JP-derived offset table serves it correctly; 1 = the fan-translated disk, whose reshaped
// Create-Machine label sub-block needs the gEkTranslatedOverrides[] re-copy. decomp-side blit
// sites (machine_create_draw.c) read this through gdx_ek_disk_is_translated() to pick the label
// textures' width/height.
static int s_ekDiskTranslated = 0;

int gdx_ek_disk_is_translated(void) {
    return s_ekDiskTranslated;
}

/* Japanese-region gate (CMake option GDX_ALLOW_JP_INPUTS, default OFF — see port/CMakeLists.txt).
   Callers must run this BEFORE installing the buffer into gdx_disk_buffer, so a refused image
   never reaches gdx_disk_finalize's consumers.

   Identity, not filename: dropping the JP name from the search list below closes the ordinary
   door, but a Japanese image renamed to the translated disk's filename would walk straight past
   a name check. The CRC-64 is the same fingerprint gdx_disk_finalize needs for its layout-variant
   decision, so the two share one pass over the image. */
static int gdx_disk_is_refused_japanese(unsigned long long pristineCrc64) {
#ifdef GDX_ALLOW_JP_INPUTS
    (void) pristineCrc64;
    return 0;
#else
    return pristineCrc64 == GDX_EK_JP_DISK_CRC64;
#endif
}

// Common post-load tail for both the archive-first and the raw-file/managed-copy branch. Requires
// gdx_disk_buffer/gdx_disk_size to already hold the PRISTINE image bytes. `diskName` keys the .gdd
// sidecar (canonical leaf name only, never a path). `pristineCrc64` is the caller's CRC-64 of those
// same bytes: the callers already compute it for the Japanese-region gate, which must run before
// the buffer is installed, so it is passed in rather than recomputed here.
static void gdx_disk_finalize(const char* diskName, const char* sourceLabel,
                              unsigned long long pristineCrc64) {
    gdx_port_logf("[leo] disk source: %s\n", sourceLabel);
    gdx_leo_on_disk_loaded(gdx_disk_buffer);
    gdx_ek_assets_fill(gdx_disk_buffer, static_cast<unsigned long long>(gdx_disk_size));
    gdx_ek_disk_overrides_apply();

    // The fill above used the retail-JP-derived offset table, correct for the JP disk and for
    // nearly all of the fan-translated one -- except the three Create-Machine label textures the
    // translator's recompile moved and/or reshaped. On the translated disk only, re-copy those
    // from their TRANSLATED offsets, overwriting what the fill just wrote. Nothing above mutates
    // the disk buffer, so the caller's pristine CRC64 is still valid here.
    {
        const unsigned long long diskCrc64 = pristineCrc64;
        if (diskCrc64 == GDX_EK_TRANSLATED_DISK_CRC64) {
            s_ekDiskTranslated = 1;
            unsigned int applied = 0;
            for (unsigned int i = 0; i < gEkTranslatedOverrideCount; i++) {
                const GdxEkTranslatedOverride& ov = gEkTranslatedOverrides[i];
                // ov.destCapacity is the destination array's real sizeof, computed at generation
                // time. Skip rather than overflow if a future disk variant reshapes a symbol
                // larger than the table's known destination budget.
                if (ov.size > ov.destCapacity) {
                    gdx_port_logf("[ek-translated] SKIP %s: size %u exceeds dest capacity %u\n",
                                  ov.name, ov.size, ov.destCapacity);
                    continue;
                }
                if (static_cast<unsigned long long>(ov.translatedDiskOffset) + ov.size <=
                    static_cast<unsigned long long>(gdx_disk_size)) {
                    memcpy(ov.dest, gdx_disk_buffer + ov.translatedDiskOffset, ov.size);
                    applied++;
                }
            }
            gdx_port_logf("[ek-translated] applied %u translated-layout overrides (disk variant: translated)\n",
                          applied);

            // Same idea for what is not a texture: the disk-status messages, Create Machine
            // prompts, driver names and Course Edit tooltips the decomp compiles in as Japanese
            // C initializers. The translated disk carries English for them in its own overlay
            // .data, so the port binds to that rather than shipping a translation of its own.
            // On the JP disk, or any unrecognized one, the compiled-in Japanese stands.
            gdx_ek_strings_apply(gdx_disk_buffer, static_cast<unsigned long long>(gdx_disk_size));
        } else if (diskCrc64 == GDX_EK_JP_DISK_CRC64) {
            s_ekDiskTranslated = 0;
            gdx_port_logf("[ek-translated] 0 translated-layout overrides applied (disk variant: JP retail -- none needed)\n");
        } else {
            s_ekDiskTranslated = 0;
            gdx_port_logf("[ek-translated] unknown variant CRC64=0x%016llX, serving JP layout\n", diskCrc64);
        }
    }

    /* The fill above copied raw BIG-ENDIAN disk bytes, but func_806F33D0 (sys_main.c) CPU-blits
       these u16s into a VI framebuffer everything else treats as host-order, and that blit is this
       texture's only consumer. Decoded-TEXTURE fixup, NOT a disk-image normalization: the disk
       buffer itself is never byte-swapped, which is why the archive can store it verbatim. */
    for (unsigned int i = 0; i < 5304; i++) {
        const unsigned short v = D_80769DF0[i];
        D_80769DF0[i] = static_cast<unsigned short>((v >> 8) | (v << 8));
    }

    gdx_ddipl_load();

    // gdx_disk_buffer is still the PRISTINE image here (everything above takes it as const or
    // patches decoded C arrays), so the fingerprint init computes is the pristine one. That
    // repeats the CRC64 the variant check already took over identical bytes; a second ~62 MB
    // table-based pass costs a few milliseconds once per boot and is not worth widening
    // gdx_disk_save_init's signature for -- each caller owning its own fingerprint is safer.
    gdx_disk_save_init(diskName, gdx_disk_buffer, gdx_disk_size);
    gdx_disk_save_apply(gdx_disk_buffer);
}

// The archive stores the disk bytes VERBATIM (the loader never byte-swaps the disk buffer -- see
// gdx_disk_finalize), so an archive-sourced image is byte-identical to a file-sourced one and the
// .gdd replay, gdx_ek_assets_fill and byte-order handling all run unchanged. Returns 1 on success
// (buffer installed and finalized), 0 to fall through to the managed-copy/raw-file search.
static int gdx_disk_load_from_archive(void) {
    unsigned char* buf = static_cast<unsigned char*>(malloc(GDX_DISK_EXACT_BYTES));
    if (buf == nullptr) {
        return 0;
    }
    size_t copied = 0;
    if (!GDiffuser_LoadArchiveFileBytes("disk/image", buf, GDX_DISK_EXACT_BYTES, &copied) ||
        copied != GDX_DISK_EXACT_BYTES) {
        free(buf);
        return 0;
    }

    // An archive built by a JP-enabled development build, or carried into a release install with
    // the rest of the game folder, must not become the loaded disk. Falling through to the file
    // search is the right recovery: a translated .ndd or managed copy beside the game still boots.
    const unsigned long long pristineCrc64 =
        gdx_disk_crc64(buf, static_cast<unsigned long long>(GDX_DISK_EXACT_BYTES));
    if (gdx_disk_is_refused_japanese(pristineCrc64)) {
        gdx_port_logf("[leo] REJECTED fzerox-disk.o2r: it holds the retail JAPANESE Expansion Kit "
                      "disk, and Japanese-region support is not enabled in this build (configure "
                      "with -DGDX_ALLOW_JP_INPUTS=ON to enable it)\n");
        free(buf);
        return 0;
    }

    // Deletion gate: SHA-256 of the reconstructed image must equal the managed-copy sha (sidecar
    // disk_sha256) before the Data & Files panel may EVER offer deletion. It has to be taken on
    // the PRISTINE bytes here, before gdx_disk_save_apply (inside finalize) replays saves over
    // them. A missing managed copy or any mismatch leaves the verdict false -- the gate never
    // suggests deletion on unproven bytes.
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    std::string archiveSha = gdx::GdxExtractSha256Bytes(buf, static_cast<unsigned long long>(GDX_DISK_EXACT_BYTES));
    std::string recorded = gdx::GdxExtractRecordedDiskSha256(exeDir);
    if (recorded.empty() && exeDir[0] != '\0') {
        char managedPath[1200];
        snprintf(managedPath, sizeof(managedPath), "%smedia/%s", exeDir, kDiskArchiveSaveKey);
        recorded = gdx::GdxExtractFileSha256(managedPath);
    }
    s_diskArchiveVerified = (!recorded.empty() && archiveSha == recorded) ? 1 : 0;
    if (s_diskArchiveVerified) {
        gdx_port_logf("[leo] disk archive verified byte-identical to the managed copy (SHA-256 %s); "
                      "original .ndd and managed copy are deletable\n",
                      archiveSha.c_str());
    } else {
        gdx_port_logf("[leo] disk archive NOT verified (reconstructed %s vs recorded %s); deletion "
                      "stays gated\n",
                      archiveSha.empty() ? "(none)" : archiveSha.c_str(),
                      recorded.empty() ? "(none)" : recorded.c_str());
    }

    gdx_disk_buffer = buf;
    gdx_disk_size = GDX_DISK_EXACT_BYTES;
    gdx_port_logf("[leo] disk image reconstructed from fzerox-disk.o2r (%u bytes)\n", GDX_DISK_EXACT_BYTES);
    gdx_disk_finalize(kDiskArchiveSaveKey, "archive", pristineCrc64);
    return 1;
}

int gdx_disk_archive_verified(void) {
    return s_diskArchiveVerified;
}

int gdx_disk_load(void) {
    if (gdx_disk_buffer != nullptr) {
        return 1;
    }

    if (gdx_disk_load_from_archive()) {
        return 1;
    }

    // The fan-translated English disk (LuigiBlood/Zoinkity, 64DD.org) keeps the retail disk
    // layout, so loading it instead of the JP dump delivers English EK text/font assets through
    // the same offsets and MFS reads, with no extraction step. It is therefore the default, and
    // the fallback whenever the loaded ROM's region cannot be determined. A confirmed Japanese
    // cartridge prefers the untranslated JP disk so on-disk text matches the cartridge.
    const bool jpRom = gdx_rom_is_japanese();
#ifdef GDX_ALLOW_JP_INPUTS
    static const char* const kUsPreferredNames[] = {
        "baserom.translated.ek.ndd",
        "baserom.translated.ek.nnd", // common transposed extension in the wild
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
    };
    static const char* const kJpPreferredNames[] = {
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
        "baserom.translated.ek.ndd",
        "baserom.translated.ek.nnd",
    };
    const char* const* diskNames = jpRom ? kJpPreferredNames : kUsPreferredNames;
    const size_t diskNameCount = 4;
#else
    /* Japanese-region gate, cheap half: the Japanese disk FILENAMES are not searched at all, so a
       .ndd dropped beside the executable under its usual JP name is never opened (no read, no
       62 MB CRC pass, no log noise). The authoritative half is the CRC-64 identity check below.
       `jpRom` stays in use for the provenance log line further down. */
    static const char* const kTranslatedOnlyNames[] = {
        "baserom.translated.ek.ndd",
        "baserom.translated.ek.nnd",
    };
    const char* const* diskNames = kTranslatedOnlyNames;
    const size_t diskNameCount = 2;
#endif

    // Search order. The managed copy under <exeDir>/media wins: gdx_firstboot.cpp creates it
    // byte-identically from the user's original under the SAME canonical leaf name, so the .gdd
    // save key (leaf name only, never a path) is unaffected by preferring it. Next to the chosen
    // ROM comes second so separate install folders with different .ndd files never
    // cross-pollinate, then the exe directory, then the working directory as a last resort for
    // scripted/dev launches that rely on CWD.
    char romDir[1024] = {};
    gdx_dir_of(gdx_rom_path, romDir, sizeof(romDir));
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    char mediaDir[1040] = {};
    if (exeDir[0] != '\0') {
        snprintf(mediaDir, sizeof(mediaDir), "%smedia/", exeDir);
    }

    struct SearchLocation {
        const char* dir;
        const char* why;
        bool managed;
    };
    const SearchLocation searchLocations[] = {
        { mediaDir, "managed copy (media/)", true },
        { romDir, "next to chosen ROM", false },
        { exeDir, "exe directory", false },
        { "", "current directory", false },
    };

    for (const SearchLocation& loc : searchLocations) {
        for (size_t i = 0; i < diskNameCount; i++) {
            char path[1200];
            snprintf(path, sizeof(path), "%s%s", loc.dir, diskNames[i]);

            FILE* f = fopen(path, "rb");
            if (f == nullptr) {
                continue;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                fclose(f);
                continue;
            }
            /* gdx_leo_on_disk_loaded and LeoReadDiskID read the LBA-14 disk ID and system area
               at fixed offsets with no size guard, and gdx_ek_assets_fill walks generated
               diskOffset tables, so a truncated .ndd OOB-reads immediately. Retail 64DD images
               are ~64.9 MB. */
            if (sz < (long)(60u * 1024u * 1024u)) {
                gdx_port_logf("[leo] REJECTED %s: %ld bytes (truncated .ndd image, need ~64.9 MB)\n", path, sz);
                fclose(f);
                continue;
            }
            unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
            if (buf == nullptr) {
                fclose(f);
                return 0;
            }
            if (fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
                free(buf);
                fclose(f);
                continue;
            }
            fclose(f);
            /* Identity gate, before the buffer is installed: catches a Japanese image renamed to
               a filename this build does search. Continue the search rather than failing
               outright, so a translated disk in a later location still wins. */
            const unsigned long long pristineCrc64 =
                gdx_disk_crc64(buf, static_cast<unsigned long long>(sz));
            if (gdx_disk_is_refused_japanese(pristineCrc64)) {
                gdx_port_logf("[leo] REJECTED %s: this is the retail JAPANESE Expansion Kit disk, "
                              "and Japanese-region support is not enabled in this build (configure "
                              "with -DGDX_ALLOW_JP_INPUTS=ON to enable it)\n", path);
                free(buf);
                continue;
            }
            gdx_disk_buffer = buf;
            gdx_disk_size = static_cast<unsigned int>(sz);
            gdx_port_logf("[leo] disk image loaded: %s (%ld bytes) -- picked from %s, region=%s\n", path, sz,
                          loc.why, jpRom ? "JP (matches cartridge ROM)" : "US/unknown (default preference)");
            // No archive reconstruction this boot, so the deletion gate stays unproven.
            gdx_disk_finalize(diskNames[i], loc.managed ? "managed" : "original", pristineCrc64);
            return 1;
        }
    }

    static bool sWarned = false;
    if (!sWarned) {
        sWarned = true;
        gdx_port_logf("[leo] no disk image found; Expansion Kit modes stay disabled\n");
    }
    return 0;
}
}
