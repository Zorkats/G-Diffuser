#include "gdx_input_viewer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"
#include "ship/window/Window.h"

extern "C" int gdx_input_viewer_state(unsigned short* outButtons, signed char* outStickX,
                                        signed char* outStickY);

namespace {

constexpr unsigned short kButtonA = 0x8000;
constexpr unsigned short kButtonB = 0x4000;
constexpr unsigned short kButtonZ = 0x2000;
constexpr unsigned short kButtonStart = 0x1000;
constexpr unsigned short kDpadUp = 0x0800;
constexpr unsigned short kDpadDown = 0x0400;
constexpr unsigned short kDpadLeft = 0x0200;
constexpr unsigned short kDpadRight = 0x0100;
constexpr unsigned short kButtonL = 0x0020;
constexpr unsigned short kButtonR = 0x0010;
constexpr unsigned short kCUp = 0x0008;
constexpr unsigned short kCDown = 0x0004;
constexpr unsigned short kCLeft = 0x0002;
constexpr unsigned short kCRight = 0x0001;

constexpr int kOutlineAlways = 0;
constexpr int kOutlineWhileReleased = 1;
constexpr int kOutlineWhilePressed = 2;
constexpr int kOutlineNever = 3;
constexpr float kCanvasWidth = 360.0f;
constexpr float kCanvasHeight = 196.0f;
constexpr float kMaximumAxis = 80.0f;

bool ShouldDrawOutline(int mode, bool pressed) {
    return mode == kOutlineAlways || (mode == kOutlineWhileReleased && !pressed) ||
           (mode == kOutlineWhilePressed && pressed);
}

ImU32 Color(int r, int g, int b, float alpha, float opacity) {
    const int a = std::clamp(static_cast<int>(std::round(alpha * opacity * 255.0f)), 0, 255);
    return IM_COL32(r, g, b, a);
}

ImVec2 Add(const ImVec2& a, const ImVec2& b) {
    return ImVec2(a.x + b.x, a.y + b.y);
}

} // namespace

GdxInputViewer::GdxInputViewer() : Ship::GuiWindow("gOpenWindows.InputViewer", false, "Input Viewer") {
    CVarRegisterFloat("gInputViewer.Scale", 1.0f);
    CVarRegisterFloat("gInputViewer.Opacity", 1.0f);
    CVarRegisterInteger("gInputViewer.EnableDragging", 1);
    CVarRegisterInteger("gInputViewer.ShowBackground", 1);
    CVarRegisterInteger("gInputViewer.ShowAnalogValues", 0);
    CVarRegisterInteger("gInputViewer.ShowDpad", 1);
    CVarRegisterInteger("gInputViewer.ButtonOutlineMode", kOutlineWhileReleased);
    CVarRegisterInteger("gInputViewer.StyleVersion", 0);

    if (CVarGetInteger("gInputViewer.StyleVersion", 0) < 3) {
        CVarSetFloat("gInputViewer.Opacity", 1.0f);
        CVarSetInteger("gInputViewer.ShowBackground", 1);
        CVarSetInteger("gInputViewer.ShowAnalogValues", 0);
        CVarSetInteger("gInputViewer.ShowDpad", 1);
        CVarSetInteger("gInputViewer.ButtonOutlineMode", kOutlineWhileReleased);
        CVarSetInteger("gInputViewer.StyleVersion", 3);

        auto context = Ship::Context::GetInstance();
        if (context != nullptr && context->GetWindow() != nullptr && context->GetWindow()->GetGui() != nullptr) {
            context->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    }
}

void GdxInputViewer::InitElement() {
}

void GdxInputViewer::UpdateElement() {
}

void GdxInputViewer::Draw() {
    if (!IsVisible()) {
        return;
    }

    DrawElement();
    SyncVisibilityConsoleVariable();
}

void GdxInputViewer::DrawElement() {
    const float scale = std::clamp(CVarGetFloat("gInputViewer.Scale", 1.0f), 0.5f, 2.5f);
    const float opacity = std::clamp(CVarGetFloat("gInputViewer.Opacity", 1.0f), 0.2f, 1.0f);
    const bool showAnalogValues = CVarGetInteger("gInputViewer.ShowAnalogValues", 0) != 0;
    const ImVec2 canvasSize(kCanvasWidth * scale, kCanvasHeight * scale);
    const float valuesHeight = showAnalogValues ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
    const ImVec2 windowSize(canvasSize.x + 20.0f, canvasSize.y + valuesHeight + 20.0f);
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowContentSize(ImVec2(canvasSize.x, canvasSize.y + valuesHeight));
    ImGui::SetNextWindowPos(viewport->WorkPos + viewport->WorkSize - canvasSize - ImVec2(30.0f, 30.0f),
                            ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;
    if (CVarGetInteger("gInputViewer.EnableDragging", 1) == 0) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("Input Viewer##GdxPrimitiveOverlay", nullptr, flags)) {
        ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        auto point = [&](float x, float y) { return Add(origin, ImVec2(x * scale, y * scale)); };

        const ImU32 outline = Color(218, 223, 230, 0.86f, opacity);
        const ImU32 muted = Color(143, 151, 163, 0.72f, opacity);
        const ImU32 label = Color(245, 247, 250, 0.96f, opacity);
        const ImU32 darkLabel = Color(24, 28, 34, 0.95f, opacity);
        const ImU32 active = Color(80, 171, 218, 0.95f, opacity);   // A button + analog stick (N64 blue)
        const ImU32 bActive = Color(96, 188, 122, 0.95f, opacity);  // B button (N64 green)
        const ImU32 cActive = Color(225, 190, 76, 0.95f, opacity);  // C buttons (N64 yellow)
        const ImU32 startActive = Color(207, 103, 92, 0.95f, opacity); // Start (N64 red)
        const ImU32 neutral = Color(150, 159, 173, 0.95f, opacity); // L / R / Z / D-pad (N64 grey)
        const float thickness = 1.5f * scale < 1.0f ? 1.0f : 1.5f * scale;

        if (CVarGetInteger("gInputViewer.ShowBackground", 1) != 0) {
            draw->AddRectFilled(point(0.0f, 0.0f), point(kCanvasWidth, kCanvasHeight),
                                Color(20, 24, 30, 0.78f, opacity), 10.0f * scale);
            draw->AddRect(point(0.0f, 0.0f), point(kCanvasWidth, kCanvasHeight),
                          Color(255, 255, 255, 0.12f, opacity), 10.0f * scale, 0, thickness);
            draw->AddText(point(14.0f, 10.0f), muted, "INPUT");
            draw->AddLine(point(14.0f, 31.0f), point(346.0f, 31.0f),
                          Color(255, 255, 255, 0.10f, opacity), thickness);
        }

        unsigned short buttons = 0;
        signed char stickX = 0;
        signed char stickY = 0;
        gdx_input_viewer_state(&buttons, &stickX, &stickY);
        const int outlineMode = std::clamp(CVarGetInteger("gInputViewer.ButtonOutlineMode", kOutlineWhileReleased),
                                           kOutlineAlways, kOutlineNever);

        auto centeredText = [&](ImVec2 center, const char* text, ImU32 color) {
            const ImVec2 size = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), color, text);
        };

        auto circleButton = [&](float x, float y, float radius, const char* text, unsigned short mask,
                                ImU32 fill, ImU32 textColor) {
            const bool pressed = (buttons & mask) != 0;
            const ImVec2 center = point(x, y);
            if (pressed) {
                draw->AddCircleFilled(center, radius * scale, fill, 24);
            }
            if (ShouldDrawOutline(outlineMode, pressed)) {
                draw->AddCircle(center, radius * scale, outline, 24, thickness);
            }
            if (pressed || ShouldDrawOutline(outlineMode, pressed)) {
                centeredText(center, text, pressed ? textColor : outline);
            }
        };

        auto rectButton = [&](float x1, float y1, float x2, float y2, const char* text,
                              unsigned short mask, ImU32 fill) {
            const bool pressed = (buttons & mask) != 0;
            const ImVec2 min = point(x1, y1);
            const ImVec2 max = point(x2, y2);
            if (pressed) {
                draw->AddRectFilled(min, max, fill, 5.0f * scale);
            }
            if (ShouldDrawOutline(outlineMode, pressed)) {
                draw->AddRect(min, max, outline, 5.0f * scale, 0, thickness);
            }
            if (pressed || ShouldDrawOutline(outlineMode, pressed)) {
                centeredText(ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), text,
                             pressed ? label : outline);
            }
        };

        // Grey on the real controller, except Start, which is red.
        rectButton(18.0f, 42.0f, 92.0f, 63.0f, "L", kButtonL, neutral);
        rectButton(268.0f, 42.0f, 342.0f, 63.0f, "R", kButtonR, neutral);
        rectButton(162.0f, 74.0f, 200.0f, 95.0f, "START", kButtonStart, startActive);
        rectButton(166.0f, 166.0f, 196.0f, 186.0f, "Z", kButtonZ, neutral);

        circleButton(298.0f, 146.0f, 20.0f, "A", kButtonA, active, label);
        circleButton(254.0f, 120.0f, 15.0f, "B", kButtonB, bActive, label);
        circleButton(302.0f, 76.0f, 10.0f, "C", kCUp, cActive, darkLabel);
        circleButton(282.0f, 94.0f, 10.0f, "C", kCLeft, cActive, darkLabel);
        circleButton(322.0f, 94.0f, 10.0f, "C", kCRight, cActive, darkLabel);
        circleButton(302.0f, 112.0f, 10.0f, "C", kCDown, cActive, darkLabel);

        const ImVec2 analogCenter = point(82.0f, 104.0f);
        draw->AddCircle(analogCenter, 27.0f * scale, outline, 32, thickness);
        draw->AddLine(point(57.0f, 104.0f), point(107.0f, 104.0f), Color(255, 255, 255, 0.12f, opacity),
                      thickness);
        draw->AddLine(point(82.0f, 80.0f), point(82.0f, 128.0f), Color(255, 255, 255, 0.12f, opacity),
                      thickness);
        constexpr float kAnalogMovement = 15.0f;
        const ImVec2 analogOffset(kAnalogMovement * (static_cast<float>(stickX) / kMaximumAxis) * scale,
                                  -kAnalogMovement * (static_cast<float>(stickY) / kMaximumAxis) * scale);
        draw->AddCircleFilled(Add(analogCenter, analogOffset), 10.0f * scale,
                              (stickX != 0 || stickY != 0) ? active : muted, 24);

        if (CVarGetInteger("gInputViewer.ShowDpad", 1) != 0) {
            const float dpadRound = 2.0f * scale;
            auto dpadArm = [&](float x1, float y1, float x2, float y2, unsigned short mask) {
                const bool pressed = (buttons & mask) != 0;
                if (pressed) {
                    draw->AddRectFilled(point(x1, y1), point(x2, y2), neutral, dpadRound);
                }
                if (ShouldDrawOutline(outlineMode, pressed)) {
                    draw->AddRect(point(x1, y1), point(x2, y2), outline, dpadRound, 0, thickness);
                }
            };
            // Hub is (35.5,151.5)-(48.5,164.5); each arm extends 15px along one axis.
            dpadArm(35.5f, 136.5f, 48.5f, 151.5f, kDpadUp);
            dpadArm(35.5f, 164.5f, 48.5f, 179.5f, kDpadDown);
            dpadArm(20.5f, 151.5f, 35.5f, 164.5f, kDpadLeft);
            dpadArm(48.5f, 151.5f, 63.5f, 164.5f, kDpadRight);
            draw->AddRectFilled(point(35.5f, 151.5f), point(48.5f, 164.5f),
                                Color(255, 255, 255, 0.06f, opacity), dpadRound);
            draw->AddRect(point(35.5f, 151.5f), point(48.5f, 164.5f), outline, dpadRound, 0, thickness);
        }

        ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));
        // A Dummy reserves the canvas without being a clickable item. An InvisibleButton here took every click,
        // and ImGui only starts a window move from a click that lands on no item, so the viewer could not be dragged.
        ImGui::Dummy(canvasSize);
        if (showAnalogValues) {
            ImGui::SetCursorPos(ImVec2(20.0f, canvasSize.y + 10.0f));
            ImGui::Text("Stick  X %+d   Y %+d", static_cast<int>(stickX), static_cast<int>(stickY));
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
