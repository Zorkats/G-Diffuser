// G-Diffuser — in-window first-time setup flow (ImGui). See gdx_firstboot_gui.h.
//
// Part of the G-Diffuser exe target. Runs AFTER libultraship is up (window + Gui + FileDropMgr exist)
// and BEFORE the game boots, so it may freely touch LUS state and ImGui.
//
// The game loop is not running yet, so this flow drives its own GUI-only frames through the abstract
// Window interface — HandleEvents / StartDraw / StartFrame / RunGuiOnly / EndDraw / EndFrame —
// mirroring Fast3dWindow::DrawAndRunGraphicsCommands with RunGuiOnly() (clears/binds the game
// framebuffer, renders no gfx task) in place of the game's Run(). That needs no libultraship change:
// RunGuiOnly()/StartFrame()/EndFrame() are all pure-virtual on Ship::Window.

#include "gdx_firstboot_gui.h"

#include "gdx_firstboot.h"       // validators, canonical names, copy/persist helpers, native picker
#include "gdx_extract_launch.h"  // async extraction driver (start/poll/reset)
#include "gdx_gui.h"             // optional bundled large/mono fonts
#include "port_log.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/MouseStateManager.h"
#include "ship/window/FileDropMgr.h"
#include "ship/window/gui/Gui.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace gdx {
namespace {

// The installed game archive name (matches gdx_extract_launch.cpp's kArchiveName). Kept as a local
// literal rather than exported: it is the SoH/Starship game-named-archive convention and is stable.
constexpr const char* kGameArchiveName = "fzerox.o2r";

enum class Phase {
    Acquire,       // Waiting for the three inputs to be provided + validated.
    Extracting,    // Async extraction is running; showing live progress.
    ExtractFailed, // Extraction failed; offering [Continue anyway] / [Retry].
    Done,          // Setup complete — the loop exits and boot continues.
};

enum class RowStatus { Missing, Ok, Invalid };

using Validator = bool (*)(const std::string&, std::string&);
using Picker = std::string (*)();

struct Row {
    const char* label;         // human-facing label
    const char* canonicalName; // on-disk name inside the data dir
    Validator validate;
    Picker pick;               // native picker (Windows); returns empty elsewhere / on cancel
    RowStatus status = RowStatus::Missing;
    std::string reason;        // populated when status == Invalid
    std::string detail;        // extra verified-identity line shown under the status (may be empty)
    std::string warning;       // visible warning shown on an OK row (unrecognized-but-accepted dump)
    std::string okHeader;      // overrides the "OK (...)" header text when non-empty (region label, or
                               // the archive-satisfied message when the original file is gone)
    // The path Recheck() actually resolved and validated this row against. Usually RowPath(i), but the
    // disk row may resolve to the managed-copy fallback and the ROM/disk rows to the accepted Japanese
    // alternate name (SetupRomFileNameJp/SetupDiskFileNameJp). Anything needing the row's real on-disk
    // location must read this, never RowPath(i).
    std::string resolvedPath;
    // True when the ROM row resolved to the accepted Japanese dump: setup must SKIP archive
    // extraction (US recipes cannot process it) and complete for the experimental raw-ROM boot.
    bool jpRom = false;
};

// ── Setup screen state machine ───────────────────────────────────────────────────────────────────
class SetupScreen {
  public:
    SetupScreen(std::string dataDir, std::string exeDir)
        : mDataDir(std::move(dataDir)), mExeDir(std::move(exeDir)) {
        mRows[0] = { "F-Zero X ROM (US/PAL rev0, .z64)", SetupRomFileName(), &ValidateRomFile, &PickRomFile };
        mRows[1] = { "Expansion Kit disk (.ndd)", SetupDiskFileName(), &ValidateDiskFile, &PickDiskFile };
        mRows[2] = { "64DD IPL ROM (N64DDIPLROM.n64)", SetupIplFileName(), &ValidateIplFile, &PickIplFile };
        for (Row& r : mRows) {
            Recheck(r); // pre-check files that already exist beside the exe (resumed setup)
        }
    }

    // Returns true if setup completed, false if the window was closed.
    bool Run(std::string& outRomPath) {
        auto ctx = Ship::Context::GetInstance();
        auto w = (ctx != nullptr) ? ctx->GetWindow() : nullptr;
        if (w == nullptr) {
            gdx_port_logf("[setup] no window; cannot run the in-window setup flow\n");
            return false;
        }

        auto fileDrop = ctx->GetFileDropMgr();
        if (fileDrop != nullptr) {
            fileDrop->RegisterDropHandler(&SetupScreen::OnFileDroppedThunk);
        }

        gdx_port_logf("[setup] entering in-window first-time setup\n");
        while (w->IsRunning()) {
            w->HandleEvents(); // drag-and-drop events dispatch synchronously here (main thread)
            Tick();
            if (mPhase == Phase::Done) {
                break;
            }
            w->GetMouseStateManager()->StartFrame();
            w->GetGui()->StartDraw(); // ImGui NewFrame + menu/registered windows
            DrawUI();                 // must land inside that same ImGui frame
            w->StartFrame();          // size the game framebuffers
            w->RunGuiOnly();          // clear/bind the game FB, run no gfx task
            w->GetGui()->EndDraw();   // composite + ImGui::Render + present floating windows
            w->EndFrame();
        }

        if (fileDrop != nullptr) {
            fileDrop->UnregisterDropHandler(&SetupScreen::OnFileDroppedThunk);
        }

        const bool completed = (mPhase == Phase::Done);
        if (completed) {
            // Resolved path, not the canonical name: an accepted Japanese ROM lives under its own
            // alternate filename (SetupRomFileNameJp) and must be handed to the boot as-is.
            outRomPath = mRows[0].resolvedPath.empty() ? RowPath(0) : mRows[0].resolvedPath;
            gdx_port_logf("[setup] completed; ROM=%s archiveMounted=%d\n", outRomPath.c_str(),
                          mArchiveMounted ? 1 : 0);
        } else {
            gdx_port_logf("[setup] window closed before completion; exiting\n");
        }
        return completed;
    }

    // Called from the (main-thread) FileDropMgr callback for a single dropped file.
    void HandleDrop(const char* path) {
        if (path == nullptr || mPhase != Phase::Acquire) {
            return; // only accept drops while acquiring inputs
        }
        std::string src(path);
        std::string why;
        // Classify by validator: try ROM, then disk, then IPL. First match wins.
        for (Row& r : mRows) {
            std::string reason;
            if (r.validate(src, reason)) {
                if (CopyInputInto(src, mDataDir, r.canonicalName)) {
                    Recheck(r);
                    mDropError.clear();
                    gdx_port_logf("[setup] accepted dropped %s -> %s\n", src.c_str(), r.canonicalName);
                } else {
                    mDropError = "Could not copy the dropped file next to the game.";
                }
                return;
            }
        }
        mDropError = "That file is not a recognized F-Zero X ROM, Expansion Kit disk, or 64DD IPL ROM.";
        gdx_port_logf("[setup] rejected dropped file (matches no expected input): %s\n", src.c_str());
    }

  private:
    // Absolute path of a row's installed copy inside the data dir.
    std::string RowPath(int i) const {
        return (fs::path(mDataDir) / mRows[i].canonicalName).string();
    }

    // Name of the installed archive that satisfies a given row's input (originals are deletable
    // once the archive covers them). Empty string for an unrecognized canonical name.
    static const char* ArchiveForCanonical(const char* canonicalName) {
        if (std::string(canonicalName) == SetupRomFileName())     return SetupGameArchiveFileName();
        if (std::string(canonicalName) == SetupRomFileNamePal())  return SetupGameArchiveFileName();
        if (std::string(canonicalName) == SetupRomFileNameEuPal()) return SetupGameArchiveFileName();
        if (std::string(canonicalName) == SetupDiskFileName())    return SetupDiskArchiveFileName();
        if (std::string(canonicalName) == SetupIplFileName())     return SetupIplArchiveFileName();
        return "";
    }

    // Companion to ArchiveForCanonical, so Recheck()'s "satisfied by installed archive" decision runs
    // through the hash-validating GdxFirstbootArchiveSatisfies rather than a presence probe, which
    // would accept a corrupt/foreign archive. False (outKind untouched) for an unrecognized name.
    static bool ArchiveKindForCanonical(const char* canonicalName, GdxFirstbootArchiveKind& outKind) {
        if (std::string(canonicalName) == SetupRomFileName())     { outKind = GdxFirstbootArchiveKind::Game; return true; }
        if (std::string(canonicalName) == SetupRomFileNamePal())  { outKind = GdxFirstbootArchiveKind::Game; return true; }
        if (std::string(canonicalName) == SetupRomFileNameEuPal()) { outKind = GdxFirstbootArchiveKind::Game; return true; }
        if (std::string(canonicalName) == SetupDiskFileName())    { outKind = GdxFirstbootArchiveKind::Disk; return true; }
        if (std::string(canonicalName) == SetupIplFileName())     { outKind = GdxFirstbootArchiveKind::Ipl;  return true; }
        return false;
    }

    void Recheck(Row& r) {
        std::string dst = (fs::path(mDataDir) / r.canonicalName).string();
        std::error_code ec;
        r.detail.clear();
        r.warning.clear();
        r.okHeader.clear();
        r.jpRom = false;
        if (!fs::is_regular_file(fs::path(dst), ec)) {
            // Accepted alternate names come before any derived fallback, so a JP test folder
            // (baserom.jp.rev0.z64 / baserom.jp.ek.ndd) or a folder holding the US prototype IPL dump
            // under its own name (64DD_IPL_US_MJR.n64) is detected without renaming.
            if (std::string(r.canonicalName) == SetupIplFileName()) {
                // GdxFindIplSourceInDir is shared with FirstBootRun and ensureIplArchive so the
                // accepted IPL alt name cannot drift between the wizard and boot-time extraction.
                std::string found = GdxFindIplSourceInDir(mDataDir);
                if (!found.empty()) {
                    dst = found;
                }
            } else {
                // Accepted alternate names come before any derived fallback, so a JP or PAL test
                // folder (baserom.jp.rev0.z64 / baserom.pal.rev0.z64 / baserom.eu.rev0.z64) is detected
                // without renaming. The JP alt is checked first to preserve the existing priority.
                const char* romAltName = nullptr;
                if (std::string(r.canonicalName) == SetupRomFileName()) {
                    romAltName = SetupRomFileNameJp();
                } else if (std::string(r.canonicalName) == SetupDiskFileName()) {
                    romAltName = SetupDiskFileNameJp();
                }
                if (romAltName != nullptr) {
                    std::string alt = (fs::path(mDataDir) / romAltName).string();
                    std::error_code altEc;
                    if (fs::is_regular_file(fs::path(alt), altEc)) {
                        dst = alt;
                    }
                }
                if (std::string(r.canonicalName) == SetupRomFileName()) {
                    std::error_code palEc;
                    std::string pal = (fs::path(mDataDir) / SetupRomFileNamePal()).string();
                    if (fs::is_regular_file(fs::path(pal), palEc)) {
                        dst = pal;
                    } else {
                        std::error_code euEc;
                        std::string eu = (fs::path(mDataDir) / SetupRomFileNameEuPal()).string();
                        if (fs::is_regular_file(fs::path(eu), euEc)) {
                            dst = eu;
                        }
                    }
                }
            }
            // The disk row's canonical copy may be gone because the user deleted their original .ndd
            // after a prior setup created the managed backup under <dataDir>/media; resolve against
            // that copy rather than reporting Missing.
            if (!fs::is_regular_file(fs::path(dst), ec) &&
                std::string(r.canonicalName) == SetupDiskFileName()) {
                std::string managed = ManagedDiskPath(mDataDir);
                std::error_code mgEc;
                if (fs::is_regular_file(fs::path(managed), mgEc)) {
                    dst = managed;
                }
            }
            if (!fs::is_regular_file(fs::path(dst), ec)) {
                // A requirement met by its INSTALLED ARCHIVE is satisfied, not missing: the original
                // is deletable and the game boots archive-only from it (rom_buffer.cpp /
                // disk_buffer.cpp), so the row must go green instead of demanding the original back.
                // GdxFirstbootArchiveSatisfies hashes rather than probes -- a corrupt/foreign archive
                // must not read as satisfied here any more than in FirstBootRun's fast path.
                const char* archiveName = ArchiveForCanonical(r.canonicalName);
                std::string archivePath = (fs::path(mDataDir) / archiveName).string();
                GdxFirstbootArchiveKind archiveKind;
                const bool archiveSatisfied = archiveName[0] != '\0' &&
                    ArchiveKindForCanonical(r.canonicalName, archiveKind) &&
                    GdxFirstbootArchiveSatisfies(archiveKind, mDataDir);
                if (archiveSatisfied) {
                    r.status = RowStatus::Ok;
                    r.reason.clear();
                    r.resolvedPath = archivePath;
                    r.okHeader = "Satisfied by installed archive (original file no longer needed)";
                    r.detail = std::string("Served from ") + archiveName;
                    gdx_port_logf("[setup] row '%s': satisfied by installed archive %s "
                                  "(original file no longer needed)\n", r.canonicalName, archiveName);
                    return;
                }
                r.status = RowStatus::Missing;
                r.reason.clear();
                r.resolvedPath.clear();
                gdx_port_logf("[setup] row '%s': missing\n", r.canonicalName);
                return;
            }
        }
        std::string why;
        if (!r.validate(dst, why)) {
            r.status = RowStatus::Invalid;
            r.reason = why;
            r.resolvedPath.clear();
            gdx_port_logf("[setup] row '%s': invalid -- %s\n", r.canonicalName, why.c_str());
            return;
        }
        r.status = RowStatus::Ok;
        r.reason.clear();
        // Record what was actually resolved above so callers that need the real on-disk location --
        // ConfirmAndStartExtraction's WriteSetupComplete call, DrawAcquire's "File:" line -- never
        // re-derive it and land on the canonical path when a fallback was used.
        r.resolvedPath = dst;

        // Hashing happens only when a row is (re)checked, never per frame, so even the 64.9 MB disk is
        // affordable here. The rulesets (ROM strict US-rev0; IPL/disk accept-with-label-or-warning) and
        // every message string live in gdx_firstboot.cpp so the future JP build reuses the tables.
        GdxInputRecognition rec = GdxRecognizeInput(r.canonicalName, dst, mExeDir);
        switch (rec.verdict) {
            case GdxInputVerdict::VerifiedKnown:
                r.detail = rec.message;
                if (!rec.okHeaderOverride.empty()) {
                    r.okHeader = rec.okHeaderOverride; // e.g. the EK disk region label in the header
                }
                gdx_port_logf("[setup] row '%s': OK -- %s\n", r.canonicalName, rec.message.c_str());
                break;
            case GdxInputVerdict::AcceptedUnknownWarn:
                r.detail = "SHA-1: " + rec.sha1;
                r.warning = rec.message; // visible warning shown on the OK row (untested dump)
                if (!rec.okHeaderOverride.empty()) {
                    r.okHeader = rec.okHeaderOverride; // e.g. the accepted-JP-ROM experimental header
                }
                r.jpRom = rec.jpRom;
                gdx_port_logf("[setup] row '%s': OK (accepted) WARNING -- %s\n", r.canonicalName,
                              rec.message.c_str());
                break;
            case GdxInputVerdict::Rejected:
                r.status = RowStatus::Invalid;
                r.reason = rec.message;
                r.detail.clear();
                r.resolvedPath.clear();
                gdx_port_logf("[setup] row '%s': rejected -- %s\n", r.canonicalName, rec.message.c_str());
                break;
        }
    }

    bool AllRowsOk() const {
        for (const Row& r : mRows) {
            if (r.status != RowStatus::Ok) {
                return false;
            }
        }
        return true;
    }

    void Browse(Row& r) {
        std::string picked = r.pick();
        if (picked.empty()) {
            return; // cancelled or no native picker
        }
        std::string why;
        if (!r.validate(picked, why)) {
            r.status = RowStatus::Invalid;
            r.reason = why;
            return;
        }
        if (!CopyInputInto(picked, mDataDir, r.canonicalName)) {
            r.status = RowStatus::Invalid;
            r.reason = "could not copy the selected file next to the game";
            return;
        }
        Recheck(r);
    }

    void StartExtraction() {
        // suppressNativeDialog = true: the ImGui screen owns the progress UX.
        const std::string romForExtraction =
            mRows[0].resolvedPath.empty() ? RowPath(0) : mRows[0].resolvedPath;
        GdxExtractStartAsync(mDataDir.c_str(), romForExtraction.c_str(), mExeDir.c_str(),
                             /*suppress=*/true);
        mStage.clear();
        mError.clear();
        mLog.clear();
        mEntriesSeen = 0;
        mSubStage = 0;
        mPhase = Phase::Extracting;
    }

    void ConfirmAndStartExtraction() {
        if (!AllRowsOk()) {
            return;
        }
        // A managed-copy-only re-run (original .ndd deleted after a prior setup made the backup)
        // validated against <dataDir>/media, so RowPath(1) would record a nonexistent canonical path
        // in the sidecar and make EnsureManagedDiskCopy warn about a copy that already exists.
        const std::string& diskPath = mRows[1].resolvedPath.empty() ? RowPath(1) : mRows[1].resolvedPath;
        // Same rule for the ROM row: an accepted Japanese dump usually lives under its own alternate
        // name (SetupRomFileNameJp), and recognition is by hash, so resolvedPath is right either way.
        const std::string& romPath = mRows[0].resolvedPath.empty() ? RowPath(0) : mRows[0].resolvedPath;
        if (!WriteSetupComplete(mDataDir, romPath, diskPath, RowPath(2))) {
            mDropError = "Could not save the setup state next to the game.";
            return;
        }
        // Browse()/HandleDrop() already committed the disk at dataDir/kDiskName. No separate media/
        // backup is written: fzerox-disk.o2r is the port's stable copy, and every reader falls back to
        // the committed copy anyway. media/ copies from older installs stay honored read-only.
        //
        // The US recipe tree cannot extract an accepted Japanese ROM, so setup skips extraction and
        // completes for the experimental raw-ROM boot (rom_buffer loads it directly; FirstBootRun's
        // fast path recognizes the recorded JP hash on later boots).
        if (mRows[0].jpRom) {
            gdx_port_logf("[setup] user confirmed all inputs; Japanese ROM accepted -- skipping "
                          "archive extraction (raw-ROM boot, experimental)\n");
            mPhase = Phase::Done;
            return;
        }
        gdx_port_logf("[setup] user confirmed all inputs; wrote completion marker; starting extraction\n");
        StartExtraction();
    }

    void MountArchive() {
        auto ctx = Ship::Context::GetInstance();
        auto rm = (ctx != nullptr) ? ctx->GetResourceManager() : nullptr;
        auto am = (rm != nullptr) ? rm->GetArchiveManager() : nullptr;
        if (am == nullptr) {
            gdx_port_logf("[setup] WARNING: archive manager unavailable; cannot hot-mount %s\n",
                          kGameArchiveName);
            return;
        }
        const std::string archive = (fs::path(mDataDir) / kGameArchiveName).string();
        std::error_code ec;
        if (!fs::is_regular_file(fs::path(archive), ec)) {
            gdx_port_logf("[setup] WARNING: %s missing after a successful extraction; raw-ROM fallback\n",
                          archive.c_str());
            return;
        }
        // AddArchive rebuilds the virtual file system internally (see GdxWorkshopReload); no game
        // threads run during setup, so no quiesce is needed. main.cpp's mount-time version gate does
        // not re-run here, but it is subsumed: extraction installs fzerox.o2r only when its SHA-256
        // equals this build's golden reference, which is strictly stronger than the gate's ROM-CRC
        // version-entry check.
        if (am->AddArchive(archive) != nullptr) {
            mArchiveMounted = true;
            gdx_port_logf("[setup] hot-mounted %s\n", archive.c_str());
        } else {
            gdx_port_logf("[setup] WARNING: AddArchive(%s) failed; raw-ROM fallback\n", archive.c_str());
        }
        // The same extraction run also installs the IPL and disk archives; without this they sit on
        // disk unmounted until the next launch and Data & Files reports them as such.
        for (const char* extraName : { "n64ddipl.o2r", "fzerox-disk.o2r" }) {
            const std::string extra = (fs::path(mDataDir) / extraName).string();
            if (!fs::is_regular_file(fs::path(extra), ec)) {
                continue;
            }
            if (am->AddArchive(extra) != nullptr) {
                gdx_port_logf("[setup] hot-mounted %s\n", extra.c_str());
            } else {
                gdx_port_logf("[setup] WARNING: AddArchive(%s) failed; file-based fallback\n", extra.c_str());
            }
        }
    }

    // Per-frame state machine (logic only; rendering is in DrawUI).
    void Tick() {
        switch (mPhase) {
            case Phase::Acquire:
                // Detection and validation are automatic, but installation is not. The user must see
                // the reviewed paths and explicitly confirm them in DrawAcquire().
                break;
            case Phase::Extracting: {
                ExtractProgress p = GdxExtractPollStatus();
                mStage = p.stage;
                // Copy the ring buffer + counters into GUI-owned state BEFORE any GdxExtractResetAsync()
                // below clears the launcher's copy; that is what lets ExtractFailed keep showing the log.
                mLog.assign(p.log.begin(), p.log.end());
                mEntriesSeen = p.entriesSeen;
                mSubStage = p.subStage;
                if (p.phase == ExtractPhase::Done) {
                    if (p.outcome == ExtractOutcome::FailedRawFallback) {
                        mError = p.lastError.empty() ? std::string(GdxExtractOutcomeString(p.outcome))
                                                     : p.lastError;
                        GdxExtractResetAsync();
                        mPhase = Phase::ExtractFailed;
                    } else {
                        GdxExtractResetAsync();
                        MountArchive();
                        mPhase = Phase::Done;
                    }
                }
                break;
            }
            case Phase::ExtractFailed:
            case Phase::Done:
                break;
        }
    }

    // ── Rendering ────────────────────────────────────────────────────────────────────────────────
    void DrawUI() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        // Re-center every frame so a window resize/minimize keeps the panel centered.
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Always);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking;
        if (!ImGui::Begin("G-Diffuser - First-Time Setup", nullptr, flags)) {
            ImGui::End();
            return;
        }

        ImFont* large = GdxGuiFontLarge();
        if (large != nullptr) {
            ImGui::PushFont(large);
        }
        ImGui::TextUnformatted("Welcome to G-Diffuser");
        if (large != nullptr) {
            ImGui::PopFont();
        }
        ImGui::TextWrapped("Review the three original files below. Files already next to the game are "
                           "detected automatically, but nothing is installed until you confirm. Nothing "
                           "is uploaded.");
        ImGui::Separator();

        switch (mPhase) {
            case Phase::Acquire:
            case Phase::Done:
                DrawAcquire();
                break;
            case Phase::Extracting:
                DrawExtracting();
                break;
            case Phase::ExtractFailed:
                DrawExtractFailed();
                break;
        }

        ImGui::End();
    }

    void DrawAcquire() {
        for (int i = 0; i < 3; ++i) {
            Row& r = mRows[i];
            ImGui::PushID(i);
            ImGui::SeparatorText(r.label);

            switch (r.status) {
                case RowStatus::Ok:
                    // okHeader replaces the file name: the EK disk region label, or the
                    // archive-satisfied message when the original is gone and its archive covers it.
                    ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.35f, 1.0f), "OK  (%s)",
                                       r.okHeader.empty() ? r.canonicalName : r.okHeader.c_str());
                    if (!r.warning.empty()) {
                        // Accepted but unrecognized dump: visible amber warning, row still passes.
                        ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.30f, 1.0f), "%s", r.warning.c_str());
                    }
                    break;
                case RowStatus::Invalid:
                    ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "Invalid");
                    ImGui::TextWrapped("Reason: %s", r.reason.c_str());
                    break;
                case RowStatus::Missing:
                default:
                    ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.35f, 1.0f), "Missing");
                    break;
            }
            // Show the path Recheck resolved (managed media/ copy, or the installed archive, when the
            // original is gone), not the canonical dataDir path -- that would label a managed/archive
            // hit as the deleted root file. Falls back to the canonical path before the first check.
            const std::string shownPath = r.resolvedPath.empty() ? RowPath(i) : r.resolvedPath;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("File: %s", shownPath.c_str());
            ImGui::PopTextWrapPos();
            if (!r.detail.empty()) {
                ImGui::TextDisabled("%s", r.detail.c_str());
            }

            if (NativeFilePickerAvailable()) {
                if (ImGui::Button(r.status == RowStatus::Ok ? "Replace..." : "Browse...")) {
                    Browse(r);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("...or drag & drop the file onto this window");
            } else {
                ImGui::TextDisabled("Drag & drop the file onto this window");
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        if (!mDropError.empty()) {
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "%s", mDropError.c_str());
        }
        if (AllRowsOk()) {
            ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.35f, 1.0f), "All three files are verified.");
            ImGui::TextWrapped("Confirm to build fzerox.o2r from your ROM and continue to the game.");
            // The deletable-files statement names each file, so the reader never has to guess which
            // originals are safe to remove. A Japanese ROM needs the opposite statement: it boots RAW,
            // no fzerox.o2r is ever built for it, so that ROM file is the only copy of the game data.
            if (mRows[0].jpRom) {
                ImGui::TextWrapped(
                    "Japanese ROM install (experimental): KEEP the ROM file -- it stays required "
                    "(no game-data archive is built for the Japanese version). The N64DD IPL ROM "
                    "and the Expansion Kit disk (.ndd) become deletable once their archives verify "
                    "(the green lines in Data & Files). Your saves live only in saves/*.gdd -- "
                    "back up that folder to preserve your progress.");
            } else {
                ImGui::TextWrapped(
                    "Once this finishes, none of your three original files are needed anymore: the "
                    "F-Zero X ROM (.z64), the N64DD IPL ROM, and the Expansion Kit disk (.ndd) are all "
                    "deletable. The disk is kept as a temporary managed copy in this folder's media/ "
                    "subfolder; after your next boot verifies the disk archive (the green line in "
                    "Data & Files), that copy is deletable too. Your saves live only in saves/*.gdd -- "
                    "back up that folder to preserve your progress.");
            }
            ImGui::Spacing();
            if (ImGui::Button("Build game data and continue", ImVec2(-1.0f, 0.0f))) {
                ConfirmAndStartExtraction();
            }
        } else {
            ImGui::TextDisabled("Waiting for all three files.");
        }
    }

    void DrawExtracting() {
        // subStage values are gdx_extract_launch.h's ExtractProgress contract: 0 = cart, 1 =
        // validating the cart archive, 2 = the independent IPL font-block archive.
        const char* stageLabel = "Extracting game assets...";
        if (mSubStage == 1) {
            stageLabel = "Validating extracted archive...";
        } else if (mSubStage == 2) {
            stageLabel = "Extracting IPL font data...";
        }
        ImGui::TextUnformatted(stageLabel);
        ImGui::TextWrapped("This happens once and usually takes only a few seconds.");
        ImGui::Spacing();

        // Determinate only when a total is known (GDX_O2R_EXPECTED_ENTRY_COUNT for cart, the frozen
        // 2-entry IPL archive for ipl), counted from the extractor's own per-asset "Processing" lines
        // (gdx_extract_launch.cpp's looksLikeEntryProgressLine). The validating sub-stage is a few
        // hash/zip checks after the child exited, with no per-entry signal, so it stays indeterminate.
        const int total = (mSubStage == 2) ? GdxExtractExpectedIplEntryCount() : GdxExtractExpectedCartEntryCount();
        if (mSubStage != 1 && total > 0) {
            int done = mEntriesSeen;
            if (done > total) {
                done = total; // clamp: the "Processing" heuristic can occasionally over-count
            }
            std::string overlay = std::to_string(done) + " / " + std::to_string(total);
            ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1.0f, 0.0f),
                               overlay.c_str());
        } else {
            // No reliable count for this sub-stage (placeholder golden header, or mid-validation) --
            // indeterminate animated bar (ImGui renders a moving indicator for a negative fraction).
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f),
                               mStage.empty() ? "working..." : mStage.c_str());
        }

        ImGui::Spacing();
        DrawLogView();
    }

    // Auto-scrolling scrollback of the extractor's stdout (ring-buffer snapshot from
    // GdxExtractPollStatus). Shared by the Extracting and ExtractFailed views so a failure keeps the
    // diagnostic context a one-line summary would drop.
    void DrawLogView() {
        ImFont* mono = GdxGuiFontMono();
        if (mono != nullptr) {
            ImGui::PushFont(mono);
        }
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("ExtractLog", ImVec2(-1.0f, rowHeight * 12.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& l : mLog) {
            ImGui::TextUnformatted(l.c_str());
        }
        // Auto-scroll to the bottom only while already pinned there, so a user who scrolls up to read
        // earlier lines is not yanked back down on the next incoming line.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        if (mono != nullptr) {
            ImGui::PopFont();
        }
    }

    void DrawExtractFailed() {
        ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "Asset extraction did not complete.");
        ImGui::TextWrapped("%s", mError.empty()
                                       ? "Verify that gdx-extract and decomp-recipes are next to the game."
                                       : mError.c_str());
        ImGui::TextWrapped("You can continue with the raw ROM (assets are read directly, which is slower "
                           "and less compatible), or retry the extraction.");
        ImGui::Spacing();
        // The log is the diagnostic detail a user or an issue report needs; keep it visible here.
        DrawLogView();
        ImGui::Separator();
        if (ImGui::Button("Continue anyway (raw assets)")) {
            // Boot with the raw-ROM fallback: no archive mounted, ROM path already installed.
            gdx_port_logf("[setup] user chose to continue with the raw-ROM fallback\n");
            mPhase = Phase::Done;
        }
        ImGui::SameLine();
        if (ImGui::Button("Retry")) {
            gdx_port_logf("[setup] user requested extraction retry\n");
            StartExtraction();
        }
    }

    // FileDroppedFunc is a plain C function pointer with no user data, so the callback forwards
    // through a file-scope pointer to the active screen. It fires on the main thread in HandleEvents().
    static bool OnFileDroppedThunk(char* path);

    std::string mDataDir;
    std::string mExeDir;
    Row mRows[3] = {};
    Phase mPhase = Phase::Acquire;
    std::string mStage;      // latest extraction stage line
    std::string mError;      // extraction failure reason (surfaced in DrawExtractFailed)
    std::string mDropError;  // transient "unrecognized drop" message
    bool mArchiveMounted = false;
    std::vector<std::string> mLog; // GUI-owned copy of the extractor stdout ring buffer; see Tick()
                                    // for why it must survive GdxExtractResetAsync()
    int mEntriesSeen = 0;          // real per-entry progress counter for the current sub-stage
    int mSubStage = 0;             // 0 = cart, 1 = validating cart, 2 = ipl (ExtractProgress::subStage)
};

SetupScreen* gActiveScreen = nullptr;

bool SetupScreen::OnFileDroppedThunk(char* path) {
    if (gActiveScreen != nullptr) {
        gActiveScreen->HandleDrop(path);
    }
    return true; // consume the event (prevents the "Unsupported file dropped" overlay)
}

} // namespace

bool GdxFirstBootSetupRun(const std::string& dataDir, const std::string& exeDir, std::string& outRomPath) {
    SetupScreen screen(dataDir, exeDir);
    gActiveScreen = &screen;
    bool completed = screen.Run(outRomPath);
    gActiveScreen = nullptr;
    return completed;
}

} // namespace gdx
