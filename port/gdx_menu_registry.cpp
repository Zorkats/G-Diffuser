// port/gdx_menu_registry.cpp — the G-Diffuser menu, declared as data.
//
// This file is the entire CONTENT of the menu: which header tabs exist, which pages each tab has,
// how many columns a page is laid out in, and every individual control on it. The shell that walks
// this tree (window, sidebar, search, MenuDrawItem) lives in port/gdx_menu.cpp; the data model
// lives in port/ui/MenuTypes.h. Adding a control here puts it on its page AND in search, with its
// tooltip and disable reasons, with no edit to the shell.
//
// HOW TO READ AN ENTRY
// --------------------
//     AddWidget("Settings", "Graphics", GdxUI::SECTION_COLUMN_1,
//               GdxUI::WidgetInfo{ .name  = "VSync",
//                                  .cVar  = "gVsyncEnabled",
//                                  .type  = GdxUI::WIDGET_CVAR_CHECKBOX }
//                   .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip("..."))
//                   .DisableWhen({ GdxUI::DISABLE_FOR_NO_WINDOW }));
//
//   .Options(...)      the UIWidgets Options struct for this widget type — tooltip, defaults,
//                      range, label position. Type-checked at compile time (MenuTypes.h ADAPTATION #2).
//   .DisableWhen(...)  named reasons; a greyed control then STATES why, and can state several.
//   .HideWhen(...)     same evaluations, but the control disappears instead of greying out.
//   .PreFunc/.Callback for the controls whose truth is not a stored CVar (live window state, a
//                      derived boolean, an index that is not the CVar value) and for live side
//                      effects.
//   .Note("(restart)") the greyed suffix drawn after the control.
//   .ModifiedMarker()  the "changed from stock" asterisk.
//   .SearchTerms(...)  extra keywords, beyond the label and tooltip.
//
// Several tooltips are load-bearing documentation — the Frame Interpolation one names both known
// artifacts, the shader-cache one explains the stall it fixes — including their manual line breaks.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "gdx_menu.h"
#include "gdx_menu_internal.h"

#include <imgui.h>

#include "ship/window/Window.h" // SetResolutionMultiplier / SetMsaaLevel / IsFullscreen / SetFullscreen
#include "fast/Fast3dWindow.h"  // Fast::Fast3dWindow::SetTextureFilter + Fast::FilteringMode
#include "ship/Context.h"       // Ship::Context::GetAppDirectoryPath

#include "libultraship/bridge/consolevariablebridge.h"
#include "fast/backends/gfx_post_shader_pipeline.h"
#include "fast/backends/gfx_slang_translator.h"

#include "gdx_palette.h" // F2 palette editor: override table + persistence (extern "C" API)
#include "gdx_custom_grid.h" // F3 custom grid: roster string CVar parse/format (extern "C" API)
#include "gdx_achievements.h" // F10 playtime tracker + local achievements

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <spdlog/spdlog.h>
#include <unordered_set>
#include <vector>


using namespace gdxmenu;

// From port/input_bridge.c: nonzero while an on-track race is live. Declared here rather than
// including the bridge header (this TU needs nothing else from it); the signature matches exactly.
// Drives DISABLE_FOR_RACE_IN_PROGRESS.
extern "C" int gdx_input_in_gameplay(void);

// From port/n64_gfx_bridge.cpp: frame-interpolation telemetry for the "subframes last tick" line.
extern "C" int gdx_gfx_interp_last_subframes(void);
extern "C" double gdx_gfx_interp_last_t(void);

// From decomp/src/game/racer.c: the player's last machine-select choice. Drawn read-only as
// custom-grid slot 0, which func_80089800 never overrides.
extern "C" int16_t gPlayerCharacters[4];

namespace {

// gMSAAValue stores the SAMPLE COUNT (1/2/4/8), not a list index, so the dropdown needs an explicit
// index <-> value mapping. Kept next to the labels it pairs with.
const int kMsaaValues[] = { 1, 2, 4, 8 };

// F9 effect-color rows: a packed 0xRRGGBB CVar with -1 = stock. Draws an "Override" checkbox
// plus a ColorEdit3 that stays in sync; unchecking restores -1 (and shows stock in the picker).
void DrawEffectColorOverride(const char* cVar, const char* label, int stockRgb) {
    int packed = CVarGetInteger(cVar, -1);
    bool overrideOn = packed >= 0;
    if (ImGui::Checkbox((std::string("##override") + cVar).c_str(), &overrideOn)) {
        CVarSetInteger(cVar, overrideOn ? stockRgb : -1);
        CVarSave();
        packed = overrideOn ? stockRgb : -1;
    }
    ImGui::SameLine();
    float rgb[3];
    int shown = overrideOn ? packed : stockRgb;
    rgb[0] = ((shown >> 16) & 0xFF) / 255.0f;
    rgb[1] = ((shown >> 8) & 0xFF) / 255.0f;
    rgb[2] = (shown & 0xFF) / 255.0f;
    if (!overrideOn) {
        ImGui::BeginDisabled();
    }
    if (ImGui::ColorEdit3(label, rgb, ImGuiColorEditFlags_NoInputs)) {
        CVarSetInteger(cVar, (static_cast<int>(rgb[0] * 255.0f + 0.5f) << 16) |
                             (static_cast<int>(rgb[1] * 255.0f + 0.5f) << 8) |
                             static_cast<int>(rgb[2] * 255.0f + 0.5f));
        CVarSave();
    }
    if (!overrideOn) {
        ImGui::EndDisabled();
    }
}

// Machine names in sDefaultMachines[] order (racer.c:475-504), shared by the F2 palette
// editor (machine index 0-29 in palette.txt) and the F3 custom-grid character dropdowns.
const char* const kMachineNames[GDX_PALETTE_MACHINE_COUNT] = {
    "Blue Falcon",   "Golden Fox",      "Wild Goose",     "Fire Stingray", "White Cat",
    "Red Gazelle",   "Great Star",      "Iron Tiger",     "Deep Claw",     "Twin Noritta",
    "Super Piranha", "Mighty Hurricane","Little Wyvern",  "Space Angler",  "Green Panther",
    "Black Bull",    "Wild Boar",       "Astro Robin",    "King Meteor",   "Queen Meteor",
    "Wonder Wasp",   "Hyper Speeder",   "Death Anchor",   "Crazy Bear",    "Night Thunder",
    "Big Fang",      "Mighty Typhoon",  "Mad Wolf",       "Sonic Phantom", "Blood Hawk",
};

// One skin row: override checkbox + ColorEdit3 showing the EFFECTIVE color (override if set,
// else stock), same visual idiom as DrawEffectColorOverride. Both mutators apply to the live
// gMachines[] table and rewrite palette.txt immediately (gdx_palette.cpp).
void DrawPaletteColorRow(int machine, int skin) {
    char label[32];
    snprintf(label, sizeof(label), "Color %d", skin + 1);

    bool overrideOn = GdxPalette_HasOverride(machine, skin) != 0;
    if (ImGui::Checkbox((std::string("##paletteOv") + std::to_string(machine) + "_" + std::to_string(skin)).c_str(),
                        &overrideOn)) {
        if (overrideOn) {
            // Start from the currently effective (stock) color so enabling never jumps the hue.
            GdxPalette_SetColor(machine, skin, GdxPalette_GetColor(machine, skin));
        } else {
            GdxPalette_ClearColor(machine, skin);
        }
    }
    ImGui::SameLine();

    int shown = GdxPalette_GetColor(machine, skin);
    float rgb[3] = { ((shown >> 16) & 0xFF) / 255.0f, ((shown >> 8) & 0xFF) / 255.0f, (shown & 0xFF) / 255.0f };
    if (!overrideOn) {
        ImGui::BeginDisabled();
    }
    if (ImGui::ColorEdit3(label, rgb, ImGuiColorEditFlags_NoInputs)) {
        GdxPalette_SetColor(machine, skin, (static_cast<int>(rgb[0] * 255.0f + 0.5f) << 16) |
                                           (static_cast<int>(rgb[1] * 255.0f + 0.5f) << 8) |
                                           static_cast<int>(rgb[2] * 255.0f + 0.5f));
    }
    if (!overrideOn) {
        ImGui::EndDisabled();
    }
}

void DrawMachinePaletteEditor() {
    static int sPaletteMachine = 0;

    ImGui::SetNextItemWidth(240.0f);
    ImGui::Combo("Machine", &sPaletteMachine, kMachineNames, GDX_PALETTE_MACHINE_COUNT);

    for (int skin = 0; skin < GDX_PALETTE_SKIN_COUNT; skin++) {
        DrawPaletteColorRow(sPaletteMachine, skin);
    }

    if (ImGui::Button("Reset this machine")) {
        GdxPalette_ClearMachine(sPaletteMachine);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Removes all 4 color overrides for %s, restoring the stock colors.",
                          kMachineNames[sPaletteMachine]);
    }

    static char sPalettePath[1024] = { 0 };
    if (sPalettePath[0] == '\0') {
        GdxPalette_GetFilePath(sPalettePath, sizeof(sPalettePath));
    }
    ImGui::TextDisabled("Saved to %s", sPalettePath);
}

// F3 custom grid: skin dropdown labels; index 0 = random, 1-4 = the machine's stock colors.
const char* const kGridSkinNames[GDX_CUSTOM_GRID_SKIN_COUNT + 1] = { "Random", "Color 1", "Color 2", "Color 3",
                                                                     "Color 4" };

// F3 custom grid: one row per grid slot, in starting-grid order. Slot 0 is the player's own
// machine (read-only; func_80089800 never overrides it); slots 1-29 take a character (Random
// + all 30 machines -- duplicates and the player's own machine are legal, matching VS mode)
// and a skin (Random + 4 colors). The roster is one compact string CVar parsed by
// gdx_custom_grid.c; the CVar is re-parsed every frame so console edits show up live, and an
// empty string means the feature is off (stock shuffled grid).
void DrawCustomGrid() {
    static const char* sGridCharNames[GDX_CUSTOM_GRID_SLOTS + 1] = { nullptr };

    if (sGridCharNames[0] == nullptr) {
        sGridCharNames[0] = "Random";
        for (int m = 0; m < GDX_CUSTOM_GRID_SLOTS; m++) {
            sGridCharNames[m + 1] = kMachineNames[m];
        }
    }

    int characters[GDX_CUSTOM_GRID_SLOTS];
    int skins[GDX_CUSTOM_GRID_SLOTS];
    bool active = GdxCustomGrid_GetRoster(characters, skins) != 0;
    char roster[GDX_CUSTOM_GRID_STRING_MAX];

    if (ImGui::Checkbox("Custom grid active", &active)) {
        if (active) {
            for (int i = 0; i < GDX_CUSTOM_GRID_SLOTS; i++) {
                characters[i] = GDX_CUSTOM_GRID_RANDOM;
                skins[i] = GDX_CUSTOM_GRID_RANDOM;
            }
            GdxCustomGrid_FormatRoster(characters, skins, roster, sizeof(roster));
        } else {
            roster[0] = '\0';
        }
        CVarSetString("gEnhancements.CustomGrid.Roster", roster);
        CVarSave();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Handpick the 29 AI opponents (machine + color) for Grand Prix,\n"
                          "Practice and Death Race. Off = stock shuffled grid.");
    }
    if (!active) {
        return;
    }

    ImGui::TextDisabled("Slot order is the starting grid; slot 1 is pole position.");
    if (gPlayerCharacters[0] >= 0 && gPlayerCharacters[0] < GDX_CUSTOM_GRID_SLOTS) {
        ImGui::TextDisabled(" 0. %s -- you (last selected machine)", kMachineNames[gPlayerCharacters[0]]);
    } else {
        ImGui::TextDisabled(" 0. You (last selected machine)");
    }

    bool changed = false;
    for (int slot = 1; slot < GDX_CUSTOM_GRID_SLOTS; slot++) {
        ImGui::PushID(slot);
        ImGui::Text("%2d.", slot);
        ImGui::SameLine();
        int charIndex = characters[slot] + 1;
        int skinIndex = skins[slot] + 1;
        ImGui::SetNextItemWidth(150.0f);
        bool slotChanged = ImGui::Combo("##character", &charIndex, sGridCharNames, GDX_CUSTOM_GRID_SLOTS + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        slotChanged |= ImGui::Combo("##skin", &skinIndex, kGridSkinNames, GDX_CUSTOM_GRID_SKIN_COUNT + 1);
        if (slotChanged) {
            characters[slot] = charIndex - 1;
            skins[slot] = skinIndex - 1;
            changed = true;
        }
        ImGui::PopID();
    }

    if (ImGui::Button("Set all slots to random")) {
        for (int i = 0; i < GDX_CUSTOM_GRID_SLOTS; i++) {
            characters[i] = GDX_CUSTOM_GRID_RANDOM;
            skins[i] = GDX_CUSTOM_GRID_RANDOM;
        }
        changed = true;
    }

    if (changed) {
        GdxCustomGrid_FormatRoster(characters, skins, roster, sizeof(roster));
        CVarSetString("gEnhancements.CustomGrid.Roster", roster);
        CVarSave();
    }
}

// F10: read-only view over the achievement table (port/gdx_achievements.cpp). The tracker ticks
// from main.cpp whether or not this page is open; the gate checkbox on the same page owns
// evaluation. Locked entries stay visible so the page doubles as a checklist.
void DrawAchievements() {
    const uint64_t total = GdxAchievements_GetTotalPlaytimeSeconds();
    const uint64_t session = GdxAchievements_GetSessionPlaytimeSeconds();
    ImGui::Text("Playtime: %llu h %02llu m total   (%llu m %02llu s this session)",
                (unsigned long long)(total / 3600), (unsigned long long)((total / 60) % 60),
                (unsigned long long)(session / 60), (unsigned long long)(session % 60));
    ImGui::TextDisabled("Time with this menu open is not counted.");

    const size_t count = GdxAchievements_Count();
    size_t unlockedCount = 0;
    for (size_t i = 0; i < count; i++) {
        GdxAchievementInfo info;
        if (GdxAchievements_Get(i, &info) && info.unlocked) {
            unlockedCount++;
        }
    }
    ImGui::Text("Achievements: %zu / %zu unlocked", unlockedCount, count);
    ImGui::Spacing();

    for (size_t i = 0; i < count; i++) {
        GdxAchievementInfo info;
        if (!GdxAchievements_Get(i, &info)) {
            continue;
        }
        ImGui::PushID(static_cast<int>(i));
        if (info.unlocked) {
            char date[16] = { 0 };
            const std::time_t t = static_cast<std::time_t>(info.unlockUnixTime);
            if (std::tm* tm = std::localtime(&t)) {
                std::strftime(date, sizeof(date), "%Y-%m-%d", tm);
            }
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "[x]");
            ImGui::SameLine();
            ImGui::Text("%s", info.name);
            ImGui::SameLine();
            ImGui::TextDisabled("-- %s (%s)", info.description, date);
        } else {
            ImGui::TextDisabled("[ ]");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", info.name);
            ImGui::SameLine();
            ImGui::TextDisabled("-- %s", info.description);
        }
        ImGui::PopID();
    }
}

} // namespace

// Named disable / hide reasons. Each evaluation runs EXACTLY ONCE PER FRAME, at the top of
// GdxMenu::DrawElement, and the result is cached in DisabledInfo::active — several are shared by
// more than one control.
//
// `reason` is what the user reads, so write it as a complete sentence naming the thing to change
// and, when it is not on the same page, where to find it.
void GdxMenu::RegisterDisableReasons() {
    mDisabledInfo.assign(GdxUI::DISABLE_OPTION_COUNT, GdxUI::DisabledInfo{});

    mDisabledInfo[GdxUI::DISABLE_FOR_NO_WINDOW] = {
        [](GdxUI::DisabledInfo&) { return GdxWindow() == nullptr; },
        "The render window is not available yet."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_WIDESCREEN_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.Widescreen", 1) == 0; },
        "Widescreen (16:9) is off. Turn it on above to use the widescreen HUD."
    };

    // Strict subset: gdx_widescreen_split_ui_active() (port/input_bridge.c) requires
    // gdx_widescreen_ui_active() as well, so the split-screen switch is inert while the 1P one is
    // off. Saying that in a disabled tooltip beats a checkbox that silently does nothing.
    mDisabledInfo[GdxUI::DISABLE_FOR_WIDESCREEN_UI_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.WidescreenUI", 1) == 0; },
        "True widescreen HUD/UI is off. Turn it on above to anchor the split-screen HUD."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_INTERPOLATION_ON] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) != 0; },
        "Frame Interpolation owns frame pacing while it is on."
    };

    // Hide condition, not a disable: interpolation's sub-controls are meaningless while the master
    // toggle is off, and the page used to omit them outright rather than grey them.
    mDisabledInfo[GdxUI::DISABLE_FOR_INTERPOLATION_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) == 0; },
        "Frame Interpolation is off."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_INTERP_OVERLAY_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.InterpDebugOverlay", 0) == 0; },
        "The interpolation debug overlay is off."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_MATCH_REFRESH_RATE_ON] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.InterpTargetMode", 0) == 0; },
        "Match Refresh Rate is on; the target follows your monitor instead. Turn it off to set a "
        "fixed target here."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_LOW_PASS_FILTER_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000) <= 0; },
        "The reconstruction filter is disabled. Enable it above to set a cutoff."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_RACE_IN_PROGRESS] = {
        [](GdxUI::DisabledInfo&) { return gdx_input_in_gameplay() != 0; },
        "A race is in progress. Ghost state must not be changed alongside the running game."
    };
}

// Scans <exe>/shaders recursively for .slangp presets and lists loose top-level .slang
// shaders, then draws a combined dropdown of built-in post modes plus one entry per file.
// Files directly in shaders/pipelines keep their legacy bare-filename CVar value so existing
// configs keep working; presets in deeper folders (e.g. a slang-shaders pack) are stored as
// forward-slash paths relative to shaders/ and display as "category/preset".
void DrawPostProcessCombo() {
    static std::vector<std::string> sPresetFiles;
    static std::vector<std::string> sPresetDisplay;
    static std::vector<const char*> sItems;

    // Walking the tree on a timer spikes the menu every interval. Rescan only on demand:
    // first draw, the Refresh button, or the shaders/ dir itself changing (a dropped folder
    // always touches it). A single file dropped into a nested folder does not touch the dir
    // mtime - that is what Refresh is for.
    static bool sScanRequested = true;
    static uint64_t sLastDirMtime = 0;

    const std::filesystem::path appDir = Ship::Context::GetAppDirectoryPath();
    const std::filesystem::path shaderDir = appDir / "shaders";

    const uint64_t dirMtime = Fast::GdxGetMtime(shaderDir);
    if (dirMtime != sLastDirMtime) {
        sLastDirMtime = dirMtime;
        sScanRequested = true;
    }

    if (!sScanRequested && !sPresetDisplay.empty()) {
        // fall through to the draw code with the cached lists
    } else {
    sScanRequested = false;

    sPresetFiles.clear();
    sPresetDisplay.clear();
    sItems.clear();
    sItems.push_back("Off");
    sItems.push_back("Scanlines");
    sItems.push_back("CRT (scanlines + grille)");

    const std::filesystem::path pipelineDir = shaderDir / "pipelines";
    if (std::filesystem::is_directory(pipelineDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(pipelineDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            constexpr size_t kSlangpExtLen = 7; // ".slangp"
            if (name.size() <= kSlangpExtLen ||
                name.compare(name.size() - kSlangpExtLen, kSlangpExtLen, ".slangp") != 0) {
                continue;
            }
            sPresetFiles.push_back(name);
            sPresetDisplay.push_back(name.substr(0, name.size() - kSlangpExtLen));
        }
    }

    // Nested presets anywhere else below shaders/. Sorted by display name so category
    // folders group together in the dropdown.
    std::vector<std::pair<std::string, std::string>> nested; // { CVar value, display }
    if (std::filesystem::is_directory(shaderDir)) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(shaderDir, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory()) {
                // include/ holds shared headers referenced by #include, never presets.
                if (it->path().filename() == "include") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file() || it->path().extension() != ".slangp") {
                continue;
            }
            const std::string rel = std::filesystem::relative(it->path(), shaderDir, ec).generic_string();
            if (ec) {
                continue;
            }
            constexpr char kPipelinesPrefix[] = "pipelines/";
            if (rel.rfind(kPipelinesPrefix, 0) == 0 && rel.find('/', strlen(kPipelinesPrefix)) == std::string::npos) {
                continue; // legacy root, already added above with its bare-filename value
            }
            constexpr size_t kSlangpExtLen = 7; // ".slangp"
            nested.emplace_back(rel, rel.substr(0, rel.size() - kSlangpExtLen));
        }
    }
    std::sort(nested.begin(), nested.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& entry : nested) {
        sPresetFiles.push_back(entry.first);
        sPresetDisplay.push_back(entry.second);
    }

    if (std::filesystem::is_directory(shaderDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(shaderDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            constexpr size_t kSlangExtLen = 6; // ".slang"
            if (name.size() <= kSlangExtLen ||
                name.compare(name.size() - kSlangExtLen, kSlangExtLen, ".slang") != 0) {
                continue;
            }
            sPresetFiles.push_back(name);
            sPresetDisplay.push_back(name.substr(0, name.size() - kSlangExtLen) + " (.slang)");
        }
    }

    for (const std::string& display : sPresetDisplay) {
        sItems.push_back(display.c_str());
    }
    } // scan refresh gate

    const int currentCrt = CVarGetInteger("gEnhancements.Graphics.CRTShader", 0);
    const char* activePipeline = CVarGetString("gEnhancements.Graphics.PostPipeline", "");
    int selection = 0;
    bool pipelineMissing = false;
    if (activePipeline != nullptr && activePipeline[0] != '\0') {
        pipelineMissing = true;
        for (size_t i = 0; i < sPresetFiles.size(); ++i) {
            if (sPresetFiles[i] == activePipeline) {
                selection = static_cast<int>(3 + i);
                pipelineMissing = false;
                break;
            }
        }
    } else if (currentCrt >= 1 && currentCrt <= 2) {
        selection = currentCrt;
    }

    ImGui::SetNextItemWidth(240.0f);
    // A configured preset that is no longer on disk is still the active setting; show it by
    // name instead of silently presenting "Off" while the backend keeps trying to load it.
    std::string missingPreview;
    if (pipelineMissing) {
        selection = -1;
        missingPreview = std::string(activePipeline) + " (missing)";
    }
    const char* preview = pipelineMissing ? missingPreview.c_str()
                          : (selection >= 0 && selection < static_cast<int>(sItems.size()))
                              ? sItems[selection]
                              : "Off";
    bool comboChanged = false;
    if (ImGui::BeginCombo("Post-process pipeline", preview)) {
        // The list can hold a whole slang-shaders pack (~1600 presets); ImGui::Combo has no
        // clipper and would pay per-item widget cost every frame the popup is open.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(sItems.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::PushID(i);
                const bool selected = (i == selection);
                if (ImGui::Selectable(sItems[i], selected)) {
                    selection = i;
                    comboChanged = true;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    if (comboChanged) {
        if (selection >= 3) {
            const size_t idx = static_cast<size_t>(selection - 3);
            if (idx < sPresetFiles.size()) {
                CVarSetInteger("gEnhancements.Graphics.CRTShader", 0);
                CVarSetString("gEnhancements.Graphics.CustomShader", "");
                CVarSetString("gEnhancements.Graphics.PostPipeline", sPresetFiles[idx].c_str());
            }
        } else {
            CVarSetInteger("gEnhancements.Graphics.CRTShader", selection);
            CVarSetString("gEnhancements.Graphics.CustomShader", "");
            CVarSetString("gEnhancements.Graphics.PostPipeline", "");
        }
        CVarSave();
        // Tracing a stored selection that reverts across runs; temporary for 1.1.0 RC.
        SPDLOG_INFO("PostPipeline combo wrote '{}'",
                    CVarGetString("gEnhancements.Graphics.PostPipeline", ""));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Post-process the frame with a shader pipeline. Built-ins downsample to\n"
                          "N64-native resolution first; .slangp presets anywhere under shaders/ run\n"
                          "multiple passes (nested folders show as category/preset), and loose\n"
                          ".slang files in shaders/ run as one-pass presets.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh list")) {
        sScanRequested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rescan the shaders/ folder. Only needed after dropping files into a\n"
                          "nested folder by hand; pack downloads and new folders are picked up\n"
                          "automatically.");
    }
}

// ── Shader pipeline UI (Settings → Shaders) ──────────────────────────────────
// RetroArch-style: the preset dropdown is the only required choice; pass info and per-parameter
// sliders describe the ACTIVE preset and live on their own page so users who don't know shaders
// never see them. Parameter values persist under
// gEnhancements.Graphics.PipelineParam.<preset-stem>.<param-name> and take precedence over the
// .slangp override (if any) and the shader default.

struct ActivePipelineInfo {
    bool valid = false;
    bool isSlangp = false;
    std::string fileName;             // CVar value: bare filename, or path relative to shaders/
    std::string stem;                 // CVar namespace for parameter overrides
    std::filesystem::path presetPath; // resolved on-disk .slangp or loose .slang
    Fast::GdxPostShaderPipeline pipeline;
    std::vector<Fast::GdxSlangParameter> params; // deduped across passes, declaration order
};

// Cached by file name + mtime so editing a shader on disk refreshes this page live.
const ActivePipelineInfo& GetActivePipelineInfo() {
    static ActivePipelineInfo sInfo;
    static std::string sCachedName;
    static uint64_t sCachedMtime = 0;

    const char* active = CVarGetString("gEnhancements.Graphics.PostPipeline", "");
    const std::string name = active != nullptr ? active : "";

    constexpr size_t kSlangpExtLen = 7; // ".slangp"
    constexpr size_t kSlangExtLen = 6;  // ".slang"
    const bool isSlangp =
        name.size() > kSlangpExtLen && name.compare(name.size() - kSlangpExtLen, kSlangpExtLen, ".slangp") == 0;
    const bool isSlang = !isSlangp && name.size() > kSlangExtLen &&
                         name.compare(name.size() - kSlangExtLen, kSlangExtLen, ".slang") == 0;

    const std::filesystem::path appDir = Ship::Context::GetAppDirectoryPath();
    const std::filesystem::path path = Fast::GdxResolvePostShaderPath(appDir, name);
    const uint64_t mtime = (isSlangp || isSlang) ? Fast::GdxGetMtime(path) : 0;

    if (name == sCachedName && mtime == sCachedMtime) {
        return sInfo;
    }
    sCachedName = name;
    sCachedMtime = mtime;

    sInfo = ActivePipelineInfo{};
    if (!isSlangp && !isSlang) {
        return sInfo;
    }

    sInfo.fileName = name;
    sInfo.presetPath = path;
    sInfo.stem = Fast::GdxPostShaderCvarStem(appDir / "shaders", path);
    sInfo.isSlangp = isSlangp;

    if (isSlangp) {
        std::string error;
        if (!Fast::GdxParsePostShaderPipeline(path, &sInfo.pipeline, &error)) {
            return sInfo; // parse errors are logged by the interpreter once per change
        }
    } else {
        // A loose .slang file runs as a one-pass pipeline (interpreter.cpp mirrors this).
        if (!std::filesystem::is_regular_file(path)) {
            return sInfo;
        }
        sInfo.pipeline.presetPath = path;
        Fast::GdxPostPassDesc pass;
        pass.shader = path.stem().string();
        sInfo.pipeline.passes.push_back(pass);
    }

    std::unordered_set<std::string> seen;
    for (const Fast::GdxPostPassDesc& pass : sInfo.pipeline.passes) {
        const std::filesystem::path shaderPath = path.parent_path() / (pass.shader + ".slang");
        for (Fast::GdxSlangParameter& p : Fast::GdxParseSlangParameters(shaderPath)) {
            if (seen.insert(p.name).second) {
                sInfo.params.push_back(std::move(p));
            }
        }
    }
    sInfo.valid = true;
    return sInfo;
}

std::string FmtPipelineFloat(float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

// "Shader passes" block: count plus one line per pass (source file, filter, scale, feedback).
void DrawPipelinePassInfo() {
    const ActivePipelineInfo& info = GetActivePipelineInfo();
    if (!info.valid) {
        return;
    }

    auto scaleTag = [](Fast::GdxPostPassDesc::ScaleType type, float scale) {
        switch (type) {
            case Fast::GdxPostPassDesc::ScaleType::Absolute:
                return FmtPipelineFloat(scale) + "px";
            case Fast::GdxPostPassDesc::ScaleType::Viewport:
                return "viewport x" + FmtPipelineFloat(scale);
            case Fast::GdxPostPassDesc::ScaleType::Source:
            default:
                return scale == 1.0f ? std::string("source") : "source x" + FmtPipelineFloat(scale);
        }
    };

    ImGui::Spacing();
    ImGui::Text("Shader passes: %d", static_cast<int>(info.pipeline.passes.size()));
    ImGui::Separator();
    for (size_t i = 0; i < info.pipeline.passes.size(); ++i) {
        const Fast::GdxPostPassDesc& pass = info.pipeline.passes[i];
        std::string line = "#" + std::to_string(i) + "  " + pass.shader + "  (" + scaleTag(pass.scaleTypeX, pass.scaleX) +
                           ", " + (pass.filterLinear ? "linear" : "nearest");
        if (pass.feedbackPass) {
            line += ", feedback";
        }
        line += ")";
        ImGui::TextDisabled("%s", line.c_str());
    }
}

// Runtime sliders for every #pragma parameter declared by the active preset. Labels use the
// shader's own description ("Scanline brightness"); the raw name and declared range sit in the
// tooltip. Reset clears this preset's whole CVar block, restoring .slangp/shader defaults.
void DrawPipelineParameters() {
    const ActivePipelineInfo& info = GetActivePipelineInfo();
    if (!info.valid || info.params.empty()) {
        return;
    }

    ImGui::Spacing();
    ImGui::Text("Shader parameters");
    ImGui::Separator();

    const std::string cvarPrefix = "gEnhancements.Graphics.PipelineParam." + info.stem + ".";
    for (const Fast::GdxSlangParameter& p : info.params) {
        const std::string cvarName = cvarPrefix + p.name;
        const std::string label = p.description.empty() ? p.name : p.description;
        // Displayed default must match runtime precedence (CVar > .slangp override > shader
        // default), otherwise a preset that overrides a parameter shows one value and runs another.
        float effectiveDefault = p.defaultValue;
        auto overrideIt = info.pipeline.parameterOverrides.find(p.name);
        if (overrideIt != info.pipeline.parameterOverrides.end()) {
            effectiveDefault = overrideIt->second;
        }
        std::string tooltip = p.name + "  |  default " + FmtPipelineFloat(effectiveDefault) + ", range " +
                              FmtPipelineFloat(p.min) + " .. " + FmtPipelineFloat(p.max);
        if (overrideIt != info.pipeline.parameterOverrides.end()) {
            tooltip += " (preset overrides shader default " + FmtPipelineFloat(p.defaultValue) + ")";
        }
        UIWidgets::CVarSliderFloat(label.c_str(), cvarName.c_str(),
                                   UIWidgets::FloatSliderOptions{ .flags = ImGuiSliderFlags_AlwaysClamp }
                                       .Min(p.min)
                                       .Max(p.max)
                                       .Step(p.step)
                                       .DefaultValue(effectiveDefault)
                                       .Tooltip(tooltip.c_str())
                                       .LabelPosition(UIWidgets::LabelPositions::Near));
    }

    if (ImGui::Button("Reset parameters to defaults")) {
        // Per-key clears, not CVarClearBlock: ClearBlock triggers ConsoleVariable::Load(), which
        // rebuilds EVERY CVar from the config file and reverts anything set at runtime but not yet
        // saved — the "reset changed my other settings" report.
        for (const Fast::GdxSlangParameter& p : info.params) {
            CVarClear((cvarPrefix + p.name).c_str());
        }
        CVarSave();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Discards your edits and restores the preset's own values.");
    }
}

// "Save preset as": writes shaders/pipelines/<name>.slangp that #references the active preset and
// bakes in the user's runtime parameter tweaks (RetroArch simple-preset style), then selects it.
void DrawSavePipelinePreset() {
    const ActivePipelineInfo& info = GetActivePipelineInfo();
    if (!info.valid || !info.isSlangp) {
        return;
    }

    static char sNameBuf[64] = "";
    static std::string sFeedback;
    static bool sFeedbackIsError = false;

    ImGui::Spacing();
    ImGui::Text("Save custom preset");
    ImGui::Separator();

    std::string sanitized;
    for (const char c : sNameBuf) {
        if (c == '\0') {
            break;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') {
            sanitized += c;
        }
    }
    while (!sanitized.empty() && sanitized.back() == ' ') {
        sanitized.pop_back();
    }

    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##savepreset", "New preset name", sNameBuf, sizeof(sNameBuf));
    ImGui::SameLine();
    if (sanitized.empty()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save##preset")) {
        const std::filesystem::path pipelineDir =
            std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "shaders" / "pipelines";
        std::error_code dirEc;
        std::filesystem::create_directories(pipelineDir, dirEc);
        const std::filesystem::path outPath = pipelineDir / (sanitized + ".slangp");
        if (std::filesystem::exists(outPath)) {
            sFeedback = sanitized + ".slangp already exists.";
            sFeedbackIsError = true;
        } else {
            std::ofstream out(outPath, std::ios::binary);
            // #reference resolves relative to the saved file, so point back at the active
            // preset wherever it lives (pipelines/, a slang-shaders pack folder, ...).
            std::error_code refEc;
            const std::string reference =
                std::filesystem::relative(info.presetPath, pipelineDir, refEc).generic_string();
            out << "#reference \"" << (refEc ? info.fileName : reference) << "\"\n";
            for (const Fast::GdxSlangParameter& p : info.params) {
                const std::string cvarName = "gEnhancements.Graphics.PipelineParam." + info.stem + "." + p.name;
                if (CVarGet(cvarName.c_str()) != nullptr) {
                    out << p.name << " = " << FmtPipelineFloat(CVarGetFloat(cvarName.c_str(), p.defaultValue))
                        << "\n";
                }
            }
            out.flush();
            if (!out.good()) {
                sFeedback = "Could not write " + outPath.filename().string() + ".";
                sFeedbackIsError = true;
            } else {
                out.close();
                sFeedback = "Saved " + sanitized + ".slangp";
                sFeedbackIsError = false;
                sNameBuf[0] = '\0';
                // The preset combo re-scans the directory every frame, so the new file is
                // selectable immediately.
                CVarSetString("gEnhancements.Graphics.PostPipeline", (sanitized + ".slangp").c_str());
                CVarSave();
                SPDLOG_INFO("PostPipeline save-preset wrote '{}.slangp'", sanitized);
            }
        }
    }
    if (sanitized.empty()) {
        ImGui::EndDisabled();
    }
    if (!sFeedback.empty()) {
        if (sFeedbackIsError) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", sFeedback.c_str());
        } else {
            ImGui::TextDisabled("%s", sFeedback.c_str());
        }
    }
}

void GdxMenu::RegisterMenu() {

    using GdxUI::SECTION_COLUMN_1;
    using GdxUI::SECTION_COLUMN_2;
    using GdxUI::WidgetInfo;

    // Sections, in tab order. Each remembers its own last-viewed page, so switching tabs and back
    // returns you where you were.
    AddMenuEntry("Settings", "gSettings.Menu.Sidebar.Settings");
    AddMenuEntry("Enhancements", "gSettings.Menu.Sidebar.Enhancements");
    AddMenuEntry("Workshop", "gSettings.Menu.Sidebar.Workshop");
    AddMenuEntry("Online", "gSettings.Menu.Sidebar.Online");
    AddMenuEntry("Dev Tools", "gSettings.Menu.Sidebar.DevTools");

    // Page-level search terms: a query that names no individual control still surfaces the page.
    AddSidebarEntry("Settings", "General", 1, "general menu opacity controller navigation about credits licenses");
    AddSidebarEntry("Settings", "Graphics", 1,
                    "graphics internal resolution msaa texture filter vsync fullscreen z fighting");
    AddSidebarEntry("Settings", "Shaders", 1,
                    "shaders shader post process pipeline preset crt scanlines slang slangp passes parameters "
                    "customize save");
    AddSidebarEntry("Settings", "Audio", 2, "audio lle hle filter low pass volume reverb latency buffer");
    AddSidebarEntry("Settings", "Controls", 1,
                    "controls controller configuration keyboard gamepad mouse bindings remap");
    AddSidebarEntry("Settings", "Input Viewer", 2, "input viewer overlay analog stick buttons speedrun");

    AddSidebarEntry("Enhancements", "Visuals", 2,
                    "visuals graphics enhancements widescreen hud ui draw distance lod frame pacing "
                    "interpolation smoothing target fps refresh rate");
    AddSidebarEntry("Enhancements", "Gameplay", 1,
                    "gameplay transitions ai aggression random opponent colors custom grid x cup seed");
    AddSidebarEntry("Enhancements", "Mouse", 1,
                    "mouse steering sensitivity cursor control hide cursor confine window course edit "
                    "create machine issue 18");
    AddSidebarEntry("Enhancements", "Course Edit & Machine", 1,
                    "course edit editor test drive machine engine official back ending scene flashing "
                    "create machine");
    AddSidebarEntry("Enhancements", "Practice", 1,
                    "practice lap delta ghost photo mode free camera replay");
    AddSidebarEntry("Enhancements", "Ghosts", 1,
                    "ghost browser replay library opponents import export staff player autosave boost trail");
    AddSidebarEntry("Enhancements", "Cosmetics", 1,
                    "cosmetics effect colors boost dash pad idle side attack machine palette editor");
    AddSidebarEntry("Enhancements", "Achievements", 1, "achievements playtime unlocks progress trophies checklist");

    AddSidebarEntry("Workshop", "Mods", 1,
                    "workshop mods texture sequence sample soundfont model packs lua scripts installed");
    AddSidebarEntry("Workshop", "Tools", 1,
                    "workshop tools asset dump dd save 64dd sidecar");
    AddSidebarEntry("Workshop", "Content Library", 1,
                    "workshop content library export import gdxc track ghosts");
    AddSidebarEntry("Workshop", "ROM Hacks", 1,
                    "workshop rom hacks romhacks hack mods o2r patched archive save isolation");

    AddSidebarEntry("Online", "Overview", 1,
                    "online discord rich presence status privacy leaderboard ghost upload download netplay spectator");

    AddSidebarEntry("Dev Tools", "General", 1,
                    "developer multi viewport tools gates logging diagnostics behavior overrides "
                    "interpolation camera");
    AddSidebarEntry("Dev Tools", "Stats", 1, "stats fps frame timing performance interpolation sub-frames presented");
    AddSidebarEntry("Dev Tools", "Console", 1, "console commands log reset");
    AddSidebarEntry("Dev Tools", "Gfx Debugger", 1, "gfx graphics debugger display list rendering");

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> General
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Menu Settings", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Stored as a 0.35..1.0 float, presented as a percentage. IsPercentage() rewrites
    // format/min/max as a side effect, so it MUST come before the explicit .Min()/.Max()
    // (UIWidgets.hpp:603-611). AlwaysClamp has no fluent setter, hence the designated initialiser;
    // it is what keeps a Ctrl+click typed value inside the range.
    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Menu background opacity",
                          .cVar = "gSettings.Menu.BackgroundOpacity",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions{ .flags = ImGuiSliderFlags_AlwaysClamp }
                               .IsPercentage()
                               .Min(0.35f)
                               .Max(1.0f)
                               .Step(0.01f)
                               .DefaultValue(0.85f)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("How opaque this menu's backdrop is over the game "
                                        "(35% = most see-through)."))
                  .SearchTerms("transparency backdrop alpha"));

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Menu controller navigation",
                          .cVar = "gControlNav",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Lets a connected gamepad navigate the menu. Game input is blocked "
                      "while the menu is open."))
                  .SearchTerms("gamepad pad joystick nav"));

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Open or close this menu with F1, Escape, or Gamepad Back.",
                          .type = GdxUI::WIDGET_TEXT_DISABLED });

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Data & Files", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { GdxDrawDataAndFilesPanel(); })
                  .SearchTerms("rom z64 ipl 64dd disk ndd archive o2r coverage fallback delete deletable setup"));

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "About G-Diffuser", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawAboutMenu(); })
                  .SearchTerms("about version credits licenses expansion kit legal"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Graphics
    //
    // The read-once trio (internal res / MSAA / texture filter) is consumed by the backend only at
    // window Init, so a plain CVar write is inert until the matching setter is called. Each
    // therefore carries a Callback that applies it live:
    //   - internal res -> Ship::Window::SetResolutionMultiplier(float)  (Window.h:140, base virtual)
    //   - MSAA         -> Ship::Window::SetMsaaLevel(uint32_t)          (Window.h:145, base virtual)
    //   - tex filter   -> Fast::Fast3dWindow::SetTextureFilter(FilteringMode) (Fast3dWindow.h:81 —
    //                     Fast3d-only, hence a null-safe downcast, skipped on other backends)
    // All run on the render/GUI thread the menu already draws on. VSync and z-fighting are read
    // live by the backend; fullscreen uses the active Window API.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Renderer", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // The callback re-reads the CVar because UIWidgets' CVar widgets report "edited this frame",
    // not the new value.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Internal resolution (x)",
                          .cVar = "gInternalResolution",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions()
                               .Min(0.5f)
                               .Max(4.0f)
                               .Step(0.01f)
                               .DefaultValue(1.0f)
                               .Format("%.2f")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Render scale relative to the window size. 1.00x = native; higher is\n"
                                        "sharper but costs GPU. Applies immediately."))
                  .Callback([](WidgetInfo&) {
                      auto window = GdxWindow();
                      if (window != nullptr) {
                          // apply live (Fast3dWindow.cpp:315)
                          window->SetResolutionMultiplier(CVarGetFloat("gInternalResolution", 1.0f));
                      }
                  })
                  .SearchTerms("supersampling render scale sharpness upscale"));

    // NOT a CVar-bound combobox: gMSAAValue stores the SAMPLE COUNT, not the list index, so the
    // mapping is explicit. The vector overload keeps the rows in declared order; the unordered_map
    // one would scramble them (UIWidgets.hpp's gap list).
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "MSAA", .type = GdxUI::WIDGET_COMBOBOX }
                  .ValuePointer(&mMsaaIndex)
                  .ComboItems({ "Off (1x)", "2x", "4x", "8x" })
                  .Options(UIWidgets::ComboboxOptions()
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Multi-sample anti-aliasing. Higher smooths edges at a GPU cost.\n"
                                        "Off (1x) = stock. Applies immediately."))
                  .PreFunc([this](WidgetInfo&) {
                      const int cur = CVarGetInteger("gMSAAValue", 1);
                      mMsaaIndex = 0;
                      for (int i = 0; i < 4; ++i) {
                          if (kMsaaValues[i] == cur) {
                              mMsaaIndex = i;
                          }
                      }
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gMSAAValue", kMsaaValues[mMsaaIndex]);
                      GdxSaveCvars();
                      auto window = GdxWindow();
                      if (window != nullptr) {
                          window->SetMsaaLevel((uint32_t)kMsaaValues[mMsaaIndex]); // live (Fast3dWindow.cpp:319)
                      }
                  })
                  .SearchTerms("anti aliasing antialiasing samples jaggies edges"));

    // Enum order is fixed by libultraship (gfx_rendering_api.h: FILTER_THREE_POINT=0,
    // FILTER_LINEAR=1, FILTER_NONE=2) and index == enum value, so a plain CVar-bound combobox owns
    // read/write/persist. The live apply is Fast3d-only; on another backend the CVar is still saved
    // and takes effect on the next restart.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Texture filter", .cVar = "gTextureFilter", .type = GdxUI::WIDGET_CVAR_COMBOBOX }
                  .ComboItems({
                      "Three-point (N64)", // FILTER_THREE_POINT = 0 (the 1:1 default)
                      "Linear",            // FILTER_LINEAR      = 1
                      "None (sharp)"       // FILTER_NONE        = 2
                  })
                  .Options(UIWidgets::ComboboxOptions()
                               .DefaultIndex(0 /* FILTER_THREE_POINT */)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("How textures are sampled. Three-point mimics the N64 (stock);\n"
                                        "Linear is smoother; None is sharp/pixelated. Applies immediately."))
                  .Callback([](WidgetInfo&) {
                      auto fast = GdxFast3dWindow();
                      if (fast != nullptr) {
                          fast->SetTextureFilter(
                              static_cast<Fast::FilteringMode>(CVarGetInteger("gTextureFilter", 0)));
                      }
                  })
                  .SearchTerms("filtering bilinear smoothing sharp pixelated nearest"));

    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Post-process shaders moved to the Shaders page.",
                          .type = GdxUI::WIDGET_TEXT_DISABLED }
                  .SearchTerms("crt scanlines post process pipeline slangp shader"));

    AddWidget("Settings", "Graphics", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });

    // Read live per-present, so a plain write takes effect immediately.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "VSync", .cVar = "gVsyncEnabled", .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Syncs presentation to the display refresh to avoid tearing.\n"
                      "On = stock. Turn off if you use Frame pacing."))
                  .SearchTerms("vertical sync tearing"));

    // Live window state, not a CVar: the truth is Window::IsFullscreen(), so there is nothing for a
    // CVar widget to read or write. Ship::Window routes the change through the active backend and
    // persists the result via Fast3dWindow's fullscreen-changed callback, exactly like F11.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Fullscreen", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mFullscreen)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Uses the active window backend (borderless fullscreen on DX11).\n"
                      "The F11 shortcut controls the same setting."))
                  .PreFunc([this](WidgetInfo&) {
                      auto window = GdxWindow();
                      mFullscreen = window != nullptr && window->IsFullscreen();
                  })
                  .Callback([this](WidgetInfo&) {
                      auto window = GdxWindow();
                      if (window != nullptr) {
                          window->SetFullscreen(mFullscreen);
                      }
                  })
                  .DisableWhen({ GdxUI::DISABLE_FOR_NO_WINDOW })
                  .SearchTerms("borderless window f11 screen"));

    // Consumed live by the Fast3D backend when it builds rasterizer state for DECAL z-mode
    // polygons: it sets the SlopeScaledDepthBias on coplanar decal surfaces — track markings,
    // shadows, surface text — so they do not z-fight the geometry they sit on
    // (gfx_direct3d11.cpp:724 and the matching gfx_opengl/gfx_metal switches). Mode 1 scales the
    // bias by render height to mimic the N64's own decal offset; mode 2 uses a stronger bias that
    // stops far decals from vanishing. Index == enum value, so this is a straight CVar combobox.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Z-fighting reduction", .cVar = "gZFightingMode",
                          .type = GdxUI::WIDGET_CVAR_COMBOBOX }
                  .ComboItems({ "Disabled", "N64-style (scaled)", "No vanishing decals" })
                  .Options(UIWidgets::ComboboxOptions()
                               .DefaultIndex(0)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Adjusts the depth bias on decal surfaces (track markings, shadows)\n"
                                        "so they don't shimmer against the road. Disabled = stock."))
                  .SearchTerms("depth bias decal shimmer flicker markings shadows"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Shaders
    //
    // RetroArch-style shader page: the preset dropdown is the only required choice; everything
    // below it describes and customizes the ACTIVE preset (pass list, parameter sliders, save-as).
    // Users who don't know what a shader is pick a preset by name and never touch the rest.
    //
    // Three CVars cooperate: CRTShader for built-in modes, CustomShader for a legacy single-file
    // shader in <exe>/shaders, and PostPipeline for a multi-pass .slangp preset found anywhere
    // under <exe>/shaders (pipelines/ is the classic root; deeper folders are stored as relative
    // paths). The dropdown lists built-ins and presets; CustomShader remains functional for
    // backward compatibility.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Shaders", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Shaders post-process the final frame (scanlines, CRT look, and more).\n"
                                  "Pick a preset; the options below adapt to the active one.",
                          .type = GdxUI::WIDGET_TEXT_DISABLED });

    AddWidget("Settings", "Shaders", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Shader preset", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { DrawPostProcessCombo(); })
                  .SearchTerms("crt scanlines aperture grille retro post process pipeline slangp shader preset"));

    AddWidget("Settings", "Shaders", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Shader passes", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { DrawPipelinePassInfo(); })
                  .SearchTerms("passes stages feedback linear nearest scale"));

    AddWidget("Settings", "Shaders", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Shader parameters", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { DrawPipelineParameters(); })
                  .SearchTerms("pipeline shader parameter slider customize tweak reset defaults"));

    AddWidget("Settings", "Shaders", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Save custom preset", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { DrawSavePipelinePreset(); })
                  .SearchTerms("save preset as custom slangp write"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Audio (2 columns: the signal path on the left, levels and latency on the right)
    //
    // These write port-owned CVars; the live-read plumbing on the audio thread lives in
    // gdx_audio_lle.c and os.cpp.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Output status", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawAudioStatus(); })
                  .SearchTerms("backend wasapi sdl driver dummy queued samples diagnostic no sound silent"));

    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Output Device", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // gEnhancements.Audio.Backend is 0=Auto, 1=WASAPI, 2=SDL, applied at startup in main.cpp's
    // InitAudio. Only backends that exist on this platform are offered, so on non-Windows hosts the
    // reduced two-entry list maps index 1 -> CVar 2 (SDL) — which is why this is a plain Combobox
    // over an index rather than a CVar-bound one.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Output backend", .type = GdxUI::WIDGET_COMBOBOX }
                  .ValuePointer(&mAudioBackendIndex)
#ifdef _WIN32
                  .ComboItems({ "Auto", "WASAPI", "SDL" })
                  .PreFunc([this](WidgetInfo&) {
                      const int sel = CVarGetInteger("gEnhancements.Audio.Backend", 0);
                      mAudioBackendIndex = (sel >= 0 && sel <= 2) ? sel : 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Audio.Backend", mAudioBackendIndex);
                      GdxSaveCvars();
                  })
#else
                  .ComboItems({ "Auto", "SDL" })
                  .PreFunc([this](WidgetInfo&) {
                      // map the stored CVar (2 = SDL) into the reduced list
                      mAudioBackendIndex = (CVarGetInteger("gEnhancements.Audio.Backend", 0) == 2) ? 1 : 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Audio.Backend", mAudioBackendIndex == 1 ? 2 : 0);
                      GdxSaveCvars();
                  })
#endif
                  .Options(UIWidgets::ComboboxOptions()
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Which OS audio output path to use. Auto keeps the platform default\n"
                                        "(WASAPI on Windows, SDL elsewhere). Applies on restart."))
                  .SearchTerms("wasapi sdl device api output"));

    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Applies on restart.", .type = GdxUI::WIDGET_TEXT_DISABLED });

    // The two buttons share one CVar: `radioValue` is what each writes, so there is no state here.
    // KNOWN QUIRK (UIWidgets.cpp:1126-1136): CVarRadioButton draws the radio with an invisible
    // label and the visible text as a separate item, and hangs the tooltip off THAT text — so
    // hovering the circle raises nothing and hovering the label does.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Synthesis Engine", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "LLE (accurate)", .cVar = "gEnhancements.Audio.LLE",
                          .type = GdxUI::WIDGET_CVAR_RADIO_BUTTON }
                  .RadioValue(1)
                  .Options(UIWidgets::RadioButtonsOptions().DefaultIndex(1).Tooltip(
                      "Low-level RSP emulation (cxd4). Most accurate; the default."))
                  .SearchTerms("engine rsp microcode accuracy audio synthesis"));
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "HLE (fast)", .cVar = "gEnhancements.Audio.LLE",
                          .type = GdxUI::WIDGET_CVAR_RADIO_BUTTON }
                  .RadioValue(0)
                  .Options(UIWidgets::RadioButtonsOptions().DefaultIndex(1).Tooltip(
                      "High-level audio emulation. Faster, less accurate."))
                  // The asterisk belongs to the PAIR, not to either button, so it is drawn once
                  // after the second one. The default is LLE, i.e. CVar == 1.
                  .PostFunc([](WidgetInfo&) { GdxModifiedMarker(CVarGetInteger("gEnhancements.Audio.LLE", 1) == 0); })
                  .SearchTerms("engine fast performance audio synthesis"));

    // gEnhancements.Audio.LowPassHz holds 0 (filter off) or a 500..16000 cutoff. Both controls are
    // non-CVar on purpose: "on" is `stored > 0`, and turning it off must stash the live cutoff in
    // mLastLowPassHz so re-enabling restores the same frequency. A CVar checkbox would write a bare
    // 0/1 into a Hz field and lose the cutoff.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Reconstruction Filter", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable filter", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mLowPassFilterOn)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Low-pass filter on the reconstructed output, softening high-frequency\n"
                      "aliasing. On = stock. Off disables the filter entirely."))
                  .PreFunc([this](WidgetInfo&) {
                      mLowPassFilterOn = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000) > 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      if (mLowPassFilterOn) {
                          const int restore = mLastLowPassHz > 0 ? mLastLowPassHz : 15000;
                          CVarSetInteger("gEnhancements.Audio.LowPassHz", restore);
                      } else {
                          const int hz = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
                          if (hz > 0) {
                              mLastLowPassHz = hz; // remember so re-enabling restores the same cutoff
                          }
                          CVarSetInteger("gEnhancements.Audio.LowPassHz", 0);
                      }
                      GdxSaveCvars();
                  })
                  .PostFunc([this](WidgetInfo&) { GdxModifiedMarker(!mLowPassFilterOn); }) // default is on
                  .SearchTerms("low pass lowpass reconstruction aliasing treble"));

    // While the filter is off the CVar holds 0, but the slider must keep showing the remembered
    // cutoff instead of snapping to the bottom of the range — hence the non-CVar slider and the
    // preFunc that restores the display value. IntSliderOptions::clamp covers the lower bound.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Cutoff (Hz)", .type = GdxUI::WIDGET_SLIDER_INT }
                  .ValuePointer(&mLowPassCutoffHz)
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(500)
                               .Max(16000)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Cutoff frequency of the reconstruction low-pass. Lower = softer/darker.\n"
                                        "Enable the filter above to adjust this."))
                  .PreFunc([this](WidgetInfo&) {
                      const int hzNow = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
                      mLowPassCutoffHz = hzNow > 0 ? hzNow : (mLastLowPassHz > 0 ? mLastLowPassHz : 15000);
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Audio.LowPassHz", mLowPassCutoffHz);
                      mLastLowPassHz = mLowPassCutoffHz;
                      GdxSaveCvars();
                  })
                  .DisableWhen({ GdxUI::DISABLE_FOR_LOW_PASS_FILTER_OFF })
                  .SearchTerms("frequency hz low pass lowpass darker brighter"));

    // Column 2: levels and latency.
    //
    // Master volume is a final-stage gain multiply on the s16 output copy in os.cpp's
    // osAiSetNextBuffer, read live there each buffer; 100 skips the multiply entirely. The
    // tooltip's "%" is literal, because UIWidgets renders tooltips through SetTooltip("%s", ...)
    // rather than as a format string.
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Levels", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Master volume (%)", .cVar = "gEnhancements.Audio.MasterVolume",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(0)
                               .Max(100)
                               .DefaultValue(100)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Final output gain. 100% = stock (bit-exact, no gain applied)."))
                  .SearchTerms("volume loudness gain level"));

    // Wired to the HLE reverb kill switch in n64_audio_hle.c (the A_MIXER wet->dry return), read
    // live there. It affects the HLE engine ONLY: under the default LLE engine reverb comes from
    // the audio microcode itself, so toggling this is inaudible while LLE is selected.
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Reverb", .cVar = "gEnhancements.Audio.Reverb",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Affects the HLE audio engine only.\n"
                      "Under the default LLE engine, reverb is the microcode's own."))
                  .ModifiedMarker()
                  .SearchTerms("echo wet dry ambience"));

    // Read ONCE at InitAudio (main.cpp), so a change applies only on the next restart — hence the
    // note. A larger reservoir rides out host scheduling jitter but adds latency.
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Latency", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Buffer size (frames)", .cVar = "gEnhancements.Audio.BufferFrames",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(1024)
                               .Max(8192)
                               .DefaultValue(4096)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Audio buffer size. Larger rides out host jitter (fewer dropouts) but\n"
                                        "adds latency; smaller is snappier but more underrun-prone. "
                                        "Applies on restart."))
                  .Note("(applies on restart)")
                  .SearchTerms("latency buffer dropouts underrun crackle stutter"));

    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "More", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Sound test / jukebox", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Controls
    // InputEditorWindow is registered in main.cpp at boot under the name "Input Editor"; this page
    // embeds it, or pops it out.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Controls", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Input Editor", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Input Editor", "Configure controllers, keyboard, mouse, deadzones, "
                                                         "sensitivity, and per-port mappings.");
                  })
                  .SearchTerms("controller keyboard mouse deadzone sensitivity mapping bindings remap port"));

    AddWidget("Settings", "Controls", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Port seating", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    // DefaultValue must match the ctor's CVarRegisterInteger default (1).
    AddWidget("Settings", "Controls", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Auto-assign gamepads to ports",
                          .cVar = "gEnhancements.Input.AutoAssignGamepadPorts",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "With two or more controllers connected, each gamepad gets its own N64 port "
                      "instead of all sharing port 1. Seats are remembered per controller across "
                      "restarts and replugs.\nOn = default. Turn off to manage ports manually in "
                      "the Input Editor."))
                  .SearchTerms("multiplayer two controllers player 2 assign port seating hotplug"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Input Viewer (2 columns)
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Input Viewer", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Non-CVar: an already-constructed GuiWindow reads its visibility CVar only at construction, so
    // the live state is the window's own and the flip must go through ToggleVisibility.
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show input viewer overlay", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mInputViewerVisible)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Shows the exact mapped N64 input state delivered to F-Zero X."))
                  .PreFunc([this](WidgetInfo&) { mInputViewerVisible = GdxWindowVisible("Input Viewer"); })
                  .Callback([](WidgetInfo&) { GdxToggleWindow("Input Viewer"); })
                  .SearchTerms("overlay hud speedrun display controller"));

    // AlwaysClamp has no fluent setter on FloatSliderOptions, hence the designated initialiser on
    // both overlay sliders.
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Overlay scale", .cVar = "gInputViewer.Scale",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions{ .flags = ImGuiSliderFlags_AlwaysClamp }
                               .Min(0.5f)
                               .Max(2.5f)
                               .Step(0.01f)
                               .DefaultValue(1.0f)
                               .Format("%.2fx")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Size of the on-screen input overlay."))
                  .SearchTerms("size zoom"));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Overlay opacity", .cVar = "gInputViewer.Opacity",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions{ .flags = ImGuiSliderFlags_AlwaysClamp }
                               .Min(0.2f)
                               .Max(1.0f)
                               .Step(0.01f)
                               .DefaultValue(1.0f)
                               .Format("%.2f")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Transparency of the input overlay."))
                  .SearchTerms("transparency alpha"));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable dragging", .cVar = "gInputViewer.EnableDragging",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Lets you reposition the overlay by dragging it with the mouse."))
                  .SearchTerms("move position mouse drag"));

    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Appearance", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Show background layer", .cVar = "gInputViewer.ShowBackground",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Draws the controller-body backdrop behind the buttons.")));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Show D-pad layers", .cVar = "gInputViewer.ShowDpad",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Includes the D-pad in the overlay (off by default; F-Zero X does not use it).")));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Button outlines", .cVar = "gInputViewer.ButtonOutlineMode",
                          .type = GdxUI::WIDGET_CVAR_COMBOBOX }
                  .ComboItems({ "Always shown", "Shown while released", "Shown while pressed", "Hidden" })
                  .Options(UIWidgets::ComboboxOptions()
                               .DefaultIndex(1)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("When each button's outline is drawn relative to its pressed state.")));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Show analog values", .cVar = "gInputViewer.ShowAnalogValues",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Prints the raw analog-stick X/Y numbers next to the stick."))
                  .SearchTerms("numbers coordinates stick x y"));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "The viewer reads G-Diffuser's final mapped N64 state, after controller "
                                  "bindings and analog curves. Inputs intentionally read neutral while this "
                                  "menu owns game input.",
                          .type = GdxUI::WIDGET_TEXT });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Visuals (2 columns: aspect/detail on the left, pacing on the right)
    //
    // The CVars themselves are registered in the GdxMenu constructor, alongside what each does.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Visual Enhancements", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Read live in interpreter.cpp AdjXForAspectRatio. OFF has two known edge cases: MSAA>1 at
    // exactly 1x internal resolution, and AdvancedResolution taking precedence.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Widescreen (16:9)", .cVar = "gEnhancements.Graphics.Widescreen",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "On: fills the window in 16:9 (hor+).\n"
                      "Off: renders 4:3 with pillarbox bars on the sides."))
                  .ModifiedMarker()
                  .SearchTerms("aspect ratio 4:3 pillarbox hor+ 16:9"));

    // DefaultValue must track the registration in gdx_menu.cpp, or the "changed from stock"
    // asterisk lies. Requires Widescreen — a NAMED disable reason rather than a bare greyed
    // checkbox, so the disabled tooltip says what to turn on and where.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "True widescreen HUD/UI", .cVar = "gEnhancements.Graphics.WidescreenUI",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Anchors the single-player HUD to the true screen edges and extends\n"
                      "the SELECT MACHINE blue background and race transitions. Other\n"
                      "menu artwork stays proportional in 4:3. Requires Widescreen."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_WIDESCREEN_OFF })
                  .SearchTerms("hud ui corner select machine transitions widescreen"));

    // Disabled rather than hidden when the 1P switch is off, because the reason is worth reading.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Widescreen split-screen HUD", .cVar = "gEnhancements.Graphics.WidescreenSplitUI",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Anchors the 2P/3P/4P race HUD to the edges of each player's view instead\n"
                      "of letting it bunch toward the middle of a 16:9 screen. Covers the timer,\n"
                      "lap counter, energy gauge, minimap, position and speed.\n\n"
                      "Elements the game centres inside a column (interval, reverse, the 3P spare\n"
                      "minimap) deliberately stay on the stock path. The VS machine-select screen\n"
                      "is not covered yet. Requires True widescreen HUD/UI."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_WIDESCREEN_UI_OFF })
                  .SearchTerms("split screen vs battle death race 2p 3p 4p hud multiplayer anchor"));

    // Default on: a Course Edit TEST DRIVE lifts the editor's 4:3 pin and renders like a normal
    // race. Off keeps the pin (stock) for the whole run.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Widescreen test drive", .cVar = "gEnhancements.Graphics.WidescreenTestDrive",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Lets Course Edit's TEST DRIVE render in widescreen like a normal race\n"
                      "instead of staying pillarboxed at the editor's 4:3. The test-drive HUD\n"
                      "(speed, minimap) keeps its stock 4:3 placement for now.\n\n"
                      "Requires Widescreen."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_WIDESCREEN_OFF })
                  .SearchTerms("course edit test drive widescreen pillarbox 4:3"));

    // Widens the game's CPU-side culls (track chunks, racers, fireworks, stars) to the true frame
    // on 21:9/32:9 displays. The 3D hor+ rendering already handles any aspect and needs no switch.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ultrawide (21:9+)", .cVar = "gEnhancements.Graphics.UltrawideMode",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Stops track pieces, machines, fireworks and background stars from\n"
                      "popping in and out at the edges of ultrawide (21:9 and wider) windows.\n"
                      "The 3D view itself already extends to any aspect; leave this off on\n"
                      "16:9 displays. The HUD anchors to the true corners, like at 16:9.\n"
                      "Requires Widescreen."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_WIDESCREEN_OFF })
                  .SearchTerms("ultrawide 21:9 32:9 super ultrawide aspect cull pop-in"));

    // Opens the CRT-overscan frame (viewport 296x224 + scissor 12,8..308,232) to the true
    // 320x240 at the FULL_SCREEN chokepoints (sys/math.c + camera.c mirrors).
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Remove black borders", .cVar = "gEnhancements.Graphics.RemoveBorders",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Removes the black frame around the whole image. The game renders inside\n"
                      "the CRT-safe area on every screen -- a TV's overscan used to hide that\n"
                      "border, but on a monitor it shows. Fills the picture to the true edges."))
                  .ModifiedMarker()
                  .SearchTerms("border overscan black frame edge fill crop"));

    // Skips Transition_WipeDraw only (decomp transition.c); the wipe state machine still runs.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Hide race intro curtain", .cVar = "gEnhancements.Graphics.HideRaceCurtain",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Hides the black wipe curtain the game draws over the screen corners\n"
                      "during the race intro and countdown. In widescreen it stretches to the\n"
                      "window corners, which is where the black staircase wedges come from.\n"
                      "Race timing is unaffected."))
                  .ModifiedMarker()
                  .SearchTerms("curtain wipe corner wedge border intro countdown"));

    // Skips carousel viewports fully outside the 4:3 frame in course select (decomp
    // course_select/course_model.c func_i5_80115E64); the N64 clipped these itself.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Clip off-screen course previews", .cVar = "gEnhancements.Graphics.CourseSelectClipOffscreen",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "On the course-select carousel the game draws all six course previews\n"
                      "and relies on N64 hardware clipping to hide the off-screen ones. The PC\n"
                      "renderer does not clip them, so on course 6 the cup's first-course\n"
                      "outline leaks onto the screen. This skips zero-pixel viewports.\n"
                      "Disable if adjacent previews pop at the frame edges in widescreen."))
                  .ModifiedMarker()
                  .SearchTerms("course 6 stray preview outline carousel first course"));

    // Scales each course's own far-render cutoff per-venue (course.c Course_Draw).
    //
    // CAPPED AT 200% ON PURPOSE. The CVar multiplies the per-chunk cull threshold
    // (sCourseFarRenderDistance * scale), but the track is streamed as a fixed set of chunks that
    // Course_SegmentsInit builds only out to a bounded horizon (gSegmentChunks, capped at
    // SEGMENT_CHUNK_COUNT). By ~200% the raised threshold already clears the furthest chunk ever
    // built, so a higher scale un-culls nothing. A content limit, not a clamp that can be raised.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Draw distance (%)", .cVar = "gEnhancements.Graphics.DrawDistance",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(100)
                               .Max(200)
                               .DefaultValue(100)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Extends how far each track's own geometry renders (100% = stock,\n"
                                        "scales per-venue). 200% is the effective max: beyond it the track's\n"
                                        "streamed geometry runs out, so there is nothing further to draw."))
                  .SearchTerms("render distance fog pop-in culling horizon"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Force max machine detail", .cVar = "gEnhancements.Graphics.ForceMaxMachineLOD",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Always renders every machine at its highest-detail model,\n"
                      "ignoring distance. Off = stock distance-based detail."))
                  .ModifiedMarker()
                  .SearchTerms("lod level of detail model quality machines"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Adjust machine detail", .cVar = "gEnhancements.Graphics.MachineLODDistMod",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(0)
                               .Max(5)
                               .DefaultValue(0)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Render each machine at a higher-detailed model than default,\n"
                                        "accounting for distance. 0 = stock distance-based detail."))
                  .ModifiedMarker()
                  .SearchTerms("lod level of detail model quality machines"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Force rendering machine shadows", .cVar = "gEnhancements.Graphics.ForceDrawMachineShad",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Always renders each machine's shadow, ignoring distance.\n"
                      "Off = stock distance-based shadow rendering."))
                  .ModifiedMarker()
                  .SearchTerms("lod level of detail shadow quality machines"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Adjust machine boosters detail", .cVar = "gEnhancements.Graphics.MachineBoostLODDistMod",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(0)
                               .Max(2)
                               .DefaultValue(0)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Render each machine's booster graphics at a higher-level than default,\n"
                                        "accounting for distance. 0 = stock distance-based detail."))
                  .ModifiedMarker()
                  .SearchTerms("lod level of detail boosters quality machines"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Fancy lighting", .cVar = "gEnhancements.Graphics.FancyLighting",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Dims ambient light on each machine so the point light and custom lights\n"
                      "read better against segment lighting. Off = stock lighting."))
                  .ModifiedMarker()
                  .SearchTerms("lighting ambient fog point light machine"));

    // Column 2: pacing / interpolation.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Enhancements (parity-gated)", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // port/gdx_frame_pacer.c holds the host loop to the true N64 NTSC field rate (~59.94Hz) with a
    // wall-clock sleep+spin. VSync should be off while it runs, since a display-refresh present
    // beats against the fixed schedule. Mutually exclusive with Frame Interpolation — both are
    // pacing owners, expressed as the named reason DISABLE_FOR_INTERPOLATION_ON.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Frame pacing (59.94 Hz)", .cVar = "gEnhancements.Graphics.FramePacing",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Experimental. The renderer already limits the game to ~60 fps; this\n"
                      "pins the loop to the true N64 rate (59.94 Hz). Turn VSync OFF when using it."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_ON })
                  .SearchTerms("ntsc 59.94 pacing timing judder"));

    // Read LIVE every tick (gdx_interp::P2HostActive), so this takes effect on the next tick.
    //
    // The tooltip is load-bearing documentation: it names both known artifacts. Every line is under
    // UIWidgets' 80-character wrap width, so WrappedText leaves the manual breaks as written.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Frame Interpolation (EXPERIMENTAL)",
                          .cVar = "gEnhancements.Graphics.FrameInterpolation",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Interpolates rendering between 60Hz logic ticks for smoother motion on\n"
                      "high-refresh displays. VSync ON is recommended. Adds about half a tick\n"
                      "of latency. Bypasses Frame pacing while on. Default OFF.\n"
                      "\n"
                      "EXPERIMENTAL - two known artifacts, both inherent to matrix-only\n"
                      "interpolation (SoH-class ports share them):\n"
                      "  - Strobing on flicker-blend effects. The game alternates certain\n"
                      "    transparencies every 60Hz tick (low-energy body gradient, pursuit\n"
                      "    marker pulse); each phase is held for a whole tick's sub-frames, and\n"
                      "    the sub-frame count oscillates, so phases get unequal screen time.\n"
                      "  - Static scenery and HUD do not tween. Only matrices the game rebuilds\n"
                      "    each frame are interpolated; baked asset display lists stay at 60Hz,\n"
                      "    so they can judder against smoothly moving geometry."))
                  .ModifiedMarker()
                  .SearchTerms("smooth motion high refresh 120hz 144hz tween subframe"));

    // The four controls below belong to Frame Interpolation and are HIDDEN while it is off, driven
    // by the same once-per-frame cache as disabling, so its CVar is read once and not four times.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Debug overlay", .cVar = "gEnhancements.Graphics.InterpDebugOverlay",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Show live sub-frame statistics."))
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .SearchTerms("interpolation subframes statistics diagnostic"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Sub-frames last tick", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) {
                      const int sub = gdx_gfx_interp_last_subframes();
                      const double t = gdx_gfx_interp_last_t();
                      ImGui::TextDisabled("subframes last tick: %d (t=%.2f)", sub, t);
                  })
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF, GdxUI::DISABLE_FOR_INTERP_OVERLAY_OFF })
                  .HideInSearch()); // a read-out with no setting behind it; the toggle above is the control

    // Non-CVar because the checkbox is inverted: it reads TRUE for CVar value 0 (Match Refresh
    // Rate) and FALSE for 1 (Capped), and CVarCheckbox cannot invert.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Match Refresh Rate", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mInterpMatchRefresh)
                  .Options(UIWidgets::CheckboxOptions().Tooltip("Targets your monitor's current refresh rate."))
                  .PreFunc([this](WidgetInfo&) {
                      mInterpMatchRefresh = CVarGetInteger("gEnhancements.Graphics.InterpTargetMode", 0) == 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Graphics.InterpTargetMode", mInterpMatchRefresh ? 0 : 1);
                      GdxSaveCvars();
                  })
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .SearchTerms("monitor hz refresh target interpolation"));

    // Only consulted in Capped mode; the named reason states exactly that when greyed.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Target FPS", .cVar = "gEnhancements.Graphics.InterpTargetFps",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(60)
                               .Max(480)
                               .DefaultValue(120)
                               .Format("%d FPS")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Interpolation target frame rate. Values above your refresh rate\n"
                                        "waste GPU without improving output. Each 60fps of target adds a\n"
                                        "full render pass per tick."))
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .DisableWhen({ GdxUI::DISABLE_FOR_MATCH_REFRESH_RATE_ON })
                  .SearchTerms("frame rate cap interpolation target"));

    // Decides what interpolation covers rather than whether it runs, so it is hidden while
    // interpolation is off. With it off, the camera and the whole track sit at 60 Hz while machines
    // tween against them, separating CPU-baked effects (booster flames) from their machines.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Interpolate Camera", .cVar = "gEnhancements.Graphics.InterpolateCamera",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Smooths the camera and the track, not just the machines.\n"
                      "Turning this off interpolates vehicles against a static world,\n"
                      "which makes engine effects appear to separate from the machines.\n"
                      "Leave on unless you are comparing the two."))
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .SearchTerms("camera track projection interpolation smooth"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Mirror mode", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "FLX reflection quality", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Gameplay
    // ═════════════════════════════════════════════════════════════════════════════════════════

    // transition.c Transition_Update re-runs its same per-tick logic in one call until finished (up
    // to 128x), so screen wipes resolve near-instantly. The per-tick switch itself is unchanged;
    // only the surrounding loop budget differs. [PB].
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Skip/shorten transitions", .cVar = "gEnhancements.Gameplay.SkippableTransitions",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Fast-completes screen-transition wipes instead of playing them in full.\n"
                      "Off by default (parity)."))
                  .ModifiedMarker()
                  .SearchTerms("wipe fade loading speed"));

    // Community request F1: stock pins AI racers to machineSkinIndex 0 in GP/Practice/Death
    // Race (func_80089800, racer.c); VS already randomizes. Hook is in the roster fill.
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Random opponent colors", .cVar = "gEnhancements.Gameplay.RandomOpponentColors",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Grand Prix, Practice and Death Race opponents get a random one of\n"
                      "their 4 machine colors every race (stock always uses the default).\n"
                      "VS mode already did this; this extends it everywhere."))
                  .ModifiedMarker()
                  .SearchTerms("opponent ai rival color skin palette randomize variety"));

    // Community request F3: handpicked 29-opponent grid. The roster is one string CVar parsed
    // by gdx_custom_grid.c; func_80089800 (racer.c) applies it over the default fill and
    // bypasses the stock grid shuffle while it is set, so slot order is the grid order.
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Custom grid", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { DrawCustomGrid(); })
                  .SearchTerms("custom grid opponents roster handpick machine color skin starting position pole"));

    // Community request F5: Cpu_GenerateInputs (ovl_i3/cpu.c) scales the spin/side-attack
    // and retaliation probabilities by this percent; past 100 the EXPERT-only combat gates
    // drop, so lower difficulties join in. 100 = stock.
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "AI aggression (%)", .cVar = "gEnhancements.Gameplay.AiAggression",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(100)
                               .Max(400)
                               .DefaultValue(100)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("How combative CPU racers are in every mode. 100% = stock.\n"
                                        "Above 100% they attack more often and no longer wait\n"
                                        "for EXPERT difficulty to fight back."))
                  .ModifiedMarker()
                  .SearchTerms("ai cpu opponent aggressive attack combat difficulty mutator"));

    // Decision M3: shareable X Cup seed codes. The pending seed is one-shot runtime state, so
    // this stays a custom block (InputText + queue button + current-code display), not a CVar.
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "X Cup seed code", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawXCupSeed(); })
                  .SearchTerms("x cup seed code share random generated track daily"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Mouse
    // ═════════════════════════════════════════════════════════════════════════════════════════

    // DefaultValue must equal the CVarRegisterInteger defaults in the GdxMenu ctor
    // (0/0/100/1/0), or the "changed from stock" asterisk lies. The decomp hook is the #ifdef
    // PORT block in both cursor drivers of course_edit/188850.c, via
    // port/gdx_course_edit_mouse.cpp.
    AddWidget("Enhancements", "Mouse", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Course Edit / Create Machine: mouse cursor",
                          .cVar = "gEnhancements.Input.CourseEditMouse",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Drives the in-game cursor with the mouse in Course Edit and\n"
                      "Create Machine instead of the stick. Requires pointing inside\n"
                      "the game view; outside it the stick drives as stock. The\n"
                      "cursor speed slider is inert while the mouse drives. Grabbing\n"
                      "(Z) keeps stock stick behavior. LMB/RMB act as A/B."))
                  .ModifiedMarker()
                  .SearchTerms("course edit create machine mouse cursor pointer absolute track editor"));

    // Issue #18. Absolute position steering (cursor X inside the game view maps to stick
    // deflection), a separate mechanism from the editor absolute drive above; gated on race
    // modes, so the two are mode-disjoint and never active together.
    AddWidget("Enhancements", "Mouse", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Mouse steering (races)",
                          .cVar = "gEnhancements.Input.MouseSteering",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Steers player 1 by mouse position in races (GP, Practice, VS,\n"
                      "Time Attack, Death Race): center of the screen is straight,\n"
                      "the edges are full lock. Keep the cursor where you want to\n"
                      "steer; park it in the middle to go straight. If the mouse is\n"
                      "untouched for a moment, the controller stick takes over.\n"
                      "Never active in Course Edit / Create Machine, where the\n"
                      "mouse cursor control above applies instead."))
                  .ModifiedMarker()
                  .SearchTerms("mouse steering race analog stick absolute position issue 18"));

    AddWidget("Enhancements", "Mouse", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Mouse steering sensitivity (%)",
                          .cVar = "gEnhancements.Input.MouseSteeringSensitivity",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(10)
                               .Max(600)
                               .DefaultValue(100)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Scales steering deflection. 100 = full lock at the\n"
                                        "edge of the game view; higher values reach full lock\n"
                                        "closer to the center, lower values soften steering."))
                  .ModifiedMarker()
                  .SearchTerms("mouse steering sensitivity speed percent"));

    AddWidget("Enhancements", "Mouse", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Confine mouse to window",
                          .cVar = "gEnhancements.Input.MouseConfineToWindow",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Clips the OS cursor to the game window while editor mouse\n"
                      "control or race mouse steering is active, so the cursor\n"
                      "cannot drift off the game view while steering."))
                  .ModifiedMarker()
                  .SearchTerms("mouse confine clip grab window cursor steering"));

    AddWidget("Enhancements", "Mouse", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Hide cursor during gameplay",
                          .cVar = "gEnhancements.Input.HideCursorInGame",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Hides the OS mouse cursor while the game renders. The cursor\n"
                      "comes back whenever this menu is open."))
                  .ModifiedMarker()
                  .SearchTerms("hide cursor os pointer gameplay invisible"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Course Edit & Machine (also covers Create Machine)
    // ═════════════════════════════════════════════════════════════════════════════════════════

    // DefaultValue must track the registration and the ReduceEditorFlashingOn migration in the
    // GdxMenu ctor, or the "changed from stock" asterisk lies.
    AddWidget("Enhancements", "Course Edit & Machine", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Reduce Course Edit flashing",
                          .cVar = "gEnhancements.Gameplay.ReduceEditorFlashing",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Halves the Course Edit blink/checker cadence. The 20Hz strobe is\n"
                      "authentic N64 behavior; this calms it on modern displays."))
                  .ModifiedMarker()
                  .SearchTerms("strobe epilepsy photosensitive blink editor accessibility"));

    AddWidget("Enhancements", "Course Edit & Machine", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Extended course height (experimental)",
                          .cVar = "gEnhancements.CourseEdit.ExtendedHeight",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Raises the Course Edit height ceiling from 5,000 to 30,000.\n"
                      "Tracks above 5,000 will not load on real hardware or in other\n"
                      "Expansion Kit tools. Experimental: extreme-height camera,\n"
                      "collision, fog, and chunk-budget behavior still needs validation."))
                  .ModifiedMarker()
                  .SearchTerms("course edit extended height altitude experimental limit 30000"));

    // Gates the PORT branch in course_edit/19C470.c func_xk2_800EB3B4: B inside the
    // OFFICIAL course list reopens the parent picker instead of closing the File menu.
    AddWidget("Enhancements", "Course Edit & Machine", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Course Edit: OFFICIAL Back returns to list",
                          .cVar = "gEnhancements.Gameplay.CourseEditOfficialBack",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "In Course Edit's File > Load, backing out of the OFFICIAL course\n"
                      "list reopens the track list (OFFICIAL + your custom tracks)\n"
                      "instead of closing the menu. Off restores stock behavior."))
                  .ModifiedMarker()
                  .SearchTerms("course edit official back load picker file menu"));

    // Index 0 = follow last machine-select choice; 1-30 pin roster machines 0-29. Super and
    // custom machines stay reachable through "Last used". Skin always follows last used.
    AddWidget("Enhancements", "Course Edit & Machine", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Course Edit: test-drive machine",
                          .cVar = "gEnhancements.Gameplay.TestDriveMachine",
                          .type = GdxUI::WIDGET_CVAR_COMBOBOX }
                  .ComboItems({
                      "Last used (incl. super/custom)", // 0: gPlayerCharacters[0]
                      "Blue Falcon",   "Golden Fox",      "Wild Goose",      "Fire Stingray", "White Cat",
                      "Red Gazelle",   "Great Star",      "Iron Tiger",      "Deep Claw",     "Twin Noritta",
                      "Super Piranha", "Mighty Hurricane","Little Wyvern",   "Space Angler",  "Green Panther",
                      "Black Bull",    "Wild Boar",       "Astro Robin",     "King Meteor",   "Queen Meteor",
                      "Wonder Wasp",   "Hyper Speeder",   "Death Anchor",    "Crazy Bear",    "Night Thunder",
                      "Big Fang",      "Mighty Typhoon",  "Mad Wolf",        "Sonic Phantom", "Blood Hawk",
                  })
                  .Options(UIWidgets::ComboboxOptions()
                               .DefaultIndex(0)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Machine used for Course Edit test drives. Stock always uses\n"
                                        "Blue Falcon. Color follows your last machine-select choice."))
                  .ModifiedMarker()
                  .SearchTerms("course edit test drive machine vehicle blue falcon"));

    AddWidget("Enhancements", "Course Edit & Machine", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Course Edit: test-drive engine (%)",
                          .cVar = "gEnhancements.Gameplay.TestDriveEngine",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(0)
                               .Max(100)
                               .DefaultValue(50)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Engine slider for Course Edit test drives. Stock uses 50%."))
                  .ModifiedMarker()
                  .SearchTerms("course edit test drive engine slider percent speed"));

    // VENUE_ENDING is fully wired in the engine (own texture segment, Mute City floor/BGM) but
    // stock keeps it out of the editor. Default on: the ending scene is fully wired and is
    // expected in the port's Course Edit experience; the toggle remains for stock parity.
    AddWidget("Enhancements", "Course Edit & Machine", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Course Edit: Ending scene option",
                          .cVar = "gEnhancements.Gameplay.CourseEditEndingVenue",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Adds an 11th scene (\"Ending\") to Course Edit's Background menu.\n"
                      "Uses the ending ceremony's venue textures with Mute City's floor and\n"
                      "BGM. Off = stock 10 scenes."))
                  .ModifiedMarker()
                  .SearchTerms("course edit ending scene venue background scenery"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Cosmetics
    // ═════════════════════════════════════════════════════════════════════════════════════════

    // Community request F9: packed-RGB overrides substituted at the hardcoded env-color /
    // highlight sites in racer.c. -1 = stock; the picker shows stock while disabled.
    AddWidget("Enhancements", "Cosmetics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Effect colors", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Enhancements", "Cosmetics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Boost trail (active)", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) {
                      DrawEffectColorOverride("gEnhancements.Gameplay.BoostColor", "Boost trail (active)", 0x5BFF5B);
                  })
                  .SearchTerms("boost green color effect custom"));
    AddWidget("Enhancements", "Cosmetics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Boost trail (dash pad)", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) {
                      DrawEffectColorOverride("gEnhancements.Gameplay.DashPadColor", "Boost trail (dash pad)", 0xFFDF00);
                  })
                  .SearchTerms("dash pad yellow boost color effect custom"));
    AddWidget("Enhancements", "Cosmetics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Boost trail (idle)", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) {
                      DrawEffectColorOverride("gEnhancements.Gameplay.BoostIdleColor", "Boost trail (idle)", 0x00FFFF);
                  })
                  .SearchTerms("idle cyan booster color effect custom"));
    AddWidget("Enhancements", "Cosmetics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Side-attack glow", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) {
                      DrawEffectColorOverride("gEnhancements.Gameplay.SideAttackColor", "Side-attack glow", 0xFF0000);
                  })
                  .SearchTerms("side attack red highlight color effect custom"));

    // Community request F2: per-machine palette overrides. Persistence is palette.txt next to
    // the exe (no CVars); the decomp hook at the end of func_8008D33C (racer.c) re-applies the
    // table every time gMachines is rebuilt, and the editor re-applies live on every edit.
    AddWidget("Enhancements", "Cosmetics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Machine palette editor", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Enhancements", "Cosmetics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Machine colors", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { DrawMachinePaletteEditor(); })
                  .SearchTerms("machine color palette editor paint rgb custom skin blue falcon"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Practice
    // ═════════════════════════════════════════════════════════════════════════════════════════

    // Drawn in hud.c under #ifdef PORT: the last completed lap against the session best, or a
    // loaded ghost's same lap once ghosts populate outside Time Attack.
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show lap deltas", .cVar = "gEnhancements.Practice.ShowLapDeltas",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "In Practice mode, shows your last lap vs your session best\n"
                      "(green = faster, red = slower). Off by default."))
                  .ModifiedMarker()
                  .SearchTerms("split time comparison lap delta"));

    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });

    // Available in every race mode: pausing then suppresses all race HUD/pause overlays and
    // reserves their controls for the free camera. Unpausing restores the game camera exactly.
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Photo mode (free camera)", .cVar = "gEnhancements.Practice.PhotoMode",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Pause during a race to hide the HUD and free-fly the camera.\n"
                      "Stick: dolly/truck  -  C-buttons: look  -  L/R: FOV  -  hold Z: raise/lower.\n"
                      "Unpausing or turning this off restores the game camera exactly. Off by default."))
                  .ModifiedMarker()
                  .SearchTerms("photo camera free fly screenshot fov"));

    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Replay theater", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Diagnostic overlay", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Ghosts
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Enhancements", "Ghosts", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost Browser", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Ghost Browser",
                                         "Manage multiple local and imported player ghosts per exact course and "
                                         "select up to three Time Attack opponents. Staff ghosts remain "
                                         "controlled by the base game.");
                  })
                  .SearchTerms("ghost library opponents time attack staff player import export"));

    // SCOPE: stock F-Zero X ALREADY commits numeric records (best times / best lap / max speed /
    // death-race stats) to SRAM on finishing a race (menus.c:252-268), and the port's SRAM is
    // write-through to fzerox.sav — those autosave regardless of this toggle. What it adds is
    // auto-persisting the best GHOST replay, which stock saves only via the manual "Save Ghost"
    // prompt (menus.c:2085-2101 / 2562-2581), so quitting before that prompt loses the ghost.
    AddWidget("Enhancements", "Ghosts", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Autosave ghost on new record", .cVar = "gEnhancements.Gameplay.AutosaveOnRecord",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Auto-save your best Time Attack ghost replay when you beat it,\n"
                      "without the manual Save-Ghost prompt.\n"
                      "(Record TIMES already autosave in stock F-Zero X.) Off by default."))
                  .ModifiedMarker()
                  .SearchTerms("ghost replay save record time attack"));

    AddWidget("Enhancements", "Ghosts", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost boost trail", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) {
                      DrawEffectColorOverride("gEnhancements.Gameplay.GhostBoostColor", "Ghost boost trail", 0xFF00FF);
                  })
                  .SearchTerms("ghost purple magenta time attack boost color effect custom"));

    AddWidget("Enhancements", "Ghosts", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost Replays", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Enhancements", "Ghosts", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost replay (.gdg)", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawGhostIo(); })
                  .SearchTerms("export import ghost gdg replay file share"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Achievements
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Enhancements", "Achievements", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Achievements", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // F10: master gate for the tracker (port/gdx_achievements.cpp). Unlocks and playtime persist
    // in saves/achievements.txt; turning this off stops evaluation and playtime accumulation but
    // keeps the stored record visible below.
    AddWidget("Enhancements", "Achievements", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Track achievements and playtime", .cVar = "gEnhancements.Achievements.Enabled",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Unlocks local achievements as you clear cups, beat staff ghosts and set\n"
                      "records, and counts playtime (time with this menu open is not counted).\n"
                      "Stored locally in saves/achievements.txt; nothing is sent anywhere."))
                  .ModifiedMarker()
                  .SearchTerms("achievements trophies playtime tracker local"));

    AddWidget("Enhancements", "Achievements", SECTION_COLUMN_1,
              WidgetInfo{ .name = "AchievementList", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { DrawAchievements(); })
                  .SearchTerms("achievement list unlock progress playtime checklist"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // WORKSHOP -> Mods
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Texture Packs", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable texture packs", .cVar = "gEnhancements.Workshop.TexturePacks",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Overrides game textures from mods/*.o2r packs.\nOff = stock rendering."))
                  .ModifiedMarker()
                  .SearchTerms("mods o2r hi-res retexture override"));
    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable sequence packs", .cVar = "gEnhancements.Workshop.SequencePacks",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Overrides game music from mods/*.o2r packs (audio/seq/<name> raw aseq bytes).\n"
                      "Applies on the next song/scene load and after Reload packs; sequences\n"
                      "already on the permanent heap cannot be retro-swapped mid-scene.\n"
                      "Off = stock music."))
                  .ModifiedMarker()
                  .SearchTerms("mods o2r music bgm sequence audio override song"));
    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable sample packs", .cVar = "gEnhancements.Workshop.SamplePacks",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Overrides individual instrument/sfx samples from mods/*.o2r packs\n"
                      "(audio/sample/<bank>__0x<offset>__<size> GSMP containers).\n"
                      "Applies at the next scene/song load and after Reload packs; fonts already\n"
                      "converted cannot be retro-swapped mid-scene.\n"
                      "Off = stock samples."))
                  .ModifiedMarker()
                  .SearchTerms("mods o2r sample audio override adpcm instrument sfx soundfont"));
    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable soundfont packs", .cVar = "gEnhancements.Workshop.SoundfontPacks",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Overrides whole instrument graphs from mods/*.o2r packs\n"
                      "(audio/font/<FONTNAME> GFT1 containers).\n"
                      "Applies when each font is converted at load time; Reload packs re-applies\n"
                      "present overlays and restores the stock font for removed ones.\n"
                      "Off = stock instruments."))
                  .ModifiedMarker()
                  .SearchTerms("mods o2r soundfont font audio override instrument adpcm"));

    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Lua Scripts", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable Lua script packs", .cVar = "gEnhancements.Workshop.Scripts",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Runs scripts/*.lua from enabled mods/*.o2r packs, one sandboxed state\n"
                      "per script. v1 is read-only: scripts can log and read race state\n"
                      "(gdx.frame/mode/racer) and receive race callbacks, nothing more.\n"
                      "Applies on enable and after Reload packs. Off = no scripts run."))
                  .ModifiedMarker()
                  .SearchTerms("mods o2r lua scripts scripting sandbox workshop"));

    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable model packs", .cVar = "gEnhancements.Workshop.ModelPacks",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Replaces stock machine models from mods/*.o2r packs\n"
                      "(models/pack/machine/<name>/lod<N> display lists; name = blue_falcon\n"
                      "style, N = 1..5). Custom (editor) machines are never overridden.\n"
                      "Applies on the next machine/screen entry and after Reload packs.\n"
                      "Off = stock models."))
                  .ModifiedMarker()
                  .SearchTerms("mods o2r model machine lod display list override workshop"));

    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Installed texture packs", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawTexturePacks(); })
                  .SearchTerms("packs list reload mods folder manifest priority order"));
    AddWidget("Workshop", "Mods", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Installed scripts", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawScripts(); })
                  .SearchTerms("lua scripts list status error mods packs"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // WORKSHOP -> Tools
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Workshop", "Tools", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Asset Dump", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Tools", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Asset Dump", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawAssetDump(); })
                  .SearchTerms("dump extract textures assets classes folder gdx-extract"));

    AddWidget("Workshop", "Tools", SECTION_COLUMN_1,
              WidgetInfo{ .name = "DD Save (64DD sidecar)", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Tools", SECTION_COLUMN_1,
              WidgetInfo{ .name = "DD Save", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawDdSave(); })
                  .SearchTerms("64dd disk save sidecar mfs format journal course edit machine create"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // WORKSHOP -> Content Library
    // GdxContentWindow is registered in main.cpp at boot under the name "Content Library"; this
    // page embeds it, or pops it out.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Workshop", "Content Library", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Content Library", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Content Library",
                                         "List the disk's tracks and machines, export them to "
                                         "exports/<name>.gdxc next to the executable, install .gdxc "
                                         "files back onto the disk, register tracks into the Edit "
                                         "Cup, and export/import the whole cup as one bundle.");
                  })
                  .SearchTerms("content library export gdxc track machine course share"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // WORKSHOP -> ROM Hacks
    // GdxHackModsWindow is registered in main.cpp at boot under the name "ROM Hacks"; this page
    // embeds it, or pops it out. Deliberately its own page rather than a row inside Mods: a hack
    // replaces the game's content, a texture pack layers over it, and mixing the two in one list
    // invites exactly the mistake the single-selection design exists to prevent.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Workshop", "ROM Hacks", SECTION_COLUMN_1,
              WidgetInfo{ .name = "ROM Hacks", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("ROM Hacks",
                                         "Run a ROM hack packaged as an .o2r archive from "
                                         "mods/~romhacks/. One hack at a time, and it gets its own "
                                         "save file, so stock progress is never touched. A change "
                                         "here takes effect on the next launch.");
                  })
                  .SearchTerms("rom hack romhacks o2r archive mod save isolation stock restart"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ONLINE -> Overview
    // ═════════════════════════════════════════════════════════════════════════════════════════
    // A privacy setting, framed as one: master default OFF, and each field the presence may publish
    // has its own opt-out. Everything the toggles govern is built in port/gdx_discord.cpp.
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Discord Rich Presence", .cVar = "gEnhancements.Online.DiscordPresence",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Shows what you are playing on your Discord profile: game mode, course,\n"
                      "lap and position, per the checkboxes below. Sent only to Discord, only\n"
                      "while this is on. Turning it off clears the presence immediately.\n"
                      "Course Edit never shows your track names."))
                  .ModifiedMarker()
                  .SearchTerms("discord rich presence status privacy online activity"));
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show course", .cVar = "gEnhancements.Online.DiscordShowCourse",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Include the course name (e.g. Big Blue) in the presence."))
                  .SearchTerms("discord course track name"));
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show lap", .cVar = "gEnhancements.Online.DiscordShowLap",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Include the current lap (e.g. Lap 2/3) in the presence."))
                  .SearchTerms("discord lap"));
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show race position", .cVar = "gEnhancements.Online.DiscordShowPosition",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Include your current position (e.g. P4) in the presence."))
                  .SearchTerms("discord position rank place"));
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show mode and cup", .cVar = "gEnhancements.Online.DiscordShowMode",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Include the game mode and cup (e.g. Grand Prix, Jack Cup - Expert)."))
                  .SearchTerms("discord mode cup difficulty grand prix"));
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show race timer", .cVar = "gEnhancements.Online.DiscordShowTimer",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Show a live elapsed timer for the CURRENT RACE only. Session length\n"
                      "is never shown."))
                  .SearchTerms("discord timer elapsed"));

    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Leaderboards (per course)", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost upload / download", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Netplay lobbies (after decision gate)", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Spectator / director cam", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // DEV TOOLS -> General
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Developer tools can be embedded in this menu or popped out into "
                                  "independent windows.",
                          .type = GdxUI::WIDGET_TEXT });
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "A popped-out window opens on top of this menu; close the menu (F1) to use it.",
                          .type = GdxUI::WIDGET_TEXT_DISABLED });
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Developer tool windows", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawDevToolButtons(); })
                  .SearchTerms("stats console gfx debugger open window popout"));

    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Windowing", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // The ImGui viewport flag is applied ONCE at Gui::Init(), so flipping the CVar persists the
    // preference but only takes effect after a restart — hence the note, and no poke at
    // ImGui::GetIO() here.
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Multi-viewport docking", .cVar = "gEnableMultiViewports",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Lets popped-out tool windows leave the main window (multi-monitor docking).\n"
                      "Applies on restart."))
                  .Note("(restart)")
                  .SearchTerms("docking monitor viewport window detach"));

    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Shader cache", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // An escape hatch rather than a preference, hence Dev Tools. The cache is read once at renderer
    // init (gfx_direct3d11.cpp / gfx_opengl.cpp Init), so the CVar cannot take effect mid-session;
    // GDX_SHADER_CACHE=0 does the same for a single run without persisting.
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Reuse compiled shaders across runs", .cVar = "gDevTools.ShaderCache",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Stores compiled shaders next to the executable so each one is built once per\n"
                      "install instead of once per launch. Building them costs 9-15ms each and they\n"
                      "arrive in bursts, which is what produced the large frame stalls on first\n"
                      "visiting a venue.\n\n"
                      "Turn this off only to rule the cache out while diagnosing a rendering fault.\n"
                      "To rebuild it from scratch, delete gdiffuser-shadercache-*.bin.\n"
                      "Applies on restart."))
                  .Note("(restart)")
                  .SearchTerms("shader cache stutter hitching compile pipeline"));

    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Developer gates", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawDevGates(); })
                  .SearchTerms("gdx environment variables logging diagnostics behavior overrides gates env"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // DEV TOOLS -> Stats / Console / Gfx Debugger
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show FPS counter overlay", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mFpsCounterVisible)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Toggles a small always-on-top frames-per-second overlay."))
                  .PreFunc([this](WidgetInfo&) { mFpsCounterVisible = GdxWindowVisible("FPS Counter"); })
                  .Callback([](WidgetInfo&) { GdxToggleWindow("FPS Counter"); })
                  .Note("Uses the same frame metrics as Stats")
                  .SearchTerms("fps counter framerate overlay"));
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Frame interpolation statistics", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawInterpStats(); })
                  .SearchTerms("presented fps subframes interpolated snapped sim"));
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Stats window", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Stats", "Live frame timing and renderer statistics.");
                  })
                  .SearchTerms("frame timing renderer statistics"));

    AddWidget("Dev Tools", "Console", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Console", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Console", "Port log, developer console and command history.");
                  })
                  .SearchTerms("log command history reset"));

    AddWidget("Dev Tools", "Gfx Debugger", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Gfx Debugger", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Gfx Debugger",
                                         "Inspect Fast3D display-list execution and rendering state.");
                  })
                  .SearchTerms("display list fast3d rendering debug"));
}
