/* port/gdx_export_all.h -- what "Export everything" puts in its zip. Pure: no file IO, no game headers.
 *
 * The export walks the data directory (and the executable's directory when it is a different one) and asks
 * gdx_export_classify for every file it finds. The answer is the one group the file belongs to, or 0 when it
 * never leaves the install. The player picks the groups; a file goes into the zip only when its group is picked.
 *
 * Never exported, whatever the player picks:
 *   - ROMs and disk images (.z64 .n64 .v64 .ndd .d64 .rom), wherever they sit
 *   - archives built from the ROM: fzerox.o2r, fzerox-disk.o2r, generic.o2r, n64ddipl.o2r, and every ROM-hack
 *     archive under mods/~romhacks/, which the ROM Hacks window rebuilds from the player's own hack ROM
 *   - exports/machines/, the machine model captures (meshes and textures read out of the running game)
 *   - the files the game ships with (gdiffuser.o2r, shaders, fonts, licences, the controller database),
 *     the dump and recipe folders, logs, shader caches, crash notes, lock files and old .bak copies
 *   - earlier exports of this kind (exports/gdiffuser-export-*.zip), so a zip never nests the last one
 *
 * Paths are relative, '/' separated, as found on disk. Matching ignores ASCII case, as Windows does. A path that
 * is absolute, empty or climbs with ".." is refused.
 */
#ifndef GDX_EXPORT_ALL_H
#define GDX_EXPORT_ALL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Groups, one bit each. */
#define GDX_EXPORT_TRACKS 0x1u   /* tracks and snapshots: the disk save, recovery and snapshots, track stores, exports */
#define GDX_EXPORT_PLAYER 0x2u   /* profiles, saves, records and ghosts */
#define GDX_EXPORT_SETTINGS 0x4u /* settings and mods: the config, texture and other packs, music */
#define GDX_EXPORT_KEYS 0x8u     /* the profiles' signing keys: whoever holds them can sign tracks as you */
#define GDX_EXPORT_ALL_GROUPS (GDX_EXPORT_TRACKS | GDX_EXPORT_PLAYER | GDX_EXPORT_SETTINGS | GDX_EXPORT_KEYS)

/* The prefix of the zips this export writes, inside exports/. */
#define GDX_EXPORT_FILE_PREFIX "gdiffuser-export-"

/* The group of one file, or 0 when it is never exported. */
unsigned gdx_export_classify(const char* relPath);

/* Whether a folder can hold anything exported, so a walk can skip the rest (dump/, logs/, mods/~romhacks/...).
 * relPath is the folder's relative path without a trailing '/'. */
int gdx_export_folder_wanted(const char* relPath);

/* The display name of a group bit, "" for anything else. */
const char* gdx_export_group_name(unsigned group);

/* Writes the export's file name for a date into out: "gdiffuser-export-YYYYMMDD-HHMMSS.zip". Returns the length,
 * or -1 when a field is out of range or out is too small. */
int gdx_export_file_name(int year, int month, int day, int hour, int minute, int second, char* out, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif
