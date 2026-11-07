// Copyright (c) Zorkats
//
// Draws the Companion window. Called on the UI thread between ImGui::NewFrame and ImGui::Render.
#pragma once

#include <string>

#include "imgui.h"
#include "gdx_companion_pages.h"
#include "gdx_companion_state.h"
#include "gdx_companion_theme.h"

namespace gdx_companion {

class CompanionUi {
public:
    // Call after ImGui::CreateContext. The base style is captured here and scaled from it, so repeated
    // scale changes do not compound.
    explicit CompanionUi(const ThemeTokens& theme);

    // Recolours every control from theme. Safe between frames.
    void SetTheme(const ThemeTokens& theme);

    void Draw(AppState& state);

private:
    void ApplyScale(int percent);
    void DrawNavigation(AppState& state);
    void DrawWorkArea(AppState& state);
    void DrawSettings(AppState& state);
    void DrawThemePicker(AppState& state);
    void DrawInstallation(AppState& state);
    void DrawLog(const AppState& state);
    void DrawStatusBar(const AppState& state);

    ThemeTokens theme_;
    WorkspacePages pages_;
    ImGuiStyle default_style_;  // ImGui's own style, captured once
    ImGuiStyle base_style_;     // default_style_ with the theme and metrics, before scaling
    int applied_scale_percent_ = 0;
    char install_path_[2048] = {};      // the editable installation path
    std::string install_path_source_;   // the state value install_path_ was last filled from
};

}  // namespace gdx_companion
