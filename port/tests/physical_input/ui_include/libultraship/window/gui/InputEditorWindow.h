#pragma once
#include <cstdint>
#include <imgui.h>
#include "test_runtime.h"

// Service seam only: the derived production editor and all ImGui widgets are real.
namespace LUS {
class InputEditorWindow {
  public:
    virtual ~InputEditorWindow() = default;
    void DrawMovementSettings(uint8_t) {}
    void DrawDeviceSettings(uint8_t) {}

  protected:
    virtual void DrawElement() {}
    virtual void UpdateElement() {
        // Production base UpdateElement resets navigation when its legacy popup is inactive.
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SetGamepadNavigationBlocked(false);
    }
};
} // namespace LUS
