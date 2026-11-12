/* port/gdx_net_room_course.h -- a G-NET room's custom track, raced from memory (O4.8).
 * Copyright (c) Zorkats
 *
 * A room races a custom track in the first Edit Cup course slot, COURSE_EDIT_1, but never from that slot's
 * file: the track is the room's verified copy, held in memory by the lobby (gdx_net_race_room.h,
 * gdx_lan_race_track). When the lock-in screen arms the race, prepare() takes those bytes, checks them again
 * (net/gdx_net_track_pack.h) and decodes everything the race reads: the course image Course_Load copies, the
 * forks, the track settings and the skybox record. The race then reads only those copies. No store, no
 * Edit Cup slot, no record and no ghost is written: a room race never touches a save (plan decision 13).
 *
 * A room course is taken by the Course_Load of the room's own race only: the bridge says when that race is
 * armed or being run (gdx_net_race_room_course_held), and the caller says when the race is a one-player
 * Practice race, which is how every room race runs. Any other Course_Load drops it.
 *
 * Plain C, and no <stdint.h>: the decomp's libc headers do not have it. Game thread only. */

#ifndef GDX_NET_ROOM_COURSE_H
#define GDX_NET_ROOM_COURSE_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_ROOM_COURSE_TRACK_CUSTOM 0xFD /* net::lobby::kTrackCustom */
#define GDX_NET_ROOM_COURSE_INDEX 24          /* COURSE_EDIT_1 */

/* The lock-in screen armed the room's race on track (a stock course, or GDX_NET_ROOM_COURSE_TRACK_CUSTOM).
 * For a custom track it reads and decodes the room's copy: 1 when the race can run on it, 0 when this copy
 * holds no verified track or the bytes do not decode. A stock track drops any room course and returns 1. */
int gdx_net_room_course_prepare(int track);

/* Course_Load, before it reads anything. roomRace is 1 when the course being loaded is for the room's own
 * race: armed or being run, one player, Practice. 1 when the room course is taken for courseIndex; it then
 * stays active until the next Course_Load or end(). Every other load drops the active state. */
int gdx_net_room_course_take(int courseIndex, int roomRace);

/* Copies the room course image over runtime (a CourseContext of size bytes). 0 when there is no taken room
 * course or size is not its size; nothing is written then. */
int gdx_net_room_course_copy(void* runtime, unsigned long size);

/* 1 while the course raced is the room's custom track. */
int gdx_net_room_course_active(void);

/* The loaded course did not check out: the race must not run. Sticks until the next prepare() or end(). */
void gdx_net_room_course_refuse(void);
int gdx_net_room_course_refused(void);

/* The race's course init: the active course's forks and settings become the active sets
 * (gdx_course_branch_format.h, gdx_course_settings.h). Settings whose points belong to another ring than
 * ringCount keep their course-wide values only, as gdx_settings_view_select_course does. */
void gdx_net_room_course_apply_forks(void);
void gdx_net_room_course_apply_settings(int ringCount);

/* The stock skybox (0..7) the active course's skybox record resolves to in this session, or -1 when it has
 * none or its key is not registered here: the course's own skybox byte then stands. */
int gdx_net_room_course_skybox(void);

/* The room's race is over on this copy: drops the course and the extended height it forced. */
void gdx_net_room_course_end(void);

/* The race bridge (gdx_net_race_bridge.c): 1 while the room's race is armed or being run. */
int gdx_net_race_room_course_held(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_ROOM_COURSE_H */
