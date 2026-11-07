// Copyright (c) Zorkats
//
// User themes. Each theme is a folder inside <Companion folder>/themes holding a theme.json and an
// optional backdrop image. A user theme's id is "user.<folder>", so it can never take a built-in
// id. A theme is data only: colours, a name, a description and an image file name. Nothing here
// depends on SDL, OpenGL or ImGui; image decoding happens in the window code.
//
// theme.json (every key optional):
//   {
//     "name": "Big Blue",              display name, 64 bytes at most (default: the folder name)
//     "description": "...",            512 bytes at most
//     "backdrop": "backdrop.png",      a .png, .jpg or .jpeg file in the same folder
//     "pixel_art": false,              true draws the backdrop with nearest filtering
//     "accent": "#62D8F7",             default: the backdrop's dominant colour, else Circuit's
//     "panel": "#101820",
//     "panel_alpha": 0.8,              0.6 to 1.0
//     "text": "#F2F7FA",
//     "text_secondary": "#BDC9D4",
//     "backdrop_color": "#080D13"      the base under the art
//   }
// Status colours (ok, warning, error) are not themable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "gdx_companion_theme.h"

namespace gdx_companion {

constexpr size_t kMaxUserThemes = 64;
constexpr uintmax_t kMaxThemeJsonBytes = 64 * 1024;
constexpr uintmax_t kMaxBackdropBytes = 32ull * 1024 * 1024;
constexpr size_t kMaxThemeNameBytes = 64;
constexpr size_t kMaxThemeDescriptionBytes = 512;
constexpr size_t kMaxThemeFolderChars = 48;

// <exe_dir>/themes
std::filesystem::path UserThemesDirectory(const std::filesystem::path& exe_dir);

// The id for a theme folder name, or an empty string when the name is not allowed. Allowed: 1 to
// kMaxThemeFolderChars ASCII letters, digits, spaces, '-' and '_'. The id is lowercase with spaces
// turned into '_'.
std::string UserThemeId(std::string_view folder_name);

// Parses theme.json text into out. out starts from Circuit's colours with the given id and name.
// backdrop_path receives the bare file name; the caller resolves it inside the folder. notes gets
// one line per ignored key. Returns false with error set on a malformed or out-of-range value.
bool ParseThemeJson(std::string_view text, const std::string& id, const std::string& folder_name,
                    ThemeTokens& out, std::vector<std::string>& notes, std::string& error);

// Checks text contrast and fixes the accent. Returns false with error set when body or secondary
// text is below kTextContrastMin on the panel. An accent below kAccentContrastMin is moved toward
// white (dark panel) or black (light panel) until it passes, and note describes the change.
// on_accent becomes whichever of text and backdrop reads better on the accent.
bool FinishTheme(ThemeTokens& theme, std::string& note, std::string& error);

// The dominant colour of an RGBA8 image: the heaviest 3-bit-per-channel bucket among opaque pixels,
// weighted by chroma. Grey, white and black pixels count only when under 5% of the art is coloured,
// so an accent taken from art does not read as grey. Returns fallback when no pixel is opaque.
Rgb8 DominantColor(const uint8_t* rgba, int width, int height, Rgb8 fallback);

struct ThemeScanResult {
    std::vector<ThemeTokens> themes;  // sorted by display name ignoring ASCII case, then id
    std::vector<std::string> notes;   // one line per skipped folder or ignored key
};

// Reads every theme folder in dir. A missing dir gives an empty result and no notes. Themes whose
// accent comes from the art are not finished here: the caller decodes the backdrop, sets the accent
// with DominantColor and then calls FinishTheme.
ThemeScanResult ScanUserThemes(const std::filesystem::path& dir);

}  // namespace gdx_companion
