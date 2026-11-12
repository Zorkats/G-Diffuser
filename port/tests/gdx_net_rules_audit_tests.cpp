// port/tests/gdx_net_rules_audit_tests.cpp -- keeps the room's lock table in step with the source (leftover L1).
// Copyright (c) Zorkats
//
// Usage: gdx_net_rules_audit_tests <source root>
//
// The test reads every C and C++ file under decomp/src and port (not port/tests, port/build or
// port/third_party) and collects the names passed to CVarGet* as a string literal. It fails when:
//   - a name under a gameplay namespace is read anywhere and the table has no row for it;
//   - any name is read in racer, CPU or course code (the sim files below) and the table has no row;
//   - a SCANNED row is no longer passed to CVarGet* as a literal, or a BY_HAND row no longer appears as a
//     string literal at all (the row is stale), or a BY_HAND row is now a CVarGet* literal (mark it SCANNED);
//   - a GRID row says O3 carries it but the host's rules capture in gdx_lan_lobby.cpp does not name it.
// It prints the BY_HAND rows and the GRID rows a later slice still has to carry.

#include "gdx_net_rules_audit.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool StartsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Racer, CPU and course code: a setting read here can change a race, whatever its namespace.
bool IsSimFile(const std::string& rel) {
    static const char* const kExact[] = {
        "decomp/src/overlays/ovl_i3/cpu.c", "port/gdx_course_bounds.c",   "port/gdx_course_meta_view.c",
        "port/gdx_course_settings_view.c",  "port/gdx_single_race.c",     "port/gdx_death_race.c",
        "port/gdx_steering_curve.c",        "port/gdx_custom_grid.c",
    };
    for (const char* exact : kExact) {
        if (rel == exact) {
            return true;
        }
    }
    if (StartsWith(rel, "decomp/src/game/") || StartsWith(rel, "port/enhancements/")) {
        return true;
    }
    return StartsWith(rel, "port/gdx_course_cpu") && EndsWith(rel, ".c");
}

bool IsSkipped(const std::string& rel) {
    return StartsWith(rel, "port/tests/") || StartsWith(rel, "port/build/") || StartsWith(rel, "port/third_party/") ||
           rel == "port/gdx_net_rules_audit.c"; // the table itself names every row
}

bool IsSourceFile(const fs::path& path) {
    const std::string ext = path.extension().string();
    return ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".hpp";
}

bool IsIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The names passed to CVarGet*( "..." ) in one file's text. A name passed any other way is not seen.
std::vector<std::string> CVarGetLiterals(const std::string& text) {
    std::vector<std::string> names;
    static const char kCall[] = "CVarGet";
    size_t at = 0;
    while ((at = text.find(kCall, at)) != std::string::npos) {
        size_t i = at + sizeof(kCall) - 1;
        const bool wordStart = at == 0 || !IsIdentChar(text[at - 1]);
        at = i;
        if (!wordStart) {
            continue;
        }
        while (i < text.size() && IsIdentChar(text[i])) {
            i++;
        }
        while (i < text.size() && IsSpace(text[i])) {
            i++;
        }
        if (i >= text.size() || text[i] != '(') {
            continue;
        }
        i++;
        while (i < text.size() && IsSpace(text[i])) {
            i++;
        }
        if (i >= text.size() || text[i] != '"') {
            continue;
        }
        const size_t end = text.find('"', i + 1);
        if (end == std::string::npos) {
            break;
        }
        names.push_back(text.substr(i + 1, end - i - 1));
    }
    return names;
}

struct Source {
    std::string rel;
    std::string text;
};

std::vector<Source> LoadSources(const fs::path& root) {
    std::vector<Source> sources;
    for (const char* top : { "decomp/src", "port" }) {
        const fs::path dir = root / top;
        if (!fs::is_directory(dir)) {
            continue;
        }
        for (const auto& item : fs::recursive_directory_iterator(dir)) {
            if (!item.is_regular_file() || !IsSourceFile(item.path())) {
                continue;
            }
            const std::string rel = fs::relative(item.path(), root).generic_string();
            if (IsSkipped(rel)) {
                continue;
            }
            sources.push_back({ rel, ReadFile(item.path()) });
        }
    }
    return sources;
}

void CheckTableShape() {
    std::set<std::string> seen;
    const int count = gdx_net_rules_audit_count();
    Check(count > 0, "the table has rows");
    Check(gdx_net_rules_audit_entry(-1) == nullptr && gdx_net_rules_audit_entry(count) == nullptr,
          "an index outside the table gives NULL");
    for (int i = 0; i < count; i++) {
        const GdxNetRuleEntry* e = gdx_net_rules_audit_entry(i);
        const std::string name = e->name != nullptr ? e->name : "";
        Check(!name.empty(), "row " + std::to_string(i) + " has a name");
        Check(seen.insert(name).second, name + " is in the table once");
        Check(e->cls >= GDX_NET_RULE_GRID && e->cls < GDX_NET_RULE_CLASS_COUNT, name + " has a class");
        Check(e->why != nullptr && e->why[0] != '\0', name + " says why");
        const bool carriedClass = e->cls == GDX_NET_RULE_GRID || e->cls == GDX_NET_RULE_HOST_CPU;
        if (carriedClass) {
            Check(e->carried != nullptr && e->carried[0] != '\0', name + " names the slice that carries it");
        } else {
            Check(e->carried == nullptr, name + " is not carried, so it names no slice");
        }
        Check(gdx_net_rules_audit_find(name.c_str()) == e, name + " is found by name");
    }
    Check(gdx_net_rules_audit_find(nullptr) == nullptr, "find(NULL) is NULL");
    Check(gdx_net_rules_audit_find("gEnhancements.Gameplay.NoSuchSetting") == nullptr, "an unknown name is not found");
    Check(gdx_net_rules_audit_is_gameplay_name("gEnhancements.Tuning.BoostDuration") == 1, "Tuning is gameplay");
    Check(gdx_net_rules_audit_is_gameplay_name("gEnhancements.Hacks.Active") == 1, "Hacks is gameplay");
    Check(gdx_net_rules_audit_is_gameplay_name("gEnhancements.Graphics.FancyLighting") == 0, "Graphics is not");
    Check(gdx_net_rules_audit_is_gameplay_name("gEnhancements.Gameplay") == 0, "a namespace alone is not a name");
    Check(gdx_net_rules_audit_is_gameplay_name(nullptr) == 0, "NULL is not gameplay");
    Check(std::strcmp(gdx_net_rules_audit_class_name(GDX_NET_RULE_NOT_IN_ROOM), "not in room") == 0,
          "class names read as words");
}

void CheckScanner() {
    const std::vector<std::string> got = CVarGetLiterals(
        "x = CVarGetInteger(\"a.B\", 0); y = CVarGetFloat (\n  \"c.D\", 1); z = CVarGetString(kName, \"\");"
        " MyCVarGetInteger(\"e.F\", 0); CVarGetInteger");
    Check(got.size() == 2 && got[0] == "a.B" && got[1] == "c.D",
          "the scanner reads literal names, across a line break, and skips variables and other functions");
}

void CheckAgainstSource(const fs::path& root) {
    Check(fs::is_directory(root / "decomp/src") && fs::is_directory(root / "port"),
          "the source root has decomp/src and port: " + root.string());
    const std::vector<Source> sources = LoadSources(root);
    Check(sources.size() > 100, "the scan read the source tree (" + std::to_string(sources.size()) + " files)");

    std::map<std::string, std::string> readAt; // a CVarGet literal name -> the first file that reads it
    std::map<std::string, std::string> simReadAt;
    size_t reads = 0;
    for (const Source& src : sources) {
        for (const std::string& name : CVarGetLiterals(src.text)) {
            reads++;
            readAt.emplace(name, src.rel);
            if (IsSimFile(src.rel)) {
                simReadAt.emplace(name, src.rel);
            }
        }
    }
    // A floor, not a count: 199 literal reads on 2026-11-12. It only proves the tree was really read.
    Check(reads > 150, "the scan found the CVar reads (" + std::to_string(reads) + ")");

    for (const auto& [name, file] : readAt) {
        if (gdx_net_rules_audit_is_gameplay_name(name.c_str())) {
            Check(gdx_net_rules_audit_find(name.c_str()) != nullptr,
                  name + " (gameplay, read in " + file + ") is classified");
        }
    }
    for (const auto& [name, file] : simReadAt) {
        Check(gdx_net_rules_audit_find(name.c_str()) != nullptr, name + " (read in " + file + ") is classified");
    }

    std::string lobby;
    for (const Source& src : sources) {
        if (src.rel == "port/gdx_lan_lobby.cpp") {
            lobby = src.text;
        }
    }
    Check(!lobby.empty(), "port/gdx_lan_lobby.cpp was read");

    for (int i = 0; i < gdx_net_rules_audit_count(); i++) {
        const GdxNetRuleEntry* e = gdx_net_rules_audit_entry(i);
        const std::string name = e->name;
        const bool scanned = readAt.count(name) != 0;
        if (e->seen == GDX_NET_RULE_SCANNED) {
            Check(scanned, name + " is still read through CVarGet* (else the row is stale)");
        } else {
            const std::string quoted = "\"" + name + "\"";
            bool named = false;
            for (const Source& src : sources) {
                if (src.text.find(quoted) != std::string::npos) {
                    named = true;
                    break;
                }
            }
            Check(named, name + " still appears in the source (else the row is stale)");
            Check(!scanned, name + " is a CVarGet* literal now, so its row should say SCANNED");
            std::printf("not seen by the scan, listed by hand: %s\n", name.c_str());
        }
        if (e->cls == GDX_NET_RULE_GRID) {
            // The milestones that have shipped: their rows must be carried by the host's rules today.
            if (std::strcmp(e->carried, "O3") == 0 || std::strcmp(e->carried, "O4.4") == 0) {
                Check(lobby.find("\"" + name + "\"") != std::string::npos,
                      name + " is carried by the host's grid rules (gdx_lan_lobby.cpp)");
            } else {
                std::printf("grid rule not carried yet, waits for %s: %s\n", e->carried, name.c_str());
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: gdx_net_rules_audit_tests <source root>\n");
        return 2;
    }
    CheckTableShape();
    CheckScanner();
    CheckAgainstSource(fs::path(argv[1]));
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
