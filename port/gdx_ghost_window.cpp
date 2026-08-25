#include "gdx_ghost_window.h"

#include <imgui.h>

#include <cstdio>

// Name tables from the decomp (common.c): gTrackNames is the same index space the game's
// own ghost UI uses (menus.c:2616), including user-named edit-cup tracks once
// func_8070D220 resolves them; gMachineNames covers the 36 roster/super machines.
extern "C" {
extern char* gTrackNames[];
extern const char* gMachineNames[];
}

namespace {

const char* GdxFormatTime(int32_t timeMs, char* buffer, size_t bufferSize) {
    if (timeMs < 0) {
        timeMs = 0;
    }
    snprintf(buffer, bufferSize, "%d'%02d.%03d", timeMs / 60000, (timeMs / 1000) % 60, timeMs % 1000);
    return buffer;
}

// The tables are lowercase because the in-game font only has capitals; ImGui has both.
const char* GdxTitleCase(const char* src, char* buffer, size_t bufferSize) {
    bool wordStart = true;
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < bufferSize; i++) {
        char c = src[i];
        if (wordStart && c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        buffer[i] = c;
        wordStart = (c == ' ' || c == '-');
    }
    buffer[i] = '\0';
    return buffer;
}

int32_t GdxBestLap(const GdxGhostLibraryEntry& entry) {
    int32_t best = -1;
    for (int i = 0; i < 3; i++) {
        if (entry.lapTimes[i] > 0 && (best < 0 || entry.lapTimes[i] < best)) {
            best = entry.lapTimes[i];
        }
    }
    return best;
}

} // namespace

void GdxGhostWindow::RefreshLibrary() {
    int count = gdx_ghost_library_list(mEntries, GDX_GHOST_LIBRARY_MAX_ENTRIES);
    if (count < 0) {
        mEntryCount = 0;
        snprintf(mStatus, sizeof(mStatus), "Could not read the ghost library (code %d).", count);
        return;
    }

    mEntryCount = count;
    bool selectionFound = false;
    for (int i = 0; i < mEntryCount; i++) {
        if (mEntries[i].encodedCourseIndex == mSelectedEncodedCourse && mEntries[i].ghostId == mSelectedGhostId) {
            selectionFound = true;
            break;
        }
    }
    if (!selectionFound) {
        mSelectedEncodedCourse = mEntryCount > 0 ? mEntries[0].encodedCourseIndex : 0;
        mSelectedGhostId = mEntryCount > 0 ? mEntries[0].ghostId : 0;
    }
}

void GdxGhostWindow::InitElement() {
    RefreshLibrary();
}

void GdxGhostWindow::UpdateElement() {
}

void GdxGhostWindow::DrawElement() {
    ImGui::TextWrapped("Player ghosts are kept as separate local/imported replays. Select up to three "
                       "opponents for each exact course, then choose Player Ghost in Time Attack. "
                       "Staff ghosts and their unlock rules remain unchanged.");

    if (ImGui::Button("Refresh")) {
        RefreshLibrary();
        snprintf(mStatus, sizeof(mStatus), "Ghost library refreshed.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d saved player ghost%s", mEntryCount, mEntryCount == 1 ? "" : "s");
    ImGui::Separator();

    if (mEntryCount == 0) {
        ImGui::TextDisabled("No player ghosts are saved in the PC library yet.");
        ImGui::TextWrapped("Use the game's Save Ghost prompt, enable autosave-on-record, or import a "
                           ".gdg file. Existing cartridge-slot data is migrated automatically.");
    } else if (ImGui::BeginTable("GdxGhostLibraryTable", 8,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingFixedFit,
                                 ImVec2(0.0f, 260.0f))) {
        ImGui::TableSetupColumn("Race");
        ImGui::TableSetupColumn("Course");
        ImGui::TableSetupColumn("Total time");
        ImGui::TableSetupColumn("Best lap");
        ImGui::TableSetupColumn("Machine");
        ImGui::TableSetupColumn("Body");
        ImGui::TableSetupColumn("Exact course ID");
        ImGui::TableSetupColumn("Ghost ID");
        ImGui::TableHeadersRow();

        for (int i = 0; i < mEntryCount; i++) {
            GdxGhostLibraryEntry& entry = mEntries[i];
            char total[32];
            char lap[32];
            int32_t bestLap = GdxBestLap(entry);
            bool selected = entry.encodedCourseIndex == mSelectedEncodedCourse && entry.ghostId == mSelectedGhostId;

            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool raceThisGhost = entry.selected != 0;
            if (ImGui::Checkbox("##Race", &raceThisGhost)) {
                int rc = gdx_ghost_library_set_selected(entry.encodedCourseIndex, entry.ghostId,
                                                        raceThisGhost ? 1 : 0);
                if (rc == GDX_GHOST_OK) {
                    entry.selected = raceThisGhost ? 1 : 0;
                    snprintf(mStatus, sizeof(mStatus), raceThisGhost ? "Ghost selected for Time Attack."
                                                                     : "Ghost removed from Time Attack selection.");
                } else if (rc == GDX_GHOST_ERR_SELECTION_FULL) {
                    snprintf(mStatus, sizeof(mStatus), "This course already has three selected ghosts.");
                } else {
                    snprintf(mStatus, sizeof(mStatus), "Could not update ghost selection (code %d).", rc);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Race this ghost in Time Attack (maximum three per exact course).");
            }
            ImGui::TableSetColumnIndex(1);
            char courseLabel[48];
            if (entry.courseIndex >= 0 && entry.courseIndex < 55 && gTrackNames[entry.courseIndex] != nullptr &&
                gTrackNames[entry.courseIndex][0] != '\0') {
                GdxTitleCase(gTrackNames[entry.courseIndex], courseLabel, sizeof(courseLabel));
            } else {
                snprintf(courseLabel, sizeof(courseLabel), "Course %d", entry.courseIndex + 1);
            }
            if (ImGui::Selectable(courseLabel, selected)) {
                mSelectedEncodedCourse = entry.encodedCourseIndex;
                mSelectedGhostId = entry.ghostId;
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(GdxFormatTime(entry.raceTime, total, sizeof(total)));
            ImGui::TableSetColumnIndex(3);
            if (bestLap >= 0) {
                ImGui::TextUnformatted(GdxFormatTime(bestLap, lap, sizeof(lap)));
            } else {
                ImGui::TextDisabled("unavailable");
            }
            ImGui::TableSetColumnIndex(4);
            if (entry.character < 36 && gMachineNames[entry.character] != nullptr) {
                char machineLabel[48];
                ImGui::TextUnformatted(GdxTitleCase(gMachineNames[entry.character], machineLabel,
                                                    sizeof(machineLabel)));
            } else {
                ImGui::Text("Character #%u", static_cast<unsigned int>(entry.character));
            }
            ImGui::TableSetColumnIndex(5);
            ImVec4 body(entry.bodyR / 255.0f, entry.bodyG / 255.0f, entry.bodyB / 255.0f, 1.0f);
            ImGui::ColorButton("##Body", body, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                               ImVec2(14.0f, 14.0f));
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("0x%08X", static_cast<unsigned int>(entry.encodedCourseIndex));
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%016llX", static_cast<unsigned long long>(entry.ghostId));
            ImGui::PopID();
        }
        ImGui::EndTable();

        ImGui::BeginDisabled(mSelectedEncodedCourse == 0 || mSelectedGhostId == 0);
        if (ImGui::Button("Export selected to .gdg")) {
            char path[1024];
            if (!gdx_ghost_default_path(path, sizeof(path))) {
                snprintf(mStatus, sizeof(mStatus), "Export failed: could not resolve output path.");
            } else {
                int rc = gdx_ghost_library_export(mSelectedEncodedCourse, mSelectedGhostId, path);
                if (rc == GDX_GHOST_OK) {
                    snprintf(mStatus, sizeof(mStatus), "Exported selected ghost to %s", path);
                } else {
                    snprintf(mStatus, sizeof(mStatus), "Export failed (code %d).", rc);
                }
            }
        }
        ImGui::EndDisabled();
    }

    if (mStatus[0] != '\0') {
        ImGui::Separator();
        ImGui::TextWrapped("%s", mStatus);
    }

    ImGui::Separator();
    ImGui::TextWrapped("Selected persistent ghosts take priority and fill the game's native three ghost "
                       "slots. Any remaining slots keep compatible same-course session ghosts. Imported "
                       ".gdg files can therefore be raced without replacing another saved replay.");
}
