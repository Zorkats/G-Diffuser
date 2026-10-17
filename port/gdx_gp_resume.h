/* port/gdx_gp_resume.h -- Save GP in progress: the resume record, its codec, and its file.
 *
 * The record is written between races at the GP next-course boundary and read back by the
 * "Continue Cup" entry. It holds only plain values; the game globals are copied in and out by the
 * PORT glue in decomp/src/game/game.c, so this module needs no decomp headers.
 *
 * Wire format (version 1, little-endian, fixed size GDX_GP_RESUME_FILE_SIZE = 584 bytes):
 *   0    magic            "GDXG"
 *   4    version          u32, 1
 *   8    size             u32, total file length (584)
 *   12   cup              s32  0..4 (Jack, Queen, King, Joker, X)
 *   16   difficulty       s32  0..3
 *   20   numPlayers       s32  1..4
 *   24   courseIndex      s32  next course to race; X Cup 48..53, stock 0..23 with course/6 == cup
 *   28   lastRandomCourseIndex  s32  -1..53
 *   32   raceCounter      s32  1..9999 (game var D_800E5FF0, race starts in this cup, retries included)
 *   36   randSeed1        s32
 *   40   randMask1        u32
 *   44   randSeed2        s32
 *   48   randMask2        u32
 *   52   lives[4]         s32  0..5
 *   68   playerCharacter[4]  s32  0..35
 *   84   playerSkin[4]    s32  0..3
 *   100  racers[30]      12 bytes each: points s32 (0..32767), character s32 (0..35), skin s32 (0..3)
 *   460  engine[30]      f32 bit pattern, 0.0 .. 1.0 (game var gCharacterLastEngine)
 *   580  crc32           u32, CRC-32 (IEEE) over bytes 0..579
 *
 * Decode checks, in order: argument, header length, magic, version, size, file length, CRC, then
 * field ranges. A record that fails any check is refused whole; nothing is partially applied.
 */

#ifndef GDX_GP_RESUME_H
#define GDX_GP_RESUME_H

/* decomp/src/game/game.c defines GDX_GP_RESUME_STATE_ONLY: host <stddef.h> and <stdint.h> clash with
 * the decomp libc headers there, and the glue needs only the record, the bounds and the file calls.
 * The record fields are 32-bit either way (gdx_gp_resume.c checks that int is 32 bits). */
#ifdef GDX_GP_RESUME_STATE_ONLY
typedef int GdxGpS32;
typedef unsigned int GdxGpU32;
#else
#include <stddef.h>
#include <stdint.h>
typedef int32_t GdxGpS32;
typedef uint32_t GdxGpU32;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_GP_RESUME_MAGIC "GDXG"
#define GDX_GP_RESUME_VERSION 1u
#define GDX_GP_RESUME_FILE_NAME "gp_resume.bin"
#define GDX_GP_RESUME_FILE_SIZE 584u
#define GDX_GP_RESUME_HEADER_SIZE 12u
#define GDX_GP_RESUME_RACERS 30
#define GDX_GP_RESUME_PLAYERS 4

#define GDX_GP_RESUME_CUP_X 4
#define GDX_GP_RESUME_CUP_COUNT 5
#define GDX_GP_RESUME_COURSE_X_FIRST 48
#define GDX_GP_RESUME_COURSE_X_LAST 53
#define GDX_GP_RESUME_COURSES_PER_CUP 6
#define GDX_GP_RESUME_DIFFICULTY_COUNT 4
#define GDX_GP_RESUME_MAX_LIVES 5
#define GDX_GP_RESUME_MACHINE_COUNT 36
#define GDX_GP_RESUME_SKIN_COUNT 4
/* D_800E5FF0 counts every race start in the cup, retries included; only == 1 matters to the game. */
#define GDX_GP_RESUME_MAX_RACE_COUNTER 9999
#define GDX_GP_RESUME_MAX_POINTS 32767

typedef enum GdxGpResumeError {
    GDX_GP_RESUME_OK = 0,
    GDX_GP_RESUME_ERR_ARG,       /* NULL pointer, or an output buffer smaller than the record */
    GDX_GP_RESUME_ERR_IO,        /* the file is missing or cannot be opened for reading */
    GDX_GP_RESUME_ERR_TRUNCATED, /* fewer bytes than the header declares */
    GDX_GP_RESUME_ERR_MAGIC,
    GDX_GP_RESUME_ERR_VERSION,
    GDX_GP_RESUME_ERR_SIZE,      /* declared size is not the fixed size, or extra bytes follow */
    GDX_GP_RESUME_ERR_CRC,
    GDX_GP_RESUME_ERR_RANGE,     /* a field is outside the bounds listed in the format comment */
    GDX_GP_RESUME_ERR_WRITE      /* the temp file could not be written or moved into place */
} GdxGpResumeError;

typedef struct GdxGpResumeRacer {
    GdxGpS32 points;
    GdxGpS32 character;
    GdxGpS32 skin;
} GdxGpResumeRacer;

typedef struct GdxGpResumeState {
    GdxGpS32 cup;
    GdxGpS32 difficulty;
    GdxGpS32 numPlayers;
    GdxGpS32 courseIndex;
    GdxGpS32 lastRandomCourseIndex;
    GdxGpS32 raceCounter;
    GdxGpS32 randSeed1;
    GdxGpU32 randMask1;
    GdxGpS32 randSeed2;
    GdxGpU32 randMask2;
    GdxGpS32 lives[GDX_GP_RESUME_PLAYERS];
    GdxGpS32 playerCharacter[GDX_GP_RESUME_PLAYERS];
    GdxGpS32 playerSkin[GDX_GP_RESUME_PLAYERS];
    GdxGpResumeRacer racers[GDX_GP_RESUME_RACERS];
    float engine[GDX_GP_RESUME_RACERS];
} GdxGpResumeState;

/* Field bounds only; the record layout is the table above. Returns GDX_GP_RESUME_OK or ERR_RANGE. */
GdxGpResumeError gdx_gp_resume_validate(const GdxGpResumeState* state);

#ifndef GDX_GP_RESUME_STATE_ONLY
/* Pure codec. Encode refuses a state that fails validation. Decode fills *out only on success. */
GdxGpResumeError gdx_gp_resume_encode(const GdxGpResumeState* state, unsigned char* out, size_t outCap);
GdxGpResumeError gdx_gp_resume_decode(const unsigned char* data, size_t length, GdxGpResumeState* out);
#endif

/* Explicit-path file operations, used by the default-path wrappers and by the tests. Save is atomic:
 * the record is written to "<path>.tmp" and moved over <path>, so a crash leaves the old file or none. */
GdxGpResumeError gdx_gp_resume_save_to(const char* path, const GdxGpResumeState* state);
GdxGpResumeError gdx_gp_resume_load_from(const char* path, GdxGpResumeState* out);
GdxGpResumeError gdx_gp_resume_clear_from(const char* path);

/* Directory for the default file. main.cpp sets it at boot to the LUS app directory, which on Linux
 * is not the working directory. Until it is set, the default is the directory of the executable. */
void gdx_gp_resume_set_directory(const char* directory);
/* Default file: gp_resume.bin in the directory above. Returns ERR_ARG when the buffer is too small
 * for the path. */
#ifndef GDX_GP_RESUME_STATE_ONLY
GdxGpResumeError gdx_gp_resume_default_path(char* out, size_t outCap);
#endif
GdxGpResumeError gdx_gp_resume_save(const GdxGpResumeState* state);
GdxGpResumeError gdx_gp_resume_load(GdxGpResumeState* out);
GdxGpResumeError gdx_gp_resume_clear(void);
/* 1 when the default file holds a cup that can be resumed, else 0. outDifficulty (may be NULL) receives
 * its class. Reads the file, so callers check once per screen, not every frame. */
int gdx_gp_resume_available(int* outDifficulty);

const char* gdx_gp_resume_error_text(GdxGpResumeError error);

#ifdef __cplusplus
}
#endif

#endif /* GDX_GP_RESUME_H */
