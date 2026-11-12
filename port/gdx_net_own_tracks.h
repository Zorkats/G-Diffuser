/* port/gdx_net_own_tracks.h -- the Edit Cup's tracks as a G-NET room sees them (O4.7).
 * Copyright (c) Zorkats
 *
 * When the host allows custom tracks, a racer may vote for or pick a track of their own Edit Cup. A room knows
 * a track by its identity, the SHA-256 of its content bytes (net/gdx_net_track_pack.h). So each slot's track is
 * read without a single write to the save (gdx_snapshot_content_peek, G-NET decision 8), hashed, and checked as
 * a receiver checks it. Its bytes then go to the room session (gdx_lan_room_keep_track), which sends them if
 * the track is chosen.
 *
 * Game thread only, the thread of the course select. A read is refused while the disk thread owns the save
 * (gdx_content_mfs_busy); the slot then reads as unavailable until a later call finds the disk idle. */

#ifndef GDX_NET_OWN_TRACKS_H
#define GDX_NET_OWN_TRACKS_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_OWN_TRACK_SLOTS 6

/* 1 when Edit Cup slot, 0 to 5, holds a track a room can race, with its identity in id
 * (GDX_NET_ROOM_TRACK_ID_BYTES bytes) and its title in title. Either may be NULL. A slot's answer is kept
 * until gdx_net_own_tracks_forget, except a refusal while the disk was busy, which is asked again. */
int gdx_net_own_track(int slot, unsigned char* id, char* title, int titleSize);
/* Drops every kept answer: the next call reads the slots again, as a track may have changed since. */
void gdx_net_own_tracks_forget(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_OWN_TRACKS_H */
