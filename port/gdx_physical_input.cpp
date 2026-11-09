// G-Diffuser: per-action physical overrides, independent of the legacy controller configuration.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "gdx_physical_input.h"

#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/controldevice/controller/mapping/factories/ButtonMappingFactory.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/mouse/MouseButtonToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/mouse/MouseWheelToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/mouse/WheelHandler.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "ship/window/Window.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// LUS's CVar compile definitions are local to its source directory; retain the same default here.
#ifndef CVAR_SIMULATED_INPUT_LAG
#define CVAR_SIMULATED_INPUT_LAG "gSimulatedInputLag"
#endif

namespace {
using Binding = std::shared_ptr<Ship::ControllerButtonMapping>;
using Bindings = std::vector<Binding>;
constexpr int kPorts = 4;
constexpr int kMaxBindings = 256; // Bound work and allocations for malformed external configuration.

// Stable on-disk values. Code is the concrete mapping's key/button/axis/wheel enum.
enum SourceType { Keyboard = 1, SDLButton = 2, SDLAxis = 3, MouseButton = 4, MouseWheel = 5 };
struct Descriptor {
    int type;
    int code;
    int direction;
    bool operator==(const Descriptor& other) const {
        return std::tie(type, code, direction) == std::tie(other.type, other.code, other.direction);
    }
    bool operator<(const Descriptor& other) const {
        return std::tie(type, code, direction) < std::tie(other.type, other.code, other.direction);
    }
};
struct Bank {
    bool loaded = false;
    bool configured = false;
    std::vector<Descriptor> saved;
    Bindings bindings;
    bool inheritedLoaded = false;
    unsigned short inheritedMask = 0;
    std::vector<Descriptor> inherited;
    Bindings inheritedBindings;
};
struct SourceState {
    bool down = false;
    int context = -1;
};
std::array<std::array<std::array<Bank, GDX_INPUT_OUT_COUNT>, GDX_INPUT_CONTEXT_COUNT>, kPorts> sBanks;
std::array<std::map<Descriptor, SourceState>, kPorts> sSources;
std::array<bool, kPorts> sPortLive{};
std::array<std::deque<unsigned short>, kPorts> sPhysicalHistory;
std::array<std::weak_ptr<Ship::ControlDeck>, kPorts> sHistoryDeck;
std::set<int> sHeldKeys;
std::set<int> sConsumedKeys; // Suppressed until release, including repeats and mapping rebuilds.
bool sBanksDirty = true;
bool sKeyboardCapture = false;
int sCapturedKey = -1;
const char* const kContexts[] = { "Menu", "Race", "CourseEdit" };
const char* const kOutputs[] = { "A", "B", "Z", "Start", "DUp", "DDown", "DLeft", "DRight",
                                "L", "R", "CUp", "CDown", "CLeft", "CRight" };

bool Valid(int port, int context, int output) {
    return port >= 0 && port < kPorts && context >= 0 && context < GDX_INPUT_CONTEXT_COUNT &&
           output >= 0 && output < GDX_INPUT_OUT_COUNT;
}
std::string Prefix(int port, int context, int output) {
    return "gEnhancements.Input.Physical.Port" + std::to_string(port + 1) + "." + kContexts[context] + "." +
           kOutputs[output];
}
bool Describe(const Binding& binding, Descriptor& result) {
    if (auto key = std::dynamic_pointer_cast<Ship::KeyboardKeyToButtonMapping>(binding)) {
        result = { Keyboard, key->GetKeyboardScancode(), 0 };
    } else if (auto button = std::dynamic_pointer_cast<Ship::SDLButtonToButtonMapping>(binding)) {
        result = { SDLButton, button->GetSDLControllerButton(), 0 };
    } else if (auto axis = std::dynamic_pointer_cast<Ship::SDLAxisDirectionToButtonMapping>(binding)) {
        result = { SDLAxis, axis->GetSDLControllerAxis(), axis->GetAxisDirection() };
    } else if (auto mouse = std::dynamic_pointer_cast<Ship::MouseButtonToButtonMapping>(binding)) {
        result = { MouseButton, mouse->GetMouseButton(), 0 };
    } else if (auto wheel = std::dynamic_pointer_cast<Ship::MouseWheelToButtonMapping>(binding)) {
        result = { MouseWheel, wheel->GetWheelDirection(), 0 };
    } else {
        return false;
    }
    return true;
}
Binding Create(int port, int output, const Descriptor& descriptor) {
    const auto bit = gdx_input_actions_output_bit(output);
    switch (descriptor.type) {
        case Keyboard:
            if (descriptor.code > Ship::LUS_KB_UNKNOWN && descriptor.code < Ship::LUS_KB_MAX &&
                descriptor.direction == 0) {
                auto key = std::make_shared<Ship::KeyboardKeyToButtonMapping>(
                    port, bit, static_cast<Ship::KbScancode>(descriptor.code));
                if (sHeldKeys.count(descriptor.code) && !sConsumedKeys.count(descriptor.code)) {
                    key->ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_DOWN,
                                              static_cast<Ship::KbScancode>(descriptor.code));
                }
                return key;
            }
            break;
        case SDLButton:
            if (descriptor.code >= 0 && descriptor.code < SDL_CONTROLLER_BUTTON_MAX && descriptor.direction == 0) {
                return std::make_shared<Ship::SDLButtonToButtonMapping>(port, bit, descriptor.code);
            }
            break;
        case SDLAxis:
            if (descriptor.code >= 0 && descriptor.code < SDL_CONTROLLER_AXIS_MAX &&
                (descriptor.direction == Ship::NEGATIVE || descriptor.direction == Ship::POSITIVE)) {
                return std::make_shared<Ship::SDLAxisDirectionToButtonMapping>(port, bit, descriptor.code,
                                                                             descriptor.direction);
            }
            break;
        case MouseButton:
            if (descriptor.code >= 0 && descriptor.code < Ship::LUS_MOUSE_BTN_COUNT && descriptor.direction == 0) {
                return std::make_shared<Ship::MouseButtonToButtonMapping>(
                    port, bit, static_cast<Ship::MouseBtn>(descriptor.code));
            }
            break;
        case MouseWheel:
            if (descriptor.code > Ship::LUS_WHEEL_NONE && descriptor.code < Ship::LUS_WHEEL_UNKNOWN &&
                descriptor.direction == 0) {
                return std::make_shared<Ship::MouseWheelToButtonMapping>(
                    port, bit, static_cast<Ship::WheelDirection>(descriptor.code));
            }
            break;
    }
    return nullptr;
}
Bindings CreateAll(int port, int output, const std::vector<Descriptor>& descriptors) {
    Bindings bindings;
    for (const auto& descriptor : descriptors) {
        if (auto binding = Create(port, output, descriptor)) {
            bindings.push_back(binding);
        }
    }
    return bindings;
}
Bank& Refresh(int port, int context, int output) {
    auto& bank = sBanks[port][context][output];
    if (bank.loaded) {
        return bank;
    }
    const auto prefix = Prefix(port, context, output);
    const bool configured = CVarGetInteger((prefix + ".Configured").c_str(), 0) != 0;
    const int count = configured ? std::clamp(CVarGetInteger((prefix + ".Count").c_str(), 0), 0, kMaxBindings) : 0;
    std::vector<Descriptor> descriptors;
    for (int i = 0; i < count; ++i) {
        const auto entry = prefix + "." + std::to_string(i);
        descriptors.push_back({ CVarGetInteger((entry + ".Type").c_str(), 0),
                                CVarGetInteger((entry + ".Code").c_str(), -1),
                                CVarGetInteger((entry + ".Direction").c_str(), 0) });
    }
    // Compare the complete fingerprint only on invalidation, including same-count external edits.
    bank.loaded = true;
    if (bank.configured != configured || bank.saved != descriptors) {
        bank.configured = configured;
        bank.saved = std::move(descriptors);
        bank.bindings = CreateAll(port, output, bank.saved);
    }
    return bank;
}
const Bindings& Effective(int port, int context, int output) {
    auto& bank = Refresh(port, context, output);
    if (bank.configured) {
        return bank.bindings;
    }
    // Action masks are already cached by the legacy layer. Detect its UI setters without
    // constructing paths or traversing controller mappings on an unchanged inherited row.
    const auto mask = gdx_input_actions_get_mask(context, output);
    if (bank.inheritedLoaded && bank.inheritedMask == mask) {
        return bank.inheritedBindings;
    }
    std::set<Descriptor> descriptors;
    auto ctx = Ship::Context::GetInstance();
    auto deck = ctx != nullptr ? ctx->GetControlDeck() : nullptr;
    auto controller = deck != nullptr ? deck->GetControllerByPort(port) : nullptr;
    if (controller != nullptr) {
        for (const auto& [bit, button] : controller->GetAllButtons()) {
            if ((mask & bit) == 0 || button == nullptr) {
                continue;
            }
            for (const auto& [id, binding] : button->GetAllButtonMappings()) {
                Descriptor descriptor;
                if (Describe(binding, descriptor)) {
                    descriptors.insert(descriptor);
                }
            }
        }
    }
    // Deterministic physical order makes RemoveBinding's UI index stable across unordered legacy maps.
    std::vector<Descriptor> inherited(descriptors.begin(), descriptors.end());
    if (bank.inherited != inherited) {
        bank.inherited = std::move(inherited);
        bank.inheritedBindings = CreateAll(port, output, bank.inherited);
    }
    bank.inheritedLoaded = deck != nullptr;
    bank.inheritedMask = mask;
    return bank.inheritedBindings;
}
void Store(int port, int context, int output, const Bindings& bindings, bool configured, bool save = true) {
    const auto prefix = Prefix(port, context, output);
    int count = 0;
    for (const auto& binding : bindings) {
        Descriptor descriptor;
        if (!Describe(binding, descriptor)) {
            continue;
        }
        const auto entry = prefix + "." + std::to_string(count++);
        CVarSetInteger((entry + ".Type").c_str(), descriptor.type);
        CVarSetInteger((entry + ".Code").c_str(), descriptor.code);
        CVarSetInteger((entry + ".Direction").c_str(), descriptor.direction);
    }
    CVarSetInteger((prefix + ".Count").c_str(), count);
    CVarSetInteger((prefix + ".Configured").c_str(), configured ? 1 : 0);
    auto& bank = sBanks[port][context][output];
    bank.loaded = false;
    if (!configured) {
        bank.inheritedLoaded = false;
    }
    Refresh(port, context, output);
    if (save) {
        CVarSave();
    }
}

// Sample even unbound sources, independently of UI blockers. Otherwise a held input could acquire a
// fresh context when a bank is loaded, a capture is committed, or a blocker goes away.
bool SampleSources(int port, int context, const std::shared_ptr<Ship::ControlDeck>& deck,
                   const std::shared_ptr<Ship::Window>& window) {
    std::map<Descriptor, bool> held;
    for (int key : sHeldKeys) {
        held[{ Keyboard, key, 0 }] = !sConsumedKeys.count(key);
    }
    const auto directions = Ship::WheelHandler::GetInstance()->GetDirections();
    for (int button = 0; button < Ship::LUS_MOUSE_BTN_COUNT; ++button) {
        held[{ MouseButton, button, 0 }] = window->GetMouseState(static_cast<Ship::MouseBtn>(button));
    }
    for (int wheel = Ship::LUS_WHEEL_LEFT; wheel < Ship::LUS_WHEEL_UNKNOWN; ++wheel) {
        held[{ MouseWheel, wheel, 0 }] = wheel == directions.X || wheel == directions.Y;
    }
    const auto settings = deck->GetGlobalSDLDeviceSettings();
    const auto gamepads = deck->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(port);
    bool hasGamepad = false;
    for (const auto& [id, gamepad] : gamepads) {
        if (gamepad == nullptr || !SDL_GameControllerGetAttached(gamepad)) {
            continue;
        }
        hasGamepad = true;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
            held[{ SDLButton, button, 0 }] |=
                SDL_GameControllerGetButton(gamepad, static_cast<SDL_GameControllerButton>(button)) != 0;
        }
        for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis) {
            const bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            const int threshold = trigger ? settings->GetTriggerAxisThresholdPercentage()
                                          : settings->GetStickAxisThresholdPercentage();
            const float minimum = SDL_JOYSTICK_AXIS_MAX * (threshold / 100.0f);
            const auto value = SDL_GameControllerGetAxis(gamepad, static_cast<SDL_GameControllerAxis>(axis));
            held[{ SDLAxis, axis, Ship::POSITIVE }] |= value > minimum;
            held[{ SDLAxis, axis, Ship::NEGATIVE }] |= value < -minimum;
        }
    }
    auto& sources = sSources[port];
    for (auto& [descriptor, state] : sources) {
        if (!held[descriptor]) {
            state = {};
        }
    }
    for (const auto& [descriptor, down] : held) {
        auto& state = sources[descriptor];
        if (down && !state.down) {
            state.context = context;
        }
        state.down = down;
    }
    return hasGamepad;
}
} // namespace

namespace GdxPhysicalInput {
void Invalidate() {
    for (auto& port : sBanks) {
        for (auto& context : port) {
            for (auto& bank : context) {
                bank.loaded = false;
                bank.inheritedLoaded = false;
            }
        }
    }
    sBanksDirty = true;
    gdx_input_actions_mark_dirty();
}
Bindings GetBindings(int port, int context, int output) {
    return Valid(port, context, output) ? Refresh(port, context, output).bindings : Bindings{};
}
Bindings GetEffectiveBindings(int port, int context, int output) {
    return Valid(port, context, output) ? Effective(port, context, output) : Bindings{};
}
bool IsConfigured(int port, int context, int output) {
    return Valid(port, context, output) && Refresh(port, context, output).configured;
}
bool AddBinding(int port, int context, int output, Binding binding) {
    Descriptor descriptor;
    if (!Valid(port, context, output) || !Describe(binding, descriptor)) {
        return false;
    }
    auto clone = Create(port, output, descriptor);
    if (clone == nullptr) {
        return false;
    }
    Bindings bindings = Effective(port, context, output);
    const bool duplicate = std::any_of(bindings.begin(), bindings.end(), [&](const Binding& existing) {
        Descriptor current;
        return Describe(existing, current) && current == descriptor;
    });
    if (bindings.size() > kMaxBindings) {
        return false;
    }
    if (!duplicate) {
        if (bindings.size() >= kMaxBindings) {
            return false;
        }
        bindings.push_back(clone);
    }
    Store(port, context, output, bindings, true);
    return true;
}
bool ReplaceBinding(int port, int context, int output, int index, Binding binding) {
    Descriptor descriptor;
    if (!Valid(port, context, output) || index < 0 || !Describe(binding, descriptor)) {
        return false;
    }
    auto clone = Create(port, output, descriptor);
    if (clone == nullptr) {
        return false;
    }
    Bindings bindings = Effective(port, context, output);
    if (static_cast<size_t>(index) >= bindings.size()) {
        return false;
    }
    bindings[index] = std::move(clone);
    std::set<Descriptor> seen;
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const Binding& existing) {
        Descriptor current;
        return Describe(existing, current) && !seen.insert(current).second;
    }), bindings.end());
    if (bindings.size() > kMaxBindings) {
        return false;
    }
    Store(port, context, output, bindings, true);
    return true;
}
bool RemoveBinding(int port, int context, int output, int index) {
    if (!Valid(port, context, output)) {
        return false;
    }
    Bindings bindings = Effective(port, context, output);
    if (index < 0 || static_cast<size_t>(index) >= bindings.size()) {
        return false;
    }
    bindings.erase(bindings.begin() + index);
    if (bindings.size() > kMaxBindings) {
        return false;
    }
    Store(port, context, output, bindings, true);
    return true;
}
void ClearBindings(int port, int context, int output) {
    if (Valid(port, context, output)) {
        Store(port, context, output, {}, true);
    }
}
void UseLegacyBindings(int port, int context, int output) {
    if (Valid(port, context, output)) {
        Store(port, context, output, {}, false);
    }
}
Binding CaptureSDL(int port, int output) {
    auto ctx = Ship::Context::GetInstance();
    if (!Valid(port, GDX_INPUT_CONTEXT_MENU, output) || ctx == nullptr || ctx->GetControlDeck() == nullptr) {
        return nullptr;
    }
    return Ship::ButtonMappingFactory::CreateButtonMappingFromSDLInput(port, gdx_input_actions_output_bit(output));
}
bool IsGamepadInputHeld(int port) {
    if (port < 0 || port >= kPorts) {
        return false;
    }
    auto ctx = Ship::Context::GetInstance();
    auto deck = ctx != nullptr ? ctx->GetControlDeck() : nullptr;
    auto devices = deck != nullptr ? deck->GetConnectedPhysicalDeviceManager() : nullptr;
    if (devices == nullptr) {
        return false;
    }
    for (const auto& [instanceId, gamepad] : devices->GetConnectedSDLGamepadsForPort(port)) {
        if (gamepad == nullptr || !SDL_GameControllerGetAttached(gamepad)) {
            continue;
        }
        for (int button = SDL_CONTROLLER_BUTTON_A; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
            if (SDL_GameControllerGetButton(gamepad, static_cast<SDL_GameControllerButton>(button))) {
                return true;
            }
        }
        for (int axis = SDL_CONTROLLER_AXIS_LEFTX; axis < SDL_CONTROLLER_AXIS_MAX; ++axis) {
            const float value =
                SDL_GameControllerGetAxis(gamepad, static_cast<SDL_GameControllerAxis>(axis)) / 32767.0f;
            if (value < -0.7f || value > 0.7f) {
                return true;
            }
        }
    }
    return false;
}

std::string ExportGamepadProfile(int port, const std::string& name, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (port < 0 || port >= kPorts) {
        if (error != nullptr) *error = "Select a valid player port first.";
        return {};
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("format"); writer.String("gdiffuser.gamepad-profile");
    writer.Key("version"); writer.Int(1);
    writer.Key("name"); writer.String(name.c_str(), static_cast<rapidjson::SizeType>(name.size()));
    writer.Key("contexts"); writer.StartObject();
    for (int context = 0; context < GDX_INPUT_CONTEXT_COUNT; ++context) {
        writer.Key(kContexts[context]); writer.StartObject();
        for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
            writer.Key(kOutputs[output]); writer.StartArray();
            for (const auto& binding : Effective(port, context, output)) {
                Descriptor descriptor;
                if (!Describe(binding, descriptor) ||
                    (descriptor.type != SDLButton && descriptor.type != SDLAxis)) {
                    continue;
                }
                writer.StartObject();
                writer.Key("type"); writer.String(descriptor.type == SDLButton ? "button" : "axis");
                writer.Key("code"); writer.Int(descriptor.code);
                writer.Key("direction"); writer.Int(descriptor.direction);
                writer.EndObject();
            }
            writer.EndArray();
        }
        writer.EndObject();
    }
    writer.EndObject();
    writer.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize());
}

bool ImportGamepadProfile(int port, const std::string& json, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    auto fail = [error](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    constexpr size_t kMaxProfileBytes = 1024 * 1024;
    if (port < 0 || port >= kPorts) {
        return fail("Select a valid player port first.");
    }
    if (json.empty() || json.size() > kMaxProfileBytes) {
        return fail("The profile file is empty or too large.");
    }

    rapidjson::Document document;
    document.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());
    if (document.HasParseError() || !document.IsObject()) {
        return fail("The profile is not valid JSON.");
    }
    const auto format = document.FindMember("format");
    const auto version = document.FindMember("version");
    const auto contexts = document.FindMember("contexts");
    if (format == document.MemberEnd() || !format->value.IsString() ||
        std::string(format->value.GetString(), format->value.GetStringLength()) != "gdiffuser.gamepad-profile" ||
        version == document.MemberEnd() || !version->value.IsInt() || version->value.GetInt() != 1 ||
        contexts == document.MemberEnd() || !contexts->value.IsObject()) {
        return fail("This is not a supported G-Diffuser gamepad profile.");
    }

    using ProfileRows = std::array<std::array<std::vector<Descriptor>, GDX_INPUT_OUT_COUNT>,
                                   GDX_INPUT_CONTEXT_COUNT>;
    ProfileRows profile;
    for (int context = 0; context < GDX_INPUT_CONTEXT_COUNT; ++context) {
        const auto contextMember = contexts->value.FindMember(kContexts[context]);
        if (contextMember == contexts->value.MemberEnd() || !contextMember->value.IsObject()) {
            return fail("The profile is missing an action context.");
        }
        for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
            const auto rowMember = contextMember->value.FindMember(kOutputs[output]);
            if (rowMember == contextMember->value.MemberEnd() || !rowMember->value.IsArray()) {
                return fail("The profile is missing an action row.");
            }
            std::set<Descriptor> seen;
            for (const auto& item : rowMember->value.GetArray()) {
                if (!item.IsObject()) {
                    return fail("The profile contains an invalid gamepad binding.");
                }
                const auto type = item.FindMember("type");
                const auto code = item.FindMember("code");
                const auto direction = item.FindMember("direction");
                if (type == item.MemberEnd() || !type->value.IsString() || code == item.MemberEnd() ||
                    !code->value.IsInt() || direction == item.MemberEnd() || !direction->value.IsInt()) {
                    return fail("The profile contains an incomplete gamepad binding.");
                }
                const std::string typeName(type->value.GetString(), type->value.GetStringLength());
                const int sourceType = typeName == "button" ? SDLButton : typeName == "axis" ? SDLAxis : 0;
                const Descriptor descriptor{ sourceType, code->value.GetInt(), direction->value.GetInt() };
                if (sourceType == 0 || Create(port, output, descriptor) == nullptr) {
                    return fail("The profile contains an unsupported gamepad button or axis.");
                }
                if (seen.insert(descriptor).second) {
                    if (profile[context][output].size() >= kMaxBindings) {
                        return fail("A profile action row contains too many bindings.");
                    }
                    profile[context][output].push_back(descriptor);
                }
            }
        }
    }

    std::array<std::array<Bindings, GDX_INPUT_OUT_COUNT>, GDX_INPUT_CONTEXT_COUNT> updated;
    std::array<std::array<bool, GDX_INPUT_OUT_COUNT>, GDX_INPUT_CONTEXT_COUNT> changed{};
    bool anyChanged = false;
    for (int context = 0; context < GDX_INPUT_CONTEXT_COUNT; ++context) {
        for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
            for (const auto& binding : Effective(port, context, output)) {
                Descriptor descriptor;
                if (!Describe(binding, descriptor)) {
                    continue;
                }
                if (descriptor.type == SDLButton || descriptor.type == SDLAxis) {
                    changed[context][output] = true;
                } else {
                    updated[context][output].push_back(Create(port, output, descriptor));
                }
            }
            for (const auto& descriptor : profile[context][output]) {
                updated[context][output].push_back(Create(port, output, descriptor));
                changed[context][output] = true;
            }
            if (updated[context][output].size() > kMaxBindings) {
                return fail("The profile would make an action row exceed the binding limit.");
            }
            anyChanged = anyChanged || changed[context][output];
        }
    }

    if (anyChanged) {
        for (int context = 0; context < GDX_INPUT_CONTEXT_COUNT; ++context) {
            for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
                if (changed[context][output]) {
                    Store(port, context, output, updated[context][output], true, false);
                }
            }
        }
        CVarSave();
    }
    return true;
}

void BeginKeyboardCapture() {
    sCapturedKey = -1;
    sKeyboardCapture = true;
}
void EndKeyboardCapture() {
    sKeyboardCapture = false;
    sCapturedKey = -1;
}
int TakeCapturedKey() {
    const int key = sCapturedKey;
    sCapturedKey = -1;
    return key;
}
void ProcessKeyboardEvent(Ship::KbEventType eventType, Ship::KbScancode scancode, bool consumed) {
    // Load all banks BEFORE handling the first event, not just the current context's banks.
    // Rebuilds seed held state from sHeldKeys, and the event below always reaches every loaded mapping.
    if (sBanksDirty) {
        for (int port = 0; port < kPorts; ++port) {
            for (int context = 0; context < GDX_INPUT_CONTEXT_COUNT; ++context) {
                for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
                    Refresh(port, context, output);
                }
            }
        }
        sBanksDirty = false;
    }
    if (eventType == Ship::LUS_KB_EVENT_ALL_KEYS_UP) {
        sHeldKeys.clear();
        sConsumedKeys.clear();
        for (auto& sources : sSources) {
            for (auto& [descriptor, state] : sources) {
                if (descriptor.type == Keyboard) {
                    state = {};
                }
            }
        }
    } else if (eventType == Ship::LUS_KB_EVENT_KEY_DOWN && scancode > Ship::LUS_KB_UNKNOWN &&
               scancode < Ship::LUS_KB_MAX) {
        const bool pressed = sHeldKeys.insert(scancode).second;
        if (consumed) {
            sConsumedKeys.insert(scancode);
        }
        if (pressed && sKeyboardCapture && sCapturedKey == -1) {
            sCapturedKey = scancode;
        }
    } else if (eventType == Ship::LUS_KB_EVENT_KEY_UP) {
        sHeldKeys.erase(scancode);
        sConsumedKeys.erase(scancode);
        // A release followed by another press between polls must still get a new context latch.
        for (auto& sources : sSources) {
            sources[{ Keyboard, scancode, 0 }] = {};
        }
    }
    // Clear mapping state too: a consumed repeat may take ownership of an already-held key.
    const auto mappingEvent = eventType == Ship::LUS_KB_EVENT_KEY_DOWN && sConsumedKeys.count(scancode)
                                  ? Ship::LUS_KB_EVENT_KEY_UP : eventType;
    for (auto& port : sBanks) {
        for (auto& context : port) {
            for (auto& bank : context) {
                for (const auto& bindings : { &bank.bindings, &bank.inheritedBindings }) {
                    for (const auto& binding : *bindings) {
                        if (auto key = std::dynamic_pointer_cast<Ship::KeyboardKeyToButtonMapping>(binding)) {
                            key->ProcessKeyboardEvent(mappingEvent, scancode);
                        }
                    }
                }
            }
        }
    }
}
} // namespace GdxPhysicalInput

extern "C" unsigned short gdx_physical_input_apply(int port, int context, unsigned short legacyButtons) {
    if (port < 0 || port >= kPorts) {
        return legacyButtons;
    }
    if (context < 0 || context >= GDX_INPUT_CONTEXT_COUNT) {
        context = GDX_INPUT_CONTEXT_MENU;
    }
    sPortLive[port] = false;
    auto ctx = Ship::Context::GetInstance();
    auto deck = ctx != nullptr ? ctx->GetControlDeck() : nullptr;
    auto window = ctx != nullptr ? ctx->GetWindow() : nullptr;
    auto& history = sPhysicalHistory[port];
    if (deck == nullptr || window == nullptr) {
        sSources[port].clear();
        history.clear();
        sHistoryDeck[port].reset();
        return legacyButtons;
    }
    if (sHistoryDeck[port].lock() != deck) {
        history.clear();
        sHistoryDeck[port] = deck;
    }
    const bool hasGamepad = SampleSources(port, context, deck, window);
    unsigned short replaced = 0;
    unsigned short physical = 0;
    for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
        const auto bit = gdx_input_actions_output_bit(output);
        for (int bankContext = 0; bankContext < GDX_INPUT_CONTEXT_COUNT; ++bankContext) {
            const auto& bindings = Effective(port, bankContext, output);
            if (sBanks[port][bankContext][output].configured) {
                replaced |= bit;
            }
            for (const auto& binding : bindings) {
                Descriptor descriptor;
                if (!Describe(binding, descriptor)) {
                    continue;
                }
                if (sBanks[port][bankContext][output].configured &&
                    (hasGamepad || (descriptor.type != SDLButton && descriptor.type != SDLAxis))) {
                    sPortLive[port] = true;
                }
                const auto& source = sSources[port][descriptor];
                if (!source.down || source.context != bankContext) {
                    continue;
                }
                if (auto mouse = std::dynamic_pointer_cast<Ship::MouseButtonToButtonMapping>(binding)) {
                    mouse->ProcessMouseButtonEvent(true, static_cast<Ship::MouseBtn>(descriptor.code));
                }
                // Concrete device checks include generic deck blockers as well as SDL filtering.
                // Raw source latches above keep tracking releases even while those checks block output.
                binding->UpdatePad(physical);
            }
        }
    }
    // LUS's deck skips ReadToOSContPad entirely under generic blockers: freeze history and
    // suppress historical physical output, but keep raw release tracking and availability above.
    if (deck->GameInputBlocked()) {
        return static_cast<unsigned short>(legacyButtons & ~replaced);
    }
    // Mirror LUS::Controller::ReadToOSContPad: mapping checks precede history; push on every
    // unblocked poll, even with no configured outputs; select BEFORE trimming to six samples.
    // The size_t conversion also deliberately matches LUS for negative/out-of-range lag values.
    history.push_front(physical);
    const auto lag = static_cast<size_t>(CVarGetInteger(CVAR_SIMULATED_INPUT_LAG, 0));
    const auto delayedPhysical = history[(std::min)(history.size() - 1, lag)];
    while (history.size() > 6) {
        history.pop_back();
    }
    // Replacement ownership is current, not historical. Both configured and recomposed inherited
    // banks use delayed physical bits, while completely inherited outputs keep the legacy pad.
    return static_cast<unsigned short>((legacyButtons & ~replaced) | (delayedPhysical & replaced));
}

extern "C" int gdx_physical_input_has_live_input(int port) {
    return port >= 0 && port < kPorts && sPortLive[port];
}
