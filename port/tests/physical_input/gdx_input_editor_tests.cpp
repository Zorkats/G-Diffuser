// Real production editor + ImGui core; no renderer, backend or private-state access.
#include "test_runtime.h"
#include "gdx_input_editor.h"
#include "gdx_physical_input.h"
#include "ship/utils/StringHelper.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include <imgui_internal.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>

namespace {
std::map<std::string, int> values;
std::map<std::string, std::string> stringValues;
int saves = 0, writes = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)
struct Item { ImGuiID id; ImRect rect; std::string label; };
std::vector<Item> items;
std::map<ImGuiID, ImRect> rectangles;
constexpr int Race = GDX_INPUT_CONTEXT_RACE, Menu = GDX_INPUT_CONTEXT_MENU;
constexpr int Edit = GDX_INPUT_CONTEXT_COURSE_EDIT, A = GDX_INPUT_OUT_A;
constexpr const char* Popup = "Bind action##PhysicalInputCapture";
class Editor : public GdxInputEditor {
  public:
    using GdxInputEditor::DrawElement;
    using GdxInputEditor::UpdateElement;
};
}

// ImGui's optional hooks supply widget coordinates, not synthetic widget return values.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) {
    rectangles.insert_or_assign(id, rect);
    // ImGui 1.91 omits ItemInfo for BeginCombo and tab labels. Match their real IDs.
    if (ctx->CurrentWindow && ctx->CurrentWindow->IDStack.Size > 0 &&
            id == ctx->CurrentWindow->GetID("Player"))
        items.push_back({id, rect, "Player"});
    if (ctx->CurrentTabBar) {
        for (const char* label : {"Race", "Menu", "Course Edit"}) {
            if (id == ImHashStr(label, 0, ctx->CurrentTabBar->ID)) items.push_back({id, rect, label});
        }
    }
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    auto found = rectangles.find(id);
    if (found != rectangles.end()) {
        std::string normalized = label;
        if (normalized.find("###Add binding") != std::string::npos) normalized = "+###Add binding";
        if (normalized.find(" Key") != std::string::npos && normalized.find("##") == std::string::npos) {
            normalized = "Key##Binding";
        }
        items.push_back({id, found->second, std::move(normalized)});
    }
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

extern "C" int CVarGetInteger(const char* name, int fallback) {
    auto found = values.find(name);
    return found == values.end() ? fallback : found->second;
}
extern "C" void CVarSetInteger(const char* name, int value) {
    CHECK(std::string(name).find("gEnhancements.Input.Physical.Port") == 0);
    ++writes;
    values[name] = value;
}
extern "C" const char* CVarGetString(const char* name, const char* fallback) {
    auto found = stringValues.find(name);
    return found == stringValues.end() ? fallback : found->second.c_str();
}
extern "C" void CVarSetString(const char* name, const char* value) {
    CHECK(std::string(name).find("gEnhancements.Input.Physical.Port") == 0);
    ++writes;
    stringValues[name] = value;
}
extern "C" void CVarSave() { ++saves; }
std::string StringHelper::Sprintf(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

namespace {
struct Harness {
    std::unique_ptr<Editor> editor;
    Harness() {
        Ship::Context::current = std::make_shared<Ship::Context>();
        values.clear();
        stringValues.clear();
        GdxPhysicalInput::EndKeyboardCapture();
        GdxPhysicalInput::Invalidate();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 1400);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::GetCurrentContext()->TestEngineHookItems = true;
        editor = std::make_unique<Editor>();
        Frame();
        Frame();
        CHECK(!CaptureActive());
    }
    ~Harness() {
        editor.reset();
        ImGui::DestroyContext();
    }
    void Frame(bool visible = true, bool collapsed = false, float dt = 1.0f / 60.0f) {
        items.clear();
        rectangles.clear();
        ImGui::GetIO().DeltaTime = dt;
        ImGui::NewFrame();
        if (editor) editor->UpdateElement();
        if (visible && editor) {
            ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(1200, 1250), ImGuiCond_Always);
            ImGui::SetNextWindowCollapsed(collapsed, ImGuiCond_Always);
            if (ImGui::Begin("Physical input test")) editor->DrawElement();
            ImGui::End();
        }
        ImGui::Render();
    }
    Item Find(const char* label, int occurrence = 0) {
        for (const auto& item : items) {
            if (item.label == label && occurrence-- == 0) return item;
        }
        std::fprintf(stderr, "Widget not found: %s; available:\n", label);
        for (const auto& item : items) std::fprintf(stderr, "  %s\n", item.label.c_str());
        CHECK(false);
        return {};
    }
    void Click(const char* label, int occurrence = 0) {
        auto point = Find(label, occurrence).rect.GetCenter();
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        Frame();
        io.AddMouseButtonEvent(0, true);
        Frame();
        io.AddMouseButtonEvent(0, false);
        Frame();
        Frame(); // Let popups/tabs finish appearing and acquire hover.
    }
    bool Modal() {
        auto* window = ImGui::FindWindowByName("Physical input test");
        CHECK(window != nullptr);
        return ImGui::IsPopupOpen(ImHashStr(Popup, 0, window->ID), ImGuiPopupFlags_AnyPopupLevel);
    }
    bool Blocked() { return Ship::Context::current->deck->GameInputBlocked(); }
    bool NavigationBlocked() { return Ship::Context::current->window->gui->gamepadNavigationBlocked; }
    bool CaptureActive() { return Ship::Context::current->window->gui->IsInputCaptureActive(); }
    void Open() {
        Frame(); // Refresh inline button positions after externally seeded bindings.
        Click("+###Add binding");
        CHECK(Modal());
        CHECK(Blocked());
        CHECK(NavigationBlocked());
        CHECK(CaptureActive());
    }
    void Key(int code, bool consumed = true) {
        // Supply ownership as the real GUI event route would, without compiling that route.
        GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_DOWN, static_cast<Ship::KbScancode>(code), consumed);
        Frame();
        GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_UP, static_cast<Ship::KbScancode>(code));
    }
    void SaveKey(int code) {
        Frame(true, false, 0.30f); // Arm only after the real production debounce deadline.
        Key(code);
        CHECK(!Modal());
        CHECK(!CaptureActive());
        CHECK(Blocked()); // Delayed release prevents the bound key leaking into the game.
        Frame(true, false, 0.40f);
        CHECK(!Blocked());
        CHECK(!NavigationBlocked());
        CHECK(!CaptureActive());
    }
};
void Binding(int port, int context, int code) {
    auto bindings = GdxPhysicalInput::GetBindings(port, context, A);
    CHECK(bindings.size() == 1);
    auto key = std::dynamic_pointer_cast<Ship::KeyboardKeyToButtonMapping>(bindings[0]);
    CHECK(key != nullptr);
    CHECK(key->GetKeyboardScancode() == static_cast<Ship::KbScancode>(code));
}
void AddSave(Harness& h) {
    const auto accelerate = h.Find("Accelerate").rect;
    const auto boost = h.Find("Boost").rect;
    const auto add = h.Find("+###Add binding").rect;
    const auto nextAdd = h.Find("+###Add binding", 1).rect;
    CHECK(accelerate.GetCenter().y == add.GetCenter().y);
    CHECK(boost.GetCenter().y == nextAdd.GetCenter().y);
    CHECK(boost.Min.y > accelerate.Max.y);
    CHECK(add.Min.x == nextAdd.Min.x);
    CHECK(add.Min.x > accelerate.Max.x);
    const auto before = values;
    h.Open();
    CHECK(values == before && saves == 0 && writes == 0);
    h.Key(Ship::LUS_KB_A); // Opening/debounce input must not persist.
    CHECK(h.Modal() && saves == 0 && writes == 0);
    h.SaveKey(Ship::LUS_KB_B);
    CHECK(saves == 1 && writes > 0);
    Binding(0, Race, Ship::LUS_KB_B);
    const auto binding = h.Find("Key##Binding").rect;
    const auto boundAdd = h.Find("+###Add binding").rect;
    CHECK(binding.GetCenter().y == h.Find("Accelerate").rect.GetCenter().y);
    CHECK(binding.GetCenter().y == boundAdd.GetCenter().y);
    CHECK(boundAdd.Min.x > binding.Max.x);
    GdxPhysicalInput::Invalidate();
    Binding(0, Race, Ship::LUS_KB_B); // Saved CVars, not just editor-local state.
}
void Cancel(Harness& h) {
    // A pre-existing Escape binding makes cancel-key leakage observable after unblocking.
    CHECK(GdxPhysicalInput::AddBinding(0, Race, A, std::make_shared<Ship::KeyboardKeyToButtonMapping>(
        0, gdx_input_actions_output_bit(A), Ship::LUS_KB_ESCAPE)));
    saves = writes = 0;
    auto before = values;
    h.Open();
    h.Click("Cancel");
    CHECK(!h.Modal());
    CHECK(!h.CaptureActive());
    CHECK(values == before && saves == 0 && writes == 0);
    h.Frame(true, false, 0.40f);
    CHECK(!h.Blocked() && !h.NavigationBlocked());
    h.Open();
    h.Frame(true, false, 0.30f); // Armed Escape must cancel, not replace/add a binding.
    GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_DOWN, Ship::LUS_KB_ESCAPE, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    h.Frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    CHECK(!h.Modal());
    CHECK(!h.CaptureActive());
    CHECK(GdxPhysicalInput::TakeCapturedKey() == -1);
    CHECK(values == before && saves == 0 && writes == 0);
    h.Frame(true, false, 0.40f);
    CHECK(!h.Blocked() && !h.NavigationBlocked() && !h.CaptureActive());
    CHECK(gdx_physical_input_apply(0, Race, 0) == 0);
    // An unconsumed repeat still belongs to the consumed hold, even after cache rebuild.
    GdxPhysicalInput::Invalidate();
    GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_DOWN, Ship::LUS_KB_ESCAPE, false);
    CHECK(gdx_physical_input_apply(0, Race, 0) == 0);
    GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_UP, Ship::LUS_KB_ESCAPE, true);
    GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_DOWN, Ship::LUS_KB_ESCAPE);
    CHECK(gdx_physical_input_apply(0, Race, 0) == gdx_input_actions_output_bit(A));
    GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_UP, Ship::LUS_KB_ESCAPE);
    CHECK(gdx_physical_input_apply(0, Race, 0) == 0);
    CHECK(values == before && saves == 0 && writes == 0);
}
void ControllerCapture(Harness& h) {
    SDL_GameController pad;
    auto devices = Ship::Context::current->deck->devices;
    devices->ports[0][7] = &pad;
    pad.buttons[SDL_CONTROLLER_BUTTON_BACK] = true;
    h.Open();
    h.Frame(true, false, 0.50f);
    h.Frame(true, false, 0.50f);
    CHECK(h.Modal() && h.CaptureActive() && h.Blocked());
    CHECK(saves == 0 && writes == 0 && values.empty());
    pad.buttons[SDL_CONTROLLER_BUTTON_BACK] = false;
    h.Frame(); // Neutral input after the deadline arms capture, without saving.
    CHECK(h.Modal() && h.CaptureActive());
    CHECK(saves == 0 && writes == 0);
    pad.buttons[SDL_CONTROLLER_BUTTON_BACK] = true;
    h.Frame();
    CHECK(!h.Modal() && !h.CaptureActive());
    CHECK(h.Blocked());
    CHECK(saves == 1 && writes > 0);
    GdxPhysicalInput::Invalidate();
    auto bindings = GdxPhysicalInput::GetBindings(0, Race, A);
    CHECK(bindings.size() == 1);
    auto button = std::dynamic_pointer_cast<Ship::SDLButtonToButtonMapping>(bindings[0]);
    CHECK(button != nullptr);
    CHECK(button->GetSDLControllerButton() == SDL_CONTROLLER_BUTTON_BACK);
    h.Frame(true, false, 0.40f); // The captured button remains held past the old timeout.
    CHECK(h.Blocked() && h.NavigationBlocked());
    pad.buttons[SDL_CONTROLLER_BUTTON_BACK] = false;
    h.Frame(true, false, 0.40f);
    CHECK(!h.Blocked() && !h.NavigationBlocked() && !h.CaptureActive());
    devices->ports[0].clear();
}
void RebindRemove(Harness& h) {
    h.Open();
    h.SaveKey(Ship::LUS_KB_A);
    h.Click("Key##Binding");
    h.Click("Change binding");
    CHECK(h.Modal() && h.CaptureActive());
    h.SaveKey(Ship::LUS_KB_B);
    Binding(0, Race, Ship::LUS_KB_B);
    CHECK(saves == 2);
    h.Click("Key##Binding");
    h.Click("Remove binding");
    CHECK(GdxPhysicalInput::IsConfigured(0, Race, A));
    CHECK(GdxPhysicalInput::GetEffectiveBindings(0, Race, A).empty());
    CHECK(saves == 3);
}
void Isolation(Harness& h) {
    // Select the player and contexts through real widgets; no private field mutation.
    h.Click("Player");
    h.Click("Player 2");
    h.Click("Menu");
    h.Open();
    h.SaveKey(Ship::LUS_KB_A);
    h.Click("Course Edit");
    h.Open();
    h.SaveKey(Ship::LUS_KB_B);
    h.Click("Player");
    h.Click("Player 1");
    h.Click("Race");
    h.Open();
    h.SaveKey(Ship::LUS_KB_C);
    Binding(1, Menu, Ship::LUS_KB_A);
    Binding(1, Edit, Ship::LUS_KB_B);
    Binding(0, Race, Ship::LUS_KB_C);
    for (int port = 0; port < 4; ++port) {
        for (int context = 0; context < GDX_INPUT_CONTEXT_COUNT; ++context) {
            for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
                bool expected = output == A && ((port == 1 && (context == Menu || context == Edit)) ||
                                                (port == 0 && context == Race));
                CHECK(GdxPhysicalInput::IsConfigured(port, context, output) == expected);
            }
        }
    }
    CHECK(saves == 3);
}
void Teardown(Harness& h, bool collapsed) {
    h.Open();
    h.Frame(collapsed, collapsed);
    h.Frame(collapsed, collapsed); // Missing-draw heartbeat cancels on the second missed frame.
    GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_DOWN, Ship::LUS_KB_A);
    CHECK(GdxPhysicalInput::TakeCapturedKey() == -1);
    CHECK(!h.CaptureActive());
    CHECK(h.Blocked());
    h.Frame(collapsed, collapsed, 0.40f);
    CHECK(!h.Blocked() && !h.NavigationBlocked() && !h.CaptureActive());
    CHECK(saves == 0 && writes == 0 && values.empty());
    h.Frame(); // Reopening must not revive the abandoned capture popup.
    CHECK(!h.Modal());
}
void Heartbeat(Harness& h) {
    h.Open();
    for (int i = 0; i < 90; ++i) {
        h.Frame();
        CHECK(h.CaptureActive());
    }
    CHECK(h.Modal() && h.Blocked() && h.NavigationBlocked());
    CHECK(saves == 0 && writes == 0);
    h.SaveKey(Ship::LUS_KB_A);
    Binding(0, Race, Ship::LUS_KB_A);
}
void Profiles(Harness& h) {
    const auto directory = std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "gamepad-profiles";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    CHECK(!ec);

    CHECK(GdxPhysicalInput::AddBinding(0, Race, A,
          std::make_shared<Ship::SDLButtonToButtonMapping>(0, gdx_input_actions_output_bit(A), SDL_CONTROLLER_BUTTON_X)));
    auto saveFixture = [&](const char* name) {
        std::string error;
        const std::string json = GdxPhysicalInput::ExportGamepadProfile(0, name, &error);
        CHECK(!json.empty() && error.empty());
        std::ofstream file(directory / (std::string(name) + ".json"), std::ios::binary);
        CHECK(static_cast<bool>(file));
        file << json;
        CHECK(static_cast<bool>(file));
    };
    saveFixture("WiiU");
    saveFixture("Personal");
    stringValues["gEnhancements.Input.Physical.Port1.ActiveGamepadProfile"] = "Default";

    h.Click("Profile Management");
    h.Frame();
    CHECK(std::filesystem::exists(directory / "Default.json")); // First-run Default is available in the list.
    CHECK(h.Find("Currently Loaded").label == "Currently Loaded");
    CHECK(h.Find("Make a new profile").label == "Make a new profile");
    h.Click("Load"); // Sorted list: Default is current, so Personal is the first Load.
    CHECK(stringValues["gEnhancements.Input.Physical.Port1.ActiveGamepadProfile"] == "Personal");

    h.Click("Remove", 1); // Keep Default and Personal; target WiiU.

    CHECK(std::filesystem::exists(directory / "WiiU.json"));
    h.Click("Remove profile");
    CHECK(!std::filesystem::exists(directory / "WiiU.json"));

    h.Click("Make a new profile");
    h.Click("Profile name");
    ImGui::GetIO().AddInputCharactersUTF8("Custom");
    h.Frame();
    h.Click("Create");
    CHECK(std::filesystem::exists(directory / "Custom.json"));
    CHECK(stringValues["gEnhancements.Input.Physical.Port1.ActiveGamepadProfile"] == "Custom");
    std::filesystem::remove_all(directory, ec);
}

void Destructor(Harness& h) {
    h.Open();
    h.editor.reset();
    CHECK(!h.Blocked());
    CHECK(!h.CaptureActive());
    GdxPhysicalInput::ProcessKeyboardEvent(Ship::LUS_KB_EVENT_KEY_DOWN, Ship::LUS_KB_A);
    CHECK(GdxPhysicalInput::TakeCapturedKey() == -1);
    CHECK(saves == 0 && writes == 0);
    CHECK(!h.NavigationBlocked());
}
} // namespace
int main(int argc, char** argv) {
    CHECK(argc == 2);
    Harness h;
    const std::string name = argv[1];
    if (name == "add_save") AddSave(h);
    else if (name == "cancel") Cancel(h);
    else if (name == "controller_capture") ControllerCapture(h);
    else if (name == "rebind_remove") RebindRemove(h);
    else if (name == "isolation") Isolation(h);
    else if (name == "hidden") Teardown(h, false);
    else if (name == "collapsed") Teardown(h, true);
    else if (name == "heartbeat") Heartbeat(h);
    else if (name == "destructor") Destructor(h);
    else if (name == "profiles") Profiles(h);
    else CHECK(false);
    std::printf("PASS %s (%d checks)\n", name.c_str(), checks);
}
