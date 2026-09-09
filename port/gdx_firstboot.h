// G-Diffuser — first-boot setup + portable data-directory resolution.
//
// Runs once at the very top of main(), before any libultraship path resolution, so that:
//   * a true development tree with assets/extracted/generic.o2r may boot headlessly with its ROM;
//   * every portable install uses the executable directory for data and runs the in-window setup on
//     its first launch, even when the user already placed all three canonical inputs beside the game.
//
// Known limits: generic.o2r generation and Windows save-path redirection are NOT handled here.
#pragma once

#include <string>

namespace gdx {

enum class FirstBootStatus {
    DevLayout,      // A development tree supplies generic.o2r, a ROM, AND a valid EK disk + IPL, so
                    // boot skips the wizard. G-Diffuser is Expansion-Kit-mandatory (the game
                    // crashes/degrades without the 64DD disk), so a dev tree missing either of those
                    // falls through to NeedsSetup rather than bypassing the wizard.
    SetupComplete,  // Setup verified or freshly completed. Boot with the configured paths.
    NeedsSetup,     // No dev/completed layout. exeDir/dataDir are resolved and the working directory is
                    // set, but the ROM/EK disk/IPL are missing: the caller must run the IN-WINDOW setup
                    // flow (port/gdx_firstboot_gui.{h,cpp}) after the window/Gui/FileDropMgr exist.
    Aborted,        // Reserved: a hard, non-recoverable setup failure. The caller should exit cleanly.
};

struct FirstBootResult {
    FirstBootStatus status = FirstBootStatus::DevLayout;
    // Absolute path to the ROM the caller should load. Empty means "let the existing loader decide"
    // (its own picker/env/next-to-exe fallbacks). When non-empty, main() injects it as a synthetic
    // argv entry so rom_buffer.cpp loads it via its CLI-arg branch and never opens its own picker.
    std::string romPath;
    // Absolute path to the validated EK disk / IPL ROM found on a DevLayout boot (informational —
    // disk_buffer.cpp / the leo emulation already resolve these by canonical name relative to the
    // chosen ROM/exe dir/CWD; these fields just record what FirstBootRun verified is there). Empty
    // on every other status: NeedsSetup defers acquisition to the in-window wizard, which records the
    // final installed paths itself via WriteSetupComplete.
    std::string diskPath;
    std::string iplPath;
    std::string dataDir;        // Resolved data directory (informational / logging).
    // Directory the executable lives in. The runtime O2R extractor (gdx_extract_launch) locates the
    // packaged gdx-extract child + decomp-recipes here, which — in installed/portable mode — is NOT
    // the same as dataDir (the extractor reads recipes from exeDir but writes generic.o2r to dataDir).
    std::string exeDir;
    bool chdirApplied = false;  // True when the working directory was moved to dataDir (installed mode).
};

// argv0 is argv[0] (used only for exe-directory fallback when the OS query fails).
FirstBootResult FirstBootRun(const char* argv0);

// True when either supplied path is inside a source tree that already provides
// assets/extracted/generic.o2r. Portable release folders must return false.
bool DevelopmentTreeProvidesArchive(const std::string& exeDir, const std::string& cwd);

// ── Shared setup helpers (reused by the in-window GUI setup flow) ─────────────────────────────────
// These expose the canonical file names, structural validators, copy/persist semantics, and the
// native file picker so port/gdx_firstboot_gui.cpp can drive the exact same acquisition rules the
// old blocking wizard used, but from an ImGui screen. All paths are UTF-8 std::string.

// Canonical on-disk names inside the data directory (what the stock loaders search for).
const char* SetupRomFileName();   // "baserom.us.rev0.z64"
const char* SetupDiskFileName();  // "baserom.translated.ek.ndd"
const char* SetupIplFileName();   // "N64DDIPLROM.n64"
// Accepted alternate names for the Japanese dumps: the wizard probes these when the canonical name
// is absent, so a JP test folder needs no renaming. A JP ROM boots RAW (experimental; no archives).
//
// They keep returning their real names even when the build does NOT accept Japanese inputs (CMake
// GDX_ALLOW_JP_INPUTS=OFF, the release default), because the probe is what lets the wizard FIND the
// file and then refuse it by name — "this is the Japanese release, not enabled in this build" —
// instead of leaving the row on a bare "Missing". The refusal itself is decided in GdxRecognizeInput,
// by hash and by ROM-header country code, never by filename.
const char* SetupRomFileNameJp();  // "baserom.jp.rev0.z64"
const char* SetupDiskFileNameJp(); // "baserom.jp.ek.ndd"
// Accepted alternate names for the PAL dumps: the wizard probes these when the canonical name is
// absent, so a PAL test folder needs no renaming. PAL extraction uses the pal/rev0 recipe tree and
// produces fzerox-pal.o2r.
const char* SetupRomFileNamePal();    // "baserom.pal.rev0.z64"
const char* SetupRomFileNameEuPal();  // "baserom.eu.rev0.z64"
// Accepted alternate name for the US prototype 64DD IPL dump, probed when N64DDIPLROM.n64 is absent
// so a folder holding it under its original filename needs no renaming. See kKnownIplDumps in
// gdx_firstboot.cpp for the recognized SHA-1/label.
const char* SetupIplFileNameUsProto(); // "64DD_IPL_US_MJR.n64"

// Resolve the 64DD IPL ROM source inside `dir`: the canonical name (N64DDIPLROM.n64) first, then the
// accepted US-prototype alternate (64DD_IPL_US_MJR.n64). Existence probe only -- callers that need
// structure/size still call ValidateIplFile themselves. Returns whichever name matched, or empty when
// neither exists.
//
// Shared by the setup GUI's Recheck(), FirstBootRun's dev-layout and SetupComplete probes, and
// gdx_extract_launch.cpp's ensureIplArchive, so the accepted alternate name cannot drift between the
// three surfaces -- a boot-time probe that knows only the canonical name boots with no IPL archive
// and no warning.
std::string GdxFindIplSourceInDir(const std::string& dir);

// Structural validators. Return true if the file at `path` is a plausible input; on false, `why`
// receives a short human-readable reason (region/size/magic mismatch). A non-existent file is
// reported as invalid.
bool ValidateRomFile(const std::string& path, std::string& why);
bool ValidateDiskFile(const std::string& path, std::string& why);
bool ValidateIplFile(const std::string& path, std::string& why);

// Copy `srcPath` into `dataDir`/`dstName`, overwriting any existing file. Returns true on success.
bool CopyInputInto(const std::string& srcPath, const std::string& dataDir, const char* dstName);

// ── SHA-1 identity recognition (region/dump labelling for the setup rows) ──────────────────────────
// The known-good SHA-1 sets live as named tables in gdx_firstboot.cpp so the future JP build can
// reuse them. GdxRecognizeInput hashes a file that has ALREADY passed its structural Validate*File
// check and classifies it:
//   * ROM  — the US-rev0 dump is VerifiedKnown; the PAL-rev0 dump is VerifiedKnown (experimental). The
//            Japan dump is ACCEPTED (AcceptedUnknownWarn with `jpRom` set) for the experimental
//            raw-ROM boot — setup then SKIPS archive extraction for it. Any other hash is Rejected
//            with the generic mismatch message.
//   * IPL  — each known dump (JP retail, US prototype) is labelled by region (VerifiedKnown); every
//            other correctly-sized dump is AcceptedUnknownWarn (accepted, but the caller must surface
//            the warning text visibly).
//   * disk — each known dump is labelled by region (VerifiedKnown); any other correctly-sized image is
//            AcceptedUnknownWarn. The size gate stays a hard reject inside ValidateDiskFile.
//
// JAPANESE-REGION GATE (CMake option GDX_ALLOW_JP_INPUTS, default OFF — see port/CMakeLists.txt).
// With the option OFF this function REJECTS Japanese game data at this validation layer, which is
// the single place the wizard, the drag & drop handler and the Browse… picker all funnel through:
//   * ROM  — the Japanese dump is Rejected by SHA-1, and ANY other image whose N64 header country
//            code is 'J' is Rejected too, so an uncatalogued Japanese dump is still named as
//            Japanese rather than reported as an unrecognised file.
//   * disk — the retail Japanese Expansion Kit disk is Rejected by SHA-1. Unrecognised images are
//            unaffected (a 64DD image carries no region marker that separates the fan-translated
//            disk from the Japanese one it was built from).
//   * IPL  — NOT gated in either state. The 64DD shipped only in Japan, so the JP retail dump IS the
//            canonical firmware, and it carries no game audio — refusing it would break every user
//            for no safety benefit.
// With the option ON, none of the refusals above apply.
enum class GdxInputVerdict {
    VerifiedKnown,        // recognized known-good dump — OK/green; `message` is a confirmation label
    AcceptedUnknownWarn,  // structurally valid but unrecognized — OK/green; `message` is a visible warning
    Rejected,             // rejected — red; `message` is the reason
};
struct GdxInputRecognition {
    GdxInputVerdict verdict = GdxInputVerdict::VerifiedKnown;
    std::string sha1;     // lowercase hex (empty on read failure — then verdict is Rejected)
    std::string message;  // display text per verdict (label / warning / reason)
    // For a recognized Expansion Kit disk, the region label ("translated Expansion Kit disk" /
    // "retail Japanese Expansion Kit disk") the OK row header should show in place of the file name.
    // Also carries the ROM row's "OK (F-Zero X (Japan) — experimental)" header for an accepted JP
    // dump. Empty for every other input and verdict.
    std::string okHeaderOverride;
    // True only for the accepted Japanese ROM: the caller must SKIP archive extraction (the US
    // recipe tree cannot extract a JP ROM) and complete setup for the raw-ROM boot instead.
    bool jpRom = false;
};
// Recognize a reviewed input by SHA-1. `canonicalName` selects the ruleset (pass SetupRomFileName() /
// SetupIplFileName() / SetupDiskFileName()); `exeDir` supplies the recipe-authoritative US ROM hash.
GdxInputRecognition GdxRecognizeInput(const std::string& canonicalName, const std::string& path,
                                      const std::string& exeDir);

// Canonical installed archive names (what a validated archive satisfies which input). Exposed so the
// in-window setup rows can truthfully report a requirement met by its archive when the original file
// is gone: originals are deletable once the archive covers them.
const char* SetupGameArchiveFileName();  // "fzerox.o2r"    — satisfies the ROM input
const char* SetupIplArchiveFileName();   // "n64ddipl.o2r"  — satisfies the 64DD IPL input
const char* SetupDiskArchiveFileName();  // "fzerox-disk.o2r" — satisfies the EK disk input

// ── Managed disk copy (disk internalization) ───────────────────────────────────────────────────
// A byte-identical copy of the validated Expansion Kit disk under <dataDir>/media/<canonical disk
// name>, so the user's original .ndd becomes deletable after setup like the ROM/IPL. It lives in a
// subdirectory SEPARATE from dataDir's root so a genuine second copy exists even when the root-level
// file (the wizard's CopyInputInto destination, or the DevLayout candidate itself) is the user's only
// other copy. port/disk_savefile.cpp keys its .gdd journal off the disk's LEAF NAME ONLY, never a
// path (gdx_disk_load passes the bare canonical name), so the managed copy must keep that same leaf
// name or the existing save key changes.

// Absolute path of the managed disk copy inside `dataDir` (<dataDir>/media/<disk name>). Pure path
// computation -- does not touch the filesystem.
std::string ManagedDiskPath(const std::string& dataDir);

// `validatedDiskPath` MUST already have passed ValidateDiskFile. Idempotent: a managed copy already
// present and correctly sized is left untouched, never re-copied or overwritten. The copy is
// atomic-ish (temp name in the same directory, size verified, then renamed into place) and its
// SHA-256 is best-effort recorded in gdx_extract_state.cfg via GdxExtractRecordManagedDisk. True iff
// a valid managed copy exists at `outManagedPath` on return; on false `outManagedPath` still holds
// the computed path, but the file may be missing or incomplete and callers must not use it.
bool EnsureManagedDiskCopy(const std::string& dataDir, const std::string& validatedDiskPath,
                           std::string& outManagedPath);

// Write the completion marker (Setup.Complete=1) plus the recorded input paths into the state file
// (gdx_firstboot.cfg) in `dataDir`. Returns true on success (a failure only means setup re-runs).
bool WriteSetupComplete(const std::string& dataDir, const std::string& romPath,
                        const std::string& diskPath, const std::string& iplPath);

// ── Missing-input diagnostic ───────────────────────────────────────────────────────────────────
// Read-only summary of which canonical inputs (ROM / 64DD IPL ROM / Expansion Kit disk) and which
// game archive are missing or invalid under `dataDir`, built from the same helpers this header
// already exports so it opens no new probe surface and never throws. Empty string when everything
// needed to boot is present and valid.
//
// NO CALLER TODAY. Intended consumer is main.cpp's no-ROM boot error path; this produces only the
// user-presentable text, and neither the decision to fail the boot nor the surface that shows it
// exists yet.
std::string GdxFirstBootDescribeMissing(const std::string& dataDir);

// ── Exported archive-satisfied checks (shared with the in-window wizard's Recheck) ────────────────
// The wizard's Recheck() must judge "satisfied by its INSTALLED ARCHIVE" through the same
// hash-validated chain FirstBootRun's SetupComplete fast path uses, not a bare existence probe that
// would accept a corrupt/foreign archive. gameArchiveSatisfies / iplArchiveSatisfies /
// diskArchiveSatisfies stay file-local in gdx_firstboot.cpp; this is a thin wrapper around them.
enum class GdxFirstbootArchiveKind { Game, Ipl, Disk };

// True when the canonical archive for `kind` is installed under `dataDir` and, when this build
// recorded a golden hash for it, hashes to the recorded value -- the same acceptance chain
// FirstBootRun's SetupComplete fast path uses. A failing hash quarantines the archive exactly like the
// fast path does (renamed <name>.bad).
//
// A PASSING result latches gdx_extract_launch's per-boot archive-validation latch
// (GdxExtractMarkArchiveValidated), so a later same-boot re-check of the same kind -- another call
// here, or GdxExtractEnsureArchive's own warm-boot check -- skips the re-hash of a multi-MB file.
// There is no caching short of that latch: Recheck() is event-driven (constructor + file-drop +
// Browse click), never per-frame. A future per-frame caller MUST memoize on (kind, mtime, size)
// first -- the underlying hash is not free.
bool GdxFirstbootArchiveSatisfies(GdxFirstbootArchiveKind kind, const std::string& dataDir);

// True when a native "Browse…" file picker is available on this platform (Windows only). On other
// platforms the GUI relies exclusively on drag & drop (no native picker in this port).
bool NativeFilePickerAvailable();

// Native file-open dialogs (Windows). Return the selected absolute path, or empty if the user
// cancelled or the platform has no native picker. Each preselects an appropriate file filter.
std::string PickRomFile();
std::string PickDiskFile();
std::string PickIplFile();

} // namespace gdx
