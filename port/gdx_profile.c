// port/gdx_profile.c: GPF1 profile blocks, GPS1 store blocks, name rules and icon helpers.
// Pure data: no file IO, no RNG, no networking. The byte layout and tag rules are documented in gdx_profile.h.

#include "gdx_profile.h"

#include <string.h>

#define PROFILE_VERSION 1u
#define PROFILE_CRC_BYTES 4u
#define PROFILE_MIN_BYTES (GDX_PROFILE_HEADER_SIZE + PROFILE_CRC_BYTES)
#define STORE_MIN_BYTES (GDX_PROFILE_STORE_HEADER_SIZE + PROFILE_CRC_BYTES)
#define PROFILE_COLOUR_BYTES 4u
#define PROFILE_MACHINE_BYTES 1u
#define PROFILE_ICON_WIRE_BYTES 2u
#define PROFILE_STORE_ACTIVE_BYTES 1u
#define PROFILE_PIXELS_BIT (1u << GDX_PROFILE_TAG_PIXELS)
#define PROFILE_REQUIRED_BITS                                                                              \
    ((1u << GDX_PROFILE_TAG_NAME) | (1u << GDX_PROFILE_TAG_ID) | (1u << GDX_PROFILE_TAG_COLOUR) |          \
     (1u << GDX_PROFILE_TAG_MACHINE) | (1u << GDX_PROFILE_TAG_ICON))

static void put_u16le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static void put_u32le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
    p[2] = (unsigned char)((value >> 16) & 0xFFu);
    p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static uint32_t get_u16le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get_u32le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int bytes_fit(size_t pos, size_t end, size_t need) {
    return (pos <= end) && (need <= end - pos);
}

// Bitwise CRC-32/ISO-HDLC. Kept local so the module links without gdx_course_registry.c.
static uint32_t crc32_ieee(const unsigned char* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Rounds to nearest, halves up. denominator must be non-zero.
static uint32_t round_div(uint32_t numerator, uint32_t denominator) {
    return (numerator + denominator / 2u) / denominator;
}

static int name_bytes_ok(const unsigned char* bytes, size_t length) {
    size_t i;

    if ((length < 1) || (length > GDX_PROFILE_NAME_MAX) || (bytes[0] == ' ') || (bytes[length - 1] == ' ')) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        if ((bytes[i] < 0x20) || (bytes[i] > 0x7E)) {
            return 0;
        }
    }
    return 1;
}

int gdx_profile_name_ok(const char* name) {
    size_t length = 0;

    if (name == NULL) {
        return 0;
    }
    while ((length <= GDX_PROFILE_NAME_MAX) && (name[length] != '\0')) {
        length++;
    }
    return (length <= GDX_PROFILE_NAME_MAX) && name_bytes_ok((const unsigned char*)name, length);
}

int gdx_profile_name_normalise(const char* raw, char* out, size_t cap) {
    const unsigned char* begin;
    size_t length;

    if ((raw == NULL) || (out == NULL) || (cap == 0)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    begin = (const unsigned char*)raw;
    length = strlen(raw);
    while ((length > 0) && (begin[0] == ' ')) {
        begin++;
        length--;
    }
    while ((length > 0) && (begin[length - 1] == ' ')) {
        length--;
    }
    if ((length > GDX_PROFILE_NAME_MAX) || (length + 1 > cap) || !name_bytes_ok(begin, length)) {
        out[0] = '\0';
        return GDX_PROFILE_ERR_VALUE;
    }
    memmove(out, begin, length);
    out[length] = '\0';
    return GDX_PROFILE_OK;
}

int gdx_profile_validate(const GdxProfile* profile) {
    int i;
    int idNonZero = 0;

    if (profile == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    for (i = 0; i < GDX_PROFILE_ID_BYTES; i++) {
        if (profile->id[i] != 0) {
            idNonZero = 1;
        }
    }
    if (!idNonZero || !gdx_profile_name_ok(profile->name) || (profile->colour > 0xFFFFFFu)) {
        return GDX_PROFILE_ERR_VALUE;
    }
    if ((profile->preferredMachine < -1) || (profile->preferredMachine >= GDX_PROFILE_ROSTER_COUNT)) {
        return GDX_PROFILE_ERR_VALUE;
    }
    if (profile->iconKind == GDX_PROFILE_ICON_STOCK) {
        if ((profile->iconIndex < 0) || (profile->iconIndex >= GDX_PROFILE_ROSTER_COUNT)) {
            return GDX_PROFILE_ERR_VALUE;
        }
    } else if ((profile->iconKind == GDX_PROFILE_ICON_CUSTOM) || (profile->iconKind == GDX_PROFILE_ICON_INITIALS)) {
        if (profile->iconIndex != 0) {
            return GDX_PROFILE_ERR_VALUE;
        }
    } else {
        return GDX_PROFILE_ERR_VALUE;
    }
    return GDX_PROFILE_OK;
}

static size_t record_bytes(size_t valueLength) {
    return GDX_PROFILE_RECORD_HEADER_SIZE + valueLength;
}

static size_t profile_block_size(const GdxProfile* profile) {
    size_t total = GDX_PROFILE_HEADER_SIZE + PROFILE_CRC_BYTES;

    total += record_bytes(strlen(profile->name));
    total += record_bytes(GDX_PROFILE_ID_BYTES);
    total += record_bytes(PROFILE_COLOUR_BYTES);
    total += record_bytes(PROFILE_MACHINE_BYTES);
    total += record_bytes(PROFILE_ICON_WIRE_BYTES);
    if (profile->iconKind == GDX_PROFILE_ICON_CUSTOM) {
        total += record_bytes(GDX_PROFILE_ICON_BYTES);
    }
    return total;
}

static size_t put_record(unsigned char* out, size_t pos, int tag, size_t valueLength) {
    out[pos] = (unsigned char)tag;
    put_u16le(out + pos + 1, (uint32_t)valueLength);
    return pos + GDX_PROFILE_RECORD_HEADER_SIZE;
}

// Writes exactly profile_block_size(profile) bytes. The caller has validated the profile and sized the buffer.
static void encode_profile_block(const GdxProfile* profile, unsigned char* out) {
    int custom = (profile->iconKind == GDX_PROFILE_ICON_CUSTOM);
    size_t nameLength = strlen(profile->name);
    size_t pos = GDX_PROFILE_HEADER_SIZE;

    memcpy(out, "GPF1", 4);
    put_u16le(out + 4, PROFILE_VERSION);
    put_u16le(out + 6, custom ? 6u : 5u);

    pos = put_record(out, pos, GDX_PROFILE_TAG_NAME, nameLength);
    memcpy(out + pos, profile->name, nameLength);
    pos += nameLength;

    pos = put_record(out, pos, GDX_PROFILE_TAG_ID, GDX_PROFILE_ID_BYTES);
    memcpy(out + pos, profile->id, GDX_PROFILE_ID_BYTES);
    pos += GDX_PROFILE_ID_BYTES;

    pos = put_record(out, pos, GDX_PROFILE_TAG_COLOUR, PROFILE_COLOUR_BYTES);
    put_u32le(out + pos, profile->colour);
    pos += PROFILE_COLOUR_BYTES;

    pos = put_record(out, pos, GDX_PROFILE_TAG_MACHINE, PROFILE_MACHINE_BYTES);
    out[pos] = (unsigned char)((profile->preferredMachine < 0) ? GDX_PROFILE_NO_MACHINE : profile->preferredMachine);
    pos += PROFILE_MACHINE_BYTES;

    pos = put_record(out, pos, GDX_PROFILE_TAG_ICON, PROFILE_ICON_WIRE_BYTES);
    out[pos] = (unsigned char)profile->iconKind;
    out[pos + 1] = (unsigned char)profile->iconIndex;
    pos += PROFILE_ICON_WIRE_BYTES;

    if (custom) {
        pos = put_record(out, pos, GDX_PROFILE_TAG_PIXELS, GDX_PROFILE_ICON_BYTES);
        memcpy(out + pos, profile->icon, GDX_PROFILE_ICON_BYTES);
        pos += GDX_PROFILE_ICON_BYTES;
    }
    put_u32le(out + pos, crc32_ieee(out, pos));
}

int gdx_profile_encode(const GdxProfile* profile, unsigned char* out, size_t cap) {
    size_t total;

    if ((profile == NULL) || (out == NULL)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if (gdx_profile_validate(profile) != GDX_PROFILE_OK) {
        return GDX_PROFILE_ERR_VALUE;
    }
    total = profile_block_size(profile);
    if (total > cap) {
        return GDX_PROFILE_ERR_FULL;
    }
    encode_profile_block(profile, out);
    return (int)total;
}

// Unknown tags return OK without setting a bit, so they are skipped and never stored.
static int apply_profile_record(int tag, const unsigned char* value, size_t valueLength, GdxProfile* out, uint32_t* seen) {
    switch (tag) {
    case GDX_PROFILE_TAG_NAME:
        if (!name_bytes_ok(value, valueLength)) {
            return GDX_PROFILE_ERR_VALUE;
        }
        memcpy(out->name, value, valueLength);
        out->name[valueLength] = '\0';
        break;
    case GDX_PROFILE_TAG_ID:
        if (valueLength != GDX_PROFILE_ID_BYTES) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        memcpy(out->id, value, GDX_PROFILE_ID_BYTES);
        break;
    case GDX_PROFILE_TAG_COLOUR:
        if (valueLength != PROFILE_COLOUR_BYTES) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        out->colour = get_u32le(value);
        break;
    case GDX_PROFILE_TAG_MACHINE:
        if (valueLength != PROFILE_MACHINE_BYTES) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        if (value[0] == GDX_PROFILE_NO_MACHINE) {
            out->preferredMachine = -1;
        } else if (value[0] < GDX_PROFILE_ROSTER_COUNT) {
            out->preferredMachine = value[0];
        } else {
            return GDX_PROFILE_ERR_VALUE;
        }
        break;
    case GDX_PROFILE_TAG_ICON:
        if (valueLength != PROFILE_ICON_WIRE_BYTES) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        out->iconKind = value[0];
        out->iconIndex = value[1];
        break;
    case GDX_PROFILE_TAG_PIXELS:
        if (valueLength != GDX_PROFILE_ICON_BYTES) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        memcpy(out->icon, value, GDX_PROFILE_ICON_BYTES);
        break;
    default:
        return GDX_PROFILE_OK;
    }
    *seen |= 1u << tag;
    return GDX_PROFILE_OK;
}

// Decodes into out, which the caller has zeroed. On failure out holds partial data and the caller must clear it.
static int decode_profile_block(const unsigned char* data, size_t length, GdxProfile* out) {
    size_t pos;
    size_t end;
    uint32_t records;
    uint32_t i;
    uint32_t seen = 0;
    int lastTag = 0;
    int tag;
    size_t valueLength;
    int rc;

    if (length < PROFILE_MIN_BYTES) {
        return GDX_PROFILE_ERR_TRUNCATED;
    }
    if (length > GDX_PROFILE_BLOCK_MAX_BYTES) {
        return GDX_PROFILE_ERR_LAYOUT;
    }
    if (memcmp(data, "GPF1", 4) != 0) {
        return GDX_PROFILE_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != PROFILE_VERSION) {
        return GDX_PROFILE_ERR_VERSION;
    }
    end = length - PROFILE_CRC_BYTES;
    if (get_u32le(data + end) != crc32_ieee(data, end)) {
        return GDX_PROFILE_ERR_CRC;
    }

    records = get_u16le(data + 6);
    pos = GDX_PROFILE_HEADER_SIZE;
    for (i = 0; i < records; i++) {
        if (!bytes_fit(pos, end, GDX_PROFILE_RECORD_HEADER_SIZE)) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        tag = data[pos];
        valueLength = get_u16le(data + pos + 1);
        pos += GDX_PROFILE_RECORD_HEADER_SIZE;
        if ((tag <= lastTag) || !bytes_fit(pos, end, valueLength)) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        lastTag = tag;
        rc = apply_profile_record(tag, data + pos, valueLength, out, &seen);
        if (rc != GDX_PROFILE_OK) {
            return rc;
        }
        pos += valueLength;
    }
    if (pos != end) {
        return GDX_PROFILE_ERR_LAYOUT;
    }
    if ((seen & PROFILE_REQUIRED_BITS) != PROFILE_REQUIRED_BITS) {
        return GDX_PROFILE_ERR_LAYOUT;
    }
    if ((out->iconKind == GDX_PROFILE_ICON_CUSTOM) && ((seen & PROFILE_PIXELS_BIT) == 0)) {
        return GDX_PROFILE_ERR_LAYOUT;
    }
    if ((out->iconKind != GDX_PROFILE_ICON_CUSTOM) && ((seen & PROFILE_PIXELS_BIT) != 0)) {
        return GDX_PROFILE_ERR_VALUE;
    }
    return gdx_profile_validate(out);
}

int gdx_profile_decode(const unsigned char* data, size_t length, GdxProfile* out) {
    int rc;

    if (out == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    if (data == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    rc = decode_profile_block(data, length, out);
    if (rc != GDX_PROFILE_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

// Structural check used by every store operation: count in range, active inside it (0 when empty).
static int store_usable(const GdxProfileStore* store) {
    if ((store->count < 0) || (store->count > GDX_PROFILE_CAPACITY)) {
        return 0;
    }
    if (store->count == 0) {
        return store->active == 0;
    }
    return (store->active >= 0) && (store->active < store->count);
}

// Full state check used before encode and after decode. Broken fields give VALUE; repeated ids give DUPLICATE.
static int store_state_ok(const GdxProfileStore* store) {
    int i;
    int j;

    if (!store_usable(store)) {
        return GDX_PROFILE_ERR_VALUE;
    }
    for (i = 0; i < store->count; i++) {
        if (gdx_profile_validate(&store->profiles[i]) != GDX_PROFILE_OK) {
            return GDX_PROFILE_ERR_VALUE;
        }
    }
    for (i = 0; i < store->count; i++) {
        for (j = 0; j < i; j++) {
            if (memcmp(store->profiles[i].id, store->profiles[j].id, GDX_PROFILE_ID_BYTES) == 0) {
                return GDX_PROFILE_ERR_DUPLICATE;
            }
        }
    }
    return GDX_PROFILE_OK;
}

static size_t store_block_size(const GdxProfileStore* store) {
    size_t total = GDX_PROFILE_STORE_HEADER_SIZE + PROFILE_CRC_BYTES + record_bytes(PROFILE_STORE_ACTIVE_BYTES);
    int i;

    for (i = 0; i < store->count; i++) {
        total += record_bytes(profile_block_size(&store->profiles[i]));
    }
    return total;
}

int gdx_profile_store_clear(GdxProfileStore* store) {
    if (store == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    memset(store, 0, sizeof(*store));
    return GDX_PROFILE_OK;
}

int gdx_profile_store_add(GdxProfileStore* store, const GdxProfile* profile) {
    int index;

    if ((store == NULL) || (profile == NULL) || !store_usable(store)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if (gdx_profile_validate(profile) != GDX_PROFILE_OK) {
        return GDX_PROFILE_ERR_VALUE;
    }
    if (gdx_profile_store_find(store, profile->id) >= 0) {
        return GDX_PROFILE_ERR_DUPLICATE;
    }
    if (store->count == GDX_PROFILE_CAPACITY) {
        return GDX_PROFILE_ERR_FULL;
    }
    index = store->count;
    store->profiles[index] = *profile;
    store->count = index + 1;
    if (index == 0) {
        store->active = 0;
    }
    return index;
}

int gdx_profile_store_remove(GdxProfileStore* store, int index) {
    int last;

    if ((store == NULL) || !store_usable(store) || (index < 0) || (index >= store->count)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    last = store->count - 1;
    if (index < last) {
        memmove(&store->profiles[index], &store->profiles[index + 1], (size_t)(last - index) * sizeof(GdxProfile));
    }
    memset(&store->profiles[last], 0, sizeof(GdxProfile));
    store->count = last;
    if (store->count == 0) {
        store->active = 0;
    } else if (index < store->active) {
        store->active--;
    } else if (store->active >= store->count) {
        store->active = store->count - 1;
    }
    return GDX_PROFILE_OK;
}

int gdx_profile_store_rename(GdxProfileStore* store, int index, const char* name) {
    size_t length;

    if ((store == NULL) || (name == NULL) || !store_usable(store) || (index < 0) || (index >= store->count)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if (!gdx_profile_name_ok(name)) {
        return GDX_PROFILE_ERR_VALUE;
    }
    length = strlen(name);
    memcpy(store->profiles[index].name, name, length + 1);
    return GDX_PROFILE_OK;
}

int gdx_profile_store_set_active(GdxProfileStore* store, int index) {
    if ((store == NULL) || !store_usable(store) || (index < 0) || (index >= store->count)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    store->active = index;
    return GDX_PROFILE_OK;
}

int gdx_profile_store_find(const GdxProfileStore* store, const unsigned char* id) {
    int i;

    if ((store == NULL) || (id == NULL) || !store_usable(store)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    for (i = 0; i < store->count; i++) {
        if (memcmp(store->profiles[i].id, id, GDX_PROFILE_ID_BYTES) == 0) {
            return i;
        }
    }
    return GDX_PROFILE_ERR_NOT_FOUND;
}

int gdx_profile_store_encode(const GdxProfileStore* store, unsigned char* out, size_t cap) {
    size_t total;
    size_t pos;
    size_t blockLength;
    int i;
    int rc;

    if ((store == NULL) || (out == NULL)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    rc = store_state_ok(store);
    if (rc != GDX_PROFILE_OK) {
        return rc;
    }
    total = store_block_size(store);
    if (total > cap) {
        return GDX_PROFILE_ERR_FULL;
    }

    memcpy(out, "GPS1", 4);
    put_u16le(out + 4, PROFILE_VERSION);
    put_u16le(out + 6, (uint32_t)store->count + 1u);
    pos = put_record(out, GDX_PROFILE_STORE_HEADER_SIZE, GDX_PROFILE_STORE_TAG_ACTIVE, PROFILE_STORE_ACTIVE_BYTES);
    out[pos] = (unsigned char)store->active;
    pos += PROFILE_STORE_ACTIVE_BYTES;
    for (i = 0; i < store->count; i++) {
        blockLength = profile_block_size(&store->profiles[i]);
        pos = put_record(out, pos, GDX_PROFILE_STORE_TAG_SLOT + i, blockLength);
        encode_profile_block(&store->profiles[i], out + pos);
        pos += blockLength;
    }
    put_u32le(out + pos, crc32_ieee(out, pos));
    return (int)total;
}

// Decodes in place into out, which the caller has zeroed. Slot records must follow tag 2 with no gaps.
static int decode_store_block(const unsigned char* data, size_t length, GdxProfileStore* out) {
    size_t pos;
    size_t end;
    uint32_t records;
    uint32_t i;
    int lastTag = 0;
    int tag;
    size_t valueLength;
    int haveActive = 0;
    int active = 0;
    int slots = 0;
    int rc;

    if (length < STORE_MIN_BYTES) {
        return GDX_PROFILE_ERR_TRUNCATED;
    }
    if (length > GDX_PROFILE_STORE_BLOCK_MAX_BYTES) {
        return GDX_PROFILE_ERR_LAYOUT;
    }
    if (memcmp(data, "GPS1", 4) != 0) {
        return GDX_PROFILE_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != PROFILE_VERSION) {
        return GDX_PROFILE_ERR_VERSION;
    }
    end = length - PROFILE_CRC_BYTES;
    if (get_u32le(data + end) != crc32_ieee(data, end)) {
        return GDX_PROFILE_ERR_CRC;
    }

    records = get_u16le(data + 6);
    pos = GDX_PROFILE_STORE_HEADER_SIZE;
    for (i = 0; i < records; i++) {
        if (!bytes_fit(pos, end, GDX_PROFILE_RECORD_HEADER_SIZE)) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        tag = data[pos];
        valueLength = get_u16le(data + pos + 1);
        pos += GDX_PROFILE_RECORD_HEADER_SIZE;
        if ((tag <= lastTag) || !bytes_fit(pos, end, valueLength)) {
            return GDX_PROFILE_ERR_LAYOUT;
        }
        lastTag = tag;
        if (tag == GDX_PROFILE_STORE_TAG_ACTIVE) {
            if (valueLength != PROFILE_STORE_ACTIVE_BYTES) {
                return GDX_PROFILE_ERR_LAYOUT;
            }
            active = data[pos];
            haveActive = 1;
        } else if ((tag >= GDX_PROFILE_STORE_TAG_SLOT) && (tag < GDX_PROFILE_STORE_TAG_SLOT + GDX_PROFILE_CAPACITY)) {
            if (tag != GDX_PROFILE_STORE_TAG_SLOT + slots) {
                return GDX_PROFILE_ERR_LAYOUT;
            }
            rc = decode_profile_block(data + pos, valueLength, &out->profiles[slots]);
            if (rc != GDX_PROFILE_OK) {
                return rc;
            }
            slots++;
        }
        pos += valueLength;
    }
    if ((pos != end) || !haveActive) {
        return GDX_PROFILE_ERR_LAYOUT;
    }
    out->count = slots;
    out->active = active;
    return store_state_ok(out);
}

int gdx_profile_store_decode(const unsigned char* data, size_t length, GdxProfileStore* out) {
    int rc;

    if (out == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    if (data == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    rc = decode_store_block(data, length, out);
    if (rc != GDX_PROFILE_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

// Alpha-weighted box filter for crops larger than the icon. Every output cell averages at least one source pixel.
static void shrink_box(const unsigned char* src, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t side, unsigned char* dst) {
    uint32_t oy;
    uint32_t ox;
    uint32_t sx;
    uint32_t sy;

    for (oy = 0; oy < GDX_PROFILE_ICON_SIZE; oy++) {
        uint32_t syBegin = oy * side / GDX_PROFILE_ICON_SIZE;
        uint32_t syEnd = (oy + 1u) * side / GDX_PROFILE_ICON_SIZE;

        for (ox = 0; ox < GDX_PROFILE_ICON_SIZE; ox++) {
            uint32_t sxBegin = ox * side / GDX_PROFILE_ICON_SIZE;
            uint32_t sxEnd = (ox + 1u) * side / GDX_PROFILE_ICON_SIZE;
            uint32_t sumR = 0;
            uint32_t sumG = 0;
            uint32_t sumB = 0;
            uint32_t sumA = 0;
            uint32_t count = 0;
            unsigned char* d = dst + ((size_t)oy * GDX_PROFILE_ICON_SIZE + ox) * 4u;

            for (sy = syBegin; sy < syEnd; sy++) {
                for (sx = sxBegin; sx < sxEnd; sx++) {
                    const unsigned char* p = src + ((size_t)(y0 + sy) * stride + (x0 + sx)) * 4u;
                    uint32_t a = p[3];

                    sumR += a * p[0];
                    sumG += a * p[1];
                    sumB += a * p[2];
                    sumA += a;
                    count++;
                }
            }
            if (sumA == 0) {
                d[0] = 0;
                d[1] = 0;
                d[2] = 0;
                d[3] = 0;
            } else {
                d[0] = (unsigned char)round_div(sumR, sumA);
                d[1] = (unsigned char)round_div(sumG, sumA);
                d[2] = (unsigned char)round_div(sumB, sumA);
                d[3] = (unsigned char)round_div(sumA, count);
            }
        }
    }
}

// Centre-sampled nearest neighbour for crops at or below the icon size. A side of exactly 64 is an identity copy.
static void nearest_scale(const unsigned char* src, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t side, unsigned char* dst) {
    uint32_t oy;
    uint32_t ox;

    for (oy = 0; oy < GDX_PROFILE_ICON_SIZE; oy++) {
        uint32_t sy = y0 + ((2u * oy + 1u) * side) / (2u * GDX_PROFILE_ICON_SIZE);

        for (ox = 0; ox < GDX_PROFILE_ICON_SIZE; ox++) {
            uint32_t sx = x0 + ((2u * ox + 1u) * side) / (2u * GDX_PROFILE_ICON_SIZE);

            memcpy(dst + ((size_t)oy * GDX_PROFILE_ICON_SIZE + ox) * 4u, src + ((size_t)sy * stride + sx) * 4u, 4);
        }
    }
}

int gdx_profile_icon_from_rgba(const unsigned char* src, int srcWidth, int srcHeight, unsigned char* dst) {
    uint32_t width;
    uint32_t height;
    uint32_t side;
    uint32_t x0;
    uint32_t y0;

    if ((src == NULL) || (dst == NULL)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if ((srcWidth < 1) || (srcHeight < 1) || (srcWidth > GDX_PROFILE_ICON_SRC_MAX) || (srcHeight > GDX_PROFILE_ICON_SRC_MAX)) {
        return GDX_PROFILE_ERR_ARGS;
    }
    width = (uint32_t)srcWidth;
    height = (uint32_t)srcHeight;
    side = (width < height) ? width : height;
    x0 = (width - side) / 2u;
    y0 = (height - side) / 2u;
    if (side > GDX_PROFILE_ICON_SIZE) {
        shrink_box(src, width, x0, y0, side, dst);
    } else {
        nearest_scale(src, width, x0, y0, side, dst);
    }
    return GDX_PROFILE_OK;
}

int gdx_profile_icon_solid(unsigned char* dst, uint32_t colour) {
    size_t i;

    if (dst == NULL) {
        return GDX_PROFILE_ERR_ARGS;
    }
    if (colour > 0xFFFFFFu) {
        return GDX_PROFILE_ERR_VALUE;
    }
    for (i = 0; i < GDX_PROFILE_ICON_BYTES; i += 4) {
        dst[i] = (unsigned char)((colour >> 16) & 0xFFu);
        dst[i + 1] = (unsigned char)((colour >> 8) & 0xFFu);
        dst[i + 2] = (unsigned char)(colour & 0xFFu);
        dst[i + 3] = 0xFFu;
    }
    return GDX_PROFILE_OK;
}
