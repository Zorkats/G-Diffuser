// port/gdx_course_edit_parts_panel.cpp -- Course Edit panel for values the stock course file has no room for.
//
// Shows the selected point's part widths, magnet values, dash pad kind and wind, and the track's lava rule, magnet
// values and wind (gdx_course_settings.h). 0 on a slider means the point or track has no value of its own. Changes
// are queued for the game thread (gdx_course_edit_input_request_extra), which owns the edit state and the undo
// stack.
//
// The panel starts collapsed and sits under the icon row, on the right. While the mouse is over it,
// GdxCourseEditPartsPanelWantsMouse keeps the click out of the editor. gEnhancements.CourseEdit.PartSettingsPanel
// = 0 turns it off.

#include <cstdio>
#include <cmath>
#include <memory>

#include "gdx_wind.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"
#include "gdx_course_branch_edit.h"
#include "gdx_course_branch_editor.h"
#include "gdx_course_edit_input.h"
#include "gdx_track_recovery_runtime.h"
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
    if (gdx_recovery_blocks_writes()) {
        return false;
    }
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
        std::snprintf(shown, sizeof shown, "%s (%s)", notSetText, scale > 0.0f ? "%.2f" : "%d");
    } else if (scale > 0.0f) {
        std::snprintf(shown, sizeof shown, "%%.2f");
    } else {
        std::snprintf(shown, sizeof shown, "%%d");
    }
    ImGui::SetNextItemWidth(kSliderWidth);
    bool changed;
    if (scale > 0.0f) {
        float scaled = static_cast<float>(*value) / scale;
        changed = ImGui::SliderFloat(label, &scaled, 0.0f, static_cast<float>(max) / scale, shown,
                                     ImGuiSliderFlags_AlwaysClamp);
        if (changed) {
            *value = static_cast<int>(std::lround(scaled * scale));
        }
    } else {
        changed = ImGui::SliderInt(label, value, 0, max, shown, ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::SetItemTooltip("%s Ctrl+click the value to type it. 0 uses the inherited value.", tip);
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

// A dash pad's kind, its width and, on a Super pad, its boost (stored as percent minus 100, 0 for 150%).
void DrawPadControls(int point) {
    static const char* const kPadKinds[] = { "Normal", "Super", "Brake" };
    int kind = gdx_branch_edit_point_extra(point, GDX_BRANCH_EDIT_EXTRA_PAD_KIND);

    ImGui::SetNextItemWidth(kSliderWidth);
    if (ImGui::Combo("Dash pad", &kind, kPadKinds, IM_ARRAYSIZE(kPadKinds))) {
        QueueChange(false, point, GDX_BRANCH_EDIT_EXTRA_PAD_KIND, kind);
    }
    ImGui::SetItemTooltip("Super boosts harder and a little longer, in blue. Brake cuts your speed and points "
                          "backward, in red. CPUs do not aim for Brake pads. The course file keeps a normal dash, "
                          "so other builds see a normal pad.");
    PointSlider(point, GDX_BRANCH_EDIT_EXTRA_PAD_WIDTH, "Pad width", "Stock", 1, GDX_SETTINGS_WIDTH_MAX, 0.0f,
                "How much of the road the pad covers, on flat road. Pipes and cylinders keep the stock plate.");
    if (kind == GDX_SETTINGS_PAD_SUPER) {
        int boost = gdx_branch_edit_point_extra(point, GDX_BRANCH_EDIT_EXTRA_PAD_BOOST);
        int percent = (boost == 0) ? 0 : (boost + 100);
        char shown[32];

        if (boost == 0) {
            std::snprintf(shown, sizeof shown, "Default (%d%%%%)", GDX_SETTINGS_PAD_BOOST_DEFAULT + 100);
        } else {
            std::snprintf(shown, sizeof shown, "%%d%%%%");
        }
        ImGui::SetNextItemWidth(kSliderWidth);
        if (ImGui::SliderInt("Super boost", &percent, 100, GDX_SETTINGS_PAD_BOOST_MAX + 100, shown,
                             ImGuiSliderFlags_AlwaysClamp)) {
            /* 100 is the slider's "not set" end; under 110 snaps there or to 110. */
            int stored = 0;

            if (percent > 100) {
                stored = percent - 100;
                if (stored < GDX_SETTINGS_PAD_BOOST_MIN) {
                    stored = (stored < GDX_SETTINGS_PAD_BOOST_MIN / 2) ? 0 : GDX_SETTINGS_PAD_BOOST_MIN;
                }
            }
            QueueChange(false, point, GDX_BRANCH_EDIT_EXTRA_PAD_BOOST, stored);
        }
        ImGui::SetItemTooltip("The Super pad's speed floor, as a share of a normal dash. The left end uses %d%%. "
                              "Ctrl+click the value to type it.",
                              GDX_SETTINGS_PAD_BOOST_DEFAULT + 100);
    }
}

constexpr float kCompassSize = 36.0f;
const char* const kAngleTip = "The way the wind blows. 0 is north, the top of the race map, and 90 is east. "
                              "Click or drag the compass, or Ctrl+click the value to type it.";

// A compass angle: a slider, and a compass that shows the wind's way and sets it on a click or drag.
void AngleControl(const char* id, int angle, bool isTrack, int point, int which) {
    int value = angle;

    ImGui::PushID(id);
    ImGui::SetNextItemWidth(kSliderWidth - kCompassSize - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::SliderInt("##angle", &value, 0, GDX_SETTINGS_WIND_ANGLE_MAX, "%d deg", ImGuiSliderFlags_AlwaysClamp)) {
        QueueChange(isTrack, point, which, value);
    }
    ImGui::SetItemTooltip("%s", kAngleTip);
    ImGui::SameLine();

    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const float radius = kCompassSize * 0.5f;
    const ImVec2 center(corner.x + radius, corner.y + radius);

    ImGui::InvisibleButton("##compass", ImVec2(kCompassSize, kCompassSize));
    if (ImGui::IsItemActive()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float dx = mouse.x - center.x;
        const float dy = mouse.y - center.y;

        if ((dx * dx) + (dy * dy) > 4.0f) {
            // Screen up is north and screen right is east, as on the race map.
            int picked = static_cast<int>(std::lround(std::atan2(dx, -dy) * 180.0f / 3.14159265f));

            picked = (picked + 360) % 360;
            if (picked != angle) {
                QueueChange(isTrack, point, which, picked);
                value = picked;
            }
        }
    }
    ImGui::SetItemTooltip("%s", kAngleTip);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 ring = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 arrow = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    float x;
    float z;

    gdx_wind_direction(value, &x, &z);
    // z is world Z, and world -Z is screen up.
    const ImVec2 tip(center.x + x * (radius - 4.0f), center.y + z * (radius - 4.0f));
    const ImVec2 tail(center.x - x * (radius - 8.0f), center.y - z * (radius - 8.0f));
    const ImVec2 side(-z * 4.0f, x * 4.0f);
    const ImVec2 back(tip.x - x * 7.0f, tip.y - z * 7.0f);

    draw->AddCircle(center, radius - 1.0f, ring, 24, 1.0f);
    draw->AddText(ImVec2(center.x - ImGui::CalcTextSize("N").x * 0.5f, corner.y - 1.0f), text, "N");
    draw->AddLine(tail, back, arrow, 2.0f);
    draw->AddTriangleFilled(tip, ImVec2(back.x + side.x, back.y + side.y), ImVec2(back.x - side.x, back.y - side.y),
                            arrow);
    ImGui::PopID();
}

// The wind on a point's segment: the WIND part's presets, an exact strength, and its angle.
void DrawPointWind(int point) {
    static const char* const kPresets[] = { "None", "Weak", "Mid", "Strong", "Gale" };
    static const int kPresetStrengths[] = { 0, 25, 50, 75, 100 };
    const int strength = gdx_branch_edit_point_extra(point, GDX_BRANCH_EDIT_EXTRA_WIND_STRENGTH);

    const char* preview = nullptr;
    for (int i = 0; i < IM_ARRAYSIZE(kPresets); i++) {
        if (strength == kPresetStrengths[i]) {
            preview = kPresets[i];
        }
    }
    ImGui::SetNextItemWidth(kSliderWidth);
    if (ImGui::BeginCombo("Wind", preview != nullptr ? preview : "Custom")) {
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); i++) {
            const bool picked = strength == kPresetStrengths[i];
            if (ImGui::Selectable(kPresets[i], picked) && !picked) {
                QueueChange(false, point, GDX_BRANCH_EDIT_EXTRA_WIND_STRENGTH, kPresetStrengths[i]);
            }
            if (picked) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The wind on the segment from this point to the next. Tunnels always block it. The WIND "
                          "part in Parts places it too; set its angle here.");
    if (strength == 0) {
        return;
    }
    int value = strength;

    ImGui::SetNextItemWidth(kSliderWidth);
    if (ImGui::SliderInt("Wind strength", &value, 1, GDX_SETTINGS_WIND_MAX, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        QueueChange(false, point, GDX_BRANCH_EDIT_EXTRA_WIND_STRENGTH, value);
    }
    ImGui::SetItemTooltip("How hard the wind pushes here. It pushes twice as hard in the air, and harder while you "
                          "slide. Ctrl+click the value to type it.");
    AngleControl("PointWindAngle", gdx_branch_edit_point_extra(point, GDX_BRANCH_EDIT_EXTRA_WIND_ANGLE), false, point,
                 GDX_BRANCH_EDIT_EXTRA_WIND_ANGLE);
    ImGui::SameLine();
    ImGui::TextUnformatted("Wind angle");
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
        ImGui::TextDisabled("No pit, lava, magnet, dirt, ice or dash pad here.");
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
    if ((parts & GDX_BRANCH_EDITOR_PART_DASH) != 0) {
        DrawPadControls(point);
    }
    DrawPointWind(point);
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

    static const char* const kAiPresets[] = { "Track", "Calm", "Normal", "Fierce", "Brutal" };
    int ai = gdx_branch_edit_track_setting(GDX_BRANCH_EDIT_TRACK_AI);

    ImGui::SetNextItemWidth(kSliderWidth);
    if (ImGui::Combo("CPU aggression", &ai, kAiPresets, IM_ARRAYSIZE(kAiPresets))) {
        QueueChange(true, 0, GDX_BRANCH_EDIT_TRACK_AI, ai);
    }
    ImGui::SetItemTooltip("How often CPUs side attack, spin attack and block on this track, on top of the\n"
                          "difficulty. Track keeps the player's AI aggression option. Calm halves it,\n"
                          "Fierce is 1.5x and Brutal doubles it. Normal sets AI aggression to 100% on this\n"
                          "track, in place of the option. Past Normal, CPUs below Expert also spin attack.");
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
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##CourseEditPartsPanel", nullptr, flags)) {
        const int path = gdx_course_branch_editor_path_showing();
        if (path != 0) {
            ImGui::TextUnformatted(path == 2 ? "Path B showing" : "Path A showing");
            ImGui::BeginDisabled(gdx_course_edit_native_grab_state() != 0 ||
                                 gdx_course_edit_native_mouse_owned() != 0);
            if (ImGui::RadioButton("Path A", path == 1)) {
                gdx_course_edit_input_request_path(1);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Path B", path == 2)) {
                gdx_course_edit_input_request_path(2);
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped("Point: select a white node. Course: Ctrl+click a white node.");
            ImGui::Separator();
        }
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
