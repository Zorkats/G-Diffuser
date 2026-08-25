// port/gdx_workshop.h — Workshop texture-pack + dump support (docs/MODDING_GUIDE.md).
//
// Every behavior is opt-in through CVars that default OFF, so a fresh gdiffuser.cfg.json renders
// byte-identically to stock:
//   - gEnhancements.Workshop.TexturePacks (int, 0): master switch for the Tier-B override shim.
//   - gEnhancements.Workshop.TextureDump  (int, 0): dump every decoded texture to dump/<key>.png.
//   - gEnhancements.Workshop.DisabledPacks (string, ""): comma-joined ids/basenames of mods/*.o2r
//     packs to skip (id match first, basename fallback).
//   - gEnhancements.Workshop.PackOrder (string, ""): comma-joined ids/basenames in mount-priority
//     order; unlisted packs mount after, alphabetical by basename.
//
// main.cpp::findArchivePaths mounts mods/*.o2r (a pack with no matching keys is inert); this module
// owns the runtime override lookup, the decoded-texture dump, and the hot reload.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Tier-B texture-pack override shim (called from n64_gfx_bridge.cpp) ────────────────────────────
// Returns non-zero when gEnhancements.Workshop.TexturePacks is on. Cheap CVar read; the expensive
// ResourceManager existence check is cached in GdxWorkshopLookupOverridePath.
int gdx_workshop_texture_packs_enabled(void);

// Given an asset key (e.g. "common_assets_compressed/aFont3ATex"), returns a process-stable
// C-string of the override resource path ("textures/pack/<key>") IF a mounted pack provides it,
// else NULL. Result is cached per key per pack epoch: the ResourceManager is consulted at most once
// per key between reloads. The returned pointer stays valid until the next GdxWorkshopReload().
const char* GdxWorkshopLookupOverridePath(const char* key);

// RDRAM whole-image variant (issue #27): like GdxWorkshopLookupOverridePath, but additionally
// requires the pack payload to cover at least minBytes — the registered RDRAM buffer's full
// size. A smaller payload is a single atlas band, not a whole-image replacement, so the call
// returns NULL and the texture keeps the raw-copy path (per-tile atlas keys remain the way to
// override those). Payload sizes are cached per path per pack epoch like the existence probes.
const char* GdxWorkshopLookupOverridePathMinSize(const char* key, size_t minBytes);

// Per-tile atlas variant: builds the scheme-2 key "atlas/<baseKey>/o<byteOffset>/<FMT>/<W>x<H>"
// for one band of a multi-tile atlas buffer (fonts, glyph sheets, machine-name strips) and returns
// its override path under the same cache/epoch rules as GdxWorkshopLookupOverridePath. baseKey is
// the containing atlas asset key (GDiffuser_LookupLoadedAssetKeyContaining); byteOffset is the
// band's offset within that buffer, already baked into the load address by the interpreter's
// LoadBlock/LoadTile row/column math. Same master switch applies; callers gate on
// gdx_workshop_texture_packs_enabled() first.
const char* GdxWorkshopLookupAtlasTileOverride(const char* baseKey, size_t byteOffset, int n64Fmt, int n64Siz,
                                               int width, int height);

// ── Texture dump (called from the interpreter's decoded-RGBA32 points) ────────────────────────────
// Returns non-zero when gEnhancements.Workshop.TextureDump is on.
int gdx_workshop_texture_dump_enabled(void);

// Number of distinct textures dumped this session (for the menu status line).
int gdx_workshop_dump_count(void);

// Adds or removes a pack from gEnhancements.Workshop.DisabledPacks (the comma-joined disable list)
// and persists it. disabled != 0 => the pack is skipped at the next mount/reload. The stored token
// is the id when the pack declares one (pass id="" when absent), else the basename; removal matches
// either key so old basename entries are cleaned up.
void GdxWorkshopSetPackDisabled(const char* id, const char* basename, int disabled);

// Replaces gEnhancements.Workshop.PackOrder (comma-joined ids/basenames in mount-priority order)
// and persists it. The Workshop menu's up/down buttons write the full row order through this.
void GdxWorkshopSetPackOrder(const char* joinedOrder);

// Dump one fully-decoded texture. First-seen-wins per session (a key/hash is written at most once).
// Writes dump/<identity>.png + appends dump/manifest.tsv (key, w, h, fmt). OFF the hot path: the
// caller must gate on gdx_workshop_texture_dump_enabled() first.
//   origSrcAddr/origSrcLen : the pre-decode RDRAM/source bytes, used to recover the asset key via
//                            the loaded-asset registry, and to hash unnamed textures.
//   resourcePathOrNull     : the OTR resource path when the texture came from an .o2r (Tier A/B),
//                            else NULL.
//   rgba32                 : width*height*4 decoded RGBA32 bytes.
//   n64Fmt/n64Siz          : the N64 tile format/size the game used (for the manifest fmt column).
void gdx_workshop_dump_texture(const void* origSrcAddr, size_t origSrcLen, const char* resourcePathOrNull,
                               const uint8_t* rgba32, int width, int height, int n64Fmt, int n64Siz);

// ── Hot reload (called from the Workshop menu) ────────────────────────────────────────────────────
// Re-scans mods/, evicts cached pack resources, clears the interpreter texture cache, and bumps the
// pack epoch (invalidating the override cache). Writes a human-readable one-line result into
// outStatus (a caller-owned buffer of outStatusLen bytes); safe to pass NULL/0.
void GdxWorkshopReload(char* outStatus, size_t outStatusLen);

#ifdef __cplusplus
} // extern "C"

#include <string>
#include <vector>

// ── Menu-facing pack listing (C++ only) ───────────────────────────────────────────────────────────
struct GdxWorkshopPackInfo {
    std::string basename;   // e.g. "10-hifonts.o2r"
    std::string path;       // absolute path on disk
    bool disabled;          // present in gEnhancements.Workshop.DisabledPacks (by id or basename)
    // Manifest fields (best-effort from the pack's OWN archive; empty when it has no readable
    // workshop.json/manifest.json).
    std::string id;         // optional stable identity; the disable/order lists prefer it over basename
    std::string name;
    std::string version;
    std::string author;
    std::string gameVersion;
    std::string keySchemeVersion;
    std::vector<std::string> depends;    // optional "depends" array (ids/basenames)
    std::vector<std::string> conflicts;  // optional "conflicts" array (ids/basenames)
    // Derived warnings (WARN ONLY; mounting is never blocked).
    std::vector<std::string> missingDepends;   // depended id/basename with no enabled pack providing it
    std::vector<std::string> activeConflicts;  // conflicting id/basename provided by an enabled pack
    bool gameVersionMismatch;      // manifest.game_version present and != the port's build version
    bool keySchemeMismatch;        // manifest.key_scheme_version present and != the expected scheme
    bool manifestPresent;

    // Disable-list / PackOrder key: the manifest id when the pack declares one, else the basename.
    std::string Identity() const {
        return id.empty() ? basename : id;
    }
};

// Scans the active mods/ directory on disk and returns one entry per *.o2r found (enabled or not),
// in effective mount order: gEnhancements.Workshop.PackOrder first, then alphabetical by basename.
std::vector<GdxWorkshopPackInfo> GdxWorkshopListPacks();

// Boot mount path (main.cpp::findArchivePaths): given absolute paths to candidate mods/*.o2r,
// returns the enabled subset in effective mount order (same dual-key disable match and PackOrder
// permutation as GdxWorkshopListPacks/the reload path). When outDisabledBasenames is non-null it
// receives the basenames of skipped packs, for logging. Safe to call before ResourceManager init.
std::vector<std::string> GdxWorkshopOrderModPaths(const std::vector<std::string>& absPaths,
                                                  std::vector<std::string>* outDisabledBasenames);

// Total number of texture-pack override resources visible across all mounted archives
// ("textures/pack/*"). Cheap VFS list; refreshed on demand.
int GdxWorkshopOverrideCount();

// Resolves the active mods/ directory (creating it if missing). Returns empty string on failure.
std::string GdxWorkshopModsDir(bool createIfMissing);

// Resolves the dump/ directory (creating it if missing). Returns empty string on failure.
std::string GdxWorkshopDumpDir(bool createIfMissing);

// The expected key-scheme version this build packs/reads against. A pack manifest declaring a
// different key_scheme_version is flagged in the menu (symbol renames are a breaking change).
// Scheme "2" adds the atlas/<baseKey>/o<byteOffset>/<FMT>/<W>x<H> per-tile band keys; single-image
// keys are byte-identical between "1" and "2", so scheme-1 packs keep working.
extern const char* kGdxWorkshopKeySchemeVersion;

#endif // __cplusplus
