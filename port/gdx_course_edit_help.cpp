// port/gdx_course_edit_help.cpp -- Course Edit controls help for the editor mouse drive.
//
// Three surfaces, all driven by live state:
//
//   1. Hint strip (GdxCourseEditDrawHintStrip). A small translucent panel at the bottom left of the
//      game view naming the active point tool, its key, and its context actions. The Ctrl/Alt line
//      appears for the width, bank, and offset tools and lights up while the modifier is held. A
//      refusal message from GdxCourseEditFlashMessage shows at the top of the strip for two seconds.
//      The strip is drawn from the Course Edit overlay and stays hidden when a menu or sub-screen is
//      open. Point mode shows the tool lines; DESIGN and PARTS show how a click picks a segment.
//      gEnhancements.Input.EditorHintStrip = 0 turns it off.
//
//   2. Controls Help (GdxCourseEditDrawControlsHelp). The control map shown under Enhancements >
//      Mouse in the F1 menu. The gesture button and the tool keys are read from the live settings.
//
//   3. Flash message (GdxCourseEditFlashMessage). gdx_course_edit_mouse.cpp calls it when the line-up
//      pre-check refuses, and course_edit/188850.c calls it through gdx_course_edit_flash_message when
//      an Insert, Split or Link is refused, so the reason is shown next to the strip.
//
// The editor gate (CourseEditEditorActive and CourseEditMouseInSubMenu) sits in an anonymous namespace
// in gdx_course_edit_mouse.cpp, so the subset this file needs is rebuilt from the exported natives in
// gdx_course_edit_input.h.

#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"
#include "gdx_course_edit_input.h"
#include <imgui.h>

#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger

// Decomp globals. Plain int matches the decomp's int32_t on this target, the same idiom
// gdx_course_edit_mouse.cpp uses. gInCourseEditTestRun is a decomp bool.
extern "C" int gGameMode;
extern "C" bool gInCourseEditTestRun;
extern "C" int gCreateOption;
extern "C" int gMoveOption;
extern "C" int gDesignStyleOption;

// Exported by gdx_course_edit_mouse.cpp.
extern "C" const char* gdx_course_edit_action_name(int index);
extern "C" const char* gdx_course_edit_mouse_key_name(int index);
extern "C" int gdx_course_edit_mouse_fine_held(void);
extern "C" int gdx_course_edit_mouse_snap_held(void);

// Decomp naming overlay (decomp/src/overlays/expansion_kit/A6340.c).
extern "C" int32_t gdx_name_entry_is_active(void);

namespace {

constexpr int kGameWidth = 320;
constexpr int kGameHeight = 240;

// Strip anchor and width cap, in 320x240 game space. The right edge stays left of x = 230, where the
// bottom-right option cluster sits (CourseEditMouseOverChrome in gdx_course_edit_mouse.cpp).
constexpr float kStripLeft = 6.0f;
constexpr float kStripBottom = 234.0f;
constexpr float kStripMaxWidth = 220.0f;

// Move options 0..4 are the point drag tools, 5 is Delete, and 6 is line up.
constexpr int kToolCount = 7;

// How long a refusal message stays in the strip.
constexpr auto kFlashDuration = std::chrono::seconds(2);
char sFlashText[160] = {};
std::chrono::steady_clock::time_point sFlashUntil{};

std::shared_ptr<Fast::Fast3dWindow> HelpWindow() {
    return std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
}

// Mirrors CourseEditEditorActive() in gdx_course_edit_mouse.cpp, except for one case. That helper keeps
// the editor active during a point drag under a native grab, which needs the private drag state. This
// gate fails closed instead: it hides the strip for any native grab that the mouse does not own.
bool EditorActive() {
    if ((gGameMode & 0x1F) != 0xD /* GAMEMODE_COURSE_EDIT */ || gInCourseEditTestRun) {
        return false;
    }
    if (gdx_course_edit_native_grab_state() == 1 && gdx_course_edit_native_mouse_owned() == 0) {
        return false;
    }
    return true;
}

// Mirrors CourseEditMouseInSubMenu() for Course Edit, the only mode the strip draws in.
bool EditorSubScreenOpen() {
    if (gdx_course_edit_native_context_state() != 0) {
        return true;
    }
    return gdx_course_edit_native_help_active() != 0;
}

bool FlashActive() {
    return sFlashText[0] != '\0' && std::chrono::steady_clock::now() < sFlashUntil;
}

// Ctrl and Alt state for the modifier line. Lit while the modifier is held.
void DrawModifierLine() {
    const ImVec4 dim(0.55f, 0.55f, 0.55f, 1.0f);
    const ImVec4 lit(1.0f, 0.85f, 0.35f, 1.0f);
    const bool fine = gdx_course_edit_mouse_fine_held() != 0;
    const bool snap = gdx_course_edit_mouse_snap_held() != 0;
    ImGui::TextColored(fine ? lit : dim, "Ctrl fine");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(dim, " / ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(snap ? lit : dim, "Alt snap");
}

// Context lines under the strip title, one case per move option. Drag tools list the drag first,
// then the selection and cancel lines that apply to them. Width, bank, and offset end with the
// Ctrl/Alt line. The Delete and line-up tools list their click and key actions.
void DrawToolLines(int tool) {
    switch (tool) {
        case 0: // Move point (horizontal)
            ImGui::TextUnformatted("Drag a point: move in X and Z");
            ImGui::TextUnformatted("Shift+click: add or remove a point");
            ImGui::TextUnformatted("Shift+drag empty space: box select");
            ImGui::TextDisabled("Right-click or Esc: cancel the drag");
            break;
        case 1: // Move point (height)
            ImGui::TextUnformatted("Drag a point: raise or lower it");
            ImGui::TextUnformatted("Shift+click: add or remove a point");
            ImGui::TextUnformatted("Shift+drag empty space: box select");
            ImGui::TextDisabled("Right-click or Esc: cancel the drag");
            break;
        case 2: // Track width
            ImGui::TextUnformatted("Drag a point left or right: sets width");
            ImGui::TextDisabled("Right-click or Esc: cancel the drag");
            DrawModifierLine();
            break;
        case 3: // Bank/tilt
            ImGui::TextUnformatted("Drag a point left or right: sets bank");
            ImGui::TextDisabled("Right-click or Esc: cancel the drag");
            DrawModifierLine();
            break;
        case 4: // Center offset
            ImGui::TextUnformatted("Drag a point left or right: sets offset");
            ImGui::TextDisabled("Right-click or Esc: cancel the drag");
            DrawModifierLine();
            break;
        case 5: // Delete
            ImGui::TextUnformatted("Click a point: delete it");
            ImGui::TextUnformatted("Drag empty space: box select");
            ImGui::TextUnformatted("Delete key: remove the selection");
            ImGui::TextDisabled("Esc: clear the selection");
            break;
        case 6: // Line up
            ImGui::TextUnformatted("Click points: add or remove them");
            ImGui::TextUnformatted("Drag empty space: box select");
            ImGui::TextUnformatted("Enter: line up a run of 3 or more");
            ImGui::TextDisabled("Esc: clear the selection");
            break;
        default:
            break;
    }
}

// Title and lines for the DESIGN and PARTS screens, where a click picks the segment under the cursor and applies the
// selected design or part to it (the PORT pick in course_edit/188850.c).
void DrawSegmentLines(int createOption) {
    const ImVec4 title(1.0f, 0.85f, 0.35f, 1.0f);
    if (createOption == 3 /* CREATE_OPTION_PARTS */) {
        ImGui::TextColored(title, "Parts");
        ImGui::TextUnformatted("Click the track: place the part on that segment");
        ImGui::TextDisabled("Clicks away from the track do nothing");
        return;
    }
    if (gDesignStyleOption == 9 /* TRACK_DESIGN_STYLE_INSERT */) {
        ImGui::TextColored(title, "Insert");
        ImGui::TextUnformatted("Click a segment: add a point halfway along it");
        ImGui::TextDisabled("Not the start segment. Up to 60 points");
        return;
    }
    if (gDesignStyleOption == 10 /* TRACK_DESIGN_STYLE_SPLIT */) {
        ImGui::TextColored(title, "Split");
        ImGui::TextUnformatted("Click a segment: fork it into path A and path B");
        ImGui::TextDisabled("Split again on a fork to switch between path A and path B");
        return;
    }
    if (gDesignStyleOption == 11 /* TRACK_DESIGN_STYLE_LINK */) {
        ImGui::TextColored(title, "Link");
        ImGui::TextUnformatted("Choose first node: click a segment of a fork");
        ImGui::TextDisabled("Then click the point where path B rejoins");
        return;
    }
    ImGui::TextColored(title, "Design");
    ImGui::TextUnformatted("Click the track: apply the design to that segment");
    ImGui::TextDisabled("Clicks away from the track do nothing");
}

// Name of the camera gesture button, from the same CVar the editor camera reads.
const char* GestureButtonName() {
    const int button = CVarGetInteger("gEnhancements.Input.EditorCameraGestureButton", 0);
    if (button == 1) {
        return "X1 (back side) button";
    }
    if (button == 2) {
        return "X2 (forward side) button";
    }
    return "middle mouse button";
}

void HelpSection(const char* title) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", title);
}

void HelpRow(const char* control, const char* action) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextWrapped("%s", control);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", action);
}

} // namespace

// Shows a short message at the top of the hint strip for two seconds. Used when an action is refused, so the reason
// is visible. The text is copied, so the caller's buffer may be reused.
void GdxCourseEditFlashMessage(const char* text) {
    std::snprintf(sFlashText, sizeof(sFlashText), "%s", text != nullptr ? text : "");
    sFlashUntil = std::chrono::steady_clock::now() + kFlashDuration;
}

// C entry for the decomp (course_edit/188850.c), which reports Insert, Split and Link refusals through the strip.
extern "C" void gdx_course_edit_flash_message(const char* text) {
    GdxCourseEditFlashMessage(text);
}

// Called from the Course Edit overlay each frame, under the selection box. Draws nothing unless the editor mouse
// drive is on, the editor is active in point mode, and no menu, sub-screen, name entry, or mouse capture is up.
void GdxCourseEditDrawHintStrip(void) {
    if (CVarGetInteger("gEnhancements.Input.EditorHintStrip", 1) == 0) {
        return;
    }
    if (CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0 || !EditorActive()) {
        return;
    }
    // Point mode shows the tool lines. DESIGN (2) and PARTS (3) show how a click picks a segment.
    const bool pointMode = gCreateOption == 1 /* CREATE_OPTION_POINT */;
    if (!pointMode && gCreateOption != 2 && gCreateOption != 3) {
        return;
    }
    if (pointMode && (gMoveOption < 0 || gMoveOption >= kToolCount)) {
        return;
    }

    std::shared_ptr<Fast::Fast3dWindow> wnd = HelpWindow();
    if (wnd == nullptr || wnd->IsMouseCaptured() || wnd->GetGui()->GetMenuOrMenubarVisible() ||
        EditorSubScreenOpen() || gdx_name_entry_is_active() != 0) {
        return;
    }
    Fast::Fast3dGui* gui = dynamic_cast<Fast::Fast3dGui*>(wnd->GetGui().get());
    float rx = 0.0f;
    float ry = 0.0f;
    float rw = 0.0f;
    float rh = 0.0f;
    if (gui == nullptr || !gui->GetGameBlitRect(&rx, &ry, &rw, &rh) || rw <= 0.0f || rh <= 0.0f) {
        return;
    }

    // Convert the 320x240 game-space anchor through the pillarbox blit rect, the same mapping the
    // cursor drive uses in reverse.
    const float sx = rw / static_cast<float>(kGameWidth);
    const float sy = rh / static_cast<float>(kGameHeight);
    const ImVec2 anchor(rx + kStripLeft * sx, ry + kStripBottom * sy);

    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(kStripMaxWidth * sx, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.8f, 0.3f, 0.35f));

    if (ImGui::Begin("##CourseEditHintStrip", nullptr, flags)) {
        ImGui::SetWindowFontScale(0.9f);
        if (FlashActive()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", sFlashText);
        }
        if (pointMode) {
            const int key = gMoveOption + 1;
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "[%s]", gdx_course_edit_mouse_key_name(key));
            ImGui::SameLine();
            ImGui::TextUnformatted(gdx_course_edit_action_name(key));
            DrawToolLines(gMoveOption);
        } else {
            DrawSegmentLines(gCreateOption);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// Read-only control map for the Enhancements > Mouse page. Tool rows read the live tool-key bindings
// and the gesture row reads the live gesture button, so a rebound key or button shows here.
void GdxCourseEditDrawControlsHelp(void) {
    ImGui::TextDisabled("Read-only. Applies in Course Edit point mode while the editor mouse drive is on.");
    ImGui::SetItemTooltip("The keys and buttons below are the live bindings, so a rebound control shows its new name.");
    if (!ImGui::BeginTable("##CourseEditControlsHelp", 2,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.40f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.60f);

    char hold[128];
    char holdShift[160];
    snprintf(hold, sizeof(hold), "Hold %s and move", GestureButtonName());
    snprintf(holdShift, sizeof(holdShift), "Shift + hold %s and move", GestureButtonName());

    HelpSection("Camera");
    HelpRow(hold, "Orbit the camera");
    HelpRow(holdShift, "Pan the camera");
    HelpRow("Mouse wheel", "Zoom in and out, in steps proportional to the distance");

    HelpSection("Selection");
    HelpRow("Left-click a point", "Select only that point. In the Delete tool, delete it");
    HelpRow("Shift + left-click", "Add or remove the point");
    HelpRow("Left-drag empty space (Delete tool)", "Box select. Replaces the selection");
    HelpRow("Shift + left-drag empty space", "Box select. Adds the points inside the box");
    HelpRow("Left-click empty space", "Clear the selection");
    HelpRow("Esc", "Cancel a drag, or clear the selection");

    HelpSection("Editing");
    HelpRow("Left-drag a point", "Apply the current tool");
    HelpRow("Left or right mouse movement", "Width, bank, and offset tools. Farther zoom moves faster");
    HelpRow("Hold Ctrl", "Fine: quarter speed");
    HelpRow("Hold Alt", "Snap the change: width and offset in steps of 50, bank in steps of 15 degrees");
    HelpRow("Right-click", "Cancel a drag");
    HelpRow("Delete",
            "Delete every selected point, in any point tool. With nothing selected, a sound plays and nothing changes");
    HelpRow("Enter (line-up tool)",
            "Line up the selected run on a straight line. A message explains a refusal");
    HelpRow("Click a point (line-up tool)", "Add it to the selection or remove it. Neither A nor B is sent");
    HelpRow("Ctrl + Z", "Undo the last change");
    HelpRow("Ctrl + Y, or Ctrl + Shift + Z", "Redo the last undone change");

    HelpSection("Design and Parts");
    HelpRow("Left-click the track",
            "Picks the segment under the cursor and applies the selected design or part to it. A click away from "
            "the track does nothing. Z still picks a segment without applying");
    HelpRow("Left-click a segment (Insert design)",
            "Adds a point halfway along the segment. Refused, with the reason in the hint strip, on the start "
            "segment, under 4 or at 60 points, on a track at the length limit, or when a neighbour would be under "
            "498 units away");
    HelpRow("Left-click a segment (Split design)",
            "Forks the segment into two paths, one point each, on either side of the road. Insert inside a split "
            "fork adds a point to both paths. Split again on a fork to switch the editor between path A and path B: "
            "the tools then edit the path that is showing. Up to 4 forks, not on the start or last segment");
    HelpRow("Left-click twice (Link design)",
            "Choose first node: click a segment of a fork. Then click the point where its path B rejoins the "
            "course. Refused with \"Travel path broken\" when the path cannot rejoin there");

    HelpSection("Tools");
    for (int i = 1; i <= kToolCount; i++) {
        char keyLabel[48];
        snprintf(keyLabel, sizeof(keyLabel), "Key %s", gdx_course_edit_mouse_key_name(i));
        HelpRow(keyLabel, gdx_course_edit_action_name(i));
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Tool keys are rebindable in the Course Edit tool keys block above.");
}
