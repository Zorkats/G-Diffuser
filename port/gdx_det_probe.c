// G-Diffuser: GDX_DET_LOG, a dev-only per-frame race state hash log.
//
// Built for the online determinism experiment (O0 in devdocs/online-design-document.md): two runs
// with the same GDX_INPUT_SCRIPT and GDX_RAND_SEED1/2 must write identical logs, on one machine
// and across machines, before input lockstep can be considered. Unset, each frame costs one call
// and a cached getenv per process.
//
//   GDX_DET_LOG=<path>             Write the log. Opened once, truncated.
//   GDX_DET_DUMP=<race>:<frame>    Also write every probed field of every racer on that frame, in
//                                  hex and as a number, so a hash mismatch names its field.
//                                  <race>:<first>-<last> does it for a run of frames.
//   GDX_DET_RACE_SEED=<n>          Reseed both RNGs and zero gGameFrameCount at the start of
//                                  every Race_Init (gdx_det_layout.c). Works without GDX_DET_LOG.
//
// Log lines, one per Race_Update:
//   race <n> mode=<gGameMode> racers=<gTotalRacers>
//   f <frame> t=<gRaceIntroTimer> gfc=<gGameFrameCount> rng=<seed1>,<mask1>,<seed2>,<mask2>
//     g=<group hashes> r=<racer hashes>      (one line in the file)
//   d <frame> <racer> <field> <hex words> <values>
//
// Hashes are FNV-1a 64 over the exact field bytes, so floats compare bit for bit. The field list
// and the frame data come from gdx_det_layout.c, which sees the decomp Racer layout.
#define _CRT_SECURE_NO_WARNINGS 1
#include "gdx_det_probe.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t gdx_det_fnv(uint64_t hash, const unsigned char* bytes, size_t size) {
    size_t i;

    for (i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static void gdx_det_dump_racer(FILE* out, int frame, int index, const unsigned char* racer) {
    int f;

    for (f = 0; f < gGdxDetFieldCount; f++) {
        const GdxDetField* field = &gGdxDetFields[f];
        const unsigned char* base = racer + field->offset;
        unsigned int at;

        fprintf(out, "d %d %d %s", frame, index, field->name);
        // Every probed field is a whole number of 4-byte words except the s16 counters.
        if (field->size == 2) {
            int16_t value;
            memcpy(&value, base, 2);
            fprintf(out, " %04X %d\n", (unsigned)(uint16_t)value, value);
            continue;
        }
        for (at = 0; at + 4 <= field->size; at += 4) {
            uint32_t bits;
            memcpy(&bits, base + at, 4);
            fprintf(out, " %08X", (unsigned)bits);
        }
        for (at = 0; at + 4 <= field->size; at += 4) {
            if (field->isFloat) {
                float value;
                memcpy(&value, base + at, 4);
                fprintf(out, " %.9g", value);
            } else {
                int32_t value;
                memcpy(&value, base + at, 4);
                fprintf(out, " %d", (int)value);
            }
        }
        fputc('\n', out);
    }
}

int gdx_det_probe_race_seed(unsigned int* seed) {
    static int sRead;
    static int sHave;
    static unsigned int sSeed;

    if (!sRead) {
        const char* env = getenv("GDX_DET_RACE_SEED");

        sRead = 1;
        if (env != NULL && env[0] != '\0') {
            sSeed = (unsigned int)strtoul(env, NULL, 0);
            sHave = 1;
        }
    }
    *seed = sSeed;
    return sHave;
}

void gdx_det_probe_record(const GdxDetFrame* frame) {
    static int sRead;
    static FILE* sOut;
    static int sDumpRace = -1;
    static int sDumpFrame = -1;
    static int sDumpLast = -1;
    static int sRace;
    static int sFrame;
    static int sLastMode = -1;
    static int sLastIntroTimer = -1;
    uint64_t groups[GDX_DET_GROUP_COUNT];
    uint64_t racers[GDX_DET_RACER_CAP];
    int count;
    int i;
    int f;

    if (!sRead) {
        const char* path = getenv("GDX_DET_LOG");
        const char* dump = getenv("GDX_DET_DUMP");

        sRead = 1;
        if (path != NULL && path[0] != '\0') {
            sOut = fopen(path, "w");
        }
        if (dump != NULL) {
            int fields = sscanf(dump, "%d:%d-%d", &sDumpRace, &sDumpFrame, &sDumpLast);

            if (fields == 2) {
                sDumpLast = sDumpFrame;
            } else if (fields != 3) {
                sDumpRace = sDumpFrame = sDumpLast = -1;
            }
        }
    }
    if (sOut == NULL) {
        return;
    }

    // A new race starts when the mode changes or the intro countdown is re-armed.
    if (frame->mode != sLastMode || frame->introTimer > sLastIntroTimer) {
        sRace++;
        sFrame = 0;
        fprintf(sOut, "race %d mode=%d racers=%d\n", sRace, frame->mode, frame->racerCount);
    }
    sLastMode = frame->mode;
    sLastIntroTimer = frame->introTimer;

    count = frame->racerCount;
    if (count < 0) {
        count = 0;
    }
    if (count > GDX_DET_RACER_CAP) {
        count = GDX_DET_RACER_CAP;
    }
    for (i = 0; i < GDX_DET_GROUP_COUNT; i++) {
        groups[i] = 0xCBF29CE484222325ull;
    }
    for (i = 0; i < count; i++) {
        const unsigned char* racer = frame->racers + (size_t)i * frame->racerSize;

        racers[i] = 0xCBF29CE484222325ull;
        for (f = 0; f < gGdxDetFieldCount; f++) {
            const GdxDetField* field = &gGdxDetFields[f];

            groups[field->group] = gdx_det_fnv(groups[field->group], racer + field->offset, field->size);
            racers[i] = gdx_det_fnv(racers[i], racer + field->offset, field->size);
        }
    }

    fprintf(sOut, "f %d t=%d gfc=%u rng=%08X,%08X,%08X,%08X g=", sFrame, frame->introTimer, frame->gameFrame,
            frame->rng[0], frame->rng[1], frame->rng[2], frame->rng[3]);
    for (i = 0; i < GDX_DET_GROUP_COUNT; i++) {
        fprintf(sOut, "%s%016llX", i ? "," : "", (unsigned long long)groups[i]);
    }
    fputs(" r=", sOut);
    for (i = 0; i < count; i++) {
        fprintf(sOut, "%s%016llX", i ? "," : "", (unsigned long long)racers[i]);
    }
    fputc('\n', sOut);

    if (sRace == sDumpRace && sFrame >= sDumpFrame && sFrame <= sDumpLast) {
        for (i = 0; i < count; i++) {
            gdx_det_dump_racer(sOut, sFrame, i, frame->racers + (size_t)i * frame->racerSize);
        }
    }
    if ((sFrame % 60) == 0) {
        fflush(sOut);
    }
    sFrame++;
}
