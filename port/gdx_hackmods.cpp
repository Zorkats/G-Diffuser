/* port/gdx_hackmods.cpp -- ROM-hack mod discovery, selection and save isolation.
 *
 * Contract and rationale live in gdx_hackmods.h.
 *
 * The name helpers are deliberately plain C with no filesystem access, because they are the part
 * with a real failure mode: a hack basename becomes part of a save filename, so anything that
 * could escape saves/ has to die here. They are unit-tested standalone
 * (port/tests/gdx_hackmods_tests.cpp).
 */

#include "gdx_hackmods.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "port_log.h" // gdx_port_logf is a static inline here, not an exported symbol

// The CVar bridge is declared rather than included so this TU stays free of libultraship and can
// be compiled unmodified by the standalone harness, which supplies its own definitions. Same
// pattern the decomp-side port hooks use (decomp/src/game/course.c:4592).
extern "C" {
const char* CVarGetString(const char* name, const char* defaultValue);
void CVarSetString(const char* name, const char* value);
void CVarSave();
}

namespace {

const char* kCVarActive = "gEnhancements.Hacks.Active";

std::string sHackDir;
std::string sProgramDir;
std::string sLatchedName;
char sLatchedSave[GDX_HACKMOD_SAVE_MAX] = GDX_HACKMOD_SAVE_STOCK;
bool sLatched = false;

bool IsKeptNameChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

} // namespace

extern "C" int gdx_hackmod_sanitize_name(const char* in, char* out, unsigned long outCap) {
    if (out == nullptr || outCap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == nullptr) {
        return 0;
    }

    // "." and ".." are the two names that mean something to a filesystem rather than naming a
    // file, so they are refused before any character-level work.
    if (std::strcmp(in, ".") == 0 || std::strcmp(in, "..") == 0) {
        return 0;
    }

    unsigned long limit = outCap - 1;
    if (limit > GDX_HACKMOD_NAME_MAX - 1) {
        limit = GDX_HACKMOD_NAME_MAX - 1;
    }

    unsigned long n = 0;
    bool lastWasDash = false;
    for (const char* p = in; *p != '\0' && n < limit; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (IsKeptNameChar(c)) {
            out[n++] = static_cast<char>(c);
            lastWasDash = false;
            continue;
        }
        // Everything else -- separators, spaces, control bytes, anything non-ASCII -- collapses to
        // a single dash. That covers '/', '\\' and ':' without needing to enumerate them.
        if (!lastWasDash && n > 0) {
            out[n++] = '-';
            lastWasDash = true;
        }
    }
    out[n] = '\0';

    // Trim trailing separators, and any leading or trailing dots, so the result can never be a
    // relative path fragment or a hidden file.
    while (n > 0 && (out[n - 1] == '-' || out[n - 1] == '.')) {
        out[--n] = '\0';
    }
    unsigned long start = 0;
    while (out[start] == '-' || out[start] == '.') {
        start++;
    }
    if (start > 0) {
        std::memmove(out, out + start, n - start + 1);
        n -= start;
    }

    return (n > 0) ? 1 : 0;
}

extern "C" int gdx_hackmod_save_basename(const char* hackName, char* out, unsigned long outCap) {
    if (out == nullptr || outCap == 0) {
        return 0;
    }
    const unsigned long stockLen = static_cast<unsigned long>(std::strlen(GDX_HACKMOD_SAVE_STOCK));
    if (outCap <= stockLen) {
        out[0] = '\0';
        return 0;
    }

    if (hackName == nullptr || hackName[0] == '\0') {
        std::memcpy(out, GDX_HACKMOD_SAVE_STOCK, stockLen + 1);
        return 1;
    }

    char clean[GDX_HACKMOD_NAME_MAX];
    if (!gdx_hackmod_sanitize_name(hackName, clean, sizeof(clean))) {
        std::memcpy(out, GDX_HACKMOD_SAVE_STOCK, stockLen + 1);
        return 0;
    }

    const unsigned long needed = static_cast<unsigned long>(std::strlen(GDX_HACKMOD_SAVE_PREFIX) +
                                                            std::strlen(clean) + std::strlen(".sav"));
    if (needed >= outCap) {
        // Falling back to the stock name here would silently share the stock save with a hack, so
        // the caller is told instead and gets a safe-but-wrong-on-purpose value it must check.
        std::memcpy(out, GDX_HACKMOD_SAVE_STOCK, stockLen + 1);
        return 0;
    }

    std::strcpy(out, GDX_HACKMOD_SAVE_PREFIX);
    std::strcat(out, clean);
    std::strcat(out, ".sav");
    return 1;
}

extern "C" const char* gdx_hackmod_active_save_basename(void) {
    return sLatchedSave;
}

void GdxHackModsSetDirectory(const std::string& dir) {
    sHackDir = dir;
}

const std::string& GdxHackModsDirectory() {
    return sHackDir;
}

void GdxHackModsSetProgramDir(const std::string& dir) {
    sProgramDir = dir;
}

const std::string& GdxHackModsProgramDir() {
    return sProgramDir;
}

std::vector<GdxHackModEntry> GdxHackModsScan() {
    std::vector<GdxHackModEntry> out;
    if (sHackDir.empty()) {
        return out;
    }
    std::error_code ec;
    std::filesystem::path dir(sHackDir);
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    const std::string selected = GdxHackModsSelectedName();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".o2r") {
            continue;
        }
        GdxHackModEntry row;
        row.basename = entry.path().stem().string();
        row.path = std::filesystem::absolute(entry.path(), ec).string();
        row.active = (row.basename == selected);
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(),
              [](const GdxHackModEntry& a, const GdxHackModEntry& b) { return a.basename < b.basename; });
    return out;
}

std::string GdxHackModsSelectedName() {
    const char* v = CVarGetString(kCVarActive, "");
    return (v != nullptr) ? std::string(v) : std::string();
}

void GdxHackModsSetSelected(const std::string& basename) {
    CVarSetString(kCVarActive, basename.c_str());
    CVarSave();
}

std::string GdxHackModsLatchActivePath() {
    // Latch once. A second call would let a mid-session CVar edit move the save file out from
    // under a running game.
    if (sLatched) {
        return sLatchedName.empty() ? std::string() : (sHackDir + "/" + sLatchedName + ".o2r");
    }
    sLatched = true;
    sLatchedName.clear();
    std::memcpy(sLatchedSave, GDX_HACKMOD_SAVE_STOCK, std::strlen(GDX_HACKMOD_SAVE_STOCK) + 1);

    const std::string selected = GdxHackModsSelectedName();
    if (selected.empty() || sHackDir.empty()) {
        return std::string();
    }

    // Resolve through the scan rather than by string concatenation, so the selection can only ever
    // name an archive that actually exists in the hack directory.
    for (const GdxHackModEntry& row : GdxHackModsScan()) {
        if (row.basename != selected) {
            continue;
        }
        char save[GDX_HACKMOD_SAVE_MAX];
        if (!gdx_hackmod_save_basename(row.basename.c_str(), save, sizeof(save))) {
            gdx_port_logf("[hackmods] '%s' has no usable save name; refusing to mount it rather than "
                          "sharing the stock save.\n",
                          row.basename.c_str());
            return std::string();
        }
        sLatchedName = row.basename;
        std::memcpy(sLatchedSave, save, std::strlen(save) + 1);
        gdx_port_logf("[hackmods] active: %s (save: %s)\n", row.basename.c_str(), sLatchedSave);
        return row.path;
    }

    gdx_port_logf("[hackmods] selected hack '%s' is not in %s; booting stock.\n", selected.c_str(),
                  sHackDir.c_str());
    return std::string();
}

std::string GdxHackModsActiveName() {
    return sLatchedName;
}
