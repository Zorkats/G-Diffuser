#pragma once

#include "gdx_hackmods.h"
#include "ship/window/gui/GuiWindow.h"

#include <string>
#include <vector>

/**
 * @brief ROM Hacks — pick which ROM-hack archive the next boot uses.
 *
 * Deliberately separate from the Workshop pack list, the same split Lighthouse ships: a texture
 * pack decorates the game, a hack replaces it. The two differ in what they can break and in how
 * many may be on at once, so they do not belong in one list.
 *
 * The window only edits a selection. Mounting and the per-hack save file are both latched at
 * boot (gdx_hackmods.h), so a change here takes effect on the next launch and the UI says so
 * rather than pretending otherwise.
 */
class GdxHackModsWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    ~GdxHackModsWindow() override = default;

  protected:
    void InitElement() override;
    void UpdateElement() override;
    void DrawElement() override;

  private:
    void Refresh();
    // Builds a hack archive from a patched ROM. Separate from selection: it produces the file the
    // list above picks from, and it is the only part of this window that does real work.
    void DrawBuildSection();

    std::vector<GdxHackModEntry> mEntries;
    std::string mSelected;   // selection as of the last refresh
    std::string mActiveBoot; // what is actually mounted this session
    bool mRefreshed = false;
    char mStatus[512] = {};

    // Build-from-ROM inputs. Kept as fixed buffers because ImGui::InputText writes into them.
    char mRomInput[1024] = {};
    char mNameInput[64] = {};
    char mBuildStatus[1024] = {};
    bool mBuildOk = false; // colours the status line; only meaningful when mBuildStatus is set
};
