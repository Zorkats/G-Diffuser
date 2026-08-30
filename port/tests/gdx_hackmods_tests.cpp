/* Standalone unit-test harness for the name helpers in port/gdx_hackmods.cpp.
 *
 * Those two functions are the only part of hack-mod handling with a real failure mode: a hack
 * archive's basename becomes part of a save filename, so anything that could escape saves/ or
 * collide with the stock save has to be stopped there. The filesystem scan and the CVar-backed
 * selection are not covered here; they need a mounted install and are covered by the manual test
 * plan in devdocs/1.2.0-import-and-limits-scope.md.
 *
 * gdx_hackmods.cpp compiles UNMODIFIED here: it declares its CVar and logging dependencies rather
 * than including libultraship, so the stubs below satisfy the link.
 */
#include <cstdio>
#include <cstring>

#include "../gdx_hackmods.h"

#include "../gdx_dev_gates.h" // GDX_GATE_COUNT, for the two symbols port_log.h's inline needs

extern "C" {
// A selection is never set in this harness; the name helpers under test do not read it.
const char* CVarGetString(const char*, const char* defaultValue) {
    return defaultValue;
}
void CVarSetString(const char*, const char*) {
}
void CVarSave() {
}
// gdx_port_logf is a static inline in port_log.h, so it needs no stub, but it does reach these
// two: the gate cache it consults and the optional console tap. Both are inert here.
int gGdxDevGateCache[GDX_GATE_COUNT] = {};
void (*gdx_port_log_tap)(const char* message) = nullptr;
}

static int gChecks = 0;
static int gFails = 0;

static void checkStr(const char* what, const char* got, const char* expected) {
    gChecks++;
    if (std::strcmp(got, expected) != 0) {
        gFails++;
        std::printf("    [x] %s: got \"%s\", expected \"%s\"\n", what, got, expected);
    }
}

static void checkInt(const char* what, int got, int expected) {
    gChecks++;
    if (got != expected) {
        gFails++;
        std::printf("    [x] %s: got %d, expected %d\n", what, got, expected);
    }
}

static void CaseSanitizeKeepsOrdinaryNames() {
    char out[GDX_HACKMOD_NAME_MAX];
    checkInt("plain name accepted", gdx_hackmod_sanitize_name("fzerox-hack-newlap", out, sizeof(out)), 1);
    checkStr("plain name unchanged", out, "fzerox-hack-newlap");

    checkInt("mixed case kept", gdx_hackmod_sanitize_name("New_Lap.v2", out, sizeof(out)), 1);
    checkStr("mixed case unchanged", out, "New_Lap.v2");

    checkInt("digits kept", gdx_hackmod_sanitize_name("climax2024", out, sizeof(out)), 1);
    checkStr("digits unchanged", out, "climax2024");
}

static void CaseSanitizeCollapsesSeparators() {
    char out[GDX_HACKMOD_NAME_MAX];
    checkInt("spaces accepted", gdx_hackmod_sanitize_name("F-Zero X New Lap", out, sizeof(out)), 1);
    checkStr("spaces become single dashes", out, "F-Zero-X-New-Lap");

    checkInt("runs accepted", gdx_hackmod_sanitize_name("a   b", out, sizeof(out)), 1);
    checkStr("runs collapse", out, "a-b");

    checkInt("non-ascii accepted", gdx_hackmod_sanitize_name("caf\xC3\xA9 hack", out, sizeof(out)), 1);
    checkStr("non-ascii collapses", out, "caf-hack");
}

/* The whole point of the helper: nothing that names a directory or climbs out of one survives. */
static void CaseSanitizeBlocksTraversal() {
    char out[GDX_HACKMOD_NAME_MAX];

    checkInt("dot refused", gdx_hackmod_sanitize_name(".", out, sizeof(out)), 0);
    checkStr("dot yields empty", out, "");
    checkInt("dotdot refused", gdx_hackmod_sanitize_name("..", out, sizeof(out)), 0);
    checkStr("dotdot yields empty", out, "");

    checkInt("posix traversal accepted as text", gdx_hackmod_sanitize_name("../../etc/passwd", out, sizeof(out)), 1);
    checkStr("posix traversal defanged", out, "etc-passwd");

    checkInt("windows traversal accepted as text",
             gdx_hackmod_sanitize_name("..\\..\\Windows\\System32", out, sizeof(out)), 1);
    checkStr("windows traversal defanged", out, "Windows-System32");

    checkInt("drive letter accepted as text", gdx_hackmod_sanitize_name("C:\\saves\\evil", out, sizeof(out)), 1);
    checkStr("drive letter defanged", out, "C-saves-evil");

    checkInt("leading dot stripped", gdx_hackmod_sanitize_name(".hidden", out, sizeof(out)), 1);
    checkStr("leading dot gone", out, "hidden");

    checkInt("trailing dot stripped", gdx_hackmod_sanitize_name("name.", out, sizeof(out)), 1);
    checkStr("trailing dot gone", out, "name");

    checkInt("separators only refused", gdx_hackmod_sanitize_name("///", out, sizeof(out)), 0);
    checkStr("separators only yields empty", out, "");

    checkInt("empty refused", gdx_hackmod_sanitize_name("", out, sizeof(out)), 0);
    checkInt("null refused", gdx_hackmod_sanitize_name(nullptr, out, sizeof(out)), 0);
}

static void CaseSanitizeRespectsBuffer() {
    char small[8];
    checkInt("truncates into a small buffer", gdx_hackmod_sanitize_name("abcdefghijklmnop", small, sizeof(small)), 1);
    checkStr("truncated to fit", small, "abcdefg");

    char tiny[1];
    checkInt("one-byte buffer refused", gdx_hackmod_sanitize_name("abc", tiny, sizeof(tiny)), 0);
    checkInt("zero cap refused", gdx_hackmod_sanitize_name("abc", small, 0), 0);
}

static void CaseSaveBasename() {
    char out[GDX_HACKMOD_SAVE_MAX];

    checkInt("no hack -> stock", gdx_hackmod_save_basename(nullptr, out, sizeof(out)), 1);
    checkStr("stock name", out, "fzerox.sav");
    checkInt("empty hack -> stock", gdx_hackmod_save_basename("", out, sizeof(out)), 1);
    checkStr("stock name", out, "fzerox.sav");

    checkInt("named hack", gdx_hackmod_save_basename("newlap", out, sizeof(out)), 1);
    checkStr("hack save name", out, "fzerox-hack-newlap.sav");

    checkInt("dirty name sanitised", gdx_hackmod_save_basename("New Lap!", out, sizeof(out)), 1);
    checkStr("dirty name sanitised", out, "fzerox-hack-New-Lap.sav");

    // A traversal attempt must never produce a path; it must fail loudly and leave a safe value.
    checkInt("traversal refused", gdx_hackmod_save_basename("..", out, sizeof(out)), 0);
    checkStr("traversal falls back to stock", out, "fzerox.sav");

    // A name that cannot fit must report failure rather than silently sharing the stock save.
    char small[16];
    checkInt("overlong refused", gdx_hackmod_save_basename("averylonghacknameindeed", small, sizeof(small)), 0);
    checkStr("overlong falls back to stock", small, "fzerox.sav");

    // The hack save must never collide with the stock one.
    checkInt("stock-shaped name still namespaced", gdx_hackmod_save_basename("fzerox", out, sizeof(out)), 1);
    checkStr("no collision with the stock save", out, "fzerox-hack-fzerox.sav");
}

static void CaseActiveSaveDefaultsToStock() {
    // Nothing latched in this harness, so the accessor must still return a usable name.
    checkStr("unlatched active save", gdx_hackmod_active_save_basename(), "fzerox.sav");
}

int main() {
    struct {
        const char* name;
        void (*fn)();
    } cases[] = {
        { "ordinary names pass through", CaseSanitizeKeepsOrdinaryNames },
        { "separators collapse to single dashes", CaseSanitizeCollapsesSeparators },
        { "path traversal cannot survive", CaseSanitizeBlocksTraversal },
        { "buffer limits respected", CaseSanitizeRespectsBuffer },
        { "save basenames", CaseSaveBasename },
        { "active save defaults to stock", CaseActiveSaveDefaultsToStock },
    };
    const int numCases = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    int failedCases = 0;

    std::printf("=== G-Diffuser hack-mod name helper harness (port/gdx_hackmods.cpp) ===\n\n");
    for (int i = 0; i < numCases; i++) {
        const int failsBefore = gFails;
        const int checksBefore = gChecks;
        std::printf("-- %s\n", cases[i].name);
        cases[i].fn();
        if (gFails > failsBefore) {
            failedCases++;
            std::printf("[FAIL] %s (%d/%d sub-checks failed)\n\n", cases[i].name, gFails - failsBefore,
                        gChecks - checksBefore);
        } else {
            std::printf("[PASS] %s (%d sub-checks)\n\n", cases[i].name, gChecks - checksBefore);
        }
    }
    std::printf("=== Summary: %d/%d cases passed (%d/%d sub-checks passed) ===\n", numCases - failedCases,
                numCases, gChecks - gFails, gChecks);
    return failedCases ? 1 : 0;
}
