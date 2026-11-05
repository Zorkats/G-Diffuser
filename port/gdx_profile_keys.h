// port/gdx_profile_keys.h: the signing seed of each local profile (plan item CM5), kept in profile-keys.gpk. Pure
// data: no file IO, no RNG. The caller supplies the seeds.
//
// The seeds live apart from profiles.gps on purpose. A profile is meant to be shared one day (.gdxprofile), and a
// seed must never travel with it. And a build that does not know signing re-saves profiles.gps without unknown
// records, which would lose the seed and with it the key every signed track names.
//
// GPK1 file (little-endian):
//
//   0x00  magic       "GPK1"
//   0x04  version     u16 = 1
//   0x06  count       u16, 0..GDX_PROFILE_KEYS_CAPACITY
//   0x08  entries     count x { id[GDX_PROFILE_ID_BYTES], seed[GDX_PROFILE_KEYS_SEED_BYTES] }, ids distinct, not all zero
//   end-4 crc32       u32, CRC-32 over every byte before it
//
// The file is a secret. Anyone who reads it can sign tracks as these profiles.

#ifndef GDX_PROFILE_KEYS_H
#define GDX_PROFILE_KEYS_H

#include <stddef.h>

#include "gdx_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_PROFILE_KEYS_SEED_BYTES 32
// More than the profile capacity, so the keys of a profile store restored from another install still fit.
#define GDX_PROFILE_KEYS_CAPACITY 16
#define GDX_PROFILE_KEYS_ENTRY_BYTES (GDX_PROFILE_ID_BYTES + GDX_PROFILE_KEYS_SEED_BYTES)
#define GDX_PROFILE_KEYS_FILE_MAX_BYTES (8 + GDX_PROFILE_KEYS_CAPACITY * GDX_PROFILE_KEYS_ENTRY_BYTES + 4)

typedef struct GdxProfileKeys {
    int count;
    unsigned char ids[GDX_PROFILE_KEYS_CAPACITY][GDX_PROFILE_ID_BYTES];
    unsigned char seeds[GDX_PROFILE_KEYS_CAPACITY][GDX_PROFILE_KEYS_SEED_BYTES];
} GdxProfileKeys;

// Index of the entry for id, or -1.
int gdx_profile_keys_find(const GdxProfileKeys* keys, const unsigned char id[GDX_PROFILE_ID_BYTES]);

// Adds or replaces the seed of id. Returns the index, GDX_PROFILE_ERR_ARGS for NULL or an all-zero id, or
// GDX_PROFILE_ERR_FULL.
int gdx_profile_keys_set(GdxProfileKeys* keys, const unsigned char id[GDX_PROFILE_ID_BYTES],
                         const unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES]);

// Removes the entry of id and wipes its seed. GDX_PROFILE_OK, or GDX_PROFILE_ERR_NOT_FOUND.
int gdx_profile_keys_remove(GdxProfileKeys* keys, const unsigned char id[GDX_PROFILE_ID_BYTES]);

// Returns the file length, or a negative GDX_PROFILE_ERR_* code.
int gdx_profile_keys_encode(const GdxProfileKeys* keys, unsigned char* out, size_t cap);

// Fills out from a whole file. out is zeroed on every failure.
int gdx_profile_keys_decode(const unsigned char* data, size_t length, GdxProfileKeys* out);

// Overwrites n bytes with zeros in a way the compiler keeps.
void gdx_profile_keys_wipe(void* p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
