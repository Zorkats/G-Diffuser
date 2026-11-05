// port/tests/gdx_profile_state_tests.cpp: checks for the live profile store (gdx_profile_state.cpp) and the
// CM1 rule it feeds: the active profile's name becomes a saved track's Author, and another profile's save sets
// Edited by. Links the real gdx_profile.c and gdx_course_meta.c. Exit code 0 when every check passes.

#include "gdx_course_meta.h"
#include "gdx_profile_state.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int sFailures = 0;

void check(bool ok, const char* text) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) {
        sFailures++;
    }
}

const GdxProfileStore* store() {
    return gdx_profile_state_store();
}

bool name_is(const char* expected) {
    const char* name = gdx_profile_state_active_name();
    return name != nullptr && std::strcmp(name, expected) == 0;
}

bool id_zero(const GdxProfile& p) {
    for (unsigned char b : p.id) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

void remove_all() {
    while (store()->count > 0) {
        gdx_profile_state_remove(0);
    }
}

void test_empty() {
    check(store()->count == 0, "empty: no profile at start");
    check(gdx_profile_state_active_name() == nullptr, "empty: no active name");
}

void test_create() {
    const unsigned long r0 = gdx_profile_state_revision();
    const int a = gdx_profile_state_create("  Zor  ", 0x112233u, 4);
    check(a == 0, "create: first profile at index 0");
    check(gdx_profile_state_revision() != r0, "create: revision moves");
    check(name_is("Zor"), "create: spaces trimmed, first profile active");
    check(!id_zero(store()->profiles[0]), "create: id is not all zero");
    check(store()->profiles[0].iconKind == GDX_PROFILE_ICON_INITIALS && store()->profiles[0].iconIndex == 0,
          "create: initials icon");
    check(store()->profiles[0].colour == 0x112233u && store()->profiles[0].preferredMachine == 4,
          "create: colour and machine kept");

    const unsigned long r1 = gdx_profile_state_revision();
    const char* badNames[] = { "", "   ", "Zo\xC3\xA9", "ABCDEFGHIJKLMNOPQ", "Tab\tName" };
    bool allRejected = true;
    for (const char* bad : badNames) {
        allRejected = allRejected && gdx_profile_state_create(bad, 0, -1) == GDX_PROFILE_ERR_VALUE;
    }
    check(allRejected, "create: empty, blank, non-ASCII, 17-character and tab names are VALUE");
    check(gdx_profile_state_create("Kai", 0x1000000u, -1) == GDX_PROFILE_ERR_VALUE, "create: colour over 24 bits");
    check(gdx_profile_state_create("Kai", 0, GDX_PROFILE_ROSTER_COUNT) == GDX_PROFILE_ERR_VALUE,
          "create: machine past the roster");
    check(gdx_profile_state_create("Kai", 0, -2) == GDX_PROFILE_ERR_VALUE, "create: machine below -1");
    check(gdx_profile_state_create(nullptr, 0, -1) < 0, "create: NULL name refused");
    check(store()->count == 1 && gdx_profile_state_revision() == r1, "create: refused creates change nothing");

    check(gdx_profile_state_create("Kai", 0, -1) == 1, "create: second profile at index 1");
    check(name_is("Zor"), "create: active profile unchanged by a second create");
    check(std::memcmp(store()->profiles[0].id, store()->profiles[1].id, GDX_PROFILE_ID_BYTES) != 0,
          "create: ids differ");
}

void test_edits() {
    unsigned long r = gdx_profile_state_revision();
    check(gdx_profile_state_rename(1, " Kai2 ") == GDX_PROFILE_OK && std::strcmp(store()->profiles[1].name, "Kai2") == 0,
          "rename: trimmed and stored");
    check(gdx_profile_state_revision() != r, "rename: revision moves");
    r = gdx_profile_state_revision();
    check(gdx_profile_state_rename(1, "Kai2") == GDX_PROFILE_OK && gdx_profile_state_revision() == r,
          "rename: same name is OK and no edit");
    check(gdx_profile_state_rename(1, "") == GDX_PROFILE_ERR_VALUE &&
              std::strcmp(store()->profiles[1].name, "Kai2") == 0 && gdx_profile_state_revision() == r,
          "rename: bad name leaves the profile");
    check(gdx_profile_state_rename(5, "Ok") == GDX_PROFILE_ERR_ARGS, "rename: index past the store is ARGS");
    check(gdx_profile_state_rename(-1, "Ok") == GDX_PROFILE_ERR_ARGS, "rename: negative index is ARGS");

    check(gdx_profile_state_set_colour(1, 0x1000000u) == GDX_PROFILE_ERR_VALUE &&
              store()->profiles[1].colour == 0 && gdx_profile_state_revision() == r,
          "colour: over 24 bits refused, colour kept");
    check(gdx_profile_state_set_colour(1, 0xABCDEFu) == GDX_PROFILE_OK && store()->profiles[1].colour == 0xABCDEFu &&
              gdx_profile_state_revision() != r,
          "colour: stored, revision moves");
    r = gdx_profile_state_revision();
    check(gdx_profile_state_set_colour(1, 0xABCDEFu) == GDX_PROFILE_OK && gdx_profile_state_revision() == r,
          "colour: same colour is no edit");
    check(gdx_profile_state_set_colour(9, 0) == GDX_PROFILE_ERR_ARGS, "colour: bad index is ARGS");

    check(gdx_profile_state_set_machine(1, -2) == GDX_PROFILE_ERR_VALUE && store()->profiles[1].preferredMachine == -1,
          "machine: -2 refused, machine kept");
    check(gdx_profile_state_set_machine(1, GDX_PROFILE_ROSTER_COUNT) == GDX_PROFILE_ERR_VALUE,
          "machine: roster count refused");
    check(gdx_profile_state_set_machine(1, GDX_PROFILE_ROSTER_COUNT - 1) == GDX_PROFILE_OK &&
              store()->profiles[1].preferredMachine == GDX_PROFILE_ROSTER_COUNT - 1,
          "machine: last roster entry stored");
    check(gdx_profile_state_revision() != r, "machine: revision moves");

    r = gdx_profile_state_revision();
    check(gdx_profile_state_set_active(1) == GDX_PROFILE_OK && name_is("Kai2") && gdx_profile_state_revision() != r,
          "active: switched, revision moves");
    r = gdx_profile_state_revision();
    check(gdx_profile_state_set_active(1) == GDX_PROFILE_OK && gdx_profile_state_revision() == r,
          "active: same profile is no edit");
    check(gdx_profile_state_set_active(7) == GDX_PROFILE_ERR_ARGS && name_is("Kai2"), "active: bad index is ARGS");
}

void test_file() {
    std::vector<unsigned char> bytes(GDX_PROFILE_FILE_MAX_BYTES);
    const int n = gdx_profile_state_encode_file(bytes.data(), bytes.size());
    check(n > 0, "file: store encodes");
    bytes.resize(n > 0 ? static_cast<size_t>(n) : 0);

    std::vector<unsigned char> broken = bytes;
    broken[broken.size() / 2] ^= 0x40;
    const unsigned long r = gdx_profile_state_revision();
    check(gdx_profile_state_restore_file(broken.data(), broken.size()) < 0, "file: a flipped byte is refused");
    check(store()->count == 2 && name_is("Kai2") && gdx_profile_state_revision() == r,
          "file: a refused file leaves the store and revision");
    check(gdx_profile_state_restore_file(bytes.data(), bytes.size() - 1) < 0, "file: a short file is refused");

    gdx_profile_state_remove(0);
    check(store()->count == 1, "file: store edited after encoding");
    check(gdx_profile_state_restore_file(bytes.data(), bytes.size()) == GDX_PROFILE_OK, "file: good file restores");
    check(store()->count == 2 && name_is("Kai2") && std::strcmp(store()->profiles[0].name, "Zor") == 0 &&
              store()->profiles[1].colour == 0xABCDEFu &&
              store()->profiles[1].preferredMachine == GDX_PROFILE_ROSTER_COUNT - 1,
          "file: every field comes back");
    check(gdx_profile_state_revision() != r, "file: restore moves the revision");
}

void test_capacity() {
    char name[8];
    std::vector<std::vector<unsigned char>> ids;
    for (int i = store()->count; i < GDX_PROFILE_CAPACITY; i++) {
        std::snprintf(name, sizeof name, "P%d", i);
        check(gdx_profile_state_create(name, 0, -1) == i, "capacity: create fills the next slot");
    }
    check(gdx_profile_state_create("Extra", 0, -1) == GDX_PROFILE_ERR_FULL, "capacity: ninth profile is FULL");
    bool distinct = true;
    for (int i = 0; i < store()->count; i++) {
        for (int j = i + 1; j < store()->count; j++) {
            distinct = distinct &&
                       std::memcmp(store()->profiles[i].id, store()->profiles[j].id, GDX_PROFILE_ID_BYTES) != 0;
        }
    }
    check(distinct, "capacity: all eight ids differ");

    std::vector<unsigned char> bytes(GDX_PROFILE_FILE_MAX_BYTES);
    const int n = gdx_profile_state_encode_file(bytes.data(), bytes.size());
    check(n > 0 && gdx_profile_state_restore_file(bytes.data(), static_cast<size_t>(n)) == GDX_PROFILE_OK,
          "capacity: a full store fits the file limit and restores");

    check(gdx_profile_state_set_active(1) == GDX_PROFILE_OK, "remove: active set to Kai2");
    check(gdx_profile_state_remove(1) == GDX_PROFILE_OK && name_is("P2"),
          "remove: the profile taking the active slot becomes active");
    check(gdx_profile_state_remove(GDX_PROFILE_CAPACITY) == GDX_PROFILE_ERR_ARGS, "remove: bad index is ARGS");
    remove_all();
    check(store()->count == 0 && gdx_profile_state_active_name() == nullptr, "remove: empty store has no name");
}

void test_cm1_author() {
    GdxCourseMeta meta;
    std::memset(&meta, 0, sizeof meta);
    gdx_meta_apply_editor_save(&meta, gdx_profile_state_active_name(), 0);
    check(meta.author[0] == '\0' && meta.editedBy[0] == '\0' && meta.madeHere == 1,
          "cm1: no profile saves no name");

    check(gdx_profile_state_create("ABCDEFGHIJKLMNOP", 0, -1) == 0, "cm1: 16-character profile created");
    check(gdx_meta_name_ok(gdx_profile_state_active_name()) == 1, "cm1: the longest profile name is a legal author");
    gdx_meta_apply_editor_save(&meta, gdx_profile_state_active_name(), 0);
    check(std::strcmp(meta.author, "ABCDEFGHIJKLMNOP") == 0 && meta.editedBy[0] == '\0',
          "cm1: first save writes the author");

    check(gdx_profile_state_create("Kai", 0, -1) == 1 && gdx_profile_state_set_active(1) == GDX_PROFILE_OK,
          "cm1: second profile active");
    gdx_meta_apply_editor_save(&meta, gdx_profile_state_active_name(), 0);
    check(std::strcmp(meta.author, "ABCDEFGHIJKLMNOP") == 0 && std::strcmp(meta.editedBy, "Kai") == 0,
          "cm1: another profile's save sets Edited by, author kept");

    check(gdx_profile_state_set_active(0) == GDX_PROFILE_OK, "cm1: author profile active again");
    gdx_meta_apply_editor_save(&meta, gdx_profile_state_active_name(), 0);
    check(std::strcmp(meta.author, "ABCDEFGHIJKLMNOP") == 0 && std::strcmp(meta.editedBy, "Kai") == 0,
          "cm1: the author's own save keeps the last editor");
    remove_all();
}

} // namespace

bool seed_zero(const unsigned char* seed) {
    for (int i = 0; i < GDX_PROFILE_KEYS_SEED_BYTES; i++) {
        if (seed[i] != 0) {
            return false;
        }
    }
    return true;
}

int key_count() {
    std::vector<unsigned char> bytes(GDX_PROFILE_KEYS_FILE_MAX_BYTES);
    const int n = gdx_profile_state_keys_encode_file(bytes.data(), bytes.size());
    return n < 0 ? -1 : (n - 12) / GDX_PROFILE_KEYS_ENTRY_BYTES;
}

// CM5: each profile gets its own signing seed on first use, kept in profile-keys.gpk and dropped with the profile.
void test_cm5_seeds() {
    unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES];
    unsigned char first[GDX_PROFILE_KEYS_SEED_BYTES];
    unsigned char again[GDX_PROFILE_KEYS_SEED_BYTES];

    remove_all();
    std::memset(seed, 0x55, sizeof seed);
    check(gdx_profile_state_active_seed(seed) == GDX_PROFILE_ERR_NOT_FOUND && seed_zero(seed),
          "cm5: no profile, no seed, and the buffer is zeroed");
    check(gdx_profile_state_active_seed(nullptr) == GDX_PROFILE_ERR_ARGS, "cm5: a NULL buffer is refused");
    check(!gdx_profile_state_active_has_seed(), "cm5: no profile has no seed");

    gdx_profile_state_create("Signer", 0x102030u, -1);
    check(!gdx_profile_state_active_has_seed(), "cm5: a new profile has no seed yet");
    const unsigned long k0 = gdx_profile_state_keys_revision();
    const unsigned long r0 = gdx_profile_state_revision();
    check(gdx_profile_state_active_seed(first) == GDX_PROFILE_OK && !seed_zero(first),
          "cm5: the first ask makes a seed");
    check(gdx_profile_state_keys_revision() != k0, "cm5: a new seed moves the key revision");
    check(gdx_profile_state_revision() == r0, "cm5: but not the profile revision, so profiles.gps is not rewritten");
    check(gdx_profile_state_active_has_seed() && key_count() == 1, "cm5: the seed is kept");
    const unsigned long k1 = gdx_profile_state_keys_revision();
    check(gdx_profile_state_active_seed(again) == GDX_PROFILE_OK && std::memcmp(first, again, sizeof first) == 0 &&
              gdx_profile_state_keys_revision() == k1,
          "cm5: the next ask returns the same seed and changes nothing");

    const int second = gdx_profile_state_create("Other", 0x405060u, -1);
    gdx_profile_state_set_active(second);
    check(gdx_profile_state_active_seed(again) == GDX_PROFILE_OK && std::memcmp(first, again, sizeof first) != 0,
          "cm5: another profile gets another seed");
    check(key_count() == 2, "cm5: two profiles, two seeds");

    std::vector<unsigned char> bytes(GDX_PROFILE_KEYS_FILE_MAX_BYTES);
    const int n = gdx_profile_state_keys_encode_file(bytes.data(), bytes.size());
    check(n == 12 + 2 * GDX_PROFILE_KEYS_ENTRY_BYTES, "cm5: the key file holds both");
    bytes.resize(n > 0 ? static_cast<size_t>(n) : 0);
    check(gdx_profile_state_remove(second) == GDX_PROFILE_OK && key_count() == 1,
          "cm5: deleting a profile deletes its seed");
    std::vector<unsigned char> broken = bytes;
    broken[20] ^= 1;
    check(gdx_profile_state_keys_restore_file(broken.data(), broken.size()) < 0 && key_count() == 1,
          "cm5: a damaged key file is refused and the seeds stay");
    check(gdx_profile_state_keys_restore_file(bytes.data(), bytes.size()) == GDX_PROFILE_OK && key_count() == 2,
          "cm5: a good key file restores");
    gdx_profile_state_set_active(0);
    check(gdx_profile_state_active_seed(seed) == GDX_PROFILE_OK && std::memcmp(seed, first, sizeof seed) == 0,
          "cm5: the restored seed is the one made before");

    // Seeds of profiles this install no longer has fill the file: the next new seed clears them out.
    GdxProfileKeys orphans{};
    for (int i = 0; i < GDX_PROFILE_KEYS_CAPACITY; i++) {
        unsigned char id[GDX_PROFILE_ID_BYTES] = {};
        id[0] = 0xEE;
        id[1] = static_cast<unsigned char>(i);
        unsigned char value[GDX_PROFILE_KEYS_SEED_BYTES];
        std::memset(value, i + 1, sizeof value);
        gdx_profile_keys_set(&orphans, id, value);
    }
    std::vector<unsigned char> full(GDX_PROFILE_KEYS_FILE_MAX_BYTES);
    const int m = gdx_profile_keys_encode(&orphans, full.data(), full.size());
    check(m > 0 && gdx_profile_state_keys_restore_file(full.data(), static_cast<size_t>(m)) == GDX_PROFILE_OK &&
              key_count() == GDX_PROFILE_KEYS_CAPACITY,
          "cm5: a full key file of strangers restores");
    check(!gdx_profile_state_active_has_seed(), "cm5: none of them is this profile's");
    check(gdx_profile_state_active_seed(seed) == GDX_PROFILE_OK && !seed_zero(seed) && key_count() == 1,
          "cm5: a new seed fits once the strangers are pruned");
    remove_all();
    check(key_count() == 0, "cm5: deleting every profile leaves no seed");
}

int main() {
    test_empty();
    test_create();
    test_edits();
    test_file();
    test_capacity();
    test_cm1_author();
    test_cm5_seeds();
    std::printf("%s: %d failure(s)\n", sFailures == 0 ? "all checks passed" : "checks failed", sFailures);
    return sFailures == 0 ? 0 : 1;
}
