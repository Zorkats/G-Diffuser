// Test-only services. Production Ship mapping headers and .cpp implementations are not mocked.
#pragma once
#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "SDL2/SDL.h"
#include "ship/controller/controldevice/controller/mapping/ControllerButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h"

namespace Ship {
inline int legacyButtonReads = 0;
inline int legacyMappingReads = 0;

class FakeCVars {
  public:
    // Fail even with NDEBUG: the override module must never save/delete legacy mapping configuration.
    void SetString(const char*, const char*) { std::abort(); }
    void SetInteger(const char*, int) { std::abort(); }
    void Save() { std::abort(); }
    void ClearVariable(const char*) { std::abort(); }
    std::string GetString(const char*, const char* fallback) { return fallback; }
    int GetInteger(const char*, int fallback) { return fallback; }
};
class Gui {
  public:
    bool gamepadNavigationBlocked = false;
    void SetGamepadNavigationBlocked(bool blocked) { gamepadNavigationBlocked = blocked; }
    void SetInputCaptureActive(bool active) { inputCaptureActive = active; }
    bool IsInputCaptureActive() const { return inputCaptureActive; }

  private:
    bool inputCaptureActive = false;
};
class Window {
  public:
    std::shared_ptr<Gui> gui = std::make_shared<Gui>();
    auto GetGui() { return gui; }
    std::array<bool, LUS_MOUSE_BTN_COUNT> mouse{};
    bool GetMouseState(MouseBtn button) { return mouse[button]; }
    const char* GetKeyName(int) { return "Key"; }
};
class ControllerButton {
  public:
    std::unordered_map<std::string, std::shared_ptr<ControllerButtonMapping>> mappings;
    auto GetAllButtonMappings() { ++legacyMappingReads; return mappings; }
};
class Controller {
  public:
    std::unordered_map<uint16_t, std::shared_ptr<ControllerButton>> buttons;
    auto GetAllButtons() { ++legacyButtonReads; return buttons; }
};
class GlobalSDLDeviceSettings {
  public:
    int stick = 25, trigger = 25;
    int GetStickAxisThresholdPercentage() { return stick; }
    int GetTriggerAxisThresholdPercentage() { return trigger; }
};
class ConnectedPhysicalDeviceManager {
  public:
    // Only connected, enabled devices appear here, as in the production per-port filtered manager.
    std::array<std::unordered_map<int32_t, SDL_GameController*>, 4> ports;
    auto GetConnectedSDLGamepadsForPort(uint8_t port) { return ports[port]; }
};
class FakeDefaults {
  public:
    std::unordered_map<uint16_t, std::unordered_set<KbScancode>> GetDefaultKeyboardKeyToButtonMappings() { return {}; }
    std::unordered_map<uint16_t, std::unordered_set<SDL_GameControllerButton>>
    GetDefaultSDLButtonToButtonMappings() { return {}; }
    std::unordered_map<uint16_t, std::vector<std::pair<SDL_GameControllerAxis, int>>>
    GetDefaultSDLAxisDirectionToButtonMappings() { return {}; }
};
class ControlDeck {
  public:
    bool keyboardBlocked = false, gamepadBlocked = false, mouseBlocked = false;
    std::array<std::shared_ptr<Controller>, 4> controllers;
    std::shared_ptr<ConnectedPhysicalDeviceManager> devices = std::make_shared<ConnectedPhysicalDeviceManager>();
    std::shared_ptr<GlobalSDLDeviceSettings> settings = std::make_shared<GlobalSDLDeviceSettings>();
    // Mirror production's generic-blocker OR device-specific-blocker contract, without ImGui.
    bool GameInputBlocked() { return AllGameInputBlocked(); }
    bool KeyboardGameInputBlocked() { return AllGameInputBlocked() || keyboardBlocked; }
    bool GamepadGameInputBlocked() { return AllGameInputBlocked() || gamepadBlocked; }
    bool MouseGameInputBlocked() { return AllGameInputBlocked() || mouseBlocked; }
    void BlockGameInput(int32_t id) { blockers.insert(id); }
    void UnblockGameInput(int32_t id) { blockers.erase(id); }
    auto GetControllerByPort(uint8_t port) { return controllers[port]; }
    auto GetConnectedPhysicalDeviceManager() { return devices; }
    auto GetGlobalSDLDeviceSettings() { return settings; }
    auto GetControllerDefaultMappings() { return std::make_shared<FakeDefaults>(); }

  private:
    bool AllGameInputBlocked() { return !blockers.empty(); }
    std::set<int32_t> blockers;
};
class Context {
  public:
    inline static std::shared_ptr<Context> current;
    std::shared_ptr<ControlDeck> deck = std::make_shared<ControlDeck>();
    std::shared_ptr<Window> window = std::make_shared<Window>();
    static auto GetInstance() { return current; }
    auto GetControlDeck() { return deck; }
    auto GetWindow() { return window; }
    auto GetConsoleVariables() { return std::make_shared<FakeCVars>(); }
};
struct WheelDirections {
    WheelDirection X = LUS_WHEEL_NONE, Y = LUS_WHEEL_NONE;
};
class WheelHandler {
  public:
    WheelDirections directions;
    static auto GetInstance() { static auto instance = std::make_shared<WheelHandler>(); return instance; }
    auto GetDirections() { return directions; }
};
} // namespace Ship
