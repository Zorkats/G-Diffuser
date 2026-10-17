// port/gdx_course_edit_history.cpp -- Course Edit History panel (undo and redo list).
//
// A small window that lists the undo stack oldest first, a "Current state" marker, and the redo stack nearest
// first. Clicking a row jumps to that state through gdx_course_edit_undo_request_row. The Undo and Redo buttons
// use the same request functions as the Ctrl+Z and Ctrl+Y hook. Each row shows the label derived when the edit
// was made, and a tooltip with the control point count of the state it restores.
//
// The window is off unless gEnhancements.CourseEdit.HistoryPanel is set, and it draws only in Course Edit
// outside a test run. The Course Edit mouse code is meant to call GdxCourseEditDrawHistoryPanel each frame,
// beside the hint strip.

#include <cstdio>

#include <imgui.h>

#include "gdx_course_edit_undo.h"

#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger

// Decomp globals. gInCourseEditTestRun is a decomp bool, which is an int typedef in libc/stdbool.h.
extern "C" int gGameMode;
extern "C" bool gInCourseEditTestRun;

namespace {

constexpr float kPanelX = 12.0f;
constexpr float kPanelY = 60.0f;
constexpr float kPanelWidth = 240.0f;
constexpr float kPanelHeight = 280.0f;
// Seconds that a first press of Clear history stays armed.
constexpr double kClearArmSeconds = 3.0;

// Time of the first press of Clear history, or -1 when no press is armed.
double sClearArmedAt = -1.0;
// Undo count seen on the previous frame. A change scrolls the Current state marker into view.
int sLastUndoCount = -1;

// Mirrors the editor gate in gdx_course_edit_help.cpp: Course Edit mode and not inside a test run.
bool EditorActive() {
    // GAMEMODE_COURSE_EDIT is 0xD.
    if ((gGameMode & 0x1F) != 0xD) {
        return false;
    }
    if (gInCourseEditTestRun) {
        return false;
    }
    return true;
}

// Shows a tooltip for the last item. Unlike SetItemTooltip, this also works when the item is disabled.
void HoverTip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

// One row of the list. Redo rows are drawn grey.
void DrawHistoryRow(int row, bool isRedo) {
    const char* label = gdx_course_edit_undo_row_text(row);
    const int points = gdx_course_edit_undo_row_points(row);
    char text[96];
    std::snprintf(text, sizeof(text), "%d. %s", row + 1, label);

    ImGui::PushID(row);
    if (isRedo) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
    }
    if (ImGui::Selectable(text)) {
        gdx_course_edit_undo_request_row(row);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n%d control point%s", label, points, points == 1 ? "" : "s");
    }
    if (isRedo) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

} // namespace

// Draws the History window. Call it every frame from the Course Edit overlay, inside the ImGui frame.
void GdxCourseEditDrawHistoryPanel(void) {
    if (CVarGetInteger("gEnhancements.CourseEdit.HistoryPanel", 0) == 0 || !EditorActive()) {
        return;
    }

    const int undoCount = gdx_course_edit_undo_undo_count();
    const int redoCount = gdx_course_edit_undo_redo_count();
    const int total = undoCount + redoCount;

    ImGui::SetNextWindowPos(ImVec2(kPanelX, kPanelY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kPanelHeight), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Course Edit History", nullptr, 0)) {
        ImGui::BeginDisabled(undoCount == 0);
        if (ImGui::Button("Undo")) {
            gdx_course_edit_undo_request_undo();
        }
        ImGui::EndDisabled();
        HoverTip(undoCount == 0 ? "Nothing to undo." : "Undo the last edit (Ctrl+Z).");

        ImGui::SameLine();
        ImGui::BeginDisabled(redoCount == 0);
        if (ImGui::Button("Redo")) {
            gdx_course_edit_undo_request_redo();
        }
        ImGui::EndDisabled();
        HoverTip(redoCount == 0 ? "Nothing to redo." : "Redo the last undone edit (Ctrl+Shift+Z or Ctrl+Y).");

        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d", total, GDX_COURSE_EDIT_UNDO_CAPACITY);
        HoverTip("Snapshots kept for undo and redo together. The oldest are dropped when the list is full.");
        ImGui::Separator();

        // The footer below the list holds the Clear history button.
        ImGui::BeginChild("##CourseEditHistoryRows", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), false);
        if (total == 0) {
            ImGui::TextDisabled("No edits yet.");
        }
        for (int row = 0; row < undoCount; row++) {
            DrawHistoryRow(row, false);
        }
        ImGui::Separator();
        const bool moved = undoCount != sLastUndoCount;
        sLastUndoCount = undoCount;
        ImGui::Selectable("Current state", true);
        if (moved) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::Separator();
        for (int row = undoCount; row < total; row++) {
            DrawHistoryRow(row, true);
        }
        ImGui::EndChild();

        // Clear history takes two presses. The first press arms it, and the second press within
        // kClearArmSeconds clears. The arm is dropped when the pointer leaves the button.
        const bool canClear = total > 0;
        if (!canClear) {
            sClearArmedAt = -1.0;
        }
        ImGui::BeginDisabled(!canClear);
        const bool clearPressed = ImGui::Button(sClearArmedAt >= 0.0 ? "Click again to clear" : "Clear history");
        ImGui::EndDisabled();
        const bool clearHovered = ImGui::IsItemHovered();
        if (clearPressed) {
            if (sClearArmedAt >= 0.0) {
                gdx_course_edit_undo_clear();
                sClearArmedAt = -1.0;
            } else {
                sClearArmedAt = ImGui::GetTime();
            }
        } else if (sClearArmedAt >= 0.0 && (!clearHovered || ImGui::GetTime() - sClearArmedAt > kClearArmSeconds)) {
            sClearArmedAt = -1.0;
        }
        HoverTip(canClear ? "Forget the undo and redo history. The course itself is not changed."
                          : "Nothing to clear.");
    }
    ImGui::End();
}
