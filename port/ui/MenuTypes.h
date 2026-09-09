// port/ui/MenuTypes.h — declarative menu registry for the G-Diffuser menu.
//
// PROVENANCE
// ----------
// Ported from HarbourMasters/Lighthouse, src/port/UI/MenuTypes.h on branch `develop`, published
// under CC0 1.0 Universal (Creative Commons Public Domain Dedication) — copy, modify and
// redistribute freely, no attribution required. Same upstream libultraship as this fork; the
// registry is pure port-side data and touches nothing below the public ImGui + CVar API. See
// port/ui/UIWidgets.hpp for the widget library this file is the registry layer for.
//
// THE MODEL
// ---------
//   MainMenuEntry (a header tab: "Settings")
//     └ SidebarEntry (a sidebar page: "Graphics"), columnCount 1..3
//         └ WidgetInfo[column] — one entry per individual control
//
// The menu's contents are DATA so that search can walk the same list the drawer does: a control
// cannot be visible-but-unsearchable, and one entry here puts it on its page and in search at once.
//
// ADAPTATIONS vs Lighthouse
// -------------------------
//  1. Namespaced. Upstream declares WidgetInfo/SidebarEntry/DisableOption/... at global scope;
//     everything here lives in namespace GdxUI (same reasoning as UIWidgets ADAPTATION #9).
//  2. `Options()` is a set of type-safe overloads instead of upstream's `Options(OptionsVariant)` +
//     `switch (type)` + `std::get<T>`, which compiles for any Options struct and only discovers a
//     type/widget mismatch at RUNTIME, as a std::bad_variant_access thrown out of the draw loop
//     (Menu.cpp:547 catches it and asserts). Here a mismatch is a compile error, and MenuDrawItem
//     needs no try/catch.
//  3. `comboItems` (ordered std::vector<const char*>) added. Upstream drives every combobox from
//     UIWidgets::ComboboxOptions::comboMap, a std::unordered_map whose iteration order is
//     unspecified. Every dropdown in this menu is index-ordered (MSAA, texture filter, z-fighting,
//     button outlines, audio backend), so the registry carries an ordered list and MenuDrawItem
//     uses UIWidgets' vector overload.
//  4. WIDGET_WINDOW_BUTTON dropped. Upstream's implementation calls UIWidgets::WindowButton, which
//     toggles a GuiWindow through its visibility CVar — and a bare CVarSetInteger is a NO-OP for an
//     already-constructed window in this fork (it reads mIsVisible each frame and samples the CVar
//     only at construction). The tool pages stay WIDGET_CUSTOM over DrawToolWindowPage(), which
//     uses ToggleVisibility() and additionally embeds the window's DrawElement() inline.
//  5. WIDGET_AUDIO_BACKEND / WIDGET_VIDEO_BACKEND dropped: both call
//     Ship::Context::GetRawInstance(), which does not exist in this fork (UIWidgets ADAPTATION #3),
//     and this port picks its audio backend through its own gEnhancements.Audio.Backend CVar.
//
// LICENSE: CC0 1.0 Universal. Original: https://github.com/HarbourMasters/Lighthouse

#ifndef GDX_MENU_TYPES_H
#define GDX_MENU_TYPES_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "UIWidgets.hpp"

namespace GdxUI {

// Named disable reasons. A greyed control that does not say WHY is a support burden, so each entry
// is a reason GdxMenu can name in a "This setting is disabled because:" tooltip that lists several
// at once. Every evaluation runs exactly once per frame (see DisabledInfo). Some are used as HIDE
// conditions (WidgetInfo::hideWhen); the enum is shared so those are amortised too.
//
// Keep this to reasons ACTUALLY WIRED to a control: an unused entry makes the enum look richer than
// the menu behaves.
enum DisableOption {
    // Renderer / window state
    DISABLE_FOR_NO_WINDOW, // the Ship::Window is not up yet (fullscreen has nothing to toggle)

    // Visual-enhancement dependencies
    DISABLE_FOR_WIDESCREEN_OFF,        // the 2D widescreen layout needs 16:9 rendering on
    DISABLE_FOR_WIDESCREEN_UI_OFF,     // split-screen HUD anchoring is a subset of the 2D layout
    DISABLE_FOR_INTERPOLATION_ON,      // interpolation owns pacing while it is on
    DISABLE_FOR_INTERPOLATION_OFF,     // (hide condition) interpolation's own sub-controls
    DISABLE_FOR_INTERP_OVERLAY_OFF,    // (hide condition) the sub-frame stat line
    DISABLE_FOR_MATCH_REFRESH_RATE_ON, // the fixed target FPS is unused while the target follows the display

    // Audio
    DISABLE_FOR_LOW_PASS_FILTER_OFF, // the cutoff slider is inert while the filter is disabled

    // Game state
    DISABLE_FOR_RACE_IN_PROGRESS, // mutating ghost state must not race the game fiber

    // Input
    DISABLE_FOR_COURSE_EDIT_MOUSE_OFF, // Course Edit mouse sub-controls are inert while the master is off

    DISABLE_OPTION_COUNT
};

struct WidgetInfo;
struct DisabledInfo;

using WidgetFunc = std::function<void(WidgetInfo&)>;
using DisableEvalFunc = std::function<bool(DisabledInfo&)>;
using DisableVec = std::vector<DisableOption>;

// Holds a reason's user-visible text and the evaluation that decides whether it currently applies.
// GdxMenu::DrawElement() runs every evaluation once at the top of the frame and caches the answer
// in `active`; MenuDrawItem only reads the cache, so a condition shared by several widgets (e.g.
// "is interpolation on?") costs one CVar read per frame rather than one per widget.
// `value` is scratch for evaluations that want to carry a number alongside the bool.
struct DisabledInfo {
    DisableEvalFunc evaluation;
    const char* reason = "";
    bool active = false;
    int32_t value = 0;
};

// The CVAR_* variants read and write a CVar themselves (via the UIWidgets CVar* wrappers); the
// plain variants operate on WidgetInfo::valuePointer and leave the write to the widget's
// `callback`, which is what every non-CVar control here needs (live window state, a derived
// boolean, a remembered-value pair, ...).
enum WidgetType {
    WIDGET_SEPARATOR,      // ImGui::Separator()
    WIDGET_SEPARATOR_TEXT, // ImGui::SeparatorText(name)
    WIDGET_TEXT,           // ImGui::TextWrapped(name), optionally coloured via TextOptions::color
    WIDGET_TEXT_DISABLED,  // ImGui::TextDisabled(name) — the unwrapped greyed note used throughout
    WIDGET_COMING_SOON,    // "<name>  -  Coming soon"
    WIDGET_CHECKBOX,
    WIDGET_CVAR_CHECKBOX,
    WIDGET_COMBOBOX,
    WIDGET_CVAR_COMBOBOX,
    WIDGET_SLIDER_INT,
    WIDGET_CVAR_SLIDER_INT,
    WIDGET_SLIDER_FLOAT,
    WIDGET_CVAR_SLIDER_FLOAT,
    WIDGET_BUTTON,
    WIDGET_CVAR_RADIO_BUTTON,
    WIDGET_CUSTOM, // customFunction draws whatever it likes (tables, status blocks, popups)
};

enum SectionColumns {
    SECTION_COLUMN_1,
    SECTION_COLUMN_2,
    SECTION_COLUMN_3,
};

// Everything needed to DRAW and to SEARCH one control.
//
//  name            the visible label, and the primary search key
//  cVar            the CVar backing the value (CVAR_* widget types only)
//  options         the Options struct MATCHING `type`; set through the Options() overloads
//  valuePointer    where a non-CVar widget reads/writes (preFunc typically refreshes it first)
//  comboItems      ordered dropdown rows (see ADAPTATION #3)
//  radioValue      the value a WIDGET_CVAR_RADIO_BUTTON writes when picked
//  callback        run after the widget reports a change — this is where side effects live
//  preFunc         run before drawing: refresh valuePointer, set isHidden, compute a note
//  postFunc        run after drawing: react to state the widget itself does not report
//  disableWhen     reasons that grey this control out (evaluated once per frame, see DisabledInfo)
//  hideWhen        reasons that remove it from the page entirely
//  activeDisables  scratch: the subset of disableWhen that is active this frame, used for the tooltip
//  searchTerms     extra keywords the search box matches, beyond name + tooltip
//  isHidden        set by preFunc (or hideWhen) to skip this control this frame
struct WidgetInfo {
    std::string name;
    const char* cVar = "";
    WidgetType type = WIDGET_TEXT;
    std::shared_ptr<UIWidgets::WidgetOptions> options;
    std::variant<bool*, int32_t*, float*> valuePointer = static_cast<bool*>(nullptr);
    std::vector<const char*> comboItems = {};
    int32_t radioValue = 0;
    WidgetFunc callback = nullptr;
    WidgetFunc preFunc = nullptr;
    WidgetFunc postFunc = nullptr;
    WidgetFunc customFunction = nullptr;
    DisableVec disableWhen = {};
    DisableVec hideWhen = {};
    DisableVec activeDisables = {};
    const char* note = "";
    std::string searchTerms = "";
    bool modifiedMarker = false;
    bool isHidden = false;
    bool sameLine = false;
    bool hideInSearch = false;

    // Type-safe Options() overloads (ADAPTATION #2): overload resolution picks the exact-match
    // derived overload over the WidgetOptions base one, so MenuDrawItem's static_pointer_cast is
    // sound by construction.
    WidgetInfo& Options(const UIWidgets::CheckboxOptions& options_) {
        options = std::make_shared<UIWidgets::CheckboxOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::ComboboxOptions& options_) {
        options = std::make_shared<UIWidgets::ComboboxOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::IntSliderOptions& options_) {
        options = std::make_shared<UIWidgets::IntSliderOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::FloatSliderOptions& options_) {
        options = std::make_shared<UIWidgets::FloatSliderOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::ButtonOptions& options_) {
        options = std::make_shared<UIWidgets::ButtonOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::RadioButtonsOptions& options_) {
        options = std::make_shared<UIWidgets::RadioButtonsOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::TextOptions& options_) {
        options = std::make_shared<UIWidgets::TextOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::WidgetOptions& options_) {
        options = std::make_shared<UIWidgets::WidgetOptions>(options_);
        return *this;
    }

    WidgetInfo& CVar(const char* cVar_) {
        cVar = cVar_;
        return *this;
    }
    WidgetInfo& ComboItems(std::vector<const char*> comboItems_) {
        comboItems = std::move(comboItems_);
        return *this;
    }
    WidgetInfo& RadioValue(int32_t radioValue_) {
        radioValue = radioValue_;
        return *this;
    }
    WidgetInfo& Callback(WidgetFunc callback_) {
        callback = std::move(callback_);
        return *this;
    }
    WidgetInfo& PreFunc(WidgetFunc preFunc_) {
        preFunc = std::move(preFunc_);
        return *this;
    }
    WidgetInfo& PostFunc(WidgetFunc postFunc_) {
        postFunc = std::move(postFunc_);
        return *this;
    }
    WidgetInfo& CustomFunction(WidgetFunc customFunction_) {
        customFunction = std::move(customFunction_);
        return *this;
    }
    WidgetInfo& ValuePointer(std::variant<bool*, int32_t*, float*> valuePointer_) {
        valuePointer = valuePointer_;
        return *this;
    }
    WidgetInfo& DisableWhen(DisableVec disableWhen_) {
        disableWhen = std::move(disableWhen_);
        return *this;
    }
    WidgetInfo& HideWhen(DisableVec hideWhen_) {
        hideWhen = std::move(hideWhen_);
        return *this;
    }
    WidgetInfo& Note(const char* note_) {
        note = note_;
        return *this;
    }
    WidgetInfo& SearchTerms(std::string searchTerms_) {
        searchTerms = std::move(searchTerms_);
        return *this;
    }
    WidgetInfo& ModifiedMarker(bool modifiedMarker_ = true) {
        modifiedMarker = modifiedMarker_;
        return *this;
    }
    WidgetInfo& SameLine(bool sameLine_ = true) {
        sameLine = sameLine_;
        return *this;
    }
    WidgetInfo& HideInSearch(bool hideInSearch_ = true) {
        hideInSearch = hideInSearch_;
        return *this;
    }

    // Per-frame scratch reset. `options` is shared and persists across frames, so a disable applied
    // last frame would otherwise stick after its reason cleared.
    void ResetDisables() {
        isHidden = false;
        if (options != nullptr) {
            options->disabled = false;
            options->disabledTooltip = "";
        }
        activeDisables.clear();
    }
};

// One sidebar page. `columnCount` (1..3) and `columnWidgets` do not have to agree: a page may
// declare 2 columns but fill only the first, which is how a dense page is split without reordering
// its registration. `searchTerms` are page-level keywords, so a query like "netplay" surfaces the
// page even when no individual control mentions the word.
struct SidebarEntry {
    uint32_t columnCount = 1;
    std::vector<std::vector<WidgetInfo>> columnWidgets = {};
    std::string searchTerms = "";
};

// One header tab. `sidebarCvar` persists which sidebar page was last viewed inside this tab, BY
// NAME, so reordering pages cannot re-point a stored selection at a different one.
struct MainMenuEntry {
    std::string label;
    const char* sidebarCvar = "";
    std::unordered_map<std::string, SidebarEntry> sidebars = {};
    std::vector<std::string> sidebarOrder = {};
};

} // namespace GdxUI

#endif /* GDX_MENU_TYPES_H */
