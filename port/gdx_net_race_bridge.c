// Copyright (c) Zorkats
//
// port/gdx_net_race_bridge.c: the half of an online race that sees the decomp Racer layout (milestone O3,
// slices S2 to S6). The poses, their buffer, the grid and the intro timing are in port/net/gdx_net_race.h.
//
// Remote racers (S2). Calls from decomp/src/game/racer.c, each one line there:
//   Racer_Init            gdx_net_race_begin          a race starts: forget the last one
//   Racer_UpdateMovement  gdx_net_race_puppet_update  a remote racer: write its pose, run no physics
//   Racer_Update          gdx_net_race_tick           once per tick: poses go out, and the ones that came go in
//   Racer_Update          gdx_net_race_contact        two machines first touch (a developer log)
//   Racer_Update          gdx_net_race_pushed         the pair loop pushed two machines apart (S6)
// racer.c also asks gdx_net_race_is_puppet in Racer_ReceiveDamage and Racer_HitWall, which do nothing
// to a puppet: what a wall or a hit did to that machine is its owner's to say.
//
// The start of a room's race (S3). The room's race is a one-player Practice race under Single Race rules
// (gdx_single_race.h) with the room's field, laps and rules:
//   lock-in screen        gdx_net_race_arm             the room moved on: is the next race ours?
//   Race_Init, first      gdx_net_race_armed_seed      (through gdx_det_race_init) the room's seed
//   Racer_Init            gdx_net_race_begin           the field: who drives which slot, with what machine
//   Racer_SetupStart...   gdx_net_race_grid_positions  the grid order
//   Race_Init, last       gdx_net_race_loaded          tell the room, with the digest of our track
//   Race_Update, first    gdx_net_race_frame           wait for the start, or leave when the room says so
//   Racer_Update, intro   gdx_net_race_intro_gate/_after  the countdown runs on the host clock
// Every copy holds its first frame until the start time the host sent, so the intro and its GO are the
// same moment for everyone, whoever loaded faster.
//
// A puppet is written the way the game writes a Time Attack ghost (Racer_Update): the position goes in,
// the place on the track is found again, and the track shape code gives the shadow and the height. Unlike
// a ghost its facing and its velocity come from its owner, and it keeps its lap, so it has a race position.
//
// Poses between copies (S4). Every second tick each copy sends the pose of the machines it drives: its
// own, and on the host every CPU. The host passes each player's pose on to the others. The poses that
// arrived are taken in once per tick. A remote racer that nothing was heard from stands where the grid
// put it.
//
// Contact (S6). A pose is a few ticks old when it is used, and a machine covers more than its own length
// in that time. So every remote machine is shown where its owner has it now: its newest pose, carried
// ahead along the road for as long as the room's clock says the pose is old (gdx_net_race.h). Two copies
// then see two machines in the same places, and each runs the game's own contact code on what it shows:
// each owner's machine is pushed and damaged on its own copy. The push that code gives a remote machine
// is kept (gdx_net_race_pushed) and shown until the owner's poses hold the bump themselves.
//
// How a race ends (S5). Each copy tells the room once how its own race ended: over the line with the
// game's own time, or out of it. The host's game also hands in its order of the humans twice a second,
// which places whoever is still driving when the room calls the race. A pose carries its owner's state
// flags, so a remote machine that finished or retired there is marked the same here: finished with the
// time on our own clock when we saw it, which the host's table then replaces for a human, or retired to
// the back of the order. The game's own results screen and its retry menu stay away (menus.c): this copy
// keeps the race on screen until the room leaves it, and the room page shows the table.

#include "global.h"
#include "unk_structs.h"
#include "fzx_course.h"
#include "fzx_game.h"
#include "fzx_racer.h"
#include "net/gdx_net_race.h"
#include "gdx_net_race_room.h"
#include "gdx_net_race_box.h"
#include "gdx_net_room_course.h"

extern s32 gGameMode;

#ifdef EXPANSION_KIT
extern CustomMachinesInfo gCustomMachinesInfo;
// A machine record is the game's CustomMachine, byte for byte (G-NET O4.5).
typedef char GdxNetRecordIsCustomMachine[(sizeof(CustomMachine) == GDX_NET_RACE_RECORD_BYTES) ? 1 : -1];
#endif

// The Create Machine record of the local player's machine for character (gdx_net_race_room.h). Read on the
// lock-in screen, so what travels is what this player drove on the select screen.
int gdx_net_race_local_record(int character, unsigned char* out) {
#ifdef EXPANSION_KIT
    const u8* src;
    s32 slot;
    s32 i;

    if ((character < 0) || (character >= GDX_NET_RACE_MAX_RACERS) || (out == NULL)) {
        return 0;
    }
    slot = Character_GetSlotFromCharacter(character);
    if (gCustomMachinesInfo.characterCustomState[slot] <= 0) {
        return 0; // stock, or a super machine: those race stock in a room
    }
    src = (const u8*) &gCustomMachinesInfo.customMachines[slot];
    for (i = 0; i < GDX_NET_RACE_RECORD_BYTES; i++) {
        out[i] = src[i];
    }
    return 1;
#else
    (void) character;
    (void) out;
    return 0;
#endif
}

// The game's machines go back to the player's own once a room's race is over: Create Machine records,
// unlocked super machines and the machine stat editor (func_8008D33C).
static void gdx_net_race_machines_restore(void) {
    if (gdx_net_race_machines_set()) {
        gdx_net_race_clear_machines();
        func_8008D33C();
    }
}
extern s32 gNumPlayers;
extern s8 gTitleDemoState;
extern s32 gRaceIntroTimer;
extern CourseInfo* gCurrentCourseInfo;
extern void (*D_800CF4B8[])(Racer*); // the track shape callbacks, by TRACK_SHAPE_INDEX

extern s16 gRacersFinished;
extern void Racer_RetireRacer(Racer* racer);

extern int gdx_net_race_dev_echo_ticks(void);
extern int gdx_net_race_dev_autopilot(void);
extern int gdx_net_race_dev_contacts(void);
extern int gdx_net_race_dev_trace_on(void);
extern void gdx_net_race_dev_trace(unsigned hostMs, int raceTicks, int slot, int driven, float x, float y, float z);
extern void gdx_net_race_dev_trace_tick(void);
extern void gdx_single_race_force_next(int laps, int racers); // gdx_single_race.c
extern void gdx_dbg_logf(const char* fmt, ...);                // the port log, on in every build

#define GDX_NET_ECHO_SLOT 1
// Two machines on exactly the same spot make the stock contact code divide by zero, and an echo of a
// machine standing still is exactly that. The echo is shown this far to the side instead.
#define GDX_NET_ECHO_SIDE 60.0f

static u32 sTick;
static f32 sEchoSide;
// The room's clock, read once a tick: every pose and every puppet of the tick gets the same reading.
static u32 sClockTick; // the tick it was read for, plus one; 0 before the first reading
static u32 sClockMs;
static s32 sClockOk;
// The place on the track under each puppet's newest pose, kept from one tick to the next so the search
// for the road under its guess starts next to it.
static RacerSegmentPositionInfo sRoadSeat[GDX_NET_RACE_MAX_RACERS];
static u8 sRoadSeated[GDX_NET_RACE_MAX_RACERS];

// The room's race: armed when the lock-in screen leaves, taken by the Racer_Init that follows.
static GdxNetRaceRoom sRoom;
static GdxNetGrid sGrid;
static s32 sArmed;
static u32 sArmMs;     // when it was armed: the load time is counted from here
static s32 sRoomRace;  // the race being run is sRoom's, or was until a Retry
static s32 sLeave;     // the next frame goes back to the room page
static s32 sHandOver;  // and the host's room goes to the next host once the race is reported (D28.5)
static s32 sReadySent; // the room has our digest for this race
static s32 sUpdates;   // frames of this race that ran
static s32 sElapsedMs; // since the room's start time, on the host clock; negative before it
static s32 sGoLogged;
static u32 sHeldAtGo; // samples held before GO, while the machines stood on the grid
static s32 sRaceTicks;  // ticks since GO: a racer's time is counted in these
static s32 sFinishSent; // the room has heard how our own race ended
static s32 sCpusGone;   // the host's game left this race and its CPU machines were taken off this copy's road

// The host's game says where the humans stand this often, in ticks.
#define GDX_NET_STANDING_EVERY 30

static s32 gdx_net_race_room_phase_is_race(const GdxNetRaceRoom* room) {
    return (room->phase == GDX_NET_RACE_PHASE_LOADING) || (room->phase == GDX_NET_RACE_PHASE_COUNTDOWN) ||
           (room->phase == GDX_NET_RACE_PHASE_RACING);
}

// The lock-in screen is over (gdx_net_machine_select.c). 0: this player has no place in the race, 1: the
// grid has not arrived yet, 2: the next Practice race is the room's.
int gdx_net_race_arm(unsigned raceIndex) {
    GdxNetRaceRoom room;

    sArmed = 0;
    gdx_lan_race_room(&room);
    if (!room.inRoom || (room.raceIndex != raceIndex) || !room.onGrid) {
        return 0;
    }
    if (!room.haveGrid) {
        return (room.phase == GDX_NET_RACE_PHASE_LOADING) ? 1 : 0;
    }
    if ((room.phase != GDX_NET_RACE_PHASE_LOADING) && (room.phase != GDX_NET_RACE_PHASE_COUNTDOWN)) {
        return 0;
    }
    gdx_net_race_build_grid(room.seed, room.machine, room.skin, room.humans, room.cpus, &sGrid);
    if (sGrid.racers == 0) {
        return 0;
    }
    // A custom track races from the room's copy in memory, decoded now so the race reads nothing from disk.
    if (!gdx_net_room_course_prepare(room.track)) {
        gdx_dbg_logf("[net-race] race %d: this copy cannot race the room's custom track\n", (int) raceIndex);
        return 0;
    }
    sRoom = room;
    sArmed = 1;
    sArmMs = gdx_lan_now_ms();
    gdx_single_race_force_next(room.laps, sGrid.racers);
    return 2;
}

// Course_Load asks whether the course it loads is the room's: the room's race is armed or being run.
int gdx_net_race_room_course_held(void) {
    return (sArmed || sRoomRace) ? 1 : 0;
}

// The seed every copy starts the room's race from (gdx_det_race_init, the first thing Race_Init does).
int gdx_net_race_armed_seed(unsigned int* seed) {
    if (!sArmed) {
        return 0;
    }
    *seed = sRoom.seed;
    return 1;
}

// How the poses of this race have travelled so far. Written when this copy leaves the race, and every ten
// seconds of one.
#define GDX_NET_POSE_LOG_EVERY 600

static void gdx_net_race_log_poses(void) {
    GdxNetRaceStats stats;

    gdx_net_race_stats(&stats);
    // The track loads and the intro starts on no two copies at the same pace, so samples are held in the
    // first seconds of the scene. Nothing moves then: the ones that count are those since GO. A skipped
    // tick is a frame one of the two games dropped, counted once for every remote machine it moved.
    gdx_dbg_logf("[net-race] poses after %d frames: %u sent, %u taken in, %u pushed out of a full box, "
                 "%u samples held since GO and %u before it, %u jumps, %u skipped ticks\n",
                 sUpdates, stats.sent, stats.received, stats.dropped, sGoLogged ? stats.held - sHeldAtGo : 0,
                 sGoLogged ? sHeldAtGo : stats.held, stats.snaps, stats.steps);
}

// Our own race is over, one way or the other. The room hears it once.
static void gdx_net_race_report(s32 retired) {
    Racer* own = &gRacers[0];

    sFinishSent = 1;
    if (retired) {
        gdx_lan_race_finish(sRoom.raceIndex, 1, 0);
        gdx_dbg_logf("[net-race] race %d: we are out after %d ticks of racing\n", (int) sRoom.raceIndex,
                     sRaceTicks);
    } else {
        gdx_lan_race_finish(sRoom.raceIndex, 0, (own->raceTime > 0) ? (unsigned) own->raceTime : 1);
        gdx_dbg_logf("[net-race] race %d: we finished in %d ms, place %d on this copy\n", (int) sRoom.raceIndex,
                     (int) own->raceTime, (int) own->position);
    }
}

// The host's game ranks every machine; the room wants the humans, first to last.
static void gdx_net_race_send_standing(void) {
    unsigned char order[GDX_NET_RACE_HUMANS];
    s32 place[GDX_NET_RACE_HUMANS];
    s32 position;
    s32 count = 0;
    s32 human;
    s32 at;

    for (human = 0; (human < sGrid.humans) && (human < GDX_NET_RACE_HUMANS); human++) {
        position = gRacers[gdx_net_race_local_slot(sRoom.self, human)].position;
        for (at = count; (at > 0) && (place[at - 1] > position); at--) {
            place[at] = place[at - 1];
            order[at] = order[at - 1];
        }
        place[at] = position;
        order[at] = (unsigned char) human;
        count++;
    }
    gdx_lan_race_standing(sRoom.raceIndex, order, count);
}

// The host's time for a human who finished replaces the one we gave its machine when we saw it cross
// the line, so every copy ranks the finishers as the table does.
static void gdx_net_race_take_results(void) {
    GdxNetRaceResults table;
    Racer* racer;
    s32 human;
    s32 local;

    if (!gdx_lan_race_results(sRoom.raceIndex, &table)) {
        return;
    }
    for (human = 0; (human < sGrid.humans) && (human < table.humans); human++) {
        local = gdx_net_race_local_slot(sRoom.self, human);
        racer = &gRacers[local];
        if ((local != 0) && (table.kind[human] == GDX_NET_RACE_RESULT_FINISHED) &&
            (racer->stateFlags & RACER_STATE_FINISHED)) {
            racer->raceTime = (s32) table.timeMs[human];
        }
    }
}

// What its owner's copy says became of a remote machine.
static void gdx_net_race_puppet_fate(Racer* racer, u32 flags) {
    if (!(racer->stateFlags & (RACER_STATE_FINISHED | RACER_STATE_RETIRED))) {
        if (flags & RACER_STATE_FINISHED) {
            // Seen a few ticks after it happened: that is how old the pose that says so is.
            racer->stateFlags |= RACER_STATE_FINISHED;
            racer->raceTime = (s32) (sRaceTicks * 50) / 3;
            gRacersFinished++;
        } else if (flags & RACER_STATE_RETIRED) {
            racer->stateFlags |= RACER_STATE_CRASHED;
            Racer_RetireRacer(racer);
        }
    }
    // A machine that fell off the track is gone on its owner's copy; a wreck on the road stays.
    if ((racer->stateFlags & RACER_STATE_RETIRED) && !(flags & RACER_STATE_ACTIVE)) {
        racer->stateFlags &= ~RACER_STATE_ACTIVE;
    }
}

// The host's game left the race (the room's state says so): nothing drives the CPU machines any more.
// Each is out of the race on this copy and off its road, once. One that had finished keeps its place.
static void gdx_net_race_cpus_out(void) {
    Racer* racer;
    s32 slot;

    sCpusGone = 1;
    for (slot = sGrid.humans; slot < sGrid.racers; slot++) {
        racer = &gRacers[gdx_net_race_local_slot(sRoom.self, slot)];
        if (!(racer->stateFlags & (RACER_STATE_FINISHED | RACER_STATE_RETIRED))) {
            racer->stateFlags |= RACER_STATE_CRASHED;
            Racer_RetireRacer(racer);
        }
        racer->stateFlags &= ~RACER_STATE_ACTIVE;
    }
    gdx_dbg_logf("[net-race] race %d: the host's game left, %d CPU machines are out after %d ticks of racing\n",
                 (int) sRoom.raceIndex, sGrid.racers - sGrid.humans, sRaceTicks);
}

// The player chose to leave the race from the quit box (gdx_net_race_box.c): the next frame goes back to
// the room screen. The others race on.
void gdx_net_race_quit(void) {
    if (sRoomRace && gdx_net_race_online()) {
        sLeave = 1;
    }
}

// The host chose LEAVE THE ROOM in the quit box: the race is left as by gdx_net_race_quit, and once this
// copy's machine and its CPUs are reported the room goes on without it (gdx_lan_room_hand_over).
void gdx_net_race_hand_over(void) {
    if (sRoomRace && gdx_net_race_online()) {
        sLeave = 1;
        sHandOver = 1;
    }
}

// What became of this copy's own machine, for the quit box: 0 it drives, 1 it finished, 2 it crashed or
// retired.
int gdx_net_race_own_state(void) {
    if (!gdx_net_race_online()) {
        return 0;
    }
    if (gRacers[0].stateFlags & RACER_STATE_FINISHED) {
        return 1;
    }
    return (gRacers[0].stateFlags & (RACER_STATE_CRASHED | RACER_STATE_RETIRED)) ? 2 : 0;
}

static void gdx_net_race_leave(void) {
    GdxNetRaceRoom room;

    gdx_net_race_box_reset(); // an open box would hold the controller in the next race
    if (sRoomRace) {
        gdx_dbg_logf("[net-race] race %d: back to the room page after %d frames\n", (int) sRoom.raceIndex,
                     sUpdates);
        gdx_net_race_log_poses();
        gdx_lan_race_room(&room);
        if (room.inRoom && (room.raceIndex == sRoom.raceIndex) && room.onGrid &&
            (room.phase == GDX_NET_RACE_PHASE_RACING)) {
            // Left by the quit box, or by a Retry, while the others race on: for the room that is a
            // retirement unless this machine had finished.
            if (!sFinishSent) {
                gdx_net_race_report(1);
            }
            // The CPU machines raced on this copy alone. They leave with it, and the room says so.
            if (sRoom.host) {
                gdx_lan_race_host_left(sRoom.raceIndex);
                gdx_dbg_logf("[net-race] race %d: the host's game leaves, the CPU machines with it\n",
                             (int) sRoom.raceIndex);
            }
        }
    }
    if (sHandOver) {
        gdx_lan_room_hand_over(); // after the reports above, which the room takes first
        gdx_dbg_logf("[net-race] race %d: the host leaves the room, which goes on without it\n",
                     (int) sRoom.raceIndex);
    }
    sArmed = sLeave = sRoomRace = sHandOver = 0;
    gdx_net_room_course_end();
    gdx_net_race_machines_restore();
    gdx_net_race_set_online(0);
    gdx_net_race_set_active(0);
    // The main menu comes up on the room screen, which says what happens next or why the room closed.
}

// Called by Racer_Init for every race and every scene that builds racers. Returns the number of racers:
// the game's own count, or the room's field.
s32 gdx_net_race_begin(s32 stockRacers) {
    GdxNetRaceRoom room;
    GdxNetRules rules;
    s32 armed = sArmed;
    s32 slot;
    s32 local;
    s32 echo;

    gdx_net_race_reset();
    sTick = 0;
    sEchoSide = 0.0f;
    sArmed = 0;
    sClockTick = 0;
    sClockOk = 0;
    for (slot = 0; slot < GDX_NET_RACE_MAX_RACERS; slot++) {
        sRoadSeated[slot] = 0;
    }

    if (armed) {
        if ((gGameMode != GAMEMODE_PRACTICE) || (gNumPlayers != 1) || (gTitleDemoState != TITLE_DEMO_INACTIVE)) {
            sLeave = 1; // not the race the lock-in screen started
            return stockRacers;
        }
        // A custom track's race runs on the room's course or not at all.
        if ((sRoom.track == GDX_NET_ROOM_COURSE_TRACK_CUSTOM) &&
            (!gdx_net_room_course_active() || gdx_net_room_course_refused())) {
            gdx_dbg_logf("[net-race] race %d: the room's custom track did not load\n", (int) sRoom.raceIndex);
            sLeave = 1;
            return stockRacers;
        }
        for (slot = 0; slot < sGrid.racers; slot++) {
            local = gdx_net_race_local_slot(sRoom.self, slot);
            gRacers[local].character = sGrid.character[slot];
            gRacers[local].machineSkinIndex = sGrid.skin[slot];
            // Another copy drives it: every human but us, and on a client the CPUs too, which only the
            // host runs.
            if ((local != 0) && (!sRoom.host || (slot < sGrid.humans))) {
                gdx_net_race_set_puppet(local, 1);
            }
        }
        rules.laps = sRoom.laps;
        rules.ignoreTrackSettings = sRoom.ignoreTrackSettings;
        rules.lavaNoKill = sRoom.lavaNoKill;
        rules.magnetRange = sRoom.magnetRange;
        rules.magnetStrength = sRoom.magnetStrength;
        rules.boostFrames = sRoom.boostFrames;
        gdx_net_race_set_rules(&rules);
        // Every copy builds the field's machines alike: a record where every human on that character
        // brought the same one, the stock machine everywhere else (decision 10).
        gdx_net_race_set_machines(sRoom.machine, sRoom.hasRecord, &sRoom.record[0][0], sRoom.humans);
        func_8008D33C();
        gdx_net_race_set_online(1);
        gdx_net_race_set_active(1);
        gdx_net_race_link(sRoom.raceIndex, sRoom.self);
        sRoomRace = 1;
        sReadySent = sUpdates = sGoLogged = 0;
        sRaceTicks = sFinishSent = sCpusGone = 0;
        gdx_net_race_box_reset();
        sHeldAtGo = 0;
        sElapsedMs = -1;
        return sGrid.racers;
    }
    gdx_net_race_machines_restore();

    if (sRoomRace) {
        // A Retry would run the room's race again on this copy alone. Its place is the room page.
        gdx_lan_race_room(&room);
        if (room.inRoom && (room.raceIndex == sRoom.raceIndex) && gdx_net_race_room_phase_is_race(&room)) {
            sLeave = 1;
            return stockRacers;
        }
        sRoomRace = 0;
    }

    echo = gdx_net_race_dev_echo_ticks();
    if ((echo > 0) && (gNumPlayers == 1) && (gTitleDemoState == TITLE_DEMO_INACTIVE) && (stockRacers >= 2) &&
        ((gGameMode == GAMEMODE_GP_RACE) || (gGameMode == GAMEMODE_PRACTICE))) {
        gdx_net_race_set_active(1);
        gdx_net_race_set_puppet(GDX_NET_ECHO_SLOT, 1);
        gdx_net_race_echo(GDX_NET_ECHO_SLOT, (u32) echo);
        sEchoSide = GDX_NET_ECHO_SIDE;
    }
    return stockRacers;
}

// After the stock code dealt the grid (Racer_SetupStartPositions): the CPUs take the front rows and the
// humans the back, in the room's order. Each copy has itself in slot 0, so the places go by race slot.
void gdx_net_race_grid_positions(void) {
    s32 slot;

    if (!gdx_net_race_online()) {
        return;
    }
    for (slot = 0; slot < sGrid.racers; slot++) {
        gRacers[gdx_net_race_local_slot(sRoom.self, slot)].position =
            gdx_net_race_grid_place(sGrid.humans, sGrid.racers, slot);
    }
}

// The last thing Race_Init does. The room hears that this copy is ready, and which track it loaded: the
// control points of the course, which is what a different version of a track would change.
void gdx_net_race_loaded(void) {
    unsigned char digest[32];
    CourseSegment* segment;
    u32 loadMs;
    s32 count;
    s32 i;

    if (!gdx_net_race_online() || sReadySent) {
        return;
    }
    sReadySent = 1;

    count = ((gCurrentCourseInfo != NULL) && (gCurrentCourseInfo->courseSegments != NULL))
                ? gCurrentCourseInfo->segmentCount
                : 0;
    if ((count < 0) || (count > 1024)) {
        count = 0;
    }
    gdx_net_race_digest_begin();
    gdx_net_race_digest_feed(&count, sizeof(count));
    for (i = 0; i < count; i++) {
        segment = &gCurrentCourseInfo->courseSegments[i];
        gdx_net_race_digest_feed(&segment->pos, sizeof(segment->pos));
        gdx_net_race_digest_feed(&segment->radiusLeft, sizeof(segment->radiusLeft));
        gdx_net_race_digest_feed(&segment->radiusRight, sizeof(segment->radiusRight));
        gdx_net_race_digest_feed(&segment->trackSegmentInfo, sizeof(segment->trackSegmentInfo));
    }
    gdx_net_race_digest_end(digest);

    loadMs = gdx_lan_now_ms() - sArmMs;
    gdx_lan_race_ready(sRoom.raceIndex, digest, loadMs);
    gdx_dbg_logf("[net-race] race %d: track loaded %d ms after the lock-in screen, %d racers, our slot %d\n",
                 (int) sRoom.raceIndex, (int) loadMs, sGrid.racers, sRoom.self);
}

// The first thing Race_Update does. 0: run the frame. 1: the room has not started the race yet, so the
// world waits as it is. 2: this copy is out of the race and the mode changes to the main menu.
s32 gdx_net_race_frame(void) {
    GdxNetRaceRoom room;
    int elapsed;

    if (sArmed || sLeave) {
        // Armed and never taken: the track did not load, and Racer_Init never ran.
        gdx_net_race_leave();
        return 2;
    }
    if (!gdx_net_race_online()) {
        return 0;
    }
    gdx_lan_race_room(&room);
    if (!room.inRoom || (room.raceIndex != sRoom.raceIndex) || !room.onGrid ||
        !gdx_net_race_room_phase_is_race(&room)) {
        // The race is over, the room closed, or the host found our track different and we watch.
        gdx_net_race_leave();
        return 2;
    }
    if (room.cpusOut && !sRoom.host && !sCpusGone) {
        gdx_net_race_cpus_out();
    }
    sElapsedMs = gdx_lan_race_elapsed(sRoom.raceIndex, &elapsed) ? (s32) elapsed : -1;
    // One frame runs first so everything that draws has been set up; then the grid waits for the others.
    if ((gRaceIntroTimer == GDX_NET_INTRO_START) && (sElapsedMs < 0) && (sUpdates > 0)) {
        return 1;
    }
    sUpdates++;
    if ((sUpdates % GDX_NET_POSE_LOG_EVERY) == 0) {
        gdx_net_race_log_poses();
    }
    return 0;
}

// The race intro (Racer_Update): whether this tick takes one off the timer.
s32 gdx_net_race_intro_gate(void) {
    if (!gdx_net_race_online()) {
        return 1;
    }
    if (!gdx_net_race_intro_run(gRaceIntroTimer, sElapsedMs)) {
        return 0;
    }
    if (gRaceIntroTimer == GDX_NET_INTRO_START) {
        gdx_dbg_logf("[net-race] intro starts %d ms after the room's start time\n", sElapsedMs);
    }
    return 1;
}

// The race intro: the timer after this tick took one off, moved on when this copy is behind the room.
s32 gdx_net_race_intro_after(s32 timer) {
    if (!gdx_net_race_online()) {
        return timer;
    }
    timer = gdx_net_race_intro_catch_up(timer, sElapsedMs);
    if ((timer == GDX_NET_INTRO_GO) && !sGoLogged) {
        GdxNetRaceStats stats;

        gdx_net_race_stats(&stats);
        sHeldAtGo = stats.held;
        sGoLogged = 1;
        gdx_dbg_logf("[net-race] GO %d ms after the room's start time, this computer's clock %u ms\n", sElapsedMs,
                     gdx_lan_now_ms());
    }
    return timer;
}

// The room's clock for this tick. 0 while this copy does not know it yet: nothing can be placed in time.
static s32 gdx_net_race_clock(u32* ms) {
    unsigned now = 0;

    if (sClockTick != sTick + 1) {
        sClockTick = sTick + 1;
        if (gdx_net_race_online()) {
            sClockOk = gdx_lan_race_clock(&now);
        } else {
            now = gdx_lan_now_ms(); // the developer echo: one copy, and its own clock is the room's
            sClockOk = 1;
        }
        sClockMs = (u32) now;
    }
    *ms = sClockMs;
    return sClockOk;
}

// A machine this copy drives, as the others will be told. `side` moves it sideways across the road.
static void gdx_net_race_pose_of(Racer* racer, f32 side, u32 nowMs, GdxNetPose* pose) {
    pose->tick = sTick;
    pose->timeMs = nowMs;
    pose->pos[0] = racer->segmentPositionInfo.pos.x + (side * racer->segmentBasis.z.x);
    pose->pos[1] = racer->segmentPositionInfo.pos.y + (side * racer->segmentBasis.z.y);
    pose->pos[2] = racer->segmentPositionInfo.pos.z + (side * racer->segmentBasis.z.z);
    pose->vel[0] = racer->velocity.x;
    pose->vel[1] = racer->velocity.y;
    pose->vel[2] = racer->velocity.z;
    pose->forward[0] = racer->modelBasis.x.x;
    pose->forward[1] = racer->modelBasis.x.y;
    pose->forward[2] = racer->modelBasis.x.z;
    pose->up[0] = racer->modelBasis.y.x;
    pose->up[1] = racer->modelBasis.y.y;
    pose->up[2] = racer->modelBasis.y.z;
    pose->lap = racer->lap;
    pose->flags = (u32) racer->stateFlags;
    pose->attack = racer->attackState;
}

// GDX_NET_DEV_TRACE: where this copy has every machine of the race after this tick, by race slot.
static void gdx_net_race_trace(u32 nowMs) {
    const Racer* racer;
    s32 slot;
    s32 local;

    for (slot = 0; slot < sGrid.racers; slot++) {
        local = gdx_net_race_local_slot(sRoom.self, slot);
        racer = &gRacers[local];
        gdx_net_race_dev_trace((unsigned) nowMs, sRaceTicks, slot, !gdx_net_race_is_puppet(local),
                               racer->segmentPositionInfo.pos.x, racer->segmentPositionInfo.pos.y,
                               racer->segmentPositionInfo.pos.z);
    }
    gdx_net_race_dev_trace_tick();
}

// Once per simulated tick, after every racer moved. A machine that crashed or retired is still sent:
// its owner is the only one who knows.
void gdx_net_race_tick(void) {
    GdxNetPose pose;
    u32 nowMs;
    s32 clock;
    s32 slot;

    if (!gdx_net_race_active()) {
        return;
    }
    clock = gdx_net_race_clock(&nowMs); // before the tick is counted: the reading the puppets used
    sTick++;
    if (clock) {
        gdx_net_race_pose_of(&gRacers[0], sEchoSide, nowMs, &pose);
        gdx_net_race_local_pose(&pose, sTick);
    }

    if (!gdx_net_race_online()) {
        return;
    }
    // The CPUs race on the host only, so the host tells the others where they are.
    if (clock && sRoom.host && gdx_net_race_send_due(sTick)) {
        for (slot = sGrid.humans; slot < sGrid.racers; slot++) {
            gdx_net_race_pose_of(&gRacers[gdx_net_race_local_slot(sRoom.self, slot)], 0.0f, nowMs, &pose);
            gdx_net_race_send_pose(slot, &pose);
        }
    }

    if (gdx_net_race_dev_autopilot()) {
        gRacers[0].stateFlags |= RACER_STATE_CPU_CONTROLLED; // GDX_NET_DEV_AUTOPILOT: see gdx_net_race_dev.c
    }
    // The race clock, as the game counts a racer's time: from GO.
    if (gRaceIntroTimer < GDX_NET_INTRO_GO) {
        sRaceTicks++;
    }
    if (!sFinishSent) {
        if (gRacers[0].stateFlags & RACER_STATE_FINISHED) {
            gdx_net_race_report(0);
        } else if (gRacers[0].stateFlags & (RACER_STATE_CRASHED | RACER_STATE_RETIRED)) {
            gdx_net_race_report(1);
        }
    }
    if (sRoom.host && (sRaceTicks > 0) && ((sTick % GDX_NET_STANDING_EVERY) == 0)) {
        gdx_net_race_send_standing();
    }
    if ((sTick & 3) == 0) {
        gdx_net_race_take_results();
    }
    if (clock && gdx_net_race_dev_trace_on()) {
        gdx_net_race_trace(nowMs);
    }
    gdx_net_race_receive();
}

// Two machines first touch (the pair loop of Racer_Update, before it deals the damage and the shove).
// Each copy runs that loop on what it shows: its own machines where they are, everyone else's where it
// guesses them. GDX_NET_DEV_CONTACTS writes down what this copy made of each contact of a machine it drives.
void gdx_net_race_contact(s32 racerId, s32 otherId, f32 gap, f32 closingSpeed) {
    const Racer* racer;
    const Racer* other;
    s32 slot;
    s32 otherSlot;
    s32 driven;
    s32 otherDriven;

    if (!gdx_net_race_online() || !gdx_net_race_dev_contacts()) {
        return;
    }
    driven = !gdx_net_race_is_puppet(racerId);
    otherDriven = !gdx_net_race_is_puppet(otherId);
    slot = gdx_net_race_local_slot(sRoom.self, racerId);
    otherSlot = gdx_net_race_local_slot(sRoom.self, otherId);
    if ((!driven && !otherDriven) || ((slot >= sGrid.humans) && (otherSlot >= sGrid.humans))) {
        return;
    }
    racer = &gRacers[racerId];
    other = &gRacers[otherId];
    gdx_dbg_logf("[net-race] contact: race tick %d, slots %d %d, driven here %d %d, gap %.1f, closing %.2f, "
                 "strength %.3f %.3f, attack %d %d, speed %.1f %.1f, at %.0f %.0f %.0f and %.0f %.0f %.0f\n",
                 sRaceTicks, slot, otherSlot, driven, otherDriven, gap, closingSpeed, racer->collidingStrength,
                 other->collidingStrength, (int) racer->attackState, (int) other->attackState, racer->speed,
                 other->speed, racer->segmentPositionInfo.pos.x, racer->segmentPositionInfo.pos.y,
                 racer->segmentPositionInfo.pos.z, other->segmentPositionInfo.pos.x,
                 other->segmentPositionInfo.pos.y, other->segmentPositionInfo.pos.z);
}

// The pair loop pushed two machines apart: `racerId` gets (x, y, z) times `share` taken off its velocity
// and `otherId` gets it times `otherShare` added, which the game's movement code then does for a machine
// it drives. A puppet runs no movement code, so its half is kept here and added to where it is shown,
// until its owner's poses hold the bump (gdx_net_race.h). Only a push by a machine this copy drives is
// kept: between two remote machines each owner's poses say what happened.
void gdx_net_race_pushed(s32 racerId, s32 otherId, f32 share, f32 otherShare, f32 x, f32 y, f32 z) {
    float push[3];
    s32 puppet;
    s32 otherPuppet;

    if (!gdx_net_race_active()) {
        return;
    }
    puppet = gdx_net_race_is_puppet(racerId);
    otherPuppet = gdx_net_race_is_puppet(otherId);
    if (puppet == otherPuppet) {
        return;
    }
    if (puppet) {
        push[0] = -share * x;
        push[1] = -share * y;
        push[2] = -share * z;
        gdx_net_race_shove(racerId, sTick, push);
    } else {
        push[0] = otherShare * x;
        push[1] = otherShare * y;
        push[2] = otherShare * z;
        gdx_net_race_shove(otherId, sTick, push);
    }
}

// Finds the place on the track for a position. The game's search walks from the last place and gives up
// on a long step, so after a jump the walk restarts from the control point nearest the target.
static s32 gdx_net_race_seat(RacerSegmentPositionInfo* info, f32 x, f32 y, f32 z) {
    CourseSegment* first;
    CourseSegment* segment;
    CourseSegment* best;
    f32 bestDistance;
    f32 distance;
    s32 guard;

    if (func_8009EBEC(info, x, y, z, 100, 1.0f) == 0) {
        return true;
    }
    if ((gCurrentCourseInfo == NULL) || (gCurrentCourseInfo->courseSegments == NULL)) {
        return false;
    }

    first = gCurrentCourseInfo->courseSegments;
    best = first;
    bestDistance = SQ(first->pos.x - x) + SQ(first->pos.y - y) + SQ(first->pos.z - z);
    guard = 0;
    for (segment = first->next; (segment != NULL) && (segment != first) && (guard < 1024); segment = segment->next) {
        distance = SQ(segment->pos.x - x) + SQ(segment->pos.y - y) + SQ(segment->pos.z - z);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = segment;
        }
        guard++;
    }

    // The same start state Racer_Init gives a racer on the grid, at that control point.
    info->courseSegment = best;
    info->segmentTValue = 0.0f;
    Course_SplineGetPosition(best, 0.0f, &info->segmentPos);
    info->pos = info->lastGroundedPos = info->segmentPos;
    info->segmentDisplacement.x = info->segmentDisplacement.y = info->segmentDisplacement.z =
        info->distanceFromSegment = 0.0f;
    info->segmentForwardMagnitude = Course_SplineGetTangent(best, 0.0f, &info->segmentForward);
    return func_8009EBEC(info, x, y, z, 100, 1.0f) == 0;
}

// The road under a place, for the guess of where a puppet is now (GdxNetRoadFrame). `context` is the
// puppet's racer slot.
static int gdx_net_race_road(void* context, const float pos[3], float frame[9]) {
    s32 slot = *(s32*) context;
    RacerSegmentPositionInfo* seat = &sRoadSeat[slot];
    Mtx3F basis;
    f32 length;
    f32 proportion;

    if (!sRoadSeated[slot]) {
        *seat = gRacers[slot].segmentPositionInfo; // where the machine was last shown
        sRoadSeated[slot] = 1;
    }
    if ((seat->courseSegment == NULL) || !gdx_net_race_seat(seat, pos[0], pos[1], pos[2])) {
        sRoadSeated[slot] = 0;
        return 0;
    }
    proportion = Course_SplineGetLengthInfo(seat->courseSegment, seat->segmentTValue, &length);
    Course_SplineGetBasis(seat->courseSegment, seat->segmentTValue, &basis, proportion);
    frame[0] = basis.x.x;
    frame[1] = basis.x.y;
    frame[2] = basis.x.z;
    frame[3] = basis.y.x;
    frame[4] = basis.y.y;
    frame[5] = basis.y.z;
    frame[6] = basis.z.x;
    frame[7] = basis.z.y;
    frame[8] = basis.z.z;
    return 1;
}

// The third branch of Racer_UpdateMovement: this racer belongs to another player.
void gdx_net_race_puppet_update(Racer* racer) {
    RacerSegmentPositionInfo* info = &racer->segmentPositionInfo;
    GdxNetPose pose;
    RacerSegmentPositionInfo seated;
    Mtx3F basis;
    Vec3f velocity;
    Vec3f shown;
    float offset[3];
    float shove[3];
    float target[3];
    float moving[3];
    float place[3];
    float ahead;
    u32 nowMs;
    s32 slot;
    s32 state;
    s32 put;
    s32 lap;
    s32 k;

    if ((racer->id < 0) || (racer->id >= GDX_NET_RACE_MAX_RACERS)) {
        return;
    }
    slot = racer->id;

    // The game's contact code leans a machine it pushed. How this one leans is its owner's to say.
    racer->stateFlags &= ~RACER_STATE_COLLISION_RECOIL;

    if (!gdx_net_race_clock(&nowMs)) {
        return; // the room's clock is not known yet: it waits where it is
    }
    state = gdx_net_race_sample(slot, sTick, nowMs, &pose, &ahead);
    if (!(state & GDX_NET_SAMPLE_POSE)) {
        return; // nothing heard yet: it waits where the grid put it
    }
    if (gdx_net_race_online()) {
        gdx_net_race_puppet_fate(racer, pose.flags);
    }
    // The pair loop reads this, so a remote machine that attacks pushes and damages ours as one here would.
    racer->attackState = (s16) pose.attack;

    // Where its owner has it now. In the air or off the track a machine does not follow the road.
    gdx_net_race_advance(&pose, ahead,
                         (pose.flags & (RACER_STATE_AIRBORNE | RACER_STATE_FALLING_OFF_TRACK)) ? NULL
                                                                                              : gdx_net_race_road,
                         &slot);
    if (state & GDX_NET_SAMPLE_HELD) {
        // Nothing heard for too long to guess on: it stands where the guess ended.
        pose.vel[0] = pose.vel[1] = pose.vel[2] = 0.0f;
    }
    gdx_net_race_shoved(slot, sTick, offset, shove);
    for (k = 0; k < 3; k++) {
        target[k] = pose.pos[k] + offset[k];
        moving[k] = pose.vel[k] + shove[k];
    }
    put = gdx_net_race_settle(slot, sTick, target, moving, place);

    shown.x = place[0];
    shown.y = place[1];
    shown.z = place[2];
    racer->velocity.x = moving[0];
    racer->velocity.y = moving[1];
    racer->velocity.z = moving[2];
    racer->modelPos = shown;
    racer->speed = sqrtf(SQ(racer->velocity.x) + SQ(racer->velocity.y) + SQ(racer->velocity.z));

    basis.x.x = pose.forward[0];
    basis.x.y = pose.forward[1];
    basis.x.z = pose.forward[2];
    basis.y.x = pose.up[0];
    basis.y.y = pose.up[1];
    basis.y.z = pose.up[2];
    if (Math_OrthonormalizeAroundForward(&basis) == 0) {
        racer->trueBasis = racer->modelBasis = basis;
    }
    racer->unk_5C.x = (racer->trueBasis.z.x * racer->velocity.x) + (racer->trueBasis.z.y * racer->velocity.y) +
                      (racer->trueBasis.z.z * racer->velocity.z);
    racer->unk_5C.y = (racer->trueBasis.y.x * racer->velocity.x) + (racer->trueBasis.y.y * racer->velocity.y) +
                      (racer->trueBasis.y.z * racer->velocity.z);
    racer->unk_5C.z = (racer->trueBasis.x.x * racer->velocity.x) + (racer->trueBasis.x.y * racer->velocity.y) +
                      (racer->trueBasis.x.z * racer->velocity.z);

    if (!gdx_net_race_seat(info, shown.x, shown.y, shown.z)) {
        // Far from any road (falling, or a track we could not follow): drawn where it is, and its
        // place in the race stays what it was.
        info->pos = shown;
        return;
    }

    info->segmentLengthProportion =
        Course_SplineGetLengthInfo(info->courseSegment, info->segmentTValue, &racer->lapDistance);
    Course_SplineGetBasis(info->courseSegment, info->segmentTValue, &racer->segmentBasis,
                          info->segmentLengthProportion);
    racer->currentRadiusLeft =
        (info->segmentLengthProportion * (info->courseSegment->next->radiusLeft - info->courseSegment->radiusLeft)) +
        info->courseSegment->radiusLeft;
    racer->currentRadiusRight =
        (info->segmentLengthProportion * (info->courseSegment->next->radiusRight - info->courseSegment->radiusRight)) +
        info->courseSegment->radiusRight;
    // The track shape code gives the height, the shadow and the airborne state, but it is also the
    // game's ground response: it lifts a machine that is under the road and takes that out of its
    // velocity. A guessed place can be a hair under the road; it stays where it is.
    seated = *info;
    velocity = racer->velocity;
    D_800CF4B8[TRACK_SHAPE_INDEX((u32) info->courseSegment->trackSegmentInfo & TRACK_SHAPE_MASK)](racer);
    *info = seated;
    racer->velocity = velocity;

    racer->raceDistance = gdx_net_race_progress(racer->lapDistance, pose.lap, gCurrentCourseInfo->length,
                                                racer->raceDistance, put, &lap);
    if (!(racer->stateFlags & RACER_STATE_RETIRED)) {
        racer->raceDistancePosition = racer->raceDistance; // a retired machine keeps its place at the back
    }
    racer->lap = (s16) lap;
    racer->lapsCompletedDistance = (f32) (lap - 1) * gCurrentCourseInfo->length;
}
