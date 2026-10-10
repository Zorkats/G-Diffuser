// G-Diffuser — port entry point.
// libultraship is initialized step by step rather than through the one-shot CreateInstance: the
// ControlDeck must be constructed AFTER the Context + ConsoleVariables exist, because its
// GlobalSDLDeviceSettings reads CVars via Context::GetInstance().

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "ship/resource/archive/Archive.h"
#include "ship/audio/AudioPlayer.h"
#include "ship/audio/Audio.h"
#include "resource/ResourceFactories.h"
// LUS's concrete N64 ControlDeck. It is `final`, so used directly rather than subclassed, and the
// abstract Ship::ControlDeck reads nothing — do not go back to extending it.
#include "libultraship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/physicaldevice/PhysicalDeviceType.h"
#include "libultraship/libultra/controller.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "fast/Fast3dWindow.h"
#include "ship/debug/Console.h"
#include "ship/window/gui/GuiWindow.h"
#include "gdx_gui.h"
#include "gdx_menu.h"
#include "libultraship/window/gui/GfxDebuggerWindow.h"
#include "libultraship/window/gui/InputEditorWindow.h"
#include "gdx_ghost_window.h"
#include "gdx_content_window.h"
#include "gdx_hackmods.h"
#include "gdx_hackmods_window.h"
#include "gdx_input_viewer.h"
#include "gdx_console_log.h"
#include "port_log.h"
#include "rom_buffer.h"
#include "gdx_firstboot.h"
#include "gdx_firstboot_gui.h"
#include "gdx_fps_overlay.h"
#include "gdx_perf.h"
#include "gdx_dev_gates.h"
#include "gdx_extract_launch.h"
#include "gdx_audio_thread.h"
#include "gdx_frame_pacer.h"
#include "gdx_workshop.h"
#include "n64_gfx_bridge.h"
#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" void GDiffuser_LoadAllAssets(void);
extern "C" void bootproc(void);                // decomp boot entry (src/sys/sys_main.c)
extern "C" void gdx_sched_init(void);
extern "C" void gdx_sched_drain_deferred_wakes(void);
extern "C" void gdx_init_rom(int argc, char** argv, int archivesValidated); // archivesValidated gates the no-ROM boot
extern "C" void gdx_vi_tick(void);             // posts retrace, which runs the Main game fiber inline
extern "C" void gdx_dispatch(void);
extern "C" void gdx_vi_present_fallback(void);
extern "C" void gdx_controller_poll(void);
extern "C" void gdx_fixed_aspect_tick(void);
extern "C" int gdx_get_force_fixed_aspect(void); // 1 while an EK editor forces 4:3
extern "C" void gdx_rdram_init(void);
extern "C" void gdx_register_host_range(void* ptr, size_t size); // expose a range to TryResolveAddress
extern "C" void gdx_register_main_module_range(void); // lets low32 EXE/BSS segment tokens resolve
extern "C" void gdx_game_request_reset(void);
extern "C" void gdx_disk_save_tick(void);
void GdxAchievements_Tick(void); // port/gdx_achievements.h — F10 tracker; C++ linkage, lazy-init
extern "C" void gdx_disk_save_flush(void);
extern "C" void gdx_discord_tick(void);
extern "C" void gdx_discord_shutdown(void);
extern "C" void gdx_pcm_capture_init(void);
extern "C" int  gdx_pcm_capture_finished(void);
extern "C" void gdx_pcm_capture_shutdown(void);
extern "C" int  GdxSegmentSourcePreload(uint32_t romBase);
extern "C" int  GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize);
extern "C" void gdx_boot_warm_asset_segments(void);
extern "C" int  gdx_vi_divider(void);               // 1 = 60Hz, 3 = Course Edit cursor mode (~20Hz)
extern "C" void gdx_course_edit_mouse_cursor_tick(void); // port/gdx_course_edit_mouse.cpp
extern "C" void gdx_course_edit_mouse_grab_tick(void);   // port/gdx_course_edit_mouse.cpp
extern "C" void gdx_hide_os_cursor_tick(void);           // port/gdx_course_edit_mouse.cpp

static void logStep(const char* s) {
    gdx_port_logf("[G-Diffuser] %s\n", s);
}

static void loadControllerDatabase() {
    const std::string databasePath = Ship::Context::LocateFileAcrossAppDirs("gamecontrollerdb.txt");
    std::error_code error;

    if (!std::filesystem::is_regular_file(databasePath, error)) {
        if (error) {
            gdx_port_logf("[input] controller database unavailable: %s (%d: %s)\n", databasePath.c_str(),
                          error.value(), error.message().c_str());
        } else {
            gdx_port_logf("[input] controller database missing: %s\n", databasePath.c_str());
        }
        return;
    }

    const int mappingsAdded = SDL_GameControllerAddMappingsFromFile(databasePath.c_str());
    if (mappingsAdded < 0) {
        gdx_port_logf("[input] controller database load FAILED: %s (%s)\n", databasePath.c_str(), SDL_GetError());
        return;
    }

    gdx_port_logf("[input] controller database loaded: %s (%d mapping(s))\n", databasePath.c_str(), mappingsAdded);
}

static void logConnectedJoysticks() {
    const int joystickCount = SDL_NumJoysticks();
    if (joystickCount < 0) {
        gdx_port_logf("[input] SDL_NumJoysticks FAILED: %s\n", SDL_GetError());
        return;
    }

    for (int index = 0; index < joystickCount; ++index) {
        const SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
        char guidString[33] = {};
        SDL_JoystickGetGUIDString(guid, guidString, sizeof(guidString));

        const char* name = SDL_JoystickNameForIndex(index);
        gdx_port_logf(
            "[input] joystick %d: name=\"%s\" guid=%s vendor=0x%04X product=0x%04X version=0x%04X "
            "gamecontroller=%s\n",
            index, name != nullptr ? name : "<unknown>", guidString,
            static_cast<unsigned int>(SDL_JoystickGetDeviceVendor(index)),
            static_cast<unsigned int>(SDL_JoystickGetDeviceProduct(index)),
            static_cast<unsigned int>(SDL_JoystickGetDeviceProductVersion(index)),
            SDL_IsGameController(index) == SDL_TRUE ? "yes" : "no");
    }
}

// ── Dev Tools gate layer <-> console-variable adapters ────────────────────────────────────────
// port/gdx_dev_gates.c is deliberately dependency-free C (the standalone unit-test executables
// compile it unchanged), so it receives the CVar entry points as function pointers. These thunks
// make the pointer types match exactly, rather than assuming int32_t and int are the same type on
// every toolchain we build with.
static int GdxGateCVarGet(const char* name, int defaultValue) {
    return static_cast<int>(CVarGetInteger(name, static_cast<int32_t>(defaultValue)));
}
static void GdxGateCVarSet(const char* name, int value) {
    CVarSetInteger(name, static_cast<int32_t>(value));
}

// ── Frame-interpolation host support ─────────────────────────────────────────────────────────
// One logic tick = one N64 NTSC field. The sim advances exactly one tick per loop iteration; with
// interpolation on the presents decouple, but this deadline still paces the sim at 60 Hz (the
// frame pacer is mutually excluded — see gdx_frame_pacer.c).
static constexpr double kGdxInterpTickSeconds = 1.001 / 60.0;
// Hard ceiling for the per-tick sub-frame count. With VSync off presents don't block, so this is
// the only bound; 8 covers both the UI's 480fps Target FPS ceiling (480/60) and a 480Hz panel.
static constexpr int kGdxInterpMaxSubframes = 8;

// Simulation rate in ticks/sec, MEASURED rather than assumed: the interpolation burst runs M
// blocking presents inside one loop iteration and nothing recovers an overrun, so the sim once
// silently ran at 8.6 Hz while every other surface still claimed 60. 0.0 means "not sampled yet".
static double gGdxSimHzMeasured = 0.0;
extern "C" double gdx_host_sim_hz(void) {
    return gGdxSimHzMeasured;
}

// Bridge telemetry getters for the [interp-p2] line. Declared here rather than in
// n64_gfx_bridge.h — same minimal-include idiom gdx_menu.cpp uses for the other accessors.
extern "C" double gdx_gfx_interp_presents_per_sec(void);
extern "C" int gdx_gfx_interp_last_lerped(void);
extern "C" int gdx_gfx_interp_last_snapped(void);
// See the block comment on gdx_gfx_interp_set_sim_slip in n64_gfx_bridge.cpp for why the host,
// not the bridge, owns this measurement.
extern "C" void gdx_gfx_interp_set_sim_slip(double seconds);
extern "C" int gdx_gfx_interp_last_borrowed(void);
// Per-racer matrices whose prev keyframe had its rotation frozen across a side-attack model-basis
// discontinuity. Bursts twice per side attack; must stay 0 through spin attacks.
extern "C" int gdx_gfx_interp_last_basis_fixed(void);
extern "C" int gdx_gfx_interp_last_cam_rebuilt(void);
extern "C" int gdx_gfx_interp_last_cam_rejected(void);
extern "C" double gdx_gfx_interp_last_cam_eye_delta(void);
// Why the camera rebuild did not run this tick: poseread/id/unreadable/build/mismatch/prevpose/snap.
extern "C" int gdx_gfx_interp_last_cam_why(int which);
extern "C" int gdx_gfx_interp_last_vp_lerped(void);
extern "C" int gdx_gfx_interp_last_vp_snapped(void);
extern "C" int gdx_gfx_interp_last_vtx_lerped(void);
extern "C" int gdx_gfx_interp_last_vtx_snapped(void);

// Monotonic seconds clock the gfx bridge samples to derive each sub-frame's t. Shared epoch so
// the logic-deadline wait below converts a deadline back to the same time base.
static const std::chrono::steady_clock::time_point gGdxHostClockEpoch = std::chrono::steady_clock::now();
static double gdx_host_now_seconds(void) {
    using namespace std::chrono;
    return duration<double>(steady_clock::now() - gGdxHostClockEpoch).count();
}
// Hold the host thread until `deadlineSeconds` (same base as gdx_host_now_seconds). No-op if the
// presents already spent the tick budget (VSync-on case). Keeps the SIM locked to 60 Hz when the
// sub-frame loop finished early (VSync-off case) — this is interpolation's logic pacer.
static void gdx_host_pace_logic_until(double deadlineSeconds) {
    using namespace std::chrono;
    const auto target = gGdxHostClockEpoch +
                        duration_cast<steady_clock::duration>(duration<double>(deadlineSeconds));
    if (steady_clock::now() < target) {
        std::this_thread::sleep_until(target);
    }
}

// libultraship's GetCurrentRefreshRate() can report 60 on a high-refresh panel (observed: 60 on a
// 143 Hz display), which would pin "Match Refresh Rate" to one sub-frame per tick. Cross-check the
// OS and take the higher of the two; returns 0 on failure so a genuine 60 Hz panel is unaffected.
//
// This MUST resolve the monitor the window actually sits on: taking the max Hz over every active
// display path puts a window on the lower-Hz panel of a mixed-refresh rig into slow motion.
// MonitorFromRect (Fast3dWindow exposes no HWND), matched by GDI device name against
// QueryDisplayConfig, then EnumDisplaySettingsW; max-over-paths is a last resort only.
#ifdef _WIN32
static int gdx_display_hz_for_device(const wchar_t* deviceName) {
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &dm) &&
        (dm.dmFields & DM_DISPLAYFREQUENCY) != 0 && dm.dmDisplayFrequency > 1) {
        return static_cast<int>(dm.dmDisplayFrequency);
    }
    return 0;
}

// outPath receives a short string naming which resolution path produced the Hz ([interp-diag]).
static int gdx_os_display_refresh_hz(int32_t windowPosX, int32_t windowPosY, uint32_t windowWidth,
                                     uint32_t windowHeight, const char** outPath) {
    *outPath = "unresolved";

    // Resolve the monitor the window rect actually sits on, then its GDI device name.
    wchar_t deviceName[CCHDEVICENAME] = {};
    RECT windowRect = { windowPosX, windowPosY, windowPosX + static_cast<LONG>(windowWidth),
                        windowPosY + static_cast<LONG>(windowHeight) };
    HMONITOR hMonitor = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONEAREST);
    if (hMonitor != nullptr) {
        MONITORINFOEXW info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(hMonitor, &info)) {
            wcsncpy(deviceName, info.szDevice, CCHDEVICENAME - 1);
        }
    }

    // Primary source: QueryDisplayConfig gives the exact rate as a rational for THAT panel —
    // EnumDisplaySettings and the DXGI backend can round down to 60 on high-refresh panels.
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS &&
        pathCount > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                               nullptr) == ERROR_SUCCESS) {
            if (deviceName[0] != L'\0') {
                for (UINT32 i = 0; i < pathCount; ++i) {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
                    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    sourceName.header.size = sizeof(sourceName);
                    sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
                    sourceName.header.id = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS &&
                        _wcsicmp(sourceName.viewGdiDeviceName, deviceName) == 0) {
                        const DISPLAYCONFIG_RATIONAL& r = paths[i].targetInfo.refreshRate;
                        if (r.Denominator != 0) {
                            const double hz =
                                static_cast<double>(r.Numerator) / static_cast<double>(r.Denominator);
                            const int rounded = static_cast<int>(hz + 0.5);
                            if (rounded > 1) {
                                *outPath = "querydisplayconfig:window-monitor";
                                return rounded;
                            }
                        }
                    }
                }
            }
            // Window's monitor didn't resolve or didn't match a QueryDisplayConfig source: fall
            // back to the max over all active paths.
            double best = 0.0;
            for (UINT32 i = 0; i < pathCount; ++i) {
                const DISPLAYCONFIG_RATIONAL& r = paths[i].targetInfo.refreshRate;
                if (r.Denominator != 0) {
                    const double hz = static_cast<double>(r.Numerator) / static_cast<double>(r.Denominator);
                    if (hz > best) {
                        best = hz;
                    }
                }
            }
            const int rounded = static_cast<int>(best + 0.5);
            if (rounded > 1) {
                *outPath = "querydisplayconfig:max-over-paths-fallback";
                return rounded;
            }
        }
    }

    // Secondary path: EnumDisplaySettingsW directly on the window's monitor device name.
    if (deviceName[0] != L'\0') {
        const int hz = gdx_display_hz_for_device(deviceName);
        if (hz > 0) {
            *outPath = "enumdisplaysettings:window-monitor";
            return hz;
        }
    }

    // Last-resort fallback: the primary display's current mode (window monitor unresolved).
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
        (dm.dmFields & DM_DISPLAYFREQUENCY) != 0 && dm.dmDisplayFrequency > 1) {
        *outPath = "enumdisplaysettings:primary-fallback";
        return static_cast<int>(dm.dmDisplayFrequency);
    }
    return 0;
}
#else
static int gdx_os_display_refresh_hz(int32_t, int32_t, uint32_t, uint32_t, const char** outPath) {
    *outPath = "unavailable";
    return 0;
}
#endif

// LUS ControlDeck's connected-port bitmask. ControlDeck::Init() stores this pointer and sets bit
// 0; the Input Editor reads it for its per-port connection display. It must outlive the deck, so
// it lives at file scope.
static uint8_t sGdxControllerBits = 0;

// ── Multi-pad routing: SDL gamepads -> LUS ControlDeck ports ─────────────────────────────────────
// LUS out of the box is single-player: RefreshConnectedSDLGamepads() ignores every new pad on
// ports 1..3, and ControlDeck::Init() seeds default mappings for port 0 only. This gives each
// connected gamepad its own port and seeds defaults once. Three deliberate restrictions:
//   * With fewer than two gamepads we do nothing — a lone pad keeps LUS's permissive port-0
//     default, so a phantom pad (hub/wireless receiver) cannot steal port 0 from the real one.
//   * Assignments are STICKY: only never-seen pads are placed (lowest free port). Re-deriving on
//     every hotplug would renumber survivors mid-race.
//   * Default mappings are only ADDED to a port with no gamepad mapping, so hand-made Input
//     Editor bindings are never overwritten.
// LUS rebuilds its in-memory ignore lists on every hotplug refresh (why the sticky map is ours),
// so routing re-applies when the connected id set changes — but NOT every frame, so the Input
// Editor's per-port checkboxes stay usable.
static std::vector<int32_t> sGdxRoutedGamepadIds;              // last seen connected set, sorted
static std::unordered_map<int32_t, uint8_t> sGdxGamepadPorts;  // SDL instance id -> owned port

// Persisted seat memory, GUID-keyed so assignments survive restarts: SDL instance ids are
// per-session, the hardware GUID is not. CVar string form: "guidhex=port;guidhex=port;..."
// (ports stored 0-based).
static std::unordered_map<std::string, uint8_t> gdxLoadGamepadSeats() {
    std::unordered_map<std::string, uint8_t> seats;
    const std::string raw = CVarGetString("gEnhancements.Input.GamepadSeats", "");
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t end = raw.find(';', pos);
        const std::string entry =
            raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const size_t eq = entry.find('=');
        if (eq > 0 && eq != std::string::npos) {
            const int port = atoi(entry.c_str() + eq + 1);
            if (port >= 0 && port < MAXCONTROLLERS) {
                seats[entry.substr(0, eq)] = static_cast<uint8_t>(port);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return seats;
}

// 32-char hex GUID for a connected instance id, or empty when SDL cannot resolve it.
static std::string gdxGamepadGuidString(int32_t instanceId) {
    SDL_GameController* controller = SDL_GameControllerFromInstanceID(instanceId);
    SDL_Joystick* joystick = controller != nullptr ? SDL_GameControllerGetJoystick(controller) : nullptr;
    if (joystick == nullptr) {
        return {};
    }
    char guidString[33] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guidString, sizeof(guidString));
    return guidString;
}

static bool gdxPortOwnedByAGamepad(uint8_t port) {
    for (const auto& [instanceId, ownedPort] : sGdxGamepadPorts) {
        if (ownedPort == port) {
            return true;
        }
    }
    return false;
}

static void gdxSyncGamepadPortRouting(const std::shared_ptr<Ship::ControlDeck>& controlDeck) {
    if (CVarGetInteger("gEnhancements.Input.AutoAssignGamepadPorts", 1) == 0) {
        return; // opt-out: leave every port exactly as the Input Editor configured it.
    }

    auto devices = controlDeck->GetConnectedPhysicalDeviceManager();
    if (devices == nullptr) {
        return;
    }

    // GetConnectedSDLGamepadNames() is an unordered_map, so sort to get a stable, reproducible
    // order. SDL instance ids increase monotonically as devices are opened, which makes ascending
    // order equal to plug order — so a pad that was already plugged in sorts before a newcomer.
    std::vector<int32_t> ids;
    for (const auto& [instanceId, name] : devices->GetConnectedSDLGamepadNames()) {
        ids.push_back(instanceId);
    }
    std::sort(ids.begin(), ids.end());

    if (ids == sGdxRoutedGamepadIds) {
        return; // connected set unchanged since the last apply — nothing to re-route.
    }
    sGdxRoutedGamepadIds = ids;

    if (ids.size() < 2) {
        sGdxGamepadPorts.clear();
        return; // single-pad (or no-pad) setup: keep LUS's permissive port-0 default.
    }

    // Release the ports of pads that are gone, so a newcomer can reuse them.
    for (auto it = sGdxGamepadPorts.begin(); it != sGdxGamepadPorts.end();) {
        if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
            it = sGdxGamepadPorts.erase(it);
        } else {
            ++it;
        }
    }

    // Place pads we have not seen before: a GUID the seat memory knows goes back to its remembered
    // port when that port is free; anything else takes the lowest unowned port.
    const std::unordered_map<std::string, uint8_t> rememberedSeats = gdxLoadGamepadSeats();
    for (int32_t instanceId : ids) {
        if (sGdxGamepadPorts.count(instanceId) != 0) {
            continue;
        }
        const std::string guid = gdxGamepadGuidString(instanceId);
        const auto seat = guid.empty() ? rememberedSeats.end() : rememberedSeats.find(guid);
        if (seat != rememberedSeats.end() && !gdxPortOwnedByAGamepad(seat->second)) {
            sGdxGamepadPorts[instanceId] = seat->second;
            continue;
        }
        for (uint8_t port = 0; port < MAXCONTROLLERS; port++) {
            if (!gdxPortOwnedByAGamepad(port)) {
                sGdxGamepadPorts[instanceId] = port;
                break;
            }
        }
        // More pads than ports: the extras stay unassigned and are ignored on every port below.
    }

    for (uint8_t port = 0; port < MAXCONTROLLERS; port++) {
        for (int32_t instanceId : ids) {
            const auto owner = sGdxGamepadPorts.find(instanceId);
            if (owner != sGdxGamepadPorts.end() && owner->second == port) {
                devices->UnignoreInstanceIdForPort(port, instanceId);
            } else {
                devices->IgnoreInstanceIdForPort(port, instanceId);
            }
        }

        // Port 0 always has bindings (ControlDeck::Init seeds them); ports 1..3 only get them here,
        // and only the first time a device lands on them.
        if (port == 0 || !gdxPortOwnedByAGamepad(port)) {
            continue;
        }
        auto controller = controlDeck->GetControllerByPort(port);
        if (controller != nullptr &&
            !controller->HasMappingsForPhysicalDeviceType(Ship::PhysicalDeviceType::SDLGamepad)) {
            controller->AddDefaultMappings(Ship::PhysicalDeviceType::SDLGamepad);
            gdx_port_logf("[input] port %d: seeded default gamepad mappings\n", port + 1);
        }
    }

    // Persist the seat memory as the union of what was remembered and what is seated now, so an
    // unplugged pad keeps its seat for the next boot. Written only when an assignment changed.
    {
        std::unordered_map<std::string, uint8_t> seats = rememberedSeats;
        bool changed = false;
        for (int32_t instanceId : ids) {
            const auto owner = sGdxGamepadPorts.find(instanceId);
            if (owner == sGdxGamepadPorts.end()) {
                continue;
            }
            const std::string guid = gdxGamepadGuidString(instanceId);
            if (guid.empty()) {
                continue;
            }
            const auto it = seats.find(guid);
            if (it == seats.end() || it->second != owner->second) {
                seats[guid] = owner->second;
                changed = true;
            }
        }
        if (changed) {
            std::vector<std::string> entries;
            entries.reserve(seats.size());
            for (const auto& [guid, port] : seats) {
                entries.push_back(guid + "=" + std::to_string(static_cast<int>(port)));
            }
            std::sort(entries.begin(), entries.end()); // stable order -> no write churn
            std::string raw;
            for (const auto& entry : entries) {
                if (!raw.empty()) {
                    raw += ';';
                }
                raw += entry;
            }
            CVarSetString("gEnhancements.Input.GamepadSeats", raw.c_str());
            CVarSave();
        }
    }

    gdx_port_logf("[input] %zu SDL gamepad(s) connected, %zu routed to their own port\n", ids.size(),
                  sGdxGamepadPorts.size());
}

// True when the ControlDeck can actually produce input for `port` this frame; a gamepad mapping
// only counts while a non-ignored pad is plugged in, so hot-unplug reads as a disconnect.
// Neither Ship::Controller::IsConnected() nor ControlDeck::GetControllerBits() can be used —
// both are declaration-only in libultraship and do not link.
static bool gdxPortHasLiveInput(const std::shared_ptr<Ship::ControlDeck>& controlDeck, uint8_t port) {
    auto controller = controlDeck->GetControllerByPort(port);
    if (controller == nullptr) {
        return false;
    }
    if (controller->HasMappingsForPhysicalDeviceType(Ship::PhysicalDeviceType::Keyboard) ||
        controller->HasMappingsForPhysicalDeviceType(Ship::PhysicalDeviceType::Mouse)) {
        return true;
    }
    if (!controller->HasMappingsForPhysicalDeviceType(Ship::PhysicalDeviceType::SDLGamepad)) {
        return false;
    }
    auto devices = controlDeck->GetConnectedPhysicalDeviceManager();
    return devices != nullptr && !devices->GetConnectedSDLGamepadsForPort(port).empty();
}

// ── PORT input read bridge: LUS ControlDeck -> decomp controller globals ─────────────────────────
// extern "C" shim for input_bridge.c: every port's resolved state as N64 buttons + stick
// (-80..80) + connected flag.
//
// ONE WriteToPad PER FRAME, ALL PORTS AT ONCE — not an optimisation. WriteToPad has per-call
// side effects that make per-port single reads incorrect: WriteToOSContPad decays the buffered
// mouse-wheel axis once per call, and ReadToOSContPad pushes one entry per call into the
// CVAR_SIMULATED_INPUT_LAG deque (cap 6) — four calls a frame would drain the wheel 4x too fast
// and flush the lag line. Read the deck once, demultiplex.
//
// SDL events were already pumped by Fast3dWindow::HandleEvents() this frame. Returns 1 on
// success, 0 if the ControlDeck is not up yet (outputs zeroed; caller degrades to zero input).
extern "C" int gdx_lus_read_pads(int capacity, unsigned short* outButtons, signed char* outStickX,
                                 signed char* outStickY, unsigned char* outConnected) {
    const int ports = (capacity < MAXCONTROLLERS) ? capacity : MAXCONTROLLERS;

    for (int i = 0; i < capacity; i++) {
        if (outButtons != nullptr) {
            outButtons[i] = 0;
        }
        if (outStickX != nullptr) {
            outStickX[i] = 0;
        }
        if (outStickY != nullptr) {
            outStickY[i] = 0;
        }
        if (outConnected != nullptr) {
            outConnected[i] = 0;
        }
    }

    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return 0;
    }
    auto controlDeck = ctx->GetControlDeck();
    if (controlDeck == nullptr || ports <= 0) {
        return 0;
    }

    gdxSyncGamepadPortRouting(controlDeck);

    // WriteToOSContPad OR-accumulates into pad->button and only overwrites a stick axis when the
    // incoming value is 0 — it never clears the buffer. So we MUST zero it every frame or buttons
    // would latch on. One entry per port (WriteToPad writes all MAXCONTROLLERS ports).
    OSContPad pads[MAXCONTROLLERS];
    std::memset(pads, 0, sizeof(pads));
    controlDeck->WriteToPad(pads);

    for (int i = 0; i < ports; i++) {
        if (outButtons != nullptr) {
            outButtons[i] = pads[i].button;
        }
        if (outStickX != nullptr) {
            outStickX[i] = pads[i].stick_x;
        }
        if (outStickY != nullptr) {
            outStickY[i] = pads[i].stick_y;
        }
        if (outConnected != nullptr) {
            outConnected[i] = gdxPortHasLiveInput(controlDeck, (uint8_t) i) ? 1 : 0;
        }
    }
    return 1;
}

// Deliberately no single-port variant: looping one over four ports would silently break the
// wheel buffer and input-lag deque described above.

// GDX_INPUT_SCRIPT QUIT hook. Reuses the exact window-close path: Window::Close() flips the
// backend's running flag, which the frame loop already polls. No separate flag/break path.
extern "C" void gdx_request_quit(void) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return; // called before the window exists (unexpected for a script QUIT); nothing to close.
    }
    auto w = ctx->GetWindow();
    if (w != nullptr) {
        w->Close();
    }
}

static void addArchiveCandidateRoots(std::vector<std::filesystem::path>& roots, std::filesystem::path start) {
    std::error_code ec;
    if (start.empty()) {
        return;
    }

    start = std::filesystem::absolute(start, ec);
    if (ec) {
        ec.clear();
    }
    if (std::filesystem::is_regular_file(start, ec)) {
        start = start.parent_path();
        ec.clear();
    }

    for (std::filesystem::path dir = start; !dir.empty(); dir = dir.parent_path()) {
        roots.push_back(dir);
        roots.push_back(dir / "assets" / "extracted");

        if (dir == dir.root_path()) {
            break;
        }
    }
}

// A pack is any .o2r, matched case-insensitively. Shared by the mount scan and the diagnostics
// beside it so the two can never disagree about what counts as a pack.
static bool gdxIsO2rFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".o2r";
}

static std::vector<std::string> findArchivePaths(const char* argv0) {
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    addArchiveCandidateRoots(roots, std::filesystem::current_path(ec));
    if (argv0 != nullptr) {
        addArchiveCandidateRoots(roots, argv0);
    }

    std::vector<std::string> archives;
    // fzerox.o2r is the game archive; generic.o2r (Torch's default output name, in-tree dev
    // archive) is a fallback only when it is absent. Never mount both: same resource keys, one
    // would shadow the other. n64ddipl.o2r / fzerox-disk.o2r carry the IPL font block and the EK
    // disk image so the raw source files become deletable after setup; both unversioned and
    // tolerated-absent (their loaders fall back to the raw files).
    for (const auto& nameGroup : { std::vector<const char*>{ "gdiffuser.o2r", "f3d.o2r" },
                                   std::vector<const char*>{ "fzerox.o2r", "generic.o2r" },
                                   std::vector<const char*>{ "n64ddipl.o2r" },
                                   std::vector<const char*>{ "fzerox-disk.o2r" } }) {
        bool found = false;
        for (const char* name : nameGroup) {
            for (const auto& root : roots) {
                const auto candidate = root / name;
                if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                    archives.push_back(std::filesystem::absolute(candidate, ec).string());
                    found = true;
                    break;
                }
                ec.clear();
            }
            if (found) {
                break;
            }
        }
    }

    if (archives.empty()) {
        archives.push_back("gdiffuser.o2r");
        archives.push_back("fzerox.o2r");
    }

    // Append mods/*.o2r after the base archives. ArchiveManager is last-wins, so load order is
    // priority order. Order and the disable filter come from GdxWorkshopOrderModPaths, which
    // applies the same PackOrder list and dual-key (id/basename) disable match as the Workshop
    // menu's hot reload — boot and reload mount the same set in the same order. First root with
    // a mods/ dir wins, like the base archives.
    //
    // W3: the selected ROM-hack archive from mods/~romhacks/, mounted BEFORE the texture packs so
    // a cosmetic pack still overrides a hack's assets rather than the other way round. Scanned
    // independently of the Workshop master switch, because a hack is game content and its
    // presence must not depend on a texture-pack setting. At most one can be selected
    // (gdx_hackmods.h), so there is no conflict to resolve here, and the choice is latched now:
    // the save file is derived from it and must not move mid-session.
    for (const auto& root : roots) {
        const auto hackDir = root / "mods" / GDX_HACKMODS_DIR;
        if (!std::filesystem::is_directory(hackDir, ec)) {
            ec.clear();
            continue;
        }
        GdxHackModsSetDirectory(std::filesystem::absolute(hackDir, ec).string());
        break; // first root with a hack dir wins, like the base archives
    }
    {
        const std::string hackPath = GdxHackModsLatchActivePath();
        if (!hackPath.empty()) {
            archives.push_back(hackPath);
        }
    }

    const bool packsEnabled = CVarGetInteger("gEnhancements.Workshop.TexturePacks", 0) != 0;

    // Diagnostics for the three ways a correctly built pack can silently do nothing: the master
    // switch being off, the pack sitting one folder too deep, or it living under a root this scan
    // never reaches. All three used to produce NO output at all, so a player could not tell "no
    // packs found" from "packs found and ignored" -- the shape behind repeated reports of packs not
    // loading. This probe changes no behaviour; it only makes the silence audible.
    {
        bool describedWinner = false;
        for (const auto& root : roots) {
            const auto modsDir = root / "mods";
            if (!std::filesystem::is_directory(modsDir, ec)) {
                ec.clear();
                continue;
            }
            ec.clear();

            if (describedWinner) {
                // Mirrors the `break` in the scan below: only the first mods folder is ever read.
                gdx_port_logf("[workshop] note: %s also exists, but only the first mods folder is "
                              "scanned, so packs there are ignored.\n",
                              modsDir.string().c_str());
                continue;
            }
            describedWinner = true;

            int topLevel = 0;
            int nested = 0;
            for (const auto& entry : std::filesystem::directory_iterator(modsDir, ec)) {
                if (entry.is_regular_file(ec)) {
                    if (gdxIsO2rFile(entry.path())) {
                        topLevel++;
                    }
                    continue;
                }
                if (!entry.is_directory(ec)) {
                    continue;
                }
                // ~romhacks is not a misplaced pack: it is the ROM-hack folder, mounted by its own
                // scan above. Counting it here would tell the player to move files that are already
                // exactly where they belong.
                if (entry.path().filename().string() == GDX_HACKMODS_DIR) {
                    continue;
                }
                // One level down is where an unzipped pack usually lands.
                for (const auto& sub : std::filesystem::directory_iterator(entry.path(), ec)) {
                    if (sub.is_regular_file(ec) && gdxIsO2rFile(sub.path())) {
                        nested++;
                    }
                }
                ec.clear();
            }
            ec.clear();

            if (nested > 0) {
                gdx_port_logf("[workshop] %d .o2r file(s) sit in SUBFOLDERS of %s and are not scanned. "
                              "Move them directly into that folder.\n",
                              nested, modsDir.string().c_str());
            }
            if (!packsEnabled) {
                if (topLevel + nested > 0) {
                    gdx_port_logf("[workshop] %d pack(s) present in %s, but the Workshop \"Texture "
                                  "packs\" switch is OFF, so none were mounted.\n",
                                  topLevel + nested, modsDir.string().c_str());
                }
            } else if (topLevel == 0) {
                gdx_port_logf("[workshop] no .o2r packs directly in %s.\n", modsDir.string().c_str());
            }
        }
    }

    // The Workshop "Texture packs" master switch gates the WHOLE scan (M-A, owner decision M1):
    // with it off, no pack mounts at all, so the direct-key shadowing channel (a pack entry at a
    // base resource's own key) is off too — previously only the textures/pack/ lookup consulted
    // the switch and direct-key packs kept applying. InitConsoleVariables has already run by the
    // time findArchivePaths is called, so the persisted value is live here.
    if (packsEnabled) {
    for (const auto& root : roots) {
        const auto modsDir = root / "mods";
        if (!std::filesystem::is_directory(modsDir, ec)) {
            ec.clear();
            continue;
        }
        std::vector<std::string> modPaths;
        for (const auto& entry : std::filesystem::directory_iterator(modsDir, ec)) {
            if (!entry.is_regular_file(ec) || !gdxIsO2rFile(entry.path())) {
                continue;
            }
            modPaths.push_back(std::filesystem::absolute(entry.path(), ec).string());
        }
        std::vector<std::string> disabled;
        for (const std::string& path : GdxWorkshopOrderModPaths(modPaths, &disabled)) {
            archives.push_back(path);
            gdx_port_logf("[workshop] pack: %s\n",
                          std::filesystem::path(path).filename().string().c_str());
        }
        for (const std::string& basename : disabled) {
            gdx_port_logf("[workshop] pack: %s (disabled)\n", basename.c_str());
        }
        break; // first root with a mods/ dir wins, like the base archives
    }
    } // master-switch gate

    return archives;
}

int main(int argc, char** argv) {
    // Must precede every log call this process makes. Precedence rules in port/gdx_dev_gates.h.
    gdx_dev_gates_init_env();

    // Must run before any libultraship path resolution, so config/logs/disk/IPL consolidate into
    // the resolved data dir.
    gdx::FirstBootResult firstBoot = gdx::FirstBootRun((argc > 0) ? argv[0] : nullptr);
    if (firstBoot.status == gdx::FirstBootStatus::Aborted) {
        return 1;
    }

    // The ROM Hacks window builds hack archives with the packaged extractor, which ships beside the
    // executable and not in the data dir. Recorded once here, where the resolved layout is known.
    GdxHackModsSetProgramDir(firstBoot.exeDir);

    // Must run BEFORE findArchivePaths builds the mount list: the data dir is a candidate root, so
    // a freshly extracted archive is picked up this same boot. Skipped on a development tree,
    // where re-extraction would clobber the developer's working generic.o2r. Never blocks boot —
    // on any failure the raw-ROM fallback carries the session.
    if (firstBoot.status != gdx::FirstBootStatus::NeedsSetup) {
        std::error_code cwdEc;
        const bool devTreeArchive = gdx::DevelopmentTreeProvidesArchive(
            firstBoot.exeDir, std::filesystem::current_path(cwdEc).string());
        if (devTreeArchive) {
            gdx_port_logf("[G-Diffuser] asset extraction: skipped (dev tree provides "
                          "assets/extracted/generic.o2r)\n");
        } else {
            gdx::ExtractOutcome extractOutcome = gdx::GdxExtractEnsureArchive(
                firstBoot.dataDir.c_str(), firstBoot.romPath.c_str(), firstBoot.exeDir.c_str());
            gdx_port_logf("[G-Diffuser] asset extraction: %s\n",
                          gdx::GdxExtractOutcomeString(extractOutcome));
        }
    }

    logStep("CreateUninitializedInstance");
    auto ctx = Ship::Context::CreateUninitializedInstance("G-Diffuser", "gdiffuser",
                                                          "gdiffuser.cfg.json");
    if (ctx == nullptr) { logStep("FATAL: CreateUninitializedInstance"); return 1; }

    logStep("InitLogging");
    if (gdx_log_file_enabled()) {
        ctx->InitLogging();
    } else {
        ctx->InitLogging(spdlog::level::off, spdlog::level::off, false);
    }
    // Buffers here, long before the console window exists, so the console is not blank on first
    // open.
    GdxConsoleLogInstall();
    logStep("InitConfiguration");    ctx->InitConfiguration();
    logStep("InitConsoleVariables"); ctx->InitConsoleVariables();

    // Earliest point the config is readable, and still ahead of essentially all boot logging —
    // that is what makes a Dev Tools toggle capture the NEXT boot. libultraship's spdlog sinks were
    // already configured above (InitLogging precedes InitConfiguration), so for THIS run their
    // level follows GDX_LOG only; the port's run-log sink opens lazily and honours the CVar.
    gdx_dev_gates_boot_seed(&GdxGateCVarGet);

    // SDL classifies already-connected joysticks while initializing this subsystem, so external
    // mappings must be available before the first init rather than waiting for the later N64 SI
    // osContInit path.
    loadControllerDatabase();
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        gdx_port_logf("[input] SDL_InitSubSystem(GAMECONTROLLER) FAILED: %s\n", SDL_GetError());
    } else {
        gdx_port_logf("[input] SDL gamecontroller subsystem up; %d joystick(s) present at boot\n",
                      SDL_NumJoysticks());
        logConnectedJoysticks();
    }
    logStep("construct ControlDeck"); auto controlDeck = std::make_shared<LUS::ControlDeck>();
    logStep("InitControlDeck");       ctx->InitControlDeck(controlDeck);

    // Context::InitControlDeck() does NOT call ControlDeck::Init() — do it explicitly, or the
    // controllers have no mappings and read all-zero. Config + CVars exist, so it is safe now.
    logStep("ControlDeck::Init");     controlDeck->Init(&sGdxControllerBits);

    // ResourceManager MUST init before the window is constructed/inited.
    auto archivePaths = findArchivePaths((argc > 0) ? argv[0] : nullptr);
    for (const auto& archivePath : archivePaths) {
        gdx_port_logf("[G-Diffuser] archive: %s\n", archivePath.c_str());
    }
    // libultraship's built-in version gate (validHashes) is reject-only and does NOT special-case
    // unversioned archives, so a non-empty set would refuse to mount f3d.o2r and the texture packs
    // (no "version" file) and break boot. It therefore stays DISABLED and the post-mount check
    // below enforces two policies instead:
    //   * fzerox.o2r/generic.o2r is ENFORCING (version = US-rev0 ROM CRC). The SETTIMG OTR rewrite
    //     path is not partial-resilient, so a mismatched archive renders blank textures with no
    //     recovery; on mismatch it is UNMOUNTED and the raw-ROM fallback carries the session.
    //   * the mounted ROM-hack archive is ENFORCING under the SAME rule: it is built from the
    //     hack ROM with the BASE recipes and stamped with the base ROM CRC (gdx-extract is
    //     given --version-crc for exactly this), so it is a game archive that merely carries a
    //     different filename. Judging it by the schema version below is what made every
    //     freshly built hack pop a spurious "stale or incompatible" warning.
    //   * every other archive is WARN-ONLY; unversioned archives pass through untouched.
    static constexpr uint32_t kGdxExpectedArchiveVersion = 1u;        // schema v1 = first versioned O2R
    static constexpr uint32_t kGdxExpectedGenericRomCrc = 0x78D90EB3u; // US-rev0 ROM CRC stamp
    logStep("InitResourceManager");   ctx->InitResourceManager(archivePaths, {}, 1);

    // No-ROM boot predicate: true iff the game archive is mounted AND survives the CRC gate.
    // Threaded into gdx_init_rom so a missing ROM is tolerated when the archive is correct.
    bool archivesValidated = false;
    {
        auto rm = ctx->GetResourceManager();
        auto am = (rm != nullptr) ? rm->GetArchiveManager() : nullptr;
        auto archives = (am != nullptr) ? am->GetArchives() : nullptr;
        // Collect generic.o2r paths to unmount AFTER the scan: RemoveArchive rebuilds the VFS and
        // mutates the internal archive list, so we must not remove while iterating the snapshot.
        std::vector<std::string> toUnmount;
        if (archives != nullptr) {
            for (const auto& archive : *archives) {
                if (archive == nullptr || !archive->HasGameVersion()) {
                    continue; // unversioned archive (f3d.o2r, texture packs) — nothing to validate
                }
                const std::string path = archive->GetPath();
                std::string basename = std::filesystem::path(path).filename().string();
                for (char& c : basename) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                const uint32_t got = archive->GetGameVersion();

                // The latched hack mounts as "<name>.o2r" beside the base archive and is a game
                // archive in every way that matters here, so it is classified with one.
                std::string hackBasename = GdxHackModsActiveName();
                if (!hackBasename.empty()) {
                    for (char& c : hackBasename) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    hackBasename += ".o2r";
                }
                const bool isHackArchive = !hackBasename.empty() && (basename == hackBasename);

                if (basename == "fzerox.o2r" || basename == "generic.o2r" || isHackArchive) {
                    // ENFORCING: version ROM-CRC must match US rev0.
                    if (got != kGdxExpectedGenericRomCrc) {
                        // Unmounting a hack drops the session back to the STOCK GAME, not to the raw
                        // ROM, and the fix is a rebuild rather than a delete. Same gate, different
                        // consequence, so the wording follows the case.
                        gdx_port_logf(
                            "[G-Diffuser] ERROR: %s \"%s\" version ROM-CRC is 0x%08X but this build "
                            "expects 0x%08X (US rev0). Unmounting it; %s.\n",
                            isHackArchive ? "ROM-hack archive" : "game archive", path.c_str(), got,
                            kGdxExpectedGenericRomCrc,
                            isHackArchive ? "the stock game runs instead; rebuild the hack to fix it"
                                          : "booting from the raw ROM; delete it (or the "
                                            "gdx_extract_state.cfg sidecar) to force a fresh extraction");
#ifdef _WIN32
                        char msg[512];
                        if (isHackArchive) {
                            snprintf(msg, sizeof(msg),
                                     "This ROM-hack archive was built for a different game "
                                     "revision:\n\n%s\n\nversion ROM-CRC 0x%08X, expected 0x%08X (US "
                                     "rev0).\n\nG-Diffuser will run the stock game. Rebuild the archive "
                                     "from its ROM under Workshop > ROM Hacks.",
                                     path.c_str(), got, kGdxExpectedGenericRomCrc);
                        } else {
                            snprintf(msg, sizeof(msg),
                                     "The extracted asset archive does not match your ROM:\n\n%s\n\nversion "
                                     "ROM-CRC 0x%08X, expected 0x%08X (US rev0).\n\nG-Diffuser will boot "
                                     "from the raw ROM. Delete this file to force a fresh extraction.",
                                     path.c_str(), got, kGdxExpectedGenericRomCrc);
                        }
                        MessageBoxA(nullptr, msg,
                                    isHackArchive ? "G-Diffuser — incompatible ROM-hack archive"
                                                  : "G-Diffuser — incompatible generic.o2r",
                                    MB_OK | MB_ICONWARNING);
#endif
                        toUnmount.push_back(path);
                    } else if (!isHackArchive) {
                        // Only the BASE archive satisfies the no-ROM boot gate. A hack replaces
                        // content on top of it and can never stand in for it.
                        archivesValidated = true;
                    }
                } else if (got != kGdxExpectedArchiveVersion) {
                    // WARN-ONLY for every other versioned archive.
                    gdx_port_logf(
                        "[G-Diffuser] ERROR: archive \"%s\" reports version %u but this build "
                        "expects %u. The archive is stale or incompatible. Regenerate it with "
                        "tools/gen_f3d_o2r.py (f3d.o2r) / tools/gen_texture_pack.py, then relaunch.\n",
                        path.c_str(), got, kGdxExpectedArchiveVersion);
#ifdef _WIN32
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                             "Asset archive is stale or incompatible:\n\n%s\n\nreports version %u "
                             "but this build expects %u.\n\nRegenerate it with tools/gen_f3d_o2r.py "
                             "(f3d.o2r) / tools/gen_texture_pack.py, then relaunch.",
                             path.c_str(), got, kGdxExpectedArchiveVersion);
                    MessageBoxA(nullptr, msg, "G-Diffuser — incompatible asset archive",
                                MB_OK | MB_ICONWARNING);
#endif
                    // Warn-and-continue: libultraship already mounted the archive (empty gate),
                    // so we let it run rather than hard-aborting a possibly-still-usable build.
                }
            }
        }
        // Must happen before any resource is read (GDiffuser_LoadAllAssets runs later), so a
        // mismatched archive leaves a clean archive-absent state rather than a partial one.
        if (am != nullptr) {
            for (const auto& path : toUnmount) {
                am->RemoveArchive(path);
                gdx_port_logf("[G-Diffuser] unmounted incompatible archive: %s\n", path.c_str());
            }
        }
    }

    // Console must exist BEFORE the Gui is built: the Gui adds a ConsoleWindow whose Init()
    // (called by AddGuiWindow) registers commands via Context::GetConsole().
    logStep("InitCrashHandler");      ctx->InitCrashHandler();
    logStep("InitConsole");           ctx->InitConsole();
    ctx->GetConsole()->AddCommand(
        "reset",
        { [](std::shared_ptr<Ship::Console>, std::vector<std::string>, std::string* output) {
             gdx_game_request_reset();
             if (output != nullptr) {
                 *output = "Game reset requested.";
             }
             return 0;
         },
          "Reset the game to the title screen.", {} });

    logStep("construct GdxFast3dGui");
    auto gui = std::make_shared<GdxFast3dGui>(std::vector<std::shared_ptr<Ship::GuiWindow>>());
    logStep("construct Fast3dWindow(gui)");
    auto window = std::make_shared<Fast::Fast3dWindow>(gui);
    logStep("InitWindow");            ctx->InitWindow(window);

    // The LUS dev/input windows the Gui ctor does NOT auto-add, then the full-screen menu
    // (libultraship already wires the F1/Esc/Back toggle).
    logStep("register enhancement menu + dev/input windows");
    {
        auto pgui = ctx->GetWindow()->GetGui();

        // CVar names are string literals: the libultraship CVAR_* macros are not in scope for the
        // port target (cvars.cmake is include()d only inside libultraship/src).
        pgui->AddGuiWindow(std::make_shared<LUS::GfxDebuggerWindow>("gGfxDebuggerEnabled", "Gfx Debugger"));
        pgui->AddGuiWindow(
            std::make_shared<LUS::InputEditorWindow>("gControllerConfigurationEnabled", "Input Editor"));
        pgui->AddGuiWindow(
            std::make_shared<GdxGhostWindow>("gEnhancements.Practice.GhostBrowserOpen", "Ghost Browser"));
        // The F12 unified Export/Import shell; v1 hosts E1 .gdxc track/machine export.
        pgui->AddGuiWindow(
            std::make_shared<GdxContentWindow>("gEnhancements.Content.LibraryOpen", "Content Library"));
        pgui->AddGuiWindow(std::make_shared<GdxHackModsWindow>("gEnhancements.Hacks.WindowOpen", "ROM Hacks"));
        pgui->AddGuiWindow(std::make_shared<GdxInputViewer>());
        pgui->AddGuiWindow(std::make_shared<GdxFpsOverlay>());

        // Default OFF on all platforms: the backend vsync caps correctly and the port pacer
        // misbehaves when on (owner evidence, ROG Ally X/Linux). Must run BEFORE the GdxMenu ctor
        // registers the CVar — RegisterInteger only sets a value when the CVar does not exist, so
        // a persisted user toggle still wins.
        CVarRegisterInteger("gEnhancements.Graphics.FramePacing", 0);

        // Interpolation and FramePacing are mutually-exclusive pacing owners; the pacer no-ops
        // itself while this is on (gdx_frame_pacer.c).
        CVarRegisterInteger("gEnhancements.Graphics.FrameInterpolation", 0);
        // An older build force-persisted FramePacing:1 into Linux configs, which RegisterInteger
        // cannot override. Reset to the OFF default exactly once; a later manual re-enable
        // persists normally. The marker key is fresh so configs the previous migration touched
        // still get it.
        if (CVarGetInteger("gdx.Migrations.FramePacingDefaultOff", 0) == 0) {
            CVarSetInteger("gdx.Migrations.FramePacingDefaultOff", 1);
            CVarSetInteger("gEnhancements.Graphics.FramePacing", 0);
            CVarSave();
            gdx_port_logf("[G-Diffuser] Frame pacer reset to OFF (new default). "
                          "Re-enable it in Settings if the game runs too fast.\n");
        }

        // From here on the CVars, not the env vars that seeded them, are the source of truth for
        // gate buckets A/B; gdx_dev_gates_refresh() re-reads once per frame.
        gdx_dev_gates_bind_cvars(&GdxGateCVarGet, &GdxGateCVarSet);

        // SetMenu calls Init(), where the GdxMenu ctor registers the port's gEnhancements.* CVars
        // at their 1:1 defaults.
        pgui->SetMenu(std::make_shared<GdxMenu>());
    }

    logStep("InitAudio");
    {
        // 4096 frames (~128ms) is large enough for the audio thread's catch-up loop to ride out
        // host scheduling jitter. Read ONCE at InitAudio, which is why the menu labels the setting
        // "(applies on restart)". The menu clamps too; this guards a hand-edited config.
        Ship::AudioSettings audioSettings{};
        int bufferFrames = CVarGetInteger("gEnhancements.Audio.BufferFrames", 4096);
        if (bufferFrames < 1024) {
            bufferFrames = 1024;
        } else if (bufferFrames > 8192) {
            bufferFrames = 8192;
        }
        audioSettings.DesiredBuffered = bufferFrames;
        ctx->InitAudio(audioSettings);

        // 0 = Auto (libultraship's per-platform default), 1 = WASAPI, 2 = SDL. An override applies
        // only if that backend exists here, so picking WASAPI on Linux is a no-op, not a failure.
        CVarRegisterInteger("gEnhancements.Audio.Backend", 0);
        int backendSel = CVarGetInteger("gEnhancements.Audio.Backend", 0);
        if (backendSel != 0) {
            auto audio = ctx->GetAudio();
            if (audio != nullptr) {
                Ship::AudioBackend want = Ship::AudioBackend::SDL;
                if (backendSel == 1) {
                    want = Ship::AudioBackend::WASAPI;
                } else if (backendSel == 2) {
                    want = Ship::AudioBackend::SDL;
                }
                auto avail = audio->GetAvailableAudioBackends();
                bool available =
                    avail != nullptr && std::find(avail->begin(), avail->end(), want) != avail->end();
                if (available && audio->GetCurrentAudioBackend() != want) {
                    audio->SetCurrentAudioBackend(want);
                }
            }
        }
    }
    logStep("InitEventSystem");       ctx->InitEventSystem();
    logStep("InitFileDropMgr");       ctx->InitFileDropMgr();

    // ── In-window first-time setup (NeedsSetup path) ─────────────────────────────────────────────
    // FirstBootRun defers acquisition to here when the ROM/EK disk/IPL are missing: this is the
    // first point where the window, Gui and FileDropMgr exist while the game has NOT booted —
    // RegisterResourceFactories, LoadAllAssets and bootproc all require the ROM. Closing the
    // window during setup exits cleanly; partial files persist and the next launch resumes.
    if (firstBoot.status == gdx::FirstBootStatus::NeedsSetup) {
        std::string setupRomPath;
        if (!gdx::GdxFirstBootSetupRun(firstBoot.dataDir, firstBoot.exeDir, setupRomPath)) {
            // The dedicated audio thread has not started yet (gdx_audio_thread_start is below), so a
            // plain return is a clean exit here.
            gdx_port_logf("[G-Diffuser] first-time setup was closed before completion; exiting.\n");
            return 0;
        }
        firstBoot.romPath = setupRomPath;
        firstBoot.status = gdx::FirstBootStatus::SetupComplete;
    }

    // Safe this early: the thread internally waits for gAudioContextInitialized (set once decomp's
    // Audio_Init runs, well after bootproc() below) before producing anything.
    logStep("gdx_audio_thread_start()");
    gdx_audio_thread_start(argc, argv);

    logStep("RegisterResourceFactories");
    GDiffuser::RegisterResourceFactories(ctx->GetResourceManager()->GetResourceLoader());

    logStep("GDiffuser_LoadAllAssets");
    GDiffuser_LoadAllAssets();

    logStep("gdx_sched_init() — cooperative fiber scheduler");
    gdx_sched_init();

    logStep("gdx_init_rom() — load ROM asset buffer");
    {
        // The first-boot ROM path goes in as a TRAILING synthetic argv entry: rom_buffer.cpp scans
        // args in order and returns on the first that loads, so a real command-line ROM still wins
        // and this is only reached as the fallback. Taking the CLI-arg branch also returns before
        // rom_buffer's interactive picker, keeping launch headless once setup is complete.
        std::vector<char*> romArgv(argv, argv + argc);
        if (!firstBoot.romPath.empty()) {
            romArgv.push_back(const_cast<char*>(firstBoot.romPath.c_str()));
        }
        gdx_init_rom(static_cast<int>(romArgv.size()), romArgv.data(), archivesValidated ? 1 : 0);
    }

    // The real no-ROM boot gate. Under archivesValidated, gdx_init_rom returns success with a null
    // buffer rather than exit(1)-ing, so archive-only boot arrives here. Without it gdx_init_rom
    // should already have terminated the process, making the else branch defense-in-depth.
    if (gdx_rom_buffer == nullptr) {
        if (archivesValidated) {
            gdx_port_logf("[G-Diffuser] no ROM loaded; continuing archive-only boot (fzerox.o2r "
                          "validated). Any raw-ROM fallback read will be logged/zero-filled.\n");
        } else {
            gdx_port_logf("[G-Diffuser] FATAL: no ROM loaded and no validated archive — cannot start game.\n");
            gdx_port_logf("[G-Diffuser] Place baserom.us.rev0.z64 next to the exe, pass it as an argument, "
                          "or complete first-boot setup so a validated fzerox.o2r archive is installed.\n");
#ifdef _WIN32
            MessageBoxA(nullptr,
                "No F-Zero X ROM was loaded, and no validated asset archive was found either.\n\n"
                "Place 'baserom.us.rev0.z64' next to G-Diffuser.exe,\n"
                "pass the ROM path as a command-line argument,\n"
                "or complete first-boot setup so a validated fzerox.o2r archive is installed.",
                "G-Diffuser — ROM required", MB_OK | MB_ICONERROR);
#endif
            return 1;
        }
    }

    logStep("gdx_rdram_init() — allocate 16MB RDRAM host buffer");
    gdx_rdram_init();

    // Register ROM buffer so TryResolveAddress can resolve low32(rom_ptr) addresses
    // (cockpit overlay textures compiled into EXE BSS at N64 addresses, accessed via
    // osVirtualToPhysical which truncates to low32 for non-RDRAM pointers).
    if (gdx_rom_buffer != nullptr) {
        gdx_register_host_range(gdx_rom_buffer, gdx_rom_size);
        gdx_port_logf("[rom] registered ROM buffer: base=%p low32=%08X size=0x%zx\n",
                      static_cast<void*>(gdx_rom_buffer),
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(gdx_rom_buffer) & 0xFFFFFFFFu),
                      gdx_rom_size);
    }
    gdx_register_main_module_range();

    // Must sit AFTER gdx_rdram_init and BEFORE bootproc, in the same window gdx_rom_buffer is
    // registered: the payloads have to be resident before the first audio DMA, and each one is
    // registered as a host range so truncated-low32 tokens of blob-served audio buffers resolve
    // through the marshaller exactly as gAudioHeap and gdx_rom_buffer do. An archive lacking the
    // entries degrades silently — preload returns 0 and the DMA sink falls back to raw ROM.
    //
    // Bases are the PORT_audio_{bank,seq,table}_ROM_START values from
    // decomp/include/port_segment_addrs.h, duplicated by value because this TU deliberately stays
    // out of the decomp include tree. The shared blob table is keyed on these exact bases.
    {
        static const uint32_t kAudioBlobBases[3] = {
            0x00524D60u, // PORT_audio_bank_ROM_START  (audio_blob/audio_bank)
            0x00527AF0u, // PORT_audio_seq_ROM_START   (audio_blob/audio_seq)
            0x00528730u, // PORT_audio_table_ROM_START (audio_blob/audio_table)
        };
        for (int i = 0; i < 3; ++i) {
            const uint32_t base = kAudioBlobBases[i];
            if (GdxSegmentSourcePreload(base)) {
                void* payload = nullptr;
                uint32_t payloadSize = 0;
                if (GdxSegmentSourcePayload(base, &payload, &payloadSize) && payload != nullptr) {
                    gdx_register_host_range(payload, payloadSize);
                    gdx_port_logf("[audio-blob] preloaded+registered base=%08X payload=%p low32=%08X size=0x%X\n",
                                  base, payload,
                                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(payload) & 0xFFFFFFFFu),
                                  payloadSize);
                }
            } else {
                gdx_port_logf("[audio-blob] base=%08X not resident (archive lacks entry) — raw-ROM fallback\n",
                              base);
            }
        }
    }

    // Venue texture banks, same boot window, but for frame pacing rather than delivery. Entering
    // Cup Select loads six banks ONE PER GAME TICK (course_model.c:35-39 walks cupType*6 + n across
    // consecutive ticks) at 8.8-26.4 ms each against a 16.68 ms budget, dropping the sim to ~11 Hz.
    // The cost is the archive read, not the ++gConvertEpoch invalidation each load performs: with
    // the [venueload] probe, translation cost stayed flat at 0.12-0.16 ms across the eight ticks
    // after each load while the epoch climbed 646->652. So warming the read/alloc/copy at boot is
    // the fix and scoped epoch invalidation would have been wasted work. MIO0 decode and endian
    // fixups still happen lazily, because they write per-segment state the game must own.
    //
    // Bases are the ROM offsets encoded in the venue symbol names gdx_load_venue_texture_segment
    // uses (D_A000000_235130 -> 0x235130). Absence degrades silently as the audio blobs do.
    {
        static const uint32_t kVenueBlobBases[11] = {
            0x00235130u, // Mute City
            0x00239A80u, // Port Town
            0x0023EC50u, // Big Blue
            0x00243D90u, // Sand Ocean
            0x0024A270u, // Devil's Forest
            0x002507F0u, // White Land
            0x00255100u, // Sector
            0x00259600u, // Red Canyon
            0x0025F360u, // Fire Field
            0x00266C20u, // Silence
            0x0026D780u, // Ending
        };
        int warmed = 0;
        const double venueT0 = gdx_host_now_seconds();
        for (uint32_t base : kVenueBlobBases) {
            if (GdxSegmentSourcePreload(base)) {
                ++warmed;
            }
        }
        gdx_port_logf("[venue-blob] preloaded %d/11 venue texture banks in %.1fms\n", warmed,
                      (gdx_host_now_seconds() - venueT0) * 1000.0);
    }

    // Decoding, not just fetching, the expensive segments. The blob preload above alone fixed
    // nothing measurable: the stalls (133.95ms for course_track_gfx in one hit, 6-26ms per venue
    // bank) came from running the load on the GAME FIBER, where the load path yields back to the
    // host mid-work. The same 12 segments take 2.2ms here on the host thread, so the runtime cost
    // was yield round-trips, not decode CPU. Decoded images never evict, so every runtime load
    // becomes a cache hit. The function snapshots and restores gSegments so boot leaves no segment
    // bound that the game did not bind itself.
    gdx_boot_warm_asset_segments();

    // PCM-parity capture: arm before the audio thread starts producing ticks, so the
    // capture window and its deterministic-RNG gate are live on the first audio tick. No-op unless
    // GDX_PCM_CAPTURE is set — zero behavior change for normal play.
    gdx_pcm_capture_init();

    logStep("bootproc() — starting the decomp game threads");
    bootproc();
    logStep("bootproc() returned; game threads running");

    // Gives the gfx bridge a monotonic clock so its sub-frame loop can derive t. Inert unless
    // FrameInterpolation is on.
    gdx_gfx_interp_set_now_fn(&gdx_host_now_seconds);

    logStep("entering frame loop");
    auto w = ctx->GetWindow();
    while (w != nullptr && w->IsRunning()) {
        // The one per-frame refresh point for every developer gate: one CVar read per gate here
        // means no hot path (per draw call, per display-list command, per audio frame) ever hashes
        // a CVar name, and everything downstream this frame sees a consistent snapshot. The Dev
        // Tools menu also calls this right after a toggle so a click applies on the same frame.
        gdx_dev_gates_refresh();
        gdx::PerfFrameBegin();
        gdx::PerfPhaseBegin(gdx::PerfEvents);
        // Must run every frame to drain the SDL event queue; without it click/close events pile up
        // and the window manager crashes.
        w->HandleEvents();
        gdx::PerfPhaseEnd(gdx::PerfEvents);
        gdx::PerfPhaseBegin(gdx::PerfInput);
        gdx_controller_poll();
        // Must publish the editor fixed-aspect state before this frame's gfx task runs below:
        // Course Edit / Create Machine render through the stock 4:3 pillarbox path.
        gdx_fixed_aspect_tick();
        gdx::PerfPhaseEnd(gdx::PerfInput);

        // The sub-frame schedule must be configured BEFORE gdx_vi_tick: the game's gfx-task
        // submission (gdx_gfx_run) executes inside gdx_vi_tick's synchronous fiber dispatch, so the
        // schedule has to be live before any gfx work runs. Inert unless FrameInterpolation is on.
        //
        // The editor gate is deliberately narrow — a DIVIDED VI clock (D_800CCFBC > 1, Course
        // Edit's ~20 Hz cursor mode), not every fixed-aspect tick. Do NOT widen it back: gGameMode
        // stays at the editor mode through Course Edit TEST RUNS and Create Machine's rotatable 3D
        // preview, so a blanket gate leaves a player driving the full race pipeline at 60 Hz inside
        // a 144 Hz window. The per-sub-frame mRendersToFb/aspect recompute that motivated the wider
        // gate is not a problem: it already runs M times per tick in every interpolated mode, is
        // idempotent intra-tick, and both backends diff-guard UpdateFramebufferParameters.
        //
        // Cursor mode stays excluded on purpose: at ~20 Hz the tween sweeps each 50 ms step inside
        // the first 16.7 ms and then holds, which looks no better than the clean steps it replaces.
        // Divider flips are cut-covered on both edges — Racer_Init on entry, the PORT-gated cut in
        // func_xk2_800EC91C (19DD60.c) on exit.
        const bool interpEditorActive = (gdx_get_force_fixed_aspect() != 0) && (gdx_vi_divider() > 1);
        const bool interpOn = (gdx_gfx_interp_host_active() != 0) && !interpEditorActive;
        const double interpTickStart = gdx_host_now_seconds();
        // This tick's sub-frame count, derived from the two live target-fps CVars:
        //   gEnhancements.Graphics.InterpTargetMode 0 -> Match Refresh Rate (target = the display's
        //   current refresh rate), 1 -> Capped (target = InterpTargetFps).
        // The target-frames falling inside one 60 Hz logic tick is fractional (143 Hz -> 143 *
        // 1.001/60 = 2.386). Rounding that up per tick does not fit the budget on a VSync-on 143 Hz
        // panel (3 blocking presents ~21 ms > 16.68 ms) and oscillates into an unstable framerate,
        // so a deterministic remainder accumulator carries the fraction across ticks instead: the
        // count alternates (2,2,3,...) and averages exactly target/60 while logic stays at 60 Hz.
        // Only computed while interpOn; the default path ignores maxSubframes entirely.
        int interpMaxSubframes = kGdxInterpMaxSubframes;
        static double sInterpFrameAccum = 0.0;
        // gdx_os_display_refresh_hz is too expensive to run every logic tick, so it is re-resolved
        // at most once a second, or immediately when Match Refresh (re-)becomes active — that keeps
        // a monitor change or hotplug responsive without paying the cost per tick.
        static double sInterpOsHzLastResolve = -1000.0;
        static int sInterpOsHzCached = 0;
        static const char* sInterpOsHzPathCached = "unresolved";
        static bool sInterpMatchRefreshActive = false;
        if (interpOn) {
            const bool interpMatchRefresh = CVarGetInteger("gEnhancements.Graphics.InterpTargetMode", 0) == 0;
            int interpEffectiveTarget;
            if (interpMatchRefresh) {
                const uint32_t refreshRate = w->GetCurrentRefreshRate();
                int detected = (refreshRate > 0) ? static_cast<int>(refreshRate) : 0;
                // Cross-check the OS panel rate for the monitor the window is actually ON (see
                // gdx_os_display_refresh_hz): libultraship may under-report on some high-refresh
                // panels. Use the higher of the two so a mis-detected 60 Hz on a real 143 Hz panel
                // still drives interpolation, while a window sitting on a genuinely lower-Hz
                // monitor doesn't get pulled up to a higher-Hz secondary display's rate.
                if (!sInterpMatchRefreshActive || (interpTickStart - sInterpOsHzLastResolve) >= 1.0) {
                    sInterpOsHzCached = gdx_os_display_refresh_hz(w->GetPosX(), w->GetPosY(), w->GetWidth(),
                                                                   w->GetHeight(), &sInterpOsHzPathCached);
                    sInterpOsHzLastResolve = interpTickStart;
                }
                sInterpMatchRefreshActive = true;
                const int osHz = sInterpOsHzCached;
                const char* osHzPath = sInterpOsHzPathCached;
                if (osHz > detected) {
                    detected = osHz;
                }
                // Fallback to 120 if neither source can report a refresh rate. Floor a genuine
                // sub-60 report (50 Hz PAL sets, VM/RDP virtual displays, eco panel modes) at 60:
                // the sim ticks at 60 Hz and always presents at least once per tick, so a sub-60
                // target cannot be honored anyway — flooring keeps framesPerTick >= 1 by
                // construction instead of leaning on the count<1 guard below.
                interpEffectiveTarget = (detected > 0) ? detected : 120;
                if (interpEffectiveTarget < 60) {
                    interpEffectiveTarget = 60;
                }
                static bool sLoggedRefreshDiag = false;
                if (!sLoggedRefreshDiag) {
                    sLoggedRefreshDiag = true;
                    gdx_port_logf(
                        "[interp-diag] MatchRefresh: lus_refresh=%u os_refresh=%d (path=%s) -> target=%d\n",
                        static_cast<unsigned>(refreshRate), osHz, osHzPath, interpEffectiveTarget);
                }
            } else {
                sInterpMatchRefreshActive = false; // re-resolve immediately if Match Refresh re-enables later
                // Clamp to the UI's enforced range so a hand-edited config below 60 cannot reach
                // the `count < 1` debit guard below, whose debit-then-floor arithmetic cannot
                // actually hold presentation below one present per tick at such a value.
                interpEffectiveTarget = CVarGetInteger("gEnhancements.Graphics.InterpTargetFps", 120);
                if (interpEffectiveTarget < 60) {
                    interpEffectiveTarget = 60;
                }
                if (interpEffectiveTarget > 480) {
                    interpEffectiveTarget = 480;
                }
            }
            // target-frames per logic tick (fractional); accumulate the remainder deterministically.
            const double framesPerTick = static_cast<double>(interpEffectiveTarget) * kGdxInterpTickSeconds;
            sInterpFrameAccum += framesPerTick;
            int count = static_cast<int>(sInterpFrameAccum); // floor
            sInterpFrameAccum -= static_cast<double>(count);  // carry the fractional remainder
            if (count < 1) {
                count = 1; // always present at least once (also covers sub-60 targets)
                // The accumulator never earned this present, so debit it as an earned one would
                // have been, or a sub-60 target's long-run present rate creeps above target/60.
                // Floored at one tick's worth of debt so consecutive hits cannot run away. Both
                // target sources are floor-bounded at >=60 above, so this should never fire —
                // defense-in-depth for a future target source that skips those clamps.
                sInterpFrameAccum -= 1.0;
                if (sInterpFrameAccum < -1.0) {
                    sInterpFrameAccum = -1.0;
                }
            }
            if (count > kGdxInterpMaxSubframes) {
                count = kGdxInterpMaxSubframes; // hard M cap
                sInterpFrameAccum = 0.0;        // don't let a clamped burst bank unbounded remainder
            }
            interpMaxSubframes = count;

            // The backend software limiter (gfx_dxgi.cpp / gfx_sdl2.cpp, mTargetFps, default 60)
            // throttles EVERY present, including each decoupled sub-frame present. Left at 60 it
            // pinned total presents at 60/s and dragged the SIM to ~25 Hz during races (60 /
            // avg_M) — zero extra presented frames AND slow motion. Raising it to the interpolation
            // target lets it pace sub-frames instead. Applied only on a change, since SetTargetFps
            // recomputes the limiter phase; the live backend value is the source of truth so on/off
            // and target-change transitions all settle.
            Fast::Fast3dWindow* interpWin = static_cast<Fast::Fast3dWindow*>(w.get());
            if (interpWin->GetTargetFps() != interpEffectiveTarget) {
                interpWin->SetTargetFps(interpEffectiveTarget);
            }
        } else {
            sInterpFrameAccum = 0.0; // reset so a later re-enable starts from a clean remainder
            sInterpMatchRefreshActive = false; // force an immediate OS-Hz re-resolve on next enable
            // Mirror of the raise above; live-value guarded so the backend is only touched on a
            // real change.
            Fast::Fast3dWindow* interpWin = static_cast<Fast::Fast3dWindow*>(w.get());
            if (interpWin->GetTargetFps() != 60) {
                interpWin->SetTargetFps(60);
            }
        }
        gdx_gfx_interp_tick_config(interpOn ? 1 : 0, interpTickStart, kGdxInterpTickSeconds,
                                   interpMaxSubframes);

        // The game frame ACTUALLY runs here, not in gdx_dispatch() below. gdx_vi_tick posts the VI
        // retrace message, which wakes the Main scheduler thread and dispatches the game fiber
        // inline (osSendMesg -> osStartThread -> __osDispatchThread, because __osRunningThread is
        // NULL in host context). Game logic AND the synchronous gfx-task submission (gdx_gfx_run:
        // DL translation, interpreter Run, frame mirror) all execute inside this call, leaving
        // gdx_dispatch() to find an empty run queue.
        gdx::PerfPhaseBegin(gdx::PerfGameTick);
        gdx_vi_tick();   // advance VI framebuffer + post retrace -> runs the Main game fiber here
        gdx::PerfPhaseEnd(gdx::PerfGameTick);
        gdx::PerfPhaseBegin(gdx::PerfInput);
        // The audio thread also self-pumps every 5ms, so a lost or late notify here is a pacing
        // hint, not a correctness issue. See gdx_audio_thread.cpp.
        gdx_audio_thread_notify_frame();
        w->GetMouseStateManager()->StartFrame();
        // Course Edit mouse S2: hides the OS cursor while the absolute mouse drive owns the
        // in-game cursor. Must run AFTER MouseStateManager::StartFrame — the manager re-shows the
        // cursor on any movement, so hiding before its tick would flicker. No-op unless the
        // Course Edit mouse gate is fully active (CVar, mode, menu closed, cursor in the blit
        // rect; yields to F2 capture) — see port/gdx_course_edit_mouse.cpp.
        gdx_course_edit_mouse_cursor_tick();
        // Course Edit / race mouse steering S3: confines the OS cursor to the window while mouse
        // steering is active. Runs after the cursor tick so the same per-frame input state is used.
        gdx_course_edit_mouse_grab_tick();
        // Hide-cursor-in-game option: hides the OS cursor during gameplay, re-shows it for the
        // ImGui menu. Same post-StartFrame ordering requirement as the Course Edit cursor tick.
        gdx_hide_os_cursor_tick();
        gdx::PerfPhaseEnd(gdx::PerfInput);
        if (!interpOn) {
            // Default path: one tick, one Run, one present, paced by the frame pacer. Nothing on
            // this branch touches the interpolation machinery.
            gdx::PerfPhaseBegin(gdx::PerfGuiStart);
            w->GetGui()->StartDraw();
            w->StartFrame(); // must precede gdx_dispatch: Run() needs an initialized frame
            // Cross-thread message wakes recorded by the dedicated audio thread (sendmesg.c PORT
            // path) become runnable here, on the host thread, right before the threads dispatch.
            // See the guard block in n64_sched.c.
            gdx_sched_drain_deferred_wakes();
            gdx::PerfPhaseEnd(gdx::PerfGuiStart);
            gdx::PerfPhaseBegin(gdx::PerfDispatch);
            gdx_dispatch();  // run the decomp's game threads cooperatively until they block again
            gdx::PerfPhaseEnd(gdx::PerfDispatch);
            gdx::PerfPhaseBegin(gdx::PerfTicks);
            // Debounced: persists the 64DD save sidecar atomically once the game's write burst has
            // drained. No-op when nothing is pending.
            gdx_disk_save_tick();
            // F10: diffs gSaveContext for achievement unlocks and accumulates playtime; debounced
            // persistence, no-op when gEnhancements.Achievements.Enabled is 0.
            GdxAchievements_Tick();
            // After gdx_dispatch deliberately: gGameMode flips MID-dispatch (see input_bridge.c's
            // staleness notes), so this is the only window where a sample reflects this frame's
            // post-update truth.
            gdx_discord_tick();
            gdx::PerfPhaseEnd(gdx::PerfTicks);
            gdx::PerfPhaseBegin(gdx::PerfPresent);
            // Presents the VI framebuffer's pixels when no GFX task rendered this frame (boot logo
            // phase, or any CPU-drawn screen). No-op when a real frame was produced.
            gdx_vi_present_fallback();
            w->GetGui()->EndDraw();
            w->EndFrame();
            gdx::PerfPhaseEnd(gdx::PerfPresent);
            // Port-level wall-clock pacer, gated on gEnhancements.Graphics.FramePacing (default OFF
            // — see the registration above). When on, holds the loop to the N64 NTSC field rate.
            gdx::PerfPhaseBegin(gdx::PerfPacer);
            gdx_frame_pacer_tick();
            gdx::PerfPhaseEnd(gdx::PerfPacer);
        } else {
            // The bridge owns the present on this branch: gdx_gfx_run replays the retained gfx task
            // as M sub-frames via fw->DrawAndRunGraphicsCommands, each a full StartDraw..EndFrame
            // bracket. Do NOT open our own ImGui StartDraw here — it would nest the per-sub-frame
            // ImGui frames. This branch presents only as a fallback for a taskless tick (boot logo).
            gdx::PerfPhaseBegin(gdx::PerfGuiStart);
            gdx_sched_drain_deferred_wakes();
            gdx::PerfPhaseEnd(gdx::PerfGuiStart);
            gdx::PerfPhaseBegin(gdx::PerfDispatch);
            gdx_dispatch();  // game runs once; gdx_gfx_run presents the interpolated sub-frames
            gdx::PerfPhaseEnd(gdx::PerfDispatch);
            gdx::PerfPhaseBegin(gdx::PerfTicks);
            gdx_disk_save_tick();
            GdxAchievements_Tick(); // F10; see the non-interp branch's note
            gdx_discord_tick(); // post-dispatch sample; see the non-interp branch's note
            gdx::PerfPhaseEnd(gdx::PerfTicks);
            gdx::PerfPhaseBegin(gdx::PerfPresent);
            if (gdx_gfx_interp_presented_last_tick() == 0) {
                // No gfx task this tick -> interpolation no-ops cleanly. Present once via
                // the normal single-frame bracket (the CPU-drawn VI framebuffer / hold pixels).
                w->GetGui()->StartDraw();
                w->StartFrame();
                gdx_vi_present_fallback();
                w->GetGui()->EndDraw();
                w->EndFrame();
            }
            gdx::PerfPhaseEnd(gdx::PerfPresent);
            // The frame pacer is deliberately NOT called here — interpolation and FramePacing are
            // mutually-exclusive pacing owners, and the pacer also no-ops itself while
            // interpolation is on. Presents already ran VSync-paced inside the sub-frame loop; what
            // remains is holding the host to the 60 Hz LOGIC deadline for the VSync-off case.
            gdx::PerfPhaseBegin(gdx::PerfPacer);
            // Pace against a RUNNING absolute schedule, not a deadline re-anchored to "now" each
            // tick. With the remainder accumulator some ticks present 2 sub-frames (~14 ms VSync)
            // and some 3 (~21 ms); re-anchoring would pad the short ticks with idle time, stutter
            // every 2-3 ticks and drag the average below 60 Hz. A running schedule lets a short
            // tick recover what a long one overran. Re-anchor on a big stall (menu, alt-tab,
            // breakpoint) so a burst of missed ticks is never replayed.
            static double sNextLogicDeadline = 0.0;
            if (sNextLogicDeadline <= 0.0 ||
                interpTickStart > sNextLogicDeadline + 4.0 * kGdxInterpTickSeconds) {
                sNextLogicDeadline = interpTickStart + kGdxInterpTickSeconds;
            } else {
                sNextLogicDeadline += kGdxInterpTickSeconds;
            }
            gdx_host_pace_logic_until(sNextLogicDeadline);
            // Sampled AFTER the pacer on purpose: a healthy tick sleeps to the deadline and
            // publishes ~0, a tick that could not afford its work skips the sleep and publishes the
            // shortfall. The burst pre-sizer steers on this, so it must describe the SCHEDULE and
            // not this tick's wall time — 2- and 3-pass ticks legitimately alternate around the
            // tick duration, and only the running deadline knows whether the sim is losing time.
            gdx_gfx_interp_set_sim_slip(gdx_host_now_seconds() - sNextLogicDeadline);
            gdx::PerfPhaseEnd(gdx::PerfPacer);

            // Telemetry: rate-limited [interp-p2] line + a one-time activation line.
            // Cadence mirrors the bridge's diagnostics: first 8 ticks then every 120th (~1/2 s).
            {
                static bool sInterpP2Announced = false;
                if (!sInterpP2Announced) {
                    sInterpP2Announced = true;
                    gdx_port_logf("[interp-p2] decoupled loop ACTIVE: sim locked at 60 Hz, presenting "
                                  "%d evenly-spaced sub-frames this tick (rational accumulator averages "
                                  "target/60; hard cap %d). Window fps-limiter raised to target; frame "
                                  "pacer mutually excluded.\n",
                                  interpMaxSubframes, kGdxInterpMaxSubframes);
                }
                static size_t sInterpP2Tick = 0;
                static size_t sInterpP2SubAccum = 0;
                const size_t tick = sInterpP2Tick++;
                const int sub = gdx_gfx_interp_last_subframes();
                sInterpP2SubAccum += (sub > 0) ? static_cast<size_t>(sub) : 0;
                // sim_hz counts loop iterations against the wall clock, which IS the sim rate: one
                // iteration advances the game exactly one tick. It has to be measured because the
                // interpolation burst runs INSIDE that iteration — M blocking presents set the
                // iteration length, and an overrun is time the 60 Hz sim never gets back.
                //
                // The failure this catches obeys sim_hz == presents/s / avg_m, which is why both
                // terms share the line: when that identity holds, the present path is pacing the
                // sim and the game runs in slow motion at exactly that ratio. A correct decoupled
                // loop BREAKS the identity, holding sim_hz at 59.94 while presents/s moves freely.
                //
                // Rolling window, not cumulative, so a dip shows up in the line that contains it.
                // Its own 30-tick cadence, independent of the 120-tick log cadence, keeps the FPS
                // overlay moving while the player watches it.
                {
                    static double sSimHzMarkTime = 0.0;
                    static size_t sSimHzMarkTick = 0;
                    if ((tick % 30) == 0) {
                        const double nowSec = gdx_host_now_seconds();
                        if (sSimHzMarkTime > 0.0 && nowSec > sSimHzMarkTime && tick > sSimHzMarkTick) {
                            gGdxSimHzMeasured =
                                static_cast<double>(tick - sSimHzMarkTick) / (nowSec - sSimHzMarkTime);
                        }
                        sSimHzMarkTime = nowSec;
                        sSimHzMarkTick = tick;
                    }
                }
                const double simHz = gGdxSimHzMeasured;
                if (tick < 8 || (tick % 120) == 0) {
                    const double avg = (tick + 1 > 0)
                                           ? static_cast<double>(sInterpP2SubAccum) / static_cast<double>(tick + 1)
                                           : 0.0;
                    // In steady state expect lerped >> snapped. pair_max / pair_susp measure
                    // whether the slots that lerped were paired with the RIGHT previous matrix:
                    // identity is a GfxPool byte offset, so a shift in the pool layout silently
                    // pairs two different objects (see the block comment on
                    // gdx_gfx_interp_pair_max_delta in n64_gfx_bridge.h). Sweep the camera and
                    // watch these — a tail that appears only while the view rotates is the defect.
                    gdx_port_logf("[interp-p2] ticks=%zu subframes=%d dropped=%d avg_m=%.2f "
                                  "sim_hz=%.1f "
                                  "t_last=%.3f tasks=%d lerped=%d snapped=%d "
                                  "vp=%d/%d vtx=%d/%d borrow=%d basisfix=%d campose=%d/%d eyed=%.0f "
                                  "camwhy=%d/%d/%d/%d/%d/%d/%d presents/s=%.1f "
                                  "pair_max=%.0f pair_susp=%d/%d idem_div=%d/%d\n",
                                  tick + 1, sub, gdx_gfx_interp_last_dropped(), avg, simHz,
                                  gdx_gfx_interp_last_t(), gdx_gfx_interp_last_tasks(),
                                  gdx_gfx_interp_last_lerped(),
                                  gdx_gfx_interp_last_snapped(),
                                  gdx_gfx_interp_last_vp_lerped(), gdx_gfx_interp_last_vp_snapped(),
                                  gdx_gfx_interp_last_vtx_lerped(), gdx_gfx_interp_last_vtx_snapped(),
                                  gdx_gfx_interp_last_borrowed(),
                                  gdx_gfx_interp_last_basis_fixed(),
                                  gdx_gfx_interp_last_cam_rebuilt(), gdx_gfx_interp_last_cam_rejected(),
                                  gdx_gfx_interp_last_cam_eye_delta(),
                                  gdx_gfx_interp_last_cam_why(0), gdx_gfx_interp_last_cam_why(1),
                                  gdx_gfx_interp_last_cam_why(2), gdx_gfx_interp_last_cam_why(3),
                                  gdx_gfx_interp_last_cam_why(4), gdx_gfx_interp_last_cam_why(5),
                                  gdx_gfx_interp_last_cam_why(6),
                                  gdx_gfx_interp_presents_per_sec(),
                                  gdx_gfx_interp_pair_max_delta(), gdx_gfx_interp_pair_suspect(),
                                  gdx_gfx_interp_pair_lerped_total(),
                                  gdx_gfx_interp_idem_divergent(), gdx_gfx_interp_idem_multipass());
                }
            }
        }
        gdx::PerfFrameEnd();

        // Auto-exit once a bounded PCM capture finalizes, through the SAME path the window-close
        // event uses, so the loop drains and the normal teardown below runs.
        {
            static const bool sCaptureMode = (std::getenv("GDX_PCM_CAPTURE") != nullptr);
            if (sCaptureMode && gdx_pcm_capture_finished()) {
                logStep("PCM capture finalized; requesting window close (auto-exit)");
                w->Close();
            }
        }
    }
    logStep("window closed; exiting");
    // Discord shutdown FIRST in the exit path, not last: discord-rpc's IO-thread join can hang
    // on Windows (upstream issue #275, open since 2018). Early, a stall is visible while the
    // window is already gone; last, it would leave a silent zombie process. ClearPresence +
    // RunCallbacks run inside so the profile clears before the socket dies.
    gdx_discord_shutdown();
    // gdx_disk_save_tick() only flushes after the write burst has been idle for kDebounceFrames
    // (~0.5s), so a save landing inside that final window would be dropped on close.
    gdx_disk_save_flush();
    // Stop/join the audio thread BEFORE finalizing PCM capture: gdx_pcm_capture_shutdown() closes
    // the file and folds the SHA, so it must not race a still-running audio thread calling feed().
    gdx_audio_thread_stop();
    gdx_pcm_capture_shutdown();
    return 0;
}
