/* port/gdx_staff_ghost.h -- the staff ghost of a custom track: the replay of the Time Attack run that set its staff
 * time. Pure data: no file IO, no game headers.
 *
 * The game keeps Edit Cup ghosts in the disk's GOST area, which an export never carries (gdx_content_io.c). So the
 * staff run's replay travels as GXM1 tag 7 (gdx_course_meta.h), next to the staff time and the splits it belongs to.
 * Time Attack offers it as the STAFF ghost of that track (decomp/src/overlays/ovl_i2/save.c).
 *
 * The replay is the game's own byte stream (racer.c func_80094FF4): signed position deltas, one byte each, or 0x80
 * and a 16-bit value. Bytes, so it reads the same on every host.
 *
 * Stored form (little-endian, no checksum: the GXM1 block around it has one):
 *
 *   0x00  raceTime    u32, 1..GDX_STAFF_GHOST_TIME_MAX, the game timer's milliseconds
 *   0x04  lapTimes    GDX_STAFF_GHOST_LAPS x u32, each lap's own time, 1..raceTime
 *   0x10  replayEnd   u32, 0..replaySize: where the finished lap ends in the replay (Ghost.replayEnd)
 *   0x14  replaySize  u16, 1..GDX_STAFF_GHOST_REPLAY_MAX
 *   0x16  machine     GDX_STAFF_GHOST_MACHINE_BYTES, MachineInfo as the game fills it (racer.c func_80089A74)
 *   0x2A  replay      replaySize bytes
 *
 * Machine bytes index the game's machine and texture tables when the ghost is drawn, so each one is range checked:
 * an imported track cannot point the draw code past a table.
 */

#ifndef GDX_STAFF_GHOST_H
#define GDX_STAFF_GHOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Time Attack ghosts are three-lap runs. */
#define GDX_STAFF_GHOST_LAPS 3
/* Ghost.replayData (decomp/include/unk_structs.h). */
#define GDX_STAFF_GHOST_REPLAY_MAX 16200
/* MachineInfo (decomp/include/unk_structs.h). */
#define GDX_STAFF_GHOST_MACHINE_BYTES 20
/* GDX_META_STAFF_TIME_MAX: one millisecond short of the game's MAX_TIMER. */
#define GDX_STAFF_GHOST_TIME_MAX (60u * 60u * 1000u - 2u)

#define GDX_STAFF_GHOST_HEADER_BYTES (4 + 4 * GDX_STAFF_GHOST_LAPS + 4 + 2 + GDX_STAFF_GHOST_MACHINE_BYTES)
#define GDX_STAFF_GHOST_VALUE_MAX_BYTES (GDX_STAFF_GHOST_HEADER_BYTES + GDX_STAFF_GHOST_REPLAY_MAX)

/* MachineInfo byte offsets and the largest value each one may hold. */
#define GDX_STAFF_GHOST_MACHINE_CHARACTER 0
#define GDX_STAFF_GHOST_MACHINE_CUSTOM_TYPE 1
#define GDX_STAFF_GHOST_MACHINE_FRONT 2
#define GDX_STAFF_GHOST_MACHINE_REAR 3
#define GDX_STAFF_GHOST_MACHINE_WING 4
#define GDX_STAFF_GHOST_MACHINE_LOGO 5
#define GDX_STAFF_GHOST_MACHINE_NUMBER 6
#define GDX_STAFF_GHOST_MACHINE_DECAL 7
#define GDX_STAFF_GHOST_CHARACTER_MAX 29   /* 30 machines */
#define GDX_STAFF_GHOST_CUSTOM_TYPE_MAX 4  /* CUSTOM_MACHINE_SUPER_CAT */
#define GDX_STAFF_GHOST_PART_MAX 6         /* 7 fronts, rears and wings */
#define GDX_STAFF_GHOST_LOGO_MAX 7         /* sLogoTextures */
#define GDX_STAFF_GHOST_NUMBER_MAX 30      /* sMachineNumberTextures */
#define GDX_STAFF_GHOST_DECAL_MAX 4        /* sDecalTextures */

#define GDX_STAFF_GHOST_OK 0
#define GDX_STAFF_GHOST_ERR_ARGS (-1)  /* NULL pointer */
#define GDX_STAFF_GHOST_ERR_FULL (-2)  /* out is too small */
#define GDX_STAFF_GHOST_ERR_VALUE (-3) /* a field breaks its rules */
#define GDX_STAFF_GHOST_ERR_LAYOUT (-4) /* the length does not match the replay size */

typedef struct GdxStaffGhost {
    uint32_t raceTime;
    uint32_t lapTimes[GDX_STAFF_GHOST_LAPS];
    uint32_t replayEnd;
    uint16_t replaySize;
    unsigned char machine[GDX_STAFF_GHOST_MACHINE_BYTES];
    unsigned char replay[GDX_STAFF_GHOST_REPLAY_MAX];
} GdxStaffGhost;

/* GDX_STAFF_GHOST_OK when every field follows the rules above. */
int gdx_staff_ghost_validate(const GdxStaffGhost* ghost);

/* Writes the stored form. Returns its length, or a negative code. */
int gdx_staff_ghost_encode(const GdxStaffGhost* ghost, unsigned char* out, size_t cap);

/* Checks a stored form in place, without copying the replay. GDX_STAFF_GHOST_OK and, when raceTime is not NULL,
 * the run's race time. */
int gdx_staff_ghost_check(const unsigned char* data, size_t length, uint32_t* raceTime);

/* Reads a stored form. out is zeroed on every failure. */
int gdx_staff_ghost_decode(const unsigned char* data, size_t length, GdxStaffGhost* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX_STAFF_GHOST_H */
