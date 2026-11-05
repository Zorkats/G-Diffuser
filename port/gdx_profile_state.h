// port/gdx_profile_state.h -- the game's one profile store, and the edits the Profile page makes to it.
//
// gdx_profile.h is the pure codec. This module owns the live GdxProfileStore, counts its edits so
// gdx_course_state.cpp writes profiles.gps only after a change, and picks each new profile's id from the
// operating system's random source. The active profile's name is the Author of tracks saved in Course Edit.
//
// Every edit returns GDX_PROFILE_OK or a new index on success, or a negative GDX_PROFILE_ERR_* code and
// leaves the store unchanged. Call gdx_course_boot() before reading the store, so the file is loaded.

#ifndef GDX_PROFILE_STATE_H
#define GDX_PROFILE_STATE_H

#include "gdx_profile.h"
#include "gdx_profile_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

// The GPS1 file is one store block.
#define GDX_PROFILE_FILE_MAX_BYTES GDX_PROFILE_STORE_BLOCK_MAX_BYTES
// The operating system gave no random bytes, so no id could be made.
#define GDX_PROFILE_STATE_ERR_RANDOM (-30)

// The live store, read-only. Never NULL.
const GdxProfileStore* gdx_profile_state_store(void);

// Name of the active profile, or NULL when there is no profile. Valid until the next edit.
const char* gdx_profile_state_active_name(void);

// Adds a profile named rawName (spaces at either end are trimmed) with a fresh random id and an initials
// icon. machine is a roster index or -1. Returns the new index. The first profile becomes active.
int gdx_profile_state_create(const char* rawName, uint32_t colour, int machine);
int gdx_profile_state_rename(int index, const char* rawName);
int gdx_profile_state_set_colour(int index, uint32_t colour);
int gdx_profile_state_set_machine(int index, int machine);
int gdx_profile_state_set_active(int index);
int gdx_profile_state_remove(int index);

// StoreFile hooks for gdx_course_state.cpp. Restore replaces the store only when the whole file decodes.
unsigned long gdx_profile_state_revision(void);
int gdx_profile_state_encode_file(unsigned char* out, size_t cap);
int gdx_profile_state_restore_file(const unsigned char* data, size_t length);

// Copies the signing seed of the active profile into seed (CM5). The first call for a profile makes the seed
// from the operating system's random source and keeps it in profile-keys.gpk, never in profiles.gps. Returns
// GDX_PROFILE_OK, GDX_PROFILE_ERR_NOT_FOUND when no profile is active, GDX_PROFILE_STATE_ERR_RANDOM, or
// GDX_PROFILE_ERR_FULL. seed is zeroed on failure. The caller wipes it after use.
int gdx_profile_state_active_seed(unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES]);
// 1 when the active profile already has a seed, so asking for it makes none.
int gdx_profile_state_active_has_seed(void);

// StoreFile hooks for profile-keys.gpk. Deleting a profile deletes its seed.
unsigned long gdx_profile_state_keys_revision(void);
int gdx_profile_state_keys_encode_file(unsigned char* out, size_t cap);
int gdx_profile_state_keys_restore_file(const unsigned char* data, size_t length);

#ifdef __cplusplus
}
#endif

#endif // GDX_PROFILE_STATE_H
