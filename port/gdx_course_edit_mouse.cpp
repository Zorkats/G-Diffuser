// G-Diffuser: Course Edit / Create Machine absolute mouse drive + issue #18 race mouse steering.
//
// TWO SEPARATE MECHANISMS share this file only because both are thin extern-"C" wrappers over
// Fast3dWindow for decomp-side C callers:
//
//   1. Editor absolute drive (gdx_course_edit_mouse_pos): maps the window mouse position back
//      through the pillarbox blit rect (Fast3dGui::GetGameBlitRect) into the 320x240 space the
//      editor cursor lives in. Used by both Course Edit cursor drivers
//      (decomp/src/overlays/course_edit/188850.c) and the Create Machine update loop; the callers
//      keep their own stock clamps, so this shim deliberately does not know per-mode ranges.
//      Native controller grab suppresses this drive because the controller owns geometry. Mouse-
//      owned transactions use the explicit input queue and native lifecycle instead.
//
//   2. Issue #18 race steering (gdx_mouse_steer_stick_x): absolute position steering for
//      port/input_bridge.c: cursor X inside the blit rect maps to stick deflection. Mode-disjoint
//      with the absolute drive above (race modes only), so the two never fight over the cursor.

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include "ship/window/gui/GuiWindow.h"
#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"
#include "gdx_course_branch_editor.h"
#include "gdx_course_edit_input.h"
#include "gdx_course_edit_undo.h"
#include <imgui.h>

#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger
#include "port_log.h"                                    // gdx_port_logf
#include <SDL2/SDL.h>

// decomp controller.h is not C++-clean under MSVC; the N64 button masks are stable ABI bits.
#define GDX_BTN_A 0x8000
#define GDX_BTN_B 0x4000
#define GDX_BTN_Z 0x2000

// Decomp globals. Plain int is ABI-identical to the decomp's int32_t on this target (same idiom as
// port/input_bridge.c); gInCourseEditTestRun is a decomp bool. GET_MODE masks the F3D-variant
// bits: GET_MODE(gGameMode) == (gGameMode & 0x1F), GAMEMODE_COURSE_EDIT == 0xD and
// GAMEMODE_CREATE_MACHINE == 0x10 (fzx_game.h).
extern "C" int gGameMode;
extern "C" bool gInCourseEditTestRun;
extern "C" int gWorksMachineMode;

// Aggregate and selection reads go through the native C accessors.
extern "C" int gCreateOption;
extern "C" int gMoveOption;
extern "C" int32_t gCourseEditCursorXPos;
extern "C" int32_t gCourseEditCursorYPos;
extern "C" int32_t D_xk1_8003A550;
extern "C" int32_t D_xk1_8003A554;

using Vec3f = GdxCourseEditVec3;

extern "C" int32_t func_xk2_800EFDE4(float maxDist);
extern "C" bool gdx_course_edit_unproject_to_plane(int32_t screenX, int32_t screenY, float planeY, Vec3f* outPos);
extern "C" bool gdx_course_edit_unproject_to_vertical_line(int32_t screenX, int32_t screenY, float fixedX, float fixedZ,
                                                            float* outY);

extern "C" int32_t func_xk2_800EF090(Vec3f pos, int32_t* outX, int32_t* outY);
extern "C" int32_t D_xk2_800F7040;
extern "C" void func_xk2_800EF78C(void);

// Defined in gdx_course_edit_help.cpp and gdx_course_edit_history.cpp.
void GdxCourseEditFlashMessage(const char* text);
void GdxCourseEditDrawHistoryPanel(void);

namespace {

std::shared_ptr<Fast::Fast3dWindow> CourseEditMouseWindow() {
    return std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
}

bool CourseEditMouseOverChrome(int32_t x, int32_t y);
bool CourseEditMouseInSubMenu(void);
bool CourseEditCtrlHeld(void);

// S3: point-drag state machine. Declared early so CourseEditMouseModeActive can allow an
// active mouse transaction to run while native grab state is set.
enum class DragState {
    Idle,
    Armed,      // LMB pressed over a point, or Shift-pressed on empty space; awaiting click vs drag threshold
    Starting,   // Native update has not yet accepted the drag transaction
    Dragging,   // Pointer moved >= 6 window pixels from the press; geometry moving
    Committing,
    Cancelling,
    BoxSelect,  // Shift-drag from empty space: rectangle on screen, selection added on release
};

// Squared window-pixel distance that separates a click from a drag (6 px).
constexpr int32_t kDragStartThresholdSq = 36;
// Squared pick radius in game pixels for mouse presses (about 12.25 px). The native hover uses the same value.
constexpr float kMousePickRadiusSq = 150.0f;

struct CourseEditDragState {
    DragState state = DragState::Idle;
    int32_t pointIndex = -1;
    int32_t startMouseX = 0;
    int32_t startMouseY = 0;
    Vec3f backupPos = {};
    int32_t moveOption = -1;
    int32_t lastMouseX = 0;
    int32_t lastMouseY = 0;
    // Sub-pixel game-space x at the press and on the last in-area frame, for the scalar tools.
    float startGameX = 0.0f;
    float lastGameX = 0.0f;
    bool released = false;
    // Press position in window pixels. The drag threshold is measured here, not in game space.
    Ship::Coords startWindow{};
    // Box select: game-space corner at the press, last in-game-area corner, and that corner in window pixels.
    float boxStartX = 0.0f;
    float boxStartY = 0.0f;
    float boxEndX = 0.0f;
    float boxEndY = 0.0f;
    Ship::Coords boxEndWindow{};
    // Set by a press that starts a box select: true for Shift (add to the selection), false to replace it.
    bool boxAdditive = false;
};
static CourseEditDragState sCourseEditDrag;
static bool sDragBlockedUntilRelease = false;

void CourseEditCancelDrag(void) {
    gdx_course_edit_input_clear_drag();
    if (gdx_course_edit_native_mouse_owned() != 0) {
        gdx_course_edit_input_request_drag_cancel();
        sCourseEditDrag.state = DragState::Cancelling;
    } else {
        sCourseEditDrag = {};
    }
}

// Applies the queued box-select add to the native selection. The native refresh flag is raised only when at
// least one point became selected.
void CourseEditFlushSelectionAdd(void) {
    GdxCourseEditMask mask = 0;
    if (!gdx_course_edit_input_take_selection_add_mask(&mask)) {
        return;
    }
    if (gdx_course_edit_native_add_selection_mask(mask) != 0) {
        D_xk2_800F7040 = 3;
    }
}

// Control points whose projected game-space position lies inside the box, edges inclusive, corners in any order.
// Points that are behind the camera or off the 320x240 screen are skipped.
GdxCourseEditMask CourseEditPointsInBox(float ax, float ay, float bx, float by) {
    const float minX = (std::min)(ax, bx);
    const float maxX = (std::max)(ax, bx);
    const float minY = (std::min)(ay, by);
    const float maxY = (std::max)(ay, by);
    GdxCourseEditMask mask = 0;
    const int32_t count = gdx_course_edit_native_point_count();
    for (int32_t i = 0; i < count; i++) {
        Vec3f pos = {};
        int32_t sx = 0;
        int32_t sy = 0;
        if (!gdx_course_edit_native_point_position(i, &pos) || func_xk2_800EF090(pos, &sx, &sy) != 0) {
            continue;
        }
        const float px = static_cast<float>(sx);
        const float py = static_cast<float>(sy);
        if (px >= minX && px <= maxX && py >= minY && py <= maxY) {
            mask |= UINT64_C(1) << i;
        }
    }
    return mask;
}

// Nearest control point to a float game-space cursor within kMousePickRadiusSq, or -1. Same squared-distance
// rule as func_xk2_800EFDE4 (strict less-than, so a tie keeps the lower index), with sub-pixel input.
int32_t CourseEditPickPoint(float cursorX, float cursorY) {
    int32_t best = -1;
    float bestDistSq = kMousePickRadiusSq;
    const int32_t count = gdx_course_edit_native_point_count();
    for (int32_t i = 0; i < count; i++) {
        Vec3f pos = {};
        int32_t sx = 0;
        int32_t sy = 0;
        if (!gdx_course_edit_native_point_position(i, &pos) || func_xk2_800EF090(pos, &sx, &sy) != 0) {
            continue;
        }
        const float dx = static_cast<float>(sx) - cursorX;
        const float dy = static_cast<float>(sy) - cursorY;
        const float distSq = dx * dx + dy * dy;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = i;
        }
    }
    return best;
}

// Course Edit test-run/grab suppression only applies to Course Edit; Create Machine has neither.
// Grab mode is tolerated while a point drag is active so the drag can run to commit/cancel.
bool CourseEditMouseModeActive(void) {
    switch (gGameMode & 0x1F) {
        case 0xD: // GAMEMODE_COURSE_EDIT
            if (gInCourseEditTestRun) {
                return false;
            }
            // A native controller grab owns the editor until it releases the grab.
            if (gdx_course_edit_native_grab_state() == 1 && gdx_course_edit_native_mouse_owned() == 0 &&
                (sCourseEditDrag.state == DragState::Idle || sCourseEditDrag.state == DragState::Armed ||
                 sCourseEditDrag.state == DragState::Starting || sCourseEditDrag.state == DragState::BoxSelect)) {
                return false;
            }
            return true;
        case 0x10: // GAMEMODE_CREATE_MACHINE
            return true;
        default:
            return false;
    }
}

// Explicit context separation: Course Edit editor is active ONLY in GAMEMODE_COURSE_EDIT (0xD)
// when not test-driving. Create Machine (0x10) and gameplay modes are strictly excluded so tool
// keys, dragging, and deletion never leak outside Course Edit.
bool CourseEditEditorActive(void) {
    if ((gGameMode & 0x1F) != 0xD /* GAMEMODE_COURSE_EDIT */) {
        return false;
    }
    if (gInCourseEditTestRun) {
        return false;
    }
    if (gdx_course_edit_native_grab_state() == 1 && gdx_course_edit_native_mouse_owned() == 0 &&
        (sCourseEditDrag.state == DragState::Idle || sCourseEditDrag.state == DragState::Armed ||
         sCourseEditDrag.state == DragState::Starting || sCourseEditDrag.state == DragState::BoxSelect)) {
        return false;
    }
    return true;
}

// Single shared definition for Course Edit action names (slots 1..7).
static const char* kCourseEditActionNames[8] = {
    nullptr,
    "Move point (horizontal)",
    "Move point (height)",
    "Track width",
    "Bank/tilt",
    "Center offset",
    "Delete selected points",
    "Line up points (Enter applies)",
};

extern "C" const char* gdx_course_edit_action_name(int index) {
    if (index >= 1 && index <= 7) {
        return kCourseEditActionNames[index];
    }
    return "Unknown";
}

// Modifier and key lifecycle state
static bool sLeftShiftHeld = false;
static bool sRightShiftHeld = false;
static bool sShiftHeld = false;
static bool sLeftCtrlHeld = false;
static bool sRightCtrlHeld = false;
static bool sCtrlHeld = false;
static bool sLeftAltHeld = false;
static bool sRightAltHeld = false;
static bool sAltHeld = false;
// Scalar drag tool state (width, bank, center offset). Cleared on release and on focus loss.
struct CourseEditScalarDragState {
    bool active = false;
    int32_t pointIndex = -1;
    int32_t moveOption = -1;
    int32_t startMouseX = 0;
    int32_t startMouseY = 0;
    float base = 0.0f;
    float anchorX = 0.0f;
    float rate = 0.0f;
    float lastOffset = 0.0f;
};
static CourseEditScalarDragState sScalarDrag;
static bool sEditorKeysConsumed[10] = {};
// Held and owned state for the keys that act on the selection, the history, and the line-up tool. An owned key is
// swallowed on both edges, so the native binding for the same key never fires while the editor uses it.
static bool sDeleteKeyHeld = false;
static bool sDeleteKeyOwned = false;
static bool sEnterHeld = false;
static bool sEnterOwned = false;
static bool sUndoZHeld = false;
static bool sUndoYHeld = false;
static bool sUndoZOwned = false;
static bool sUndoYOwned = false;
static bool sAKeyHeld = false;
static bool sAKeyOwned = false;
static bool sEditorEscOwned = false;

uint64_t CourseEditSelectionMask(void) {
    uint64_t mask = 0;
    int total = gdx_course_edit_native_point_count();
    for (int i = 0; i < total; i++) {
        if (gdx_course_edit_native_point_selected(i) != 0) {
            mask |= (UINT64_C(1) << i);
        }
    }
    return mask;
}

// Sub-menus, dialogs, and full sub-screens keep the in-game cursor but should not confine the
// OS cursor: the user may want to move the real cursor to the window edge or another monitor
// while a popup/overlay is open. Course Edit covers every native dialog state (dialogs,
// pickers, CREATE/POINT screens) plus the START help overlay that does not touch unk_08.
bool CourseEditMouseInSubMenu(void) {
    switch (gGameMode & 0x1F) {
        case 0xD: // GAMEMODE_COURSE_EDIT
            return (gdx_course_edit_native_context_state() != 0) || (gdx_course_edit_native_help_active() != 0);
        case 0x10: // GAMEMODE_CREATE_MACHINE
            return gWorksMachineMode != 0;
        default:
            return false;
    }
}

// Point-screen context for the keys and clicks that change the selection: Course Edit on the POINT screen, with no
// transaction, no native grab, no menu, and no sub-menu. The Course Edit mouse CVar gates it, as it gates the tool
// keys.
bool CourseEditPointInputContextActive(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    return CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) != 0 && CourseEditEditorActive() &&
           gCreateOption == 1 && gdx_course_edit_native_context_state() == 0 &&
           gdx_course_edit_native_grab_state() == 0 && gdx_course_edit_native_mouse_owned() == 0 &&
           wnd != nullptr && !wnd->IsMouseCaptured() && !wnd->GetGui()->GetMenuOrMenubarVisible() &&
           !CourseEditMouseInSubMenu();
}

// Replaces the native selection now, not through the input queue, so a delete queued in the same host frame sees the
// new selection. Raises the same refresh flag as CourseEditFlushSelectionAdd. An empty mask clears the selection.
void CourseEditReplaceSelection(GdxCourseEditMask mask) {
    gdx_course_edit_native_set_selection_mask(mask);
    D_xk2_800F7040 = 3;
}

// Deletes the selected points. An empty selection is refused with NA_SE_32. An accepted delete takes the undo backup
// first, and the game tick applies it through the native delete consumer.
void CourseEditRequestDelete(void) {
    const GdxCourseEditMask mask = CourseEditSelectionMask();
    if (mask == 0) {
        gdx_course_edit_native_play_refusal();
        return;
    }
    gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_DELETE);
    func_xk2_800EF78C();
    gdx_course_edit_input_request_delete(mask, gdx_course_edit_native_point_count());
}

// Delete tool click on a point. The backup comes before the click changes the selection, so an undo returns to the
// selection from before the click. The click then selects only that point, and the delete removes it.
void CourseEditDeleteClickedPoint(int32_t point) {
    const GdxCourseEditMask mask = UINT64_C(1) << point;
    gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_DELETE);
    func_xk2_800EF78C();
    CourseEditReplaceSelection(mask);
    gdx_course_edit_input_request_delete(mask, gdx_course_edit_native_point_count());
}

// Enter in the line-up tool. The pre-check runs the same scan the game tick runs and writes nothing. A refusal shows
// the reason beside the hint strip and plays NA_SE_32. An accepted line-up is queued, and the tick takes the undo
// backup before it runs.
void CourseEditRequestLineUp(void) {
    GdxCourseEditLineUpPreview preview = {};
    const int32_t result = gdx_course_edit_native_line_up_scan(&preview);
    if (result == GDX_COURSE_EDIT_LINE_UP_TOO_FEW) {
        GdxCourseEditFlashMessage("Select 3 or more points in a row");
    } else if (result == GDX_COURSE_EDIT_LINE_UP_TOO_CLOSE) {
        GdxCourseEditFlashMessage("Points would end up too close (under 498 units)");
    } else {
        gdx_course_edit_input_request_line_up();
        return;
    }
    gdx_course_edit_native_play_refusal();
}

// Orbit gain at 100% sensitivity: whole degrees of yaw or pitch per OS pixel.
static constexpr float kOrbitDegreesPerPixel = 0.5f;

// Camera gesture state for Slice 1 of better course-edit controls.
// Lives entirely in the port shim; decomp consumers only see the extern "C" getters below.
// Active only while MMB is held, the CVar is on, the editor is active, no ImGui menu/menubar is
// open, and the F2 relative-mode capture is not active. Once started, the gesture continues even
// if the OS cursor drifts outside the game blit rect (e.g. pillarbox bars) so a long drag does not
// cut off at the edge.
//
// Host frames run faster than the ~20 Hz Course Edit game tick. Motion is summed here on every host
// frame and drained by the getters once per game tick, so drags between ticks are not lost. The
// pending motion lives in this struct, so every reset (gesture end, mode exit, editor inactive)
// clears it as well.
struct CourseEditCameraGesture {
    bool active = false;
    bool shift = false;
    Ship::Coords lastPos{ -1, -1 };
    // Pending motion with the sensitivity already applied: orbit in degrees, pan in OS pixels.
    float orbitYawDeg = 0.0f;
    float orbitPitchDeg = 0.0f;
    float panDx = 0.0f;
    float panDy = 0.0f;
};
static CourseEditCameraGesture sCourseEditCameraGesture;

// S2/S3: keyboard shortcut and point-drag state for better course-edit controls.
// These live in the port shim; decomp code consumes keyboard choices and transaction commands
// through explicit boundaries. All hotkeys are only active while the Course Edit mouse gate is
// satisfied so they never leak into races or menus.
struct CourseEditKeyState {
    bool held[10] = {};
    bool pressedAccum[10] = {};
    bool pressedGame[10] = {};
};
static CourseEditKeyState sCourseEditKeys;

// Default bindings for the nine Course Edit tool keys, in LUS KbScancode space (PS/2 set-1,
// the numbering Fast3dWindow::KeyDown delivers on both the SDL and DXGI backends; see
// ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h).
static const int kDefaultEditorKeyScancodes[10] = {
    0,
    2, 3, 4, 5, 6, 7, 8, 9, 10, // LUS_KB_1 .. LUS_KB_9
};

// Runtime option helpers. Default ON so a user who only flips CourseEditMouse keeps the same
// behavior as before these toggles existed.
namespace {
int GdxOrbitSensitivity(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMouseOrbitSensitivity", 100);
}
int GdxPanSensitivity(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMousePanSensitivity", 100);
}
float GdxInvertSign(const char* cvar) {
    return CVarGetInteger(cvar, 0) != 0 ? -1.0f : 1.0f;
}
int GdxZoomSensitivity(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMouseZoomSensitivity", 100);
}
int GdxDragSensitivity(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMouseDragSensitivity", 100);
}

bool GdxCameraGesturesEnabled(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMouseCameraGestures", 1) != 0;
}
bool GdxWheelZoomEnabled(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMouseWheelZoom", 1) != 0;
}
bool GdxKeybindsEnabled(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMouseKeybinds", 1) != 0;
}
bool GdxDragEnabled(void) {
    return CVarGetInteger("gEnhancements.Input.EditorMouseDrag", 1) != 0;
}

int GdxGestureButton(void) {
    return CVarGetInteger("gEnhancements.Input.EditorCameraGestureButton", 0);
}

static const char* kEditorKeybindsVersionCVar = "gEnhancements.Input.EditorKeybindsVersion";

void GdxEnsureEditorKeybindsMigrated(void) {
    int version = CVarGetInteger(kEditorKeybindsVersionCVar, 0);
    if (version >= 2) {
        return;
    }
    // Check if configuration matches the old unversioned SDL defaults (30..36 for slots 1..7).
    // In that short-lived build, defaults were 29+index (SDL_SCANCODE_1 = 30 .. SDL_SCANCODE_7 = 36).
    // If all slots 1..7 match 29+index, migrate them to LUS defaults (2..8).
    bool allSdlDefaults = true;
    for (int i = 1; i <= 7; i++) {
        char cvar[64];
        snprintf(cvar, sizeof(cvar), "gEnhancements.Input.EditorKey%d", i);
        int val = CVarGetInteger(cvar, 0);
        if (val != 29 + i) {
            allSdlDefaults = false;
            break;
        }
    }
    if (allSdlDefaults) {
        for (int i = 1; i <= 7; i++) {
            char cvar[64];
            snprintf(cvar, sizeof(cvar), "gEnhancements.Input.EditorKey%d", i);
            CVarSetInteger(cvar, kDefaultEditorKeyScancodes[i]);
        }
    }
    CVarSetInteger(kEditorKeybindsVersionCVar, 2);
    CVarSave();
}

int GdxEditorKeyScancode(int index) {
    if (index < 1 || index > 7) {
        return 0;
    }
    GdxEnsureEditorKeybindsMigrated();
    char cvar[64];
    snprintf(cvar, sizeof(cvar), "gEnhancements.Input.EditorKey%d", index);
    return CVarGetInteger(cvar, kDefaultEditorKeyScancodes[index]);
}
} // namespace

// Key capture state for the menu key-rebinding widget. Active only while the menu is waiting for
// the next keydown; the capture itself is applied inside gdx_course_edit_mouse_on_key().
struct CourseEditKeyCapture {
    bool active = false;
    int index = 0; // 1..7 when active
    char conflictMsg[128] = {};
};
static CourseEditKeyCapture sCourseEditKeyCapture;

extern "C" const char* gdx_course_edit_mouse_key_conflict_msg(void) {
    return sCourseEditKeyCapture.conflictMsg[0] != '\0' ? sCourseEditKeyCapture.conflictMsg : nullptr;
}

extern "C" void gdx_course_edit_mouse_clear_conflict_msg(void) {
    sCourseEditKeyCapture.conflictMsg[0] = '\0';
}

// Per-frame update of the camera gesture state. Must run once per host frame, after
// MouseStateManager::StartFrame (called from main.cpp alongside the other mouse ticks).
void CourseEditMouseCameraTick(void) {
    CourseEditCameraGesture& g = sCourseEditCameraGesture;
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->IsMouseCaptured()) {
        g = {};
        return;
    }
    if (CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0 || !CourseEditEditorActive()) {
        g = {};
        return;
    }
    if (wnd->GetGui()->GetMenuOrMenubarVisible() || CourseEditMouseInSubMenu()) {
        g = {};
        return;
    }
    if (!GdxCameraGesturesEnabled()) {
        g = {};
        return;
    }

    Ship::Coords mouse = wnd->GetMousePos();
    Fast::Fast3dGui* gui = dynamic_cast<Fast::Fast3dGui*>(wnd->GetGui().get());
    float rx, ry, rw, rh;
    bool inBlitRect = false;
    if (gui != nullptr && gui->GetGameBlitRect(&rx, &ry, &rw, &rh) && rw > 0.0f && rh > 0.0f) {
        inBlitRect = (mouse.x >= rx && mouse.x < rx + rw && mouse.y >= ry && mouse.y < ry + rh);
    }

    bool gestureHeld = false;
    switch (GdxGestureButton()) {
        case 1:
            gestureHeld = wnd->GetMouseState(Ship::LUS_MOUSE_BTN_BACKWARD);
            break;
        case 2:
            gestureHeld = wnd->GetMouseState(Ship::LUS_MOUSE_BTN_FORWARD);
            break;
        case 0:
        default:
            gestureHeld = wnd->GetMouseState(Ship::LUS_MOUSE_BTN_MIDDLE);
            break;
    }
    if (!gestureHeld) {
        g = {};
        return;
    }

    // Require the first gesture press to land inside the blit rect to avoid starting a drag from a
    // pillarbox/letterbox dead area. Once started, keep tracking while the gesture button is held.
    if (!g.active && !inBlitRect) {
        g = {};
        return;
    }

    // The first frame of a gesture has no previous position, so it moves nothing.
    const float dx = g.active ? static_cast<float>(mouse.x - g.lastPos.x) : 0.0f;
    const float dy = g.active ? static_cast<float>(mouse.y - g.lastPos.y) : 0.0f;
    g.active = true;
    g.lastPos = mouse;

    const bool shift = sShiftHeld || ((SDL_GetModState() & KMOD_SHIFT) != 0);
    if (shift != g.shift) {
        // Shift switches orbit and pan mid-gesture. Motion the mode being left has not drained yet,
        // so drop it: it must not land in the other mode or resume when Shift is released.
        if (shift) {
            g.orbitYawDeg = 0.0f;
            g.orbitPitchDeg = 0.0f;
        } else {
            g.panDx = 0.0f;
            g.panDy = 0.0f;
        }
        g.shift = shift;
    }
    if (shift) {
        const float scale = GdxPanSensitivity() / 100.0f * GdxInvertSign("gEnhancements.Input.EditorMouseInvertPan");
        g.panDx += dx * scale;
        g.panDy += dy * scale;
    } else {
        // Signs match the D-pad: a drag right yaws like BTN_RIGHT, a drag up pitches like BTN_UP.
        const float scale = kOrbitDegreesPerPixel * GdxOrbitSensitivity() / 100.0f;
        g.orbitYawDeg -= dx * scale * GdxInvertSign("gEnhancements.Input.EditorMouseInvertOrbitX");
        g.orbitPitchDeg -= dy * scale * GdxInvertSign("gEnhancements.Input.EditorMouseInvertOrbitY");
    }
}

// Full S1/S4 gate: CVar on, in Course Edit or Create Machine, not test-driving (Course Edit only),
// not grabbing geometry (Course Edit only), no menu/menubar up, and the OS cursor inside the game
// blit rect. On success outX/outY receive the cursor position in 320x240 game space.
bool CourseEditMousePosF(std::shared_ptr<Fast::Fast3dWindow>& wnd, float* outX, float* outY) {
    if (CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0) {
        return false;
    }
    if (!CourseEditMouseModeActive()) {
        return false;
    }
    wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->GetGui()->GetMenuOrMenubarVisible()) {
        return false;
    }
    Fast::Fast3dGui* gui = dynamic_cast<Fast::Fast3dGui*>(wnd->GetGui().get());
    float rx, ry, rw, rh;
    if (gui == nullptr || !gui->GetGameBlitRect(&rx, &ry, &rw, &rh) || rw <= 0.0f || rh <= 0.0f) {
        return false;
    }
    Ship::Coords mouse = wnd->GetMousePos();
    if (mouse.x < rx || mouse.x >= rx + rw || mouse.y < ry || mouse.y >= ry + rh) {
        return false;
    }
    *outX = (mouse.x - rx) * 320.0f / rw;
    *outY = (mouse.y - ry) * 240.0f / rh;
    return true;
}

// Whole game-pixel form of CourseEditMousePosF (truncated toward zero, as before).
bool CourseEditMousePos(std::shared_ptr<Fast::Fast3dWindow>& wnd, int* outX, int* outY) {
    float x = 0.0f;
    float y = 0.0f;
    if (!CourseEditMousePosF(wnd, &x, &y)) {
        return false;
    }
    *outX = static_cast<int>(x);
    *outY = static_cast<int>(y);
    return true;
}

} // namespace

// Called from Course Edit cursor drivers and the Create Machine update loop under #ifdef PORT.
// 1 = outX/outY hold the absolute game-space cursor position and the caller must skip its stick
// accumulation (clamps and audio cues stay with the caller).
extern "C" int gdx_course_edit_mouse_pos(int* outX, int* outY) {
    std::shared_ptr<Fast::Fast3dWindow> wnd;
    return CourseEditMousePos(wnd, outX, outY) ? 1 : 0;
}

// S2: per-frame OS-cursor hide, called from main.cpp AFTER MouseStateManager::StartFrame. The
// manager re-shows the cursor on any movement, so hiding before its tick would flicker. Yields to
// F2 mouse capture (relative-mode look owns the cursor) and, via the gate, to menu-open.
extern "C" void gdx_course_edit_mouse_cursor_tick(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd;
    int x, y;
    if (!CourseEditMousePos(wnd, &x, &y) || wnd->IsMouseCaptured()) {
        return;
    }
    wnd->SetCursorVisibility(false);
}

static bool RaceMouseSteeringActive(void) {
    if (CVarGetInteger("gEnhancements.Input.MouseSteering", 0) == 0) {
        return false;
    }
    switch (gGameMode & 0x1F) {
        case 0x1: // GAMEMODE_GP_RACE
        case 0x2: // GAMEMODE_PRACTICE
        case 0x3: // GAMEMODE_VS_2P
        case 0x4: // GAMEMODE_VS_3P
        case 0x5: // GAMEMODE_VS_4P
        case 0xE: // GAMEMODE_TIME_ATTACK
        case 0x15: // GAMEMODE_DEATH_RACE
            return true;
        default:
            return false;
    }
}

// Issue #18: absolute steering. The cursor's X position inside the game blit rect IS the stick
// deflection: center is straight, the edges are full lock, so a line can be held instead of
// snapping straight the moment the mouse stops moving (the failure mode of the old
// relative-delta scheme). The sensitivity slider scales deflection: 100% reaches full lock at
// the blit-rect edge, higher values reach it closer to center. When the mouse has not moved for
// kIdleMs the controller stick takes over again, so a gamepad can grab control mid-race without
// opening the menu. INT32_MIN means "mouse is not driving this frame"; the caller leaves the
// stick untouched then (also while a menu/menubar is open, so menu browsing never steers).
extern "C" int gdx_mouse_steer_stick_x(void) {
    if (!RaceMouseSteeringActive()) {
        return INT32_MIN;
    }
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->GetGui()->GetMenuOrMenubarVisible()) {
        return INT32_MIN;
    }
    Fast::Fast3dGui* gui = dynamic_cast<Fast::Fast3dGui*>(wnd->GetGui().get());
    float rx, ry, rw, rh;
    if (gui == nullptr || !gui->GetGameBlitRect(&rx, &ry, &rw, &rh) || rw <= 0.0f) {
        return INT32_MIN;
    }
    Ship::Coords mouse = wnd->GetMousePos();

    static Ship::Coords sLastPos{ -1, -1 };
    static std::chrono::steady_clock::time_point sLastMove{};
    if (mouse.x != sLastPos.x || mouse.y != sLastPos.y) {
        sLastPos = mouse;
        sLastMove = std::chrono::steady_clock::now();
    }
    constexpr auto kIdleMs = std::chrono::milliseconds(750);
    if (sLastMove.time_since_epoch().count() == 0 ||
        std::chrono::steady_clock::now() - sLastMove > kIdleMs) {
        return INT32_MIN;
    }

    float n = (mouse.x - (rx + rw * 0.5f)) / (rw * 0.5f);
    int sensitivity = CVarGetInteger("gEnhancements.Input.MouseSteeringSensitivity", 100);
    n *= sensitivity / 100.0f;
    if (n > 1.0f) {
        n = 1.0f;
    } else if (n < -1.0f) {
        n = -1.0f;
    }
    if (n < 0.05f && n > -0.05f) {
        n = 0.0f; // deadzone: small jitters around center must not steer
    }
    return static_cast<int>(n * 80.0f);
}

// S3: per-frame window mouse-grab for Course Edit / race mouse steering. Confines the OS cursor
// to the game window while either mouse-steering mode is active and no ImGui menu is open.
// Yields to F2 relative-mode capture; the backend restores this grab when capture is released.
extern "C" void gdx_course_edit_mouse_grab_tick(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->IsMouseCaptured()) {
        return;
    }
    if (CVarGetInteger("gEnhancements.Input.MouseConfineToWindow", 1) == 0) {
        wnd->SetMouseGrab(false);
        return;
    }
    bool wantGrab = false;
    if (!wnd->GetGui()->GetMenuOrMenubarVisible() && !CourseEditMouseInSubMenu()) {
        if ((CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) != 0) && CourseEditMouseModeActive()) {
            wantGrab = true;
        } else if (RaceMouseSteeringActive()) {
            wantGrab = true;
        }
    }
    wnd->SetMouseGrab(wantGrab);
}

// Hide-OS-cursor option: hides the OS cursor during gameplay, re-shows it for the ImGui
// menu/menubar. Runs after MouseStateManager::StartFrame (which re-shows on any movement), so
// the per-frame hide wins while the game renders. The menu-open path force-shows EVERY frame,
// not just on a hidden -> shown transition tracked here: the Course Edit cursor tick above hides
// the cursor through the same window without touching sHidden, so a transition-only show leaves
// the cursor invisible until the next mouse movement.
extern "C" void gdx_hide_os_cursor_tick(void) {
    static bool sHidden = false;
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->IsMouseCaptured()) {
        sHidden = false;
        return;
    }
    if (wnd->GetGui()->GetMenuOrMenubarVisible()) {
        wnd->SetCursorVisibility(true);
        sHidden = false;
        return;
    }
    if (CVarGetInteger("gEnhancements.Input.HideCursorInGame", 0) == 0) {
        if (sHidden) {
            wnd->SetCursorVisibility(true);
            sHidden = false;
        }
        return;
    }
    wnd->SetCursorVisibility(false);
    sHidden = true;
}

// Camera gesture query: returns 1 while MMB is held and the camera-drag gate is satisfied.
// Used by the cursor drivers to suppress cursor movement and by the button injector to suppress
// LMB/RMB -> A/B leakage for the duration of the gesture.
extern "C" int gdx_course_edit_mouse_camera_gesture_active(void) {
    return sCourseEditCameraGesture.active ? 1 : 0;
}

// Mouse buttons are only a source-specific supplement to the controller state. Point-edit
// gestures are consumed by the transaction state machine instead of being translated to A/B.
extern "C" int gdx_course_edit_mouse_owns_button(int button) {
    return (button == Ship::LUS_MOUSE_BTN_LEFT || button == Ship::LUS_MOUSE_BTN_RIGHT) &&
           CourseEditEditorActive() && CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) != 0;
}

extern "C" int gdx_course_edit_mouse_buttons(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->IsMouseCaptured() || wnd->GetGui()->GetMenuOrMenubarVisible() ||
        CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0 || !CourseEditMouseModeActive()) {
        CourseEditCancelDrag();
        return 0;
    }
    if (gdx_course_edit_mouse_camera_gesture_active() || sCourseEditDrag.state != DragState::Idle ||
        gdx_course_edit_native_mouse_owned() != 0) {
        return 0;
    }
    // The line-up tool sends no A or B from the mouse. Clicks there only select and box select.
    if (CourseEditEditorActive() && gCreateOption == 1 && gMoveOption == 6) {
        return 0;
    }

    int x, y;
    if (!CourseEditMousePos(wnd, &x, &y)) {
        return 0;
    }
    if (CourseEditEditorActive() && !CourseEditMouseInSubMenu() && !CourseEditMouseOverChrome(x, y) &&
        gCreateOption == 1 && ((GdxDragEnabled() && gMoveOption >= 0 && gMoveOption <= 4) || gMoveOption == 5)) {
        return 0;
    }

    int buttons = 0;
    if (wnd->GetMouseState(Ship::LUS_MOUSE_BTN_LEFT)) {
        // Ctrl+click is the controller's Z, the stock segment pick outside point mode. It selects without applying.
        const bool pickSegment = CourseEditEditorActive() && gCreateOption != 1 && !CourseEditMouseInSubMenu() &&
                                 !CourseEditMouseOverChrome(x, y) && CourseEditCtrlHeld();
        buttons |= pickSegment ? GDX_BTN_Z : GDX_BTN_A;
    }
    if (wnd->GetMouseState(Ship::LUS_MOUSE_BTN_RIGHT)) {
        buttons |= GDX_BTN_B;
    }
    return buttons;
}

// Per-frame state pump for the MMB camera gesture. Called from main.cpp after
// MouseStateManager::StartFrame, before gdx_dispatch() reads the decomp input.
extern "C" void gdx_course_edit_mouse_camera_tick(void) {
    CourseEditMouseCameraTick();
}

// Wheel detents accumulated by gdx_course_edit_mouse_on_wheel at HOST-frame rate. The decomp
// consumers read them through gdx_course_edit_mouse_wheel() on game ticks, which run at ~20 Hz in
// Course Edit: reading LUS's GetMouseWheel() there loses every detent that landed between ticks,
// because MouseStateManager::StartFrame clears the LUS wheel state on every host frame.
static float sCourseEditWheelDetents = 0.0f;
static int sCourseEditMenuDetents = 0;
static const void* sCourseEditWheelMenu = nullptr;

// Expose the OS mouse wheel as integer detents while the absolute mouse drive owns the cursor.
// Positive return values move the selection down (wheel toward the user), negative values move it
// up; zero means no detent pending or the wheel-zoom CVar is off.
extern "C" int gdx_course_edit_mouse_wheel(void) {
    if (!GdxWheelZoomEnabled() || !CourseEditEditorActive() || CourseEditMouseInSubMenu()) {
        sCourseEditWheelDetents = 0.0f;
        return 0;
    }
    std::shared_ptr<Fast::Fast3dWindow> wnd;
    int x, y;
    if (!CourseEditMousePos(wnd, &x, &y)) {
        // Drop stale detents instead of banking them: a detent that arrived while the cursor was
        // outside the game area (or in a menu) must not fire later when the cursor re-enters.
        if (sCourseEditWheelDetents != 0.0f) {
            gdx_port_logf("[ce-wheel] dropped %.2f detents (cursor outside game area)\n",
                          static_cast<double>(sCourseEditWheelDetents));
        }
        sCourseEditWheelDetents = 0.0f;
        return 0;
    }
    int steps = static_cast<int>(sCourseEditWheelDetents); // truncates toward zero
    sCourseEditWheelDetents -= steps;
    if (steps != 0) {
        gdx_port_logf("[ce-wheel] consumed %d steps\n", steps);
    }
    return -steps;
}

// Drains the whole part of a pending camera value and keeps the fraction for the next game tick.
// The cast truncates toward zero, so the remainder keeps the sign of the motion it came from.
static int32_t TakeWholeUnits(float& pending) {
    const int32_t whole = static_cast<int32_t>(pending);
    pending -= static_cast<float>(whole);
    return whole;
}

// If an orbit gesture (MMB without Shift) is active, fills yawDeg/pitchDeg with the whole degrees of
// yaw and pitch change accumulated since the last game tick and returns 1. The values already carry
// the sign the native camera adds, and the fraction stays pending. Returns 0 if no gesture is active
// or Shift is held.
extern "C" int gdx_course_edit_mouse_orbit_delta(int32_t* yawDeg, int32_t* pitchDeg) {
    CourseEditCameraGesture& g = sCourseEditCameraGesture;
    if (!g.active || g.shift) {
        return 0;
    }
    *yawDeg = TakeWholeUnits(g.orbitYawDeg);
    *pitchDeg = TakeWholeUnits(g.orbitPitchDeg);
    return 1;
}

// Same as the orbit helper, but for pan (MMB + Shift): whole OS pixels with the pan sensitivity applied.
extern "C" int gdx_course_edit_mouse_pan_delta(int32_t* dx, int32_t* dy) {
    CourseEditCameraGesture& g = sCourseEditCameraGesture;
    if (!g.active || !g.shift) {
        return 0;
    }
    *dx = TakeWholeUnits(g.panDx);
    *dy = TakeWholeUnits(g.panDy);
    return 1;
}

// Human-readable name for a LUS KbScancode (PS/2 set-1 numbering; extended E0 keys carry +0x100).
// The shim stores bindings in this space because Fast3dWindow::KeyDown delivers it on every
// backend, so a binding made under DirectX 11 reads identically under OpenGL/SDL.
const char* GdxLusKeyName(int sc, char* buf, size_t bufSize) {
    if (sc <= 0) {
        return "None";
    }
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd != nullptr) {
        const char* backendName = wnd->GetKeyName(sc);
        if (backendName != nullptr && backendName[0] != '\0' && strcmp(backendName, "?") != 0) {
            snprintf(buf, bufSize, "%s", backendName);
            return buf;
        }
    }
    static const struct {
        int sc;
        const char* name;
    } kNames[] = {
        { 1, "Esc" },    { 12, "-" },      { 13, "=" },      { 14, "Backspace" }, { 15, "Tab" },
        { 26, "[" },     { 27, "]" },      { 28, "Enter" },  { 29, "Ctrl" },      { 39, ";" },
        { 40, "'" },     { 41, "`" },      { 42, "Shift" },  { 43, "\\" },        { 51, "," },
        { 52, "." },     { 53, "/" },      { 54, "RShift" }, { 55, "Num *" },     { 56, "Alt" },
        { 57, "Space" }, { 58, "CapsLock" },
        { 71, "Num 7" }, { 72, "Num 8" },  { 73, "Num 9" },  { 74, "Num -" },     { 75, "Num 4" },
        { 76, "Num 5" }, { 77, "Num 6" },  { 78, "Num +" },  { 79, "Num 1" },     { 80, "Num 2" },
        { 81, "Num 3" }, { 82, "Num 0" },  { 83, "Num ." },
        { 328, "Up" },   { 331, "Left" },  { 333, "Right" }, { 336, "Down" },     { 339, "Delete" },
    };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (kNames[i].sc == sc) {
            return kNames[i].name;
        }
    }
    if (sc >= 2 && sc <= 10) { // LUS_KB_1 .. LUS_KB_9
        buf[0] = static_cast<char>('1' + (sc - 2));
        buf[1] = '\0';
        return buf;
    }
    if (sc == 11) {
        return "0";
    }
    static const int kLetters[] = { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 30, 31, 32, 33,
                                    34, 35, 36, 37, 38, 44, 45, 46, 47, 48, 49, 50 };
    for (size_t i = 0; i < sizeof(kLetters) / sizeof(kLetters[0]); i++) {
        if (kLetters[i] == sc) {
            buf[0] = "QWERTYUIOPASDFGHJKLZXCVBNM"[i];
            buf[1] = '\0';
            return buf;
        }
    }
    if ((sc >= 59 && sc <= 68) || sc == 87 || sc == 88) { // F1..F10, F11, F12
        snprintf(buf, bufSize, "F%d", sc <= 68 ? sc - 58 : sc - 76);
        return buf;
    }
    snprintf(buf, bufSize, "Key %d", sc);
    return buf;
}

// Last physical keydown seen by the key hook, for the menu's "Last key seen" readout. Lets a user
// (or a remote diagnostic) tell "events never arrive" apart from "arrived but did not match".
static int sLastKeydownScancode = 0;

extern "C" int gdx_course_edit_mouse_last_keydown(void) {
    return sLastKeydownScancode;
}

extern "C" const char* gdx_course_edit_mouse_last_key_name(void) {
    static char buf[64];
    char name[24];
    if (sLastKeydownScancode == 0) {
        return "none yet";
    }
    snprintf(buf, sizeof(buf), "%s (scancode %d)",
             GdxLusKeyName(sLastKeydownScancode, name, sizeof(name)), sLastKeydownScancode);
    return buf;
}

// Backend-agnostic keyboard entry point, called from Fast3dWindow::KeyDown/KeyUp, the one funnel
// both window backends (SDL and DXGI) feed, in LUS KbScancode space. Tool keys accumulate edges
// here and are applied on game-boundary frames by gdx_course_edit_mouse_keyboard_tick.
extern "C" void gdx_course_edit_mouse_key_capture_cancel(void);
extern "C" int32_t gdx_name_entry_is_active(void);
extern "C" void gdx_name_entry_queue_push(int32_t key);
extern "C" void gdx_name_entry_queue_clear(void);

#define GDX_NAME_KEY_ESC -1
#define GDX_NAME_KEY_ENTER -2
#define GDX_NAME_KEY_BACKSPACE -3

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static char GdxLusScancodeToChar(int scancode, bool shiftHeld) {
    BYTE kbdState[256] = {};
    if (shiftHeld) {
        kbdState[VK_SHIFT] = 0x80;
    }
    UINT vk = MapVirtualKeyA(scancode & 0xFF, MAPVK_VSC_TO_VK);
    if (vk == 0) {
        return 0;
    }
    WORD ascii = 0;
    if (ToAscii(vk, scancode & 0xFF, kbdState, &ascii, 0) == 1) {
        char c = static_cast<char>(ascii);
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        return c;
    }
    return 0;
}
#endif

static char GdxFallbackScancodeToChar(int scancode, bool shiftHeld) {
    if (scancode >= 2 && scancode <= 10) { // 1..9
        if (shiftHeld && scancode == 8) {  // Shift + 7 = &
            return '&';
        }
        return static_cast<char>('1' + (scancode - 2));
    }
    if (scancode == 11) { // 0
        return '0';
    }
    if (scancode == 57) { // Space
        return ' ';
    }
    if (scancode == 12 || scancode == 74) { // - or Num -
        return '-';
    }
    if (scancode == 51) { // ,
        return ',';
    }
    if (scancode == 52 || scancode == 83) { // . or Num .
        return '.';
    }
    if (scancode == 40) { // '
        return '\'';
    }
    static const struct { int sc; char c; } kLetters[] = {
        { 16, 'Q' }, { 17, 'W' }, { 18, 'E' }, { 19, 'R' }, { 20, 'T' },
        { 21, 'Y' }, { 22, 'U' }, { 23, 'I' }, { 24, 'O' }, { 25, 'P' },
        { 30, 'A' }, { 31, 'S' }, { 32, 'D' }, { 33, 'F' }, { 34, 'G' },
        { 35, 'H' }, { 36, 'J' }, { 37, 'K' }, { 38, 'L' },
        { 44, 'Z' }, { 45, 'X' }, { 46, 'C' }, { 47, 'V' }, { 48, 'B' },
        { 49, 'N' }, { 50, 'M' },
    };
    for (size_t i = 0; i < sizeof(kLetters) / sizeof(kLetters[0]); i++) {
        if (kLetters[i].sc == scancode) {
            return kLetters[i].c;
        }
    }
    if (scancode >= 79 && scancode <= 81) return static_cast<char>('1' + (scancode - 79));
    if (scancode >= 75 && scancode <= 77) return static_cast<char>('4' + (scancode - 75));
    if (scancode >= 71 && scancode <= 73) return static_cast<char>('7' + (scancode - 71));
    if (scancode == 82) return '0';
    return 0;
}

static char GdxResolveChar(int scancode, bool shiftHeld) {
    char c = 0;
#if defined(_WIN32)
    c = GdxLusScancodeToChar(scancode, shiftHeld);
#endif
    if (c == 0) {
        c = GdxFallbackScancodeToChar(scancode, shiftHeld);
    }
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    return c;
}

static bool GdxIsValidNameChar(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == ' ' || c == '-' || c == ',' || c == '.' || c == '\'' || c == '&') return true;
    return false;
}

extern "C" int gdx_course_edit_mouse_on_key(int lusScancode, int isDown) {
    if (lusScancode <= 0) {
        return 0;
    }
    if (isDown) {
        sLastKeydownScancode = lusScancode;
    }
    if (lusScancode == 42) {
        sLeftShiftHeld = isDown != 0;
    } else if (lusScancode == 54) {
        sRightShiftHeld = isDown != 0;
    }
    sShiftHeld = sLeftShiftHeld || sRightShiftHeld;
    if (lusScancode == 29) {
        sLeftCtrlHeld = isDown != 0;
    } else if (lusScancode == 285) {
        sRightCtrlHeld = isDown != 0;
    } else if (lusScancode == 56) {
        sLeftAltHeld = isDown != 0;
    } else if (lusScancode == 312) {
        sRightAltHeld = isDown != 0;
    }
    sCtrlHeld = sLeftCtrlHeld || sRightCtrlHeld;
    sAltHeld = sLeftAltHeld || sRightAltHeld;

    // "WasHeld" is the state before this event. Only the first edge of a key acts, so auto-repeat never repeats an
    // action. An owned key is swallowed on both edges, so the native binding for the same key never sees it.
    const bool deleteWasHeld = sDeleteKeyHeld;
    const bool enterWasHeld = sEnterHeld;
    const bool undoZWasHeld = sUndoZHeld;
    const bool undoYWasHeld = sUndoYHeld;
    const bool aKeyWasHeld = sAKeyHeld;
    if (lusScancode == 339) {
        sDeleteKeyHeld = isDown != 0;
        if (sDeleteKeyOwned) {
            sDeleteKeyOwned = isDown != 0;
            return 1;
        }
    }
    if (lusScancode == 28 || lusScancode == 284) {
        sEnterHeld = isDown != 0;
        if (sEnterOwned) {
            sEnterOwned = isDown != 0;
            return 1;
        }
    }
    if (lusScancode == 44) {
        sUndoZHeld = isDown != 0;
        if (sUndoZOwned) {
            sUndoZOwned = isDown != 0;
            return 1;
        }
    }
    if (lusScancode == 21) {
        sUndoYHeld = isDown != 0;
        if (sUndoYOwned) {
            sUndoYOwned = isDown != 0;
            return 1;
        }
    }
    if (lusScancode == 45) {
        sAKeyHeld = isDown != 0;
        if (sAKeyOwned) {
            sAKeyOwned = isDown != 0;
            return 1;
        }
    }
    if (lusScancode == 1 && sEditorEscOwned) {
        if (!isDown) {
            sEditorEscOwned = false;
        }
        return 1;
    }
    for (int i = 1; i <= 7; i++) {
        if (!isDown && sEditorKeysConsumed[i] && lusScancode == GdxEditorKeyScancode(i)) {
            sEditorKeysConsumed[i] = false;
            sCourseEditKeys.held[i] = false;
            return 1;
        }
    }

    if (lusScancode == 1) {
        auto wnd = CourseEditMouseWindow();
        bool active = (gGameMode & 0x1F) == 0xD && gInCourseEditTestRun &&
            CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) != 0 &&
            !sCourseEditKeyCapture.active && wnd != nullptr && !wnd->IsMouseCaptured() &&
            !wnd->GetGui()->GetMenuOrMenubarVisible();
        if (gdx_course_edit_input_test_esc(isDown, active)) {
            return 1;
        }
    }

    // Name Entry ownership: when Name Entry is active in Course Edit, all keys are consumed
    // and typed text is routed exclusively to the name buffer on game ticks.
    if (CourseEditEditorActive() && gdx_name_entry_is_active()) {
        if (isDown) {
            if (lusScancode == 1) { // Esc
                gdx_name_entry_queue_push(GDX_NAME_KEY_ESC);
            } else if (lusScancode == 28 || lusScancode == 284) { // Enter, Num Enter
                gdx_name_entry_queue_push(GDX_NAME_KEY_ENTER);
            } else if (lusScancode == 14 || lusScancode == 339) { // Backspace, Delete
                gdx_name_entry_queue_push(GDX_NAME_KEY_BACKSPACE);
            } else {
                char c = GdxResolveChar(lusScancode, sShiftHeld);
                if (GdxIsValidNameChar(c)) {
                    gdx_name_entry_queue_push(static_cast<int32_t>(c));
                }
            }
        }
        return 1;
    }

    // The menu's rebind capture owns the next keydown, regardless of editor state or menu
    // visibility: Esc cancels, Backspace/Delete restore the stock number key, anything else binds.
    if (sCourseEditKeyCapture.active) {
        if (!isDown) {
            return 1;
        }
        const int index = sCourseEditKeyCapture.index;
        gdx_port_logf("[ce-capture] keydown sc=%d slot=%d\n", lusScancode, index);
        if (lusScancode == 1) { // LUS_KB_ESCAPE cancels without touching the binding
            sCourseEditKeyCapture.conflictMsg[0] = '\0';
            gdx_course_edit_mouse_key_capture_cancel();
            return 1;
        }
        if (lusScancode == 14 || lusScancode == 339) { // Backspace, Delete
            char cvar[64];
            snprintf(cvar, sizeof(cvar), "gEnhancements.Input.EditorKey%d", index);
            CVarSetInteger(cvar, kDefaultEditorKeyScancodes[index]);
            CVarSave();
            sCourseEditKeyCapture.conflictMsg[0] = '\0';
            gdx_course_edit_mouse_key_capture_cancel();
            return 1;
        }

        std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
        if (wnd != nullptr) {
            if (lusScancode == wnd->GetFullscreenScancode()) {
                snprintf(sCourseEditKeyCapture.conflictMsg, sizeof(sCourseEditKeyCapture.conflictMsg),
                         "Key is reserved for Fullscreen toggle");
                gdx_course_edit_mouse_key_capture_cancel();
                return 1;
            }
            if (lusScancode == wnd->GetMouseCaptureScancode()) {
                snprintf(sCourseEditKeyCapture.conflictMsg, sizeof(sCourseEditKeyCapture.conflictMsg),
                         "Key is reserved for Mouse Capture");
                gdx_course_edit_mouse_key_capture_cancel();
                return 1;
            }
        }
        if (lusScancode == 59 /* F1 */) {
            snprintf(sCourseEditKeyCapture.conflictMsg, sizeof(sCourseEditKeyCapture.conflictMsg),
                     "Key 'F1' is reserved for menu toggle");
            gdx_course_edit_mouse_key_capture_cancel();
            return 1;
        }

        int conflictSlot = 0;
        for (int j = 1; j <= 7; j++) {
            if (j != index && GdxEditorKeyScancode(j) == lusScancode) {
                conflictSlot = j;
                break;
            }
        }
        if (conflictSlot != 0) {
            char keyNameBuf[32];
            snprintf(sCourseEditKeyCapture.conflictMsg, sizeof(sCourseEditKeyCapture.conflictMsg),
                     "Key '%s' is already assigned to %s",
                     GdxLusKeyName(lusScancode, keyNameBuf, sizeof(keyNameBuf)),
                     gdx_course_edit_action_name(conflictSlot));
            gdx_course_edit_mouse_key_capture_cancel();
            return 1;
        }

        char cvar[64];
        snprintf(cvar, sizeof(cvar), "gEnhancements.Input.EditorKey%d", index);
        CVarSetInteger(cvar, lusScancode);
        CVarSave();
        sCourseEditKeyCapture.conflictMsg[0] = '\0';
        gdx_course_edit_mouse_key_capture_cancel();
        return 1;
    }

    auto inputWindow = CourseEditMouseWindow();
    bool editorInput = CourseEditEditorActive() && inputWindow != nullptr &&
        !inputWindow->IsMouseCaptured() && !inputWindow->GetGui()->GetMenuOrMenubarVisible() &&
        !CourseEditMouseInSubMenu();
    // The selection, line-up, and history keys act only while the drag machine is idle and no native transaction runs.
    const bool dragIdle = sCourseEditDrag.state == DragState::Idle && gdx_course_edit_native_mouse_owned() == 0;
    if (editorInput && lusScancode == 1 && isDown) {
        // Esc cancels a drag or transaction first, then clears a non-empty selection on the point screen.
        if (!dragIdle) {
            CourseEditCancelDrag();
            sEditorEscOwned = true;
            return 1;
        }
        if (CourseEditPointInputContextActive() && CourseEditSelectionMask() != 0) {
            CourseEditReplaceSelection(0);
            sEditorEscOwned = true;
            return 1;
        }
    }
    if (isDown && !enterWasHeld && (lusScancode == 28 || lusScancode == 284) && dragIdle &&
        CourseEditPointInputContextActive() && gMoveOption == 6) {
        sEnterOwned = true;
        CourseEditRequestLineUp();
        return 1;
    }
    if (isDown && !deleteWasHeld && lusScancode == 339 && dragIdle && CourseEditPointInputContextActive()) {
        sDeleteKeyOwned = true;
        CourseEditRequestDelete();
        return 1;
    }
    if (isDown && !undoZWasHeld && lusScancode == 44 && sCtrlHeld && editorInput && dragIdle) {
        sUndoZOwned = true;
        if (sShiftHeld) {
            gdx_course_edit_undo_request_redo();
        } else {
            gdx_course_edit_undo_request_undo();
        }
        return 1;
    }
    if (isDown && !undoYWasHeld && lusScancode == 21 && sCtrlHeld && editorInput && dragIdle) {
        sUndoYOwned = true;
        gdx_course_edit_undo_request_redo();
        return 1;
    }

    if (CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0 || !CourseEditEditorActive()) {
        return 0;
    }
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr) {
        return 0;
    }
    // Keys keep the strict gate (no F2 capture, no ImGui menu, no editor sub-menu) so dropdown
    // typing and menu navigation are unaffected.
    if (!GdxKeybindsEnabled() || wnd->IsMouseCaptured() || wnd->GetGui()->GetMenuOrMenubarVisible() ||
        CourseEditMouseInSubMenu()) {
        return 0;
    }
    int idx = -1;
    for (int i = 1; i <= 7; i++) {
        if (lusScancode == GdxEditorKeyScancode(i)) {
            idx = i;
            break;
        }
    }
    if (idx < 1) {
        for (int i = 1; i <= 7; i++) {
            if (!isDown && sEditorKeysConsumed[i] && lusScancode == GdxEditorKeyScancode(i)) {
                sEditorKeysConsumed[i] = false;
                sCourseEditKeys.held[i] = false;
                return 1;
            }
        }
        // The line-up tool sends no A. The keyboard A key (X by default) is swallowed on its first edge here, and the
        // owned flag at the top of this function handles its auto-repeat and release.
        if (isDown && !aKeyWasHeld && lusScancode == 45 && gCreateOption == 1 && gMoveOption == 6 &&
            CourseEditEditorActive()) {
            sAKeyOwned = true;
            return 1;
        }
        return 0;
    }
    if (isDown) {
        sEditorKeysConsumed[idx] = true;
        // Win32 auto-repeat refires keydown while a key is held; only the first edge counts.
        if (!sCourseEditKeys.held[idx]) {
            sCourseEditKeys.held[idx] = true;
            sCourseEditKeys.pressedAccum[idx] = sCourseEditDrag.state == DragState::Idle &&
                gdx_course_edit_native_mouse_owned() == 0;
            gdx_port_logf("[ce-keys] tool key %d pressed\n", idx);
        }
    } else {
        sEditorKeysConsumed[idx] = false;
        sCourseEditKeys.held[idx] = false;
    }
    return 1;
}

// Backend-agnostic wheel entry point: gfx_sdl2 (SDL_MOUSEWHEEL) and gfx_dxgi (WM_MOUSEWHEEL) both
// call here at event time, so no detent can be lost between the host frame rate and the ~20 Hz
// game tick the read side (gdx_course_edit_mouse_wheel) runs on.
extern "C" void gdx_course_edit_mouse_on_wheel(int detents) {
    std::shared_ptr<Fast::Fast3dWindow> wnd;
    int x, y;
    if (detents == 0 || !CourseEditEditorActive() || !CourseEditMousePos(wnd, &x, &y) ||
        wnd->IsMouseCaptured() || sCourseEditKeyCapture.active) {
        return;
    }
    const void* menu = gdx_course_edit_native_menu_id();
    if (menu != nullptr) {
        sCourseEditWheelDetents = 0.0f;
        if (sCourseEditWheelMenu != menu) {
            sCourseEditMenuDetents = 0;
        }
        sCourseEditWheelMenu = menu;
        sCourseEditMenuDetents = std::clamp(sCourseEditMenuDetents - detents, -8, 8);
        return;
    }
    sCourseEditMenuDetents = 0;
    sCourseEditWheelMenu = nullptr;
    if (!CourseEditMouseInSubMenu() && GdxWheelZoomEnabled()) {
        sCourseEditWheelDetents = std::clamp(
            sCourseEditWheelDetents + detents * (GdxZoomSensitivity() / 100.0f), -8.0f, 8.0f);
    }
}

extern "C" int gdx_course_edit_mouse_menu_wheel(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd;
    int x, y;
    int steps = sCourseEditMenuDetents;
    sCourseEditMenuDetents = 0;
    if (!CourseEditEditorActive() || !CourseEditMousePos(wnd, &x, &y) || wnd->IsMouseCaptured() ||
        sCourseEditWheelMenu == nullptr || sCourseEditWheelMenu != gdx_course_edit_native_menu_id()) {
        return 0;
    }
    return steps;
}

// S2: finalize keyboard shortcuts on game-boundary frames. The Course Edit editor runs its game
// loop at a VI divider (~20 Hz), so pressed edges are accumulated across host frames and applied
// once per game tick, mirroring the per-port button edge accumulation in input_bridge.c.
extern "C" void gdx_course_edit_mouse_keyboard_tick(int gameBoundary) {
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (!CourseEditEditorActive() || sCourseEditWheelMenu != gdx_course_edit_native_menu_id() ||
        wnd == nullptr || wnd->IsMouseCaptured() || wnd->GetGui()->GetMenuOrMenubarVisible()) {
        sCourseEditMenuDetents = 0;
        sCourseEditWheelMenu = nullptr;
    }
    if (CourseEditMouseInSubMenu()) {
        sCourseEditWheelDetents = 0.0f;
    }
    if (!gInCourseEditTestRun || wnd == nullptr || wnd->IsMouseCaptured() ||
        wnd->GetGui()->GetMenuOrMenubarVisible() || sCourseEditKeyCapture.active ||
        CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0) {
        gdx_course_edit_input_clear_test_esc();
    }
    if (CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0 ||
        !CourseEditEditorActive() || CourseEditMouseInSubMenu() || wnd == nullptr || wnd->IsMouseCaptured() ||
        wnd->GetGui()->GetMenuOrMenubarVisible()) {
        gdx_course_edit_input_clear();
        CourseEditCancelDrag();
        for (int i = 1; i <= 9; i++) {
            sCourseEditKeys.pressedAccum[i] = false;
            sCourseEditKeys.pressedGame[i] = false;
        }
        return;
    }
    if (gameBoundary) {
        for (int i = 1; i <= 7; i++) {
            sCourseEditKeys.pressedGame[i] = sCourseEditKeys.pressedAccum[i];
            sCourseEditKeys.pressedAccum[i] = false;
        }
        for (int i = 8; i <= 9; i++) {
            sCourseEditKeys.pressedAccum[i] = false;
            sCourseEditKeys.pressedGame[i] = false;
        }
        for (int i = 1; i <= 7; i++) {
            if (sCourseEditKeys.pressedGame[i] && GdxKeybindsEnabled() &&
                sCourseEditDrag.state == DragState::Idle && gdx_course_edit_native_mouse_owned() == 0) {
                sCourseEditKeys.pressedGame[i] = false;
                // Map 1..7 to the seven MOVE_OPTION_* values in order. Every point tool is gated
                // on CREATE_OPTION_POINT in the editor, so switch tabs as well. Otherwise the
                // hotkey is a silent no-op whenever another tab (COURSE/PARTS/...) is active.
                gCreateOption = 1 /* CREATE_OPTION_POINT */;
                gMoveOption = i - 1;
                gdx_port_logf("[ce-keys] tool key %d -> move option %d\n", i, i - 1);
                break;
            }
        }
    }
}

// Menu key-capture helpers. The menu custom widget arms/disarms capture; the next keydown is
// applied inside gdx_course_edit_mouse_on_key (the backend-agnostic hook), so capture works
// identically on the SDL and DXGI window backends.
extern "C" void gdx_course_edit_mouse_key_capture_begin(int index) {
    sCourseEditKeyCapture.active = (index >= 1 && index <= 7);
    sCourseEditKeyCapture.index = sCourseEditKeyCapture.active ? index : 0;
    sCourseEditKeyCapture.conflictMsg[0] = '\0';
    gdx_port_logf("[ce-capture] begin slot %d\n", index);
}

extern "C" void gdx_course_edit_mouse_key_capture_cancel(void) {
    if (sCourseEditKeyCapture.active) {
        gdx_port_logf("[ce-capture] end slot %d\n", sCourseEditKeyCapture.index);
    }
    sCourseEditKeyCapture.active = false;
    sCourseEditKeyCapture.index = 0;
}

extern "C" int gdx_course_edit_mouse_key_capture_active(void) {
    return sCourseEditKeyCapture.active ? sCourseEditKeyCapture.index : 0;
}

// GDX_INPUT_SCRIPT KEY command: inject a key tap through the same entry point the window backends
// call (LUS KbScancode space), so scripted tests exercise the same path a physical keypress takes.
extern "C" void gdx_course_edit_mouse_inject_key_tap(int lusScancode) {
    gdx_course_edit_mouse_on_key(lusScancode, 1);
    gdx_course_edit_mouse_on_key(lusScancode, 0);
}

extern "C" const char* gdx_course_edit_mouse_key_name(int index) {
    if (index < 1 || index > 7) {
        return "?";
    }
    // Persistent ring of small buffers so the menu can hold several names on screen at once.
    static char names[8][32];
    static int slot = 0;
    slot = (slot + 1) % 8;
    return GdxLusKeyName(GdxEditorKeyScancode(index), names[slot], sizeof(names[slot]));
}

extern "C" int gdx_course_edit_mouse_key_scancode(int index) {
    return GdxEditorKeyScancode(index);
}

extern "C" int gdx_course_edit_mouse_key_default_scancode(int index) {
    if (index < 1 || index > 7) {
        return 0;
    }
    return kDefaultEditorKeyScancodes[index];
}

extern "C" void gdx_course_edit_mouse_reset_keys(void) {
    for (int i = 1; i <= 9; i++) {
        sCourseEditKeys.held[i] = false;
        sCourseEditKeys.pressedAccum[i] = false;
        sCourseEditKeys.pressedGame[i] = false;
        sEditorKeysConsumed[i] = false;
    }
    sLeftShiftHeld = false;
    sRightShiftHeld = false;
    sShiftHeld = false;
    sLeftCtrlHeld = false;
    sRightCtrlHeld = false;
    sCtrlHeld = false;
    sLeftAltHeld = false;
    sRightAltHeld = false;
    sAltHeld = false;
    sScalarDrag = CourseEditScalarDragState();
    sDeleteKeyHeld = false;
    sDeleteKeyOwned = false;
    sEnterHeld = false;
    sEnterOwned = false;
    sUndoZHeld = false;
    sUndoYHeld = false;
    sUndoZOwned = false;
    sUndoYOwned = false;
    sAKeyHeld = false;
    sAKeyOwned = false;
    sEditorEscOwned = false;
    gdx_name_entry_queue_clear();
}

extern "C" void gdx_course_edit_mouse_all_keys_up(void) {
    sDragBlockedUntilRelease = true;
    sCourseEditWheelDetents = 0.0f;
    sCourseEditMenuDetents = 0;
    CourseEditCancelDrag();
    gdx_course_edit_input_clear_test_esc();
    // Focus loss drops pending actions, not release ownership: repeats must not become new presses.
    for (int i = 1; i <= 9; i++) {
        sCourseEditKeys.pressedAccum[i] = false;
        sCourseEditKeys.pressedGame[i] = false;
    }
    sLeftShiftHeld = false;
    sRightShiftHeld = false;
    sShiftHeld = false;
    sLeftCtrlHeld = false;
    sRightCtrlHeld = false;
    sCtrlHeld = false;
    sLeftAltHeld = false;
    sRightAltHeld = false;
    sAltHeld = false;
    sScalarDrag = CourseEditScalarDragState();
    gdx_name_entry_queue_clear();
}

// Hint strip, defined in port/gdx_course_edit_help.cpp. Drawn by the overlay below.
void GdxCourseEditDrawHintStrip(void);

namespace {
// Game-space point to window pixels: the inverse of CourseEditMousePosF, so markers sit where clicks land.
bool CourseEditGameToWindow(float gameX, float gameY, ImVec2* out) {
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr) {
        return false;
    }
    Fast::Fast3dGui* gui = dynamic_cast<Fast::Fast3dGui*>(wnd->GetGui().get());
    float rx, ry, rw, rh;
    if (gui == nullptr || !gui->GetGameBlitRect(&rx, &ry, &rw, &rh) || rw <= 0.0f || rh <= 0.0f) {
        return false;
    }
    out->x = rx + gameX * rw / 320.0f;
    out->y = ry + gameY * rh / 240.0f;
    return true;
}

// World-space point to window pixels, through the projection the editor uses for its points. Returns false when the
// point is behind the camera or the blit rect is unavailable.
bool CourseEditWorldToWindow(Vec3f pos, ImVec2* out) {
    int32_t sx = 0;
    int32_t sy = 0;
    if (func_xk2_800EF090(pos, &sx, &sy) != 0) {
        return false;
    }
    return CourseEditGameToWindow(static_cast<float>(sx), static_cast<float>(sy), out);
}

// Delete tool: the point under the cursor is drawn red, so a click on it deletes it.
void CourseEditDrawDeleteHover(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd;
    float gameX = 0.0f;
    float gameY = 0.0f;
    if (gMoveOption != 5 || !CourseEditPointInputContextActive() ||
        sCourseEditDrag.state != DragState::Idle || !CourseEditMousePosF(wnd, &gameX, &gameY) ||
        CourseEditMouseOverChrome(static_cast<int32_t>(gameX), static_cast<int32_t>(gameY))) {
        return;
    }
    const int32_t idx = CourseEditPickPoint(gameX, gameY);
    Vec3f pos = {};
    ImVec2 at(0.0f, 0.0f);
    if (idx < 0 || !gdx_course_edit_native_point_position(idx, &pos) || !CourseEditWorldToWindow(pos, &at)) {
        return;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddCircleFilled(at, 6.0f, IM_COL32(255, 40, 40, 255));
    draw->AddCircle(at, 6.0f, IM_COL32(255, 255, 255, 230), 0, 1.5f);
}

// Line-up tool: the line the selection would land on, with a marker at each landing point. Drawn only when the scan
// accepts the selection, so a refused selection shows nothing here.
void CourseEditDrawLineUpPreview(void) {
    if (gMoveOption != 6 || !CourseEditPointInputContextActive()) {
        return;
    }
    GdxCourseEditLineUpPreview preview = {};
    if (gdx_course_edit_native_line_up_scan(&preview) != GDX_COURSE_EDIT_LINE_UP_OK) {
        return;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImVec2 start(0.0f, 0.0f);
    ImVec2 end(0.0f, 0.0f);
    if (CourseEditWorldToWindow(preview.start, &start) && CourseEditWorldToWindow(preview.end, &end)) {
        draw->AddLine(start, end, IM_COL32(120, 200, 255, 220), 2.0f);
    }
    for (int32_t i = 0; i < preview.count; i++) {
        ImVec2 at(0.0f, 0.0f);
        if (CourseEditWorldToWindow(preview.landing[i], &at)) {
            draw->AddCircleFilled(at, 4.0f, IM_COL32(120, 200, 255, 255));
        }
    }
}

// Course forks (port/gdx_course_branch_editor.c): green fork and merge points joined through the nodes of the path
// not showing, the segment a first Split press picked, and which path the editor shows. Drawn in the editor screens
// whether or not the mouse drive is on, since the controller has no other way to see the hidden path.
void CourseEditDrawForkMarks(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (!CourseEditEditorActive() || CourseEditMouseInSubMenu() || gCreateOption < 1 || gCreateOption > 3 ||
        wnd == nullptr || wnd->GetGui()->GetMenuOrMenubarVisible()) {
        return;
    }
    const int showing = gdx_course_branch_editor_path_showing();
    GdxBranchMark marks[160];
    const int count = gdx_course_branch_editor_marks(marks, static_cast<int>(sizeof(marks) / sizeof(marks[0])));
    if (showing == 0 && count == 0) {
        return;
    }

    const ImU32 green = IM_COL32(70, 230, 110, 255);
    const ImU32 line = IM_COL32(70, 230, 110, 150);
    const ImU32 node = IM_COL32(235, 245, 235, 230);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImVec2 last(0.0f, 0.0f);
    bool haveLast = false;
    for (int i = 0; i < count; i++) {
        ImVec2 at(0.0f, 0.0f);
        const Vec3f pos = { marks[i].pos[0], marks[i].pos[1], marks[i].pos[2] };
        const bool visible = CourseEditWorldToWindow(pos, &at);
        if (visible && haveLast && marks[i].chain == marks[i - 1].chain) {
            draw->AddLine(last, at, line, 2.0f);
        }
        if (visible) {
            if (marks[i].kind == GDX_BRANCH_MARK_NODE) {
                draw->AddCircleFilled(at, 3.5f, node);
            } else {
                draw->AddCircleFilled(at, 6.0f, green);
                draw->AddCircle(at, 6.0f, IM_COL32(255, 255, 255, 230), 0, 1.5f);
            }
        }
        last = at;
        haveLast = visible;
    }

    ImVec2 corner(0.0f, 0.0f);
    if (showing != 0 && CourseEditGameToWindow(312.0f, 60.0f, &corner)) {
        const char* title = showing == 2 ? "Path B showing" : "Path A showing";
        const char* hint = "Split on a fork switches paths";
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
        const float width = (std::max)(titleSize.x, hintSize.x) + 12.0f;
        const ImVec2 a(corner.x - width, corner.y);
        const ImVec2 b(corner.x, corner.y + titleSize.y + hintSize.y + 8.0f);
        draw->AddRectFilled(a, b, IM_COL32(10, 13, 20, 190), 4.0f);
        draw->AddRect(a, b, IM_COL32(70, 230, 110, 120), 4.0f);
        draw->AddText(ImVec2(a.x + 6.0f, a.y + 4.0f), showing == 2 ? IM_COL32(120, 200, 255, 255) : green, title);
        draw->AddText(ImVec2(a.x + 6.0f, a.y + 4.0f + titleSize.y), IM_COL32(190, 190, 190, 255), hint);
    }
}

// Shift-drag selection rectangle in window pixels, the same space as the press and release samples. Drawn on the
// foreground list so it sits above the editor windows, and only while a box select is in progress.
void CourseEditDrawSelectionBox(void) {
    const CourseEditDragState& d = sCourseEditDrag;
    if (d.state != DragState::BoxSelect) {
        return;
    }
    const ImVec2 a(static_cast<float>((std::min)(d.startWindow.x, d.boxEndWindow.x)),
                   static_cast<float>((std::min)(d.startWindow.y, d.boxEndWindow.y)));
    const ImVec2 b(static_cast<float>((std::max)(d.startWindow.x, d.boxEndWindow.x)),
                   static_cast<float>((std::max)(d.startWindow.y, d.boxEndWindow.y)));
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(a, b, IM_COL32(255, 220, 90, 40));
    draw->AddRect(a, b, IM_COL32(255, 220, 90, 220), 0.0f, 0, 1.0f);
}

class CourseEditDeleteOverlay final : public Ship::GuiWindow {
public:
    CourseEditDeleteOverlay()
        : Ship::GuiWindow("gOpenWindows.CourseEditDeleteOverlay", true, "Course Edit Delete Overlay") {}

    void Draw() override {
        CourseEditDrawForkMarks();
        GdxCourseEditDrawHintStrip();
        CourseEditDrawSelectionBox();
        CourseEditDrawDeleteHover();
        CourseEditDrawLineUpPreview();
        if (CVarGetInteger("gEnhancements.CourseEdit.HistoryPanel", 0) != 0) {
            GdxCourseEditDrawHistoryPanel();
        }
    }
    void DrawElement() override {}
    void InitElement() override {}
    void UpdateElement() override {}
};
} // namespace

std::shared_ptr<Ship::GuiWindow> GdxCreateCourseEditDeleteOverlay(void) {
    return std::make_shared<CourseEditDeleteOverlay>();
}

namespace {

// Number of MOVE_OPTION_* values; 1..7 are valid hotkeys.
constexpr int32_t kMoveOptionCount = 7;

// Course Edit UI regions where LMB should keep its normal A meaning even in point mode.
bool CourseEditMouseOverChrome(int32_t x, int32_t y) {
    // Top menu bar (icon row).
    if (y < 56) {
        return true;
    }
    // Bottom-right icon cluster used for CREATE/POINT/etc. option switching.
    if (x >= 230 && x <= 297 && y >= 202 && y <= 221) {
        return true;
    }
    return false;
}

} // namespace

// Camera distance in world units (s32). Defined in decomp/src/overlays/course_edit/1A2D70.c:50.
extern "C" int32_t D_xk2_80104CB4;

namespace {

// Distance at which the scalar drag rate is the stock 1.0 (default camera distance, 1A2D70.c:50).
constexpr float kScalarReferenceDistance = 15000.0f;
// Floor from the editor zoom minimum (188850.c:203-205): 300 / 15000 = 0.02.
constexpr float kScalarDistanceScaleMin = 0.02f;
// Guard against out-of-range distances. The stock zoom ceiling of 16000 gives about 1.07.
constexpr float kScalarDistanceScaleMax = 4.0f;
// Ctrl fine mode runs at one quarter speed.
constexpr float kScalarFineMultiplier = 0.25f;
// Alt snap steps. The native side already moves width and center in steps of 10 and bank in steps of
// 3 degrees (188850.c, gdx_course_edit_set_scalar_target), so the snap steps are coarser
// multiples of those grids: 50 units for width and center, 15 degrees for bank.
constexpr float kScalarSnapStepWidth = 50.0f;
constexpr float kScalarSnapStepBank = 15.0f;

float CourseEditScalarDistanceScale(void) {
    float scale = static_cast<float>(D_xk2_80104CB4) / kScalarReferenceDistance;
    if (scale < kScalarDistanceScaleMin) {
        return kScalarDistanceScaleMin;
    }
    if (scale > kScalarDistanceScaleMax) {
        return kScalarDistanceScaleMax;
    }
    return scale;
}

bool CourseEditCtrlHeld(void) {
    return sCtrlHeld || ((SDL_GetModState() & KMOD_CTRL) != 0);
}

bool CourseEditAltHeld(void) {
    if (sAltHeld || ((SDL_GetModState() & KMOD_ALT) != 0)) {
        return true;
    }
#if defined(_WIN32)
    // The DXGI backend forwards only WM_KEYDOWN/WM_KEYUP, and Alt arrives as WM_SYSKEYDOWN, so neither
    // the key hook nor SDL sees it. Ask Windows directly, but only while this process has the foreground.
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    return foregroundPid == GetCurrentProcessId() && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
#else
    return false;
#endif
}

// Rounds half away from zero to a multiple of step, the same rounding as std::round on the stock path.
float CourseEditSnapToStep(float value, float step) {
    float scaled = value / step;
    float rounded = scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f;
    return static_cast<float>(static_cast<int32_t>(rounded)) * step;
}

// Returns the unsnapped scalar offset for this tick. The rate is recomputed every tick. When it
// changes, the value so far is carried into base and measurement restarts from the current cursor,
// so Ctrl press/release and camera zoom do not make the value jump.
float CourseEditScalarOffset(const CourseEditDragState& d, float rate) {
    CourseEditScalarDragState& s = sScalarDrag;
    if (!s.active || s.pointIndex != d.pointIndex || s.moveOption != d.moveOption ||
        s.startMouseX != d.startMouseX || s.startMouseY != d.startMouseY) {
        s.active = true;
        s.pointIndex = d.pointIndex;
        s.moveOption = d.moveOption;
        s.startMouseX = d.startMouseX;
        s.startMouseY = d.startMouseY;
        s.base = 0.0f;
        s.anchorX = d.startGameX;
        s.rate = rate;
        s.lastOffset = 0.0f;
    }
    float mouseX = d.lastGameX;
    if (rate != s.rate) {
        s.base = s.lastOffset;
        s.anchorX = mouseX;
        s.rate = rate;
    }
    s.lastOffset = s.base + (mouseX - s.anchorX) * s.rate;
    return s.lastOffset;
}

} // namespace

// Modifier queries for the help HUD. Return 1 when held, 0 otherwise.
extern "C" int gdx_course_edit_mouse_fine_held(void) {
    return CourseEditCtrlHeld() ? 1 : 0;
}

extern "C" int gdx_course_edit_mouse_snap_held(void) {
    return CourseEditAltHeld() ? 1 : 0;
}

// S3: per-frame point-drag state machine. Called from main.cpp alongside the other mouse ticks,
// after MouseStateManager::StartFrame so the button state is stable for this frame.
extern "C" void gdx_course_edit_mouse_drag_tick(void) {
    CourseEditDragState& d = sCourseEditDrag;
    static bool leftHeld = false;
    static bool rightHeld = false;
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    bool lmb = wnd != nullptr && wnd->GetMouseState(Ship::LUS_MOUSE_BTN_LEFT);
    bool rmb = wnd != nullptr && wnd->GetMouseState(Ship::LUS_MOUSE_BTN_RIGHT);
    bool lmbDown = lmb && !leftHeld;
    bool lmbUp = !lmb && leftHeld;
    bool rmbDown = rmb && !rightHeld;
    leftHeld = lmb;
    rightHeld = rmb;

    if (wnd == nullptr || wnd->IsMouseCaptured() || wnd->GetGui()->GetMenuOrMenubarVisible() ||
        CourseEditMouseInSubMenu() || CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0 ||
        !CourseEditEditorActive() || (!GdxDragEnabled() && gMoveOption != 5 && gMoveOption != 6)) {
        CourseEditCancelDrag();
        sDragBlockedUntilRelease = lmb || rmb;
        return;
    }
    if (sDragBlockedUntilRelease) {
        sDragBlockedUntilRelease = lmb || rmb;
        return;
    }

    bool nativeOwned = gdx_course_edit_native_mouse_owned() != 0;
    if (d.state == DragState::Starting) {
        if (nativeOwned) {
            d.state = DragState::Dragging;
        } else if (!gdx_course_edit_input_drag_start_pending()) {
            d = {};
        }
    } else if ((d.state == DragState::Dragging || d.state == DragState::Committing ||
                d.state == DragState::Cancelling) && !nativeOwned) {
        d = {};
    }
    if (d.state != DragState::Idle && (gCreateOption != 1 || gMoveOption != d.moveOption)) {
        CourseEditCancelDrag();
        return;
    }

    float gameX = 0.0f;
    float gameY = 0.0f;
    bool havePos = CourseEditMousePosF(wnd, &gameX, &gameY);
    const int32_t x = static_cast<int32_t>(gameX);
    const int32_t y = static_cast<int32_t>(gameY);
    // Window pixels, not game pixels: the drag threshold is measured on the real pointer.
    const Ship::Coords mouse = wnd->GetMousePos();
    if (havePos && !d.released) {
        d.lastMouseX = x;
        d.lastMouseY = y;
        d.lastGameX = gameX;
    }
    if (lmbUp) {
        d.released = true;
    }
    if (rmbDown) {
        CourseEditCancelDrag();
        return;
    }

    switch (d.state) {
        case DragState::Idle: {
            if (!lmbDown || !havePos || CourseEditMouseOverChrome(x, y) ||
                gCreateOption != 1 || gdx_course_edit_native_grab_state() != 0) {
                return;
            }
            d.released = false;
            if (gMoveOption < 0 || gMoveOption >= kMoveOptionCount) {
                return;
            }
            gCourseEditCursorXPos = x;
            gCourseEditCursorYPos = y;
            const int32_t idx = CourseEditPickPoint(gameX, gameY);
            const bool shiftHeld = sShiftHeld || ((SDL_GetModState() & KMOD_SHIFT) != 0);
            // The Delete tool arms on every press, so an empty-space press can clear the selection or start a box
            // select.
            if (idx >= 0 || shiftHeld || gMoveOption == 5) {
                // idx < 0 is a box-select candidate; Armed promotes it once the pointer moves.
                d.state = DragState::Armed;
                d.pointIndex = idx;
                d.startMouseX = x;
                d.startMouseY = y;
                d.lastMouseX = x;
                d.lastMouseY = y;
                d.moveOption = gMoveOption;
                d.startGameX = gameX;
                d.lastGameX = gameX;
                d.startWindow = mouse;
                d.boxStartX = gameX;
                d.boxStartY = gameY;
                d.boxAdditive = shiftHeld;
                // A new press never inherits a scalar drag that ended in a cancel.
                sScalarDrag = CourseEditScalarDragState();
            } else {
                gdx_course_edit_input_request_selection(-1, 0);
            }
            break;
        }
        case DragState::Armed: {
            const int32_t dxWindow = mouse.x - d.startWindow.x;
            const int32_t dyWindow = mouse.y - d.startWindow.y;
            const bool moved = dxWindow * dxWindow + dyWindow * dyWindow >= kDragStartThresholdSq;
            if (d.pointIndex < 0) {
                // Press on empty space. A moved pointer becomes a box select. A plain click clears the selection;
                // a Shift-click on empty space does nothing.
                if (moved) {
                    d.state = DragState::BoxSelect;
                    d.boxEndX = d.boxStartX;
                    d.boxEndY = d.boxStartY;
                    d.boxEndWindow = d.startWindow;
                } else if (d.released) {
                    if (!d.boxAdditive) {
                        CourseEditReplaceSelection(0);
                    }
                    d = {};
                }
                break;
            }
            if (moved && d.moveOption != 5 && d.moveOption != 6) {
                if (!gdx_course_edit_native_point_position(d.pointIndex, &d.backupPos)) {
                    d = {};
                    return;
                }
                gdx_course_edit_input_request_drag_start(d.pointIndex);
                d.state = DragState::Starting;
            } else if (moved) {
                // The Delete and line-up tools never move points, so a moved press on a point does nothing.
                d = {};
            } else if (d.released) {
                const bool shiftHeld = sShiftHeld || ((SDL_GetModState() & KMOD_SHIFT) != 0);
                if (d.moveOption == 5 && !shiftHeld) {
                    CourseEditDeleteClickedPoint(d.pointIndex);
                } else {
                    // Line up works on several points, so a plain click there adds or removes one, like Shift+click.
                    const bool toggle = shiftHeld || d.moveOption == 6;
                    gdx_course_edit_input_request_selection(d.pointIndex, toggle ? 1 : 0);
                }
                d = {};
            }
            break;
        }
        case DragState::Starting:
            break;
        case DragState::Dragging: {
            float sensitivity = GdxDragSensitivity() / 100.0f;
            if (d.moveOption == 0) {
                Vec3f origin;
                Vec3f target;
                if (gdx_course_edit_unproject_to_plane(d.startMouseX, d.startMouseY, d.backupPos.y, &origin) &&
                    gdx_course_edit_unproject_to_plane(d.lastMouseX, d.lastMouseY, d.backupPos.y, &target)) {
                    target.x = d.backupPos.x + (target.x - origin.x) * sensitivity;
                    target.z = d.backupPos.z + (target.z - origin.z) * sensitivity;
                    gdx_course_edit_input_request_move_target(d.moveOption, target);
                }
            } else if (d.moveOption == 1) {
                float originY;
                float targetY;
                if (gdx_course_edit_unproject_to_vertical_line(d.startMouseX, d.startMouseY,
                        d.backupPos.x, d.backupPos.z, &originY) &&
                    gdx_course_edit_unproject_to_vertical_line(d.lastMouseX, d.lastMouseY,
                        d.backupPos.x, d.backupPos.z, &targetY)) {
                    Vec3f target = { d.backupPos.x, d.backupPos.y + (targetY - originY) * sensitivity, d.backupPos.z };
                    gdx_course_edit_input_request_move_target(d.moveOption, target);
                }
            } else if (d.moveOption >= 2 && d.moveOption <= 4) {
                float unitsPerPixel = d.moveOption == 2 ? 5.0f : (d.moveOption == 4 ? 3.0f : 0.75f);
                float fineMultiplier = CourseEditCtrlHeld() ? kScalarFineMultiplier : 1.0f;
                float rate = unitsPerPixel * sensitivity * CourseEditScalarDistanceScale() * fineMultiplier;
                float offset = CourseEditScalarOffset(d, rate);
                int32_t target = static_cast<int32_t>(std::round(offset));
                if (CourseEditAltHeld()) {
                    const float step = d.moveOption == 3 ? kScalarSnapStepBank : kScalarSnapStepWidth;
                    target = static_cast<int32_t>(CourseEditSnapToStep(offset, step));
                }
                gdx_course_edit_input_request_scalar_target(d.moveOption, target);
                if (d.released) {
                    sScalarDrag = CourseEditScalarDragState();
                }
            }
            // The consumer applies the final target before acknowledging the release.
            if (d.released) {
                gdx_course_edit_input_request_drag_commit();
                d.state = DragState::Committing;
            }
            break;
        }
        case DragState::BoxSelect: {
            if (havePos) {
                d.boxEndX = gameX;
                d.boxEndY = gameY;
                d.boxEndWindow = mouse;
            }
            if (d.released) {
                // The native selection changes once, on release, from the box as last seen in the game area.
                const GdxCourseEditMask box = CourseEditPointsInBox(d.boxStartX, d.boxStartY, d.boxEndX, d.boxEndY);
                if (d.boxAdditive) {
                    gdx_course_edit_input_request_selection_add_mask(box);
                    CourseEditFlushSelectionAdd();
                } else {
                    CourseEditReplaceSelection(box);
                }
                d = {};
            }
            break;
        }
        case DragState::Committing:
        case DragState::Cancelling:
            break;
    }
}
