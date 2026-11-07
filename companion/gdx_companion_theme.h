// Copyright (c) Zorkats
//
// Typed theme tokens. Views take colours from one ThemeTokens value and never from literals. A theme
// is data: the built-in Circuit theme below, or a user theme read from a folder of its own
// (gdx_companion_themes.h). Contrast is checked on the panel at panel_alpha over the darkest and
// brightest art pixel the backdrop can show (MinContrastOnPanel); companion_theme_tests reports the
// built-in values.
#pragma once

#include <cstdint>
#include <string>

namespace gdx_companion {

struct Rgb8 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

// Backdrop art is drawn at this opacity over black, so it sits behind text and never under it.
constexpr float kBackdropOpacity = 0.25f;

// Minimum contrast ratios: body text, and the accent used for borders, grabs and focus.
constexpr double kTextContrastMin = 4.5;
constexpr double kAccentContrastMin = 3.0;

struct ThemeTokens {
    std::string id;            // "gdx.theme.<name>" for built-ins, "user.<folder>" for user themes
    std::string display_name;
    std::string description;   // optional, shown under the picker
    std::string backdrop_path;  // UTF-8 path of the backdrop image; empty for none
    bool pixel_art = false;    // draw the backdrop with nearest filtering
    bool accent_from_art = false;  // no accent given: take it from the backdrop's dominant colour
    Rgb8 backdrop;             // window clear colour, the base surface
    Rgb8 panel;                // panel fill, drawn at panel_alpha over the backdrop
    float panel_alpha;
    Rgb8 text;                 // body text
    Rgb8 text_secondary;       // metadata, paths, hints
    Rgb8 accent;               // the single theme accent
    Rgb8 on_accent;            // label colour on filled accent controls
    Rgb8 status_ok;            // status colours stay the same in every theme
    Rgb8 status_warning;
    Rgb8 status_error;
};

inline ThemeTokens CircuitTheme() {
    ThemeTokens t;
    t.id = "gdx.theme.circuit";
    t.display_name = "Circuit";
    t.description = "The built-in theme.";
    t.backdrop = {0x08, 0x0D, 0x13};        // #080D13
    t.panel = {0x10, 0x18, 0x20};           // #101820
    t.panel_alpha = 0.94f;
    t.text = {0xF2, 0xF7, 0xFA};            // #F2F7FA
    t.text_secondary = {0xBD, 0xC9, 0xD4};  // #BDC9D4
    t.accent = {0x62, 0xD8, 0xF7};          // #62D8F7 cyan
    t.on_accent = {0x08, 0x0D, 0x13};       // #080D13
    t.status_ok = {0x7C, 0xE4, 0xA1};       // #7CE4A1
    t.status_warning = {0xF4, 0xBB, 0x55};  // #F4BB55
    t.status_error = {0xFF, 0x8F, 0x8F};    // #FF8F8F
    return t;
}

// WCAG relative luminance and contrast ratio.
double RelativeLuminance(Rgb8 color);
double ContrastRatio(Rgb8 a, Rgb8 b);

// The lower contrast of fg on the panel over the darkest and the brightest art pixel: black and white
// drawn at kBackdropOpacity over the base colour.
double MinContrastOnPanel(const ThemeTokens& theme, Rgb8 fg);

}  // namespace gdx_companion
