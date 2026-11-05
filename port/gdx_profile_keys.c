// port/gdx_profile_keys.c: GPK1 seed file, described in gdx_profile_keys.h.

#include "gdx_profile_keys.h"

#include "gdx_course_registry.h"

#include <string.h>

#define KEYS_HEADER_BYTES 8

static void put_u16le(unsigned char* p, unsigned int v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
}

static void put_u32le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

static unsigned int get_u16le(const unsigned char* p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}

static uint32_t get_u32le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int id_ok(const unsigned char* id) {
    int i;

    for (i = 0; i < GDX_PROFILE_ID_BYTES; i++) {
        if (id[i] != 0) {
            return 1;
        }
    }
    return 0;
}

void gdx_profile_keys_wipe(void* p, size_t n) {
    // Writes through a volatile pointer, so the stores are not dropped as dead.
    volatile unsigned char* bytes = (volatile unsigned char*) p;

    while (n-- > 0) {
        *bytes++ = 0;
    }
}

int gdx_profile_keys_find(const GdxProfileKeys* keys, const unsigned char id[GDX_PROFILE_ID_BYTES]) {
    int i;

    if ((keys == NULL) || (id == NULL)) {
        return -1;
    }
    for (i = 0; i < keys->count; i++) {
        if (memcmp(keys->ids[i], id, GDX_PROFILE_ID_BYTES) == 0) {
            return i;
        }
    }
    return -1;
}

int gdx_profile_keys_set(GdxProfileKeys* keys, const unsigned char id[GDX_PROFILE_ID_BYTES],
                         const unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES]) {
    int index;

    if ((keys == NULL) || (id == NULL) || (seed == NULL) || !id_ok(id)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    index = gdx_profile_keys_find(keys, id);
    if (index < 0) {
        if (keys->count >= GDX_PROFILE_KEYS_CAPACITY) {
            return GDX_PROFILE_ERR_FULL;
        }
        index = keys->count++;
        memcpy(keys->ids[index], id, GDX_PROFILE_ID_BYTES);
    }
    memcpy(keys->seeds[index], seed, GDX_PROFILE_KEYS_SEED_BYTES);
    return index;
}

int gdx_profile_keys_remove(GdxProfileKeys* keys, const unsigned char id[GDX_PROFILE_ID_BYTES]) {
    const int index = gdx_profile_keys_find(keys, id);
    int last;

    if (index < 0) {
        return GDX_PROFILE_ERR_NOT_FOUND;
    }
    last = keys->count - 1;
    if (index != last) {
        memcpy(keys->ids[index], keys->ids[last], GDX_PROFILE_ID_BYTES);
        memcpy(keys->seeds[index], keys->seeds[last], GDX_PROFILE_KEYS_SEED_BYTES);
    }
    gdx_profile_keys_wipe(keys->ids[last], GDX_PROFILE_ID_BYTES);
    gdx_profile_keys_wipe(keys->seeds[last], GDX_PROFILE_KEYS_SEED_BYTES);
    keys->count = last;
    return GDX_PROFILE_OK;
}

int gdx_profile_keys_encode(const GdxProfileKeys* keys, unsigned char* out, size_t cap) {
    size_t pos = KEYS_HEADER_BYTES;
    int i;

    if ((keys == NULL) || (out == NULL) || (keys->count < 0) || (keys->count > GDX_PROFILE_KEYS_CAPACITY)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if (cap < KEYS_HEADER_BYTES + (size_t) keys->count * GDX_PROFILE_KEYS_ENTRY_BYTES + 4) {
        return GDX_PROFILE_ERR_FULL;
    }
    memcpy(out, "GPK1", 4);
    put_u16le(out + 4, 1);
    put_u16le(out + 6, (unsigned int) keys->count);
    for (i = 0; i < keys->count; i++) {
        memcpy(out + pos, keys->ids[i], GDX_PROFILE_ID_BYTES);
        memcpy(out + pos + GDX_PROFILE_ID_BYTES, keys->seeds[i], GDX_PROFILE_KEYS_SEED_BYTES);
        pos += GDX_PROFILE_KEYS_ENTRY_BYTES;
    }
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

static int decode_keys(const unsigned char* data, size_t length, GdxProfileKeys* out) {
    unsigned int count;
    unsigned int i;
    size_t pos = KEYS_HEADER_BYTES;

    if (data == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if (length < KEYS_HEADER_BYTES + 4) {
        return GDX_PROFILE_ERR_TRUNCATED;
    }
    if (memcmp(data, "GPK1", 4) != 0) {
        return GDX_PROFILE_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != 1) {
        return GDX_PROFILE_ERR_VERSION;
    }
    if (get_u32le(data + length - 4) != gdx_course_registry_crc32(data, length - 4)) {
        return GDX_PROFILE_ERR_CRC;
    }
    count = get_u16le(data + 6);
    if ((count > GDX_PROFILE_KEYS_CAPACITY) ||
        (length != KEYS_HEADER_BYTES + (size_t) count * GDX_PROFILE_KEYS_ENTRY_BYTES + 4)) {
        return GDX_PROFILE_ERR_LAYOUT;
    }
    for (i = 0; i < count; i++) {
        const unsigned char* id = data + pos;

        if (!id_ok(id) || (gdx_profile_keys_find(out, id) >= 0)) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        memcpy(out->ids[i], id, GDX_PROFILE_ID_BYTES);
        memcpy(out->seeds[i], data + pos + GDX_PROFILE_ID_BYTES, GDX_PROFILE_KEYS_SEED_BYTES);
        out->count = (int) i + 1;
        pos += GDX_PROFILE_KEYS_ENTRY_BYTES;
    }
    return GDX_PROFILE_OK;
}

int gdx_profile_keys_decode(const unsigned char* data, size_t length, GdxProfileKeys* out) {
    int rc;

    if (out == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    gdx_profile_keys_wipe(out, sizeof(*out));
    rc = decode_keys(data, length, out);
    if (rc != GDX_PROFILE_OK) {
        gdx_profile_keys_wipe(out, sizeof(*out));
    }
    return rc;
}
