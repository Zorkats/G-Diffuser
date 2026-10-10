// G-Diffuser — runtime O2R asset extraction launcher.
//
// Produces (or refreshes) <dataDir>/generic.o2r from the cartridge ROM by spawning the packaged
// `gdx-extract` child against the shipped `decomp-recipes` tree. An installed/packaged build has NO
// generic.o2r until this runs.
//
// Invariants:
//   * ROM SHA-1 is validated BEFORE the child is spawned (expected hash from
//     decomp-recipes/config.yml).
//   * Nothing installs before it validates: exit code, zip entry count, archive SHA-256, version
//     entry — then an atomic temp->rename with a Windows sharing-violation retry loop.
//   * Complete-or-absent: extraction NEVER blocks boot. On any failure the temp is deleted, any
//     previous archive is preserved, and the raw-ROM fallback carries the session.
//   * State model: completion sidecar gdx_extract_state.cfg + warm-boot skip.
//   * Called from main.cpp before the archive mount list is built; progress UX is a Win32 modeless
//     dialog on Windows and log-only on Linux.
//
// Part of the G-Diffuser exe target (not the decomp game library), so it may freely use the host CRT,
// <filesystem>, and (Windows) the Win32 process + common-controls APIs. It runs before libultraship
// is constructed, so it logs through the port's own gdx_port_logf and touches no LUS state.
#pragma once

#include <string>
#include <vector>

namespace gdx {

// The caller (main.cpp) only logs this; boot proceeds regardless. FailedRawFallback is deliberately
// the single catch-all failure value, because the boot posture is identical for every failure mode:
// no valid generic.o2r produced → raw-ROM fallback.
enum class ExtractOutcome {
    UpToDate,          // A valid (golden) generic.o2r is already present; nothing to do.
    Extracted,         // The extractor ran, its output validated, and it was atomically installed.
    FailedRawFallback, // Extraction was not performed or did not validate; boot degrades to raw ROM.
};

// Ensure a valid <dataDir>/generic.o2r exists, extracting it from the cartridge ROM if needed.
//
//   dataDir  Absolute path to the writable data directory (the extractor's output dir and the sidecar
//            location). MUST be passed explicitly — never rely on the inherited CWD.
//   romPath  Absolute path to the validated cartridge ROM (as installed/injected by first-boot).
//   exeDir   Absolute path to the executable's directory (where gdx-extract + decomp-recipes ship).
//
// Never throws; every failure path returns FailedRawFallback with an actionable gdx_port_logf line.
// The caller MUST skip this entirely in the dev/portable layout (FirstBootStatus::DevLayout), where
// the in-tree assets/extracted probe already provides generic.o2r.
ExtractOutcome GdxExtractEnsureArchive(const char* dataDir, const char* romPath, const char* exeDir);

// Human-readable label for logging.
const char* GdxExtractOutcomeString(ExtractOutcome outcome);

// -- ROM-hack archive build ----------------------------------------------------------------------
// Builds <hackDir>/<name>.o2r from a PATCHED ROM by running the packaged extractor against the stock
// recipe tree, named explicitly through gdx-extract's --config-key because a hack hashes to something
// decomp-recipes/config.yml does not describe.
//
// Deliberately weaker than the cartridge path, and it says so. A hack can never have a golden archive
// to compare against, because nobody has produced a verified one. The zip entry count IS still
// meaningful, since the recipe tree fixes it regardless of what the ROM contains, and it is reported;
// it does not veto the install. Asset CONTENT cannot be checked at all: an asset read from an offset
// the hack repurposed is structurally valid and visually wrong, and only running it reveals that.
//
// Scope limit the UI must state: G-Diffuser executes decompiled game code, so the archive carries the
// hack's ASSETS and COURSES. Code the hack patched is never executed.
struct HackBuildResult {
    bool ok = false;
    std::string archivePath;     // installed archive path; set only when ok
    std::string message;         // human-readable outcome; always set, on success and failure
    long entryCount = -1;        // zip records in the produced archive (-1 = unknown)
    long expectedEntryCount = 0; // the stock archive's record count (0 = no golden was generated)
};

// romPath   Absolute path to the patched ROM.
// hackName  Display name; sanitised into the archive basename with gdx_hackmod_sanitize_name.
// hackDir   Absolute path to mods/~romhacks: the install target, and host of the temp staging dir.
// exeDir    Absolute path to the directory shipping gdx-extract and decomp-recipes.
//
// Never throws. Complete-or-absent: on any failure nothing is installed and the temp dir is removed.
HackBuildResult GdxExtractBuildHackArchive(const char* romPath, const char* hackName, const char* hackDir,
                                           const char* exeDir);

// ── Async driver for the in-window setup GUI ─────────────────────────────────────────────────────
// GdxExtractEnsureArchive blocks (normally ~2s, up to a 120s hang deadline), but the ImGui setup
// screen must keep pumping frames, so this wrapper runs it on a background thread behind a pollable
// snapshot. Single-flight: only one async extraction may be in progress.

enum class ExtractPhase {
    Idle,     // No async extraction has been started (or it was reset).
    Running,  // The background worker is executing.
    Done,     // The worker finished; `outcome` is valid.
};

struct ExtractProgress {
    ExtractPhase phase = ExtractPhase::Idle;
    ExtractOutcome outcome = ExtractOutcome::FailedRawFallback; // valid only when phase == Done
    std::string stage;      // latest stage line captured from the extractor (may be empty)
    std::string lastError;  // last actionable error line (valid on a failed Done; may be empty)

    // ── Log view + real progress (setup-GUI progress bar/log region) ────────────────────────────────
    std::vector<std::string> log; // ring-buffer snapshot of the extractor's last ~200 stdout lines
                                   // (oldest first). Reset to empty whenever a new async extraction
                                   // starts (GdxExtractStartAsync). May be shorter early in a run.
    int entriesSeen = 0;          // "- [type] Processing <name>" lines seen so far in the CURRENT
                                   // sub-stage. Torch emits one per asset node it visits
                                   // (torch/src/Companion.cpp ParseNode), so this tracks progress
                                   // against GdxExtractExpectedCartEntryCount() /
                                   // GdxExtractExpectedIplEntryCount(). Approximate -- a node that
                                   // finds no exporter still logs the line but writes no entry -- but
                                   // monotonic, and it reaches the expected total on a successful run.
                                   // Reset to 0 at the start of each sub-stage.
    int subStage = 0;             // 0 = extracting the cartridge archive, 1 = validating it (hash/entry
                                   // count gates, after the extractor child has exited), 2 = extracting
                                   // the IPL font-block archive. Monotonic within one async run; stays
                                   // 0 for a run that never reaches the later stages.
};

// Start GdxExtractEnsureArchive on a background thread. `suppressNativeDialog` (pass true from the
// GUI) suppresses the Windows Win32 marquee progress dialog so the ImGui screen owns the progress UX.
// A no-op (logs a warning) if an async extraction is already Running.
void GdxExtractStartAsync(const char* dataDir, const char* romPath, const char* exeDir,
                          bool suppressNativeDialog);

// Snapshot the current async state. Safe to call every frame.
ExtractProgress GdxExtractPollStatus();

// Join the finished worker and reset back to Idle. Call once after handling a Done result (before a
// retry, or when leaving the setup flow). Safe to call when already Idle.
void GdxExtractResetAsync();

// The denominator for a determinate progress bar during the cart sub-stage
// (GDX_O2R_EXPECTED_ENTRY_COUNT, the US-rev0 golden's entry count). Returns 0 when the golden header
// was never generated (gen/gdx_o2r_expected.h missing, placeholder build), and callers must fall back
// to an indeterminate bar.
int GdxExtractExpectedCartEntryCount();

// Frozen at 2 (ipl/font_block + ipl/identity), so always > 0. A function rather than a header
// constant so gdx_extract_launch.cpp stays the single source of truth for the value.
int GdxExtractExpectedIplEntryCount();

// ── ROM identity helpers for the setup GUI ───────────────────────────────────────────────────────
// The setup screen shows the user their ROM's SHA-1 against the expected US-rev0 hash so a wrong
// dump is diagnosed at acquisition time rather than at extraction time.

// Lowercase-hex SHA-1 of the file at `path` (empty string on read failure). ~50 ms for a 16 MiB ROM.
std::string GdxExtractFileSha1(const char* path);

// The expected US-rev0 ROM SHA-1 (lowercase hex), read from <exeDir>/decomp-recipes/config.yml with
// the built-in constant as fallback — same resolution the extraction gate uses.
std::string GdxExtractExpectedRomSha1(const char* exeDir);

// Lowercase-hex SHA-256 of the file at `path` (empty string on read failure). Reuses the same
// streamed hasher the archive-install gate uses; ~150 ms for the 64.9 MB EK disk image.
std::string GdxExtractFileSha256(const char* path);

// ── Managed disk copy bookkeeping (disk internalization) ──────────────────────────────────────
// Records the managed Expansion Kit disk copy's identity (SHA-256 + size) into the same completion
// sidecar (gdx_extract_state.cfg) the extraction gate owns, so one file documents everything this
// build verified about the installed inputs. Read-modify-write: existing ROM/archive fields survive
// untouched and only the disk_* keys are set. A missing/unwritable sidecar is a log-only failure --
// the copy on disk stays the source of truth, and this call never gates anything.
void GdxExtractRecordManagedDisk(const char* dataDir, const char* diskSha256, unsigned long long diskSize);

// ── IPL identity bookkeeping (IPL extraction) ────────────────────────────────────────────────────
// Records the acquire-time SHA-256 of the 64DD IPL ROM into the completion sidecar (key
// `ipl_sha256`). Read-modify-write: existing cart/disk/archive fields are preserved. The dedicated
// IPL extraction step (from GdxExtractEnsureArchive) later refreshes this to the byte-order-NORMALIZED
// identity and adds `ipl_archive_sha256` (the n64ddipl.o2r hash). Diagnostic only -- nothing gates
// boot on these fields, and the archive carries its own ipl/identity entry.
void GdxExtractRecordIpl(const char* dataDir, const char* iplSha256);

// ── Disk deletion-gate helpers (disk internalization) ────────────────────────────────────────────
// The boot-time deletion gate (port/disk_buffer.cpp) proves the disk archive is byte-identical to the
// managed copy before the Data & Files panel marks the disk deletable. These let that TU reuse the
// sidecar reader and the vendored SHA-256 rather than duplicate either.

// Lowercase-hex SHA-256 of the managed disk copy, as recorded in the completion sidecar
// (gdx_extract_state.cfg key `disk_sha256`). Empty if the sidecar is missing or the key is unset.
std::string GdxExtractRecordedDiskSha256(const char* dataDir);

// Lowercase-hex SHA-256 of the disk archive container (fzerox-disk.o2r) as recorded in the completion
// sidecar (key `disk_archive_sha256`, authored by ensureDiskArchive). Empty if the sidecar is missing
// or the key is unset. First-boot's setup-required predicate checks an installed fzerox-disk.o2r
// against this before accepting it as the disk input once the raw .ndd and managed copy are gone.
std::string GdxExtractRecordedDiskArchiveSha256(const char* dataDir);

// Lowercase-hex SHA-256 of the cart archive container (fzerox.o2r) as recorded in the completion
// sidecar (key `archive_sha256`, authored by ensureCartArchive/runExtraction). Empty if the sidecar is
// missing or the key is unset. First-boot checks an installed fzerox.o2r against this before
// accepting it as the ROM input once the original .z64 is gone. Mirrors the disk-archive helper above.
std::string GdxExtractRecordedCartArchiveSha256(const char* dataDir);

// Lowercase-hex SHA-256 of the IPL archive container (n64ddipl.o2r) as recorded in the completion
// sidecar (key `ipl_archive_sha256`, authored by ensureIplArchive). Empty if the sidecar is missing or
// the key is unset. First-boot checks an installed n64ddipl.o2r against this before accepting it as
// the IPL input.
std::string GdxExtractRecordedIplArchiveSha256(const char* dataDir);

// Rename <dataDir>/<archiveName> to <archiveName>.bad so the mount path can never pick up an archive
// that failed verification, forcing first-boot's setup flow to rebuild it. Only the port's own
// generated .o2r is touched; user media (ROM/disk/IPL) is never affected. True when a file was
// quarantined, false when there was nothing to quarantine or the rename failed.
//
// `archiveName` MUST be one of the three port-generated names (fzerox.o2r, n64ddipl.o2r,
// fzerox-disk.o2r); anything else is refused and logged. This must never become a way to rename an
// arbitrary user file.
bool GdxExtractQuarantineArchive(const char* dataDir, const char* archiveName);

// ── Per-boot archive validation latch ────────────────────────────────────────────────────────────
// FirstBootRun's *ArchiveSatisfies helpers hash each installed archive against its recorded sidecar
// SHA-256 to decide whether the wizard can be skipped, and GdxExtractEnsureArchive's own warm-boot
// checks re-hash the SAME file moments later in the same boot. This latch lets a PASSING firstboot
// check short-circuit that. File-static, main-thread only, never persisted, reset every process start.
enum class GdxExtractArchiveKind { Cart, Ipl, Disk };

// Call ONLY on a passing verification. A failed check must never latch, or the ensure* warm-boot step
// stops seeing reality and never rebuilds/quarantines.
void GdxExtractMarkArchiveValidated(GdxExtractArchiveKind kind);

// True when `kind`'s archive was already validated earlier this boot via GdxExtractMarkArchiveValidated.
bool GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind kind);

// Lowercase-hex SHA-256 over an in-memory buffer, using the same vendored hasher the archive gates
// use. Empty on (nullptr / zero-length) input. Used to hash the inflated disk/image at boot.
std::string GdxExtractSha256Bytes(const void* data, unsigned long long len);

} // namespace gdx
