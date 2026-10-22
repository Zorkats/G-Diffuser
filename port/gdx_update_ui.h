#pragma once

// In-game updater UI over gdx_update.h: the launch check, the "update available" notice, and the Updates
// block of the About page.

#include "ship/window/gui/GuiWindow.h"

// main.cpp, once the GUI exists: removes what the last update left behind and, when
// gEnhancements.Updates.AutoCheck is on (default), starts the launch check.
void GdxUpdateBoot();

// The About page's Updates block.
void GdxUpdateDrawAboutSection();

// Small corner notice shown once per new version while the menu is closed. Later dismisses it until the
// next version.
class GdxUpdateNotice final : public Ship::GuiWindow {
  public:
    GdxUpdateNotice();

    void Draw() override;
    void DrawElement() override;

  protected:
    void InitElement() override;
    void UpdateElement() override;
};
