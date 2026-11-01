// port/gdx_course_edit_parts_panel.cpp -- Course Edit panel for values the stock course file has no room for.
//
// Shows the selected point's part widths and magnet values, and the track's lava rule and magnet values
// (gdx_course_settings.h). 0 on a slider means the point or track has no value of its own. Changes are queued
// for the game thread (gdx_course_edit_input_request_extra), which owns the edit state and the undo stack.
//
// The panel starts collapsed and sits under the icon row, on the right. While the mouse is over it,
// GdxCourseEditPartsPanelWantsMouse keeps the click out of the editor. gEnhancements.CourseEdit.PartSettingsPanel
// = 0 turns it off.

#include <cstdio>
#include <memory>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"
#include "gdx_course_branch_edit.h"
#include "gdx_course_branch_editor.h"
#include "gdx_course_edit_input.h"
#include "gdx_course_settings.h"
#include <imgui.h>

#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger

// Decomp globals, the same idiom gdx_course_edit_help.cpp uses.
extern "C" int gGameMode;
extern "C" bool gInCourseEditTestRun;
extern "C" int32_t gdx_name_entry_is_active(void);

namespace {

constexpr float kGameWidth = 320.0f;
constexpr float kGameHeight = 240.0f;
// Right edge and top of the panel in 320x240 game space, under the icon row.
constexpr float kPanelRight = 314.0f;
constexpr float kPanelTop = 58.0f;
constexpr float kPanelWidth = 260.0f;
constexpr float kSliderWidth = 110.0f;

bool sWantsMouse = false;
// A slider drag is one undo step: true from its first change until it is released.
bool sEditOpen = false;

bool PanelAllowed() {
    if (CVarGetInteger("gEnhancements.CourseEdit.PartSettingsPanel", 1) == 0) {
        return false;
    }
    if ((gGameMode & 0x1F) != 0xD /* GAMEMODE_COURSE_EDIT */ || gInCourseEditTestRun) {
        return false;
    }
    if (gdx_course_edit_native_grab_state() == 1 && gdx_course_edit_native_mouse_owned() == 0) {
        return false;
    }
    return gdx_course_edit_native_context_state() == 0 && gdx_course_edit_native_help_active() == 0 &&
           gdx_name_entry_is_active() == 0;
}

void QueueChange(bool isTrack, int point, int which, int value) {
    GdxCourseEditExtraRequest request;

    request.is_track = isTrack ? 1 : 0;
    request.point = point;
    request.which = which;
    request.value = value;
    request.begin_edit = sEditOpen ? 0 : 1;
    gdx_course_edit_input_request_extra(&request);
    sEditOpen = true;
}

// A value of 0 ("not set") or snapMin..max, as the store requires. scale > 0 shows value / scale.
bool ValueSlider(const char* label, const char* notSetText, int* value, int snapMin, int max, float scale,
                 const char* tip) {
    char shown[32];

    if (*value == 0) {
        std::snprintf(shown, sizeof shown, "%s", notSetText);
    } else if (scale > 0.0f) {
        std::snprintf(shown, sizeof shown, "%.2f", static_cast<float>(*value) / scale);
    } else {
        std::snprintf(shown, sizeof shown, "%d", *value);
    }
    ImGui::SetNextItemWidth(kSliderWidth);
    bool changed = ImGui::SliderInt(label, value, 0, max, shown, ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("%s", tip);
    if (changed && *value > 0 && *value < snapMin) {
        *value = (*value < snapMin / 2) ? 0 : snapMin;
    }
    return changed;
}

void PointSlider(int point, int which, const char* label, const char* notSetText, int snapMin, int max, float scale,
                 const char* tip) {
    int value = gdx_branch_edit_point_extra(point, which);

    if (ValueSlider(label, notSetText, &value, snapMin, max, scale, tip)) {
        QueueChange(false, point, which, value);
    }
}

void TrackSlider(int which, const char* label, int snapMin, int max, float scale, const char* tip) {
    int value = gdx_branch_edit_track_setting(which);

    if (ValueSlider(label, "Menu", &value, snapMin, max, scale, tip)) {
        QueueChange(true, 0, which, value);
    }
}

void DrawPointSection() {
    const int point = gdx_course_branch_editor_active_point();
    const char* widthTip = "How much of the road the part covers. Stock keeps the normal width.";

    if (point < 0) {
        ImGui::TextDisabled("No point selected.");
        return;
    }
    ImGui::Text("Point %d", point + 1);
    const int parts = gdx_course_branch_editor_point_parts(point);

    if (parts == 0) {
        ImGui::TextDisabled("No pit, lava, magnet, dirt or ice here.");
        return;
    }
    if ((parts & GDX_BRANCH_EDITOR_PART_PIT) != 0) {
        PointSlider(point, GDX_BRANCH_EDIT_EXTRA_PIT_WIDTH, "Pit or lava width", "Stock", 1, GDX_SETTINGS_WIDTH_MAX, 0.0f,
                    widthTip);
    }
    if ((parts & GDX_BRANCH_EDITOR_PART_DIRT) != 0) {
        PointSlider(point, GDX_BRANCH_EDIT_EXTRA_DIRT_WIDTH, "Dirt width", "Stock", 1, GDX_SETTINGS_WIDTH_MAX, 0.0f,
                    widthTip);
    }
    if ((parts & GDX_BRANCH_EDITOR_PART_ICE) != 0) {
        PointSlider(point, GDX_BRANCH_EDIT_EXTRA_ICE_WIDTH, "Ice width", "Stock", 1, GDX_SETTINGS_WIDTH_MAX, 0.0f,
                    widthTip);
    }
    if ((parts & GDX_BRANCH_EDITOR_PART_MAGNET) != 0) {
        PointSlider(point, GDX_BRANCH_EDIT_EXTRA_MAGNET_STRENGTH, "Magnet strength", "Track", GDX_SETTINGS_STRENGTH_MIN,
                    GDX_SETTINGS_STRENGTH_MAX, GDX_SETTINGS_STRENGTH_SCALE,
                    "How hard this magnet pulls. Track uses the value below.");
        PointSlider(point, GDX_BRANCH_EDIT_EXTRA_MAGNET_RANGE, "Magnet range", "Track", GDX_SETTINGS_RANGE_MIN,
                    GDX_SETTINGS_RANGE_MAX, 0.0f, "How far this magnet reaches. Track uses the value below.");
    }
}

void DrawTrackSection() {
    static const char* const kLavaModes[] = { "Follow the menu", "Always kills", "Never kills" };
    int lava = gdx_branch_edit_track_setting(GDX_BRANCH_EDIT_TRACK_LAVA);

    ImGui::SetNextItemWidth(kSliderWidth);
    if (ImGui::Combo("Lava", &lava, kLavaModes, IM_ARRAYSIZE(kLavaModes))) {
        QueueChange(true, 0, GDX_BRANCH_EDIT_TRACK_LAVA, lava);
    }
    ImGui::SetItemTooltip("Whether 0 energy on lava kills on this track. Test runs never kill.");
    TrackSlider(GDX_BRANCH_EDIT_TRACK_STRENGTH, "Strength", GDX_SETTINGS_STRENGTH_MIN, GDX_SETTINGS_STRENGTH_MAX,
                GDX_SETTINGS_STRENGTH_SCALE, "Magnet strength on this track. Menu uses the Gameplay option.");
    TrackSlider(GDX_BRANCH_EDIT_TRACK_RANGE, "Range", GDX_SETTINGS_RANGE_MIN, GDX_SETTINGS_RANGE_MAX, 0.0f,
                "Magnet range on this track. Menu uses the Gameplay option.");
    if (CVarGetInteger("gEnhancements.Gameplay.IgnoreTrackSettings", 0) != 0) {
        ImGui::TextDisabled("Ignore track settings is on, so you race on the menu's values.");
    }
}

} // namespace

// 1 while the mouse belongs to the panel. Read by gdx_course_edit_mouse.cpp.
int GdxCourseEditPartsPanelWantsMouse(void) {
    return sWantsMouse ? 1 : 0;
}

// Called from the Course Edit overlay each frame.
void GdxCourseEditDrawPartsPanel(void) {
    sWantsMouse = false;
    if (!PanelAllowed()) {
        sEditOpen = false;
        return;
    }
    std::shared_ptr<Fast::Fast3dWindow> wnd =
        std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
    if (wnd == nullptr || wnd->IsMouseCaptured() || wnd->GetGui()->GetMenuOrMenubarVisible()) {
        sEditOpen = false;
        return;
    }
    Fast::Fast3dGui* gui = dynamic_cast<Fast::Fast3dGui*>(wnd->GetGui().get());
    float rx = 0.0f;
    float ry = 0.0f;
    float rw = 0.0f;
    float rh = 0.0f;
    if (gui == nullptr || !gui->GetGameBlitRect(&rx, &ry, &rw, &rh) || rw <= 0.0f || rh <= 0.0f) {
        sEditOpen = false;
        return;
    }

    const ImVec2 anchor(rx + kPanelRight * (rw / kGameWidth), ry + kPanelTop * (rh / kGameHeight));

    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.8f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##CourseEditPartsPanel", nullptr, flags)) {
        if (ImGui::CollapsingHeader("Point and track settings")) {
            DrawPointSection();
            ImGui::Separator();
            ImGui::TextUnformatted("Whole track");
            DrawTrackSection();
        }
        sWantsMouse = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                             ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                      ImGui::IsAnyItemActive() ||
                      ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
    ImGui::End();
    if (!ImGui::IsAnyItemActive()) {
        sEditOpen = false;
    }
}
