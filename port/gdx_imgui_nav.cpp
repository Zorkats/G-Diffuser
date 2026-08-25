#include "gdx_imgui_nav.h"

#include <imgui.h>
#include <SDL2/SDL.h>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h" // Gui::GetMenuOrMenubarVisible (nav-flag sync)

#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger

namespace {

// Our own controller handle for menu navigation: the game-facing pad is intentionally zeroed while
// the menu blocks game input, so this reads physical state instead. SDL allows several open handles
// on one device, so this does not disturb the ControlDeck or an ImGui backend.
SDL_GameController* sController = nullptr;

// True while this module, rather than an ImGui platform backend, is the source of the gamepad keys.
bool sOwnsFeed = false;

// ImGui_ImplSDL2_UpdateGamepadAnalog's thumb deadzone, normalized. Matching it keeps the stick
// feeling identical whichever of the two feeds is live on a given platform.
constexpr float kStickDeadzone = 8000.0f / 32767.0f;

SDL_GameController* AcquireController() {
    if (sController != nullptr && SDL_GameControllerGetAttached(sController)) {
        return sController;
    }
    if (sController != nullptr) {
        SDL_GameControllerClose(sController);
        sController = nullptr;
    }
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            sController = SDL_GameControllerOpen(i);
            if (sController != nullptr) {
                break;
            }
        }
    }
    return sController;
}

void FeedButton(ImGuiIO& io, SDL_GameController* c, SDL_GameControllerButton sdlBtn, ImGuiKey key) {
    io.AddKeyEvent(key, SDL_GameControllerGetButton(c, sdlBtn) != 0);
}

// One stick axis -> a pair of opposing analog nav keys, rescaled so the magnitude ramps from 0 at
// the deadzone edge instead of stepping. Mirrors ImGui_ImplSDL2_UpdateGamepadAnalog.
void FeedAxis(ImGuiIO& io, SDL_GameController* c, SDL_GameControllerAxis axis, ImGuiKey negKey,
              ImGuiKey posKey) {
    const float v = static_cast<float>(SDL_GameControllerGetAxis(c, axis)) / 32767.0f;
    const float mag = (v < 0.0f) ? -v : v;
    float out = 0.0f;
    if (mag > kStickDeadzone) {
        out = (mag - kStickDeadzone) / (1.0f - kStickDeadzone);
        if (out > 1.0f) {
            out = 1.0f;
        }
    }
    const float neg = (v < 0.0f) ? out : 0.0f;
    const float pos = (v > 0.0f) ? out : 0.0f;
    io.AddKeyAnalogEvent(negKey, neg > 0.0f, neg);
    io.AddKeyAnalogEvent(posKey, pos > 0.0f, pos);
}

} // namespace

extern "C" void gdx_imgui_nav_tick(void) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();

    // Drop last frame's claim before testing the flag, so whatever remains was set by a backend.
    if (sOwnsFeed) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        sOwnsFeed = false;
    }

    auto ctx = Ship::Context::GetInstance();
    auto window = ctx != nullptr ? ctx->GetWindow() : nullptr;
    auto gui = window != nullptr ? window->GetGui() : nullptr;
    const bool navOn = CVarGetInteger("gControlNav", 0) != 0;
    const bool canUseNavigation = gui != nullptr && gui->CanUseGamepadNavigation();
    if (canUseNavigation) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }
    if (!navOn) {
        return;
    }

    // Exactly one writer per ImGuiKey. Two readers queueing different values for one key in a frame
    // make ImGui stop applying the rest of the event queue (imgui.cpp UpdateInputEvents' trickling
    // rule), which shows up as nav that fires every other frame and input that lands late. The SDL2
    // backend feeds the whole gamepad itself, so we only step in where no backend does: Win32/DX11,
    // whose XInput reader is compiled out (IMGUI_IMPL_WIN32_DISABLE_GAMEPAD, windows.cmake) because
    // it cannot see a raw DualSense.
    if (io.BackendFlags & ImGuiBackendFlags_HasGamepad) {
        return;
    }

    SDL_GameController* c = AcquireController();
    if (c == nullptr) {
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    sOwnsFeed = true;

    // Read by libultraship's Gui toggle even while the menu is closed, so these stay fed
    // unconditionally: Back is the pad's only path to OPEN the menu (CanUseGamepadNavigation is
    // false while it is hidden).
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_BACK, ImGuiKey_GamepadBack);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_START, ImGuiKey_GamepadStart);

    // Honor the nav block (input-editor capture popup sets it): while navigation is disallowed a
    // face-button press must reach only the raw capture, never ImGui activation — otherwise the
    // press being captured also fires whatever widget holds default focus (Cancel) and closes the
    // popup early.
    if (!canUseNavigation) {
        return;
    }

    FeedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight);

    // SDL and ImGui both name face buttons by position, so the physical bottom button confirms and
    // the right one cancels on Xbox and DualSense alike. FaceLeft is deliberately unfed: ImGui
    // spends it on window switching and the menu-bar layer, neither of which this menu has, so a
    // press only flashes the window-switcher overlay and drops the active widget.
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_A, ImGuiKey_GamepadFaceDown);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_B, ImGuiKey_GamepadFaceRight);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_Y, ImGuiKey_GamepadFaceUp);

    // Fine/coarse slider tweak while a widget is active; the menu also cycles header tabs with them.
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ImGuiKey_GamepadL1);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ImGuiKey_GamepadR1);

    // Left stick scrolls the focused pane; the D-pad moves the selection. That split is ImGui's own
    // and the only one the SDL backend can also produce, so both platforms feel the same.
    FeedAxis(io, c, SDL_CONTROLLER_AXIS_LEFTX, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
    FeedAxis(io, c, SDL_CONTROLLER_AXIS_LEFTY, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown);
}
