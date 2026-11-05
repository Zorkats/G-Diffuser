// port/gdx_profile_state.cpp -- see gdx_profile_state.h.

#include "gdx_profile_state.h"

#include <cstring>
#include <random>

namespace {

// About 131 KB each, so both stay static.
GdxProfileStore sStore;
GdxProfileStore sStage;
unsigned long sRevision = 0;
GdxProfileKeys sKeys;
GdxProfileKeys sKeysStage;
unsigned long sKeysRevision = 0;

bool index_ok(int index) {
    return index >= 0 && index < sStore.count;
}

// std::random_device reads the OS source on the toolchains we ship (RtlGenRandom on MSVC,
// /dev/urandom or getentropy on libc++ and libstdc++). It throws when that source is unavailable.
// n is a multiple of 4.
bool random_bytes(unsigned char* out, int n) {
    try {
        std::random_device rd;
        for (int i = 0; i < n; i += 4) {
            const unsigned int word = rd();
            out[i + 0] = static_cast<unsigned char>(word);
            out[i + 1] = static_cast<unsigned char>(word >> 8);
            out[i + 2] = static_cast<unsigned char>(word >> 16);
            out[i + 3] = static_cast<unsigned char>(word >> 24);
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool random_id(unsigned char out[GDX_PROFILE_ID_BYTES]) {
    return random_bytes(out, GDX_PROFILE_ID_BYTES);
}

bool known_id(const unsigned char* id) {
    for (int i = 0; i < sStore.count; i++) {
        if (std::memcmp(sStore.profiles[i].id, id, GDX_PROFILE_ID_BYTES) == 0) {
            return true;
        }
    }
    return false;
}

// Drops the seeds of profiles this store no longer has, such as those of a profiles.gps from another install.
bool prune_keys() {
    bool pruned = false;
    for (int i = sKeys.count - 1; i >= 0; i--) {
        if (!known_id(sKeys.ids[i])) {
            unsigned char id[GDX_PROFILE_ID_BYTES];
            std::memcpy(id, sKeys.ids[i], sizeof id);
            gdx_profile_keys_remove(&sKeys, id);
            pruned = true;
        }
    }
    return pruned;
}

// Applies one field change to a copy, so a value the codec would reject never reaches the store.
template <typename Change> int edit(int index, Change change) {
    if (!index_ok(index)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    GdxProfile& profile = sStore.profiles[index];
    const uint32_t oldColour = profile.colour;
    const int oldMachine = profile.preferredMachine;
    change(profile);
    if (gdx_profile_validate(&profile) != GDX_PROFILE_OK) {
        profile.colour = oldColour;
        profile.preferredMachine = oldMachine;
        return GDX_PROFILE_ERR_VALUE;
    }
    if (profile.colour != oldColour || profile.preferredMachine != oldMachine) {
        sRevision++;
    }
    return GDX_PROFILE_OK;
}

} // namespace

extern "C" const GdxProfileStore* gdx_profile_state_store(void) {
    return &sStore;
}

extern "C" const char* gdx_profile_state_active_name(void) {
    return index_ok(sStore.active) ? sStore.profiles[sStore.active].name : nullptr;
}

extern "C" int gdx_profile_state_create(const char* rawName, uint32_t colour, int machine) {
    static GdxProfile sNew; // 16 KB of icon pixels: not for the stack either
    std::memset(&sNew, 0, sizeof sNew);
    const int nameRc = gdx_profile_name_normalise(rawName, sNew.name, sizeof sNew.name);
    if (nameRc != GDX_PROFILE_OK) {
        return nameRc;
    }
    sNew.colour = colour;
    sNew.preferredMachine = machine;
    sNew.iconKind = GDX_PROFILE_ICON_INITIALS;
    sNew.iconIndex = 0;
    // Check every field but the id first, so a VALUE from add below can only mean an all-zero id.
    sNew.id[0] = 1;
    if (gdx_profile_validate(&sNew) != GDX_PROFILE_OK) {
        return GDX_PROFILE_ERR_VALUE;
    }
    // A zero or repeated id out of 128 random bits means the source is broken; a few tries tell.
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!random_id(sNew.id)) {
            return GDX_PROFILE_STATE_ERR_RANDOM;
        }
        const int rc = gdx_profile_store_add(&sStore, &sNew);
        if (rc >= 0) {
            sRevision++;
            return rc;
        }
        if (rc != GDX_PROFILE_ERR_DUPLICATE && rc != GDX_PROFILE_ERR_VALUE) {
            return rc;
        }
    }
    return GDX_PROFILE_STATE_ERR_RANDOM;
}

extern "C" int gdx_profile_state_rename(int index, const char* rawName) {
    char name[GDX_PROFILE_NAME_MAX + 1];
    const int nameRc = gdx_profile_name_normalise(rawName, name, sizeof name);
    if (nameRc != GDX_PROFILE_OK) {
        return nameRc;
    }
    if (!index_ok(index)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if (std::strcmp(sStore.profiles[index].name, name) == 0) {
        return GDX_PROFILE_OK;
    }
    const int rc = gdx_profile_store_rename(&sStore, index, name);
    if (rc == GDX_PROFILE_OK) {
        sRevision++;
    }
    return rc;
}

extern "C" int gdx_profile_state_set_colour(int index, uint32_t colour) {
    return edit(index, [colour](GdxProfile& p) { p.colour = colour; });
}

extern "C" int gdx_profile_state_set_machine(int index, int machine) {
    return edit(index, [machine](GdxProfile& p) { p.preferredMachine = machine; });
}

extern "C" int gdx_profile_state_set_active(int index) {
    if (index_ok(index) && index == sStore.active) {
        return GDX_PROFILE_OK;
    }
    const int rc = gdx_profile_store_set_active(&sStore, index);
    if (rc == GDX_PROFILE_OK) {
        sRevision++;
    }
    return rc;
}

extern "C" int gdx_profile_state_remove(int index) {
    unsigned char id[GDX_PROFILE_ID_BYTES] = {};
    if (index_ok(index)) {
        std::memcpy(id, sStore.profiles[index].id, sizeof id);
    }
    const int rc = gdx_profile_store_remove(&sStore, index);
    if (rc == GDX_PROFILE_OK) {
        sRevision++;
        if (gdx_profile_keys_remove(&sKeys, id) == GDX_PROFILE_OK) {
            sKeysRevision++;
        }
    }
    return rc;
}

extern "C" unsigned long gdx_profile_state_revision(void) {
    return sRevision;
}

extern "C" int gdx_profile_state_encode_file(unsigned char* out, size_t cap) {
    return gdx_profile_store_encode(&sStore, out, cap);
}

extern "C" int gdx_profile_state_restore_file(const unsigned char* data, size_t length) {
    const int rc = gdx_profile_store_decode(data, length, &sStage);
    if (rc != GDX_PROFILE_OK) {
        return rc;
    }
    std::memcpy(&sStore, &sStage, sizeof sStore);
    sRevision++;
    return GDX_PROFILE_OK;
}

extern "C" int gdx_profile_state_active_seed(unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES]) {
    if (!seed) {
        return GDX_PROFILE_ERR_ARGS;
    }
    std::memset(seed, 0, GDX_PROFILE_KEYS_SEED_BYTES);
    if (!index_ok(sStore.active)) {
        return GDX_PROFILE_ERR_NOT_FOUND;
    }
    const unsigned char* id = sStore.profiles[sStore.active].id;
    int index = gdx_profile_keys_find(&sKeys, id);
    if (index < 0) {
        unsigned char fresh[GDX_PROFILE_KEYS_SEED_BYTES];
        if (!random_bytes(fresh, GDX_PROFILE_KEYS_SEED_BYTES)) {
            return GDX_PROFILE_STATE_ERR_RANDOM;
        }
        index = gdx_profile_keys_set(&sKeys, id, fresh);
        if (index == GDX_PROFILE_ERR_FULL && prune_keys()) {
            sKeysRevision++;
            index = gdx_profile_keys_set(&sKeys, id, fresh);
        }
        gdx_profile_keys_wipe(fresh, sizeof fresh);
        if (index < 0) {
            return index;
        }
        sKeysRevision++;
    }
    std::memcpy(seed, sKeys.seeds[index], GDX_PROFILE_KEYS_SEED_BYTES);
    return GDX_PROFILE_OK;
}

extern "C" int gdx_profile_state_active_has_seed(void) {
    return index_ok(sStore.active) && gdx_profile_keys_find(&sKeys, sStore.profiles[sStore.active].id) >= 0;
}

extern "C" unsigned long gdx_profile_state_keys_revision(void) {
    return sKeysRevision;
}

extern "C" int gdx_profile_state_keys_encode_file(unsigned char* out, size_t cap) {
    return gdx_profile_keys_encode(&sKeys, out, cap);
}

extern "C" int gdx_profile_state_keys_restore_file(const unsigned char* data, size_t length) {
    const int rc = gdx_profile_keys_decode(data, length, &sKeysStage);
    if (rc != GDX_PROFILE_OK) {
        return rc;
    }
    std::memcpy(&sKeys, &sKeysStage, sizeof sKeys);
    gdx_profile_keys_wipe(&sKeysStage, sizeof sKeysStage);
    sKeysRevision++;
    return GDX_PROFILE_OK;
}
