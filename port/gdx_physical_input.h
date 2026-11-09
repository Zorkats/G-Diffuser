// G-Diffuser: direct physical bindings for individual action rows. Legacy bindings are the default.
// Own CVars: gEnhancements.Input.Physical.PortN.<Context>.<Output> (N is 1..4).
// Configured=0 inherits; Configured=1 and Count=0 explicitly unbinds the row.
// Entries <index>.Type/Code/Direction: type 1=keyboard, 2=SDL button, 3=SDL axis,
// 4=mouse button, 5=mouse wheel. Code uses the Ship/SDL enum; Direction is +/-1 for axes, 0 otherwise.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Resolved physical/recomposed outputs use a per-port history matching LUS simulated lag.
// Generic deck blockers freeze that history and suppress physical output; device checks happen
// before samples enter it. Replacement masks stay current. Sticks remain entirely on the legacy path.
unsigned short gdx_physical_input_apply(int port, int context, unsigned short legacyButtons);
// Availability of configured physical bindings, refreshed by apply (independent of UI blockers).
int gdx_physical_input_has_live_input(int port);

#ifdef __cplusplus
}

#include <memory>
#include <string>
#include <vector>
#include "gdx_input_actions.h"
#include "ship/controller/controldevice/controller/mapping/ControllerButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h"

namespace GdxPhysicalInput {
// Call on the input/main thread after CVarLoad() (ConsoleVariable::Load), external physical CVar
// edits, or legacy controller mapping edits. Also dirties the legacy action-mask cache. No config
// writes and no latch reset: the next read/event/apply rebuilds caches and seeds held key state.
// There is no reload notification in LUS; reload callers must explicitly invoke this API.
void Invalidate();

// Ports, contexts and outputs use the zero-based indices from gdx_input_actions.h.
// GetBindings returns only the configured bank; GetEffectiveBindings also resolves inheritance.
std::vector<std::shared_ptr<Ship::ControllerButtonMapping>> GetBindings(int port, int context, int output);
std::vector<std::shared_ptr<Ship::ControllerButtonMapping>> GetEffectiveBindings(int port, int context, int output);
bool IsConfigured(int port, int context, int output);
// Unsupported mappings / invalid indices or snapshots exceeding 256 entries return false without changes.
// The first Add/Replace/Remove snapshots the effective inherited row, preserving its other bindings.
bool AddBinding(int port, int context, int output, std::shared_ptr<Ship::ControllerButtonMapping> binding);
// One main-thread edit: validate/clone first, replace the effective index, then persist once.
// Duplicate physical descriptors collapse to their first occurrence; UI indices may shift.
bool ReplaceBinding(int port, int context, int output, int index,
                    std::shared_ptr<Ship::ControllerButtonMapping> binding);
bool RemoveBinding(int port, int context, int output, int index);
void ClearBindings(int port, int context, int output);     // Explicitly configured, empty row.
void UseLegacyBindings(int port, int context, int output); // Inherit without changing legacy settings.
std::shared_ptr<Ship::ControllerButtonMapping> CaptureSDL(int port, int output);
// True while any gamepad button or capture-threshold axis on the port is held.
bool IsGamepadInputHeld(int port);

// Profiles contain only SDL gamepad button/axis bindings. Import replaces those bindings across
// every action context while preserving keyboard and mouse bindings. Import validates completely
// before changing or persisting any rows.
std::string ExportGamepadProfile(int port, const std::string& name, std::string* error = nullptr);
bool ImportGamepadProfile(int port, const std::string& json, std::string* error = nullptr);

// Capture does not acquire input blockers or persist anything; the UI owns that lifetime.
void BeginKeyboardCapture();
void EndKeyboardCapture();
int TakeCapturedKey(); // LUS (PS/2 set-1) scancode, or -1 when no key is pending.
// Consumed presses are capturable but cannot activate gameplay until released and pressed again.
// Releases/all-keys-up must always be delivered, regardless of consumption.
void ProcessKeyboardEvent(Ship::KbEventType eventType, Ship::KbScancode scancode, bool consumed = false);
} // namespace GdxPhysicalInput
#endif
