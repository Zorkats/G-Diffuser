// port/gdx_menu.cpp — implementation of the G-Diffuser modern full-screen menu.
//
// See gdx_menu.h for the design. This file is the SHELL and the draw dispatcher: the window, header
// tab strip, sidebar, content pane, search box and quit modal; MenuDrawItem(), which turns one
// registered GdxUI::WidgetInfo into ImGui calls; DrawSearchResults(); and the WIDGET_CUSTOM blocks
// that no generic widget can express. The CONTENTS live in port/gdx_menu_registry.cpp.
//
// CVar NAMES ARE STRING LITERALS ON PURPOSE
// -----------------------------------------
// libultraship defines CVAR_* macros (e.g. CVAR_MENU_BAR_OPEN) in cmake/cvars.cmake, but that file
// is include()d only inside libultraship/src (libultraship/src/CMakeLists.txt:1), so its
// add_compile_definitions() do NOT reach the port/ target. Each literal below matches cvars.cmake
// exactly. The port's own knobs use the gEnhancements.* convention.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // ShellExecuteA for the Workshop "Open folder" buttons
#include <shellapi.h> // ShellExecuteA (excluded by WIN32_LEAN_AND_MEAN)
#endif

#include "gdx_menu.h"
#include "gdx_menu_internal.h" // the helpers below, shared with port/gdx_menu_registry.cpp

#include <imgui.h>

#include "ship/Context.h"           // Ship::Context::GetInstance()
#include "ship/window/Window.h"     // Ship::Window::GetGui() + the SetResolutionMultiplier/
                                    // SetMsaaLevel virtuals used to apply the graphics knobs live
#include "ship/window/gui/Gui.h"    // Ship::Gui::{GetGuiWindow, SaveConsoleVariablesNextFrame}
#include "ship/window/gui/IconsFontAwesome4.h"
#include "fast/Fast3dWindow.h"      // Fast::Fast3dWindow::SetTextureFilter + Fast::FilteringMode
#include "fast/Fast3dGui.h"         // About page: LoadTextureFromRawImage + GetTextureByName (logo)
                                    // (the texture-filter setter is Fast3d-only, not on the base
                                    // Ship::Window, so it needs a downcast — see DrawGraphicsMenu)
#include "ship/resource/ResourceManager.h"       // Data & Files: ResourceManager::GetArchiveManager
#include "ship/resource/archive/ArchiveManager.h" // Data & Files: ArchiveManager::GetArchives
#include "ship/resource/archive/Archive.h"        // Data & Files: Archive::GetPath (basename match)

#include "libultraship/bridge/consolevariablebridge.h" // CVarGet/Set/Register*
#include "libultraship/bridge/audiobridge.h"           // AudioPlayerBuffered (Audio tab status line)

#include <cstring> // strcmp (Audio tab: SDL driver-name check)

// Declared here rather than pulling in <SDL2/SDL.h>: this TU builds inside libultraship's include
// environment, where the SDL umbrella clashes. Signature matches SDL_audio.h exactly.
extern "C" const char* SDL_GetCurrentAudioDriver(void);

#include <algorithm>
#include <cctype>
#include <cmath>  // std::sin (the search-navigation highlight pulse in MenuDrawItem)
#include <cstdio> // snprintf (Practice-tab ghost import/export status line)
#include <cstdlib> // std::system (non-Windows open-folder fallback)
#include <memory> // std::dynamic_pointer_cast (null-safe downcast to Fast::Fast3dWindow)
#include <string>

#include "ui/UIWidgets.hpp" // CVar-bound ImGui widgets: read + draw + write + persist + tooltip in
                           // one call

#include "gdx_console_log.h" // Console page: drains the queued port-log lines into the LUS console
#include "gdx_ghost_io.h" // .gdg ghost import/export C API (Practice tab Export / Import buttons)
#include "gdx_xcup_seed.h" // M3 X Cup seed codes (Gameplay tab custom block)
#include "gdx_gui.h"
#include "gdx_workshop.h"    // Workshop tab: texture-pack listing, override count, reload, dump dir
#include "gdx_lua.h"         // Workshop tab: Lua script pack status list (DrawScripts)
#include "gdx_dump_launch.h" // Workshop tab "Asset Dump" section: per-class offline dump launcher
#include "disk_savefile.h"   // Workshop tab "DD Save" subsection: sidecar status + one-shot format
#include "rom_buffer.h"      // Data & Files: gdx_rom_buffer/gdx_rom_path (live ROM residency signal)
#include "gdx_firstboot.h"   // Data & Files: canonical file names + gdx::ManagedDiskPath
#include "gdx_segment_source.h" // Data & Files: archive-coverage telemetry (fallback counters)
#include "gdx_dev_gates.h"   // Dev Tools: the developer-gate table driving DrawDevGates()

#include <vector>
#include <filesystem>

// From port/input_bridge.c: nonzero while an on-track race is live. The ghost Import writes to the
// SRAM ghost slot, which must not race the game fiber, so the Import button is disabled in-race.
extern "C" int gdx_input_in_gameplay(void);
extern "C" void gdx_game_request_reset(void);
// Deletion-gate verdict (port/disk_buffer.cpp). 1 iff this boot reconstructed the EK disk from
// fzerox-disk.o2r AND proved it byte-identical to the managed copy. The Data & Files panel
// offers disk deletion ONLY on a passed verdict; it never deletes anything itself.
extern "C" int gdx_disk_archive_verified(void);

// Frame-interpolation telemetry for the Stats page. Declared here rather than pulling in
// n64_gfx_bridge.h — signatures match the header exactly.
extern "C" int gdx_gfx_interp_last_subframes(void);
extern "C" double gdx_gfx_interp_last_t(void);
extern "C" int gdx_gfx_interp_host_active(void);
extern "C" double gdx_gfx_interp_presents_per_sec(void);
extern "C" int gdx_gfx_interp_last_lerped(void);
extern "C" int gdx_gfx_interp_last_snapped(void);
// Per-tick truth: main.cpp forces interpolation off for a tick (Course Edit / Create Machine
// editors) while the raw CVar above is still on, so the Stats block must show the paused truth
// rather than the stale live numbers.
extern "C" int gdx_gfx_interp_tick_active(void);

// Small helpers, main-thread only (the whole menu draws inside Gui::StartDraw/EndDraw). A named
// namespace declared in port/gdx_menu_internal.h rather than an anonymous one, because
// port/gdx_menu_registry.cpp calls these too and a second copy there would drift.

namespace gdxmenu {

// Returns the live Gui, or nullptr if the window/gui is not up yet (defensive; the menu only
// draws once the Gui exists, so this is essentially always non-null while visible).
std::shared_ptr<Ship::Gui> GdxGui() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    auto window = ctx->GetWindow();
    if (window == nullptr) {
        return nullptr;
    }
    return window->GetGui();
}

// Returns the live top-level window, or nullptr if it is not up yet. SetResolutionMultiplier and
// SetMsaaLevel are virtuals on the Ship::Window base (Window.h:140,145), so this is enough for
// them; SetTextureFilter is Fast3d-only and needs the downcast helper below.
std::shared_ptr<Ship::Window> GdxWindow() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    return ctx->GetWindow();
}

// The window downcast to Fast::Fast3dWindow, or nullptr on any non-Fast3d backend — callers then
// skip the live apply and the saved CVar takes effect on the next restart. Needed only for
// SetTextureFilter, which takes a Fast::FilteringMode the Ship::Window base does not know.
std::shared_ptr<Fast::Fast3dWindow> GdxFast3dWindow() {
    return std::dynamic_pointer_cast<Fast::Fast3dWindow>(GdxWindow());
}

// Schedules a CVar flush to gdiffuser.cfg.json at end of frame (coalesced — safe to call often).
void GdxSaveCvars() {
    auto gui = GdxGui();
    if (gui != nullptr) {
        gui->SaveConsoleVariablesNextFrame();
    }
}

// Flips a registered GuiWindow's LIVE visibility by name. A bare CVarSetInteger on the visibility
// CVar is a NO-OP for an already-constructed window: it checks its in-memory mIsVisible each frame
// and reads the CVar only at construction. ToggleVisibility() flips mIsVisible AND mirrors +
// persists the CVar (GuiWindow.cpp).
// Returns false when the named window does not exist, so callers can say so instead of leaving a
// button that silently does nothing.
bool GdxToggleWindow(const char* name) {
    auto gui = GdxGui();
    if (gui == nullptr) {
        return false;
    }
    auto window = gui->GetGuiWindow(name);
    if (window == nullptr) {
        return false;
    }
    window->ToggleVisibility();

    // Gui::Draw() draws the full-screen menu BEFORE the tool windows, and libultraship registers
    // the Console with ImGuiWindowFlags_NoFocusOnAppearing, so a window turned on from inside the
    // menu appears unfocused and therefore underneath an opaque full-screen panel — genuinely
    // drawing, but indistinguishable from a dead button. Focusing it on the frame it becomes
    // visible puts it on top; on the way back to hidden there is nothing to focus.
    if (window->IsVisible()) {
        ImGui::SetWindowFocus(name);
    }
    return true;
}

// True if the named window exists and is currently shown (drives the menu-item checkmark).
bool GdxWindowVisible(const char* name) {
    auto gui = GdxGui();
    if (gui == nullptr) {
        return false;
    }
    auto window = gui->GetGuiWindow(name);
    return window != nullptr && window->IsVisible();
}

// A "Coming soon" roadmap line: a greyed, non-interactive entry naming a planned feature.
void GdxComingSoon(const char* label) {
    ImGui::TextDisabled("%s  -  Coming soon", label);
}

// Marks the item just submitted as differing from its stock default (the SoH "modified" cue). Call
// immediately after a widget, and after its own hover tooltip if any, so the SameLine anchors to
// that widget.
void GdxModifiedMarker(bool changed) {
    if (!changed) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.63f, 0.76f, 1.0f, 1.0f), "*");
    UIWidgets::Tooltip("Changed from the default (stock) value.");
}

// Opens a filesystem directory in the host file browser (Workshop "Open ... folder" buttons). The
// directory is created first if absent. Windows uses ShellExecute; other hosts fall back to xdg-open.
void GdxOpenFolder(const std::string& dir) {
    if (dir.empty()) {
        return;
    }
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open '" + dir + "' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

const ImVec4 kGdxBlue = ImVec4(0.035f, 0.25f, 0.82f, 1.0f);
const ImVec4 kGdxBlueHovered = ImVec4(0.055f, 0.31f, 0.96f, 1.0f);
const ImVec4 kGdxBlueActive = ImVec4(0.025f, 0.18f, 0.67f, 1.0f);

void GdxPushModernStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 3.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.93f, 0.97f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.55f, 0.57f, 0.64f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, kGdxBlue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGdxBlueHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGdxBlueActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.035f, 0.15f, 0.43f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.045f, 0.22f, 0.65f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.04f, 0.27f, 0.78f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.63f, 0.76f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.48f, 0.64f, 0.96f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.72f, 0.82f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, kGdxBlue);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kGdxBlueHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kGdxBlueActive);
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.63f, 0.65f, 0.72f, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.27f, 0.34f, 0.85f));
}

void GdxPopModernStyle() {
    ImGui::PopStyleColor(16);
    ImGui::PopStyleVar(8);
}

bool GdxNavigationButton(const char* label, bool selected, const ImVec2& size) {
    if (!selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    }
    const bool pressed = ImGui::Button(label, size);
    if (!selected) {
        ImGui::PopStyleColor();
    }
    return pressed;
}

std::string GdxLowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// "Data & Files" (General tab): live on-disk state for the three original setup inputs. This TU
// cannot reach disk_buffer.cpp's IPL/EK disk load state, so it reports filesystem + archive-mount
// facts rather than guessing which source was actually read.

// True if any mounted archive's filename (case-insensitive) matches `basename` exactly. Mirrors
// main.cpp's own basename-match pattern (main.cpp:427-431) for the version-gate scan.
bool GdxArchiveMounted(const char* basename) {
    auto ctx = Ship::Context::GetInstance();
    auto rm = (ctx != nullptr) ? ctx->GetResourceManager() : nullptr;
    auto am = (rm != nullptr) ? rm->GetArchiveManager() : nullptr;
    auto archives = (am != nullptr) ? am->GetArchives() : nullptr;
    if (archives == nullptr) {
        return false;
    }
    const std::string want = GdxLowercase(basename);
    for (const auto& archive : *archives) {
        if (archive == nullptr) {
            continue;
        }
        std::string name = GdxLowercase(std::filesystem::path(archive->GetPath()).filename().string());
        if (name == want) {
            return true;
        }
    }
    return false;
}

void GdxDrawDataAndFilesPanel() {
    if (!ImGui::CollapsingHeader("Data & Files")) {
        return; // collapsed by default -- this is diagnostic detail, not a control most players need
    }
    ImGui::TextWrapped(
        "What G-Diffuser currently has on disk for the three original setup inputs, and which of "
        "them are safe to delete once setup has completed.");
    ImGui::Spacing();

    // The working directory is the data directory: gdx_firstboot.cpp's FirstBootRun chdirs there at
    // boot. A failed query degrades to "." -- still correct, since every canonical name below is
    // looked up relative to the CWD either way.
    std::error_code dirEc;
    std::filesystem::path dataDir = std::filesystem::current_path(dirEc);
    if (dirEc) {
        dataDir = std::filesystem::path(".");
    }

    const ImVec4 kGdxOk = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 kGdxWarn = ImVec4(0.90f, 0.70f, 0.30f, 1.0f);
    const ImVec4 kGdxBad = ImVec4(1.0f, 0.40f, 0.40f, 1.0f);

    // gdx_rom_buffer non-null means the raw cartridge image is resident THIS session -- the one row
    // here with a true "in use" fact rather than an inference.
    ImGui::SeparatorText("F-Zero X ROM (.z64)");
    if (gdx_rom_buffer != nullptr) {
        ImGui::TextColored(kGdxWarn, "In use from: %s", gdx_rom_path[0] != '\0' ? gdx_rom_path : "(unknown path)");
    } else {
        ImGui::TextColored(kGdxOk, "Not loaded -- served from the game archive this session.");
    }
    ImGui::TextDisabled(
        "Deletable once setup has completed and Archive coverage below reads 0 fallbacks across a "
        "full play session.");

    // disk_buffer.cpp exposes no "which source is loaded" getter, so this reports the two
    // independently-checkable facts: is the original file present, and is the archive mounted.
    ImGui::SeparatorText("64DD IPL ROM (N64DDIPLROM.n64)");
    {
        std::error_code ec;
        bool iplFilePresent =
            std::filesystem::is_regular_file(dataDir / gdx::SetupIplFileName(), ec);
        ec.clear();
        // Mount state alone is misleading right after a first-boot extraction: the archive file is
        // on disk but only mounts on the next launch. Check file presence separately so the panel
        // never claims an existing archive is absent.
        bool iplArchivePresent = std::filesystem::is_regular_file(dataDir / "n64ddipl.o2r", ec);
        ec.clear();
        bool iplArchiveMounted = GdxArchiveMounted("n64ddipl.o2r");
        ImGui::Text("Original file: %s", iplFilePresent ? "present" : "not present");
        ImGui::Text("Archive (n64ddipl.o2r): %s",
                    iplArchiveMounted ? "mounted"
                    : iplArchivePresent ? "present (mounts on next launch)"
                                        : "not present");
        if (!iplFilePresent && iplArchiveMounted) {
            ImGui::TextColored(kGdxOk, "Served from the archive.");
        } else if (!iplFilePresent && iplArchivePresent && !iplArchiveMounted) {
            ImGui::TextColored(kGdxWarn, "Archive generated -- restart G-Diffuser to mount it.");
        } else if (!iplFilePresent && !iplArchivePresent) {
            ImGui::TextColored(kGdxBad, "Neither the file nor the archive is present -- do not delete anything here.");
        }
    }
    ImGui::TextDisabled("Deletable once setup has completed (the port never reads the IPL file again after that).");

    // The "deletable" line is shown ONLY on a passed deletion-gate verdict
    // (gdx_disk_archive_verified); otherwise the panel never suggests deletion.
    ImGui::SeparatorText("Expansion Kit disk (.ndd)");
    {
        std::error_code ec;
        bool diskFilePresent =
            std::filesystem::is_regular_file(dataDir / gdx::SetupDiskFileName(), ec);
        ec.clear();
        // The media/ managed copy was retired; a row for it only appears when a legacy install
        // still carries one, so current installs stop seeing a permanently-red "not present".
        std::string managedPath = gdx::ManagedDiskPath(dataDir.string());
        bool managedPresent = !managedPath.empty() && std::filesystem::is_regular_file(managedPath, ec);
        ec.clear();
        bool diskArchivePresent = std::filesystem::is_regular_file(dataDir / "fzerox-disk.o2r", ec);
        ec.clear();
        bool diskArchiveMounted = GdxArchiveMounted("fzerox-disk.o2r");
        bool archiveVerified = gdx_disk_archive_verified() != 0;
        ImGui::Text("Original file: %s", diskFilePresent ? "present" : "not present");
        if (managedPresent) {
            ImGui::Text("Managed copy (media/): present (legacy, no longer used)");
        }
        ImGui::Text("Archive (fzerox-disk.o2r): %s",
                    diskArchiveMounted ? "mounted"
                    : diskArchivePresent ? "present (mounts on next launch)"
                                         : "not present");
        if (archiveVerified) {
            ImGui::TextColored(kGdxOk,
                               "Disk archive: verified byte-identical -- the original disk is deletable.");
        } else if (diskArchiveMounted && !diskFilePresent) {
            ImGui::TextColored(kGdxOk, "Served from the archive.");
        } else if (diskArchivePresent && !diskArchiveMounted) {
            ImGui::TextColored(kGdxWarn, "Archive generated -- restart G-Diffuser to mount it.");
        } else if (!diskArchivePresent) {
            ImGui::TextColored(kGdxBad, "No archive yet -- do NOT delete the original disk.");
        }
    }
    ImGui::TextDisabled(
        "The original .ndd is deletable ONLY after the row above reads \"verified byte-identical\" "
        "(a boot reconstructed the disk from the archive and proved it matches). Your saves live "
        "in saves/*.gdd -- back those up regardless.");

    ImGui::Spacing();
    ImGui::SeparatorText("Archive coverage");
    unsigned int fallbackTotal = gdx_segment_source_fallback_total();
    if (fallbackTotal == 0) {
        ImGui::TextColored(kGdxOk, "Raw-ROM fallback reads this session: 0");
    } else {
        ImGui::TextColored(kGdxWarn, "Raw-ROM fallback reads this session: %u", fallbackTotal);
        ImFont* monoFont = GdxGuiFontMono(); // gdx_gui.h -- optional bundled mono font, null-safe
        if (monoFont != nullptr) {
            ImGui::PushFont(monoFont);
        }
        for (unsigned int i = 0;; ++i) {
            const char* key = nullptr;
            unsigned int count = 0;
            if (!GdxSegmentSourceFamilyStats(i, &key, &count)) {
                break;
            }
            if (count > 0) {
                ImGui::TextUnformatted((std::string("  ") + (key != nullptr ? key : "?") + ": " +
                                        std::to_string(count)).c_str());
            }
        }
        if (monoFont != nullptr) {
            ImGui::PopFont();
        }
    }
    ImGui::TextDisabled("Run with GDX_STRICT_ARCHIVE=1 during a soak to log every raw-ROM fallback.");
}

} // namespace gdxmenu

using namespace gdxmenu;

// "gOpenMenuBar" is the compatibility CVar libultraship's F1 / Esc / Gamepad-Back toggle flips, so
// binding to it makes those keys open this menu.
GdxMenu::GdxMenu() : Ship::GuiWindow("gOpenMenuBar", false, "G-Diffuser Menu") {
    CVarRegisterFloat("gSettings.Menu.BackgroundOpacity", 0.85f);
    // Persisted BY NAME (see gdx_menu.h): at worst it names a page that no longer exists, which
    // falls back to the section's first page.
    CVarRegisterString("gSettings.Menu.ActiveSection", "Settings");
    // libultraship's own CVar: a connected pad can both OPEN the menu (Gamepad Back) and navigate
    // it, and game input is blocked while the menu is up (Gui.cpp / ControlDeck.cpp). On by
    // default — essential on a handheld with no keyboard. Every CVarRegister* call in this ctor is
    // a no-op once the CVar has a stored value, so user settings are never clobbered.
    CVarRegisterInteger("gControlNav", 1);
    // One-time migration: a stored value beats the register default above, so configs written while
    // gamepad nav was still broken would keep it dead forever. The marker is written once, so a
    // later deliberate OFF stays intact. Same pattern for the two migrations below.
    if (CVarGetInteger("gdx.Migrations.ControlNavDefaultOn", 0) == 0) {
        CVarSetInteger("gdx.Migrations.ControlNavDefaultOn", 1);
        CVarSetInteger("gControlNav", 1);
        CVarSave();
    }
    // AUDIO. All live-read on the audio thread except BufferFrames, which InitAudio (main.cpp)
    // reads once — so a change to it applies on the next restart. LowPassHz is a cutoff in Hz and 0
    // disables the filter; MasterVolume 100 skips the gain multiply entirely, so the default is
    // bit-exact; Reverb governs the HLE path only, since LLE reverb is the ucode's own.
    //
    // The GRAPHICS controls bind to libultraship-owned g* CVars (gInternalResolution, gMSAAValue,
    // gTextureFilter, ...), which libultraship registers itself. Do NOT re-register those.
    CVarRegisterInteger("gEnhancements.Audio.LLE", 1);
    CVarRegisterInteger("gEnhancements.Audio.LowPassHz", 15000);
    CVarRegisterInteger("gEnhancements.Audio.MasterVolume", 100);
    CVarRegisterInteger("gEnhancements.Audio.Reverb", 1);
    CVarRegisterInteger("gEnhancements.Audio.BufferFrames", 4096);

    // GRAPHICS (port-owned; distinct from the libultraship g* CVars). Every default reproduces
    // today's rendering, except the two widescreen switches below. Widescreen is read live in
    // interpreter.cpp AdjXForAspectRatio: 1 = the current 16:9 hor+ output, 0 = 4:3 pillarbox.
    CVarRegisterInteger("gEnhancements.Graphics.Widescreen", 1);
    // Default ON, the second deliberate exception to "every default reproduces stock" (Widescreen
    // is the first): 3D widescreen behind a 4:3-placed HUD reads as a defect rather than a choice,
    // so the two switches ship together. Covers the true-corner 1P HUD, the full-width SELECT
    // MACHINE background and race transitions; other menu artwork stays 4:3.
    CVarRegisterInteger("gEnhancements.Graphics.WidescreenUI", 1);
    if (CVarGetInteger("gdx.Migrations.WidescreenUiDefaultOn", 0) == 0) {
        CVarSetInteger("gdx.Migrations.WidescreenUiDefaultOn", 1);
        CVarSetInteger("gEnhancements.Graphics.WidescreenUI", 1);
        CVarSave();
    }
    // Split-screen HUD anchoring (2P/3P/4P). gdx_widescreen_split_ui_active() (port/input_bridge.c)
    // requires gdx_widescreen_ui_active() too, so this is a strict subset and inert while the
    // switch above is off. It gets its own switch because the split HUD is authored to a per-column
    // grid rather than to screen edges, leaving a few mid-column elements (interval, reverse, the
    // 3P spare minimap) on the stock centred path — a layout judgment call worth its own opt-out.
    CVarRegisterInteger("gEnhancements.Graphics.WidescreenSplitUI", 1);
    // On (default), a Course Edit TEST DRIVE renders widescreen like a normal race; off keeps the
    // editor's 4:3 pin for the whole run (stock). port/input_bridge.c gdx_test_drive_active().
    CVarRegisterInteger("gEnhancements.Graphics.WidescreenTestDrive", 1);
    // UltrawideMode off is bit-exact 16:9 (every consumer collapses to an IEEE-exact 1.0 factor).
    // On, the game-side CPU culls (track chunks, racers, fireworks, stars) widen to the true frame
    // so content stops popping at the edges of 21:9/32:9 windows; 3D hor+ needs no switch because
    // it already works at any aspect.
    // HudMaxAspect 0 leaves ANCHOR-scoped HUD elements on the true screen corners at any aspect. A
    // value in [1.334, 8) confines the HUD to a centred band of that aspect instead, for 32:9 users
    // who find the corners too far apart.
    CVarRegisterInteger("gEnhancements.Graphics.UltrawideMode", 0);
    CVarRegisterFloat("gEnhancements.Graphics.HudMaxAspect", 0.0f);
    CVarRegisterInteger("gEnhancements.Graphics.HideRaceCurtain", 0);
    CVarRegisterInteger("gEnhancements.Graphics.CourseSelectClipOffscreen", 1);
    CVarRegisterInteger("gEnhancements.Graphics.RemoveBorders", 0);
    // CRTShader is read live per frame by the interpreter's StartFrame latch and forces the
    // offscreen render path when on, so a plain write takes effect on the next frame.
    CVarRegisterInteger("gEnhancements.Graphics.CRTShader", 0);
    CVarRegisterString("gEnhancements.Graphics.CustomShader", "");
    // PostPipeline holds the filename of an active .slangp preset in <exe>/shaders/pipelines/.
    // When non-empty it takes precedence over CRTShader/CustomShader; the latch parses/reloads
    // the preset each frame so edits on disk are picked up live.
    CVarRegisterString("gEnhancements.Graphics.PostPipeline", "");
    CVarRegisterInteger("gEnhancements.Graphics.DrawDistance", 100);
    CVarRegisterInteger("gEnhancements.Graphics.ForceMaxMachineLOD", 0);
    CVarRegisterInteger("gEnhancements.Graphics.MachineLODDistMod", 0);
    CVarRegisterInteger("gEnhancements.Graphics.ForceDrawMachineShad", 0);
    CVarRegisterInteger("gEnhancements.Graphics.MachineBoostLODDistMod", 0);
    CVarRegisterInteger("gEnhancements.Graphics.FancyLighting", 0);

    // FramePacing and FrameInterpolation are mutually exclusive pacing owners, both read live.
    // libultraship's Fast3D backend already caps the loop to ~60fps; FramePacing pins it to the N64
    // NTSC rate (~59.94Hz) instead, and wants VSync OFF to avoid beating against the display.
    CVarRegisterInteger("gEnhancements.Graphics.FramePacing", 0);
    CVarRegisterInteger("gEnhancements.Graphics.FrameInterpolation", 0);
    CVarRegisterInteger("gEnhancements.Graphics.InterpDebugOverlay", 0);
    // InterpTargetMode 0 follows the display (Ship::Window::GetCurrentRefreshRate()), 1 uses
    // InterpTargetFps. Both feed main.cpp's per-tick M derivation,
    // M = clamp(ceil(target/60), 1, kGdxInterpMaxSubframes).
    CVarRegisterInteger("gEnhancements.Graphics.InterpTargetMode", 0);
    CVarRegisterInteger("gEnhancements.Graphics.InterpTargetFps", 120);
    // Consulted only while FrameInterpolation is on, so this selects what interpolation MEANS
    // rather than whether it runs. race.c loads the combined projection*view camera with
    // G_MTX_PROJECTION and course.c emits no gSPMatrix at all, so with this off the camera AND the
    // whole track stay at 60 Hz while machines tween against a static world.
    CVarRegisterInteger("gEnhancements.Graphics.InterpolateCamera", 1);
    // InterpRigidBasis rescales element-wise-lerped per-racer basis rows back to rigid; without it
    // the machine's basis collapses up to 18% mid-tick (rowlen 0.0818 at t=0.5 against 0.10000 at
    // t=1), shearing the model for a sub-frame. InterpBasisJump freezes the previous keyframe's
    // rotation across a side attack's two model-basis discontinuities (racer.c:4556). Both default
    // 0 pending owner A/B and are pinned either way by GDX_INTERP_ROT_FIX / GDX_INTERP_BASIS_FIX;
    // they are registered so the console can reach them, since an env-var-only switch is not a
    // shipped fix.
    CVarRegisterInteger("gEnhancements.Graphics.InterpRigidBasis", 0);
    CVarRegisterInteger("gEnhancements.Graphics.InterpBasisJump", 0);
    // InterpRotSnap snaps a paired slot whose basis rotation moved more than ~60 deg in one tick.
    // It is the Slice-3 mispairing guard for objects closer together than the 300-unit teleport
    // threshold (adjacent track chunks); default 0 pending owner A/B, pinned on by
    // GDX_INTERP_ROT_SNAP for testing.
    CVarRegisterInteger("gEnhancements.Graphics.InterpRotSnap", 0);

    // Discord Rich Presence (port/gdx_discord.cpp) is a privacy feature: OFF by default, and while
    // off nothing is initialized — no thread, no socket. The Show* toggles pick which fields the
    // presence may publish and default ON, because enabling the master is already the opt-in.
    CVarRegisterInteger("gEnhancements.Online.DiscordPresence", 0);
    CVarRegisterInteger("gEnhancements.Online.DiscordShowCourse", 1);
    CVarRegisterInteger("gEnhancements.Online.DiscordShowLap", 1);
    CVarRegisterInteger("gEnhancements.Online.DiscordShowPosition", 1);
    CVarRegisterInteger("gEnhancements.Online.DiscordShowMode", 1);
    CVarRegisterInteger("gEnhancements.Online.DiscordShowTimer", 1);

    // GAMEPLAY. Stock F-Zero X already commits the NUMERIC records to SRAM on finish
    // (menus.c:252-268), so AutosaveOnRecord adds only auto-persisting the best GHOST replay, which
    // stock saves solely via the manual Save-Ghost prompt (menus.c Gdx_AutosaveGhostOnRecord).
    CVarRegisterInteger("gEnhancements.Gameplay.AutosaveOnRecord", 0);
    CVarRegisterInteger("gEnhancements.Gameplay.SkippableTransitions", 0);
    // On by default: the node blink/checker parity and the flagged-node size pulse advance at half
    // rate, halving Course Edit's ~20 Hz strobe on modern displays (course_edit/191080.c
    // func_xk2_800E04E0, #ifdef PORT). Off is bit-identical to stock.
    CVarRegisterInteger("gEnhancements.Gameplay.ReduceEditorFlashing", 1);
    if (CVarGetInteger("gdx.Migrations.ReduceEditorFlashingOn", 0) == 0) {
        CVarSetInteger("gdx.Migrations.ReduceEditorFlashingOn", 1);
        CVarSetInteger("gEnhancements.Gameplay.ReduceEditorFlashing", 1);
        CVarSave();
    }

    // COURSE EDIT UX. OfficialBack gates the PORT branch in func_xk2_800EB3B4 (19C470.c);
    // TestDrive* are read by func_xk2_800DEE20 (188850.c): machine 0 follows the player's
    // last selection, 1-30 pin a roster machine; engine is a percent fed through the stock
    // slider transform func_8008960C.
    CVarRegisterInteger("gEnhancements.Gameplay.CourseEditOfficialBack", 1);
    CVarRegisterInteger("gEnhancements.Gameplay.TestDriveMachine", 0);
    CVarRegisterInteger("gEnhancements.Gameplay.TestDriveEngine", 50);
    // EndingVenue gates the 11th Course Edit scene option (VENUE_ENDING): the menu item count in
    // A3AE0.c func_xk1_80028250 and the preview-loader range in segment.c func_80077AD8.
    CVarRegisterInteger("gEnhancements.Gameplay.CourseEditEndingVenue", 1);
    // INPUT. CourseEditMouse gates the absolute mouse drive in course_edit/188850.c via
    // port/gdx_course_edit_mouse.cpp; MouseSteering (issue #18) maps cursor X position to stick X
    // in input_bridge.c gdx_controller_poll, race modes only — the two are mode-disjoint.
    CVarRegisterInteger("gEnhancements.Input.CourseEditMouse", 0);
    CVarRegisterInteger("gEnhancements.Input.MouseSteering", 0);
    CVarRegisterInteger("gEnhancements.Input.MouseSteeringSensitivity", 100);
    // MouseConfineToWindow clips the OS cursor to the game window while Course Edit mouse control
    // or race mouse steering is active, so the cursor cannot drift off the game view mid-steer.
    CVarRegisterInteger("gEnhancements.Input.MouseConfineToWindow", 1);
    // HideCursorInGame hides the OS cursor while gameplay renders; the cursor returns whenever
    // the ImGui menu/menubar is open (gdx_hide_os_cursor_tick in port/gdx_course_edit_mouse.cpp).
    CVarRegisterInteger("gEnhancements.Input.HideCursorInGame", 0);
    CVarRegisterInteger("gEnhancements.Input.AutoAssignGamepadPorts", 1);
    // GUID->port seat memory written by gdxSyncGamepadPortRouting (port/main.cpp); not user-facing.
    CVarRegisterString("gEnhancements.Input.GamepadSeats", "");
    // Community request F1: func_80089800 (racer.c) randomizes AI machineSkinIndex 0-3 in
    // GP/Practice/Death Race, matching the variety VS mode already has. Off = stock parity.
    CVarRegisterInteger("gEnhancements.Gameplay.RandomOpponentColors", 0);
    // Community request F3: compact custom-grid roster string ("char:skin" per grid slot 0-29,
    // -1/"r" = random; slot 0 is the player and never applied). Parsed by gdx_custom_grid.c and
    // applied in func_80089800 over the default fill, bypassing the stock grid shuffle.
    // Empty = off (stock roster build).
    CVarRegisterString("gEnhancements.CustomGrid.Roster", "");
    // F10: port-side playtime/achievement tracker (port/gdx_achievements.cpp). On (default) it
    // polls gSaveContext each frame and persists unlocks + playtime to saves/achievements.txt;
    // off disables evaluation and playtime accumulation entirely.
    CVarRegisterInteger("gEnhancements.Achievements.Enabled", 1);
    // Community request F9: packed 0xRRGGBB overrides for the hardcoded effect colors in
    // racer.c; -1 keeps stock. GhostBoost keeps its stock alpha (160) either way.
    CVarRegisterInteger("gEnhancements.Gameplay.BoostColor", -1);
    CVarRegisterInteger("gEnhancements.Gameplay.DashPadColor", -1);
    CVarRegisterInteger("gEnhancements.Gameplay.BoostIdleColor", -1);
    CVarRegisterInteger("gEnhancements.Gameplay.SideAttackColor", -1);
    CVarRegisterInteger("gEnhancements.Gameplay.GhostBoostColor", -1);
    // Community request F5: percent scaling of the CPU combat probabilities in
    // Cpu_GenerateInputs (ovl_i3/cpu.c); past 100 the EXPERT-only gates drop too.
    // 100 = stock behavior.
    CVarRegisterInteger("gEnhancements.Gameplay.AiAggression", 100);

    // CONTENT (E1 .gdxc export). Default 0 = track exports strip author ghosts + records
    // (privacy-first; POST_1_0_SCOPING section 2). Read in port/gdx_content_io.c.
    CVarRegisterInteger("gEnhancements.Content.ExportIncludeGhosts", 0);

    // PRACTICE. Both are drawn under #ifdef PORT in hud.c / camera.c; photo mode saves and restores
    // eye/at/fov each frame so unpausing is 1:1.
    CVarRegisterInteger("gEnhancements.Practice.ShowLapDeltas", 0);
    CVarRegisterInteger("gEnhancements.Practice.PhotoMode", 0);

    // WORKSHOP. TexturePacks on lets the Tier-B shim (n64_gfx_bridge.cpp) rewrite a common-asset
    // load to a mounted pack's "textures/pack/<key>" resource.
    CVarRegisterInteger("gEnhancements.Workshop.TexturePacks", 0);
    // SequencePacks on lets the load hooks in decomp/src/audio/{disk,rom}/lib/load.c serve a
    // mounted pack's "audio/seq/<name>" bytes instead of DMA-ing the stock sequence.
    CVarRegisterInteger("gEnhancements.Workshop.SequencePacks", 0);
    // SamplePacks on lets the font-conversion hook in decomp/src/audio/disk/lib/load.c rewrite
    // individual converted samples from a mounted pack's "audio/sample/<key>" GSMP containers.
    CVarRegisterInteger("gEnhancements.Workshop.SamplePacks", 0);
    // SoundfontPacks on lets the same conversion site swap a font's whole instrument graph from a
    // mounted pack's "audio/font/<FONTNAME>" GFT1 container; GdxSoundfontPackTick services reloads.
    CVarRegisterInteger("gEnhancements.Workshop.SoundfontPacks", 0);
    // Scripts on runs every enabled pack's scripts/*.lua in a sandboxed per-script lua_State
    // (port/gdx_lua.cpp); loads and reloads key off the workshop pack epoch.
    CVarRegisterInteger("gEnhancements.Workshop.Scripts", 0);
    // ModelPacks on lets the racer.c LOD-registration hooks repoint stock machine display
    // lists at a mounted pack's "models/pack/machine/<name>/lod<N>" resources.
    CVarRegisterInteger("gEnhancements.Workshop.ModelPacks", 0);
    // TextureDump is retired in favour of Asset Dump, but the runtime hook in gdx_workshop.cpp
    // still reads it — hence forced to 0 rather than merely un-registered, so a user who had it ON
    // does not keep dumping forever with no surviving checkbox to turn it off.
    CVarSetInteger("gEnhancements.Workshop.TextureDump", 0);
    // Comma-joined ids/basenames of mods/*.o2r packs to skip at mount time (id match first,
    // basename fallback).
    CVarRegisterString("gEnhancements.Workshop.DisabledPacks", "");
    // Comma-joined ids/basenames in mount-priority order; unlisted packs mount after, alphabetical
    // by basename. The Workshop tab's up/down buttons rewrite this list.
    CVarRegisterString("gEnhancements.Workshop.PackOrder", "");
    // One-shot: set to 1, the D6 disk-format guard consumes it at the NEXT boot to authorize a
    // single MFS format into the sidecar (never the .ndd), then clears it.
    CVarRegisterInteger("gEnhancements.Workshop.AllowDDFormatOnce", 0);

    // Seed the "last cutoff" restore value from whatever is persisted (falls back to 15000).
    int hz = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
    if (hz > 0) {
        mLastLowPassHz = hz;
    }
}

void GdxMenu::InitElement() {
    // Build the registry once. InitElement runs on the ImGui thread after the Gui exists, which is
    // what the CVar reads inside the registration (defaults, combo lists) and the disable-reason
    // evaluations both assume.
    if (!mRegistered) {
        mRegistered = true;
        RegisterDisableReasons();
        RegisterMenu();
    }

    // Restore the last section by NAME, falling back to the first registered section when the
    // stored name is unknown (a renamed or removed tab) rather than to a stale index.
    const std::string storedSection = CVarGetString("gSettings.Menu.ActiveSection", "Settings");
    if (mMenuEntries.count(storedSection) != 0) {
        mActiveSection = storedSection;
    } else if (!mMenuOrder.empty()) {
        mActiveSection = mMenuOrder.front();
    }
}

void GdxMenu::UpdateElement() {
    // Gui::DrawMenu calls this before it draws the registered windows, and always on the ImGui
    // thread — the one place the queued log lines can safely reach the Console window.
    GdxConsoleLogDrain();
}

void GdxMenu::Draw() {
    if (!IsVisible()) {
        // Menu just closed (or was never open this frame): undo any nav tuning we applied and clear
        // the open-transition latches so focus is re-seeded the next time the menu opens.
        RestoreNavTuning();
        mMenuWasVisible = false;
        mNavCancelHadTarget = false;
        return;
    }
    DrawElement();
    SyncVisibilityConsoleVariable();
}

void GdxMenu::RestoreNavTuning() {
    if (!mNavTuningApplied) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.KeyRepeatDelay = mSavedKeyRepeatDelay;
        io.KeyRepeatRate = mSavedKeyRepeatRate;
        io.ConfigNavCursorVisibleAlways = mSavedNavCursorAlways;
    }
    mNavTuningApplied = false;
}

void GdxMenu::DrawElement() {
    // The registry is built in InitElement(), which libultraship calls before any Draw. Repeating
    // the guard here costs one bool test and rules out drawing an empty menu (or indexing an empty
    // mDisabledInfo) should that ordering ever change.
    if (!mRegistered) {
        InitElement();
    }

    // ONCE PER FRAME, before anything draws: every disable/hide reason is evaluated here and cached
    // in DisabledInfo::active, and MenuDrawItem only reads the cache. Several controls share the
    // same condition, so without this each would re-read the same CVar once per widget per frame.
    for (GdxUI::DisabledInfo& info : mDisabledInfo) {
        if (info.evaluation != nullptr) {
            info.active = info.evaluation(info);
        }
    }

    // Consume a pending "jump to this control" request from the search results. Deferred to the top
    // of the next frame rather than applied inside DrawSearchResults, because that runs mid-layout:
    // switching section and sidebar there would tear down the child window the result button was
    // just submitted into.
    if (mNavigateRequested) {
        mNavigateRequested = false;
        auto section = mMenuEntries.find(mNavigateSection);
        if (section != mMenuEntries.end() && section->second.sidebars.count(mNavigateSidebar) != 0) {
            mSearch[0] = '\0';
            mActiveSection = mNavigateSection;
            CVarSetString("gSettings.Menu.ActiveSection", mActiveSection.c_str());
            CVarSetString(section->second.sidebarCvar, mNavigateSidebar.c_str());
            GdxSaveCvars();
            // Arm the highlight: MenuDrawItem outlines the named control and scrolls it into view
            // for the next few seconds. Without this, "navigate" drops you on a page of thirty
            // controls with no indication which one you were looking for.
            mHighlightWidget = mNavigateWidget;
            mHighlightUntil = ImGui::GetTime() + 3.0;
            mHighlightScrollPending = true;
        }
    }

    const bool navActive = CVarGetInteger("gControlNav", 0) != 0;

    // On each open, seed nav focus onto the active sidebar page (consumed in DrawSidebar). Only when
    // gamepad nav is on, so mouse/keyboard users are not force-focused away from the search box.
    if (!mMenuWasVisible) {
        mMenuWasVisible = true;
        mFocusSidebar = navActive;
    }

    // Nav feel, applied only while THIS menu is open with gamepad nav on, and restored on close or
    // when the user turns nav off.
    //
    // ImGui derives its repeat rates from io.KeyRepeat*: nav moves at Delay*0.72 / Rate*0.80,
    // slider tweaks at Delay*0.72 / Rate*0.30. The stock 0.275/0.050 gives 25 selection moves per
    // second, which overshoots badly on a pad; 0.40/0.105 lands at ~12 moves/s while still tweaking
    // a slider at ~32 steps/s.
    //
    // ConfigNavCursorVisibleAlways is what makes the focus rectangle appear at all: both ways this
    // menu parks focus hide it — SetKeyboardFocusHere passes NoSetNavCursorVisible, and SetFocusID
    // hides the cursor unless the last activation came from a pad.
    if (navActive && !mNavTuningApplied) {
        ImGuiIO& io = ImGui::GetIO();
        mSavedKeyRepeatDelay = io.KeyRepeatDelay;
        mSavedKeyRepeatRate = io.KeyRepeatRate;
        mSavedNavCursorAlways = io.ConfigNavCursorVisibleAlways;
        io.KeyRepeatDelay = 0.40f;
        io.KeyRepeatRate = 0.105f;
        io.ConfigNavCursorVisibleAlways = true;
        mNavTuningApplied = true;
    } else if (!navActive && mNavTuningApplied) {
        RestoreNavTuning();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float opacity = std::clamp(CVarGetFloat("gSettings.Menu.BackgroundOpacity", 0.85f), 0.35f, 1.0f);

    GdxPushModernStyle();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.006f, 0.008f, 0.018f, opacity));
    const ImGuiWindowFlags outerFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("G-Diffuser Menu##Modern", nullptr, outerFlags)) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ImVec2 menuSize = available;
        if (available.x > 1280.0f) {
            menuSize.x = (std::min)(available.x * 0.90f, available.y * 1.78f);
        }
        if (available.y > 800.0f) {
            menuSize.y = available.y * 0.90f;
        }
        menuSize.x = (std::max)(menuSize.x, (std::min)(available.x, 640.0f));
        menuSize.y = (std::max)(menuSize.y, (std::min)(available.y, 480.0f));

        ImGui::SetCursorPos((available - menuSize) * 0.5f);
        // NavFlattened on the block + sidebar + content children: ImGui nav cannot cross a
        // child-window border without it, so a pad could move within the sidebar but never reach
        // the content pane's widgets. Flattened, the whole panel is one nav surface.
        if (ImGui::BeginChild("##ModernMenuBlock", menuSize, ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            DrawHeader();
            ImGui::Separator();

            const float bodyHeight = ImGui::GetContentRegionAvail().y;
            const float sidebarWidth = menuSize.x > 1500.0f ? menuSize.x * 0.15f : 210.0f;
            if (ImGui::BeginChild("##ModernSidebar", ImVec2(sidebarWidth, bodyHeight), ImGuiChildFlags_NavFlattened)) {
                DrawSidebar();
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImVec2 dividerMin = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(dividerMin, dividerMin + ImVec2(3.0f, bodyHeight),
                                                       ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::Dummy(ImVec2(3.0f, bodyHeight));
            ImGui::SameLine();

            const float contentWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::BeginChild("##ModernContent", ImVec2(contentWidth, bodyHeight), ImGuiChildFlags_NavFlattened,
                                  ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
                if (GdxGuiFontLarge() != nullptr) {
                    ImGui::PushFont(GdxGuiFontLarge());
                }
                ImGui::TextUnformatted(mSearch[0] != '\0' ? "Search Results" : ActiveSidebar().c_str());
                if (GdxGuiFontLarge() != nullptr) {
                    ImGui::PopFont();
                }
                ImGui::Separator();
                DrawCurrentPage();

                // Bring a search-navigated control into view. Done here, in the scrolling content
                // pane and after the page has laid out, because the control itself may have been
                // drawn inside a non-scrolling column child. Screen-space Y is converted back to
                // this window's scroll space, then centred.
                if (mHighlightScrollPending && mHighlightScreenY != 0.0f) {
                    const float local = mHighlightScreenY - ImGui::GetWindowPos().y + ImGui::GetScrollY();
                    ImGui::SetScrollY(local - ImGui::GetWindowHeight() * 0.5f);
                    mHighlightScrollPending = false;
                    mHighlightScreenY = 0.0f;
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        DrawQuitModal();

        // B / Circle = "back": close the menu, but only when ImGui had nothing of its own to cancel.
        // Testing that here is too late — NavUpdateCancelRequest ran back in NewFrame and has already
        // dropped the slider or popup the press was meant to leave, so a live check sees a clean menu
        // and one B would both back out and close. The end-of-frame snapshot below is the state ImGui
        // itself saw. Edge-triggered so a held B does not re-fire.
        const bool cancelWasConsumed = mNavCancelHadTarget;
        const bool popupOpen =
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (navActive && !cancelWasConsumed && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) &&
            !ImGui::IsAnyItemActive() && !popupOpen) {
            Hide();
        }
        mNavCancelHadTarget = ImGui::IsAnyItemActive() || popupOpen;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    GdxPopModernStyle();
}

void GdxMenu::DrawHeader() {
    auto gui = GdxGui();
    const bool navActive = gui != nullptr && gui->CanUseGamepadNavigation();

    // Shoulder buttons cycle the header tabs (wrapping). ImGui spends L1/R1 on window-cycling only
    // while FaceLeft is held, and on slider tweak-speed only while a slider is being dragged, so a
    // bare shoulder tap in this single fullscreen window hits neither path and an edge-triggered
    // read needs no SetKeyOwner juggling. Suppressed while an item is active so this never yanks
    // focus out of a slider or text field mid-edit, and while a popup is open so shoulders serve
    // the modal instead of cycling tabs behind it.
    const int tabCount = static_cast<int>(mMenuOrder.size());
    if (navActive && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        tabCount > 0) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) dir += 1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) dir -= 1;
        if (dir != 0) {
            int cur = 0;
            for (int i = 0; i < tabCount; ++i) {
                if (mMenuOrder[i] == mActiveSection) {
                    cur = i;
                }
            }
            const int idx = (cur + dir + tabCount) % tabCount;
            mSearch[0] = '\0';
            SelectSection(mMenuOrder[idx]); // sets mFocusSidebar -> focus lands on the new tab
        }
    }

    const float height = ImGui::GetFrameHeight() + 4.0f;
    const float controlsWidth = ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float searchWidth = ImGui::GetContentRegionAvail().x >= 900.0f ? 210.0f : 140.0f;

    if (ImGui::BeginTable("##ModernHeader", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("Navigation", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Search", ImGuiTableColumnFlags_WidthFixed, searchWidth);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, controlsWidth);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        // Flattened like the sidebar and content children: an unflattened child is a single nav
        // item a pad must press A to enter and B to leave. Flattened, Up from the sidebar lands
        // straight on a tab.
        if (ImGui::BeginChild("##HeaderNavigation", ImVec2(0, height), ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            // Flank the tab strip with shoulder-button hints, so L1/R1 tab cycling is discoverable.
            if (navActive) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled(ICON_FA_CHEVRON_LEFT " LB");
                UIWidgets::Tooltip("Previous tab (L1 / LB)");
            }
            for (int i = 0; i < tabCount; ++i) {
                if (i > 0 || navActive) {
                    ImGui::SameLine();
                }
                const char* label = mMenuOrder[i].c_str();
                const ImVec2 buttonSize(ImGui::CalcTextSize(label).x + 20.0f, ImGui::GetFrameHeight());
                if (GdxNavigationButton(label, mActiveSection == mMenuOrder[i], buttonSize)) {
                    mSearch[0] = '\0';
                    SelectSection(mMenuOrder[i]);
                }
            }
            if (navActive) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("RB " ICON_FA_CHEVRON_RIGHT);
                UIWidgets::Tooltip("Next tab (R1 / RB)");
            }
        }
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##MenuSearch", "Search...", mSearch, sizeof(mSearch));

        ImGui::TableSetColumnIndex(2);
        const ImVec2 actionSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.0f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.03f, 0.03f, 1.0f));
        if (ImGui::Button(ICON_FA_POWER_OFF "##Quit", actionSize)) {
            mOpenQuitModal = true;
        }
        UIWidgets::Tooltip("Quit G-Diffuser");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO "##Reset", actionSize)) {
            // The menu is already on the host side of the bridge, so request the reset directly.
            // Ctrl+R goes through the console command; both converge on the same deferred flag.
            gdx_game_request_reset();
        }
        UIWidgets::Tooltip("Reset game (Ctrl+R)");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.32f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.43f, 0.47f, 1.0f));
        if (ImGui::Button(ICON_FA_TIMES_CIRCLE "##Close", actionSize)) {
            Hide();
        }
        ImGui::PopStyleColor(2);
        UIWidgets::Tooltip("Close menu (Esc or F1)");
        ImGui::EndTable();
    }
}

void GdxMenu::DrawSidebar() {
    // When flagged (menu just opened or the tab changed) and gamepad nav is on, park the nav cursor
    // on the active page. SetKeyboardFocusHere() targets the NEXT submitted item and is the
    // reliable idiom under NavEnableGamepad; SetItemDefaultFocus() covers the child's very first
    // appearance.
    const bool wantFocus = mFocusSidebar && CVarGetInteger("gControlNav", 0) != 0;

    auto sidebarButton = [&](const std::string& sidebar) {
        const bool isActive = mSearch[0] == '\0' && ActiveSidebar() == sidebar;
        if (wantFocus && isActive) {
            ImGui::SetKeyboardFocusHere();
        }
        if (GdxNavigationButton(sidebar.c_str(), isActive, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            mSearch[0] = '\0';
            SelectSidebar(sidebar);
        }
        if (wantFocus && isActive) {
            ImGui::SetItemDefaultFocus();
        }
    };

    auto section = mMenuEntries.find(mActiveSection);
    if (section != mMenuEntries.end()) {
        for (const std::string& sidebar : section->second.sidebarOrder) {
            sidebarButton(sidebar);
        }
    }

    // One-shot: focus request (if any) has now been submitted for this frame.
    mFocusSidebar = false;
}

// The registration decides how many columns (1-3) a page has and which one each control belongs to;
// this only lays them out, collapsing to a single column on a content pane too narrow to hold the
// widgets side by side.
void GdxMenu::DrawCurrentPage() {
    if (mSearch[0] != '\0') {
        if (DrawSearchResults() == 0) {
            ImGui::TextDisabled("No settings or tools match \"%s\".", mSearch);
        }
        return;
    }

    GdxUI::SidebarEntry* entry = ActiveSidebarEntry();
    if (entry == nullptr) {
        return;
    }

    const float available = ImGui::GetContentRegionAvail().x;
    int columns = static_cast<int>(entry->columnCount);
    if (columns < 1) {
        columns = 1;
    }
    // 420px per column is the width below which this menu's widest controls (a slider with its
    // label positioned Near, or a combobox sized to its longest entry) start truncating.
    while (columns > 1 && available / static_cast<float>(columns) < 420.0f) {
        --columns;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float columnWidth = (available - style.ItemSpacing.x * static_cast<float>(columns - 1)) /
                              static_cast<float>(columns);
    const int columnGroups = static_cast<int>(entry->columnWidgets.size());

    for (int i = 0; i < columnGroups; ++i) {
        const bool useColumns = columns > 1 && i < columns;
        if (useColumns) {
            // NavFlattened for the same reason as every other child here (see DrawElement):
            // unflattened, the second column would be unreachable by gamepad.
            ImGui::BeginChild(("##PageColumn" + std::to_string(i)).c_str(), ImVec2(columnWidth, 0.0f),
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
        }
        for (GdxUI::WidgetInfo& widget : entry->columnWidgets[i]) {
            MenuDrawItem(widget);
        }
        if (useColumns) {
            ImGui::EndChild();
            if (i < columns - 1) {
                ImGui::SameLine();
            }
        }
    }
}

// Widget-level search: walks the SAME registry MenuDrawItem draws from, so anything on a page is
// findable and anything findable is really on a page. Each hit draws the LIVE control, so it can be
// changed right there, followed by a button naming where it lives ("Enhancements -> Visuals, Col
// 2") that jumps the menu to that page and briefly outlines it. Page-level hits come from
// SidebarEntry::searchTerms. Returns the number of results drawn.
uint32_t GdxMenu::DrawSearchResults() {
    // Spaces are stripped from BOTH sides of the comparison (upstream does the same), so "lowpass"
    // finds "Low-pass"-adjacent wording and "framepacing" finds "Frame pacing".
    std::string query = GdxLowercase(mSearch);
    query.erase(std::remove(query.begin(), query.end(), ' '), query.end());
    if (query.empty()) {
        return 0;
    }

    auto normalise = [](std::string value) {
        value = GdxLowercase(std::move(value));
        value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
        return value;
    };

    uint32_t matches = 0;

    // ── Page hits ────────────────────────────────────────────────────────────────────────────
    for (const std::string& sectionName : mMenuOrder) {
        GdxUI::MainMenuEntry& section = mMenuEntries.at(sectionName);
        for (const std::string& sidebarName : section.sidebarOrder) {
            const GdxUI::SidebarEntry& sidebar = section.sidebars.at(sidebarName);
            if (normalise(sectionName + " " + sidebarName + " " + sidebar.searchTerms).find(query) ==
                std::string::npos) {
                continue;
            }
            ++matches;
            ImGui::PushID(("page_" + sectionName + sidebarName).c_str());
            if (ImGui::Button(sidebarName.c_str(),
                              ImVec2((std::min)(430.0f, ImGui::GetContentRegionAvail().x), 0.0f))) {
                mSearch[0] = '\0';
                mActiveSection = sectionName;
                CVarSetString("gSettings.Menu.ActiveSection", mActiveSection.c_str());
                SelectSidebar(sidebarName);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", sectionName.c_str());
            ImGui::PopID();
        }
    }

    // ── Control hits ─────────────────────────────────────────────────────────────────────────
    for (const std::string& sectionName : mMenuOrder) {
        GdxUI::MainMenuEntry& section = mMenuEntries.at(sectionName);
        for (const std::string& sidebarName : section.sidebarOrder) {
            GdxUI::SidebarEntry& sidebar = section.sidebars.at(sidebarName);
            for (size_t col = 0; col < sidebar.columnWidgets.size(); ++col) {
                for (GdxUI::WidgetInfo& widget : sidebar.columnWidgets[col]) {
                    // Decoration carries no setting, so it is never a search result.
                    if (widget.hideInSearch || widget.type == GdxUI::WIDGET_SEPARATOR ||
                        widget.type == GdxUI::WIDGET_SEPARATOR_TEXT || widget.type == GdxUI::WIDGET_TEXT ||
                        widget.type == GdxUI::WIDGET_TEXT_DISABLED) {
                        continue;
                    }
                    // A control its own page would not show is a dead end: MenuDrawItem would draw
                    // nothing and the "go there" button would land on a page without it. The hide
                    // conditions are re-checked here rather than trusting WidgetInfo::isHidden,
                    // which is only refreshed for widgets that were drawn this frame — i.e. for the
                    // ACTIVE page. Reads the same once-per-frame cache, so it costs nothing.
                    bool hidden = false;
                    for (GdxUI::DisableOption reason : widget.hideWhen) {
                        if (mDisabledInfo[reason].active) {
                            hidden = true;
                        }
                    }
                    if (hidden) {
                        continue;
                    }
                    const char* tooltip = widget.options != nullptr ? widget.options->tooltip : "";
                    const std::string haystack = normalise(
                        widget.name + " " + (tooltip != nullptr ? tooltip : "") + " " + widget.searchTerms);
                    if (haystack.find(query) == std::string::npos) {
                        continue;
                    }
                    ++matches;

                    ImGui::PushID(("hit_" + sectionName + sidebarName + std::to_string(col) + widget.name).c_str());
                    if (widget.type == GdxUI::WIDGET_CUSTOM) {
                        // A custom block is a whole sub-panel (a table, a modal owner, a status
                        // read-out). Rendering one inside the result list would duplicate its
                        // ImGui IDs against the copy on its own page and, for the dev-gate table,
                        // dwarf every other result. Name it and offer the jump instead.
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(widget.name.c_str());
                    } else {
                        MenuDrawItem(widget);
                    }

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    const std::string origin = "  " ICON_FA_ARROW_RIGHT "  " + sectionName + " -> " + sidebarName +
                                               ", Col " + std::to_string(col + 1);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.57f, 0.64f, 1.0f));
                    const bool go = ImGui::Button(origin.c_str());
                    ImGui::PopStyleColor(2);
                    UIWidgets::Tooltip("Go to this setting on its own page.");
                    if (go) {
                        mNavigateRequested = true;
                        mNavigateSection = sectionName;
                        mNavigateSidebar = sidebarName;
                        mNavigateWidget = widget.name;
                    }
                    ImGui::PopID();
                }
            }
        }
    }

    return matches;
}

void GdxMenu::DrawQuitModal() {
    if (mOpenQuitModal) {
        ImGui::OpenPopup("Quit G-Diffuser");
        mOpenQuitModal = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit G-Diffuser", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Are you sure you want to quit G-Diffuser?");
        ImGui::Spacing();
        if (ImGui::Button("Quit", ImVec2(90.0f, 0.0f))) {
            Hide();
            if (auto window = GdxWindow()) {
                window->Close();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            ImGui::CloseCurrentPopup();
        }
        // Popup nav focus otherwise lands on Quit: default-park it on the non-destructive choice.
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void GdxMenu::SelectSection(const std::string& section) {
    if (mMenuEntries.count(section) == 0) {
        return;
    }
    mActiveSection = section;
    CVarSetString("gSettings.Menu.ActiveSection", mActiveSection.c_str());
    GdxSaveCvars();
    // A tab change moves the whole page list; re-park the nav cursor on the new tab's first page so
    // the pad does not end up focused on a now-hidden item. Harmless with mouse/keyboard (gated in
    // DrawSidebar on gControlNav).
    mFocusSidebar = true;
}

void GdxMenu::SelectSidebar(const std::string& sidebar) {
    auto section = mMenuEntries.find(mActiveSection);
    if (section == mMenuEntries.end() || section->second.sidebars.count(sidebar) == 0) {
        return;
    }
    // Persisted per SECTION, so each tab remembers where you were in it independently.
    CVarSetString(section->second.sidebarCvar, sidebar.c_str());
    GdxSaveCvars();
}

// The active page of the active section, falling back to that section's first page when the stored
// name is unknown (page renamed, removed, or never set).
const std::string& GdxMenu::ActiveSidebar() {
    static const std::string kNone;
    auto section = mMenuEntries.find(mActiveSection);
    if (section == mMenuEntries.end() || section->second.sidebarOrder.empty()) {
        return kNone;
    }
    const std::string stored = CVarGetString(section->second.sidebarCvar, "");
    for (const std::string& name : section->second.sidebarOrder) {
        if (name == stored) {
            return section->second.sidebars.find(name)->first;
        }
    }
    return section->second.sidebarOrder.front();
}

GdxUI::SidebarEntry* GdxMenu::ActiveSidebarEntry() {
    auto section = mMenuEntries.find(mActiveSection);
    if (section == mMenuEntries.end()) {
        return nullptr;
    }
    auto sidebar = section->second.sidebars.find(ActiveSidebar());
    return sidebar != section->second.sidebars.end() ? &sidebar->second : nullptr;
}

void GdxMenu::AddMenuEntry(const std::string& label, const char* sidebarCvar) {
    GdxUI::MainMenuEntry entry;
    entry.label = label;
    entry.sidebarCvar = sidebarCvar;
    mMenuEntries.emplace(label, std::move(entry));
    mMenuOrder.push_back(label);
}

void GdxMenu::AddSidebarEntry(const std::string& section, const std::string& sidebar, uint32_t columnCount,
                              const std::string& searchTerms) {
    auto it = mMenuEntries.find(section);
    if (it == mMenuEntries.end()) {
        return;
    }
    GdxUI::SidebarEntry entry;
    entry.columnCount = columnCount == 0 ? 1 : columnCount;
    // One widget vector per declared column, allocated up front so AddWidget can index straight in
    // and a page may legally leave a column empty.
    entry.columnWidgets.resize(entry.columnCount);
    entry.searchTerms = searchTerms;
    it->second.sidebars.emplace(sidebar, std::move(entry));
    it->second.sidebarOrder.push_back(sidebar);
}

void GdxMenu::AddWidget(const std::string& section, const std::string& sidebar, GdxUI::SectionColumns column,
                        GdxUI::WidgetInfo widget) {
    auto sectionIt = mMenuEntries.find(section);
    if (sectionIt == mMenuEntries.end()) {
        return;
    }
    auto sidebarIt = sectionIt->second.sidebars.find(sidebar);
    if (sidebarIt == sectionIt->second.sidebars.end()) {
        return;
    }
    // Every widget must carry an Options struct: MenuDrawItem writes options->disabled /
    // ->disabledTooltip for the named disable reasons, and the search reads options->tooltip.
    //
    // It must be the struct MATCHING widget.type, not the base. MenuDrawItem reaches its options
    // through static_pointer_cast, which does no checking: handed a plain WidgetOptions for a
    // WIDGET_TEXT, `options->color` reads past the end of the allocation and the garbage enum goes
    // to ColorValues.at(), which throws std::out_of_range with nothing to catch it. Allocating by
    // type here fixes the whole class, instead of requiring every future registration to remember
    // .Options() on precisely the subset of types that dereference it.
    if (widget.options == nullptr) {
        switch (widget.type) {
            case GdxUI::WIDGET_TEXT:
                widget.options = std::make_shared<UIWidgets::TextOptions>();
                break;
            case GdxUI::WIDGET_BUTTON:
                widget.options = std::make_shared<UIWidgets::ButtonOptions>();
                break;
            case GdxUI::WIDGET_CHECKBOX:
            case GdxUI::WIDGET_CVAR_CHECKBOX:
                widget.options = std::make_shared<UIWidgets::CheckboxOptions>();
                break;
            case GdxUI::WIDGET_COMBOBOX:
            case GdxUI::WIDGET_CVAR_COMBOBOX:
                widget.options = std::make_shared<UIWidgets::ComboboxOptions>();
                break;
            case GdxUI::WIDGET_SLIDER_INT:
            case GdxUI::WIDGET_CVAR_SLIDER_INT:
                widget.options = std::make_shared<UIWidgets::IntSliderOptions>();
                break;
            case GdxUI::WIDGET_SLIDER_FLOAT:
            case GdxUI::WIDGET_CVAR_SLIDER_FLOAT:
                widget.options = std::make_shared<UIWidgets::FloatSliderOptions>();
                break;
            case GdxUI::WIDGET_CVAR_RADIO_BUTTON:
                widget.options = std::make_shared<UIWidgets::RadioButtonsOptions>();
                break;
            default:
                // Decorative and custom types never downcast; they read only the base fields.
                widget.options = std::make_shared<UIWidgets::WidgetOptions>();
                break;
        }
    }
    auto& columns = sidebarIt->second.columnWidgets;
    size_t index = static_cast<size_t>(column);
    if (index >= columns.size()) {
        index = columns.empty() ? 0 : columns.size() - 1; // declared fewer columns than requested
    }
    if (columns.empty()) {
        columns.resize(1);
    }
    columns[index].push_back(std::move(widget));
}

// One registered WidgetInfo -> ImGui. Order of operations:
//   1. hideWhen / preFunc            decide whether the control exists this frame at all
//   2. disableWhen                   turn the active named reasons into ONE tooltip listing all of
//                                    them, so a control greyed for two reasons says both
//   3. the widget itself             via the UIWidgets CVar-bound library
//   4. callback                      side effects, only when the widget reported a change
//   5. modified marker / note        the "* changed from default" cue and the "(restart)" suffix
//   6. postFunc                      anything reacting to state the widget cannot report
//   7. highlight                     the search-navigation outline
//
// Unlike Lighthouse's MenuDrawItem, this does NOT overwrite options->color with a global theme
// colour: each Options struct's own default is what every call site here relies on, and forcing one
// colour on all of them would silently restyle the whole menu.
void GdxMenu::MenuDrawItem(GdxUI::WidgetInfo& widget) {
    widget.ResetDisables();

    // Hide conditions are evaluated from the same once-per-frame cache as the disable conditions.
    for (GdxUI::DisableOption reason : widget.hideWhen) {
        if (mDisabledInfo[reason].active) {
            widget.isHidden = true;
        }
    }
    if (widget.preFunc != nullptr) {
        widget.preFunc(widget);
    }
    if (widget.isHidden) {
        return;
    }

    for (GdxUI::DisableOption reason : widget.disableWhen) {
        if (mDisabledInfo[reason].active) {
            widget.activeDisables.push_back(reason);
        }
    }
    if (!widget.activeDisables.empty()) {
        // Built into a member string because UIWidgets' Options structs borrow the pointer.
        mDisabledTooltip = "This setting is unavailable because:";
        for (GdxUI::DisableOption reason : widget.activeDisables) {
            mDisabledTooltip += "\n  - ";
            mDisabledTooltip += mDisabledInfo[reason].reason;
        }
        widget.options->disabled = true;
        widget.options->disabledTooltip = mDisabledTooltip.c_str();
    }

    if (widget.sameLine) {
        ImGui::SameLine();
    }

    const bool highlight = !mHighlightWidget.empty() && widget.name == mHighlightWidget &&
                           ImGui::GetTime() < mHighlightUntil;
    if (highlight) {
        // Group the whole control so the outline below covers the label + widget + any inline
        // buttons, not just whichever ImGui item happened to be submitted last.
        ImGui::BeginGroup();
    }

    // A widget registered WITHOUT .Options() must fall back to that type's defaults, never
    // dereference null: options is populated only by .Options() and is legitimately absent for the
    // decorative types (WIDGET_SEPARATOR, WIDGET_CUSTOM, ...), so the switch below cannot assume
    // it away. The registry is hand-edited and grown, and "remember .Options() on this subset of
    // types or the game dies on that page" is not a contract worth shipping.
    //
    // The argument is a type tag only; the fallback it names is a function-local static, so the
    // returned reference outlives the call (a reference to the caller's temporary would trade the
    // null deref for a dangling one).
    const auto optionsOr = [&widget](auto tag) -> const decltype(tag)& {
        using T = decltype(tag);
        static const T kDefaults{};
        return widget.options != nullptr ? *std::static_pointer_cast<T>(widget.options) : kDefaults;
    };

    bool changed = false;
    switch (widget.type) {
        case GdxUI::WIDGET_SEPARATOR:
            ImGui::Separator();
            break;
        case GdxUI::WIDGET_SEPARATOR_TEXT:
            ImGui::SeparatorText(widget.name.c_str());
            break;
        case GdxUI::WIDGET_TEXT: {
            const UIWidgets::TextOptions& options = optionsOr(UIWidgets::TextOptions{});
            const bool coloured = options.color != UIWidgets::Colors::NoColor;
            if (coloured) {
                ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(options.color));
            }
            ImGui::TextWrapped("%s", widget.name.c_str());
            if (coloured) {
                ImGui::PopStyleColor();
            }
            break;
        }
        case GdxUI::WIDGET_TEXT_DISABLED:
            ImGui::TextDisabled("%s", widget.name.c_str());
            break;
        case GdxUI::WIDGET_COMING_SOON:
            GdxComingSoon(widget.name.c_str());
            break;
        case GdxUI::WIDGET_CHECKBOX: {
            bool* value = std::get<bool*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::Checkbox(widget.name.c_str(), value,
                                          optionsOr(UIWidgets::CheckboxOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_CHECKBOX:
            changed = UIWidgets::CVarCheckbox(widget.name.c_str(), widget.cVar,
                                              optionsOr(UIWidgets::CheckboxOptions{}));
            break;
        case GdxUI::WIDGET_COMBOBOX: {
            int32_t* value = std::get<int32_t*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::Combobox<int32_t>(widget.name.c_str(), value, widget.comboItems,
                                                   optionsOr(UIWidgets::ComboboxOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_COMBOBOX:
            changed = UIWidgets::CVarCombobox(widget.name.c_str(), widget.cVar, widget.comboItems,
                                              optionsOr(UIWidgets::ComboboxOptions{}));
            break;
        case GdxUI::WIDGET_SLIDER_INT: {
            int32_t* value = std::get<int32_t*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::SliderInt(widget.name.c_str(), value,
                                           optionsOr(UIWidgets::IntSliderOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_SLIDER_INT:
            changed = UIWidgets::CVarSliderInt(widget.name.c_str(), widget.cVar,
                                               optionsOr(UIWidgets::IntSliderOptions{}));
            break;
        case GdxUI::WIDGET_SLIDER_FLOAT: {
            float* value = std::get<float*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::SliderFloat(widget.name.c_str(), value,
                                             optionsOr(UIWidgets::FloatSliderOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_SLIDER_FLOAT:
            changed =
                UIWidgets::CVarSliderFloat(widget.name.c_str(), widget.cVar,
                                           optionsOr(UIWidgets::FloatSliderOptions{}));
            break;
        case GdxUI::WIDGET_BUTTON: {
            // Plain ImGui::Button, NOT UIWidgets::Button: this menu's shell already styles buttons
            // through GdxPushModernStyle (the G-Diffuser blue), and UIWidgets::Button unconditionally
            // pushes its own palette from ButtonOptions::color, which would repaint every button in
            // the menu grey. Only the disabled handling and tooltip are taken from the Options.
            const UIWidgets::ButtonOptions& options = optionsOr(UIWidgets::ButtonOptions{});
            ImGui::BeginDisabled(options.disabled);
            changed = ImGui::Button(widget.name.c_str(), options.size);
            ImGui::EndDisabled();
            // AllowWhenDisabled so a greyed button is exactly the one that explains itself.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                const char* text = options.disabled && !UIWidgets::IsCStringEmpty(options.disabledTooltip)
                                       ? options.disabledTooltip
                                       : options.tooltip;
                if (!UIWidgets::IsCStringEmpty(text)) {
                    ImGui::SetTooltip("%s", UIWidgets::WrappedText(text).c_str());
                }
            }
            break;
        }
        case GdxUI::WIDGET_CVAR_RADIO_BUTTON:
            changed = UIWidgets::CVarRadioButton(widget.name.c_str(), widget.cVar, widget.radioValue,
                                                 optionsOr(UIWidgets::RadioButtonsOptions{}));
            break;
        case GdxUI::WIDGET_CUSTOM:
            if (widget.customFunction != nullptr) {
                widget.customFunction(widget);
            }
            break;
    }

    if (changed && widget.callback != nullptr) {
        widget.callback(widget);
    }

    // The default comes from the widget's own CheckboxOptions::defaultValue, so the marker and the
    // control can never disagree about what stock is.
    if (widget.modifiedMarker && widget.type == GdxUI::WIDGET_CVAR_CHECKBOX) {
        const bool def = optionsOr(UIWidgets::CheckboxOptions{}).defaultValue;
        GdxModifiedMarker((CVarGetInteger(widget.cVar, def) != 0) != def);
    }

    // The greyed suffix ("(restart)", "(disabled in-race)"). UIWidgets has no slot for one, so it
    // stays a SameLine + TextDisabled — just declared in the registry rather than written out.
    if (!UIWidgets::IsCStringEmpty(widget.note)) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", widget.note);
    }

    if (widget.postFunc != nullptr) {
        widget.postFunc(widget);
    }

    if (highlight) {
        ImGui::EndGroup();
        // Pulsing outline around the control the search sent us to; ImGui has no "flash this item"
        // primitive, and Lighthouse's highlightWidget/navigateWidgetName globals are never read.
        const ImVec2 min = ImGui::GetItemRectMin() - ImVec2(4.0f, 3.0f);
        const ImVec2 max = ImGui::GetItemRectMax() + ImVec2(4.0f, 3.0f);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
        ImGui::GetWindowDrawList()->AddRect(min, max,
                                            ImGui::GetColorU32(ImVec4(0.63f, 0.76f, 1.0f, 0.35f + 0.55f * pulse)),
                                            3.0f, 0, 2.0f);
        if (mHighlightScrollPending) {
            // Record where it landed; DrawElement scrolls the content pane to it once this page has
            // finished laying out. Deliberately NOT ImGui::SetScrollHereY(): on a multi-column page
            // this runs inside a non-scrolling column child, which has no scroll to set.
            mHighlightScreenY = min.y;
        }
    }
}

// A "no audio" report is undebuggable remotely without knowing which backend the session picked and
// whether samples are actually queued.
void GdxMenu::DrawAudioStatus() {
    // The backend name comes from AudioPlayerBackendName, not SDL_GetCurrentAudioDriver(), which
    // returns "none" for the working WASAPI/CoreAudio backends. For the SDL backend the SDL driver
    // is surfaced as well ("pipewire"/"pulse"); "dummy" there means the launch environment lost the
    // audio socket, so the game synthesizes fine but the samples go nowhere.
    const char* backend = AudioPlayerBackendName();
    const bool isSdl = std::strcmp(backend, "SDL") == 0;
    const char* sdlDriver = isSdl ? SDL_GetCurrentAudioDriver() : nullptr;
    const int32_t buffered = AudioPlayerBuffered();
    const int32_t desired = AudioPlayerGetDesiredBuffered();

    ImGui::SeparatorText("Output status");
    if (isSdl) {
        ImGui::Text("Active backend: SDL (%s)", sdlDriver != nullptr ? sdlDriver : "no driver");
    } else {
        ImGui::Text("Active backend: %s", backend);
    }
    ImGui::Text("Queued samples: %d / %d desired", buffered, desired);

    // Only meaningful when SDL is the active backend: WASAPI/CoreAudio legitimately report no SDL
    // driver, so warning about it there would be a false alarm.
    if (isSdl && sdlDriver == nullptr) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "No SDL audio device is open. Audio is synthesized but discarded.");
    } else if (isSdl && std::strcmp(sdlDriver, "dummy") == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "SDL fell back to the dummy driver: this launch environment has no\n"
                           "audio socket. Launch from a terminal or fix the launcher's env.");
    }
}

// Export is read-only. Import adds a validated player replay to the per-course PC library and
// mirrors it into SRAM only when that does not evict another course; it stays disabled while an
// on-track race is live, to avoid mutating ghost state alongside the game fiber. Both use the
// default path next to the exe (ghost_export.gdg); a proper file picker remains future work.
//
// A custom block rather than two WIDGET_BUTTONs: the pair shares a status buffer and a resolved
// path that both branches format into, and the export tooltip interpolates that path.
void GdxMenu::DrawGhostIo() {
    ImGui::TextDisabled("Ghost replay (.gdg)");

    static char sGhostStatus[192] = { 0 };
    char path[1024];
    bool haveDefault = gdx_ghost_default_path(path, sizeof(path)) != 0;

    if (ImGui::Button("Export saved ghost")) {
        if (!haveDefault) {
            snprintf(sGhostStatus, sizeof(sGhostStatus), "Export failed: could not resolve output path.");
        } else {
            int rc = gdx_ghost_export(GDX_GHOST_ANY_COURSE, path);
            if (rc == GDX_GHOST_OK) {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Exported to %s", path);
            } else if (rc == GDX_GHOST_ERR_NO_GHOST) {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Export: no ghost is saved yet.");
            } else {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Export failed (code %d).", rc);
            }
        }
    }
    // Raw SetTooltip rather than UIWidgets::Tooltip: that runs every tooltip through WrappedText at
    // 80 columns, which would break this path at the spaces inside it.
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Writes the currently-saved ghost replay to:\n%s",
                          haveDefault ? path : "(unavailable)");
    }

    ImGui::SameLine();

    // The in-race lockout is the named reason DISABLE_FOR_RACE_IN_PROGRESS, so the greyed button
    // states why. The "(disabled in-race)" label stays too, because it is visible without hovering.
    const bool inGame = mDisabledInfo[GdxUI::DISABLE_FOR_RACE_IN_PROGRESS].active;
    ImGui::BeginDisabled(inGame);
    if (ImGui::Button("Import ghost")) {
        if (!haveDefault) {
            snprintf(sGhostStatus, sizeof(sGhostStatus), "Import failed: could not resolve input path.");
        } else {
            int rc = gdx_ghost_import(path);
            if (rc == GDX_GHOST_OK) {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Imported into the player ghost library: %s", path);
            } else if (rc == GDX_GHOST_ERR_COURSE_MISMATCH) {
                snprintf(sGhostStatus, sizeof(sGhostStatus),
                         "Import refused: the save slot holds a different course's ghost.");
            } else if (rc == GDX_GHOST_ERR_BAD_MAGIC || rc == GDX_GHOST_ERR_BAD_VERSION) {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Import failed: not a valid .gdg file (code %d).", rc);
            } else {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Import failed (code %d).", rc);
            }
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", inGame ? "This setting is unavailable because:\n  - A race is in progress."
                                       : "Reads ghost_export.gdg back into your per-course ghost library.");
    }
    if (inGame) {
        ImGui::SameLine();
        ImGui::TextDisabled("(disabled in-race)");
    }

    if (sGhostStatus[0] != '\0') {
        ImGui::TextWrapped("%s", sGhostStatus);
    }
}

// M3 X Cup seed codes (port/gdx_xcup_seed.c). A custom block rather than a CVar widget: the
// pending seed is one-shot runtime state, not persisted config, and the input needs a parse
// verdict plus the current code display.
void GdxMenu::DrawXCupSeed() {
    static char sCodeInput[64] = { 0 };
    static char sSeedStatus[160] = { 0 };
    char current[GDX_XCUP_CODE_BUF_SIZE];

    if (gdx_xcup_current_seed_code(current, sizeof(current))) {
        ImGui::Text("Last generated X Cup course: %s", current);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Share this code and the other player gets the exact same track.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(current);
            snprintf(sSeedStatus, sizeof(sSeedStatus), "Copied to clipboard.");
        }
    } else {
        ImGui::TextDisabled("Race an X Cup course once to see its seed code here.");
    }

    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##xcupseed", "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-X", sCodeInput, sizeof(sCodeInput));
    ImGui::SameLine();
    if (ImGui::Button("Use for next X Cup course")) {
        if (gdx_xcup_set_pending_code(sCodeInput)) {
            snprintf(sSeedStatus, sizeof(sSeedStatus), "Seed queued — the next generated X Cup course will match the code.");
            sCodeInput[0] = '\0';
        } else {
            snprintf(sSeedStatus, sizeof(sSeedStatus), "That is not a valid seed code.");
        }
    }
    if (gdx_xcup_has_pending_seed()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.55f, 1.0f), "(queued)");
    }
    if (sSeedStatus[0] != '\0') {
        ImGui::TextWrapped("%s", sSeedStatus);
    }
}

void GdxMenu::DrawToolWindowPage(const char* name, const char* description) {
    auto gui = GdxGui();
    auto window = gui != nullptr ? gui->GetGuiWindow(name) : nullptr;
    if (window == nullptr) {
        ImGui::TextDisabled("%s is unavailable.", name);
        return;
    }

    ImGui::TextWrapped("%s", description);
    const bool poppedOut = window->IsVisible();
    std::string buttonLabel = poppedOut ? std::string("Return to menu##") + name
                                        : std::string("Pop out ") + name + "##" + name;
    if (ImGui::Button(buttonLabel.c_str())) {
        window->ToggleVisibility();
    }
    ImGui::Separator();
    if (window->IsVisible()) {
        ImGui::TextDisabled("%s is open in a separate window.", name);
    } else {
        window->DrawElement();
    }
}

// Presented rate alongside the 60 Hz logic rate, plus the live sub-frame count and the previous
// tick's tween/snap breakdown — so a "cost without benefit" regression (lerped == 0) is visible at
// a glance.
void GdxMenu::DrawInterpStats() {
    if (gdx_gfx_interp_host_active() == 0) {
        return;
    }
    ImGui::SeparatorText("Frame Interpolation");
    if (gdx_gfx_interp_tick_active() == 0) {
        // The toggle is on but main.cpp forced interpolation off this tick, so the numbers below
        // are from before the editor was entered. Show the paused truth instead of stale figures.
        ImGui::TextDisabled("Interpolation paused (editor active)");
    } else {
        const double presentedFps = gdx_gfx_interp_presents_per_sec();
        const float imguiFps = ImGui::GetIO().Framerate;
        const int m = gdx_gfx_interp_last_subframes();
        const int lerped = gdx_gfx_interp_last_lerped();
        const int snapped = gdx_gfx_interp_last_snapped();
        // The presents/s meter is bridge-measured; fall back to ImGui's rate until the first
        // window fills.
        const double shownFps = (presentedFps > 0.0) ? presentedFps : static_cast<double>(imguiFps);
        ImGui::Text("Presented: %.0f fps (sim 60 Hz)", shownFps);
        ImGui::Text("Sub-frames/tick (M): %d", m);
        if (lerped == 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.20f, 1.0f),
                               "Interpolated slots: 0 (no tween - snapping every tick)");
        } else {
            ImGui::Text("Interpolated slots: %d   Snapped: %d", lerped, snapped);
        }
    }
    ImGui::Separator();
}

// The Workshop custom blocks. Each needs a table, a subprocess snapshot or a modal, which no plain
// registry widget can express.
void GdxMenu::DrawTexturePacks() {
    static char sReloadStatus[160] = "";
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

    std::vector<GdxWorkshopPackInfo> packs = GdxWorkshopListPacks();
    ImGui::TextDisabled("%d override(s) available across mounted packs.", GdxWorkshopOverrideCount());

    if (packs.empty()) {
        ImGui::TextDisabled("No packs found. Drop .o2r packs into the mods/ folder.");
    } else if (ImGui::BeginTable("##WorkshopPacks", 4,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t row = 0; row < packs.size(); row++) {
            const auto& p = packs[row];
            ImGui::TableNextRow();
            ImGui::PushID(p.basename.c_str());

            ImGui::TableSetColumnIndex(0);
            bool enabled = !p.disabled;
            // ImGui::Checkbox, not UIWidgets::Checkbox: this lives in a 32px WidthFixed column, and
            // UIWidgets' re-implementation always adds ItemInnerSpacing.x * 2 of label gutter to
            // its bounding box even for an empty "##" label, which would push it out of the column.
            // The pack state is not a CVar either -- it lives in the comma-joined DisabledPacks
            // list that GdxWorkshopSetPackDisabled maintains.
            if (ImGui::Checkbox("##en", &enabled)) {
                // The archive set is mounted once, so this takes effect on the next Reload or boot.
                GdxWorkshopSetPackDisabled(p.id.c_str(), p.basename.c_str(), enabled ? 0 : 1);
            }
            UIWidgets::Tooltip("Enable or disable this pack. Takes effect on the next Reload or boot.");

            ImGui::TableSetColumnIndex(1);
            // Rows are listed in mount-priority order (see GdxWorkshopListPacks), so a move rewrites
            // PackOrder as the full identity list with this row swapped.
            auto movePack = [&](int delta) {
                // size_t wrap makes an off-the-end target huge, so one check covers both edges.
                if (row + delta >= packs.size()) {
                    return;
                }
                std::string joined;
                for (size_t i = 0; i < packs.size(); i++) {
                    size_t src = i;
                    if (i == row) {
                        src = row + delta;
                    } else if (i == row + delta) {
                        src = row;
                    }
                    if (i != 0) {
                        joined += ",";
                    }
                    joined += packs[src].Identity();
                }
                GdxWorkshopSetPackOrder(joined.c_str());
            };
            ImGui::BeginDisabled(row == 0);
            if (ImGui::SmallButton("Up")) {
                movePack(-1);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(row + 1 >= packs.size());
            if (ImGui::SmallButton("Dn")) {
                movePack(1);
            }
            ImGui::EndDisabled();
            UIWidgets::Tooltip("Move this pack up/down in mount priority (rewrites the PackOrder\n"
                               "list). Takes effect on the next Reload or boot.");

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(p.basename.c_str());
            if (!p.name.empty()) {
                ImGui::TextDisabled("%s", p.name.c_str());
            }

            ImGui::TableSetColumnIndex(3);
            if (p.manifestPresent) {
                ImGui::Text("v%s by %s", p.version.empty() ? "?" : p.version.c_str(),
                            p.author.empty() ? "?" : p.author.c_str());
                if (!p.id.empty() && p.id != p.basename) {
                    ImGui::TextDisabled("id: %s", p.id.c_str());
                }
                if (p.gameVersionMismatch) {
                    ImGui::TextColored(kRed, "game_version mismatch (%s)", p.gameVersion.c_str());
                }
                if (p.keySchemeMismatch) {
                    ImGui::TextColored(kRed, "key_scheme_version mismatch (%s)", p.keySchemeVersion.c_str());
                }
                auto joinTokens = [](const std::vector<std::string>& tokens) {
                    std::string joined;
                    for (size_t i = 0; i < tokens.size(); i++) {
                        if (i != 0) {
                            joined += ", ";
                        }
                        joined += tokens[i];
                    }
                    return joined;
                };
                if (!p.missingDepends.empty()) {
                    ImGui::TextColored(kRed, "missing dependency: %s", joinTokens(p.missingDepends).c_str());
                }
                if (!p.activeConflicts.empty()) {
                    ImGui::TextColored(kRed, "conflicts with: %s", joinTokens(p.activeConflicts).c_str());
                }
            } else {
                ImGui::TextDisabled("(no manifest.json)");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        ImGui::TextDisabled("Packs mount in the order listed above; later packs win per-file.");
    }

    if (ImGui::Button("Reload packs")) {
        GdxWorkshopReload(sReloadStatus, sizeof(sReloadStatus));
    }
    UIWidgets::Tooltip("Re-scans mods/, re-mounts packs, and clears the texture cache so edits\n"
                       "appear without restarting.");
    ImGui::SameLine();
    if (ImGui::Button("Open mods folder")) {
        GdxOpenFolder(GdxWorkshopModsDir(true));
    }
    UIWidgets::Tooltip("Open the mods/ folder in your file browser (created if absent).");
    if (sReloadStatus[0] != '\0') {
        ImGui::TextDisabled("%s", sReloadStatus);
    }
}

// Lua script packs (port/gdx_lua.cpp): one row per discovered scripts/*.lua entry, with load/
// runtime errors surfaced in place. The runtime scans on enable and on every workshop epoch bump,
// so "Reload packs" above is also the "retry errored scripts" button — no second reload here.
void GdxMenu::DrawScripts() {
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

    if (!gdx_lua_enabled()) {
        ImGui::TextDisabled("Script packs are off. Tick \"Enable Lua script packs\" to scan mods/*.o2r.");
        return;
    }

    const int count = GdxLuaScriptCount();
    if (count == 0) {
        ImGui::TextDisabled("No scripts found. Packs provide them as scripts/*.lua inside the .o2r.");
        return;
    }

    if (ImGui::BeginTable("##WorkshopScripts", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Script", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int row = 0; row < count; row++) {
            GdxLuaScriptInfo info;
            if (!GdxLuaGetScriptInfo(row, &info)) {
                continue; // a reload landed mid-frame; next frame re-reads from zero
            }
            ImGui::TableNextRow();
            ImGui::PushID(row);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(info.pack);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(info.name);
            ImGui::TableSetColumnIndex(2);
            if (info.errored) {
                ImGui::TextColored(kRed, "error: %s", info.error);
            } else {
                ImGui::TextDisabled("ok");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        ImGui::TextDisabled("Errored scripts are disabled until the next Reload packs.");
    }
}


// Offline per-class asset dump, native-first via the bundled gdx-extract (falling back to
// tools/gen_dump_all.py in dev checkouts without the native binary). The work lives in
// port/gdx_dump_launch.{h,cpp}; this block only READS the shared snapshot. Every subprocess runs on
// a detached worker thread, one child PER CLASS so a broken class never aborts the rest.
//
// Custom rather than registry data because the class list is DISCOVERED AT RUNTIME (an async
// `--list-classes` probe), so the per-class checkboxes cannot be declared ahead of time. Their
// disabled state comes from the batch snapshot this block already holds rather than from the
// named-reason map, which would have to take a second locked copy of it.
void GdxMenu::DrawAssetDump() {
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

    // Backend discovery is pure filesystem/PATH, no subprocess; the class-list probe is async.
    // Both run once and are cached across frames by the function-local statics.
    static gdx::DumpEnvironment sDumpEnv = gdx::GdxDumpDiscover();
    static bool sProbeKicked = (gdx::GdxDumpBeginClassListProbe(sDumpEnv), true);
    (void)sProbeKicked;

    ImGui::TextWrapped("Decode named assets straight from the extracted archive - no gameplay "
                       "needed. Runs the bundled dump tool once per selected class; results land "
                       "in dump/ (same place as the runtime texture dump).");
    if (!sDumpEnv.available) {
        ImGui::TextDisabled("%s", sDumpEnv.reason.c_str());
    }

    gdx::DumpBatchSnapshot snap = gdx::GdxDumpSnapshot();
    const bool running = snap.running;
    const std::vector<std::string> dumpClasses = gdx::GdxDumpCurrentClasses();

    ImGui::BeginDisabled(!sDumpEnv.available || running);
    if (ImGui::BeginTable("##DumpClasses", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& cls : dumpClasses) {
            ImGui::TableNextColumn();
            // The per-class CVar name (gEnhancements.Workshop.DumpClass.<name>) is built at
            // runtime; CVarCheckbox takes a plain const char*, so the dynamic key works unchanged.
            // Nested inside the BeginDisabled above -- ImGui's BeginDisabled(false) preserves an
            // already-disabled scope, so the rows stay greyed while a dump runs.
            std::string cvarKey = "gEnhancements.Workshop.DumpClass." + cls;
            std::string label = gdx::GdxDumpPrettyName(cls) + "##dumpclass_" + cls;
            UIWidgets::CVarCheckbox(label.c_str(), cvarKey.c_str(),
                                    UIWidgets::CheckboxOptions().DefaultValue(true));
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Dump selected")) {
        std::vector<std::string> selected;
        for (const auto& cls : dumpClasses) {
            std::string cvarKey = "gEnhancements.Workshop.DumpClass." + cls;
            if (CVarGetInteger(cvarKey.c_str(), 1) != 0) {
                selected.push_back(cls);
            }
        }
        gdx::GdxDumpStartBatch(sDumpEnv, selected, GdxWorkshopDumpDir(true));
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump everything")) {
        gdx::GdxDumpStartBatch(sDumpEnv, dumpClasses, GdxWorkshopDumpDir(true));
    }
    ImGui::EndDisabled();

    // Cancel is cooperative: it stops AFTER the current class finishes.
    ImGui::SameLine();
    ImGui::BeginDisabled(!running || snap.cancelRequested);
    if (ImGui::Button("Cancel")) {
        gdx::GdxDumpRequestCancel();
    }
    ImGui::EndDisabled();
    UIWidgets::Tooltip("Stops after the current class finishes. The running class is left to "
                       "complete cleanly - no child process is killed.");

    for (const auto& p : snap.classes) {
        std::string pretty = gdx::GdxDumpPrettyName(p.name);
        switch (p.phase) {
        case gdx::DumpPhase::Queued:
            ImGui::TextDisabled("%s: queued", pretty.c_str());
            break;
        case gdx::DumpPhase::Running: {
            const char spin[] = {'|', '/', '-', '\\'};
            ImGui::Text("%s: running %c", pretty.c_str(),
                        spin[static_cast<int>(ImGui::GetTime() * 4.0) & 3]);
            break;
        }
        case gdx::DumpPhase::Done:
            if (p.itemsDumped >= 0) {
                ImGui::Text("%s: done - %d item(s) in %.1fs", pretty.c_str(), p.itemsDumped,
                            p.elapsedSeconds);
            } else {
                ImGui::Text("%s: done - %.1fs", pretty.c_str(), p.elapsedSeconds);
            }
            break;
        case gdx::DumpPhase::Failed:
            ImGui::TextColored(kRed, "%s: FAILED (exit %d) %s", pretty.c_str(), p.exitCode,
                               p.lastLine.c_str());
            break;
        default:
            ImGui::TextDisabled("%s: idle", pretty.c_str());
            break;
        }
    }
    if (!snap.summary.empty()) {
        ImGui::TextDisabled("%s", snap.summary.c_str());
    }

    if (ImGui::Button("Open dump folder")) {
        GdxOpenFolder(GdxWorkshopDumpDir(true));
    }
    UIWidgets::Tooltip("Open the dump/ folder in your file browser (created if absent).");
}

// 64DD durable-save sidecar status and the one-shot format authorization modal.
void GdxMenu::DrawDdSave() {
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

    ImGui::Text("Sidecar: %s", gdx_disk_sidecar_present() ? "present" : "none yet");
    ImGui::Text("Journal records: %d", gdx_disk_sidecar_record_count());
    ImGui::Text("Last flush: %s", gdx_disk_last_flush_ok() ? "ok" : "FAILED");
    if (gdx_disk_format_refused_this_boot()) {
        ImGui::TextColored(kRed, "The disk's MFS save area is uninitialized.");
        if (ImGui::Button("Initialize DD save area")) {
            ImGui::OpenPopup("##ddformat");
        }
        UIWidgets::Tooltip("Authorizes a one-time format of the 64DD MFS save area on the NEXT boot.");
        if (ImGui::BeginPopupModal("##ddformat", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(
                "Initialize the 64DD MFS save area?\n\n"
                "This authorizes a one-time format the NEXT time the game boots. The format is\n"
                "written to the durable save sidecar only -- your original .ndd disk file is never\n"
                "modified. This is needed before Course Edit / Machine Create can save to disk.");
            ImGui::Separator();
            if (ImGui::Button("Authorize (next boot)")) {
                CVarSetInteger("gEnhancements.Workshop.AllowDDFormatOnce", 1);
                GdxSaveCvars();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                ImGui::CloseCurrentPopup();
            }
            // Default-park focus on Cancel, not the format authorization.
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
    }
}

// Pop-out buttons for the libultraship dev windows. Custom because the three share one error line:
// a missing window is REPORTED rather than ignored, and the report belongs to the group.
void GdxMenu::DrawDevToolButtons() {
    // These names must match the registrations in main.cpp / the libultraship Gui ctor exactly. A
    // typo produces a button that looks fine and does nothing.
    static std::string sToolStatus;
    struct ToolButton {
        const char* label;
        const char* window;
        const char* tip;
    };
    static const ToolButton kTools[] = {
        { "Open Stats", "Stats", "Toggle the live frame-timing / renderer Stats window." },
        { "Open Console", "Console", "Toggle the developer console and command history." },
        { "Open Gfx Debugger", "Gfx Debugger", "Toggle the Fast3D display-list debugger." },
    };
    for (int i = 0; i < static_cast<int>(sizeof(kTools) / sizeof(kTools[0])); ++i) {
        if (i != 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(kTools[i].label)) {
            if (GdxToggleWindow(kTools[i].window)) {
                sToolStatus.clear();
            } else {
                sToolStatus = std::string(kTools[i].window) + " is not registered in this build.";
            }
        }
        UIWidgets::Tooltip(kTools[i].tip);
    }
    if (!sToolStatus.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", sToolStatus.c_str());
    }
}

// The checkbox surface for port/gdx_dev_gates.{h,c}. Table-driven: adding a gate to kGates in
// gdx_dev_gates.c makes it appear here with no edit to this function.
//
// Three sections, matching the bucket policy:
//   Logging     (Bucket D) — persisted CVar adopted at boot; carries the "applies to new output;
//                            restart to capture boot" caveat, and is DISABLED when an environment
//                            variable pinned it for this run.
//   Diagnostics (Bucket A) — logging only, safe to leave on, live.
//   Behaviour   (Bucket B) — CHANGES RENDERING OR GAME BEHAVIOUR. Whole section compiled out
//                            unless GDX_DEV_TOOLS (Debug-only by default; see port/CMakeLists.txt).
//
// A toggle writes the CVar, schedules the config save, and calls gdx_dev_gates_refresh() so the
// change lands on the CURRENT frame: the frame loop's own refresh already ran before the menu drew.
void GdxMenu::DrawDevGates() {
    ImGui::SeparatorText("Developer gates");
    ImGui::TextWrapped("These replace the port's GDX_* environment variables. Changes apply "
                       "immediately and persist in gdiffuser.cfg.json. A variable exported at "
                       "launch still works and is marked below.");

    // Confirmed rather than immediate: this discards every gate across all three sections at once
    // with no undo, and one stray click would cost someone their whole bisect setup.
    if (ImGui::Button("Reset all gates to defaults")) {
        ImGui::OpenPopup("##resetgates");
    }
    UIWidgets::Tooltip("Clears every gate back to stock. Gates pinned by an environment "
                       "variable are unaffected until the next launch.");
    if (ImGui::BeginPopupModal("##resetgates", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int changed = 0;
        for (int id = 0; id < gdx_dev_gate_count(); ++id) {
            if ((gdx_dev_gate(id) != 0) != (gdx_dev_gate_default(id) != 0)) {
                ++changed;
            }
        }
        ImGui::TextUnformatted("Reset every developer gate to its default?");
        ImGui::Separator();
        if (changed == 0) {
            ImGui::TextDisabled("Nothing to reset - every gate is already at its default.");
        } else {
            ImGui::Text("%d gate(s) will be turned back to stock. This cannot be undone.", changed);
        }
        ImGui::Separator();
        if (ImGui::Button("Reset")) {
            for (int id = 0; id < gdx_dev_gate_count(); ++id) {
                CVarSetInteger(gdx_dev_gate_cvar_name(id), gdx_dev_gate_default(id));
            }
            GdxSaveCvars();
            gdx_dev_gates_refresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            ImGui::CloseCurrentPopup();
        }
        // Default-park focus on Cancel, not the mass reset.
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }

    // Presentation order, which is NOT the enum order: logging first (it is the one a bug reporter
    // needs), then read-only diagnostics, then the dangerous section last.
    static const int kBucketOrder[] = { GDX_GATE_BUCKET_BOOT, GDX_GATE_BUCKET_DIAG,
                                        GDX_GATE_BUCKET_BEHAVIOR };
    for (int bucketIndex = 0; bucketIndex < (int) (sizeof(kBucketOrder) / sizeof(kBucketOrder[0]));
         ++bucketIndex) {
        const int bucket = kBucketOrder[bucketIndex];
#ifndef GDX_DEV_TOOLS
        // Bucket B is not compiled into this build: its gates are hard-wired to 0 with no getenv
        // and no CVar read, so live checkboxes would be a lie. Say the section is excluded rather
        // than dropping it, which would read as "this build is broken".
        if (bucket == GDX_GATE_BUCKET_BEHAVIOR) {
            ImGui::SeparatorText("Behavior overrides (not in this build)");
            ImGui::TextDisabled("Compiled out of Release. These gates change what the game renders or\n"
                                "how it sounds, and exist to bisect bugs rather than to play with.\n"
                                "Configure with -DGDX_FORCE_DEV_TOOLS=ON to build them in.");
            continue;
        }
#endif
        if (bucket == GDX_GATE_BUCKET_BOOT) {
            ImGui::SeparatorText("Logging");
            ImGui::TextWrapped("Read at startup from the saved setting, so enabling one here and "
                               "restarting captures the next boot.");
        } else if (bucket == GDX_GATE_BUCKET_DIAG) {
            ImGui::SeparatorText("Diagnostics");
            ImGui::TextWrapped("Extra log output only. Safe to leave on; enable the Logging gates "
                               "above so the lines reach gdiffuser-run.log.");
        } else {
            ImGui::SeparatorText("Behavior overrides (not for normal play)");
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
                               "WARNING: these change what the game renders or how it sounds. "
                               "They exist to bisect bugs, not to play with. Leave them off unless "
                               "you are reproducing a specific issue.");
            ImGui::TextDisabled("Debug builds only - this section is compiled out of a Release build.");
        }

        for (int group = 0; group < GDX_GATE_GROUP_COUNT; ++group) {
            bool groupHeaderDrawn = false;
            for (int id = 0; id < gdx_dev_gate_count(); ++id) {
                if (gdx_dev_gate_bucket(id) != bucket || gdx_dev_gate_group(id) != group) {
                    continue;
                }
                // Only the Diagnostics and Behavior sections need a per-domain sub-header; the
                // Logging bucket is a single group already named by its section title.
                if (!groupHeaderDrawn && bucket != GDX_GATE_BUCKET_BOOT) {
                    groupHeaderDrawn = true;
                    ImGui::TextDisabled("%s", gdx_dev_gate_group_name(group));
                }

                const bool pinned = gdx_dev_gate_is_env_pinned(id) != 0;
                bool value = gdx_dev_gate(id) != 0;

                if (pinned) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Checkbox(gdx_dev_gate_label(id), &value)) {
                    CVarSetInteger(gdx_dev_gate_cvar_name(id), value ? 1 : 0);
                    GdxSaveCvars();
                    gdx_dev_gates_refresh(); // land the change on THIS frame
                }
                if (pinned) {
                    ImGui::EndDisabled();
                }

                // AllowWhenDisabled: ImGui otherwise suppresses hover on disabled items, so the one
                // checkbox that refuses to move would also be the only one that never says why.
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s\n\nEnvironment: %s\nSetting: %s", gdx_dev_gate_help(id),
                                      gdx_dev_gate_env_name(id), gdx_dev_gate_cvar_name(id));
                }
                GdxModifiedMarker(value != (gdx_dev_gate_default(id) != 0));

                // Order matters: a BOOT gate merely PRESENT in the environment but resolved to off
                // is freely toggleable, so it gets the restart hint rather than a "(started from
                // ...)" label that would be stale the moment the user ticked it.
                if (pinned) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(pinned ON by %s this run)", gdx_dev_gate_env_name(id));
                } else if (bucket == GDX_GATE_BUCKET_BOOT) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(applies to new output; restart to capture boot)");
                } else if (gdx_dev_gate_from_env(id)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(started from %s)", gdx_dev_gate_env_name(id));
                }
            }
        }
    }

    // Bucket C stays environment-only: consumed before the console exists, or carrying a value
    // rather than a flag. Listed so this page is a complete map of the port's switches.
    ImGui::SeparatorText("Environment only (no setting)");
    ImGui::TextWrapped("Consumed before the settings system exists, or not a simple on/off:");
    ImGui::BulletText("GDX_INPUT_SCRIPT - deterministic input playback for unattended tests");
    ImGui::BulletText("FZEROX_ROM, GDX_STRICT_ARCHIVE, GDX_DUMP_SELFTEST - boot and tooling paths");
    ImGui::BulletText("GDX_CAPTURE_FRAMES / GDX_CAPTURE_MODE / GDX_CAPTURE_WINDOW - \"start:count\" captures");
    ImGui::BulletText("GDX_PCM_CAPTURE, GDX_PCM_CAPTURE_FRAMES - audio capture prefix and length");
    ImGui::BulletText("GDX_RAND_SEED1 / GDX_RAND_SEED2 - RNG determinism pins (numeric seeds)");
    ImGui::BulletText("GDX_SEED_BOOT_LOGO, GDX_AUDIO_THREAD, GDX_AI_CUSHION - decided before this menu exists");
    ImGui::BulletText("GDX_INTERP_P1 / GDX_INTERP_P2 - interpolation test overrides (see Graphics)");
}

// CMake supplies the release label; the fallback keeps standalone source builds identifiable.
#ifndef GDX_VERSION_STRING
#define GDX_VERSION_STRING "development"
#endif
static constexpr const char* kGdxVersionString = GDX_VERSION_STRING;

void GdxMenu::DrawAboutMenu() {
    // Lazy one-shot load on first open rather than at boot: the About page is visited rarely, and a
    // failed load (an archive predating the branding entry) must degrade to the text title rather
    // than block the menu. tried/loaded are separate so a failure does not retry every frame.
    static bool sLogoTried = false;
    static bool sLogoLoaded = false;
    if (!sLogoTried) {
        sLogoTried = true;
        auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(
            Ship::Context::GetInstance()->GetWindow()->GetGui());
        if (gui != nullptr) {
            gui->LoadTextureFromRawImage("gdx-logo", "branding/gdiffuser-logo.png");
            sLogoLoaded = gui->GetTextureByName("gdx-logo") != nullptr;
        }
    }
    bool logoDrawn = false;
    if (sLogoLoaded) {
        auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(
            Ship::Context::GetInstance()->GetWindow()->GetGui());
        ImTextureID logo = (gui != nullptr) ? gui->GetTextureByName("gdx-logo") : nullptr;
        if (logo != nullptr) {
            // Source is 1024x324; the height below preserves that aspect.
            const float w = std::min(420.0f, ImGui::GetContentRegionAvail().x);
            const float h = w * (324.0f / 1024.0f);
            ImGui::Image(logo, ImVec2(w, h));
            logoDrawn = true;
        }
    }
    if (!logoDrawn) {
        ImGui::Text("G-Diffuser");
    }
    ImGui::Text("Version %s", kGdxVersionString);
    ImGui::TextDisabled("A native PC source port of F-Zero X (N64) + Expansion Kit (64DD)");

    ImGui::Separator();

    ImGui::TextDisabled("Credits / licenses");
    ImGui::BulletText("F-Zero X decompilation (inspectredc/fzerox) - CC0 1.0");
    ImGui::BulletText("cxd4 RSP interpreter (Iconoclast) - CC0");
    ImGui::BulletText("libultraship (fork of Kenix3/libultraship) - MIT");
    ImGui::BulletText("Torch asset tool (HarbourMasters) - MIT");
    ImGui::BulletText("StormLib (Ladislav Zezula) - MIT");
    ImGui::BulletText("Dear ImGui (Omar Cornut) - MIT");
    ImGui::BulletText("SDL2 (Sam Lantinga) - zlib");
    ImGui::BulletText("Montserrat and Inconsolata fonts - SIL Open Font License 1.1");
    ImGui::BulletText("Logo & icon artwork - Kiziio (github.com/Kiziio1)");

    ImGui::Separator();
    ImGui::TextDisabled("https://github.com/Zorkats/G-Diffuser");

}
