// G-Diffuser: the decomp half of the GDX_DET_LOG determinism probe (see gdx_det_probe.c).
//
// Lists the Racer fields the probe hashes and gathers one frame of state for the writer. Pointers,
// render matrices, colors and sound flags are left out: they differ between processes without
// meaning the simulation diverged.
#include "global.h"
#include "unk_structs.h"
#include "gdx_det_probe.h"

extern s32 gGameMode;
extern s32 gRaceIntroTimer;
extern s32 gTotalRacers;
extern Racer gRacers[];
extern s32 gRandSeed1;
extern u32 gRandMask1;
extern s32 gRandSeed2;
extern u32 gRandMask2;
extern u32 gGameFrameCount;

#define GDX_DET_F(group, field) { group, #field, offsetof(Racer, field), sizeof(((Racer*)0)->field), 1 }
#define GDX_DET_I(group, field) { group, #field, offsetof(Racer, field), sizeof(((Racer*)0)->field), 0 }

const GdxDetField gGdxDetFields[] = {
    GDX_DET_F(GDX_DET_POS, segmentPositionInfo.pos),
    GDX_DET_F(GDX_DET_POS, segmentPositionInfo.segmentTValue),
    GDX_DET_F(GDX_DET_POS, segmentPositionInfo.segmentPos),
    GDX_DET_F(GDX_DET_POS, segmentPositionInfo.distanceFromSegment),
    GDX_DET_F(GDX_DET_POS, segmentPositionInfo.lastGroundedPos),
    GDX_DET_F(GDX_DET_MOTION, velocity),
    GDX_DET_F(GDX_DET_MOTION, unk_5C),
    GDX_DET_F(GDX_DET_MOTION, unk_68),
    GDX_DET_F(GDX_DET_MOTION, unk_80),
    GDX_DET_F(GDX_DET_MOTION, acceleration),
    GDX_DET_F(GDX_DET_MOTION, speed),
    GDX_DET_F(GDX_DET_MOTION, maxSpeed),
    GDX_DET_F(GDX_DET_MOTION, heightAboveGround),
    GDX_DET_F(GDX_DET_BASIS, gravityUp),
    GDX_DET_F(GDX_DET_BASIS, tiltUp),
    GDX_DET_F(GDX_DET_BASIS, trueBasis),
    GDX_DET_F(GDX_DET_BASIS, modelBasis),
    GDX_DET_F(GDX_DET_BASIS, modelPos),
    GDX_DET_I(GDX_DET_STATE, stateFlags),
    GDX_DET_F(GDX_DET_STATE, energy),
    GDX_DET_F(GDX_DET_STATE, maxEnergy),
    GDX_DET_I(GDX_DET_STATE, boostTimer),
    GDX_DET_I(GDX_DET_STATE, spinOutTimer),
    GDX_DET_I(GDX_DET_STATE, attackState),
    GDX_DET_I(GDX_DET_STATE, driftAttackDirection),
    GDX_DET_I(GDX_DET_STATE, driftingCounter),
    GDX_DET_I(GDX_DET_STATE, zButtonTimer),
    GDX_DET_I(GDX_DET_STATE, rButtonTimer),
    GDX_DET_F(GDX_DET_PROGRESS, raceDistance),
    GDX_DET_F(GDX_DET_PROGRESS, lapDistance),
    GDX_DET_F(GDX_DET_PROGRESS, lapsCompletedDistance),
    GDX_DET_I(GDX_DET_PROGRESS, raceTime),
    GDX_DET_I(GDX_DET_PROGRESS, lap),
    GDX_DET_I(GDX_DET_PROGRESS, lapsCompleted),
    GDX_DET_I(GDX_DET_PROGRESS, position),
    GDX_DET_I(GDX_DET_PROGRESS, lastSegmentIndex),
    GDX_DET_I(GDX_DET_INPUT, lastStickX),
    GDX_DET_I(GDX_DET_INPUT, lastStickY),
    GDX_DET_I(GDX_DET_INPUT, lastButtonsCurrent),
    GDX_DET_I(GDX_DET_INPUT, lastButtonsPressed),
};
const int gGdxDetFieldCount = sizeof(gGdxDetFields) / sizeof(gGdxDetFields[0]);

// Called at the top of Race_Init. With GDX_DET_RACE_SEED set, both RNGs restart from that seed
// in the boot pins' shapes (sys_gfx.c) and gGameFrameCount restarts at 0, so a race no longer
// inherits how many frames and draws the menus and loads took before it. The frame counter
// matters as much as the seeds: Cpu_GenerateInputs (cpu.c) lets each CPU racer think only on
// frames where (id & 3) == gGameFrameCount % 4. This is the seam an online race would feed.
void gdx_det_race_init(void) {
    extern int gdx_net_race_armed_seed(unsigned int* seed); // gdx_net_race_bridge.c
    unsigned int seed;

    // The developer seed wins; otherwise a room's race starts every copy from the room's seed.
    if (gdx_det_probe_race_seed(&seed) || gdx_net_race_armed_seed(&seed)) {
        Math_Rand1Init(seed, seed + seed);
        Math_Rand2Init(seed + seed, seed);
        gGameFrameCount = 0;
    }
}

// Called once at the end of Race_Update (decomp/src/overlays/ovl_i2/race.c), after the whole
// simulation step for that frame.
void gdx_det_probe_frame(void) {
    GdxDetFrame frame;

    frame.racers = (const unsigned char*)gRacers;
    frame.racerSize = sizeof(Racer);
    frame.racerCount = gTotalRacers;
    frame.mode = gGameMode;
    frame.introTimer = gRaceIntroTimer;
    frame.rng[0] = (u32)gRandSeed1;
    frame.rng[1] = gRandMask1;
    frame.rng[2] = (u32)gRandSeed2;
    frame.rng[3] = gRandMask2;
    frame.gameFrame = gGameFrameCount;
    gdx_det_probe_record(&frame);
}
