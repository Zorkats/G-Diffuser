// Copyright (c) Zorkats

#include "gdx_companion_themes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "gdx_companion_json.h"
#include "gdx_companion_paths.h"

namespace fs = std::filesystem;

namespace gdx_companion {

namespace {

double Channel(uint8_t value) {
    const double c = static_cast<double>(value) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

uint8_t Mix(uint8_t from, uint8_t to, double amount) {
    const double v = static_cast<double>(from) + (static_cast<double>(to) - from) * amount;
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
}

Rgb8 MixColor(Rgb8 from, Rgb8 to, double amount) {
    return {Mix(from.r, to.r, amount), Mix(from.g, to.g, amount), Mix(from.b, to.b, amount)};
}

Rgb8 PanelOver(const ThemeTokens& theme, Rgb8 base) {
    return MixColor(base, theme.panel, theme.panel_alpha);
}

std::string Hex(Rgb8 c) {
    char text[8];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X", c.r, c.g, c.b);
    return text;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseHexColor(const std::string& text, Rgb8& out) {
    if (text.size() != 7 || text[0] != '#') return false;
    uint8_t parts[3];
    for (int i = 0; i < 3; ++i) {
        const int hi = HexDigit(text[1 + i * 2]);
        const int lo = HexDigit(text[2 + i * 2]);
        if (hi < 0 || lo < 0) return false;
        parts[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    out = {parts[0], parts[1], parts[2]};
    return true;
}

bool IsBackdropFileName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find_first_of("/\\:") != std::string::npos) return false;
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}

bool ReadSmallFile(const fs::path& path, uintmax_t limit, std::string& out, std::string& error) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        error = "cannot read theme.json: " + ec.message();
        return false;
    }
    if (size > limit) {
        error = "theme.json is larger than " + std::to_string(limit / 1024) + " KiB";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open theme.json";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

double RelativeLuminance(Rgb8 color) {
    return 0.2126 * Channel(color.r) + 0.7152 * Channel(color.g) + 0.0722 * Channel(color.b);
}

double ContrastRatio(Rgb8 a, Rgb8 b) {
    const double la = RelativeLuminance(a);
    const double lb = RelativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

double MinContrastOnPanel(const ThemeTokens& theme, Rgb8 fg) {
    // The art is drawn at kBackdropOpacity over the base colour, so its pixels land between these two.
    const Rgb8 darkest = MixColor(theme.backdrop, {0, 0, 0}, kBackdropOpacity);
    const Rgb8 brightest = MixColor(theme.backdrop, {255, 255, 255}, kBackdropOpacity);
    return std::min(ContrastRatio(fg, PanelOver(theme, darkest)), ContrastRatio(fg, PanelOver(theme, brightest)));
}

fs::path UserThemesDirectory(const fs::path& exe_dir) { return exe_dir / "themes"; }

std::string UserThemeId(std::string_view folder_name) {
    if (folder_name.empty() || folder_name.size() > kMaxThemeFolderChars) return std::string();
    std::string id = "user.";
    for (const char c : folder_name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 0x80) {
            id += static_cast<char>(std::tolower(u));
        } else if (c == '-' || c == '_') {
            id += c;
        } else if (c == ' ') {
            id += '_';
        } else {
            return std::string();
        }
    }
    return id;
}

bool ParseThemeJson(std::string_view text, const std::string& id, const std::string& folder_name,
                    ThemeTokens& out, std::vector<std::string>& notes, std::string& error) {
    JsonValue root;
    if (!JsonValue::Parse(text, root, error)) return false;
    if (root.kind() != JsonValue::Kind::Object) {
        error = "theme.json must hold one object";
        return false;
    }

    ThemeTokens theme = CircuitTheme();
    theme.id = id;
    theme.display_name = folder_name;
    theme.description.clear();
    theme.panel_alpha = 0.80f;  // let the art show around the text panels
    theme.accent_from_art = true;

    const auto string_field = [&](const char* key, size_t max_bytes, std::string& target) {
        const JsonValue* v = root.Find(key);
        if (v == nullptr) return true;
        if (v->kind() != JsonValue::Kind::String) {
            error = std::string("\"") + key + "\" must be a string";
            return false;
        }
        if (v->AsString().size() > max_bytes) {
            error = std::string("\"") + key + "\" is longer than " + std::to_string(max_bytes) + " bytes";
            return false;
        }
        target = v->AsString();
        return true;
    };
    const auto color_field = [&](const char* key, Rgb8& target) {
        const JsonValue* v = root.Find(key);
        if (v == nullptr) return true;
        if (v->kind() != JsonValue::Kind::String || !ParseHexColor(v->AsString(), target)) {
            error = std::string("\"") + key + "\" must be a colour like \"#62D8F7\"";
            return false;
        }
        return true;
    };

    std::string name;
    if (!string_field("name", kMaxThemeNameBytes, name)) return false;
    if (!name.empty()) theme.display_name = name;
    if (!string_field("description", kMaxThemeDescriptionBytes, theme.description)) return false;

    std::string backdrop;
    if (!string_field("backdrop", 255, backdrop)) return false;
    if (!backdrop.empty() && !IsBackdropFileName(backdrop)) {
        error = "\"backdrop\" must be a .png, .jpg or .jpeg file name in the theme folder";
        return false;
    }
    theme.backdrop_path = backdrop;

    if (const JsonValue* v = root.Find("pixel_art")) {
        if (v->kind() != JsonValue::Kind::Bool) {
            error = "\"pixel_art\" must be true or false";
            return false;
        }
        theme.pixel_art = v->AsBool();
    }
    if (const JsonValue* v = root.Find("panel_alpha")) {
        if (v->kind() != JsonValue::Kind::Number || v->AsNumber() < 0.6 || v->AsNumber() > 1.0) {
            error = "\"panel_alpha\" must be a number from 0.6 to 1.0";
            return false;
        }
        theme.panel_alpha = static_cast<float>(v->AsNumber());
    }
    if (root.Find("accent") != nullptr) {
        if (!color_field("accent", theme.accent)) return false;
        theme.accent_from_art = false;
    }
    if (!color_field("panel", theme.panel) || !color_field("text", theme.text) ||
        !color_field("text_secondary", theme.text_secondary) || !color_field("backdrop_color", theme.backdrop)) {
        return false;
    }
    // Without art there is nothing to take an accent from, so Circuit's stays.
    if (theme.backdrop_path.empty()) theme.accent_from_art = false;

    static const char* const kKnown[] = {"name", "description", "backdrop", "pixel_art", "accent", "panel",
                                         "panel_alpha", "text", "text_secondary", "backdrop_color"};
    for (const std::string& key : root.Keys()) {
        if (std::find(std::begin(kKnown), std::end(kKnown), key) == std::end(kKnown)) {
            notes.push_back("theme " + id + ": unknown key \"" + key + "\" ignored");
        }
    }
    out = theme;
    return true;
}

bool FinishTheme(ThemeTokens& theme, std::string& note, std::string& error) {
    note.clear();
    const double text = MinContrastOnPanel(theme, theme.text);
    const double secondary = MinContrastOnPanel(theme, theme.text_secondary);
    char buffer[160];
    if (text < kTextContrastMin || secondary < kTextContrastMin) {
        std::snprintf(buffer, sizeof(buffer),
                      "text contrast on the panel is %.2f and secondary text %.2f; both need %.1f",
                      text, secondary, kTextContrastMin);
        error = buffer;
        return false;
    }

    const Rgb8 original = theme.accent;
    if (MinContrastOnPanel(theme, theme.accent) < kAccentContrastMin) {
        const Rgb8 target = RelativeLuminance(PanelOver(theme, theme.backdrop)) < 0.18 ? Rgb8{255, 255, 255}
                                                                                  : Rgb8{0, 0, 0};
        for (int step = 1; step <= 20; ++step) {
            theme.accent = MixColor(original, target, step * 0.05);
            if (MinContrastOnPanel(theme, theme.accent) >= kAccentContrastMin) break;
        }
        std::snprintf(buffer, sizeof(buffer), "accent %s moved to %s to reach %.1f contrast on the panel",
                      Hex(original).c_str(), Hex(theme.accent).c_str(), kAccentContrastMin);
        note = buffer;
    }
    theme.on_accent = ContrastRatio(theme.backdrop, theme.accent) >= ContrastRatio(theme.text, theme.accent)
                          ? theme.backdrop
                          : theme.text;
    return true;
}

Rgb8 DominantColor(const uint8_t* rgba, int width, int height, Rgb8 fallback) {
    if (rgba == nullptr || width <= 0 || height <= 0) return fallback;
    struct Bucket {
        double weight = 0.0;
        double r = 0.0, g = 0.0, b = 0.0;
    };
    // 3 bits per channel, so a soft gradient gathers in a few buckets. Coloured and grey pixels are
    // kept apart: grey wins only when the art has almost no colour.
    constexpr int kChroma = 48;              // max - min channel at or above this counts as coloured
    constexpr double kColouredShare = 0.05;  // coloured pixels needed to ignore the greys
    std::vector<Bucket> coloured(512);
    std::vector<Bucket> grey(512);
    size_t opaque = 0;
    size_t coloured_count = 0;
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t stride = std::max<size_t>(1, pixels / 262144);  // sample about 256k pixels at most
    for (size_t i = 0; i < pixels; i += stride) {
        const uint8_t* p = rgba + i * 4;
        if (p[3] < 128) continue;
        ++opaque;
        const int chroma = std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
        const bool is_coloured = chroma >= kChroma;
        coloured_count += is_coloured ? 1 : 0;
        const double weight = is_coloured ? static_cast<double>(chroma) / 255.0 : 1.0;
        Bucket& bucket = (is_coloured ? coloured : grey)[(p[0] >> 5) << 6 | (p[1] >> 5) << 3 | (p[2] >> 5)];
        bucket.weight += weight;
        bucket.r += p[0] * weight;
        bucket.g += p[1] * weight;
        bucket.b += p[2] * weight;
    }
    if (opaque == 0) return fallback;
    const std::vector<Bucket>& pool =
        static_cast<double>(coloured_count) >= kColouredShare * static_cast<double>(opaque) ? coloured : grey;
    const auto best = std::max_element(pool.begin(), pool.end(),
                                       [](const Bucket& a, const Bucket& b) { return a.weight < b.weight; });
    if (best->weight <= 0.0) return fallback;
    return {static_cast<uint8_t>(std::lround(best->r / best->weight)),
            static_cast<uint8_t>(std::lround(best->g / best->weight)),
            static_cast<uint8_t>(std::lround(best->b / best->weight))};
}

ThemeScanResult ScanUserThemes(const fs::path& dir) {
    ThemeScanResult result;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return result;

    std::vector<fs::path> folders;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_directory(type_ec)) folders.push_back(it->path());
    }
    if (ec) result.notes.push_back("themes folder could not be read: " + ec.message());
    std::sort(folders.begin(), folders.end());

    for (const fs::path& folder : folders) {
        const std::string folder_name = PathToUtf8(folder.filename());
        if (result.themes.size() >= kMaxUserThemes) {
            result.notes.push_back("theme folder \"" + folder_name + "\" skipped: only " +
                                   std::to_string(kMaxUserThemes) + " user themes are read");
            continue;
        }
        const std::string id = UserThemeId(folder_name);
        if (id.empty()) {
            result.notes.push_back("theme folder \"" + folder_name +
                                   "\" skipped: use up to 48 letters, digits, spaces, - or _");
            continue;
        }
        const bool duplicate = std::any_of(result.themes.begin(), result.themes.end(),
                                           [&](const ThemeTokens& t) { return t.id == id; });
        if (duplicate) {
            result.notes.push_back("theme folder \"" + folder_name + "\" skipped: another folder has the id " + id);
            continue;
        }
        const fs::path json = folder / "theme.json";
        std::error_code exists_ec;
        if (!fs::is_regular_file(json, exists_ec)) {
            result.notes.push_back("theme folder \"" + folder_name + "\" skipped: it has no theme.json");
            continue;
        }
        std::string text;
        std::string error;
        ThemeTokens theme;
        if (!ReadSmallFile(json, kMaxThemeJsonBytes, text, error) ||
            !ParseThemeJson(text, id, folder_name, theme, result.notes, error)) {
            result.notes.push_back("theme " + id + " skipped: " + error);
            continue;
        }
        if (!theme.backdrop_path.empty()) {
            const fs::path image = folder / PathFromUtf8(theme.backdrop_path);
            std::error_code size_ec;
            const bool regular = fs::is_regular_file(image, size_ec);
            const uintmax_t size = regular ? fs::file_size(image, size_ec) : 0;
            if (!regular || size_ec) {
                result.notes.push_back("theme " + id + ": backdrop " + theme.backdrop_path + " not found, none used");
                theme.backdrop_path.clear();
                theme.accent_from_art = false;
            } else if (size > kMaxBackdropBytes) {
                result.notes.push_back("theme " + id + ": backdrop is larger than 32 MiB, none used");
                theme.backdrop_path.clear();
                theme.accent_from_art = false;
            } else {
                theme.backdrop_path = PathToUtf8(image);
            }
        }
        if (!theme.accent_from_art) {
            std::string note;
            if (!FinishTheme(theme, note, error)) {
                result.notes.push_back("theme " + id + " skipped: " + error);
                continue;
            }
            if (!note.empty()) result.notes.push_back("theme " + id + ": " + note);
        }
        result.themes.push_back(theme);
    }
    const auto folded = [](const std::string& text) {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    std::sort(result.themes.begin(), result.themes.end(), [&](const ThemeTokens& a, const ThemeTokens& b) {
        const std::string fa = folded(a.display_name);
        const std::string fb = folded(b.display_name);
        return fa != fb ? fa < fb : a.id < b.id;
    });
    return result;
}

}  // namespace gdx_companion
