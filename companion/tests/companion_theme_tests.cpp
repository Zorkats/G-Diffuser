// Copyright (c) Zorkats
//
// Headless tests for Companion themes: folder ids, theme.json parsing, the contrast pass, the
// dominant colour, and the folder scan. Links gdx_companion_host only: no SDL, OpenGL or ImGui.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gdx_companion_theme.h"
#include "gdx_companion_themes.h"

using namespace gdx_companion;
namespace fs = std::filesystem;

namespace {

int gPassed = 0;
int gFailed = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        ++gPassed;
        std::printf("PASS: %s\n", name);
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name);
    }
}

bool Same(Rgb8 a, Rgb8 b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

bool HasNote(const std::vector<std::string>& notes, const std::string& needle) {
    for (const std::string& note : notes) {
        if (note.find(needle) != std::string::npos) return true;
    }
    return false;
}

void WriteFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

void BuiltIn() {
    ThemeTokens circuit = CircuitTheme();
    std::printf("Circuit contrast: text %.2f, secondary %.2f, accent %.2f, warning %.2f, ok %.2f, error %.2f\n",
                MinContrastOnPanel(circuit, circuit.text), MinContrastOnPanel(circuit, circuit.text_secondary),
                MinContrastOnPanel(circuit, circuit.accent), MinContrastOnPanel(circuit, circuit.status_warning),
                MinContrastOnPanel(circuit, circuit.status_ok), MinContrastOnPanel(circuit, circuit.status_error));
    const Rgb8 accent = circuit.accent;
    std::string note;
    std::string error;
    Check(FinishTheme(circuit, note, error) && note.empty() && Same(circuit.accent, accent),
          "Circuit passes the contrast pass unchanged");
    Check(MinContrastOnPanel(circuit, circuit.status_error) >= kTextContrastMin &&
              MinContrastOnPanel(circuit, circuit.status_warning) >= kTextContrastMin &&
              MinContrastOnPanel(circuit, circuit.status_ok) >= kTextContrastMin,
          "status colours read as text on Circuit's panel");
    Check(ContrastRatio({0, 0, 0}, {255, 255, 255}) > 20.99 && ContrastRatio({0, 0, 0}, {255, 255, 255}) < 21.01,
          "black on white is 21:1");
}

void FolderIds() {
    Check(UserThemeId("Big Blue") == "user.big_blue", "spaces become underscores and the id is lowercase");
    Check(UserThemeId("mute-city_2") == "user.mute-city_2", "digits, - and _ are kept");
    Check(UserThemeId("gdx.theme.circuit").empty(), "a dotted folder cannot spell a built-in id");
    Check(UserThemeId("..").empty() && UserThemeId("a/b").empty() && UserThemeId("").empty(),
          "path-like and empty folder names are refused");
    Check(UserThemeId(std::string(48, 'a')) == "user." + std::string(48, 'a') &&
              UserThemeId(std::string(49, 'a')).empty(),
          "folder names stop at 48 characters");
    Check(UserThemeId("\xC3\xA9t\xC3\xA9").empty(), "non-ASCII folder names are refused");
}

void Parsing() {
    std::vector<std::string> notes;
    std::string error;
    ThemeTokens theme;
    const char* full =
        "{\"name\":\"Big Blue\",\"description\":\"Night race\",\"backdrop\":\"sky.PNG\",\"pixel_art\":true,"
        "\"accent\":\"#62d8f7\",\"panel\":\"#101820\",\"panel_alpha\":0.85,\"text\":\"#FFFFFF\","
        "\"text_secondary\":\"#C0C8D0\",\"backdrop_color\":\"#000000\",\"music\":\"x.wav\"}";
    const bool ok = ParseThemeJson(full, "user.big_blue", "Big Blue", theme, notes, error);
    Check(ok && theme.id == "user.big_blue" && theme.display_name == "Big Blue" &&
              theme.description == "Night race" && theme.backdrop_path == "sky.PNG" && theme.pixel_art &&
              !theme.accent_from_art && Same(theme.accent, {0x62, 0xD8, 0xF7}) && theme.panel_alpha == 0.85f &&
              Same(theme.text, {255, 255, 255}) && Same(theme.backdrop, {0, 0, 0}),
          "every documented key is read");
    Check(HasNote(notes, "unknown key \"music\" ignored"), "an unknown key is noted, not fatal");

    notes.clear();
    Check(ParseThemeJson("{\"backdrop\":\"art.jpg\"}", "user.a", "a", theme, notes, error) &&
              theme.accent_from_art && theme.display_name == "a" && theme.panel_alpha == 0.80f &&
              Same(theme.status_error, CircuitTheme().status_error),
          "art without an accent takes the accent from the art; status colours stay fixed");
    Check(ParseThemeJson("{}", "user.a", "a", theme, notes, error) && !theme.accent_from_art &&
              Same(theme.accent, CircuitTheme().accent),
          "no art and no accent keeps Circuit's accent");

    const char* bad[] = {
        "[1]",
        "{\"accent\":\"62D8F7\"}",
        "{\"accent\":\"#62D8FG\"}",
        "{\"backdrop\":\"../sky.png\"}",
        "{\"backdrop\":\"C:sky.png\"}",
        "{\"backdrop\":\"sky.gif\"}",
        "{\"panel_alpha\":0.2}",
        "{\"pixel_art\":\"yes\"}",
        "{\"name\":7}",
        "{\"accent\":\"#000000\",\"accent\":\"#FFFFFF\"}",
    };
    bool all_refused = true;
    for (const char* text : bad) {
        error.clear();
        if (ParseThemeJson(text, "user.a", "a", theme, notes, error) || error.empty()) {
            std::printf("  accepted: %s\n", text);
            all_refused = false;
        }
    }
    Check(all_refused, "malformed, out-of-range and path-escaping values are refused with a reason");
    Check(!ParseThemeJson("{\"name\":\"" + std::string(65, 'n') + "\"}", "user.a", "a", theme, notes, error),
          "a name over 64 bytes is refused");
}

void Contrast() {
    ThemeTokens theme = CircuitTheme();
    theme.accent = {0x20, 0x28, 0x30};  // nearly the panel colour
    std::string note;
    std::string error;
    Check(FinishTheme(theme, note, error) && MinContrastOnPanel(theme, theme.accent) >= kAccentContrastMin &&
              note.find("#202830 moved to") != std::string::npos,
          "a dim accent is lightened until it reaches 3:1, and the change is noted");
    Check(ContrastRatio(theme.on_accent, theme.accent) >= ContrastRatio(theme.text, theme.accent) ||
              ContrastRatio(theme.on_accent, theme.accent) >= ContrastRatio(theme.backdrop, theme.accent),
          "the label colour on the accent is the more readable of text and base");

    ThemeTokens light = CircuitTheme();
    light.backdrop = {0xF0, 0xF0, 0xF0};
    light.panel = {0xFF, 0xFF, 0xFF};
    light.text = {0x10, 0x10, 0x10};
    light.text_secondary = {0x40, 0x40, 0x40};
    light.accent = {0xFF, 0xF0, 0xA0};  // pale yellow on white
    Check(FinishTheme(light, note, error) && MinContrastOnPanel(light, light.accent) >= kAccentContrastMin,
          "on a light panel the accent is darkened instead");

    ThemeTokens unreadable = CircuitTheme();
    unreadable.text = {0x30, 0x38, 0x40};
    error.clear();
    Check(!FinishTheme(unreadable, note, error) && error.find("need 4.5") != std::string::npos,
          "text below 4.5:1 refuses the theme instead of lowering contrast");
}

void Dominant() {
    std::vector<uint8_t> image(64 * 64 * 4);
    for (size_t i = 0; i < 64 * 64; ++i) {
        uint8_t* p = &image[i * 4];
        const bool red = i % 4 == 0;  // a quarter of the pixels
        p[0] = red ? 200 : 128;
        p[1] = red ? 30 : 128;
        p[2] = red ? 40 : 128;
        p[3] = 255;
    }
    const Rgb8 dominant = DominantColor(image.data(), 64, 64, {1, 2, 3});
    Check(dominant.r == 200 && dominant.g == 30 && dominant.b == 40,
          "a saturated quarter outweighs a grey majority");
    // A third of the art is one white block; the rest is a red-to-orange gradient spread over many shades.
    for (size_t i = 0; i < 64 * 64; ++i) {
        uint8_t* p = &image[i * 4];
        const size_t x = i % 64;
        const bool white = i < 64 * 21;
        p[0] = white ? 255 : 230;
        p[1] = white ? 255 : static_cast<uint8_t>(40 + x * 2);
        p[2] = white ? 255 : 30;
        p[3] = 255;
    }
    const Rgb8 warm = DominantColor(image.data(), 64, 64, {1, 2, 3});
    Check(warm.r > 200 && warm.g < 180 && warm.b < 60, "a large white block does not beat a coloured gradient");
    for (size_t i = 0; i < 64 * 64; ++i) {
        image[i * 4] = image[i * 4 + 1] = image[i * 4 + 2] = i < 64 * 40 ? 20 : 90;
    }
    const Rgb8 dark = DominantColor(image.data(), 64, 64, {1, 2, 3});
    Check(dark.r == 20 && dark.g == 20 && dark.b == 20, "grey-only art still gives its main grey");
    for (size_t i = 0; i < 64 * 64; ++i) image[i * 4 + 3] = 0;
    Check(Same(DominantColor(image.data(), 64, 64, {1, 2, 3}), {1, 2, 3}),
          "a fully transparent image gives the fallback");
    Check(Same(DominantColor(nullptr, 0, 0, {4, 5, 6}), {4, 5, 6}), "no image gives the fallback");
}

void Scan() {
    const fs::path root = fs::temp_directory_path() /
                          ("gdx-companion-themes-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(ScanUserThemes(root).themes.empty() && ScanUserThemes(root).notes.empty(),
          "a missing themes folder gives nothing and no warnings");

    fs::create_directories(root / "Zeta");
    WriteFile(root / "Zeta" / "theme.json", "{\"name\":\"Zeta\",\"accent\":\"#E05050\"}");
    fs::create_directories(root / "alpha");
    WriteFile(root / "alpha" / "theme.json", "{\"name\":\"Alpha\",\"backdrop\":\"missing.png\"}");
    fs::create_directories(root / "with art");
    WriteFile(root / "with art" / "theme.json", "{\"backdrop\":\"art.png\"}");
    WriteFile(root / "with art" / "art.png", "not decoded by the scan");
    fs::create_directories(root / "bad name!");
    WriteFile(root / "bad name!" / "theme.json", "{}");
    fs::create_directories(root / "empty");
    fs::create_directories(root / "broken");
    WriteFile(root / "broken" / "theme.json", "{\"name\":");
    fs::create_directories(root / "big");
    WriteFile(root / "big" / "theme.json", std::string(kMaxThemeJsonBytes + 1, ' '));
    WriteFile(root / "loose-file.json", "{}");

    const ThemeScanResult scan = ScanUserThemes(root);
    std::vector<std::string> ids;
    for (const ThemeTokens& t : scan.themes) ids.push_back(t.id);
    Check(ids == std::vector<std::string>{"user.alpha", "user.with_art", "user.zeta"},
          "valid folders become themes sorted by name; files beside them are ignored");
    Check(HasNote(scan.notes, "\"bad name!\" skipped") && HasNote(scan.notes, "\"empty\" skipped: it has no theme.json") &&
              HasNote(scan.notes, "theme user.broken skipped") && HasNote(scan.notes, "theme user.big skipped"),
          "each skipped folder gets one line saying why");
    Check(HasNote(scan.notes, "backdrop missing.png not found") && scan.themes[0].backdrop_path.empty() &&
              !scan.themes[0].accent_from_art,
          "a missing backdrop is noted and the theme still loads without art");
    const ThemeTokens& art = scan.themes[1];
    Check(art.accent_from_art && fs::path(art.backdrop_path).filename() == "art.png" &&
              fs::path(art.backdrop_path).is_absolute(),
          "a present backdrop resolves to a full path and waits for its accent");

    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

int main() {
    BuiltIn();
    FolderIds();
    Parsing();
    Contrast();
    Dominant();
    Scan();
    std::printf("theme tests: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
