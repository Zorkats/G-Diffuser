// G-Diffuser — Course Edit / Create Machine absolute mouse drive + issue #18 race mouse steering.
//
// TWO SEPARATE MECHANISMS share this file only because both are thin extern-"C" wrappers over
// Fast3dWindow for decomp-side C callers:
//
//   1. Editor absolute drive (gdx_course_edit_mouse_pos): maps the window mouse position back
//      through the pillarbox blit rect (Fast3dGui::GetGameBlitRect) into the 320x240 space the
//      editor cursor lives in. Used by both Course Edit cursor drivers
//      (decomp/src/overlays/course_edit/188850.c) and the Create Machine update loop; the callers
//      keep their own stock clamps, so this shim deliberately does not know per-mode ranges.
//      Course Edit grab mode (D_800D6CA0.unk_00 == 1) is suppressed because grabbing moves geometry
//      via the stick and already suppresses the cursor draw.
//
//   2. Issue #18 race steering (gdx_mouse_steer_stick_x): absolute position steering for
//      port/input_bridge.c — cursor X inside the blit rect maps to stick deflection. Mode-disjoint
//      with the absolute drive above (race modes only), so the two never fight over the cursor.

#include <chrono>
#include <cstdint>
#include <memory>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"

#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger

// decomp controller.h is not C++-clean under MSVC; the N64 button masks are stable ABI bits.
#define GDX_BTN_A 0x8000
#define GDX_BTN_B 0x4000

// Decomp globals. Plain int is ABI-identical to the decomp's s32 on this target (same idiom as
// port/input_bridge.c); gInCourseEditTestRun is a decomp bool. GET_MODE masks the F3D-variant
// bits: GET_MODE(gGameMode) == (gGameMode & 0x1F), GAMEMODE_COURSE_EDIT == 0xD and
// GAMEMODE_CREATE_MACHINE == 0x10 (fzx_game.h).
extern "C" int gGameMode;
extern "C" bool gInCourseEditTestRun;
extern "C" int gWorksMachineMode;
extern "C" int D_xk2_80119918; // course_edit/188850.c help/pause overlay flag

// Only the first word of unk_800D6CA0 (decomp/include/unk_structs.h:606) is read for grab mode;
// unk_08 is the Course Edit menu/dialog state. The struct is re-declared rather than including
// decomp headers into a host TU.
namespace {
struct CourseEditGrabPeek {
    int unk_00;
    int unk_08;
};
} // namespace
extern "C" CourseEditGrabPeek D_800D6CA0;

namespace {

std::shared_ptr<Fast::Fast3dWindow> CourseEditMouseWindow() {
    return std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
}

// Course Edit test-run/grab suppression only applies to Course Edit; Create Machine has neither.
bool CourseEditMouseModeActive(void) {
    switch (gGameMode & 0x1F) {
        case 0xD: // GAMEMODE_COURSE_EDIT
            return !gInCourseEditTestRun && (D_800D6CA0.unk_00 != 1);
        case 0x10: // GAMEMODE_CREATE_MACHINE
            return true;
        default:
            return false;
    }
}

// Sub-menus, dialogs, and full sub-screens keep the in-game cursor but should not confine the
// OS cursor: the user may want to move the real cursor to the window edge or another monitor
// while a popup/overlay is open. Course Edit covers every D_800D6CA0.unk_08 state (dialogs,
// pickers, CREATE/POINT screens) plus the START help overlay that does not touch unk_08.
bool CourseEditMouseInSubMenu(void) {
    switch (gGameMode & 0x1F) {
        case 0xD: // GAMEMODE_COURSE_EDIT
            return (D_800D6CA0.unk_08 != 0) || (D_xk2_80119918 != 0);
        case 0x10: // GAMEMODE_CREATE_MACHINE
            return gWorksMachineMode != 0;
        default:
            return false;
    }
}

// Full S1/S4 gate: CVar on, in Course Edit or Create Machine, not test-driving (Course Edit only),
// not grabbing geometry (Course Edit only), no menu/menubar up, and the OS cursor inside the game
// blit rect. On success outX/outY receive the cursor position in 320x240 game space.
bool CourseEditMousePos(std::shared_ptr<Fast::Fast3dWindow>& wnd, int* outX, int* outY) {
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
    *outX = static_cast<int>((mouse.x - rx) * 320.0f / rw);
    *outY = static_cast<int>((mouse.y - ry) * 240.0f / rh);
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

// S2: per-frame OS-cursor hide, called from main.cpp AFTER MouseStateManager::StartFrame — the
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
// deflection — center is straight, the edges are full lock — so a line can be held instead of
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
// the per-frame hide wins while the game renders. Only force-shows on the hidden -> shown
// transition; with the CVar off or the F2 capture active it leaves cursor state to everyone
// else (MouseStateManager, the Course Edit cursor tick above).
extern "C" void gdx_hide_os_cursor_tick(void) {
    static bool sHidden = false;
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->IsMouseCaptured()) {
        sHidden = false;
        return;
    }
    bool show = wnd->GetGui()->GetMenuOrMenubarVisible() ||
                CVarGetInteger("gEnhancements.Input.HideCursorInGame", 0) == 0;
    if (show) {
        if (sHidden) {
            wnd->SetCursorVisibility(true);
            sHidden = false;
        }
        return;
    }
    wnd->SetCursorVisibility(false);
    sHidden = true;
}

// S4: inject N64 A/B from OS left/right mouse clicks while the absolute mouse drive owns the
// in-game cursor. OR'd onto the LUS ControlDeck state in input_bridge.c so the editor cursors work
// out-of-the-box without manual LMB/RMB bindings.
extern "C" int gdx_course_edit_mouse_buttons(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd = CourseEditMouseWindow();
    if (wnd == nullptr || wnd->IsMouseCaptured() || wnd->GetGui()->GetMenuOrMenubarVisible()) {
        return 0;
    }
    if (CVarGetInteger("gEnhancements.Input.CourseEditMouse", 0) == 0 || !CourseEditMouseModeActive()) {
        return 0;
    }
    int x, y;
    if (!CourseEditMousePos(wnd, &x, &y)) {
        return 0;
    }
    int buttons = 0;
    if (wnd->GetMouseState(Ship::LUS_MOUSE_BTN_LEFT)) {
        buttons |= GDX_BTN_A;
    }
    if (wnd->GetMouseState(Ship::LUS_MOUSE_BTN_RIGHT)) {
        buttons |= GDX_BTN_B;
    }
    return buttons;
}

// Expose the OS mouse wheel as integer detents while the absolute mouse drive owns the cursor.
// Positive return values move the selection down (wheel toward the user), negative values move it
// up; zero means no detent this frame or the mouse-controls CVar is off.
extern "C" int gdx_course_edit_mouse_wheel(void) {
    std::shared_ptr<Fast::Fast3dWindow> wnd;
    int x, y;
    if (!CourseEditMousePos(wnd, &x, &y)) {
        return 0;
    }
    static float sWheelAccumulator = 0.0f;
    sWheelAccumulator += wnd->GetMouseWheel().y;
    if (sWheelAccumulator >= 1.0f) {
        int steps = static_cast<int>(sWheelAccumulator);
        sWheelAccumulator -= steps;
        return -steps;
    }
    if (sWheelAccumulator <= -1.0f) {
        int steps = static_cast<int>(-sWheelAccumulator);
        sWheelAccumulator += steps;
        return steps;
    }
    return 0;
}
