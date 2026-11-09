#include "gdx_input_editor.h"
#include "gdx_physical_input.h"

#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToButtonMapping.h"
#include "ship/window/Window.h"
#include "ship/window/gui/IconsFontAwesome4.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
constexpr int kCaptureBlockId = 95237930;
constexpr const char* kCapturePopup = "Bind action##PhysicalInputCapture";
struct ActionText {
    const char* label;
    const char* tip;
};

// Output order is GdxInputOutput. Race D-pad outputs have no gameplay consumers.
const ActionText kRaceActions[GDX_INPUT_OUT_COUNT] = {
    { "Accelerate", "Hold to accelerate." },
    { "Boost", "Boost when the machine has energy and no boost is running." },
    { "Drift left / side attack", "Hold to drift. Press for a side attack. Hold drift right and press drift left for a spin attack." },
    { "Pause", "Pause the race and open the pause menu." },
    { nullptr, nullptr }, { nullptr, nullptr }, { nullptr, nullptr }, { nullptr, nullptr },
    { "Gap to leader toggle", "Toggle the gap-to-leader time display." },
    { "Drift right / side attack", "Hold to drift. Press for a side attack. Hold drift left and press drift right for a spin attack." },
    { "Rear view", "Hold to look back." },
    { "Air brake", "Hold to brake." },
    { "Lap counter toggle", "Toggle the minimap lap counter in races with two or more players." },
    { "Cycle camera", "Cycle the race camera." },
};

// Contextual labels follow the decomp consumers; additional screen-specific behavior is in tips.
const ActionText kMenuActions[GDX_INPUT_OUT_COUNT] = {
    { "Confirm / interact", "Confirm choices. In Machine Create view mode, hold to zoom in." },
    { "Back / cancel", "Return to the previous screen. In Machine Create view mode, hold to zoom out." },
    { "Previous color / fast scroll", "Cycle machine color backward; hold for faster file-list scrolling. In Photo Mode, hold with the stick for vertical movement." },
    { "Confirm / switch view controls", "Confirm menu and pause-menu choices. In Machine Create, finish the panel or toggle view controls." },
    { "Navigate up", "Navigate menus or rotate the Machine Create model vertically." },
    { "Navigate down", "Navigate menus or rotate the Machine Create model vertically." },
    { "Navigate left", "Navigate or adjust settings; rotate the Machine Create model horizontally." },
    { "Navigate right", "Navigate or adjust settings; rotate the Machine Create model horizontally." },
    { "Zoom out", "Shrink the Machine Create preview or widen Photo Mode's field of view." },
    { "Next color / zoom in", "Cycle machine color forward; enlarge the Machine Create preview or narrow Photo Mode's field of view." },
    { "View up / Master trophies", "Rotate or pan previews upward; look up in Photo Mode. On machine select, show Master trophies when unlocked." },
    { "View down / Standard trophies", "Rotate or pan previews downward; look down in Photo Mode. On machine select, show Standard trophies." },
    { "View left / Novice trophies", "Rotate or pan previews left; look left in Photo Mode. On machine select, show Novice trophies." },
    { "View right / Expert trophies", "Rotate or pan previews right; look right in Photo Mode. On machine select, show Expert trophies." },
};

const ActionText kCourseActions[GDX_INPUT_OUT_COUNT] = {
    { "Confirm / edit", "Confirm choices, select points, and apply edits. In camera mode, hold to zoom in." },
    { "Undo / cancel", "Undo an editor change, cancel point manipulation, or leave a dialog. In camera mode, hold to zoom out." },
    { "Select / focus / speed up", "Select a segment or drag a box to select points. Double-tap near a point to focus. Hold for faster movement or scrolling." },
    { "Toggle camera mode", "Switch between editing and camera controls. The movement stick orbits in camera mode." },
    { "Orbit camera up", "Increase the camera's vertical orbit angle." },
    { "Orbit camera down", "Decrease the camera's vertical orbit angle." },
    { "Orbit camera left", "Orbit the camera horizontally to the left." },
    { "Orbit camera right", "Orbit the camera horizontally to the right." },
    { "Zoom out", "Move the camera farther from its target." },
    { "Zoom in", "Move the camera closer; at minimum distance, advance its target when permitted." },
    { "Pan camera up", "Move the camera target upward relative to the view." },
    { "Pan camera down", "Move the camera target downward relative to the view." },
    { "Pan camera left", "Move the camera target left relative to the view." },
    { "Pan camera right", "Move the camera target right relative to the view." },
};

const ActionText* Actions(int context) {
    if (context == GDX_INPUT_CONTEXT_RACE) {
        return kRaceActions;
    }
    return context == GDX_INPUT_CONTEXT_COURSE_EDIT ? kCourseActions : kMenuActions;
}

void GetBindingColors(Ship::PhysicalDeviceType deviceType, ImVec4& color, ImVec4& hoveredColor) {
    switch (deviceType) {
        case Ship::PhysicalDeviceType::Keyboard:
            color = ImVec4(0.651f, 0.482f, 0.357f, 0.5f);
            hoveredColor = ImVec4(0.651f, 0.482f, 0.357f, 1.0f);
            break;
        case Ship::PhysicalDeviceType::Mouse:
            color = ImVec4(0.5f, 0.5f, 0.5f, 0.5f);
            hoveredColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            break;
        case Ship::PhysicalDeviceType::SDLGamepad:
            color = ImVec4(0.0f, 0.255f, 0.976f, 0.5f);
            hoveredColor = ImVec4(0.0f, 0.255f, 0.976f, 1.0f);
            break;
        default:
            color = ImVec4(0.431f, 0.369f, 0.706f, 0.5f);
            hoveredColor = ImVec4(0.431f, 0.369f, 0.706f, 1.0f);
            break;
    }
}

const char* BindingIcon(Ship::ControllerButtonMapping& binding) {
    switch (binding.GetMappingType()) {
        case MAPPING_TYPE_GAMEPAD:
            return ICON_FA_GAMEPAD;
        case MAPPING_TYPE_KEYBOARD:
        case MAPPING_TYPE_MOUSE:
            return ICON_FA_KEYBOARD_O;
        default:
            return ICON_FA_BUG;
    }
}

std::string ProfileStem(const char* name) {
    std::string stem;
    if (name == nullptr) return stem;
    for (const unsigned char character : std::string(name)) {
        if (std::isalnum(character) || character == '-' || character == '_') {
            stem.push_back(static_cast<char>(character));
        } else if (character == ' ') {
            stem.push_back('-');
        } else {
            return {};
        }
    }
    return stem;
}

std::filesystem::path LocalProfilePath(const char* name) {
    const std::string stem = ProfileStem(name);
    if (stem.empty()) return {};
    return std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "gamepad-profiles" / (stem + ".json");
}

bool ReadProfileFile(const std::filesystem::path& path, std::string& contents, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 1024 * 1024) {
        error = "Could not read the profile file, or it is empty/over 1 MB.";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Could not open the profile file.";
        return false;
    }
    contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad() || contents.size() != size) {
        error = "Could not read the complete profile file.";
        return false;
    }
    return true;
}

bool WriteProfileFile(const std::filesystem::path& path, const std::string& contents, std::string& error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    if (ec) {
        error = "Could not create the profile directory.";
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not create the profile file.";
        return false;
    }
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) {
        error = "Could not write the complete profile file.";
        return false;
    }
    return true;
}
} // namespace

GdxInputEditor::~GdxInputEditor() {
    GdxPhysicalInput::EndKeyboardCapture();
    auto ctx = Ship::Context::GetInstance();
    if (ctx != nullptr) {
        if (ctx->GetControlDeck() != nullptr) {
            ctx->GetControlDeck()->UnblockGameInput(kCaptureBlockId);
        }
        if (ctx->GetWindow() != nullptr && ctx->GetWindow()->GetGui() != nullptr) {
            ctx->GetWindow()->GetGui()->SetInputCaptureActive(false);
            ctx->GetWindow()->GetGui()->SetGamepadNavigationBlocked(false);
        }
    }
}

void GdxInputEditor::BeginCapture(int context, int output, int index) {
    mCapturePort = mPort;
    mCaptureContext = context;
    mCaptureOutput = output;
    mCaptureIndex = index;
    mCapturing = true;
    mCaptureArmed = false;
    mLastCaptureFrame = ImGui::GetFrameCount();
    mArmAfter = ImGui::GetTime() + 0.25;
    GdxPhysicalInput::BeginKeyboardCapture();
    auto ctx = Ship::Context::GetInstance();
    ctx->GetControlDeck()->BlockGameInput(kCaptureBlockId);
    ctx->GetWindow()->GetGui()->SetInputCaptureActive(true);
    ctx->GetWindow()->GetGui()->SetGamepadNavigationBlocked(true);
    ImGui::OpenPopup(kCapturePopup);
}

void GdxInputEditor::EndCapture() {
    GdxPhysicalInput::EndKeyboardCapture();
    Ship::Context::GetInstance()->GetWindow()->GetGui()->SetInputCaptureActive(false);
    mCapturing = false;
    mUnblockAfter = ImGui::GetTime() + 0.33;
}

void GdxInputEditor::UpdateElement() {
    LUS::InputEditorWindow::UpdateElement();
    // Updates run even while the window is hidden. A missing draw cancels capture rather than
    // leaving input blocked after changing pages, closing the menu, or collapsing the pop-out.
    if (mCapturing && mLastCaptureFrame < ImGui::GetFrameCount() - 1) {
        EndCapture();
    }
    auto ctx = Ship::Context::GetInstance();
    const bool waitingForGamepadRelease =
        mAwaitingGamepadRelease && GdxPhysicalInput::IsGamepadInputHeld(mCapturePort);
    if (mCapturing || ImGui::GetTime() < mUnblockAfter || waitingForGamepadRelease) {
        ctx->GetControlDeck()->BlockGameInput(kCaptureBlockId);
        ctx->GetWindow()->GetGui()->SetGamepadNavigationBlocked(true);
    } else {
        mAwaitingGamepadRelease = false;
        ctx->GetControlDeck()->UnblockGameInput(kCaptureBlockId);
    }
}

void GdxInputEditor::DrawActions(int context) {
    const auto* rows = Actions(context);
    ImGui::PushID(context);
    if (ImGui::Button("Restore saved defaults")) {
        ImGui::OpenPopup("Restore bindings?");
    }
    if (ImGui::BeginPopup("Restore bindings?")) {
        ImGui::TextWrapped("Restore this player's %s actions to the bindings used before direct action edits? Other players and contexts are unchanged.",
                           context == GDX_INPUT_CONTEXT_RACE ? "Race" :
                           context == GDX_INPUT_CONTEXT_MENU ? "Menu" : "Course Edit");
        if (ImGui::Button("Restore")) {
            for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
                GdxPhysicalInput::UseLegacyBindings(mPort, context, output);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float rowIndent = 32.0f * scale;
    float labelWidth = 0.0f;
    for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
        if (rows[output].label != nullptr) {
            const float width = ImGui::CalcTextSize(rows[output].label).x;
            if (width > labelWidth) {
                labelWidth = width;
            }
        }
    }
    labelWidth += ImGui::GetStyle().FramePadding.x * 2.0f;
    const float bindingsOffset = rowIndent + labelWidth + ImGui::GetStyle().ItemSpacing.x;
    for (int output = 0; output < GDX_INPUT_OUT_COUNT; ++output) {
        if (rows[output].label == nullptr) {
            continue;
        }
        ImGui::PushID(output);
        ImGui::NewLine();
        ImGui::SameLine(rowIndent);
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::Button(rows[output].label, ImVec2(labelWidth, 0.0f));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", rows[output].tip);
        }
        ImGui::SameLine(bindingsOffset);
        const float rowRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        auto bindings = GdxPhysicalInput::GetEffectiveBindings(mPort, context, output);
        if (bindings.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Unbound");
            ImGui::SameLine();
        }
        for (int index = 0; index < static_cast<int>(bindings.size()); ++index) {
            ImGui::PushID(index);
            const auto& binding = bindings[index];
            const std::string inputName = binding->GetPhysicalInputName();
            const std::string label = std::string(BindingIcon(*binding)) + " " + inputName;
            ImVec4 buttonColor;
            ImVec4 buttonHoveredColor;
            GetBindingColors(binding->GetPhysicalDeviceType(), buttonColor, buttonHoveredColor);
            ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, buttonHoveredColor);
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            const float buttonWidth = ImGui::CalcTextSize(label.c_str()).x + 12.0f * scale;
            const float removeButtonWidth = ImGui::CalcTextSize(ICON_FA_TIMES).x + 10.0f * scale;
            const float bindingGroupWidth = buttonWidth + removeButtonWidth + 4.0f * scale;
            if (ImGui::GetCursorScreenPos().x + bindingGroupWidth > rowRight) {
                ImGui::NewLine();
                ImGui::SameLine(bindingsOffset);
            }
            const bool clicked = ImGui::Button(label.c_str(), ImVec2(buttonWidth, 0.0f));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (clicked) {
                mCaptureContext = context;
                mCaptureOutput = output;
                mCaptureIndex = index;
                mCapturePort = mPort;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\nClick to change binding.", binding->GetPhysicalDeviceName().c_str());
            }
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, buttonHoveredColor);
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.0f, 0.5f));
            const bool removeClicked = ImGui::Button(ICON_FA_TIMES "###Remove binding", ImVec2(removeButtonWidth, 0.0f));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (removeClicked) {
                GdxPhysicalInput::RemoveBinding(mPort, context, output, index);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove binding");
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (ImGui::GetCursorScreenPos().x + 20.0f * scale > rowRight) {
            ImGui::NewLine();
            ImGui::SameLine(bindingsOffset);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.0f, 0.5f));
        const bool addBinding = ImGui::Button(ICON_FA_PLUS "###Add binding", ImVec2(20.0f * scale, 0.0f));
        ImGui::PopStyleVar();
        if (addBinding) {
            mCaptureContext = context;
            mCaptureOutput = output;
            mCaptureIndex = -1;
            mCapturePort = mPort;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add binding");
        }

        ImGui::PopID();
    }
    ImGui::PopID();
}

void GdxInputEditor::DrawCapture() {
    if (!mCapturing) {
        return;
    }
    mLastCaptureFrame = ImGui::GetFrameCount();
    if (ImGui::BeginPopupModal(kCapturePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Player %d: %s", mCapturePort + 1, Actions(mCaptureContext)[mCaptureOutput].label);
        ImGui::TextUnformatted(mCaptureArmed ? "Press a key, gamepad button, or move an axis."
                                          : "Release gamepad buttons and center the sticks...");
        bool finished = false;
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            finished = true;
        } else {
            auto binding = GdxPhysicalInput::CaptureSDL(mCapturePort, mCaptureOutput);
            const int key = GdxPhysicalInput::TakeCapturedKey();
            if (!mCaptureArmed) {
                if (ImGui::GetTime() >= mArmAfter && binding == nullptr) {
                    mCaptureArmed = true;
                }
            } else {
                if (key >= 0) {
                    binding = std::make_shared<Ship::KeyboardKeyToButtonMapping>(
                        mCapturePort, gdx_input_actions_output_bit(mCaptureOutput), static_cast<Ship::KbScancode>(key));
                }
                if (binding != nullptr) {
                    mAwaitingGamepadRelease = mAwaitingGamepadRelease ||
                                               GdxPhysicalInput::IsGamepadInputHeld(mCapturePort);
                    finished = mCaptureIndex < 0 ?
                        GdxPhysicalInput::AddBinding(mCapturePort, mCaptureContext, mCaptureOutput, binding) :
                        GdxPhysicalInput::ReplaceBinding(mCapturePort, mCaptureContext, mCaptureOutput, mCaptureIndex, binding);
                    if (!finished) {
                        ImGui::TextWrapped("Couldn't store this binding. Remove an existing binding and try again.");
                    }
                }
            }
        }
        if (finished) {
            EndCapture();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        EndCapture();
    }
}

void GdxInputEditor::DrawElement() {
    // A request made by a row is opened after leaving its ImGui ID stack.
    mCaptureOutput = mCapturing ? mCaptureOutput : -1;
    ImGui::TextWrapped("Choose an action, then add a keyboard key, gamepad button, or axis. Click an existing binding to change or remove it. Changes save automatically.");
    const char* players[] = { "Player 1", "Player 2", "Player 3", "Player 4" };
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("Player", &mPort, players, 4);
    ImGui::TextDisabled("Bindings below affect the game, not navigation in this settings window.");

    if (ImGui::CollapsingHeader("Profile Management")) {
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("Profile name", mProfileName, sizeof(mProfileName));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("Import / export file", mProfileFile, sizeof(mProfileFile));
        ImGui::TextWrapped("Profiles include only gamepad buttons/axes; imports replace those for the selected player and preserve keyboard/mouse bindings.");
        ImGui::TextDisabled("Save/load uses the local gamepad-profiles folder; enter a file path above to import or export.");

        if (ImGui::Button("Save profile")) {
            const auto path = LocalProfilePath(mProfileName);
            if (path.empty()) {
                mProfileStatus = "Use a profile name containing only letters, numbers, spaces, - or _.";
            } else {
                std::string error;
                const std::string json = GdxPhysicalInput::ExportGamepadProfile(mPort, mProfileName, &error);
                if (json.empty()) {
                    mProfileStatus = error.empty() ? "Could not create the profile." : error;
                } else if (WriteProfileFile(path, json, error)) {
                    mProfileStatus = "Saved: " + path.string();
                } else {
                    mProfileStatus = error;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load profile")) {
            const auto path = LocalProfilePath(mProfileName);
            std::string json, error;
            if (path.empty()) {
                mProfileStatus = "Use a profile name containing only letters, numbers, spaces, - or _.";
            } else if (ReadProfileFile(path, json, error) &&
                       GdxPhysicalInput::ImportGamepadProfile(mPort, json, &error)) {
                mProfileStatus = "Loaded profile for Player " + std::to_string(mPort + 1) + ".";
            } else {
                mProfileStatus = error;
            }
        }
        ImGui::NewLine();
        if (ImGui::Button("Import profile")) {
            std::string json, error;
            if (mProfileFile[0] == '\0') {
                mProfileStatus = "Enter the path to a gamepad profile JSON file.";
            } else if (ReadProfileFile(std::filesystem::path(mProfileFile), json, error) &&
                       GdxPhysicalInput::ImportGamepadProfile(mPort, json, &error)) {
                const auto localPath = LocalProfilePath(mProfileName);
                if (!localPath.empty() && WriteProfileFile(localPath, json, error)) {
                    mProfileStatus = "Imported for Player " + std::to_string(mPort + 1) + " and saved: " +
                                     localPath.string();
                } else {
                    mProfileStatus = "Imported for Player " + std::to_string(mPort + 1) +
                                     ", but could not save locally: " + error;
                }
            } else {
                mProfileStatus = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Export profile")) {
            const auto localPath = LocalProfilePath(mProfileName);
            std::string json, error;
            if (mProfileFile[0] == '\0') {
                mProfileStatus = "Enter the destination path for the exported profile.";
            } else if (localPath.empty()) {
                mProfileStatus = "Use a profile name containing only letters, numbers, spaces, - or _.";
            } else if (ReadProfileFile(localPath, json, error) &&
                       WriteProfileFile(std::filesystem::path(mProfileFile), json, error)) {
                mProfileStatus = "Exported: " + std::filesystem::path(mProfileFile).string();
            } else {
                mProfileStatus = error;
            }
        }
        if (!mProfileStatus.empty()) {
            ImGui::TextWrapped("%s", mProfileStatus.c_str());
        }
    }

    if (ImGui::BeginTabBar("##ActionContexts")) {
        if (ImGui::BeginTabItem("Race")) {
            ImGui::TextDisabled("On track and test drives, while not paused.");
            DrawActions(GDX_INPUT_CONTEXT_RACE);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Menu")) {
            ImGui::TextDisabled("Game menus, pause menus, and Machine Create.");
            DrawActions(GDX_INPUT_CONTEXT_MENU);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Course Edit")) {
            ImGui::TextDisabled("Editing controls. Test drives use Race bindings.");
            DrawActions(GDX_INPUT_CONTEXT_COURSE_EDIT);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!mCapturing && mCaptureOutput >= 0) {
        BeginCapture(mCaptureContext, mCaptureOutput, mCaptureIndex);
    }
    ImGui::PushID(mPort);
    if (ImGui::CollapsingHeader("Steering / movement")) {
        ImGui::TextWrapped("Shared analog movement: steer and pitch in races, navigate menus, and move or orbit in the editor.");
        DrawMovementSettings(mPort);
    }
    if (ImGui::CollapsingHeader("Devices and feedback")) {
        DrawDeviceSettings(mPort);
    }
    ImGui::PopID();
    DrawCapture();
}
