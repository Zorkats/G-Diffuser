// Copyright (c) Zorkats

#include "gdx_companion_ui.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include "gdx_companion_protocol.h"

namespace gdx_companion {

namespace {

struct SectionEntry {
    Section section;
    const char* label;
};

constexpr SectionEntry kSections[] = {
    {Section::Music, "Music"},     {Section::Textures, "Textures"}, {Section::Models, "Models"},
    {Section::Tracks, "Tracks"},   {Section::Inspect, "Inspect"},   {Section::Advanced, "Advanced"},
    {Section::Settings, "Settings"},
};

constexpr int kScalePercents[] = {100, 125, 150, 200};

ImVec4 ToVec4(Rgb8 color, float alpha = 1.0f) {
    return ImVec4(static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                  static_cast<float>(color.b) / 255.0f, alpha);
}

const SectionEntry& FindSection(Section section) {
    for (const SectionEntry& entry : kSections) {
        if (entry.section == section) return entry;
    }
    return kSections[sizeof(kSections) / sizeof(kSections[0]) - 1];
}

const char* InstallLabel(InstallState state) {
    switch (state) {
    case InstallState::Unknown:
        return "Not checked";
    case InstallState::Validated:
        return "Validated";
    case InstallState::Unavailable:
        return "Unavailable";
    }
    return "Not checked";
}

const char* WorkerLabel(WorkerState state) {
    switch (state) {
    case WorkerState::NotStarted:
        return "Not started";
    case WorkerState::Starting:
        return "Starting";
    case WorkerState::Ready:
        return "Ready";
    case WorkerState::Failed:
        return "Failed";
    case WorkerState::Stopped:
        return "Stopped";
    }
    return "Not started";
}

void ApplyThemeColors(ImGuiStyle& style, const ThemeTokens& theme) {
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ToVec4(theme.text);
    colors[ImGuiCol_TextDisabled] = ToVec4(theme.text_secondary);
    colors[ImGuiCol_TextSelectedBg] = ToVec4(theme.accent, 0.30f);
    // The root window draws no background so the backdrop shows between panels; every text block sits
    // in a child panel.
    colors[ImGuiCol_WindowBg] = ToVec4(theme.panel, theme.panel_alpha);
    colors[ImGuiCol_ChildBg] = ToVec4(theme.panel, theme.panel_alpha);
    colors[ImGuiCol_PopupBg] = ToVec4(theme.panel, 0.98f);
    colors[ImGuiCol_Border] = ToVec4(theme.accent, 0.30f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = ToVec4(theme.text, 0.06f);
    colors[ImGuiCol_FrameBgHovered] = ToVec4(theme.accent, 0.20f);
    colors[ImGuiCol_FrameBgActive] = ToVec4(theme.accent, 0.30f);
    colors[ImGuiCol_Button] = ToVec4(theme.accent, 0.20f);
    colors[ImGuiCol_ButtonHovered] = ToVec4(theme.accent, 0.30f);
    colors[ImGuiCol_ButtonActive] = ToVec4(theme.accent, 0.50f);
    colors[ImGuiCol_Header] = ToVec4(theme.accent, 0.20f);
    colors[ImGuiCol_HeaderHovered] = ToVec4(theme.accent, 0.30f);
    colors[ImGuiCol_HeaderActive] = ToVec4(theme.accent, 0.50f);
    colors[ImGuiCol_CheckMark] = ToVec4(theme.accent);
    colors[ImGuiCol_SliderGrab] = ToVec4(theme.accent);
    colors[ImGuiCol_SliderGrabActive] = ToVec4(theme.accent);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = ToVec4(theme.accent, 0.50f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ToVec4(theme.accent, 0.70f);
    colors[ImGuiCol_ScrollbarGrabActive] = ToVec4(theme.accent);
    colors[ImGuiCol_Separator] = ToVec4(theme.accent, 0.30f);
    colors[ImGuiCol_NavCursor] = ToVec4(theme.accent);
    colors[ImGuiCol_NavWindowingHighlight] = ToVec4(theme.accent, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
}

void ApplyStyleMetrics(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 4.0f);
    style.WindowRounding = 6.0f;
    style.ChildRounding = 5.0f;
    style.PopupRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
}

}  // namespace

CompanionUi::CompanionUi(const ThemeTokens& theme) : default_style_(ImGui::GetStyle()) {
    SetTheme(theme);
}

void CompanionUi::SetTheme(const ThemeTokens& theme) {
    theme_ = theme;
    pages_.SetTheme(theme);
    base_style_ = default_style_;
    ApplyThemeColors(base_style_, theme_);
    ApplyStyleMetrics(base_style_);
    const int percent = applied_scale_percent_ != 0 ? applied_scale_percent_ : 100;
    applied_scale_percent_ = 0;
    ApplyScale(percent);
}

void CompanionUi::ApplyScale(int percent) {
    if (percent == applied_scale_percent_) return;
    applied_scale_percent_ = percent;
    const float scale = static_cast<float>(percent) / 100.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style = base_style_;
    style.ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
}

void CompanionUi::Draw(AppState& state) {
    ApplyScale(state.ui_scale_percent);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("G-Diffuser Companion", nullptr, flags);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float footer_height = ImGui::GetTextLineHeight() + 2.0f * style.WindowPadding.y;
    const float log_height = ImGui::GetContentRegionAvail().y * 0.30f;
    const float reserved = log_height + footer_height + 2.0f * style.ItemSpacing.y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("body", ImVec2(0.0f, -reserved), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    ImGui::BeginChild("nav", ImVec2(ImGui::GetFontSize() * 9.0f, 0.0f), ImGuiChildFlags_Borders);
    DrawNavigation(state);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("work", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    DrawWorkArea(state);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("log", ImVec2(0.0f, log_height), ImGuiChildFlags_Borders);
    DrawLog(state);
    ImGui::EndChild();

    ImGui::BeginChild("status", ImVec2(0.0f, footer_height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    DrawStatusBar(state);
    ImGui::EndChild();
    ImGui::End();
}

void CompanionUi::DrawNavigation(AppState& state) {
    ImGui::TextDisabled("Sections");
    ImGui::Separator();
    for (const SectionEntry& entry : kSections) {
        if (ImGui::Selectable(entry.label, state.section == entry.section)) {
            state.section = entry.section;
        }
    }
}

void CompanionUi::DrawWorkArea(AppState& state) {
    const SectionEntry& current = FindSection(state.section);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted(current.label);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    if (state.section == Section::Settings) {
        DrawSettings(state);
    } else {
        pages_.Draw(state.section, state);
    }
}

void CompanionUi::DrawSettings(AppState& state) {
    const ImVec4 secondary = ToVec4(theme_.text_secondary);

    DrawThemePicker(state);

    DrawInstallation(state);

    ImGui::SeparatorText("Worker");
    ImGui::Text("Status: %s", WorkerLabel(state.worker.state));
    if (!state.worker.detail.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, secondary);
        ImGui::TextWrapped("%s", state.worker.detail.c_str());
        ImGui::PopStyleColor();
    }
    if (!state.worker.version.empty()) {
        ImGui::Text("Version: %s, protocol %s", state.worker.version.c_str(),
                    std::to_string(kProtocolVersion).c_str());
    }
    if (!state.worker.ops.empty()) {
        std::string ops;
        for (size_t i = 0; i < state.worker.ops.size(); ++i) {
            if (i > 0) ops += ", ";
            ops += state.worker.ops[i];
        }
        ImGui::Text("Operations: %s", ops.c_str());
    }

    ImGui::SeparatorText("Interface scale");
    for (size_t i = 0; i < sizeof(kScalePercents) / sizeof(kScalePercents[0]); ++i) {
        const int percent = kScalePercents[i];
        if (i > 0) ImGui::SameLine();
        const std::string label = std::to_string(percent) + "%";
        if (ImGui::RadioButton(label.c_str(), state.ui_scale_percent == percent)) {
            state.ui_scale_percent = percent;
        }
    }

    ImGui::SeparatorText("Accessibility");
    ImGui::PushStyleColor(ImGuiCol_Text, secondary);
    ImGui::TextWrapped(
        "Keyboard: Tab and Shift+Tab move between controls, and the arrow keys move within a list. "
        "This window does not provide screen reader support.");
    ImGui::PopStyleColor();
}

void CompanionUi::DrawInstallation(AppState& state) {
    ImGui::SeparatorText("Installation");
    ImGui::Text("Status: %s", InstallLabel(state.installation));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.text_secondary));
    ImGui::TextWrapped("%s", state.installation_detail.c_str());
    ImGui::PopStyleColor();

    // The field follows the found folder until the user types; a new result refills it.
    if (install_path_source_ != state.installation_dir) {
        install_path_source_ = state.installation_dir;
        const size_t length = std::min(install_path_source_.size(), sizeof(install_path_) - 1);
        std::memcpy(install_path_, install_path_source_.data(), length);
        install_path_[length] = '\0';
    }
    ImGui::TextUnformatted("Game folder, or the path to the game executable:");
    ImGui::SetNextItemWidth(-1.0f);
    const bool entered = ImGui::InputText("##install_path", install_path_, sizeof(install_path_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Use this path") || entered) {
        state.install_choice = install_path_;
        if (state.install_choice.empty()) state.installation_error = "Type a path first, or use Browse.";
    }
    if (state.browse_available) {
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) state.install_browse = true;
#if defined(_WIN32)
        ImGui::SetItemTooltip("Pick G-Diffuser.exe in the game's folder.");
#else
        ImGui::SetItemTooltip("Pick the folder that holds the game.");
#endif
    }
    if (state.installation_chosen) {
        ImGui::SameLine();
        if (ImGui::Button("Find automatically")) state.install_reset = true;
        ImGui::SetItemTooltip("Forget the chosen folder and look beside the Companion and in the folder above it.");
    }
    if (!state.installation_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.status_error));
        ImGui::TextWrapped("%s", state.installation_error.c_str());
        ImGui::PopStyleColor();
    }
}

void CompanionUi::DrawThemePicker(AppState& state) {
    ImGui::SeparatorText("Theme");
    if (state.themes.empty()) return;
    const size_t active = state.theme_index < state.themes.size() ? state.theme_index : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo("##theme", state.themes[active].display_name.c_str())) {
        for (size_t i = 0; i < state.themes.size(); ++i) {
            const ThemeTokens& theme = state.themes[i];
            const bool picked = i == active;
            ImGui::PushID(static_cast<int>(i));
            const std::string label =
                theme.id.rfind("user.", 0) == 0 ? theme.display_name : theme.display_name + " (built-in)";
            if (ImGui::Selectable(label.c_str(), picked) && !picked) {
                state.theme_request = static_cast<int>(i);
            }
            if (picked) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) state.theme_rescan = true;
    ImGui::SetItemTooltip("Read the themes folder again after adding, renaming or deleting a theme.");

    const ThemeTokens& current = state.themes[active];
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.text_secondary));
    if (!current.description.empty()) ImGui::TextWrapped("%s", current.description.c_str());
    ImGui::TextWrapped(
        "Add a theme: make a folder in the themes folder with a theme.json and an optional backdrop image. "
        "The theme's id is user.<folder name>.");
    ImGui::PopStyleColor();

    // Read-only text field, so the path can be selected and copied.
    std::string path = state.themes_dir;
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    ImGui::InputText("##themes_dir", path.data(), path.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) state.theme_open_folder = true;
}

void CompanionUi::DrawLog(const AppState& state) {
    ImGui::TextUnformatted("Log");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy log")) {
        std::string text;
        for (const LogEntry& entry : state.log.entries()) {
            text += entry.text;
            text += '\n';
        }
        ImGui::SetClipboardText(text.c_str());
    }
    if (state.log.dropped_entries() > 0) {
        ImGui::TextColored(ToVec4(theme_.text_secondary),
                           "%zu older entries were removed to stay within the 256 KiB log limit.",
                           state.log.dropped_entries());
    }
    ImGui::Separator();

    for (const LogEntry& entry : state.log.entries()) {
        ImVec4 color = ToVec4(theme_.text);
        if (entry.level == LogLevel::Warning) {
            color = ToVec4(theme_.status_warning);
        } else if (entry.level == LogLevel::Error) {
            color = ToVec4(theme_.status_error);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", entry.text.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
}

void CompanionUi::DrawStatusBar(const AppState& state) {
    ImGui::Text("Installation: %s", InstallLabel(state.installation));
    ImGui::SameLine(0.0f, 24.0f);
    ImVec4 worker_color = ToVec4(theme_.text);
    if (state.worker.state == WorkerState::Ready) worker_color = ToVec4(theme_.status_ok);
    if (state.worker.state == WorkerState::Failed) worker_color = ToVec4(theme_.status_error);
    ImGui::PushStyleColor(ImGuiCol_Text, worker_color);
    ImGui::Text("Worker: %s", WorkerLabel(state.worker.state));
    ImGui::PopStyleColor();
}

}  // namespace gdx_companion
