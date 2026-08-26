#include "gdx_content_window.h"

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#include "libultraship/bridge/consolevariablebridge.h"

extern "C" int gdx_input_in_gameplay(void);

namespace {

const char* GdxContentTypeLabel(const GdxContentEntry& entry) {
    if (entry.contentType == GDX_CONTENT_TYPE_MACHINE) {
        return "Machine";
    }
    // CRSD is the editable variant; CRSE is the share-locked one the editor writes when
    // courseData.flag == 0. Both are tracks; only CRSD can be re-opened in Course Edit.
    return entry.extension[3] == 'D' ? "Track" : "Track (locked)";
}

const char* GdxImportTypeLabel(const GdxContentImportEntry& entry) {
    if (entry.contentType == GDX_CONTENT_TYPE_MACHINE) {
        return "Machine";
    }
    if (entry.contentType == GDX_CONTENT_TYPE_BUNDLE) {
        return "Cup bundle";
    }
    if (entry.contentType == GDX_CONTENT_TYPE_TRACK) {
        return entry.extension[3] == 'D' ? "Track" : "Track (locked)";
    }
    return "Unknown";
}

const char* GdxImageTypeLabel(const GdxMfsImageEntry& entry) {
    if (entry.contentType == GDX_MFSIMG_TYPE_MACHINE) {
        return "Machine";
    }
    if (entry.contentType == GDX_MFSIMG_TYPE_TRACK) {
        return entry.extension[3] == 'D' ? "Track" : "Track (locked)";
    }
    return "Unknown";
}

// Maps the image reader's type enum onto the content one; the numeric contracts match but the
// two headers are deliberately decoupled, so convert explicitly at the seam.
int GdxImageContentType(const GdxMfsImageEntry& entry) {
    return entry.contentType == GDX_MFSIMG_TYPE_MACHINE ? GDX_CONTENT_TYPE_MACHINE : GDX_CONTENT_TYPE_TRACK;
}

#ifdef _WIN32
// Native open dialog; other platforms type/paste the path into the text field, the same split
// the first-boot pickers make (gdx_firstboot.cpp).
bool GdxPickDiskImage(char* outPath, size_t outCap) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"64DD disk images (*.ndd, *.ram)\0*.ndd;*.ram\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open a 64DD disk image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }
    return WideCharToMultiByte(CP_UTF8, 0, fileName, -1, outPath, (int) outCap, nullptr, nullptr) > 0;
}
#endif

} // namespace

GdxContentWindow::~GdxContentWindow() {
    CloseDiskImage();
}

int GdxContentWindow::RefreshLibrary() {
    int count = gdx_content_list(mEntries, GDX_CONTENT_MAX_ENTRIES);
    if (count < 0) {
        mEntryCount = 0;
        snprintf(mStatus, sizeof(mStatus), "Could not list disk content: %s.", gdx_content_error_string(count));
        return count;
    }
    if (count > GDX_CONTENT_MAX_ENTRIES) {
        snprintf(mStatus, sizeof(mStatus), "Showing the first %d of %d files.", GDX_CONTENT_MAX_ENTRIES, count);
        count = GDX_CONTENT_MAX_ENTRIES;
    }
    mEntryCount = count;
    if (mCupPickerTrack >= mEntryCount) {
        mCupPickerTrack = -1;
    }
    return count;
}

void GdxContentWindow::RefreshCupSlots() {
    memset(mCupSlots, 0, sizeof(mCupSlots));
    // Busy / no-disk are transient here; the picker simply shows empty slots until the next
    // refresh. mStatus is left alone: this runs on every refresh, not just user actions.
    gdx_content_cup_list(mCupSlots);
}

void GdxContentWindow::RefreshImports() {
    int count = gdx_content_import_list(mImports, GDX_CONTENT_IMPORT_MAX);
    if (count < 0) {
        mImportCount = 0;
        snprintf(mStatus, sizeof(mStatus), "Could not list exports/: %s.", gdx_content_error_string(count));
        return;
    }
    if (count > GDX_CONTENT_IMPORT_MAX) {
        snprintf(mStatus, sizeof(mStatus), "Showing the first %d of %d importable files.", GDX_CONTENT_IMPORT_MAX,
                 count);
        count = GDX_CONTENT_IMPORT_MAX;
    }
    mImportCount = count;
    mImportArmed = -1;
}

void GdxContentWindow::InitElement() {
    mStatus[0] = '\0';
    mAutoRefreshDone = RefreshLibrary() >= 0;
    mNextAutoRefresh = 0.0;
    mAutoRefreshDeadline = ImGui::GetTime() + 15.0;
    RefreshImports();
    RefreshCupSlots();
}

void GdxContentWindow::UpdateElement() {
    if (mAutoRefreshDone) {
        return;
    }
    const double now = ImGui::GetTime();
    if (now > mAutoRefreshDeadline) {
        mAutoRefreshDone = true; // the disk never came up; the manual Refresh button remains
        return;
    }
    if (now < mNextAutoRefresh) {
        return;
    }
    mNextAutoRefresh = now + 0.5;
    if (RefreshLibrary() >= 0) {
        mAutoRefreshDone = true;
        mStatus[0] = '\0'; // the startup listing error is stale the moment a listing succeeds
        RefreshImports();
        RefreshCupSlots();
    }
}

void GdxContentWindow::DrawElement() {
    ImGui::TextWrapped("Tracks and machines stored on the 64DD disk. Export writes a .gdxc file to the "
                       "exports/ folder next to the executable; share that file, and the other player imports "
                       "it from the Import section below.");

    // E3b: the strip toggle lives next to the export buttons, not buried in the settings menu
    // (risk R5: author ghosts and records travel inside tracks by default).
    bool includeGhosts = CVarGetInteger("gEnhancements.Content.ExportIncludeGhosts", 0) != 0;
    if (ImGui::Checkbox("Include author ghosts and time records in track exports", &includeGhosts)) {
        CVarSetInteger("gEnhancements.Content.ExportIncludeGhosts", includeGhosts ? 1 : 0);
    }
    if (includeGhosts) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Track exports currently INCLUDE your author ghosts and time records.");
    } else {
        ImGui::TextDisabled("Track exports strip author ghosts and time records (default).");
    }

    const bool busy = gdx_content_mfs_busy() != 0;
    if (busy) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "Disk operation in flight; export and import are paused.");
    }

    const bool inGameplay = gdx_input_in_gameplay() != 0;

    // E3 completion poll: on the port the disk-thread handoff is synchronous, so a begun op has
    // normally already finished; the poll keeps the UI honest if scheduling ever changes.
    if (mCupWaiting && !gdx_content_cup_pending()) {
        int32_t mfsError = 0;
        int rc = gdx_content_cup_result(&mfsError);
        mCupWaiting = false;
        mCupArmedSlot = -1;
        mCupArmedUnregister = -1;
        if (rc == GDX_CONTENT_OK) {
            snprintf(mStatus, sizeof(mStatus), "Edit Cup updated and saved.");
        } else if (rc == GDX_CONTENT_ERR_IO) {
            snprintf(mStatus, sizeof(mStatus), "Edit Cup save failed at the disk write (MFS error %d).",
                     (int) mfsError);
        } else {
            snprintf(mStatus, sizeof(mStatus), "Edit Cup update failed: %s.", gdx_content_error_string(rc));
        }
        RefreshCupSlots();
    }

    if (ImGui::Button("Refresh")) {
        // Stale results from a previous action (e.g. an early no-disk failure) must not
        // outlive a successful refresh.
        mStatus[0] = '\0';
        if (RefreshLibrary() >= 0) {
            mAutoRefreshDone = true;
        } else {
            // The disk still is not up; keep the auto-retry alive so it self-heals.
            mAutoRefreshDone = false;
            mNextAutoRefresh = 0.0;
            mAutoRefreshDeadline = ImGui::GetTime() + 15.0;
        }
        RefreshImports();
        RefreshCupSlots();
        if (mStatus[0] == '\0') {
            snprintf(mStatus, sizeof(mStatus), "Content list refreshed.");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d file%s on disk", mEntryCount, mEntryCount == 1 ? "" : "s");

    // E4: whole-cup export. Empty cups are refused by the export call itself.
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Export Edit Cup as bundle")) {
        char path[1024];
        int rc = gdx_content_export_bundle(path, sizeof(path));
        if (rc == GDX_CONTENT_OK) {
            snprintf(mStatus, sizeof(mStatus), "Cup bundle exported to %s", path);
            RefreshImports();
        } else if (rc == GDX_CONTENT_ERR_NOT_FOUND) {
            snprintf(mStatus, sizeof(mStatus), "The Edit Cup has no registered tracks to bundle.");
        } else {
            snprintf(mStatus, sizeof(mStatus), "Cup bundle export failed: %s.", gdx_content_error_string(rc));
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("CENT + its tracks -> exports/edit-cup.gdxc");
    ImGui::Separator();

    if (mEntryCount == 0) {
        ImGui::TextDisabled("No tracks or machines on the disk yet.");
        ImGui::TextWrapped("Save a course in Course Edit or a machine in Create Machine, then refresh.");
    } else if (ImGui::BeginTable("GdxContentTable", 4,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingFixedFit,
                                 ImVec2(0.0f, 280.0f))) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        for (int i = 0; i < mEntryCount; i++) {
            GdxContentEntry& entry = mEntries[i];
            const bool editableTrack =
                entry.contentType == GDX_CONTENT_TYPE_TRACK && entry.extension[3] == 'D'; // CRSD only (E3)
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.name[0] != '\0' ? entry.name : "(unnamed)");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(GdxContentTypeLabel(entry));
            ImGui::TableNextColumn();
            ImGui::Text("%d B", entry.fileSize);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Export")) {
                char path[1024];
                int rc = gdx_content_export(&entry, path, sizeof(path));
                if (rc == GDX_CONTENT_OK) {
                    snprintf(mStatus, sizeof(mStatus), "Exported to %s", path);
                    RefreshImports();
                } else {
                    snprintf(mStatus, sizeof(mStatus), "Export of '%s' failed: %s.", entry.name,
                             gdx_content_error_string(rc));
                }
            }
            // CRSE tracks are ineligible for the Edit Cup, mirroring the editor UI
            // (course_edit/19C470.c:457-461).
            if (editableTrack) {
                ImGui::SameLine();
                if (ImGui::SmallButton(mCupPickerTrack == i ? "Close Cup" : "Edit Cup")) {
                    mCupPickerTrack = (mCupPickerTrack == i) ? -1 : i;
                    mCupArmedSlot = -1;
                    mCupArmedUnregister = -1;
                    RefreshCupSlots();
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    DrawCupPicker(busy, inGameplay);
    DrawImportSection(busy, inGameplay);
    DrawImageImportSection(busy, inGameplay);

    if (mStatus[0] != '\0') {
        ImGui::Separator();
        ImGui::TextWrapped("%s", mStatus);
    }
}

void GdxContentWindow::DrawCupPicker(bool busy, bool inGameplay) {
    if (mCupPickerTrack < 0 || mCupPickerTrack >= mEntryCount) {
        return;
    }
    const GdxContentEntry& track = mEntries[mCupPickerTrack];

    ImGui::Separator();
    ImGui::Text("Edit Cup — register '%s':", track.name);
    if (strlen(track.name) > GDX_CONTENT_CUP_NAME_MAX) {
        // A slot holds 8 chars; a longer name would never resolve and the game would silently
        // clear it. Refuse up front instead.
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "Track names in the Edit Cup are limited to %d characters.", GDX_CONTENT_CUP_NAME_MAX);
        if (ImGui::SmallButton("Close")) {
            mCupPickerTrack = -1;
        }
        return;
    }
    if (inGameplay) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Edit Cup changes are unavailable while a race is live.");
    }

    const bool opsDisabled = busy || inGameplay || mCupWaiting || gdx_content_import_pending() != 0;
    for (int slot = 0; slot < GDX_CONTENT_CUP_SLOT_COUNT; slot++) {
        const char* occupant = mCupSlots[slot];
        const bool occupied = occupant[0] != '\0';
        const bool alreadyMine = occupied && strcmp(occupant, track.name) == 0;
        ImGui::PushID(2000 + slot);
        ImGui::Text("%d: %s", slot + 1, occupied ? occupant : "(empty)");
        ImGui::SameLine();
        ImGui::BeginDisabled(opsDisabled || alreadyMine);
        // R6: overwriting an occupied slot is a two-click confirm; an empty slot registers in one.
        const bool armed = mCupArmedSlot == slot;
        if (ImGui::SmallButton(!occupied ? "Register" : (armed ? "Confirm overwrite" : "Register..."))) {
            if (occupied && !armed) {
                mCupArmedSlot = slot;
                mCupArmedUnregister = -1;
            } else {
                int rc = gdx_content_cup_register_begin(track.name, slot);
                mCupArmedSlot = -1;
                if (rc == GDX_CONTENT_OK) {
                    mCupWaiting = true;
                } else {
                    snprintf(mStatus, sizeof(mStatus), "Cup registration failed: %s.",
                             gdx_content_error_string(rc));
                }
            }
        }
        ImGui::EndDisabled();
        if (occupied) {
            ImGui::SameLine();
            ImGui::BeginDisabled(opsDisabled);
            const bool armedUnreg = mCupArmedUnregister == slot;
            if (ImGui::SmallButton(armedUnreg ? "Confirm unregister" : "Unregister")) {
                if (!armedUnreg) {
                    mCupArmedUnregister = slot;
                    mCupArmedSlot = -1;
                } else {
                    int rc = gdx_content_cup_unregister_begin(slot);
                    mCupArmedUnregister = -1;
                    if (rc == GDX_CONTENT_OK) {
                        mCupWaiting = true;
                    } else {
                        snprintf(mStatus, sizeof(mStatus), "Cup unregister failed: %s.",
                                 gdx_content_error_string(rc));
                    }
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
}

void GdxContentWindow::DrawImportSection(bool busy, bool inGameplay) {
    // Completion poll: on the port the disk-thread handoff is synchronous, so a begun import has
    // normally already finished; the poll keeps the UI honest if scheduling ever changes.
    if (mImportWaiting && !gdx_content_import_pending()) {
        int32_t mfsError = 0;
        int rc = gdx_content_import_result(&mfsError);
        mImportWaiting = false;
        if (rc == GDX_CONTENT_OK) {
            snprintf(mStatus, sizeof(mStatus), "Import installed. Refresh the game's file browser to see it.");
        } else if (rc == GDX_CONTENT_ERR_IO) {
            int32_t failed = gdx_content_import_bundle_failed_index();
            if (failed >= 0) {
                // MFS has no multi-file transaction: entries before this index were installed.
                snprintf(mStatus, sizeof(mStatus),
                         "Bundle import failed at entry %d (MFS error %d); earlier entries were installed.",
                         (int) failed, (int) mfsError);
            } else {
                snprintf(mStatus, sizeof(mStatus), "Import failed at the disk write (MFS error %d).", (int) mfsError);
            }
        } else {
            snprintf(mStatus, sizeof(mStatus), "Import failed: %s.", gdx_content_error_string(rc));
        }
        // The install changed the disk (and possibly deleted a twin); both lists are stale.
        RefreshLibrary();
        RefreshImports();
        RefreshCupSlots();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Import from exports/");
    ImGui::TextDisabled("%d .gdxc file%s found", mImportCount, mImportCount == 1 ? "" : "s");
    if (inGameplay) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Import is unavailable while a race is live.");
    }

    if (mImportCount == 0) {
        ImGui::TextDisabled("Drop a .gdxc file into the exports/ folder next to the executable, then refresh.");
        return;
    }
    if (!ImGui::BeginTable("GdxContentImportTable", 4,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingFixedFit,
                           ImVec2(0.0f, 200.0f))) {
        return;
    }
    ImGui::TableSetupColumn("File");
    ImGui::TableSetupColumn("Content");
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();

    for (int i = 0; i < mImportCount; i++) {
        GdxContentImportEntry& entry = mImports[i];
        const bool isBundle = entry.contentType == GDX_CONTENT_TYPE_BUNDLE;
        const bool valid = entry.errors == 0;
        // STATE_UNKNOWN alone means the disk state could not be probed (busy/still mounting),
        // not that the file failed validation.
        const bool stateUnknown = entry.errors == GDX_IMPORT_ERR_STATE_UNKNOWN;
        // Bundles always confirm (they replace the Edit Cup and write several files at once);
        // single items confirm on overwrite/twin/cup warnings (R4).
        const bool needsConfirm =
            isBundle ||
            (entry.warnings & (GDX_IMPORT_WARN_OVERWRITE | GDX_IMPORT_WARN_TWIN_DELETE | GDX_IMPORT_WARN_CUP_CLEAR)) !=
                0;
        ImGui::PushID(1000 + i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.fileName);
        ImGui::TableNextColumn();
        if (isBundle) {
            ImGui::Text("Cup bundle (%d track%s)", entry.bundleTrackCount, entry.bundleTrackCount == 1 ? "" : "s");
            if (entry.bundleContents[0] != '\0') {
                ImGui::TextWrapped("%s", entry.bundleContents);
            }
            if (entry.classFileCount >= 0) {
                ImGui::TextDisabled("%d/100 in class", entry.classFileCount);
            }
        } else if (entry.contentType == GDX_CONTENT_TYPE_TRACK || entry.contentType == GDX_CONTENT_TYPE_MACHINE) {
            ImGui::Text("%s (%s)", entry.name[0] != '\0' ? entry.name : "(unnamed)", GdxImportTypeLabel(entry));
            if (entry.classFileCount >= 0) {
                ImGui::TextDisabled("%d/100 in class", entry.classFileCount);
            }
        } else {
            ImGui::TextDisabled("(unrecognized)");
        }
        // E3b: the container's strip bits are informational on import — say what traveled.
        if ((entry.flags & GDX_CONTENT_FLAG_GHOSTS_STRIPPED) != 0) {
            ImGui::TextDisabled("ghosts stripped");
        }
        ImGui::TableNextColumn();
        if (valid) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "OK");
        } else if (stateUnknown) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Unknown");
            ImGui::TextWrapped("Disk state not probed (disk busy or still mounting); refresh in a moment.");
        } else {
            char errors[384];
            gdx_content_import_format_errors(entry.errors, errors, sizeof(errors));
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Rejected");
            ImGui::TextWrapped("%s", errors);
        }
        if (entry.warnings != 0) {
            char warnings[256];
            gdx_content_import_format_warnings(entry.warnings, warnings, sizeof(warnings));
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", warnings);
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!valid || busy || inGameplay || mImportWaiting || mCupWaiting);
        // R4: overwrite/twin/cup side effects are an explicit two-click confirmation, never silent.
        const bool armed = mImportArmed == i;
        if (ImGui::SmallButton(needsConfirm && !armed ? "Import..." : (needsConfirm ? "Confirm" : "Import"))) {
            if (needsConfirm && !armed) {
                mImportArmed = i;
            } else {
                int rc = gdx_content_import_begin(entry.fileName);
                mImportArmed = -1;
                if (rc == GDX_CONTENT_OK) {
                    mImportWaiting = true;
                } else if (rc == GDX_CONTENT_IMPORT_ERR_VALIDATION) {
                    snprintf(mStatus, sizeof(mStatus), "'%s' failed its re-check; the list was refreshed.",
                             entry.fileName);
                    RefreshImports();
                } else if (rc == GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY) {
                    snprintf(mStatus, sizeof(mStatus), "Import is unavailable while a race is live.");
                } else {
                    snprintf(mStatus, sizeof(mStatus), "Import of '%s' failed: %s.", entry.fileName,
                             gdx_content_error_string(rc));
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void GdxContentWindow::CloseDiskImage() {
    if (mDiskImage != nullptr) {
        gdx_mfsimg_close(mDiskImage);
        mDiskImage = nullptr;
    }
    mImageEntryCount = 0;
    mImageTotalCount = 0;
    mImageArmed = -1;
}

void GdxContentWindow::OpenDiskImage(const char* path) {
    GdxMfsImage* image = nullptr;
    int rc = gdx_mfsimg_open(path, &image);
    if (rc != GDX_MFSIMG_OK) {
        snprintf(mStatus, sizeof(mStatus), "Could not open the disk image: %s.", gdx_mfsimg_strerror(rc));
        return;
    }

    int count = gdx_mfsimg_list(image, nullptr, 0);
    if (count < 0) {
        snprintf(mStatus, sizeof(mStatus), "Could not list the disk image: %s.", gdx_mfsimg_strerror(count));
        gdx_mfsimg_close(image);
        return;
    }

    CloseDiskImage();
    mDiskImage = image;
    mImageTotalCount = count;
    if (count > GDX_CONTENT_MAX_ENTRIES) {
        count = GDX_CONTENT_MAX_ENTRIES;
    }
    mImageEntryCount = count > 0 ? gdx_mfsimg_list(mDiskImage, mImageEntries, count) : 0;
    if (mImageEntryCount < 0) {
        snprintf(mStatus, sizeof(mStatus), "Could not list the disk image: %s.",
                 gdx_mfsimg_strerror(mImageEntryCount));
        CloseDiskImage();
        return;
    }

    // Per-row validation for display, mirroring RefreshImports: read each payload and run the
    // same chain begin_payload re-runs authoritatively at click time.
    for (int i = 0; i < mImageEntryCount; i++) {
        GdxMfsImageEntry& entry = mImageEntries[i];
        GdxContentImportEntry& row = mImageImportRows[i];
        memset(&row, 0, sizeof(row));
        row.classFileCount = -1;
        row.bundleTrackCount = -1;
        if (entry.encoded) {
            continue; // rendered as a refused row from the encoded flag itself
        }
        if (entry.fileSize <= 0 || entry.fileSize > GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
            row.errors = GDX_IMPORT_ERR_BAD_SIZE;
            continue;
        }
        uint8_t* payload = static_cast<uint8_t*>(malloc(entry.fileSize));
        if (payload == nullptr) {
            row.errors = GDX_IMPORT_ERR_IO;
            continue;
        }
        rc = gdx_mfsimg_read_file(mDiskImage, entry.entryId, payload, entry.fileSize);
        if (rc != GDX_MFSIMG_OK) {
            row.errors = GDX_IMPORT_ERR_IO;
        } else {
            gdx_content_import_validate_payload(entry.name, entry.extension, GdxImageContentType(entry), payload,
                                                entry.fileSize, &row);
        }
        free(payload);
    }
    snprintf(mStatus, sizeof(mStatus), "Disk image opened: %d track/machine file%s found.", mImageTotalCount,
             mImageTotalCount == 1 ? "" : "s");
}

void GdxContentWindow::DrawImageImportSection(bool busy, bool inGameplay) {
    ImGui::Separator();
    ImGui::TextUnformatted("Import from a disk image (EXPERIMENTAL)");
    ImGui::TextWrapped("Reads tracks and machines straight out of a 64DD disk dump (.ndd or raw .ram) from an "
                       "emulator or console — no .gdxc conversion step needed. The image is only read, never "
                       "written.");

    ImGui::InputTextWithHint("##diskimagepath", "Path to a .ndd / .ram disk image...", mDiskImageInput,
                             sizeof(mDiskImageInput));
    ImGui::SameLine();
#ifdef _WIN32
    if (ImGui::Button("Browse...")) {
        GdxPickDiskImage(mDiskImageInput, sizeof(mDiskImageInput));
    }
    ImGui::SameLine();
#endif
    if (ImGui::Button("Open image")) {
        if (mDiskImageInput[0] == '\0') {
            snprintf(mStatus, sizeof(mStatus), "Enter or pick a disk-image path first.");
        } else {
            OpenDiskImage(mDiskImageInput);
        }
    }
    if (mDiskImage != nullptr) {
        ImGui::SameLine();
        if (ImGui::Button("Close image")) {
            CloseDiskImage();
        }
    }

    if (mDiskImage == nullptr) {
        return;
    }
    if (mImageTotalCount > mImageEntryCount) {
        ImGui::TextDisabled("Showing the first %d of %d files in the image.", mImageEntryCount, mImageTotalCount);
    }
    if (mImageEntryCount == 0) {
        ImGui::TextDisabled("The image holds no Course Edit tracks or Create Machine machines.");
        return;
    }
    if (!ImGui::BeginTable("GdxImageImportTable", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingFixedFit,
                           ImVec2(0.0f, 200.0f))) {
        return;
    }
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();

    for (int i = 0; i < mImageEntryCount; i++) {
        GdxMfsImageEntry& entry = mImageEntries[i];
        GdxContentImportEntry& row = mImageImportRows[i];
        const bool valid = !entry.encoded && row.errors == 0;
        // STATE_UNKNOWN alone means the live disk state could not be probed, not that the payload
        // failed validation.
        const bool stateUnknown = row.errors == GDX_IMPORT_ERR_STATE_UNKNOWN;
        const bool needsConfirm =
            (row.warnings & (GDX_IMPORT_WARN_OVERWRITE | GDX_IMPORT_WARN_TWIN_DELETE | GDX_IMPORT_WARN_CUP_CLEAR)) !=
            0;
        ImGui::PushID(3000 + i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.name[0] != '\0' ? entry.name : "(unnamed)");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(GdxImageTypeLabel(entry));
        ImGui::TableNextColumn();
        ImGui::Text("%d B", (int) entry.fileSize);
        if (row.classFileCount >= 0) {
            ImGui::TextDisabled("%d/100 in class", row.classFileCount);
        }
        ImGui::TableNextColumn();
        if (entry.encoded) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Refused");
            ImGui::TextWrapped("Copy-protected (encoded) payload; extraction is not supported.");
        } else if (valid) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "OK");
        } else if (stateUnknown) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Unknown");
            ImGui::TextWrapped("Disk state not probed (disk busy or still mounting); close and reopen the image in a "
                               "moment.");
        } else {
            char errors[384];
            gdx_content_import_format_errors(row.errors, errors, sizeof(errors));
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Rejected");
            ImGui::TextWrapped("%s", errors);
        }
        if (row.warnings != 0) {
            char warnings[256];
            gdx_content_import_format_warnings(row.warnings, warnings, sizeof(warnings));
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", warnings);
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!valid || busy || inGameplay || mImportWaiting || mCupWaiting);
        // Same two-click rule as the exports import: overwrite/twin/cup side effects confirm.
        const bool armed = mImageArmed == i;
        if (ImGui::SmallButton(needsConfirm && !armed ? "Import..." : (needsConfirm ? "Confirm" : "Import"))) {
            if (needsConfirm && !armed) {
                mImageArmed = i;
            } else {
                mImageArmed = -1;
                // Re-read the payload at click time: never trust the list's earlier snapshot.
                uint8_t* payload = static_cast<uint8_t*>(malloc(entry.fileSize));
                if (payload == nullptr) {
                    snprintf(mStatus, sizeof(mStatus), "Out of memory reading '%s' from the image.", entry.name);
                } else {
                    int readRc = gdx_mfsimg_read_file(mDiskImage, entry.entryId, payload, entry.fileSize);
                    if (readRc != GDX_MFSIMG_OK) {
                        snprintf(mStatus, sizeof(mStatus), "Could not read '%s' from the image: %s.", entry.name,
                                 gdx_mfsimg_strerror(readRc));
                    } else {
                        int irc = gdx_content_import_begin_payload(entry.name, entry.extension,
                                                                   GdxImageContentType(entry), payload,
                                                                   entry.fileSize);
                        if (irc == GDX_CONTENT_OK) {
                            // The shared completion poll in DrawImportSection reports and refreshes.
                            mImportWaiting = true;
                        } else if (irc == GDX_CONTENT_IMPORT_ERR_VALIDATION) {
                            snprintf(mStatus, sizeof(mStatus),
                                     "'%s' failed its re-check; close and reopen the image to refresh.", entry.name);
                        } else if (irc == GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY) {
                            snprintf(mStatus, sizeof(mStatus), "Import is unavailable while a race is live.");
                        } else {
                            snprintf(mStatus, sizeof(mStatus), "Import of '%s' failed: %s.", entry.name,
                                     gdx_content_error_string(irc));
                        }
                    }
                    free(payload);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}
