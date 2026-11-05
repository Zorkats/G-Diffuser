// G-Diffuser: GDX_DET_LOG determinism probe, shared between its two halves.
//
// gdx_det_layout.c compiles with the decomp headers (and their own libc), so it knows the Racer
// layout of this build, including the EXPANSION_KIT fields. gdx_det_probe.c compiles with the
// system headers and owns the file. This header includes nothing so both sides can use it.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum GdxDetGroup {
    GDX_DET_POS,
    GDX_DET_MOTION,
    GDX_DET_BASIS,
    GDX_DET_STATE,
    GDX_DET_PROGRESS,
    GDX_DET_INPUT,
    GDX_DET_GROUP_COUNT
};

#define GDX_DET_RACER_CAP 30

typedef struct GdxDetField {
    int group;
    const char* name;
    unsigned int offset;
    unsigned int size;
    int isFloat;
} GdxDetField;

typedef struct GdxDetFrame {
    const unsigned char* racers; // gRacers
    unsigned int racerSize;      // sizeof(Racer) in this build
    int racerCount;              // gTotalRacers
    int mode;                    // gGameMode
    int introTimer;              // gRaceIntroTimer
    unsigned int rng[4];         // gRandSeed1, gRandMask1, gRandSeed2, gRandMask2
    unsigned int gameFrame;      // gGameFrameCount; CPU racers think when (id & 3) == it % 4
} GdxDetFrame;

// gdx_det_layout.c: the probed Racer fields.
extern const GdxDetField gGdxDetFields[];
extern const int gGdxDetFieldCount;

// gdx_det_probe.c: writes one frame when GDX_DET_LOG is set.
void gdx_det_probe_record(const GdxDetFrame* frame);

// gdx_det_probe.c: nonzero, with the seed, when GDX_DET_RACE_SEED is set.
int gdx_det_probe_race_seed(unsigned int* seed);

#ifdef __cplusplus
}
#endif
