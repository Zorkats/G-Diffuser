#pragma once

#include "gdx_content_import.h"
#include "gdx_content_io.h"
#include "ship/window/gui/GuiWindow.h"

/**
 * @brief Content Library — the unified content-IO surface (F12 shell).
 *
 * E1 lists the disk's tracks (CRSD/CRSE) and machines (CARD) with per-entry export to
 * exports/<name>.gdxc. E2 lists every .gdxc under exports/ with full per-file validation and
 * installs
 * via the disk-thread op slot (gdx_content_import.c). E3 adds per-track Edit-Cup
 * register/unregister with slot occupancy and two-click confirms (op slot 31), E4 adds cup
 * bundles: "Export Edit Cup as bundle" writes exports/edit-cup.gdxc and bundle imports install
 * via op slot 32.
 */
class GdxContentWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

  protected:
    void InitElement() override;
    void UpdateElement() override;
    void DrawElement() override;

  private:
    int RefreshLibrary();
    void RefreshImports();
    void RefreshCupSlots();
    void DrawCupPicker(bool busy, bool inGameplay);
    void DrawImportSection(bool busy, bool inGameplay);

    GdxContentEntry mEntries[GDX_CONTENT_MAX_ENTRIES] = {};
    int mEntryCount = 0;
    GdxContentImportEntry mImports[GDX_CONTENT_IMPORT_MAX] = {};
    int mImportCount = 0;
    int mImportArmed = -1;      // row index whose overwrite/twin confirmation is primed
    bool mImportWaiting = false; // an install was enqueued; poll for its completion
    char mCupSlots[GDX_CONTENT_CUP_SLOT_COUNT][GDX_CONTENT_CUP_NAME_MAX + 1] = {};
    int mCupPickerTrack = -1;    // library row whose Edit-Cup slot picker is open
    int mCupArmedSlot = -1;      // slot primed for an overwrite confirm
    int mCupArmedUnregister = -1; // slot primed for an unregister confirm
    bool mCupWaiting = false;    // a register/unregister was enqueued; poll for its completion
    // Startup auto-retry: the disk filesystem mounts after the window initializes, so a failed
    // first listing (and the stale error it prints) retries on a timer instead of waiting for
    // the user to find the Refresh button.
    bool mAutoRefreshDone = false;
    double mNextAutoRefresh = 0.0;
    double mAutoRefreshDeadline = 0.0;
    char mStatus[512] = {};
};
