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

// Release latch for the gated inputs (the buttons and stick directions the navigation gate can swallow). A set
// bit means the input was held while the gate was closed and has not been released since, so it reads up until
// it is physically released once. Buttons use 1 << SDL_GameControllerButton; the stick uses kLatchStick* below.
uint32_t sLatched = 0;

// ImGui_ImplSDL2_UpdateGamepadAnalog's thumb deadzone, normalized. Matching it keeps the stick
// feeling identical whichever of the two feeds is live on a given platform.
constexpr float kStickDeadzone = 8000.0f / 32767.0f;

// Stick direction bits for sLatched, above every SDL_GameControllerButton value.
constexpr uint32_t kLatchStickLeft = 1u << 24;
constexpr uint32_t kLatchStickRight = 1u << 25;
constexpr uint32_t kLatchStickUp = 1u << 26;
constexpr uint32_t kLatchStickDown = 1u << 27;

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

// Returns the down state of one gated input this frame and updates its latch. While the gate is closed a held
// input takes the latch, which keeps it up after re-arm; releasing the input clears the latch.
bool GatedDown(uint32_t bit, bool held, bool armed) {
    if (!held) {
        sLatched &= ~bit;
    } else if (!armed) {
        sLatched |= bit;
    }
    return armed && held && (sLatched & bit) == 0;
}

void FeedGatedButton(ImGuiIO& io, SDL_GameController* c, SDL_GameControllerButton sdlBtn, ImGuiKey key, bool armed) {
    const bool down = GatedDown(1u << sdlBtn, SDL_GameControllerGetButton(c, sdlBtn) != 0, armed);
    if (armed) {
        io.AddKeyEvent(key, down);
    }
}

// One stick axis -> a pair of opposing analog nav keys, rescaled so the magnitude ramps from 0 at
// the deadzone edge instead of stepping. Mirrors ImGui_ImplSDL2_UpdateGamepadAnalog.
void FeedAxis(ImGuiIO& io, SDL_GameController* c, SDL_GameControllerAxis axis, ImGuiKey negKey, uint32_t negBit,
              ImGuiKey posKey, uint32_t posBit, bool armed) {
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
    const bool negDown = GatedDown(negBit, neg > 0.0f, armed);
    const bool posDown = GatedDown(posBit, pos > 0.0f, armed);
    if (armed) {
        io.AddKeyAnalogEvent(negKey, negDown, negDown ? neg : 0.0f);
        io.AddKeyAnalogEvent(posKey, posDown, posDown ? pos : 0.0f);
    }
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
    // face-button press must reach only the raw capture, never ImGui activation. Otherwise the press
    // being captured also fires whatever widget holds default focus (Cancel) and closes the popup
    // early. So the gated inputs below are not reported while the gate is closed, but they keep
    // feeding the latch: a press still held when the gate re-arms (usually the capture's own press)
    // stays up until it is released once. Unlatched, that held press would read as a fresh press on
    // re-arm and cycle the header tabs or close the menu.
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp, canUseNavigation);
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown, canUseNavigation);
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft, canUseNavigation);
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight, canUseNavigation);

    // SDL and ImGui both name face buttons by position, so the physical bottom button confirms and
    // the right one cancels on Xbox and DualSense alike. FaceLeft is deliberately unfed: ImGui
    // spends it on window switching and the menu-bar layer, neither of which this menu has, so a
    // press only flashes the window-switcher overlay and drops the active widget.
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_A, ImGuiKey_GamepadFaceDown, canUseNavigation);
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_B, ImGuiKey_GamepadFaceRight, canUseNavigation);
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_Y, ImGuiKey_GamepadFaceUp, canUseNavigation);

    // Fine/coarse slider tweak while a widget is active; the menu also cycles header tabs with them.
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ImGuiKey_GamepadL1, canUseNavigation);
    FeedGatedButton(io, c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ImGuiKey_GamepadR1, canUseNavigation);

    // Left stick scrolls the focused pane; the D-pad moves the selection. That split is ImGui's own
    // and the only one the SDL backend can also produce, so both platforms feel the same.
    FeedAxis(io, c, SDL_CONTROLLER_AXIS_LEFTX, ImGuiKey_GamepadLStickLeft, kLatchStickLeft,
             ImGuiKey_GamepadLStickRight, kLatchStickRight, canUseNavigation);
    FeedAxis(io, c, SDL_CONTROLLER_AXIS_LEFTY, ImGuiKey_GamepadLStickUp, kLatchStickUp,
             ImGuiKey_GamepadLStickDown, kLatchStickDown, canUseNavigation);
}
