/* port/gdx_net_track_save.h -- "Save to my tracks": keeping a G-NET room's custom track (O4.9).
 * Copyright (c) Zorkats
 *
 * A room race never touches a save (plan decision 13). The one way a track a player raced reaches their
 * Edit Cup is this row of the results page (plan decision 14). It takes the room's verified copy from the
 * lobby again (gdx_lan_race_track), checks its hash on this thread, stages it as an import
 * (gdx_snapshot_import_stage_content) and queues the install on the disk thread
 * (gdx_content_import_room_track_begin), which also puts it in the first free Edit Cup slot. With every slot
 * full the row says so and nothing is imported.
 *
 * The answer belongs to one race: a new race, or another track, starts over at READY. An install queued
 * before the page closed is still collected by gdx_net_track_save_poll, which the room screen runs every
 * frame, and the result is only logged then.
 *
 * Game thread only, the room screen's thread. */

#ifndef GDX_NET_TRACK_SAVE_H
#define GDX_NET_TRACK_SAVE_H

#include "gdx_net_room_view.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_TRACK_SAVE_NONE 0   /* no row: not a results page of a custom track this copy holds */
#define GDX_NET_TRACK_SAVE_READY 1  /* the row offers to save */
#define GDX_NET_TRACK_SAVE_SAVING 2 /* queued on the disk thread */
#define GDX_NET_TRACK_SAVE_SAVED 3
#define GDX_NET_TRACK_SAVE_FULL 4   /* every Edit Cup slot holds a track: nothing was imported */
#define GDX_NET_TRACK_SAVE_FAILED 5 /* the row may be pressed again */
#define GDX_NET_TRACK_SAVE_OWN 6    /* the track already sits in this copy's Edit Cup */

/* What the row of view's results page shows. */
int gdx_net_track_save_state(const GdxNetRoomView* view);

/* The row was pressed: starts the save when the state allows it. Returns the state after the press; a disk
 * that is busy for a moment leaves it at READY. */
int gdx_net_track_save_begin(const GdxNetRoomView* view);

/* Collects a finished install. */
void gdx_net_track_save_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_TRACK_SAVE_H */
