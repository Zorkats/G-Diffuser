/* port/gdx_staff_ghost.c -- see gdx_staff_ghost.h. */

#include "gdx_staff_ghost.h"

#include <string.h>

static void put_u16le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char) (value & 0xFFu);
    p[1] = (unsigned char) ((value >> 8) & 0xFFu);
}

static void put_u32le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char) (value & 0xFFu);
    p[1] = (unsigned char) ((value >> 8) & 0xFFu);
    p[2] = (unsigned char) ((value >> 16) & 0xFFu);
    p[3] = (unsigned char) ((value >> 24) & 0xFFu);
}

static uint32_t get_u16le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

static uint32_t get_u32le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int machine_ok(const unsigned char* machine) {
    return (machine[GDX_STAFF_GHOST_MACHINE_CHARACTER] <= GDX_STAFF_GHOST_CHARACTER_MAX) &&
           (machine[GDX_STAFF_GHOST_MACHINE_CUSTOM_TYPE] <= GDX_STAFF_GHOST_CUSTOM_TYPE_MAX) &&
           (machine[GDX_STAFF_GHOST_MACHINE_FRONT] <= GDX_STAFF_GHOST_PART_MAX) &&
           (machine[GDX_STAFF_GHOST_MACHINE_REAR] <= GDX_STAFF_GHOST_PART_MAX) &&
           (machine[GDX_STAFF_GHOST_MACHINE_WING] <= GDX_STAFF_GHOST_PART_MAX) &&
           (machine[GDX_STAFF_GHOST_MACHINE_LOGO] <= GDX_STAFF_GHOST_LOGO_MAX) &&
           (machine[GDX_STAFF_GHOST_MACHINE_NUMBER] <= GDX_STAFF_GHOST_NUMBER_MAX) &&
           (machine[GDX_STAFF_GHOST_MACHINE_DECAL] <= GDX_STAFF_GHOST_DECAL_MAX);
}

static int fields_ok(uint32_t raceTime, const uint32_t* lapTimes, uint32_t replayEnd, uint32_t replaySize,
                     const unsigned char* machine) {
    int i;

    if ((raceTime < 1) || (raceTime > GDX_STAFF_GHOST_TIME_MAX)) {
        return 0;
    }
    for (i = 0; i < GDX_STAFF_GHOST_LAPS; i++) {
        if ((lapTimes[i] < 1) || (lapTimes[i] > raceTime)) {
            return 0;
        }
    }
    if ((replaySize < 1) || (replaySize > GDX_STAFF_GHOST_REPLAY_MAX) || (replayEnd > replaySize)) {
        return 0;
    }
    return machine_ok(machine);
}

int gdx_staff_ghost_validate(const GdxStaffGhost* ghost) {
    if (ghost == NULL) {
        return GDX_STAFF_GHOST_ERR_ARGS;
    }
    return fields_ok(ghost->raceTime, ghost->lapTimes, ghost->replayEnd, ghost->replaySize, ghost->machine)
               ? GDX_STAFF_GHOST_OK
               : GDX_STAFF_GHOST_ERR_VALUE;
}

int gdx_staff_ghost_encode(const GdxStaffGhost* ghost, unsigned char* out, size_t cap) {
    size_t length;
    int i;
    int rc;

    if ((ghost == NULL) || (out == NULL)) {
        return GDX_STAFF_GHOST_ERR_ARGS;
    }
    rc = gdx_staff_ghost_validate(ghost);
    if (rc != GDX_STAFF_GHOST_OK) {
        return rc;
    }
    length = GDX_STAFF_GHOST_HEADER_BYTES + (size_t) ghost->replaySize;
    if (cap < length) {
        return GDX_STAFF_GHOST_ERR_FULL;
    }
    put_u32le(out, ghost->raceTime);
    for (i = 0; i < GDX_STAFF_GHOST_LAPS; i++) {
        put_u32le(out + 4 + 4 * i, ghost->lapTimes[i]);
    }
    put_u32le(out + 0x10, ghost->replayEnd);
    put_u16le(out + 0x14, ghost->replaySize);
    memcpy(out + 0x16, ghost->machine, GDX_STAFF_GHOST_MACHINE_BYTES);
    memcpy(out + GDX_STAFF_GHOST_HEADER_BYTES, ghost->replay, ghost->replaySize);
    return (int) length;
}

int gdx_staff_ghost_check(const unsigned char* data, size_t length, uint32_t* raceTime) {
    uint32_t lapTimes[GDX_STAFF_GHOST_LAPS];
    uint32_t replaySize;
    int i;

    if (data == NULL) {
        return GDX_STAFF_GHOST_ERR_ARGS;
    }
    if (length < GDX_STAFF_GHOST_HEADER_BYTES) {
        return GDX_STAFF_GHOST_ERR_LAYOUT;
    }
    replaySize = get_u16le(data + 0x14);
    if (length != GDX_STAFF_GHOST_HEADER_BYTES + (size_t) replaySize) {
        return GDX_STAFF_GHOST_ERR_LAYOUT;
    }
    for (i = 0; i < GDX_STAFF_GHOST_LAPS; i++) {
        lapTimes[i] = get_u32le(data + 4 + 4 * i);
    }
    if (!fields_ok(get_u32le(data), lapTimes, get_u32le(data + 0x10), replaySize, data + 0x16)) {
        return GDX_STAFF_GHOST_ERR_VALUE;
    }
    if (raceTime != NULL) {
        *raceTime = get_u32le(data);
    }
    return GDX_STAFF_GHOST_OK;
}

int gdx_staff_ghost_decode(const unsigned char* data, size_t length, GdxStaffGhost* out) {
    int rc;
    int i;

    if (out == NULL) {
        return GDX_STAFF_GHOST_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    rc = gdx_staff_ghost_check(data, length, NULL);
    if (rc != GDX_STAFF_GHOST_OK) {
        return rc;
    }
    out->raceTime = get_u32le(data);
    for (i = 0; i < GDX_STAFF_GHOST_LAPS; i++) {
        out->lapTimes[i] = get_u32le(data + 4 + 4 * i);
    }
    out->replayEnd = get_u32le(data + 0x10);
    out->replaySize = (uint16_t) get_u16le(data + 0x14);
    memcpy(out->machine, data + 0x16, GDX_STAFF_GHOST_MACHINE_BYTES);
    memcpy(out->replay, data + GDX_STAFF_GHOST_HEADER_BYTES, out->replaySize);
    return GDX_STAFF_GHOST_OK;
}
