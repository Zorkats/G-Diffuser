// port/gdx_profile.h: player profiles for the online milestone (name, icon, colour and preferred machine). A GPF1
// block holds one profile (the .gdxprofile file) and a GPS1 block holds the whole store. Pure data: no file IO, no
// RNG, no networking. The caller reads and writes the bytes and supplies the 16-byte profile id.
//
// A profile name becomes the Author of every track made under it. The name rules follow the author field of
// gdx_course_meta.h with a shorter limit, so every profile name fits that field.
//
// GPF1 block (little-endian):
//
//   0x00  magic        "GPF1"
//   0x04  version      u16 = 1
//   0x06  recordCount  u16
//   0x08  records      recordCount x { u8 tag, u16 length, value[length] }, tags strictly increasing
//   end-4 crc32       u32, CRC-32 over every byte before it
//
// Tags of version 1:
//
//   1  name     1..GDX_PROFILE_NAME_MAX printable ASCII bytes, no space at either end
//   2  id       GDX_PROFILE_ID_BYTES bytes, not all zero, chosen by the caller
//   3  colour   u32, 0x00RRGGBB, at most 0xFFFFFF
//   4  machine  u8 roster index below GDX_PROFILE_ROSTER_COUNT, or GDX_PROFILE_NO_MACHINE
//   5  icon     u8 kind, u8 index. Stock: index is a roster pilot. Custom or initials: index is 0.
//   6  pixels   GDX_PROFILE_ICON_BYTES of RGBA8888, row-major, top row first. Only with a custom icon.
//
// The decoder skips tags it does not know and does not keep them, so a later build can add records without a
// format break. No signature is stored yet.
//
// GPS1 store block (little-endian):
//
//   0x00  magic        "GPS1"
//   0x04  version      u16 = 1
//   0x06  recordCount  u16 = 1 + store count
//   0x08  records      tag 1: u8 active index, 0 when the store is empty
//                      tag 2 + n: one GPF1 block for slot n, slots 0..count-1 with no gaps
//   end-4 crc32        u32, CRC-32 over every byte before it
//
// GdxProfileStore is about 131 KB. Keep it static or on the heap, never on a thread stack.

#ifndef GDX_PROFILE_H
#define GDX_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_PROFILE_NAME_MAX 16
#define GDX_PROFILE_ID_BYTES 16
// The 30 selectable machines and pilots, in sDefaultMachines[] order (racer.c). gMachineNames[] in common.c has 36
// entries because 30 to 35 are story variants (Super Falcon and others) that no player can pick, so a profile never
// names them.
#define GDX_PROFILE_ROSTER_COUNT 30
#define GDX_PROFILE_NO_MACHINE 0xFF
#define GDX_PROFILE_CAPACITY 8
#define GDX_PROFILE_ICON_SIZE 64
#define GDX_PROFILE_ICON_BYTES (GDX_PROFILE_ICON_SIZE * GDX_PROFILE_ICON_SIZE * 4)
// Largest side of a source image the icon helpers accept. Callers decode their image outside this module.
#define GDX_PROFILE_ICON_SRC_MAX 4096

#define GDX_PROFILE_ICON_STOCK 0
#define GDX_PROFILE_ICON_CUSTOM 1
#define GDX_PROFILE_ICON_INITIALS 2

#define GDX_PROFILE_TAG_NAME 1
#define GDX_PROFILE_TAG_ID 2
#define GDX_PROFILE_TAG_COLOUR 3
#define GDX_PROFILE_TAG_MACHINE 4
#define GDX_PROFILE_TAG_ICON 5
#define GDX_PROFILE_TAG_PIXELS 6

#define GDX_PROFILE_STORE_TAG_ACTIVE 1
#define GDX_PROFILE_STORE_TAG_SLOT 2

#define GDX_PROFILE_HEADER_SIZE 8
#define GDX_PROFILE_RECORD_HEADER_SIZE 3
// The largest profile block: a custom icon and a 16-character name.
#define GDX_PROFILE_BLOCK_MAX_BYTES                                                                         \
    (GDX_PROFILE_HEADER_SIZE + (GDX_PROFILE_RECORD_HEADER_SIZE + GDX_PROFILE_NAME_MAX) +                    \
     (GDX_PROFILE_RECORD_HEADER_SIZE + GDX_PROFILE_ID_BYTES) + (GDX_PROFILE_RECORD_HEADER_SIZE + 4) +       \
     (GDX_PROFILE_RECORD_HEADER_SIZE + 1) + (GDX_PROFILE_RECORD_HEADER_SIZE + 2) +                          \
     (GDX_PROFILE_RECORD_HEADER_SIZE + GDX_PROFILE_ICON_BYTES) + 4)

#define GDX_PROFILE_STORE_HEADER_SIZE 8
// The largest store block: a full store of custom profiles with 16-character names.
#define GDX_PROFILE_STORE_BLOCK_MAX_BYTES                                                                   \
    (GDX_PROFILE_STORE_HEADER_SIZE + (GDX_PROFILE_RECORD_HEADER_SIZE + 1) +                                 \
     GDX_PROFILE_CAPACITY * (GDX_PROFILE_RECORD_HEADER_SIZE + GDX_PROFILE_BLOCK_MAX_BYTES) + 4)

#define GDX_PROFILE_OK 0
#define GDX_PROFILE_ERR_ARGS (-1)         // NULL pointer or otherwise unusable argument
#define GDX_PROFILE_ERR_FULL (-2)         // output buffer or store is full
#define GDX_PROFILE_ERR_VALUE (-3)        // a value breaks its rules
#define GDX_PROFILE_ERR_NOT_FOUND (-4)    // no profile with that id
#define GDX_PROFILE_ERR_DUPLICATE (-5)    // a profile with that id is already stored
#define GDX_PROFILE_ERR_TRUNCATED (-20)   // shorter than the minimum
#define GDX_PROFILE_ERR_MAGIC (-21)       // wrong magic
#define GDX_PROFILE_ERR_VERSION (-22)     // unknown version
#define GDX_PROFILE_ERR_CRC (-23)         // trailing CRC-32 does not match the bytes
#define GDX_PROFILE_ERR_LAYOUT (-25)      // sizes, order or required records do not add up
// -24 is GDX_META_ERR_BINDING in gdx_course_meta.h. Profiles are not bound to a course, so it is unused here.

// One player profile. The name is NUL-terminated and must pass gdx_profile_name_ok. icon holds pixels only when
// iconKind is GDX_PROFILE_ICON_CUSTOM; decode leaves it zero for the other kinds.
typedef struct GdxProfile {
    unsigned char id[GDX_PROFILE_ID_BYTES];
    char name[GDX_PROFILE_NAME_MAX + 1];
    uint32_t colour;                              // 0x00RRGGBB
    int preferredMachine;                         // roster index, or -1 for none
    int iconKind;                                 // GDX_PROFILE_ICON_*
    int iconIndex;                                // roster pilot for stock icons, 0 otherwise
    unsigned char icon[GDX_PROFILE_ICON_BYTES];   // RGBA8888, top row first
} GdxProfile;

// Fixed-capacity store. Indexes are positions 0..count-1. active is the index of the active profile, and is 0 when
// the store is empty, so a zeroed store is a valid empty store.
typedef struct GdxProfileStore {
    int count;
    int active;
    GdxProfile profiles[GDX_PROFILE_CAPACITY];
} GdxProfileStore;

// 1 for a legal profile name: 1..GDX_PROFILE_NAME_MAX printable ASCII bytes, no space at either end.
int gdx_profile_name_ok(const char* name);

// Trims spaces from both ends of raw and copies the rest, NUL-terminated, to out. Fails with VALUE when the trimmed
// text is not a legal name or does not fit cap; out[0] is 0 after such a failure. ARGS for a NULL pointer or cap 0.
int gdx_profile_name_normalise(const char* raw, char* out, size_t cap);

// GDX_PROFILE_OK when every field follows its rules, VALUE otherwise, ARGS for NULL. An all-zero id is not legal.
int gdx_profile_validate(const GdxProfile* profile);

// GPF1 codec. Encode returns the block length, or a negative code: ARGS for NULL, VALUE when the profile is not valid,
// FULL when cap is too small. Decode returns the first failing code and zeroes out on every failure. Unknown tags are
// skipped and not kept.
int gdx_profile_encode(const GdxProfile* profile, unsigned char* out, size_t cap);
int gdx_profile_decode(const unsigned char* data, size_t length, GdxProfile* out);

// Store operations. Remove shifts later profiles down. The active profile stays active when it survives; when it is
// removed, the profile that takes its position becomes active, or the last one when it was at the end.
// Clear empties the store. Add returns the new index, or a negative code: VALUE for an invalid profile, FULL when the
// store holds GDX_PROFILE_CAPACITY profiles, DUPLICATE when the id is already stored. Adding to an empty store makes
// the new profile active. Rename checks the name and leaves the profile untouched on failure. Every operation except
// clear returns ARGS for a NULL pointer, an unusable store (count or active out of range) or a bad index. Clear accepts
// any non-NULL store, so it can reset one that is unusable. Add checks VALUE, then DUPLICATE, then FULL. Remove,
// rename and set_active return OK on success.
int gdx_profile_store_clear(GdxProfileStore* store);
int gdx_profile_store_add(GdxProfileStore* store, const GdxProfile* profile);
int gdx_profile_store_remove(GdxProfileStore* store, int index);
int gdx_profile_store_rename(GdxProfileStore* store, int index, const char* name);
int gdx_profile_store_set_active(GdxProfileStore* store, int index);
// Index of the profile with this id, or GDX_PROFILE_ERR_NOT_FOUND; ARGS for a NULL pointer or an unusable store.
int gdx_profile_store_find(const GdxProfileStore* store, const unsigned char* id);

// GPS1 codec, same contract as the profile codec. Encode and decode reject a broken store with VALUE, or with
// DUPLICATE when two slots share an id. Decode skips GPS1 tags 10 and above.
int gdx_profile_store_encode(const GdxProfileStore* store, unsigned char* out, size_t cap);
int gdx_profile_store_decode(const unsigned char* data, size_t length, GdxProfileStore* out);

// Crops the largest centred square of a RGBA8888 source (srcWidth x srcHeight, top row first, 4 bytes per pixel) and
// scales it to GDX_PROFILE_ICON_SIZE squared in dst. The source must hold srcWidth * srcHeight * 4 bytes, and dst must
// hold GDX_PROFILE_ICON_BYTES. Crops larger than the icon are shrunk with an alpha-weighted box filter. Smaller crops
// are enlarged with nearest-neighbour sampling, and a crop of exactly the icon size is copied unchanged.
// ARGS for a NULL pointer or a size outside 1..GDX_PROFILE_ICON_SRC_MAX; dst is left untouched then.
int gdx_profile_icon_from_rgba(const unsigned char* src, int srcWidth, int srcHeight, unsigned char* dst);

// Fills dst with one opaque colour, 0x00RRGGBB. ARGS for a NULL dst, VALUE when colour exceeds 0xFFFFFF.
int gdx_profile_icon_solid(unsigned char* dst, uint32_t colour);

#ifdef __cplusplus
}
#endif

#endif // GDX_PROFILE_H
