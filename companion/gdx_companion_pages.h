// Copyright (c) Zorkats
//
// The workspace pages: Music, Textures, Models, Tracks and Advanced. Each page keeps its own form, checks
// it every frame with the host modules, shows each problem under its field, and keeps Build disabled
// while a problem remains. Dialogs, worker jobs and audio go through AppState requests that the main
// loop carries out. Called on the UI thread between ImGui::NewFrame and ImGui::Render.
#pragma once

#include <memory>

#include "gdx_companion_state.h"
#include "gdx_companion_theme.h"

namespace gdx_companion {

class WorkspacePages {
public:
    WorkspacePages();
    ~WorkspacePages();
    WorkspacePages(const WorkspacePages&) = delete;
    WorkspacePages& operator=(const WorkspacePages&) = delete;

    void SetTheme(const ThemeTokens& theme);

    // Draws the page for section. Settings is drawn by CompanionUi, not here.
    void Draw(Section section, AppState& state);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace gdx_companion
