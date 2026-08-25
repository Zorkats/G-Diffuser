// port/ui/UIWidgets.hpp — CVar-bound ImGui widget library for the G-Diffuser menu.
//
// PROVENANCE
// ----------
// Ported from HarbourMasters/Lighthouse, src/port/UI/UIWidgets.{hpp,cpp} on branch `develop`,
// published under CC0 1.0 Universal (Creative Commons Public Domain Dedication) — copy, modify and
// redistribute freely, no attribution required.
//
// Lighthouse builds against the same upstream libultraship (Kenix3/libultraship) that G-Diffuser
// forks, and the framework is port-local: it touches only ImGui and the public CVar bridge, so it
// needed no libultraship change. Every API delta is listed under ADAPTATIONS below.
//
// The CVar-bound widgets (CVarCheckbox, CVarCombobox, CVarSliderInt, CVarSliderFloat,
// CVarInputString, CVarInputInt, CVarColorPicker, CVarRadioButton) fold read + draw + write +
// persist + tooltip into one call:
//
//     UIWidgets::CVarCheckbox("Foo", "gFoo", { .tooltip = "..." });
//
// What deliberately does NOT go through this library:
//   - DrawDevGates()'s per-gate rows. Their tooltip is a four-argument runtime format
//     ("<help>\n\nEnvironment: %s\nSetting: %s"), which no Options struct can carry, and the value
//     is a gdx_dev_gate() call rather than a CVar read.
//   - The Workshop pack-list checkbox, which sits in a 32px WidthFixed table column that
//     Checkbox()'s unconditional ItemInnerSpacing.x * 2 label gutter would overflow.
//   - The ghost-export button's tooltip, which interpolates a filesystem path that WrappedText
//     would break across lines at the spaces inside it.
//
// Known gaps:
//   - IntSliderOptions / FloatSliderOptions expose no Flags() setter, so ImGuiSliderFlags (e.g.
//     AlwaysClamp) can only be set through a designated initialiser.
//   - IntSliderOptions / FloatSliderOptions / ComboboxOptions do not re-declare Disabled() or
//     DisabledTooltip(), so those two must also come from the WidgetOptions aggregate; the
//     inherited setters return WidgetOptions& and break the fluent chain's type.
//   - CVarCombobox's std::unordered_map overload renders its rows in unspecified order, so it is
//     unusable for any dropdown whose ordering is meaningful. Use the vector/array overloads.
//   - CVarRadioButton attaches its tooltip to the label Text it draws after the radio, not to the
//     radio itself, so hovering the button does not raise it.
//
// ADAPTATIONS vs Lighthouse
// -------------------------
//  1. Ship_IsCStringEmpty() lives in Lighthouse's own src/port/ShipUtils.h and does not exist
//     anywhere in this fork. Replaced by UIWidgets::IsCStringEmpty() below, same semantics.
//  2. ShipInit::Init(cvarName) — Lighthouse's "run the registered side effect for this CVar"
//     dispatch table (src/port/ShipInit.hpp). No equivalent here; G-Diffuser applies side effects
//     at the call site, so it is dropped from every CVar* widget.
//  3. Ship::Context::GetRawInstance() is not present in this fork's ship/Context.h, which only has
//     the shared_ptr GetInstance() (ship/Context.h:50). Every save call routes through
//     UIWidgets::SaveCVars(), which null-guards the whole chain.
//  4. fmt::format via <spdlog/fmt/fmt.h> is reachable only implicitly, through vcpkg's MSBuild
//     integration rather than any include path this repo's CMake spells out. The two uses (both in
//     DrawFlagArray*) became plain std::string concatenation, so this file has no fmt dependency.
//  5. ImGui::RenderNavHighlight() -> ImGui::RenderNavCursor(). Renamed in ImGui 1.91.4; the
//     vendored ImGui is 1.91.9b (build/x64/_deps/imgui-src/imgui.h:31), where the old name only
//     survives as an obsolete-API inline alias.
//  6. ImGuiCol_TabActive -> ImGuiCol_TabSelected (renamed in 1.90.9; old name is obsolete-only).
//  7. ImGuiColorEditFlags_AlphaPreview — removed in ImGui 1.91.8 and now the default behaviour
//     (imgui.h:1869 keeps it as a `= 0` obsolete stub). Dropped from CVarColorPicker's flags.
//  8. InputOptions::addedFlags was typed ImGuiInputFlags upstream but is passed to InputText /
//     InputScalar, which take ImGuiInputTextFlags. Both are `typedef int` so it compiled by
//     accident; retyped to ImGuiInputTextFlags.
//  9. GetRandomValue / RGBA8FromVec / VecFromRGBA8 moved inside namespace UIWidgets so this library
//     adds no unqualified global symbols (upstream declared them at global scope).
// 10. Upstream defects fixed in place (each marked "FIX:" at the site):
//       - `const char* longest;` left uninitialised in all four Combobox overloads, then read
//         when the container is empty.
//       - ColorPickerOptions' five bool members left uninitialised.
//       - RadioButton was declared with 2 parameters in the .hpp but defined with 3 in the .cpp,
//         so the declared overload had no definition to link against.
//       - CVarInputInt called std::stoi on InputOptions::defaultValue, which defaults to "" and
//         therefore throws std::invalid_argument on the struct's own default.
//       - Combobox's LabelPositions::Far path called comboMap.at(*value), which throws when the
//         current value is not a key in the map.
//
// LICENSE: CC0 1.0 Universal. Original: https://github.com/HarbourMasters/Lighthouse

#ifndef GDX_UIWIDGETS_HPP
#define GDX_UIWIDGETS_HPP

#include <cstring>
#include <memory>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

// ImGui's ImVec2/ImVec4 math operators are opt-in. The define is guarded on imgui.h not having been
// pulled in yet: setting it after imgui.h is already include-guarded leaves
// IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED unset, and any later imgui_internal.h include then
// hard-#errors (build/x64/_deps/imgui-src/imgui_internal.h:112-113). That ordering is reachable —
// GuiWindow.h includes imgui.h (libultraship/include/ship/window/gui/GuiWindow.h:4). Nothing in
// this header needs the operators; UIWidgets.cpp does, and defines the macro before any include.
#if !defined(IMGUI_VERSION) && !defined(IMGUI_DEFINE_MATH_OPERATORS)
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#include "libultraship/bridge/consolevariablebridge.h" // CVarGet*/CVarSet*/CVarClear*
#include "libultraship/color.h"                        // Color_RGBA8 (used by the colour picker)

namespace Ship {
// Forward-declared instead of including ship/window/gui/GuiWindow.h: that header drags in
// imgui_internal.h, and only WindowButton()'s signature needs the type. UIWidgets.cpp includes
// the real header.
class GuiWindow;
} // namespace Ship

namespace UIWidgets {

// Replacement for Lighthouse's Ship_IsCStringEmpty() (ADAPTATION #1). Tooltip sites spell "no
// tooltip" as the empty string as often as nullptr, so both cases must be handled.
inline bool IsCStringEmpty(const char* str) {
    return str == nullptr || str[0] == '\0';
}

// Queues a CVar flush to disk on the next frame. Null-guarded end to end: the Window/Gui may not
// exist yet during early startup (same shape as port/gdx_menu.cpp's GdxSaveCvars).
void SaveCVars();

struct TextFilters {
    static int FilterNumbers(ImGuiInputTextCallbackData* data) {
        if (data->EventChar < 256 && strchr("1234567890", (char)data->EventChar)) {
            return 0;
        }
        return 1;
    }

    static int FilterAlphaNum(ImGuiInputTextCallbackData* data) {
        const char* alphanum = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWYZ0123456789";
        if (data->EventChar < 256 && strchr(alphanum, (char)data->EventChar)) {
            return 0;
        }
        return 1;
    }
};

std::string WrappedText(const char* text, unsigned int charactersPerLine = 80);
std::string WrappedText(const std::string& text, unsigned int charactersPerLine = 80);
void PaddedSeparator(bool padTop = true, bool padBottom = true, float extraVerticalTopPadding = 0.0f,
                     float extraVerticalBottomPadding = 0.0f);
void Tooltip(const char* text);

typedef enum ColorPickerModifiers {
    ColorPickerResetButton = 1,
    ColorPickerRandomButton = 2,
    ColorPickerRainbowCheck = 4,
    ColorPickerLockCheck = 8,
} ColorPickerModifiers;

// mostly in order for colors usable by the menu without custom text color
enum Colors {
    Red,
    DarkRed,
    Orange,
    Green,
    DarkGreen,
    LightBlue,
    SkyBlue,
    Blue,
    DarkBlue,
    Indigo,
    Violet,
    Purple,
    Brown,
    Gray,
    DarkGray,
    // not suitable for menu theme use
    Pink,
    Yellow,
    Cyan,
    Black,
    LightGray,
    White,
    NoColor
};

enum InputTypes { String, Scalar };

// `inline`: upstream had a bare `const`, which has internal linkage in a header and therefore built
// one private copy of this map per translation unit that includes it.
inline const std::unordered_map<Colors, ImVec4> ColorValues = {
    { Colors::Pink, ImVec4(0.87f, 0.3f, 0.87f, 1.0f) },     { Colors::Red, ImVec4(0.55f, 0.0f, 0.0f, 1.0f) },
    { Colors::DarkRed, ImVec4(0.3f, 0.0f, 0.0f, 1.0f) },    { Colors::Orange, ImVec4(0.85f, 0.55f, 0.0f, 1.0f) },
    { Colors::Yellow, ImVec4(0.95f, 0.95f, 0.0f, 1.0f) },   { Colors::Green, ImVec4(0.0f, 0.55f, 0.0f, 1.0f) },
    { Colors::DarkGreen, ImVec4(0.0f, 0.3f, 0.0f, 1.0f) },  { Colors::Cyan, ImVec4(0.0f, 0.9f, 0.9f, 1.0f) },
    { Colors::LightBlue, ImVec4(0.0f, 0.24f, 0.8f, 1.0f) }, { Colors::Blue, ImVec4(0.08f, 0.03f, 0.65f, 1.0f) },
    { Colors::DarkBlue, ImVec4(0.03f, 0.0f, 0.5f, 1.0f) },  { Colors::Indigo, ImVec4(0.35f, 0.0f, 0.87f, 1.0f) },
    { Colors::Violet, ImVec4(0.5f, 0.0f, 0.9f, 1.0f) },     { Colors::Purple, ImVec4(0.31f, 0.0f, 0.67f, 1.0f) },
    { Colors::Brown, ImVec4(0.37f, 0.18f, 0.0f, 1.0f) },    { Colors::LightGray, ImVec4(0.75f, 0.75f, 0.75f, 1.0f) },
    { Colors::Gray, ImVec4(0.45f, 0.45f, 0.45f, 1.0f) },    { Colors::DarkGray, ImVec4(0.15f, 0.15f, 0.15f, 1.0f) },
    { Colors::Black, ImVec4(0.0f, 0.0f, 0.0f, 1.0f) },      { Colors::White, ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
    { Colors::NoColor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f) },    { Colors::SkyBlue, ImVec4(0.00f, 0.75f, 1.00f, 1.00f) },
};

namespace Sizes {
inline const ImVec2 Inline = ImVec2(0.0f, 0.0f); // widget takes exactly its content's width
inline const ImVec2 Fill = ImVec2(-1.0f, 0.0f);  // widget stretches to the remaining line width
} // namespace Sizes

enum LabelPositions {
    Near,
    Far,
    Above,
    None,
    Within,
};

enum ComponentAlignments {
    Left,
    Right,
};

// Base of every Options struct. The setters return *this so options can be spelled fluently:
//   UIWidgets::CheckboxOptions().Tooltip("...").Color(UIWidgets::Colors::Green)
// C++20 designated initialisers work too, which is how the aggregate defaults get overridden:
//   UIWidgets::CheckboxOptions({ { .tooltip = "..." } }).Color(...)
struct WidgetOptions {
    const char* tooltip = "";
    bool disabled = false;
    const char* disabledTooltip = "";

    WidgetOptions& Tooltip(const char* tooltip_) {
        tooltip = tooltip_;
        return *this;
    }

    WidgetOptions& Disabled(bool disabled_) {
        disabled = disabled_;
        return *this;
    }

    WidgetOptions& DisabledTooltip(const char* disabledTooltip_) {
        disabledTooltip = disabledTooltip_;
        return *this;
    }
};

struct TextOptions : WidgetOptions {
    Colors color = Colors::NoColor;

    TextOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }
};

struct ButtonOptions : WidgetOptions {
    ImVec2 size = Sizes::Fill;
    ImVec2 padding = ImVec2(10.0f, 8.0f);
    Colors color = Colors::Gray;

    ButtonOptions& Size(ImVec2 size_) {
        size = size_;
        return *this;
    }

    ButtonOptions& Padding(ImVec2 padding_) {
        padding = padding_;
        return *this;
    }

    // Re-declared (rather than inherited) so the fluent chain keeps the derived type and later
    // .Color()/.Size() calls still resolve. Same reason on every other Options struct below.
    ButtonOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    ButtonOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }
};

struct ColorPickerOptions : WidgetOptions {
    ImVec2 size = Sizes::Fill;
    ImVec2 padding = ImVec2(10.0f, 8.0f);
    Colors color = Colors::Gray;
    Color_RGBA8 defaultValue = { 255, 255, 255, 255 };
    // FIX: upstream declared these five as `bool useAlpha, showReset, showRandom, showRainbow,
    // showLock;` with no initialisers, so a default-constructed ColorPickerOptions carried
    // indeterminate values and the picker sprouted random buttons.
    bool useAlpha = false;
    bool showReset = false;
    bool showRandom = false;
    bool showRainbow = false;
    bool showLock = false;

    ColorPickerOptions& Size(ImVec2 size_) {
        size = size_;
        return *this;
    }

    ColorPickerOptions& Padding(ImVec2 padding_) {
        padding = padding_;
        return *this;
    }

    ColorPickerOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    ColorPickerOptions& ShowReset(bool showReset_ = true) {
        showReset = showReset_;
        return *this;
    }

    ColorPickerOptions& ShowRandom(bool showRandom_ = true) {
        showRandom = showRandom_;
        return *this;
    }

    ColorPickerOptions& ShowRainbow(bool showRainbow_ = true) {
        showRainbow = showRainbow_;
        return *this;
    }

    ColorPickerOptions& ShowLock(bool showLock_ = true) {
        showLock = showLock_;
        return *this;
    }

    ColorPickerOptions& UseAlpha(bool useAlpha_ = true) {
        useAlpha = useAlpha_;
        return *this;
    }

    ColorPickerOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }

    ColorPickerOptions& DefaultValue(Color_RGBA8 defaultValue_) {
        defaultValue = defaultValue_;
        return *this;
    }
};

struct WindowButtonOptions : WidgetOptions {
    ImVec2 size = Sizes::Inline;
    ImVec2 padding = ImVec2(10.0f, 8.0f);
    Colors color = Colors::Gray;
    bool showButton = true;
    bool embedWindow = true;

    WindowButtonOptions& Size(ImVec2 size_) {
        size = size_;
        return *this;
    }

    WindowButtonOptions& Padding(ImVec2 padding_) {
        padding = padding_;
        return *this;
    }

    WindowButtonOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    WindowButtonOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }

    WindowButtonOptions& ShowButton(bool showButton_) {
        showButton = showButton_;
        return *this;
    }

    WindowButtonOptions& EmbedWindow(bool embedWindow_) {
        embedWindow = embedWindow_;
        return *this;
    }
};

struct CheckboxOptions : WidgetOptions {
    bool defaultValue = false; // Only applicable to CVarCheckbox
    ComponentAlignments alignment = ComponentAlignments::Left;
    LabelPositions labelPosition = LabelPositions::Near;
    ImVec2 padding = ImVec2(10.0f, 8.0f);
    Colors color = Colors::LightBlue;

    CheckboxOptions& DefaultValue(bool defaultValue_) {
        defaultValue = defaultValue_;
        return *this;
    }

    CheckboxOptions& ComponentAlignment(ComponentAlignments alignment_) {
        alignment = alignment_;
        return *this;
    }

    CheckboxOptions& LabelPosition(LabelPositions labelPosition_) {
        labelPosition = labelPosition_;
        return *this;
    }

    CheckboxOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    CheckboxOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }

    CheckboxOptions& DisabledTooltip(const char* disabledTooltip_) {
        WidgetOptions::disabledTooltip = disabledTooltip_;
        return *this;
    }

    CheckboxOptions& Padding(ImVec2 padding_) {
        padding = padding_;
        return *this;
    }
};

struct ComboboxOptions : WidgetOptions {
    std::unordered_map<int32_t, const char*> comboMap = {};
    uint32_t defaultIndex = 0; // Only applicable to CVarCombobox
    ComponentAlignments alignment = ComponentAlignments::Left;
    LabelPositions labelPosition = LabelPositions::Above;
    ImGuiComboFlags flags = 0;
    Colors color = Colors::LightBlue;

    ComboboxOptions& ComboMap(std::unordered_map<int32_t, const char*> comboMap_) {
        comboMap = comboMap_;
        return *this;
    }

    ComboboxOptions& DefaultIndex(uint32_t defaultIndex_) {
        defaultIndex = defaultIndex_;
        return *this;
    }

    ComboboxOptions& ComponentAlignment(ComponentAlignments alignment_) {
        alignment = alignment_;
        return *this;
    }

    ComboboxOptions& LabelPosition(LabelPositions labelPosition_) {
        labelPosition = labelPosition_;
        return *this;
    }

    ComboboxOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    ComboboxOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }
};

struct IntSliderOptions : WidgetOptions {
    bool showButtons = true;
    const char* format = "%d";
    int32_t step = 1;
    int32_t min = 1;
    int32_t max = 10;
    int32_t defaultValue = 1;
    bool clamp = true;
    ComponentAlignments alignment = ComponentAlignments::Left;
    LabelPositions labelPosition = LabelPositions::Above;
    Colors color = Colors::Gray;
    ImGuiSliderFlags flags = 0;
    ImVec2 size = { 0, 0 };

    IntSliderOptions& ShowButtons(bool showButtons_) {
        showButtons = showButtons_;
        return *this;
    }

    IntSliderOptions& Format(const char* format_) {
        format = format_;
        return *this;
    }

    IntSliderOptions& Step(int32_t step_) {
        step = step_;
        return *this;
    }

    IntSliderOptions& Min(int32_t min_) {
        min = min_;
        return *this;
    }

    IntSliderOptions& Max(int32_t max_) {
        max = max_;
        return *this;
    }

    IntSliderOptions& DefaultValue(int32_t defaultValue_) {
        defaultValue = defaultValue_;
        return *this;
    }

    IntSliderOptions& ComponentAlignment(ComponentAlignments alignment_) {
        alignment = alignment_;
        return *this;
    }

    IntSliderOptions& LabelPosition(LabelPositions labelPosition_) {
        labelPosition = labelPosition_;
        return *this;
    }

    IntSliderOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    IntSliderOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }

    IntSliderOptions& Size(ImVec2 size_) {
        size = size_;
        return *this;
    }

    IntSliderOptions& Clamp(bool clamp_) {
        clamp = clamp_;
        return *this;
    }
};

struct FloatSliderOptions : WidgetOptions {
    bool showButtons = true;
    const char* format = "%f";
    float step = 0.01f;
    float min = 0.01f;
    float max = 10.0f;
    float defaultValue = 1.0f;
    bool clamp = true;
    bool isPercentage = false; // Multiplies visual value by 100
    ComponentAlignments alignment = ComponentAlignments::Left;
    LabelPositions labelPosition = LabelPositions::Above;
    Colors color = Colors::Gray;
    ImGuiSliderFlags flags = 0;
    ImVec2 size = { 0, 0 };

    FloatSliderOptions& ShowButtons(bool showButtons_) {
        showButtons = showButtons_;
        return *this;
    }

    FloatSliderOptions& Format(const char* format_) {
        format = format_;
        return *this;
    }

    FloatSliderOptions& Step(float step_) {
        step = step_;
        return *this;
    }

    FloatSliderOptions& Min(float min_) {
        min = min_;
        return *this;
    }

    FloatSliderOptions& Max(float max_) {
        max = max_;
        return *this;
    }

    FloatSliderOptions& DefaultValue(float defaultValue_) {
        defaultValue = defaultValue_;
        return *this;
    }

    FloatSliderOptions& ComponentAlignment(ComponentAlignments alignment_) {
        alignment = alignment_;
        return *this;
    }

    FloatSliderOptions& LabelPosition(LabelPositions labelPosition_) {
        labelPosition = labelPosition_;
        return *this;
    }

    // Note the side effects: asking for percentage display also rewrites format/min/max, so call
    // this BEFORE any explicit .Format()/.Min()/.Max() or they will be overwritten.
    FloatSliderOptions& IsPercentage(bool isPercentage_ = true) {
        isPercentage = isPercentage_;
        format = "%.0f%%";
        min = 0.0f;
        max = 1.0f;
        return *this;
    }

    FloatSliderOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    FloatSliderOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }

    FloatSliderOptions& Size(ImVec2 size_) {
        size = size_;
        return *this;
    }

    FloatSliderOptions& Clamp(bool clamp_) {
        clamp = clamp_;
        return *this;
    }
};

struct RadioButtonsOptions : WidgetOptions {
    std::unordered_map<int32_t, const char*> buttonMap;
    int32_t defaultIndex = 0;
    Colors color = Colors::LightBlue;

    RadioButtonsOptions& ButtonMap(std::unordered_map<int32_t, const char*> buttonMap_) {
        buttonMap = buttonMap_;
        return *this;
    }

    RadioButtonsOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    RadioButtonsOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }

    RadioButtonsOptions& DefaultIndex(int32_t defaultIndex_) {
        defaultIndex = defaultIndex_;
        return *this;
    }
};

struct InputOptions : WidgetOptions {
    ComponentAlignments alignment = ComponentAlignments::Left;
    LabelPositions labelPosition = LabelPositions::Above;
    Colors color = Colors::Gray;
    ImVec2 size = { 0, 0 };
    std::string placeholder = "";
    InputTypes type = InputTypes::String;
    std::string defaultValue = "";
    bool secret = false;
    // Retyped from ImGuiInputFlags: this is forwarded to InputText()/InputScalar(), whose flags
    // parameter is ImGuiInputTextFlags. Both are `typedef int` so upstream compiled by accident.
    ImGuiInputTextFlags addedFlags = 0;
    bool hasError = false;
    const char* errorText = "";

    InputOptions& Tooltip(const char* tooltip_) {
        WidgetOptions::tooltip = tooltip_;
        return *this;
    }

    InputOptions& Color(Colors color_) {
        color = color_;
        return *this;
    }

    InputOptions& Size(ImVec2 size_) {
        size = size_;
        return *this;
    }

    InputOptions& LabelPosition(LabelPositions labelPosition_) {
        labelPosition = labelPosition_;
        return *this;
    }

    InputOptions& PlaceholderText(std::string&& placeholder_) {
        placeholder = std::move(placeholder_);
        return *this;
    }

    InputOptions& PlaceholderText(std::string& placeholder_) {
        placeholder = placeholder_;
        return *this;
    }

    InputOptions& InputType(InputTypes type_) {
        type = type_;
        return *this;
    }

    InputOptions& ComponentAlignment(ComponentAlignments alignment_) {
        alignment = alignment_;
        return *this;
    }

    InputOptions& DefaultValue(std::string defaultValue_) {
        defaultValue = defaultValue_;
        return *this;
    }

    InputOptions& IsSecret(bool secret_ = false) {
        secret = secret_;
        return *this;
    }

    InputOptions& HasError(bool error_ = false) {
        hasError = error_;
        return *this;
    }

    InputOptions& ErrorText(const char* errorText_) {
        errorText = errorText_;
        return *this;
    }
};

void PushStyleMenu(const ImVec4& color);
void PushStyleMenu(Colors color = Colors::LightBlue);
void PopStyleMenu();
bool BeginMenu(const char* label, Colors color = Colors::LightBlue);

void PushStyleMenuItem(const ImVec4& color);
void PushStyleMenuItem(Colors color = Colors::LightBlue);
void PopStyleMenuItem();
bool MenuItem(const char* label, const char* shortcut = NULL, Colors color = Colors::LightBlue);

void PushStyleButton(const ImVec4& color, ImVec2 padding = ImVec2(10.0f, 8.0f));
void PushStyleButton(Colors color = Colors::Gray, ImVec2 padding = ImVec2(10.0f, 8.0f));
void PopStyleButton();
bool Button(const char* label, const ButtonOptions& options = {});
bool WindowButton(const char* label, const char* cvarName, std::shared_ptr<Ship::GuiWindow> windowPtr,
                  const WindowButtonOptions& options = {});

void PushStyleCheckbox(const ImVec4& color, ImVec2 padding = ImVec2(10.0f, 6.0f));
void PushStyleCheckbox(Colors color = Colors::LightBlue, ImVec2 padding = ImVec2(10.0f, 6.0f));
void PopStyleCheckbox();
void RenderText(ImVec2 pos, const char* text, const char* text_end, bool hide_text_after_hash);
bool Checkbox(const char* label, bool* v, const CheckboxOptions& options = {});
bool CVarCheckbox(const char* label, const char* cvarName, const CheckboxOptions& options = {});

void PushStyleCombobox(const ImVec4& color);
void PushStyleCombobox(Colors color = Colors::LightBlue);
void PopStyleCombobox();

void PushStyleTabs(const ImVec4& color);
void PushStyleTabs(Colors color = Colors::LightBlue);
void PopStyleTabs();

void PushStyleInput(const ImVec4& color);
void PushStyleInput(Colors color = Colors::LightBlue);
void PopStyleInput();

void PushStyleHeader(const ImVec4& color);
void PushStyleHeader(Colors color = Colors::LightBlue);
void PopStyleHeader();

void Spacer(float height = 0.0f);
void Separator(bool padTop = true, bool padBottom = true, float extraVerticalTopPadding = 0.0f,
               float extraVerticalBottomPadding = 0.0f);

float CalcComboWidth(const char* preview_value, ImGuiComboFlags flags);

template <typename T>
bool Combobox(const char* label, T* value, const std::unordered_map<T, const char*>& comboMap,
              const ComboboxOptions& options = {}) {
    bool dirty = false;
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    // The combo is sized to its widest entry so the popup does not resize as the selection
    // changes. FIX: `longest` was declared uninitialised upstream and read below even when the
    // map is empty.
    const char* longest = "";
    size_t length = 0;
    for (auto& [index, string] : comboMap) {
        size_t len = strlen(string);
        if (len > length) {
            longest = string;
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);

    auto previewIt = comboMap.find(*value);
    const char* previewLabel = previewIt != comboMap.end() ? previewIt->second : "";

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", label);
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", label);
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo(invisibleLabel, previewLabel, options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (const auto& pair : comboMap) {
            // Entries whose label is a single character are treated as hidden/placeholder rows.
            if (strlen(pair.second) > 1) {
                const bool isSelected = pair.first == *value;
                if (ImGui::Selectable(pair.second, isSelected)) {
                    *value = pair.first;
                    dirty = true;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", label);
            } else if (options.labelPosition == LabelPositions::Far) {
                // FIX: upstream called comboMap.at(*value) here, which throws std::out_of_range
                // when the CVar holds a value that is not a key of the map (easy to hit after a
                // config edit or an enum reorder). previewLabel is the same lookup, made safe.
                float width = ImGui::CalcTextSize(previewLabel).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", label);
            }
        }
    }
    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

template <typename T = size_t>
bool Combobox(const char* label, T* value, const std::vector<const char*>& comboVector,
              const ComboboxOptions& options = {}) {
    bool dirty = false;
    size_t currentValueIndex = static_cast<size_t>(*value);
    if (currentValueIndex >= comboVector.size()) {
        currentValueIndex = 0;
    }
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    const char* longest = ""; // FIX: uninitialised upstream, see the map overload above
    size_t length = 0;
    for (auto& string : comboVector) {
        size_t len = strlen(string);
        if (len > length) {
            longest = string;
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);
    const char* previewLabel = comboVector.empty() ? "" : comboVector.at(currentValueIndex);

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", label);
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", label);
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo(invisibleLabel, previewLabel, options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (size_t i = 0; i < comboVector.size(); ++i) {
            auto newValue = static_cast<T>(i);
            if (strlen(comboVector.at(i)) > 1) {
                const bool isSelected = newValue == *value;
                if (ImGui::Selectable(comboVector.at(i), isSelected)) {
                    *value = newValue;
                    dirty = true;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", label);
            } else if (options.labelPosition == LabelPositions::Far) {
                // FIX: was comboVector.at(*value), which throws when the CVar is out of range.
                float width = ImGui::CalcTextSize(previewLabel).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", label);
            }
        }
    }

    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

template <typename T = size_t>
bool Combobox(const char* label, T* value, const std::vector<std::string>& comboVector,
              const ComboboxOptions& options = {}) {
    bool dirty = false;
    size_t currentValueIndex = static_cast<size_t>(*value);
    if (currentValueIndex >= comboVector.size()) {
        currentValueIndex = 0;
    }
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    const char* longest = ""; // FIX: uninitialised upstream, see the map overload above
    size_t length = 0;
    for (auto& string : comboVector) {
        size_t len = string.length();
        if (len > length) {
            longest = string.c_str();
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);
    const char* previewLabel = comboVector.empty() ? "" : comboVector.at(currentValueIndex).c_str();

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", label);
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", label);
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo(invisibleLabel, previewLabel, options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (size_t i = 0; i < comboVector.size(); ++i) {
            auto newValue = static_cast<T>(i);
            if (comboVector.at(i).length() > 1) {
                const bool isSelected = newValue == *value;
                if (ImGui::Selectable(comboVector.at(i).c_str(), isSelected)) {
                    *value = newValue;
                    dirty = true;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", label);
            } else if (options.labelPosition == LabelPositions::Far) {
                // FIX: was comboVector.at(*value), which throws when the CVar is out of range.
                float width = ImGui::CalcTextSize(previewLabel).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", label);
            }
        }
    }

    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

template <typename T = size_t, size_t N>
bool Combobox(const char* label, T* value, const char* (&comboArray)[N], const ComboboxOptions& options = {}) {
    bool dirty = false;
    size_t currentValueIndex = static_cast<size_t>(*value);
    if (currentValueIndex >= N) {
        currentValueIndex = 0;
    }
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    const char* longest = ""; // FIX: uninitialised upstream, see the map overload above
    size_t length = 0;
    for (size_t i = 0; i < N; i++) {
        size_t len = strlen(comboArray[i]);
        if (len > length) {
            longest = comboArray[i];
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);
    const char* previewLabel = comboArray[currentValueIndex];

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", label);
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", label);
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo(invisibleLabel, previewLabel, options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (size_t i = 0; i < N; ++i) {
            auto newValue = static_cast<T>(i);
            if (strlen(comboArray[i]) > 1) {
                const bool isSelected = newValue == *value;
                if (ImGui::Selectable(comboArray[i], isSelected)) {
                    *value = newValue;
                    dirty = true;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", label);
            } else if (options.labelPosition == LabelPositions::Far) {
                // FIX: was comboArray[*value], an out-of-bounds read for an out-of-range CVar.
                float width = ImGui::CalcTextSize(previewLabel).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", label);
            }
        }
    }
    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

// The CVar* wrappers read the CVar, draw the widget, and on change write it back and queue the
// config flush. Lighthouse also called ShipInit::Init(cvarName) here to fire the CVar's registered
// side effect; we have no such registry (ADAPTATIONS #2), so callers apply side effects themselves.
template <typename T = int32_t>
bool CVarCombobox(const char* label, const char* cvarName, const std::unordered_map<T, const char*>& comboMap,
                  const ComboboxOptions& options = {}) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultIndex);
    if (Combobox<T>(label, &value, comboMap, options)) {
        CVarSetInteger(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

template <typename T = int32_t>
bool CVarCombobox(const char* label, const char* cvarName, const std::vector<const char*>& comboVector,
                  const ComboboxOptions& options = {}) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultIndex);
    if (Combobox<T>(label, &value, comboVector, options)) {
        CVarSetInteger(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

template <typename T = int32_t>
bool CVarCombobox(const char* label, const char* cvarName, const std::vector<std::string>& comboVector,
                  const ComboboxOptions& options = {}) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultIndex);
    if (Combobox<T>(label, &value, comboVector, options)) {
        CVarSetInteger(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

template <typename T = int32_t, size_t N>
bool CVarCombobox(const char* label, const char* cvarName, const char* (&comboArray)[N],
                  const ComboboxOptions& options = {}) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultIndex);
    if (Combobox<T>(label, &value, comboArray, options)) {
        CVarSetInteger(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

void PushStyleSlider(Colors color = Colors::LightBlue);
void PopStyleSlider();
bool SliderInt(const char* label, int32_t* value, const IntSliderOptions& options = {});
bool CVarSliderInt(const char* label, const char* cvarName, const IntSliderOptions& options = {});
bool SliderFloat(const char* label, float* value, const FloatSliderOptions& options = {});
bool CVarSliderFloat(const char* label, const char* cvarName, const FloatSliderOptions& options = {});
bool InputString(const char* label, std::string* value, const InputOptions& options = {});
bool CVarInputString(const char* label, const char* cvarName, const InputOptions& options = {});
bool InputInt(const char* label, int32_t* value, const InputOptions& options = {});
bool CVarInputInt(const char* label, const char* cvarName, const InputOptions& options = {});

// Stores three sibling CVars under `cvarName`: "<cvarName>.Value" (Color_RGBA8), ".Rainbow" and
// ".Locked" (both integer booleans). `modifiers` is a bitmask of ColorPickerModifiers selecting
// which of the reset/random/rainbow/lock affordances are drawn.
bool CVarColorPicker(const char* label, const char* cvarName, Color_RGBA8 defaultColor, bool hasAlpha = false,
                     uint8_t modifiers = 0, Colors themeColor = Colors::LightBlue);
// Options-struct overload, forwarding onto the modifier bitmask above. Upstream declared
// ColorPickerOptions but never wired it to anything.
bool CVarColorPicker(const char* label, const char* cvarName, const ColorPickerOptions& options);

// FIX: upstream declared `RadioButton(const char*, bool)` in the header but defined the 3-argument
// form in the .cpp, so the declared overload had no definition and any caller failed to link.
bool RadioButton(const char* label, bool active, const RadioButtonsOptions& options = {});
bool CVarRadioButton(const char* text, const char* cvarName, int32_t id, const RadioButtonsOptions& options = {});
bool StateButton(const char* str_id, const char* label, ImVec2 size, ButtonOptions options,
                 ImGuiButtonFlags flags = ImGuiButtonFlags_None);
void DrawFlagArray32(const std::string& name, uint32_t& flags, Colors color = Colors::LightBlue);
void DrawFlagArray16(const std::string& name, uint16_t& flags, Colors color = Colors::LightBlue);
void DrawFlagArray8(const std::string& name, uint8_t& flags, Colors color = Colors::LightBlue);

void InsertHelpHoverText(const std::string& text);
void InsertHelpHoverText(const char* text);

ImVec4 GetRandomValue();
Color_RGBA8 RGBA8FromVec(ImVec4 vec);
ImVec4 VecFromRGBA8(Color_RGBA8 color);

} // namespace UIWidgets

#endif /* GDX_UIWIDGETS_HPP */
