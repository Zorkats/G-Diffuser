// Atomic per-row replacement, inheritance snapshots, descriptor cloning/deduplication and rejection.
#include "test_runtime.h"
#include "gdx_physical_input.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/mouse/MouseButtonToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/mouse/MouseWheelToButtonMapping.h"

#include <cstdio>
#include <cstdlib>

extern std::map<std::string, int> values;
extern int saves, checks;
extern bool allowActionMaskWrite;
void Reset(bool warm);
void Down(int code);
void Up(int code);
unsigned short Apply(int context, unsigned short legacy, int port);
void Legacy(int port, unsigned short bit, std::shared_ptr<Ship::ControllerButtonMapping> mapping);

namespace {
using namespace Ship;
using namespace GdxPhysicalInput;
using Binding = std::shared_ptr<ControllerButtonMapping>;
constexpr int Port = 2, Race = GDX_INPUT_CONTEXT_RACE, Menu = GDX_INPUT_CONTEXT_MENU;
constexpr int A = GDX_INPUT_OUT_A, Z = GDX_INPUT_OUT_Z;
constexpr unsigned short Abit = 0x8000, Bbit = 0x4000, Zbit = 0x2000;
const std::string Prefix = "gEnhancements.Input.Physical.Port3.Race.A";
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #condition); std::exit(1); \
} } while (0)

auto Key(int code, unsigned short bit = Zbit) {
    return std::make_shared<KeyboardKeyToButtonMapping>(0, bit, static_cast<KbScancode>(code));
}
class UnsupportedMapping final : public ControllerButtonMapping {
  public:
    UnsupportedMapping()
        : ControllerInputMapping(PhysicalDeviceType::Keyboard),
          ControllerButtonMapping(PhysicalDeviceType::Keyboard, 0, Zbit) {}
    std::string GetButtonMappingId() override { return "unsupported"; }
    void UpdatePad(unsigned short&) override {}
    void SaveToConfig() override { std::abort(); }
    void EraseFromConfig() override { std::abort(); }
};
void SetupInheritedRow() {
    Reset(true);
    Legacy(Port, Bbit, Key(LUS_KB_Q, Bbit));
    Legacy(Port, Bbit, std::make_shared<SDLButtonToButtonMapping>(0, Bbit, SDL_CONTROLLER_BUTTON_B));
    Legacy(Port, Bbit, std::make_shared<MouseButtonToButtonMapping>(0, Bbit, LUS_MOUSE_BTN_RIGHT));
    Invalidate();
    allowActionMaskWrite = true;
    gdx_input_actions_set_mask(Race, A, Bbit);
    allowActionMaskWrite = false;
    CHECK(GetEffectiveBindings(Port, Race, A).size() == 3);
    CHECK(!IsConfigured(Port, Race, A));
}
void RejectWithoutChanges(int port, int context, int output, int index, Binding binding) {
    const auto before = values;
    const int previousSaves = saves;
    CHECK(!ReplaceBinding(port, context, output, index, binding));
    CHECK(values == before);
    CHECK(saves == previousSaves);
}
void TestRejectedReplacement() {
    SetupInheritedRow();
    auto valid = Key(LUS_KB_E);
    RejectWithoutChanges(-1, Race, A, 0, valid);
    RejectWithoutChanges(4, Race, A, 0, valid);
    RejectWithoutChanges(Port, -1, A, 0, valid);
    RejectWithoutChanges(Port, GDX_INPUT_CONTEXT_COUNT, A, 0, valid);
    RejectWithoutChanges(Port, Race, -1, 0, valid);
    RejectWithoutChanges(Port, Race, GDX_INPUT_OUT_COUNT, 0, valid);
    RejectWithoutChanges(Port, Race, A, -1, valid);
    RejectWithoutChanges(Port, Race, A, 3, valid);
    RejectWithoutChanges(Port, Race, A, 1000000, valid);
    RejectWithoutChanges(Port, Race, A, 0, nullptr);
    RejectWithoutChanges(Port, Race, A, 0, std::make_shared<UnsupportedMapping>());
    RejectWithoutChanges(Port, Race, A, 0, Key(LUS_KB_UNKNOWN));
    RejectWithoutChanges(Port, Race, A, 0, Key(LUS_KB_MAX));
    RejectWithoutChanges(Port, Race, A, 0, std::make_shared<SDLButtonToButtonMapping>(0, Zbit, -1));
    RejectWithoutChanges(Port, Race, A, 0,
                         std::make_shared<SDLAxisDirectionToButtonMapping>(0, Zbit, SDL_CONTROLLER_AXIS_LEFTX, 0));
    RejectWithoutChanges(Port, Race, A, 0,
                         std::make_shared<MouseButtonToButtonMapping>(0, Zbit, LUS_MOUSE_BTN_UNKNOWN));
    RejectWithoutChanges(Port, Race, A, 0,
                         std::make_shared<MouseWheelToButtonMapping>(0, Zbit, LUS_WHEEL_NONE));
    CHECK(!IsConfigured(Port, Race, A));
    CHECK(GetEffectiveBindings(Port, Race, A).size() == 3);
    ClearBindings(Port, Race, A);
    RejectWithoutChanges(Port, Race, A, 0, valid);
    CHECK(IsConfigured(Port, Race, A));
    CHECK(GetBindings(Port, Race, A).empty());
}
void TestSnapshotAndCloning() {
    SetupInheritedRow();
    CHECK(AddBinding(Port, Menu, Z, Key(LUS_KB_T)));
    const auto sibling = GetBindings(Port, Menu, Z)[0];
    const auto legacy = Context::current->deck->controllers[Port]->buttons[Bbit]->mappings;
    const int previousSaves = saves;
    auto incoming = Key(LUS_KB_E); // Deliberately captured for a different output and port.
    CHECK(ReplaceBinding(Port, Race, A, 1, incoming));
    CHECK(saves == previousSaves + 1);
    CHECK(incoming->GetBitmask() == Zbit);
    CHECK(IsConfigured(Port, Race, A));
    auto row = GetBindings(Port, Race, A);
    CHECK(row.size() == 3);
    CHECK(row[1] != incoming);
    for (const auto& binding : row) CHECK(binding->GetBitmask() == Abit);
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(row[0])->GetKeyboardScancode() == LUS_KB_Q);
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(row[1])->GetKeyboardScancode() == LUS_KB_E);
    CHECK(std::dynamic_pointer_cast<MouseButtonToButtonMapping>(row[2])->GetMouseButton() == LUS_MOUSE_BTN_RIGHT);
    CHECK(GetBindings(Port, Menu, Z)[0] == sibling);
    CHECK(Context::current->deck->controllers[Port]->buttons[Bbit]->mappings == legacy);
    CHECK(values["gEnhancements.Input.Actions.Race.A"] == Bbit);
    CHECK(values.at(Prefix + ".Count") == 3);
    CHECK(values.at(Prefix + ".1.Type") == 1);
    CHECK(values.at(Prefix + ".1.Code") == LUS_KB_E);
    CHECK(values.at(Prefix + ".1.Direction") == 0);

    // Every concrete descriptor type can replace the same slot without dropping its neighbors.
    std::vector<Binding> replacements = {
        std::make_shared<SDLButtonToButtonMapping>(0, Zbit, SDL_CONTROLLER_BUTTON_A),
        std::make_shared<SDLAxisDirectionToButtonMapping>(0, Zbit, SDL_CONTROLLER_AXIS_LEFTX, NEGATIVE),
        std::make_shared<MouseButtonToButtonMapping>(0, Zbit, LUS_MOUSE_BTN_LEFT),
        std::make_shared<MouseWheelToButtonMapping>(0, Zbit, LUS_WHEEL_UP),
        Key(LUS_KB_E),
    };
    const int types[] = { 2, 3, 4, 5, 1 };
    const int codes[] = { SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_AXIS_LEFTX, LUS_MOUSE_BTN_LEFT,
                          LUS_WHEEL_UP, LUS_KB_E };
    SDL_GameController pad;
    pad.buttons[SDL_CONTROLLER_BUTTON_A] = true;
    Context::current->deck->devices->ports[0][1] = &pad;
    for (size_t i = 0; i < replacements.size(); ++i) {
        const int before = saves;
        CHECK(ReplaceBinding(Port, Race, A, 1, replacements[i]));
        CHECK(saves == before + 1);
        CHECK(GetBindings(Port, Race, A).size() == 3);
        CHECK(GetBindings(Port, Race, A)[1]->GetBitmask() == Abit);
        CHECK(replacements[i]->GetBitmask() == Zbit);
        CHECK(values.at(Prefix + ".1.Type") == types[i]);
        CHECK(values.at(Prefix + ".1.Code") == codes[i]);
        CHECK(values.at(Prefix + ".1.Direction") == (types[i] == 3 ? NEGATIVE : 0));
        if (i == 0) {
            CHECK(Apply(Race, 0, Port) == 0); // Do not accidentally read the capture object's port 0.
            Context::current->deck->devices->ports[Port][1] = &pad;
            CHECK(Apply(Race, 0, Port) == Abit);
        }
    }
    const auto before = values;
    const int beforeSaves = saves;
    Invalidate();
    CHECK(GetBindings(Port, Race, A).size() == 3);
    CHECK(values == before);
    CHECK(saves == beforeSaves);
    CHECK(GetBindings(Port, Menu, Z)[0] == sibling);
}
void TestDeduplication() {
    SetupInheritedRow();
    CHECK(ReplaceBinding(Port, Race, A, 1, Key(LUS_KB_Q)));
    auto row = GetBindings(Port, Race, A);
    CHECK(IsConfigured(Port, Race, A));
    CHECK(row.size() == 2); // First replacement snapshots inheritance, then removes the duplicate.
    CHECK(values.at(Prefix + ".Count") == 2);
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(row[0])->GetKeyboardScancode() == LUS_KB_Q);
    CHECK(std::dynamic_pointer_cast<MouseButtonToButtonMapping>(row[1])->GetMouseButton() == LUS_MOUSE_BTN_RIGHT);
    CHECK(ReplaceBinding(Port, Race, A, 0, Key(LUS_KB_Q))); // Same descriptor is a valid edit.
    CHECK(GetBindings(Port, Race, A).size() == 2);
    CHECK(ReplaceBinding(Port, Race, A, 0,
                         std::make_shared<MouseButtonToButtonMapping>(0, Zbit, LUS_MOUSE_BTN_RIGHT)));
    CHECK(GetBindings(Port, Race, A).size() == 1); // Duplicate after the replacement slot, too.

    // External banks may already contain duplicates. Collapse all identical descriptors, stably.
    values[Prefix + ".Count"] = 3;
    for (int i = 0; i < 3; ++i) {
        const auto entry = Prefix + "." + std::to_string(i);
        values[entry + ".Type"] = 1;
        values[entry + ".Code"] = i == 2 ? LUS_KB_Q : LUS_KB_E;
        values[entry + ".Direction"] = 0;
    }
    Invalidate();
    CHECK(GetBindings(Port, Race, A).size() == 3);
    CHECK(ReplaceBinding(Port, Race, A, 2, Key(LUS_KB_W)));
    row = GetBindings(Port, Race, A);
    CHECK(row.size() == 2);
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(row[0])->GetKeyboardScancode() == LUS_KB_E);
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(row[1])->GetKeyboardScancode() == LUS_KB_W);

    Reset(true);
    CHECK(AddBinding(0, Race, A, Key(LUS_KB_ESCAPE)));
    CHECK(AddBinding(0, Race, A, std::make_shared<SDLButtonToButtonMapping>(0, Zbit, SDL_CONTROLLER_BUTTON_B)));
    CHECK(ReplaceBinding(0, Race, A, 0, Key(LUS_KB_ESCAPE)));
    CHECK(GetBindings(0, Race, A).size() == 2); // Same Code but different Type must not deduplicate.
    ClearBindings(0, Race, A);
    CHECK(AddBinding(0, Race, A,
                     std::make_shared<SDLAxisDirectionToButtonMapping>(0, Zbit, SDL_CONTROLLER_AXIS_LEFTX, NEGATIVE)));
    CHECK(AddBinding(0, Race, A,
                     std::make_shared<SDLAxisDirectionToButtonMapping>(0, Zbit, SDL_CONTROLLER_AXIS_LEFTX, POSITIVE)));
    CHECK(ReplaceBinding(0, Race, A, 0,
                         std::make_shared<SDLAxisDirectionToButtonMapping>(0, Zbit, SDL_CONTROLLER_AXIS_LEFTX, NEGATIVE)));
    CHECK(GetBindings(0, Race, A).size() == 2); // Direction is part of descriptor identity.
    CHECK(ReplaceBinding(0, Race, A, 0,
                         std::make_shared<SDLAxisDirectionToButtonMapping>(0, Zbit, SDL_CONTROLLER_AXIS_LEFTX, POSITIVE)));
    CHECK(GetBindings(0, Race, A).size() == 1);
}
void TestGamepadProfiles() {
    Reset(false);
    CHECK(AddBinding(0, Race, A, Key(LUS_KB_Q)));
    CHECK(AddBinding(0, Race, A, std::make_shared<SDLButtonToButtonMapping>(0, Abit, SDL_CONTROLLER_BUTTON_X)));
    CHECK(AddBinding(0, Menu, Z,
                     std::make_shared<SDLAxisDirectionToButtonMapping>(0, Zbit, SDL_CONTROLLER_AXIS_LEFTY, POSITIVE)));

    std::string error;
    const std::string profile = ExportGamepadProfile(0, "Test Pad", &error);
    CHECK(!profile.empty());
    CHECK(error.empty());
    CHECK(profile.find("gdiffuser.gamepad-profile") != std::string::npos);
    CHECK(profile.find("\"name\":\"Test Pad\"") != std::string::npos);

    CHECK(AddBinding(1, Race, A, Key(LUS_KB_E)));
    CHECK(AddBinding(1, Race, A, std::make_shared<SDLButtonToButtonMapping>(0, Abit, SDL_CONTROLLER_BUTTON_B)));
    CHECK(AddBinding(1, Menu, Z, std::make_shared<MouseButtonToButtonMapping>(0, Zbit, LUS_MOUSE_BTN_RIGHT)));
    Legacy(1, Bbit, Key(LUS_KB_W, Bbit));
    Invalidate();
    const int beforeSaves = saves;
    CHECK(ImportGamepadProfile(1, profile, &error));
    CHECK(error.empty());
    CHECK(saves == beforeSaves + 1); // Whole-profile import persists once.

    auto raceA = GetBindings(1, Race, A);
    CHECK(raceA.size() == 2);
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(raceA[0])->GetKeyboardScancode() == LUS_KB_E);
    CHECK(std::dynamic_pointer_cast<SDLButtonToButtonMapping>(raceA[1])->GetSDLControllerButton() ==
          SDL_CONTROLLER_BUTTON_X);
    CHECK(!IsConfigured(1, Race, GDX_INPUT_OUT_B)); // Unrelated inherited rows stay inherited.
    auto inheritedB = GetEffectiveBindings(1, Race, GDX_INPUT_OUT_B);
    CHECK(inheritedB.size() == 1);
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(inheritedB[0])->GetKeyboardScancode() == LUS_KB_W);
    auto menuZ = GetBindings(1, Menu, Z);
    CHECK(menuZ.size() == 2);
    CHECK(std::dynamic_pointer_cast<MouseButtonToButtonMapping>(menuZ[0])->GetMouseButton() == LUS_MOUSE_BTN_RIGHT);
    auto axis = std::dynamic_pointer_cast<SDLAxisDirectionToButtonMapping>(menuZ[1]);
    CHECK(axis != nullptr);
    CHECK(axis->GetSDLControllerAxis() == SDL_CONTROLLER_AXIS_LEFTY);
    CHECK(axis->GetAxisDirection() == POSITIVE);

    const auto beforeInvalid = values;
    const int savesBeforeInvalid = saves;
    CHECK(!ImportGamepadProfile(1, "{ invalid json", &error));
    CHECK(!error.empty());
    CHECK(values == beforeInvalid);
    CHECK(saves == savesBeforeInvalid);
    CHECK(!ImportGamepadProfile(1, profile + std::string(1024 * 1024, ' '), &error));
    CHECK(values == beforeInvalid);
    CHECK(saves == savesBeforeInvalid);
}

void TestDefaultGamepadProfileInitialization() {
    Reset(false);
    Legacy(2, Abit, Key(LUS_KB_Q, Abit));
    Invalidate();
    const int savesBeforeInitialization = saves;
    for (int port = 0; port < 4; ++port) {
        CHECK(InitializeDefaultGamepadProfile(port));
        CHECK(values["gEnhancements.Input.Physical.Port" + std::to_string(port + 1) +
                     ".DefaultProfileVersion"] == 1);
        CHECK(IsConfigured(port, Race, A));
        auto raceA = GetBindings(port, Race, A);
        CHECK(raceA.size() == (port == 2 ? 2u : 1u));
        CHECK(std::dynamic_pointer_cast<SDLButtonToButtonMapping>(raceA.back())->GetSDLControllerButton() ==
              SDL_CONTROLLER_BUTTON_A);
    }
    auto preservedKey = std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(GetBindings(2, Race, A).front());
    CHECK(preservedKey != nullptr && preservedKey->GetKeyboardScancode() == LUS_KB_Q);
    auto raceB = GetBindings(0, Race, GDX_INPUT_OUT_B);
    CHECK(std::dynamic_pointer_cast<SDLButtonToButtonMapping>(raceB[0])->GetSDLControllerButton() ==
          SDL_CONTROLLER_BUTTON_X);
    auto raceCDown = GetBindings(0, Race, GDX_INPUT_OUT_CDOWN);
    CHECK(raceCDown.size() == 2);
    auto cDownAxis = std::dynamic_pointer_cast<SDLAxisDirectionToButtonMapping>(raceCDown[0]);
    CHECK(cDownAxis != nullptr && cDownAxis->GetSDLControllerAxis() == SDL_CONTROLLER_AXIS_TRIGGERRIGHT &&
          cDownAxis->GetAxisDirection() == POSITIVE);
    CHECK(std::dynamic_pointer_cast<SDLButtonToButtonMapping>(raceCDown[1])->GetSDLControllerButton() ==
          SDL_CONTROLLER_BUTTON_B);
    CHECK(saves == savesBeforeInitialization + 4);

    CHECK(ReplaceBinding(0, Race, A, 0,
                         std::make_shared<SDLButtonToButtonMapping>(0, Abit, SDL_CONTROLLER_BUTTON_Y)));
    Invalidate();
    CHECK(InitializeDefaultGamepadProfile(0));
    CHECK(std::dynamic_pointer_cast<SDLButtonToButtonMapping>(GetBindings(0, Race, A)[0])->
              GetSDLControllerButton() == SDL_CONTROLLER_BUTTON_Y);
    CHECK(!InitializeDefaultGamepadProfile(-1));

    Reset(false);
    CHECK(AddBinding(1, Race, A, Key(LUS_KB_E)));
    const auto savedRaceA = GetBindings(1, Race, A);
    CHECK(InitializeDefaultGamepadProfile(1));
    CHECK(GetBindings(1, Race, A).size() == savedRaceA.size());
    CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(GetBindings(1, Race, A)[0])->
              GetKeyboardScancode() == LUS_KB_E);
    CHECK(!IsConfigured(1, Race, GDX_INPUT_OUT_B));
}

void TestReplacementStateAndCapacity() {
    Reset(false);
    CHECK(AddBinding(0, Race, A, Key(LUS_KB_Q)));
    CHECK(AddBinding(0, Menu, GDX_INPUT_OUT_B, Key(LUS_KB_E)));
    Down(LUS_KB_E);
    CHECK(Apply(Race, 0, 0) == 0);
    CHECK(ReplaceBinding(0, Race, A, 0, Key(LUS_KB_E)));
    CHECK(Apply(Menu, 0, 0) == Abit); // The held source must retain Race ownership.
    Up(LUS_KB_E); CHECK(Apply(Menu, 0, 0) == 0);
    Down(LUS_KB_E); CHECK(Apply(Menu, 0, 0) == Bbit);

    Reset(false);
    values["gSimulatedInputLag"] = 1;
    CHECK(AddBinding(0, Race, A, Key(LUS_KB_E)));
    CHECK(Apply(Race, 0, 0) == 0);
    Down(LUS_KB_E); CHECK(Apply(Race, 0, 0) == 0);
    RejectWithoutChanges(0, Race, A, 99, Key(LUS_KB_W));
    CHECK(ReplaceBinding(0, Race, A, 0, Key(LUS_KB_W)));
    Up(LUS_KB_E); CHECK(Apply(Race, 0, 0) == Abit); // Atomic replacement must not clear pending history.
    CHECK(Apply(Race, 0, 0) == 0);
    Down(LUS_KB_W); CHECK(Apply(Race, 0, 0) == 0);
    CHECK(Apply(Race, 0, 0) == Abit);

    Reset(true);
    for (int key = 1; key <= 257; ++key) Legacy(0, Abit, Key(key, Abit));
    Invalidate();
    CHECK(GetEffectiveBindings(0, Race, A).size() == 257);
    RejectWithoutChanges(0, Race, A, 0, Key(LUS_KB_ARROWKEY_DOWN));
    CHECK(!IsConfigured(0, Race, A));
    // A duplicate replacement reduces the final snapshot to the supported limit without losing others.
    CHECK(ReplaceBinding(0, Race, A, 0, Key(LUS_KB_E)));
    CHECK(GetBindings(0, Race, A).size() == 256);
}
} // namespace

void TestReplaceBinding() {
    TestRejectedReplacement();
    TestSnapshotAndCloning();
    TestDeduplication();
    TestGamepadProfiles();
    TestReplacementStateAndCapacity();
}
