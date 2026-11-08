// Exercises production runtime/actions and Ship mappings against deterministic service fixtures.
#include "test_runtime.h"
#include "gdx_physical_input.h"
#include "ship/utils/StringHelper.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/mouse/MouseButtonToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/mouse/MouseWheelToButtonMapping.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
using namespace Ship;
using namespace GdxPhysicalInput;
std::map<std::string, int> values;
int saves = 0, checks = 0, reads = 0, lagReads = 0;
bool allowActionMaskWrite = false;
std::map<std::string, int> readsByKey;
#define CHECK(condition) do { ++checks; if (!(condition)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)
extern "C" int CVarGetInteger(const char* name, int fallback) {
 if (std::string(name) == "gSimulatedInputLag") {
  ++lagReads;
  auto found = values.find(name); return found == values.end() ? fallback : found->second;
 }
 ++reads;
 ++readsByKey[name];
 auto found = values.find(name); return found == values.end() ? fallback : found->second;
}
extern "C" void CVarSetInteger(const char* name, int value) {
 CHECK(std::string(name).find("gEnhancements.Input.Physical.Port") == 0 ||
       (allowActionMaskWrite && std::string(name).find("gEnhancements.Input.Actions.") == 0));
 values[name] = value;
}
extern "C" void CVarSave() { ++saves; }
std::string StringHelper::Sprintf(const char* format, ...) {
 char text[512]; va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args); return text;
}
constexpr int Menu = GDX_INPUT_CONTEXT_MENU, Race = GDX_INPUT_CONTEXT_RACE, Edit = GDX_INPUT_CONTEXT_COURSE_EDIT;
constexpr int A = GDX_INPUT_OUT_A, B = GDX_INPUT_OUT_B, Z = GDX_INPUT_OUT_Z;
constexpr unsigned short Abit = 0x8000, Bbit = 0x4000, Zbit = 0x2000;
auto Key(int code, unsigned short bit = Abit) {
 return std::make_shared<KeyboardKeyToButtonMapping>(0, bit, static_cast<KbScancode>(code));
}
void Down(int code) { ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, static_cast<KbScancode>(code)); }
void Up(int code) { ProcessKeyboardEvent(LUS_KB_EVENT_KEY_UP, static_cast<KbScancode>(code)); }
unsigned short Apply(int context, unsigned short legacy = 0, int port = 0) {
 return gdx_physical_input_apply(port, context, legacy);
}
void AllUp() { ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN); }
void Legacy(int port, unsigned short bit, std::shared_ptr<ControllerButtonMapping> mapping) {
 auto deck = Context::current->deck;
 if (!deck->controllers[port]) deck->controllers[port] = std::make_shared<Controller>();
 auto& button = deck->controllers[port]->buttons[bit];
 if (!button) button = std::make_shared<ControllerButton>();
 button->mappings[mapping->GetButtonMappingId()] = mapping;
}
void Reset(bool warm = true) {
 AllUp();
 EndKeyboardCapture();
 Context::current = std::make_shared<Context>();
 WheelHandler::GetInstance()->directions = {};
 values.clear();
 Invalidate();
 if (warm) {
  for (int port = 0; port < 4; ++port) Apply(Menu, 0, port);
 }
}
void ResetCounters() {
 reads = 0;
 lagReads = 0;
 readsByKey.clear();
 legacyButtonReads = 0;
 legacyMappingReads = 0;
}
void TestCacheAndInvalidation() {
 Reset();
 Legacy(0, Abit, Key(LUS_KB_Q));
 Legacy(0, Bbit, Key(LUS_KB_W, Bbit));
 Invalidate();
 for (int port = 0; port < 4; ++port) Apply(Race, 0, port);
 AllUp(); // Warm the one-time all-bank event initialization too.
 ResetCounters();
 for (int frame = 0; frame < 12; ++frame) {
  Down(LUS_KB_E); Up(LUS_KB_E);
  for (int port = 0; port < 4; ++port) Apply(frame % GDX_INPUT_CONTEXT_COUNT, 0, port);
  GetEffectiveBindings(0, Race, A);
  GetBindings(0, Race, A);
  IsConfigured(0, Race, A);
 }
 CHECK(reads == 0);
 CHECK(lagReads == 12 * 4); // Only the live lag setting is read once per unblocked port poll.
 CHECK(legacyButtonReads == 0);
 CHECK(legacyMappingReads == 0);

 CHECK(AddBinding(0, Race, Z, Key(LUS_KB_Y)));
 auto sibling = GetBindings(0, Race, Z)[0];
 ResetCounters();
 ClearBindings(0, Race, A);
 CHECK(reads == 2); // Only the edited empty row's Configured + Count, not all 168 rows.
 CHECK(readsByKey.size() == 2);
 CHECK(readsByKey.count("gEnhancements.Input.Physical.Port1.Race.A.Configured") == 1);
 CHECK(readsByKey.count("gEnhancements.Input.Physical.Port1.Race.A.Count") == 1);
 CHECK(GetBindings(0, Race, Z)[0] == sibling);
 CHECK(GetBindings(0, Race, A).empty());
 ResetCounters();
 Apply(Race); Down(LUS_KB_E); Up(LUS_KB_E);
 CHECK(reads == 0);
 CHECK(legacyButtonReads == 0);
 CHECK(legacyMappingReads == 0);

 // Invalidation itself neither persists nor reads, and a subsequent event refreshes every bank once.
 const auto previousValues = values;
 const int previousSaves = saves;
 Invalidate();
 CHECK(reads == 0); CHECK(values == previousValues); CHECK(saves == previousSaves);
 Down(LUS_KB_Y);
 CHECK(reads > 0);
 CHECK(GetBindings(0, Race, Z)[0] == sibling); // Unchanged fingerprints retain their objects.
 CHECK((Apply(Race) & Zbit) != 0);
 Invalidate();
 CHECK((Apply(Menu) & Zbit) != 0); // Reload must not relatch held sources into Menu.
 Up(LUS_KB_Y);
 for (int port = 0; port < 4; ++port) Apply(Menu, 0, port);
 ResetCounters();
 AllUp(); Apply(Menu);
 CHECK(reads == 0); CHECK(legacyButtonReads == 0); CHECK(legacyMappingReads == 0);

 // Legacy action-mask setters change the cached mask, without a physical-cache invalidation.
 UseLegacyBindings(0, Race, A);
 CHECK(GetEffectiveBindings(0, Race, A).size() == 1);
 allowActionMaskWrite = true;
 gdx_input_actions_set_mask(Race, A, Bbit);
 allowActionMaskWrite = false;
 ResetCounters();
 auto effective = GetEffectiveBindings(0, Race, A);
 CHECK(effective.size() == 1);
 CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(effective[0])->GetKeyboardScancode() == LUS_KB_W);
 CHECK(reads == 0); // Mask came from the legacy cache, not a new CVar query.
 CHECK(legacyButtonReads == 1);
 ResetCounters(); GetEffectiveBindings(0, Race, A);
 CHECK(legacyButtonReads == 0); CHECK(legacyMappingReads == 0); CHECK(reads == 0);

 // Replacing a legacy physical mapping requires explicit invalidation even when the mask is unchanged.
 auto button = Context::current->deck->controllers[0]->buttons[Bbit];
 button->mappings.clear();
 Legacy(0, Bbit, Key(LUS_KB_R, Bbit));
 CHECK(GetEffectiveBindings(0, Race, A)[0] == effective[0]);
 Invalidate();
 effective = GetEffectiveBindings(0, Race, A);
 CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(effective[0])->GetKeyboardScancode() == LUS_KB_R);
 // Reload-before-release: newly loaded sibling banks must get that release, too.
 values["gEnhancements.Input.Physical.Port4.CourseEdit.A.Configured"] = 1;
 values["gEnhancements.Input.Physical.Port4.CourseEdit.A.Count"] = 1;
 values["gEnhancements.Input.Physical.Port4.CourseEdit.A.0.Type"] = 1;
 values["gEnhancements.Input.Physical.Port4.CourseEdit.A.0.Code"] = LUS_KB_T;
 Down(LUS_KB_T); Apply(Edit, 0, 3);
 Invalidate(); Up(LUS_KB_T);
 CHECK(Apply(Edit, 0, 3) == 0);
 auto reloaded = GetBindings(3, Edit, A)[0];
 unsigned short buttons = 0;
 reloaded->UpdatePad(buttons);
 CHECK(buttons == 0); // Prove delivery, not merely suppression by the source latch.
}
void TestGenericBlockers() {
 for (int source = 0; source < 5; ++source) {
  Reset();
  auto deck = Context::current->deck;
  SDL_GameController pad;
  deck->devices->ports[0][1] = &pad;
  std::vector<std::shared_ptr<ControllerButtonMapping>> mappings = {
   Key(LUS_KB_E),
   std::make_shared<SDLButtonToButtonMapping>(0, Bbit, SDL_CONTROLLER_BUTTON_B),
   std::make_shared<SDLAxisDirectionToButtonMapping>(0, Bbit, SDL_CONTROLLER_AXIS_LEFTX, NEGATIVE),
   std::make_shared<MouseButtonToButtonMapping>(0, Bbit, LUS_MOUSE_BTN_LEFT),
   std::make_shared<MouseWheelToButtonMapping>(0, Bbit, LUS_WHEEL_UP),
  };
  ClearBindings(0, Race, A);
  CHECK(AddBinding(0, Race, A, mappings[source]));
  Down(LUS_KB_E);
  pad.buttons[SDL_CONTROLLER_BUTTON_B] = true;
  pad.axes[SDL_CONTROLLER_AXIS_LEFTX] = -24000;
  Context::current->window->mouse[LUS_MOUSE_BTN_LEFT] = true;
  WheelHandler::GetInstance()->directions.Y = LUS_WHEEL_UP;
  deck->BlockGameInput(100); deck->BlockGameInput(200);
  CHECK(Apply(Race) == 0);
  CHECK(gdx_physical_input_has_live_input(0)); // Blocking is not disconnection.
  deck->UnblockGameInput(100); CHECK(Apply(Menu) == 0);
  deck->UnblockGameInput(200); CHECK(Apply(Menu) == Abit); // Original press context survives blockers.
  deck->BlockGameInput(100);
  AllUp(); pad.buttons.fill(false); pad.axes.fill(0);
  Context::current->window->mouse.fill(false);
  WheelHandler::GetInstance()->directions = {};
  CHECK(Apply(Menu) == 0); // Releases must be sampled while blocked.
  deck->UnblockGameInput(100); CHECK(Apply(Menu) == 0);
 }
}
void TestAvailability() {
 Reset();
 auto deck = Context::current->deck;
 CHECK(AddBinding(2, Race, A, std::make_shared<SDLButtonToButtonMapping>(0, Bbit, SDL_CONTROLLER_BUTTON_A)));
 Apply(Race, 0, 2); CHECK(!gdx_physical_input_has_live_input(2));
 SDL_GameController pad;
 deck->devices->ports[2][1] = &pad;
 Apply(Race, 0, 2); CHECK(gdx_physical_input_has_live_input(2)); // Available even without a button held.
 deck->devices->ports[2].clear();
 Apply(Race, 0, 2); CHECK(!gdx_physical_input_has_live_input(2)); // Device disabled/ignored on this port.
 ClearBindings(2, Race, A);
 CHECK(AddBinding(2, Race, A, Key(LUS_KB_E)));
 Apply(Race, 0, 2); CHECK(gdx_physical_input_has_live_input(2));
 ClearBindings(2, Race, A);
 Apply(Race, 0, 2); CHECK(!gdx_physical_input_has_live_input(2));
}
void TestConsumedKeyboardPresses() {
 Reset();
 for (int port = 0; port < 4; ++port) {
  Legacy(port, Abit, Key(LUS_KB_Q));
  Invalidate();
  ClearBindings(port, Race, A);
  CHECK(AddBinding(port, Race, A, Key(LUS_KB_Q)));
  ClearBindings(port, Menu, B);
  CHECK(AddBinding(port, Menu, B, Key(LUS_KB_Q)));
 }
 auto CheckNeutral = [&]() {
  for (int port = 0; port < 4; ++port) {
   CHECK(Apply(Race, 0, port) == 0);
   CHECK(Apply(Menu, 0, port) == 0);
   for (int context : { Race, Menu }) {
    unsigned short buttons = 0;
    for (int output : { A, B }) {
     for (const auto& binding : GetEffectiveBindings(port, context, output)) binding->UpdatePad(buttons);
    }
    CHECK(buttons == 0); // Mapping state, not just source-latch suppression.
   }
  }
 };
 BeginKeyboardCapture();
 const int previousSaves = saves;
 ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_Q, true);
 CHECK(TakeCapturedKey() == LUS_KB_Q);
 CHECK(saves == previousSaves);
 CheckNeutral();
 EndKeyboardCapture();
 Down(LUS_KB_Q); // Ownership ending or an unconsumed auto-repeat must not resurrect the press.
 CheckNeutral();
 // Force configured and inherited mappings to rebuild while the consumed key is still held.
 for (int port = 0; port < 4; ++port) {
  ClearBindings(port, Race, A);
  CHECK(AddBinding(port, Race, A, Key(LUS_KB_Q)));
  Legacy(port, Abit, Key(LUS_KB_W));
 }
 Invalidate();
 CheckNeutral();
 Up(LUS_KB_Q);
 Down(LUS_KB_Q);
 for (int port = 0; port < 4; ++port) CHECK(Apply(Menu, 0, port) == (Abit | Bbit));
 // A consumed repeat taking ownership clears an already-active mapping too.
 ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_Q, true);
 CheckNeutral();
 AllUp();
 Down(LUS_KB_Q);
 for (int port = 0; port < 4; ++port) CHECK(Apply(Race, 0, port) == Abit);
 // A release/press pair between polls must re-arm with the new context.
 Up(LUS_KB_Q);
 Down(LUS_KB_Q);
 for (int port = 0; port < 4; ++port) CHECK(Apply(Menu, 0, port) == (Abit | Bbit));
 AllUp();
}
void TestLagParity();
void TestReplaceBinding();
int main() {
 Context::current = std::make_shared<Context>();
 auto deck = Context::current->deck;
 // First event must initialize a persisted bank; duplicated key bindings in every context/port all see it.
 values["gEnhancements.Input.Physical.Port1.Race.A.Configured"] = 1;
 values["gEnhancements.Input.Physical.Port1.Race.A.Count"] = 1;
 values["gEnhancements.Input.Physical.Port1.Race.A.0.Type"] = 1;
 values["gEnhancements.Input.Physical.Port1.Race.A.0.Code"] = LUS_KB_K;
 Down(LUS_KB_K);
 CHECK(Apply(Race, Bbit) == (Abit | Bbit));
 CHECK(GetBindings(0, Race, A).size() == 1);
 CHECK(Apply(Menu) == Abit); // held Race action remains Race action
 CHECK(AddBinding(0, Menu, B, Key(LUS_KB_K)));
 CHECK(Apply(Menu) == Abit); // no new Menu action on a held source
 Up(LUS_KB_K); CHECK(Apply(Menu) == 0);
 Down(LUS_KB_K); CHECK(Apply(Menu) == Bbit);
 CHECK(Apply(Race) == Bbit);
 AllUp(); CHECK(Apply(Race) == 0);
 ClearBindings(0, Race, A); CHECK(IsConfigured(0, Race, A));
 CHECK(GetBindings(0, Race, A).empty()); CHECK(Apply(Race, Abit | Zbit) == Zbit);
 UseLegacyBindings(0, Race, A); CHECK(!IsConfigured(0, Race, A)); CHECK(Apply(Race, Abit) == Abit);
 UseLegacyBindings(0, Menu, B);
 // Inheritance composes old action masks including X/Y; clones target the new output bit.
 Legacy(0, Bbit, Key(LUS_KB_Q, Bbit));
 Legacy(0, Bbit, std::make_shared<SDLButtonToButtonMapping>(0, Bbit, SDL_CONTROLLER_BUTTON_B));
 Legacy(0, GDX_INPUT_BTN_X, std::make_shared<SDLAxisDirectionToButtonMapping>(0, GDX_INPUT_BTN_X, SDL_CONTROLLER_AXIS_LEFTX, NEGATIVE));
 Legacy(0, GDX_INPUT_BTN_X, std::make_shared<MouseButtonToButtonMapping>(0, GDX_INPUT_BTN_X, LUS_MOUSE_BTN_RIGHT));
 Legacy(0, GDX_INPUT_BTN_X, std::make_shared<MouseWheelToButtonMapping>(0, GDX_INPUT_BTN_X, LUS_WHEEL_DOWN));
 values["gEnhancements.Input.Actions.Race.A"] = Bbit | GDX_INPUT_BTN_X;
 Invalidate(); // External legacy mapping edits and action masks share the explicit refresh seam.
 int before = saves;
 auto effective = GetEffectiveBindings(0, Race, A);
 CHECK(effective.size() == 5); CHECK(saves == before); CHECK(!IsConfigured(0, Race, A));
 for (const auto& mapping : effective) CHECK(mapping->GetBitmask() == Abit);
 CHECK(AddBinding(0, Race, A, Key(LUS_KB_W)));
 CHECK(GetBindings(0, Race, A).size() == 6);
 CHECK(values["gEnhancements.Input.Actions.Race.A"] == (Bbit | GDX_INPUT_BTN_X));
 CHECK(RemoveBinding(0, Race, A, 0)); CHECK(GetBindings(0, Race, A).size() == 5);
 CHECK(!RemoveBinding(0, Race, A, -1)); CHECK(!RemoveBinding(0, Race, A, 500));
 UseLegacyBindings(0, Race, A);
 CHECK(RemoveBinding(0, Race, A, 0)); CHECK(GetBindings(0, Race, A).size() == 4);
 CHECK(GetEffectiveBindings(0, Menu, B).size() == 2); // untouched sibling row
 // All contexts and all ports receive releases, with a shared physical descriptor latch per port.
 for (int port = 0; port < 4; ++port) {
  ClearBindings(port, Race, A); ClearBindings(port, Menu, B);
  CHECK(AddBinding(port, Race, A, Key(LUS_KB_E)));
  CHECK(AddBinding(port, Menu, B, Key(LUS_KB_E)));
 }
 Down(LUS_KB_E);
 for (int port = 0; port < 4; ++port) {
  CHECK((Apply(Race, 0, port) & Abit) != 0);
  for (int context : { Race, Menu }) {
   unsigned short buttons = 0;
   GetBindings(port, context, context == Race ? A : B)[0]->UpdatePad(buttons);
   CHECK(buttons == (context == Race ? Abit : Bbit)); // Every mapping got key-down, regardless of latch context.
  }
 }
 for (int port = 0; port < 4; ++port) CHECK(Apply(Menu, 0, port) == Abit);
 Up(LUS_KB_E);
 for (int port = 0; port < 4; ++port) {
  CHECK(Apply(Menu, 0, port) == 0);
  for (int context : { Race, Menu }) {
   unsigned short buttons = 0;
   GetBindings(port, context, context == Race ? A : B)[0]->UpdatePad(buttons);
   CHECK(buttons == 0); // Do not let source-latch suppression hide a missed mapping release.
  }
 }
 // Release + repress between polls is a fresh context press, not a stale latch.
 Down(LUS_KB_E); CHECK(Apply(Race) == Abit); Up(LUS_KB_E); Down(LUS_KB_E); CHECK(Apply(Menu) == Bbit);
 AllUp(); CHECK(Apply(Menu) == 0);
 // A blocker suppresses output, not physical release tracking or press-context ownership.
 deck->keyboardBlocked = true; Down(LUS_KB_E); CHECK(Apply(Race) == 0);
 CHECK(Apply(Menu) == 0); deck->keyboardBlocked = false; CHECK(Apply(Menu) == Abit);
 AllUp(); CHECK(Apply(Menu) == 0);
 // Same-count external descriptor reload, plus held state seeding on a freshly reconstructed mapping.
 const std::string entry = "gEnhancements.Input.Physical.Port1.Race.A.0.Code";
 Down(LUS_KB_T); CHECK(Apply(Race) == 0);
 values[entry] = LUS_KB_T;
 CHECK(Apply(Menu) == 0); // External edits are invisible until the explicit reload notification.
 Invalidate();
 CHECK(Apply(Menu) == Abit);
 CHECK(std::dynamic_pointer_cast<KeyboardKeyToButtonMapping>(GetBindings(0, Race, A)[0])->GetKeyboardScancode() == LUS_KB_T);
 Up(LUS_KB_T); CHECK(Apply(Menu) == 0);
 // Configured row only in Menu must still allow inherited Race sources to latch and then release.
 UseLegacyBindings(0, Race, A); ClearBindings(0, Menu, A);
 Down(LUS_KB_Q); CHECK((Apply(Race) & Abit) != 0); CHECK((Apply(Menu) & Abit) != 0);
 Up(LUS_KB_Q); CHECK((Apply(Menu, Abit) & Abit) == 0);
 Down(LUS_KB_Q); CHECK((Apply(Menu) & Abit) == 0); AllUp();
 // SDL factory capture returns an unsaved object and respects port filtering.
 SDL_GameController pad;
 deck->devices->ports[0][1] = &pad;
 pad.buttons[SDL_CONTROLLER_BUTTON_X] = true;
 before = saves; auto captured = CaptureSDL(0, Z); CHECK(captured != nullptr); CHECK(saves == before);
 CHECK(captured->GetBitmask() == Zbit); CHECK(CaptureSDL(1, Z) == nullptr);
 ClearBindings(0, Race, Z); CHECK(AddBinding(0, Race, Z, captured));
 CHECK((Apply(Race, Zbit) & Zbit) != 0); CHECK(gdx_physical_input_has_live_input(0));
 deck->gamepadBlocked = true; CHECK((Apply(Menu) & Zbit) == 0);
 deck->gamepadBlocked = false; CHECK((Apply(Menu) & Zbit) != 0);
 pad.buttons[SDL_CONTROLLER_BUTTON_X] = false; CHECK((Apply(Menu) & Zbit) == 0);
 pad.buttons[SDL_CONTROLLER_BUTTON_X] = true; CHECK((Apply(Menu) & Zbit) == 0); // Menu press cannot activate Race Z
 pad.attached = false; CHECK((Apply(Race) & Zbit) == 0);
 deck->devices->ports[0].clear(); CHECK((Apply(Race) & Zbit) == 0);
 // SDL axes use direction and the existing per-device thresholds.
 pad.attached = true; pad.buttons.fill(false); deck->devices->ports[0][1] = &pad;
 ClearBindings(0, Race, Z);
 CHECK(AddBinding(0, Race, Z, std::make_shared<SDLAxisDirectionToButtonMapping>(0, Abit, SDL_CONTROLLER_AXIS_LEFTX, NEGATIVE)));
 pad.axes[SDL_CONTROLLER_AXIS_LEFTX] = -10000; CHECK((Apply(Race) & Zbit) != 0);
 pad.axes[SDL_CONTROLLER_AXIS_LEFTX] = 10000; CHECK((Apply(Race) & Zbit) == 0);
 deck->settings->stick = 80; pad.axes[SDL_CONTROLLER_AXIS_LEFTX] = -10000; CHECK((Apply(Race) & Zbit) == 0);
 pad.axes.fill(0); Apply(Race);
 // Mouse button/wheel descriptors are cloneable and honor mouse input blockers.
 ClearBindings(0, Race, Z);
 CHECK(AddBinding(0, Race, Z, std::make_shared<MouseButtonToButtonMapping>(0, Abit, LUS_MOUSE_BTN_LEFT)));
 CHECK(AddBinding(0, Race, Z, std::make_shared<MouseWheelToButtonMapping>(0, Abit, LUS_WHEEL_UP)));
 Context::current->window->mouse[LUS_MOUSE_BTN_LEFT] = true; CHECK((Apply(Race) & Zbit) != 0);
 CHECK((Apply(Menu) & Zbit) != 0); deck->mouseBlocked = true; CHECK((Apply(Menu) & Zbit) == 0);
 deck->mouseBlocked = false; Context::current->window->mouse[LUS_MOUSE_BTN_LEFT] = false; CHECK((Apply(Menu) & Zbit) == 0);
 WheelHandler::GetInstance()->directions.Y = LUS_WHEEL_UP; CHECK((Apply(Race) & Zbit) != 0);
 WheelHandler::GetInstance()->directions.Y = LUS_WHEEL_NONE; CHECK((Apply(Menu) & Zbit) == 0);
 // Keyboard capture has no config or blocker side effects, ignores repeats, and drains one key.
 before = saves; Down(LUS_KB_R); BeginKeyboardCapture(); Down(LUS_KB_R); CHECK(TakeCapturedKey() == -1);
 Down(LUS_KB_Y); CHECK(TakeCapturedKey() == LUS_KB_Y); CHECK(TakeCapturedKey() == -1);
 EndKeyboardCapture(); Down(LUS_KB_U); CHECK(TakeCapturedKey() == -1); CHECK(saves == before);
 AllUp(); Apply(Menu);
 CHECK(!AddBinding(-1, Race, A, Key(LUS_KB_E))); CHECK(!AddBinding(0, Race, A, nullptr));
 CHECK(!IsConfigured(4, Race, A)); CHECK(GetEffectiveBindings(0, -1, A).empty());
 CHECK(Apply(Menu, Abit, -1) == Abit); CHECK(CaptureSDL(0, 99) == nullptr);
 // Real legacy apply remains default for completely inherited outputs.
 gdx_input_actions_begin_frame(Race); CHECK(gdx_input_actions_apply(3, Bbit) == (Abit | Bbit));
 CHECK((Apply(Race, Zbit, 3) & Zbit) != 0);
 TestCacheAndInvalidation();
 TestGenericBlockers();
 TestAvailability();
 TestLagParity();
 TestReplaceBinding();
 TestConsumedKeyboardPresses();
 std::printf("PASS: %d checks; production physical/action modules + Ship mappings/factory, stubbed services\n", checks);
}
