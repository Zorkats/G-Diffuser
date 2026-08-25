#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ship/window/gui/GuiWindow.h"
#include "ui/MenuTypes.h" // GdxUI::{MainMenuEntry, SidebarEntry, WidgetInfo, DisabledInfo, ...}

/**
 * Full-screen settings menu, modelled on Ship of Harkinian's 9.2.3 menu shell.
 *
 * The menu's contents are DATA, not code: port/gdx_menu_registry.cpp builds the tree
 *
 *     MainMenuEntry (header tab) -> SidebarEntry (sidebar page) -> WidgetInfo[column]
 *
 * once at InitElement(), and this class only walks it. That is what makes widget-level search
 * possible — DrawSearchResults() iterates the exact registry MenuDrawItem() draws from, so a query
 * can name an INDIVIDUAL control, say which page it lives on, and jump the menu to it.
 *
 * Only the CONTENT model comes from Lighthouse. The shell is the port's own: the gamepad-nav
 * tuning, L1/R1 tab cycling, quit modal, embedded tool windows and focus seeding below have no
 * Lighthouse equivalent, and its window chrome (popout window, theme picker, its own header/sidebar
 * hit-testing, Fast3dWindow-specific backend pickers) is deliberately not ported.
 */
class GdxMenu final : public Ship::GuiWindow {
  public:
    GdxMenu();

    void Draw() override;
    void DrawElement() override;

  protected:
    void InitElement() override;
    void UpdateElement() override;

  private:
    // ── Registry construction (port/gdx_menu_registry.cpp) ───────────────────────────────────
    // Called once from InitElement(). In its own translation unit because it is a long flat
    // declaration of every control, with no logic worth reading alongside the shell.
    void RegisterMenu();
    void RegisterDisableReasons();

    void AddMenuEntry(const std::string& label, const char* sidebarCvar);
    void AddSidebarEntry(const std::string& section, const std::string& sidebar, uint32_t columnCount,
                         const std::string& searchTerms = "");
    void AddWidget(const std::string& section, const std::string& sidebar, GdxUI::SectionColumns column,
                   GdxUI::WidgetInfo widget);

    // ── Registry drawing ────────────────────────────────────────────────────────────────────
    void MenuDrawItem(GdxUI::WidgetInfo& widget);
    void DrawHeader();
    void DrawSidebar();
    void DrawCurrentPage();
    uint32_t DrawSearchResults();
    void DrawQuitModal();

    // ── Custom (WIDGET_CUSTOM) blocks ───────────────────────────────────────────────────────
    // Everything a plain widget cannot express: tables, live status read-outs, modals, and the
    // table-driven developer-gate surface. Each is registered as a single WIDGET_CUSTOM entry.
    void DrawAudioStatus();
    void DrawGhostIo();
    void DrawXCupSeed();
    void DrawTexturePacks();
    void DrawScripts();
    void DrawAssetDump();
    void DrawDdSave();
    void DrawDevToolButtons();
    // Developer gates (port/gdx_dev_gates.{h,c}). Stays a custom block: the per-gate tooltip is a
    // four-argument runtime format and the value is a gdx_dev_gate() call, not a CVar read.
    void DrawDevGates();
    void DrawInterpStats();
    void DrawToolWindowPage(const char* name, const char* description);
    void DrawAboutMenu();

    // ── Section / sidebar selection ─────────────────────────────────────────────────────────
    // Both are addressed BY NAME and persisted as strings, so reordering pages cannot make a
    // stored selection point at a different page.
    void SelectSection(const std::string& section);
    void SelectSidebar(const std::string& sidebar);
    const std::string& ActiveSidebar();
    GdxUI::SidebarEntry* ActiveSidebarEntry();

    // Restore the io fields the menu overrides for gamepad nav. Safe to call when nothing was
    // applied (no-op).
    void RestoreNavTuning();

    // ── Registry ────────────────────────────────────────────────────────────────────────────
    std::unordered_map<std::string, GdxUI::MainMenuEntry> mMenuEntries;
    std::vector<std::string> mMenuOrder;
    // Indexed by GdxUI::DisableOption. A dense vector rather than upstream's unordered_map: the
    // enum is contiguous and every entry is touched every frame.
    std::vector<GdxUI::DisabledInfo> mDisabledInfo;
    // Scratch for the "This setting is disabled because:" tooltip. Must outlive the widget draw,
    // because UIWidgets' Options structs hold a borrowed const char*.
    std::string mDisabledTooltip;
    bool mRegistered = false;

    std::string mActiveSection = "Settings";

    // ── Search navigation ───────────────────────────────────────────────────────────────────
    // Set when a search result's location button is clicked; consumed at the top of the next
    // DrawElement(), which switches section + sidebar, clears the query, and arms the highlight.
    bool mNavigateRequested = false;
    std::string mNavigateSection;
    std::string mNavigateSidebar;
    std::string mNavigateWidget;
    // The control to outline after a navigation, and until when (seconds, ImGui::GetTime clock).
    std::string mHighlightWidget;
    double mHighlightUntil = 0.0;
    bool mHighlightScrollPending = false;
    // Screen-space Y of the highlighted control, recorded by MenuDrawItem and consumed one level up
    // in DrawElement. Screen space rather than ImGui::SetScrollHereY() because a multi-column page
    // draws its widgets inside non-scrolling child windows, where SetScrollHereY has nothing to
    // scroll — the scrollbar belongs to the content pane that owns those children.
    float mHighlightScreenY = 0.0f;

    char mSearch[128] = {};
    bool mOpenQuitModal = false;
    int mLastLowPassHz = 15000;

    // ── Backing storage for the non-CVar widgets ────────────────────────────────────────────
    // Controls whose truth is not a stored setting (live window state, a derived boolean, a
    // value-remembering pair, an index that is not the CVar value). Each is refreshed by its
    // widget's preFunc and written back by its callback.
    bool mFullscreen = false;         // Ship::Window::IsFullscreen()
    int32_t mMsaaIndex = 0;           // index into kMsaaValues; gMSAAValue stores the SAMPLE COUNT
    int32_t mAudioBackendIndex = 0;   // reduced list index on non-Windows hosts
    bool mLowPassFilterOn = true;     // gEnhancements.Audio.LowPassHz > 0
    int32_t mLowPassCutoffHz = 15000; // shown while the filter is off, when the CVar holds 0
    bool mInterpMatchRefresh = true;  // gEnhancements.Graphics.InterpTargetMode == 0 (inverted)
    bool mInputViewerVisible = false; // live GuiWindow visibility
    bool mFpsCounterVisible = false;  // live GuiWindow visibility

    // Gamepad-navigation state (only meaningful while gControlNav is on).
    bool mFocusSidebar = false;    // one-shot: land nav focus on the active sidebar page next draw
    bool mMenuWasVisible = false;  // tracks open transitions so focus is seeded on each open
    bool mNavTuningApplied = false;     // io fields below currently overridden by the menu
    float mSavedKeyRepeatDelay = 0.0f;  // pre-override io.KeyRepeatDelay
    float mSavedKeyRepeatRate = 0.0f;   // pre-override io.KeyRepeatRate
    bool mSavedNavCursorAlways = false; // pre-override io.ConfigNavCursorVisibleAlways
    // End-of-frame snapshot: was there an active widget or open popup for ImGui's B/cancel to
    // consume? Read on the next frame, because by the time this menu draws, ImGui has already
    // cleared whatever the press cancelled.
    bool mNavCancelHadTarget = false;
};
