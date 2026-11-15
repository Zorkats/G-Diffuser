/* port/gdx_net_spectate.h -- WATCH in a G-NET room's race: the camera follows another human.
 * Copyright (c) Zorkats
 *
 * Once the copy's own machine has finished or is out, the quit box's first row reads WATCH. Choosing it turns
 * spectating on: camera 0 follows another human of the room instead of the copy's own machine. The HUD, the
 * controller and everything else stay on the copy's own racer; only the camera's racer changes
 * (decomp/src/game/camera.c, Camera_GdxRacerId).
 *
 *   - WATCH picks the nearest human ahead who still drives, else the nearest behind. With nobody driving it
 *     picks the nearest human that is not this copy's, so the camera has someone to show.
 *   - L and R go to the human ahead and behind among those who still drive, round the field. CPUs are never
 *     watched.
 *   - Z toggles the camera between the race camera behind the machine and the stock trackside cinematic,
 *     which starts over on the watched machine each time it ends.
 *   - When the watched machine finishes or is out, the camera goes to the nearest human who still drives.
 *     With nobody left it stays on the last one.
 *
 * The race bridge supplies the field (gdx_net_race_spectate_field) and the names (gdx_net_race_spectate_name)
 * and resets this module when a race is set up and when this copy leaves one. Those two functions are this
 * module's whole view of the game, so the tests replace them.
 *
 * Plain numbers in and out: the game's camera and menu code is built without the port's headers and declares
 * what it calls itself. Game thread only. */

#ifndef GDX_NET_SPECTATE_H
#define GDX_NET_SPECTATE_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_SPECTATE_HUMANS 12 /* GDX_NET_RACE_HUMANS */
#define GDX_NET_SPECTATE_NAME_BYTES 24
#define GDX_NET_SPECTATE_LABEL_BYTES 40

/* One human of the race as the bridge sees it this frame. */
typedef struct GdxNetSpectateHuman {
    int racer;    /* the game's racer id (gRacers index) */
    int position; /* the race position, 1 for the leader */
    int driving;  /* still in the race: not finished, not crashed, not retired */
} GdxNetSpectateHuman;

typedef struct GdxNetSpectateField {
    int count;
    int self; /* the index in human of this copy's own machine, or -1 */
    GdxNetSpectateHuman human[GDX_NET_SPECTATE_HUMANS];
} GdxNetSpectateField;

/* Supplied by the race bridge. field: the humans of the race this frame, 0 outside a room's race (field is
   then not read). name: the human's name as the small stock font draws it, or "PLAYER n" from its place on
   the grid. */
int gdx_net_race_spectate_field(GdxNetSpectateField* field);
void gdx_net_race_spectate_name(int racer, char* out, int size);

/* WATCH was chosen. 1 when the camera now follows someone (or already did), 0 when there is nobody else to
   watch. */
int gdx_net_spectate_watch(void);

/* One frame while spectating: prev and next went down this frame (L and R), toggle too (Z). Follows the
   watched machine's fate as well, so it runs every frame of the race, presses or not. */
void gdx_net_spectate_frame(int prev, int next, int toggle);

/* 1 while the camera follows another human. */
int gdx_net_spectate_on(void);

/* The racer camera cameraId follows: the watched racer for camera 0 while spectating, else cameraId. */
int gdx_net_spectate_racer(int cameraId);

/* 1 while the camera is the trackside cinematic, 0 the race camera. */
int gdx_net_spectate_cinematic(void);

/* Counts every change of what the camera should show: spectating on, another racer, the other camera. The
   camera starts its mode over when it sees a new number. */
unsigned gdx_net_spectate_generation(void);

/* The label's two lines: "WATCHING " and the watched human's name, and the buttons. "" while not
   spectating. */
void gdx_net_spectate_label(char* out, int size);
const char* gdx_net_spectate_hint(void);

/* Spectating off. The bridge calls it when a race is set up and when this copy leaves one. */
void gdx_net_spectate_reset(void);

#ifdef __cplusplus
}
#endif

#endif
