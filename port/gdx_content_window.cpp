#include "gdx_content_window.h"

#include <imgui.h>
#include <imgui_internal.h> // ImStristr — case-insensitive substring search for the filter

#include "ship/window/gui/IconsFontAwesome4.h" // ICON_FA_FOLDER for accordion headers

#include "rom_buffer.h" // gdx_rom_path: the clean ROM baseline for W2 course comparison
#include "gdx_course_registry.h" // GDX_COURSE_EXT_ERR_* names for the sidecar status line

#include <cstdarg>
#include <cmath>
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

// 8A: the session title of a track (a .gdxt title, or the full name behind a short cup alias an
// older build made).
// Returns false when the course has no title.
bool SessionTitle(const char* name, const char* extension, char* out, size_t cap) {
    char courseId[GDX_COURSE_FILE_ID_MAX + 1];

    snprintf(courseId, sizeof(courseId), "%s.%s", name, extension);
    return gdx_course_title_get(courseId, out, cap) != 0;
}

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
    ofn.lpstrFilter = L"64DD disk images (*.ndd, *.ndr, *.d6r, *.n64dd, *.disk, *.disk_save, *.d64, *.ram)\0"
                      L"*.ndd;*.ndr;*.d6r;*.n64dd;*.disk;*.disk_save;*.d64;*.ram\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open a 64DD disk image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }
    return WideCharToMultiByte(CP_UTF8, 0, fileName, -1, outPath, (int) outCap, nullptr, nullptr) > 0;
}

bool GdxPickRom(char* outPath, size_t outCap) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"N64 ROM images (*.z64, *.n64, *.v64)\0*.z64;*.n64;*.v64\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open an F-Zero X ROM";
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
    CloseRomCourses();
}

int GdxContentWindow::RefreshLibrary() {
    int count = gdx_content_list(mEntries, GDX_CONTENT_MAX_ENTRIES);
    if (count < 0) {
        mEntryCount = 0;
        mCupPickerTrack = -1;
        LoadTrackPreview(-1);
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
        LoadTrackPreview(-1);
    } else if (mCupPickerTrack >= 0) {
        LoadTrackPreview(mCupPickerTrack);
    }
    return count;
}

void GdxContentWindow::LoadTrackPreview(int row) {
    mTrackPreviewCount = 0;
    mTrackPreviewRow = -1;
    if (row < 0 || row >= mEntryCount || mEntries[row].contentType != GDX_CONTENT_TYPE_TRACK ||
        mEntries[row].extension[3] != 'D') {
        return;
    }
    mTrackPreviewRow = row;

    uint8_t payload[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
    if (gdx_content_load_track_payload(mEntries[row].name, payload, sizeof(payload)) != GDX_CONTENT_OK) {
        return;
    }

    // CourseData stores count at byte 1 and its control-point array at 0x20. Each point begins
    // with its 3D position (three floats) and has a 0x14-byte stride in the on-disk context.
    const int count = static_cast<int8_t>(payload[1]);
    if (count < 2 || count > 64) {
        return;
    }
    for (int i = 0; i < count; i++) {
        const size_t pointOffset = 0x20u + static_cast<size_t>(i) * 0x14u;
        memcpy(&mTrackPreviewX[i], payload + pointOffset, sizeof(float));
        memcpy(&mTrackPreviewZ[i], payload + pointOffset + 8u, sizeof(float));
        if (!std::isfinite(mTrackPreviewX[i]) || !std::isfinite(mTrackPreviewZ[i])) {
            return;
        }
    }
    mTrackPreviewCount = count;
}

void GdxContentWindow::DrawTrackPreview() {
    ImGui::TextUnformatted("Track preview");
    if (mTrackPreviewRow >= 0 && mTrackPreviewRow < mEntryCount) {
        const GdxContentEntry& selected = mEntries[mTrackPreviewRow];
        char title[GDX_COURSE_TITLE_MAX + 1];
        const bool hasTitle = SessionTitle(selected.name, selected.extension, title, sizeof(title));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hasTitle ? title : selected.name);
    }

    if (mTrackPreviewCount < 2) {
        ImGui::TextDisabled(mTrackPreviewRow >= 0 ? "Track layout preview unavailable."
                                                 : "Select a track or occupied deck slot to preview its layout.");
        return;
    }

    float minX = mTrackPreviewX[0];
    float maxX = minX;
    float minZ = mTrackPreviewZ[0];
    float maxZ = minZ;
    for (int i = 1; i < mTrackPreviewCount; i++) {
        if (mTrackPreviewX[i] < minX) minX = mTrackPreviewX[i];
        if (mTrackPreviewX[i] > maxX) maxX = mTrackPreviewX[i];
        if (mTrackPreviewZ[i] < minZ) minZ = mTrackPreviewZ[i];
        if (mTrackPreviewZ[i] > maxZ) maxZ = mTrackPreviewZ[i];
    }

    const ImVec2 canvas = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float rangeX = maxX - minX > 1.0f ? maxX - minX : 1.0f;
    const float rangeZ = maxZ - minZ > 1.0f ? maxZ - minZ : 1.0f;
    const float width = avail.x > 16.0f ? avail.x - 16.0f : 1.0f;
    const float height = avail.y > 8.0f ? avail.y - 8.0f : 1.0f;
    const float scale = (width / rangeX < height / rangeZ) ? width / rangeX : height / rangeZ;
    const ImVec2 center(canvas.x + avail.x * 0.5f, canvas.y + height * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < mTrackPreviewCount; i++) {
        const int next = (i + 1) % mTrackPreviewCount;
        const ImVec2 a(center.x + (mTrackPreviewX[i] - (minX + maxX) * 0.5f) * scale,
                       center.y - (mTrackPreviewZ[i] - (minZ + maxZ) * 0.5f) * scale);
        const ImVec2 b(center.x + (mTrackPreviewX[next] - (minX + maxX) * 0.5f) * scale,
                       center.y - (mTrackPreviewZ[next] - (minZ + maxZ) * 0.5f) * scale);
        draw->AddLine(a, b, IM_COL32(90, 185, 255, 255), 3.0f);
    }
    const ImVec2 start(center.x + (mTrackPreviewX[0] - (minX + maxX) * 0.5f) * scale,
                       center.y - (mTrackPreviewZ[0] - (minZ + maxZ) * 0.5f) * scale);
    draw->AddCircleFilled(start, 4.0f, IM_COL32(120, 230, 150, 255));
}

void GdxContentWindow::RefreshCupSlots() {
    memset(mCupSlots, 0, sizeof(mCupSlots));
    // Busy / no-disk are transient here; the picker simply shows empty slots until the next
    // refresh. mStatus is left alone: this runs on every refresh, not just user actions.
    gdx_content_cup_list(mCupSlots);
    if (mCupPickerTrack >= 0 && mCupPickerTrack < mEntryCount) {
        return;
    }
    if (mCupPickerSlot >= 0 && mCupPickerSlot < GDX_CONTENT_CUP_SLOT_COUNT && mCupSlots[mCupPickerSlot][0] != '\0') {
        for (int i = 0; i < mEntryCount; i++) {
            if (mEntries[i].contentType == GDX_CONTENT_TYPE_TRACK &&
                strcmp(mEntries[i].name, mCupSlots[mCupPickerSlot]) == 0) {
                LoadTrackPreview(i);
                return;
            }
        }
    }
    LoadTrackPreview(-1);
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

    const bool busy = gdx_content_mfs_busy() != 0;
    const bool inGameplay = gdx_input_in_gameplay() != 0;
    const bool opsDisabled = busy || inGameplay || mCupWaiting || gdx_content_import_pending() != 0;

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

    // ── Primary Toolbar ──────────────────────────────────────────────────────────────────────────
    if (ImGui::Button(ICON_FA_REFRESH " Refresh")) {
        mStatus[0] = '\0';
        if (RefreshLibrary() >= 0) {
            mAutoRefreshDone = true;
        } else {
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
    ImGui::TextDisabled("(%d file%s on disk)", mEntryCount, mEntryCount == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(ICON_FA_ARCHIVE " Export Edit Cup as bundle")) {
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
    if (ImGui::Button(ICON_FA_COG " Options")) {
        ImGui::OpenPopup("ContentOptions");
    }
    if (ImGui::BeginPopup("ContentOptions")) {
        bool includeGhosts = CVarGetInteger("gEnhancements.Content.ExportIncludeGhosts", 0) != 0;
        if (ImGui::Checkbox("Include author ghosts and time records in track exports", &includeGhosts)) {
            CVarSetInteger("gEnhancements.Content.ExportIncludeGhosts", includeGhosts ? 1 : 0);
        }
        if (includeGhosts) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Track exports INCLUDE your author ghosts and time records.");
        } else {
            ImGui::TextDisabled("Track exports strip author ghosts and time records (default).");
        }
        ImGui::EndPopup();
    }

    // Keep Help on the primary toolbar row, aligned to the window's right edge.
    ImGui::SameLine();
    const float helpButtonWidth = ImGui::CalcTextSize("? Help").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - helpButtonWidth);
    if (ImGui::Button("? Help")) {
        mHelpOpen = !mHelpOpen;
    }
    if (mHelpOpen) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 1.0f));
        if (ImGui::BeginChild("HelpBanner", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
            ImGui::TextWrapped(
                "Tracks and machines stored on the 64DD disk. "
                "Export writes one file to the exports/ folder next to the executable: "
                "a .gdxp package for a track (with its custom music) or a .gdxc for a machine. "
                "Share that file, and the other player imports it from the Import section below.");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (busy) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "Disk operation in flight; export and import are paused.");
    }
    if (inGameplay) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "Edit Cup changes are unavailable while a race is live.");
    }

    // ── Edit Cup Deck — 6-slot card grid (3 × 2) ────────────────────────────────────────────────
    //
    // Interaction model: select a track then a slot, or select a slot then a track.
    //   • Clicking an empty slot with a track selected registers in one click.
    //   • Clicking an occupied slot with a track selected arms it for confirmation.
    //   • Clicking the armed slot again confirms overwrite.
    //   • The × button on an occupied card: unregister (same two-click rule).
    ImGui::Separator();
    ImGui::TextUnformatted("Edit Cup");
    if (mCupPickerTrack >= 0 && mCupPickerTrack < mEntryCount) {
        const GdxContentEntry& sel = mEntries[mCupPickerTrack];
        char selTitle[GDX_COURSE_TITLE_MAX + 1];
        const bool hasSelTitle = SessionTitle(sel.name, sel.extension, selTitle, sizeof(selTitle));
        ImGui::SameLine();
        ImGui::TextDisabled("— click a slot to place  \"%s\"", hasSelTitle ? selTitle : sel.name);
    } else if (mCupPickerSlot >= 0 && mCupPickerSlot < GDX_CONTENT_CUP_SLOT_COUNT) {
        ImGui::SameLine();
        ImGui::TextDisabled("— choose a track for Slot %d", mCupPickerSlot + 1);
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    // 6×1 row: divide full available width evenly across all 6 slots.
    const float deckGapX    = st.ItemSpacing.x;
    const float deckCardW   = (ImGui::GetContentRegionAvail().x - deckGapX * (GDX_CONTENT_CUP_SLOT_COUNT - 1))
                              / GDX_CONTENT_CUP_SLOT_COUNT;
    const ImVec2 cardSize(deckCardW, 72.0f);

    for (int slot = 0; slot < GDX_CONTENT_CUP_SLOT_COUNT; slot++) {
        if (slot != 0) {
            ImGui::SameLine(0.0f, deckGapX);
        }

        const char* occupant = mCupSlots[slot];
        const bool occupied  = occupant[0] != '\0';

        // Which track (if any) in the library is this slot's current occupant?
        int occupantLibIdx = -1;
        if (occupied) {
            for (int k = 0; k < mEntryCount; k++) {
                if (strcmp(mEntries[k].name, occupant) == 0) {
                    occupantLibIdx = k;
                    break;
                }
            }
        }

        const bool isSelectedTrack = occupied && mCupPickerTrack >= 0 &&
                                     mCupPickerTrack < mEntryCount &&
                                     strcmp(occupant, mEntries[mCupPickerTrack].name) == 0;
        const bool isSelectedSlot   = mCupPickerSlot == slot;
        const bool isArmedSlot     = mCupArmedSlot == slot;
        const bool isArmedUnreg    = mCupArmedUnregister == slot;

        // Card border colour: gold = already holds the selected track, blue = selected destination,
        // amber = armed for confirm, default = normal border.
        ImVec4 borderCol = st.Colors[ImGuiCol_Border];
        if (isSelectedTrack) {
            borderCol = ImVec4(0.95f, 0.80f, 0.20f, 1.0f); // gold — "already in deck"
        } else if (isSelectedSlot) {
            borderCol = ImVec4(0.25f, 0.55f, 1.0f, 1.0f);  // blue — selected destination
        } else if (isArmedSlot || isArmedUnreg) {
            borderCol = ImVec4(1.0f, 0.45f, 0.15f, 1.0f);  // amber — awaiting confirm
        }

        // Card background: slightly lighter when a picker track is active and this slot can accept.
        ImVec4 bgCol = st.Colors[ImGuiCol_FrameBg];
        if (isSelectedSlot) {
            bgCol = ImVec4(0.12f, 0.22f, 0.42f, 1.0f);
        } else if (!occupied && mCupPickerTrack >= 0) {
            bgCol = ImVec4(bgCol.x + 0.06f, bgCol.y + 0.06f, bgCol.z + 0.06f, bgCol.w);
        }

        ImGui::PushID(2000 + slot);
        ImGui::PushStyleColor(ImGuiCol_Border,  borderCol);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, bgCol);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        // The whole card is a clickable child region.
        bool cardClicked = false;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bgCol);
        if (ImGui::BeginChild("##slot", cardSize, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            // Slot label (top-left, small/muted)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.65f, 1.0f));
            ImGui::TextUnformatted(slot == 0 ? "Slot 1" : slot == 1 ? "Slot 2" :
                                   slot == 2 ? "Slot 3" : slot == 3 ? "Slot 4" :
                                   slot == 4 ? "Slot 5" : "Slot 6");
            ImGui::PopStyleColor();

            if (occupied) {
                char occTitle[GDX_COURSE_TITLE_MAX + 1];
                const char* displayName = occupant;
                if (occupantLibIdx >= 0 &&
                    SessionTitle(mEntries[occupantLibIdx].name, mEntries[occupantLibIdx].extension,
                                 occTitle, sizeof(occTitle))) {
                    displayName = occTitle;
                }
                if (isSelectedTrack) {
                    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.20f, 1.0f), "%s", displayName);
                } else {
                    ImGui::TextUnformatted(displayName);
                }
                // Unregister button: small × pinned to bottom-right of the card.
                const char* unregLabel = isArmedUnreg ? "OK?" : ICON_FA_TIMES;
                float xBtnW = ImGui::CalcTextSize(unregLabel).x + st.FramePadding.x * 2.0f;
                ImGui::SetCursorPos(ImVec2(cardSize.x - xBtnW - st.WindowPadding.x,
                                           cardSize.y - ImGui::GetFrameHeight() - st.WindowPadding.y));
                ImGui::BeginDisabled(opsDisabled);
                if (ImGui::SmallButton(unregLabel)) {
                    if (!isArmedUnreg) {
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
            } else {
                ImGui::TextDisabled("Empty");
            }

            // The card selects a destination when no track is selected, or assigns the selected
            // track to this slot. The unregister button above retains priority over this hit target.
            const bool canInteract = !opsDisabled &&
                ((mCupPickerTrack >= 0 && mCupPickerTrack < mEntryCount && !isSelectedTrack) ||
                 mCupPickerTrack < 0);
            if (canInteract) {
                ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.07f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.12f));
                cardClicked = ImGui::InvisibleButton("##assign", cardSize);
                ImGui::PopStyleColor(3);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(); // ChildBg
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2); // Border, FrameBg

        // Handle the assign click outside the child context.
        if (cardClicked) {
            if (mCupPickerTrack < 0 || mCupPickerTrack >= mEntryCount) {
                // Select a destination first; clicking it again cancels the selection.
                mCupPickerSlot = mCupPickerSlot == slot ? -1 : slot;
                LoadTrackPreview(mCupPickerSlot == slot ? occupantLibIdx : -1);
                mCupArmedSlot = -1;
                mCupArmedUnregister = -1;
            } else if (occupied && !isArmedSlot) {
                // First click on occupied: arm for confirm (R6).
                mCupArmedSlot = slot;
                mCupArmedUnregister = -1;
            } else {
                // Empty slot, or second click on armed occupied slot: register.
                int rc = gdx_content_cup_register_begin(mEntries[mCupPickerTrack].name, slot);
                mCupArmedSlot = -1;
                if (rc == GDX_CONTENT_OK) {
                    mCupWaiting = true;
                } else {
                    snprintf(mStatus, sizeof(mStatus), "Cup registration failed: %s.",
                             gdx_content_error_string(rc));
                }
            }
        }
        // Tooltip: show full occupant name and confirm-overwrite hint.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && occupied) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(occupant);
            if (isArmedSlot) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.15f, 1.0f), "Click again to confirm overwrite");
            }
            ImGui::EndTooltip();
        }

        ImGui::PopID();
    }

    // ── Local Files on Disk — card grid ─────────────────────────────────────────────────────────
    ImGui::Separator();

    // Filter bar + "select track to assign" hint side by side
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.40f);
    ImGui::InputTextWithHint("##filter", ICON_FA_SEARCH " Search / Filter...", mFilterBuf, sizeof(mFilterBuf));
    if (mCupPickerTrack >= 0 || mCupPickerSlot >= 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_TIMES " Clear selection")) {
            mCupPickerTrack = -1;
            LoadTrackPreview(-1);
            mCupPickerSlot = -1;
            mCupArmedSlot = -1;
            mCupArmedUnregister = -1;
        }
    }

    const int gridCols = 4;
    const float fileCardH = 90.0f;
    const float gapX = st.ItemSpacing.x;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float oldGridScrollbar = mEntryCount > 12 ? st.ScrollbarSize : 0.0f;
    const float fileCardW = (availableWidth - st.WindowPadding.x * 2.0f - oldGridScrollbar - gapX * 5.0f) / 6.0f;
    const float newGridScrollbar = mEntryCount > 8 ? st.ScrollbarSize : 0.0f;
    const float selectorWidth = fileCardW * gridCols + gapX * (gridCols - 1) +
                                st.WindowPadding.x * 2.0f + newGridScrollbar;

    // Keep each card at the former six-column width and reserve the remaining width for preview.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (ImGui::BeginChild("LibraryGrid", ImVec2(selectorWidth, 280.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_None)) {
        if (mEntryCount == 0) {
            ImGui::TextDisabled("No tracks or machines on the disk yet.");
            ImGui::TextWrapped("Save a course in Course Edit or a machine in Create Machine, then refresh.");
        } else {
            const float gapY = st.ItemSpacing.y;
            int visibleCol = 0;
            for (int i = 0; i < mEntryCount; i++) {
                GdxContentEntry& entry = mEntries[i];
                const bool editableTrack =
                    entry.contentType == GDX_CONTENT_TYPE_TRACK && entry.extension[3] == 'D';

                // Search filter (case-insensitive).
                if (mFilterBuf[0] != '\0') {
                    char ftitle[GDX_COURSE_TITLE_MAX + 1];
                    bool hasFTitle = SessionTitle(entry.name, entry.extension, ftitle, sizeof(ftitle));
                    const char* nm = entry.name[0] != '\0' ? entry.name : "(unnamed)";
                    if (ImStristr(nm, nullptr, mFilterBuf, nullptr) == nullptr &&
                        (!hasFTitle || ImStristr(ftitle, nullptr, mFilterBuf, nullptr) == nullptr)) {
                        continue;
                    }
                }

                ImGui::PushID(i);

                // Is this card the currently-selected picker track?
                const bool isSelected = (mCupPickerTrack == i);
                // Is this track already in at least one cup slot?
                int inSlot = -1;
                for (int s = 0; s < GDX_CONTENT_CUP_SLOT_COUNT; s++) {
                    if (mCupSlots[s][0] != '\0' && strcmp(mCupSlots[s], entry.name) == 0) {
                        inSlot = s;
                        break;
                    }
                }

                // Dimming: unselected, non-deck cards fade to ~15% opacity to recede visually.
                // Selected (being placed) and in-deck cards render at full opacity.
                const bool prominent = isSelected || inSlot >= 0;
                const float dimAlpha = prominent ? 1.0f : 0.15f;

                // Card border colour.
                ImVec4 fcBorder = st.Colors[ImGuiCol_Border];
                fcBorder.w *= dimAlpha;
                if (isSelected) {
                    fcBorder = ImVec4(0.25f, 0.55f, 1.0f, 1.0f);   // blue — selected
                } else if (inSlot >= 0) {
                    fcBorder = st.Colors[ImGuiCol_Border]; // default color — in deck
                }

                // Card background.
                ImVec4 fcBg = st.Colors[ImGuiCol_FrameBg];
                if (isSelected) {
                    fcBg = ImVec4(0.12f, 0.22f, 0.42f, 1.0f);
                } else {
                    fcBg.w *= dimAlpha;
                }

                // Place cards side-by-side; SameLine for every card after the first in a row.
                if (visibleCol % gridCols != 0) {
                    ImGui::SameLine(0.0f, gapX);
                }

                ImGui::PushStyleColor(ImGuiCol_Border,  fcBorder);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, fcBg);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);

                bool cardClicked = false;
                bool cardHovered = false;
                if (ImGui::BeginChild("##fc", ImVec2(fileCardW, fileCardH),
                                      ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                    char title[GDX_COURSE_TITLE_MAX + 1];
                    const bool hasTitle = SessionTitle(entry.name, entry.extension, title, sizeof(title));
                    const char* displayName = hasTitle ? title
                                            : (entry.name[0] != '\0' ? entry.name : "(unnamed)");

                    // Name line.
                    if (isSelected) {
                        ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", displayName);
                    } else {
                        ImGui::TextUnformatted(displayName);
                    }

                    // Type + slot badge.
                    if (inSlot >= 0) {
                        ImGui::TextDisabled("%s \xe2\x80\xa2 Slot %d", GdxContentTypeLabel(entry), inSlot + 1);
                    } else {
                        ImGui::TextDisabled("%s", GdxContentTypeLabel(entry));
                    }

                    // Export button.
                    ImGui::BeginDisabled(busy);
                    if (ImGui::SmallButton(ICON_FA_UPLOAD " Export")) {
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
                    ImGui::EndDisabled();

                    // Full-card invisible click target layered on top (drawn last so buttons above
                    // still receive their own clicks first via ImGui's last-item priority).
                    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.06f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.10f));
                    ImGui::InvisibleButton("##hit", ImVec2(fileCardW, fileCardH));
                    cardClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                    cardHovered = ImGui::IsItemHovered();
                    ImGui::PopStyleColor(3);
                }
                ImGui::EndChild();
                // IsItemHovered on the child itself — covers the whole card including the border.
                cardHovered = cardHovered || ImGui::IsItemHovered();

                ImGui::PopStyleVar();    // FrameRounding
                ImGui::PopStyleColor(2); // Border, ChildBg

                // Card click: either complete a slot-first assignment or select this track.
                if (cardClicked && editableTrack && !opsDisabled) {
                    if (mCupPickerSlot >= 0 && mCupPickerSlot < GDX_CONTENT_CUP_SLOT_COUNT &&
                        (mCupPickerTrack < 0 || mCupPickerTrack >= mEntryCount)) {
                        const int targetSlot = mCupPickerSlot;
                        int rc = gdx_content_cup_register_begin(entry.name, targetSlot);
                        if (rc == GDX_CONTENT_OK) {
                            mCupWaiting = true;
                            mCupPickerSlot = -1;
                        } else {
                            snprintf(mStatus, sizeof(mStatus), "Cup registration failed: %s.",
                                     gdx_content_error_string(rc));
                        }
                    } else if (mCupPickerTrack == i) {
                        mCupPickerTrack = -1;
                        LoadTrackPreview(-1);
                        mCupPickerSlot = -1;
                        mCupArmedSlot = -1;
                        mCupArmedUnregister = -1;
                    } else {
                        mCupPickerTrack = i;
                        LoadTrackPreview(i);
                        mCupPickerSlot = -1;
                        mCupArmedSlot = -1;
                        mCupArmedUnregister = -1;
                        RefreshCupSlots();
                    }
                }

                // Tooltip for non-editable entries.
                if (cardHovered && !editableTrack) {
                    ImGui::BeginTooltip();
                    ImGui::TextDisabled("Machines and locked tracks cannot be added to the Edit Cup.");
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
                visibleCol++;
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(); // outer ChildBg transparent wrapper
    ImGui::SameLine(0.0f, gapX);
    if (ImGui::BeginChild("TrackPreviewPanel", ImVec2(0.0f, 280.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        DrawTrackPreview();
    }
    ImGui::EndChild();

    DrawImportSection(busy, inGameplay);

    // ── Advanced Sources (collapsible accordions) ────────────────────────────────────────────────
    DrawImageImportSection(busy, inGameplay);
    DrawRomImportSection(busy, inGameplay);

    if (mStatus[0] != '\0') {
        ImGui::Separator();
        ImGui::TextWrapped("%s", mStatus);
    }
}

// Appends formatted text to a status buffer, truncating at the buffer end rather than overflowing.
static void AppendStatus(char* buf, size_t cap, const char* fmt, ...) {
    size_t used = strlen(buf);
    va_list args;

    if (used + 1 >= cap) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(buf + used, cap - used, fmt, args);
    va_end(args);
}

// Plain-words reason for a sidecar decode failure, for the Content Library status line.
static const char* SidecarDecodeReason(int32_t code) {
    switch (code) {
        case GDX_COURSE_EXT_ERR_TRUNCATED:
            return "the file is cut short";
        case GDX_COURSE_EXT_ERR_MAGIC:
            return "it is not a GXE1 file";
        case GDX_COURSE_EXT_ERR_VERSION:
            return "its version is not supported";
        case GDX_COURSE_EXT_ERR_COUNT:
            return "it lists too many records";
        case GDX_COURSE_EXT_ERR_CRC:
            return "its checksum does not match";
        case GDX_COURSE_EXT_ERR_BINDING:
            return "it belongs to a different course";
        case GDX_COURSE_EXT_ERR_RECORD:
            return "a record is malformed or repeated";
        case GDX_COURSE_EXT_ERR_LAYOUT:
            return "its records do not fit the file";
        case GDX_COURSE_ERR_SCOPE:
            return "a point index is out of range";
        default:
            return "it could not be decoded";
    }
}

// Adds what the course element sidecar (.gdxe) of the last import reported. The sidecar is advisory:
// an unknown or broken one never blocks an import, the course just keeps its stock element values.
static void AppendSidecarStatus(char* status, size_t cap) {
    GdxContentImportExtension ext;

    memset(&ext, 0, sizeof(ext));
    if (gdx_content_import_last_extension(&ext) != GDX_CONTENT_OK) {
        return;
    }
    switch (ext.status) {
        case GDX_CONTENT_EXT_OK:
            if (ext.missingCount == 0) {
                if (ext.recordCount > 0) {
                    AppendStatus(status, cap, "\nCourse elements: all %d resolved.", (int) ext.resolvedCount);
                }
                break;
            }
            AppendStatus(status, cap, "\nCourse elements: %d resolved, %d not in this build (stock values kept): ",
                         (int) ext.resolvedCount, (int) ext.missingCount);
            for (int32_t i = 0; i < ext.missingStored; i++) {
                AppendStatus(status, cap, "%s%s", i > 0 ? ", " : "", ext.missingKeys[i]);
            }
            if (ext.missingCount > ext.missingStored) {
                AppendStatus(status, cap, ", and %d more", (int) (ext.missingCount - ext.missingStored));
            }
            AppendStatus(status, cap, ".");
            break;
        case GDX_CONTENT_EXT_SIDECAR_MISSING:
            AppendStatus(status, cap, "\nCourse elements: the .gdxe sidecar is missing, so stock values are used.");
            break;
        case GDX_CONTENT_EXT_SIDECAR_INVALID:
            AppendStatus(status, cap,
                         "\nCourse elements: the .gdxe sidecar was rejected because %s. Stock values are used.",
                         SidecarDecodeReason(ext.decodeCode));
            break;
        case GDX_CONTENT_EXT_IO:
            AppendStatus(status, cap, "\nCourse elements: the .gdxe sidecar could not be read. Stock values are used.");
            break;
        default:
            break;
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
        if (rc == GDX_CONTENT_OK) {
            AppendSidecarStatus(mStatus, sizeof(mStatus));
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Import from exports/");
    ImGui::TextDisabled("%d .gdxp/.gdxc file%s found", mImportCount, mImportCount == 1 ? "" : "s");
    if (inGameplay) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Import is unavailable while a race is live.");
    }

    // Helper banner — overwrite warning (R4 risk communication).
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
    ImGui::TextWrapped("Importing will overwrite existing files with matching names.");
    ImGui::PopStyleColor();

    if (mImportCount == 0) {
        ImGui::TextDisabled("Drop a .gdxp or .gdxc file into the exports/ folder next to the executable, then refresh.");
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

void GdxContentWindow::CloseRomCourses() {
    if (mRomCourses != nullptr) {
        gdx_romcourse_close(mRomCourses);
        mRomCourses = nullptr;
    }
    mRomEntryCount = 0;
    mRomArmed = -1;
    mRomHasBaseline = false;
    mRomTouchesNonCourse = false;
    mRomHeaderChecksumChanged = false;
}

// Runs the shared validation chain for one ROM row against its current (possibly edited) name.
// Same chain gdx_content_import_begin_payload re-runs authoritatively at click time.
void GdxContentWindow::ValidateRomRow(int row) {
    if (row < 0 || row >= mRomEntryCount || mRomCourses == nullptr) {
        return;
    }
    GdxRomCourseEntry& entry = mRomEntries[row];
    GdxContentImportEntry& out = mRomImportRows[row];
    memset(&out, 0, sizeof(out));
    out.classFileCount = -1;
    out.bundleTrackCount = -1;

    uint8_t* payload = static_cast<uint8_t*>(malloc(GDX_ROMCOURSE_PAYLOAD_SIZE));
    if (payload == nullptr) {
        out.errors = GDX_IMPORT_ERR_IO;
        return;
    }
    int rc = gdx_romcourse_payload(mRomCourses, entry.index, payload, GDX_ROMCOURSE_PAYLOAD_SIZE);
    if (rc != GDX_ROMCOURSE_OK) {
        out.errors = GDX_IMPORT_ERR_IO;
    } else {
        // Courses install as CRSD, the finished playable form the Edit Cup references.
        gdx_content_import_validate_payload(mRomNames[row], "CRSD", GDX_CONTENT_TYPE_TRACK, payload,
                                            GDX_ROMCOURSE_PAYLOAD_SIZE, &out);
    }
    free(payload);
}

void GdxContentWindow::OpenRomCourses(const char* path) {
    // The player's clean ROM is the baseline that says which courses a hack actually changed. It
    // is the same path first-boot extraction used; when it is gone we still list every course,
    // just without the changed/unchanged split.
    const char* baseline = (gdx_rom_path[0] != '\0') ? gdx_rom_path : nullptr;

    GdxRomCourseSet* set = nullptr;
    int rc = gdx_romcourse_open(path, baseline, &set);
    if (rc == GDX_ROMCOURSE_ERR_NO_BASELINE && baseline != nullptr) {
        // A missing or moved clean ROM must not block the import outright.
        rc = gdx_romcourse_open(path, nullptr, &set);
    }
    if (rc != GDX_ROMCOURSE_OK) {
        snprintf(mStatus, sizeof(mStatus), "Could not read the ROM: %s.", gdx_romcourse_strerror(rc));
        return;
    }

    CloseRomCourses();
    mRomCourses = set;
    mRomHasBaseline = gdx_romcourse_has_baseline(set) != 0;
    mRomTouchesNonCourse = gdx_romcourse_touches_non_course(set) != 0;
    mRomHeaderChecksumChanged = gdx_romcourse_header_checksum_changed(set) != 0;

    // With no baseline there is no "changed" to filter on, so show everything.
    if (!mRomHasBaseline) {
        mRomChangedOnly = false;
    }
    int count = gdx_romcourse_list(mRomCourses, mRomEntries, GDX_ROMCOURSE_COUNT, mRomChangedOnly ? 1 : 0);
    if (count < 0) {
        snprintf(mStatus, sizeof(mStatus), "Could not list the ROM's courses: %s.", gdx_romcourse_strerror(count));
        CloseRomCourses();
        return;
    }
    mRomEntryCount = count;

    for (int i = 0; i < mRomEntryCount; i++) {
        // Seed the editable name from the course's own internal id, truncated to the MFS limit.
        snprintf(mRomNames[i], sizeof(mRomNames[i]), "%s", mRomEntries[i].name);
        ValidateRomRow(i);
    }

    if (mRomHasBaseline) {
        snprintf(mStatus, sizeof(mStatus), "ROM read: %d course%s differ from your clean ROM.",
                 gdx_romcourse_changed_count(mRomCourses),
                 gdx_romcourse_changed_count(mRomCourses) == 1 ? "" : "s");
    } else {
        snprintf(mStatus, sizeof(mStatus), "ROM read: %d courses (no clean ROM to compare against).",
                 mRomEntryCount);
    }
}

void GdxContentWindow::DrawRomImportSection(bool busy, bool inGameplay) {
    ImGui::Separator();
    // Advanced source: collapsible accordion so it does not crowd the main library view.
    if (!ImGui::CollapsingHeader(ICON_FA_FOLDER " Import courses from a ROM (EXPERIMENTAL)")) {
        return;
    }

    ImGui::TextWrapped("A course inside an F-Zero X ROM and a Course Edit track are the same structure, so a "
                       "course-table ROM hack (FZEP tracks, New Lap, and the like) can be read straight into your "
                       "Content Library — no ROM swap, no separate save. Patch your own clean ROM with any "
                       "patcher first, then point this at the result. Both ROMs are only read, never written.");

    ImGui::InputTextWithHint("##romcoursepath", "Path to a patched .z64 / .n64 / .v64 ROM...", mRomInput,
                             sizeof(mRomInput));
    ImGui::SameLine();
#ifdef _WIN32
    if (ImGui::Button("Browse...##rom")) {
        GdxPickRom(mRomInput, sizeof(mRomInput));
    }
    ImGui::SameLine();
#endif
    if (ImGui::Button("Read ROM")) {
        if (mRomInput[0] == '\0') {
            snprintf(mStatus, sizeof(mStatus), "Enter or pick a ROM path first.");
        } else {
            OpenRomCourses(mRomInput);
        }
    }
    if (mRomCourses != nullptr) {
        ImGui::SameLine();
        if (ImGui::Button("Close ROM")) {
            CloseRomCourses();
        }
    }

    if (mRomCourses == nullptr) {
        return;
    }

    if (mRomHasBaseline) {
        if (ImGui::Checkbox("Only courses this ROM changed", &mRomChangedOnly)) {
            int count = gdx_romcourse_list(mRomCourses, mRomEntries, GDX_ROMCOURSE_COUNT, mRomChangedOnly ? 1 : 0);
            mRomEntryCount = (count > 0) ? count : 0;
            mRomArmed = -1;
            for (int i = 0; i < mRomEntryCount; i++) {
                snprintf(mRomNames[i], sizeof(mRomNames[i]), "%s", mRomEntries[i].name);
                ValidateRomRow(i);
            }
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "No clean ROM available to compare against, so every course is listed.");
    }

    if (mRomTouchesNonCourse) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "This ROM also changes data outside the course table.");
        ImGui::TextWrapped("Courses below still import normally. Anything else the hack changes (music, textures, "
                           "code) cannot come across this way and needs the ROM-hack path instead.");
    }
    if (mRomHeaderChecksumChanged) {
        // Benign and common: patchers recalculate the header CRC so the ROM still boots. Said out
        // loud so the difference is not mistaken for a silent one.
        ImGui::TextDisabled("The ROM header checksum was recalculated. That is normal after patching and "
                            "affects no course data.");
    }

    if (mRomEntryCount == 0) {
        ImGui::TextDisabled(mRomChangedOnly ? "This ROM's courses are identical to your clean ROM."
                                            : "No readable courses in this ROM.");
        return;
    }

    if (!ImGui::BeginTable("GdxRomImportTable", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingFixedFit,
                           ImVec2(0.0f, 220.0f))) {
        return;
    }
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Save as");
    ImGui::TableSetupColumn("Course");
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();

    for (int i = 0; i < mRomEntryCount; i++) {
        GdxRomCourseEntry& entry = mRomEntries[i];
        GdxContentImportEntry& row = mRomImportRows[i];
        const bool valid = row.errors == 0;
        const bool stateUnknown = row.errors == GDX_IMPORT_ERR_STATE_UNKNOWN;
        const bool needsConfirm =
            (row.warnings & (GDX_IMPORT_WARN_OVERWRITE | GDX_IMPORT_WARN_TWIN_DELETE | GDX_IMPORT_WARN_CUP_CLEAR)) !=
            0;
        ImGui::PushID(4000 + i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.symbol != nullptr ? entry.symbol : "(slot)");
        if (entry.changed) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "changed");
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputText("##romname", mRomNames[i], sizeof(mRomNames[i]))) {
            ValidateRomRow(i);
            mRomArmed = -1;
        }

        ImGui::TableNextColumn();
        ImGui::Text("id %s", entry.name[0] != '\0' ? entry.name : "(none)");
        ImGui::TextDisabled("%d points, venue %d", (int) entry.controlPointCount, (int) entry.venue);

        ImGui::TableNextColumn();
        if (valid) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "OK");
        } else if (stateUnknown) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Unknown");
            ImGui::TextWrapped("Disk state not probed (disk busy or still mounting); close and reopen the ROM in a "
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
        const bool armed = mRomArmed == i;
        if (ImGui::SmallButton(needsConfirm && !armed ? "Import..." : (needsConfirm ? "Confirm" : "Import"))) {
            if (needsConfirm && !armed) {
                mRomArmed = i;
            } else {
                mRomArmed = -1;
                // Re-read the course at click time: never trust the list's earlier snapshot.
                uint8_t* payload = static_cast<uint8_t*>(malloc(GDX_ROMCOURSE_PAYLOAD_SIZE));
                if (payload == nullptr) {
                    snprintf(mStatus, sizeof(mStatus), "Out of memory reading '%s' from the ROM.", mRomNames[i]);
                } else {
                    int readRc = gdx_romcourse_payload(mRomCourses, entry.index, payload, GDX_ROMCOURSE_PAYLOAD_SIZE);
                    if (readRc != GDX_ROMCOURSE_OK) {
                        snprintf(mStatus, sizeof(mStatus), "Could not read that course from the ROM: %s.",
                                 gdx_romcourse_strerror(readRc));
                    } else {
                        int irc = gdx_content_import_begin_payload(mRomNames[i], "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                                   payload, GDX_ROMCOURSE_PAYLOAD_SIZE);
                        if (irc == GDX_CONTENT_OK) {
                            // The shared completion poll in DrawImportSection reports and refreshes.
                            mImportWaiting = true;
                        } else if (irc == GDX_CONTENT_IMPORT_ERR_VALIDATION) {
                            snprintf(mStatus, sizeof(mStatus),
                                     "'%s' failed its re-check; close and reopen the ROM to refresh.", mRomNames[i]);
                        } else if (irc == GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY) {
                            snprintf(mStatus, sizeof(mStatus), "Import is unavailable while a race is live.");
                        } else {
                            snprintf(mStatus, sizeof(mStatus), "Import of '%s' failed: %s.", mRomNames[i],
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

void GdxContentWindow::DrawImageImportSection(bool busy, bool inGameplay) {
    ImGui::Separator();
    // Advanced source: collapsible accordion so it does not crowd the main library view.
    if (!ImGui::CollapsingHeader(ICON_FA_FOLDER " Import from a 64DD Disk Image (EXPERIMENTAL)")) {
        return;
    }

    ImGui::TextWrapped("Reads tracks and machines straight out of a 64DD disk dump from an emulator or console "
                       "— no .gdxc conversion step needed. Accepts .ndd, .ndr, .d6r and .n64dd (Project64, "
                       "Mupen64Plus, RMG, ares, 64DD Dump Tool), .disk and .disk_save (ares, MAME, RetroArch), "
                       ".d64, and raw .ram partition dumps (MiSTer). The extension is not what decides: the image "
                       "is identified by its size and filesystem, so a renamed dump still opens. It is only read, "
                       "never written.");

    ImGui::InputTextWithHint("##diskimagepath",
                             "Path to a 64DD disk image (.ndd, .ndr, .d6r, .n64dd, .disk, .disk_save, .d64, .ram)...",
                             mDiskImageInput, sizeof(mDiskImageInput));
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
