/* port/gdx_test_drive.h -- CPUs in a Course Edit test drive.
 *
 * Enhancements > Course Edit & Machine sets how many CPUs join a test drive (0 to 29, 0 is the stock solo run)
 * and their difficulty. The values are read once when the test drive starts and hold until it ends, so a
 * restart after falling off the course keeps the same field.
 *
 * With CPUs on, the test drive races like an Edit Cup race on the same track: the CPUs use the Edit Cup
 * tuning (decomp/src/overlays/ovl_i3/cpu.c), machines far away are drawn simpler, as in a race
 * (decomp/src/game/racer.c), and the player starts on pole with the field behind. The laps stay endless and
 * there is still no countdown, as in a stock test drive.
 */
#ifndef GDX_TEST_DRIVE_H
#define GDX_TEST_DRIVE_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_TEST_DRIVE_MAX_CPUS 29
#define GDX_TEST_DRIVE_ROSTER 30 /* stock characters 0 to 29 */
#define GDX_TEST_DRIVE_DEFAULT_DIFFICULTY 2 /* Expert */
#define GDX_TEST_DRIVE_DIFFICULTY_MAX 3     /* Master */
/* The CPU speed table the race loader normally allocates (0x200 entries, four per course segment). */
#define GDX_TEST_DRIVE_CPU_LINE_ENTRIES 0x200

/* Starts a test drive. cpus and difficulty are the menu values, clamped here. gameDifficulty is the game's
 * current difficulty. Returns the difficulty the test drive races at: the menu's with CPUs, the game's
 * own without, so a solo test drive changes nothing. */
int gdx_test_drive_begin(int cpus, int difficulty, int gameDifficulty);

/* Ends a test drive. Returns the difficulty to put back: the one gdx_test_drive_begin was given when the
 * drive had CPUs, gameDifficulty otherwise. Safe to call when no test drive is running. */
int gdx_test_drive_end(int gameDifficulty);

/* The CPUs in the running test drive, 0 when none is running. */
int gdx_test_drive_cpus(void);

/* The racers in the running test drive: the player plus the CPUs. */
int gdx_test_drive_total_racers(void);

/* Moves the player (positions[0]) to pole and every racer that started ahead of it back one place.
 * positions holds 1-based grid places, one per racer. */
void gdx_test_drive_pole(int* positions, int count);

/* The character a CPU races as. The roster leaves out rosterSkipped, the player's last machine-select
 * choice, but a test drive can pin another machine for the player. A CPU on the player's machine takes the
 * left-out one instead, when that is a stock character. */
int gdx_test_drive_cpu_character(int cpuCharacter, int playerCharacter, int rosterSkipped);

/* Storage for the CPU speed table during a test drive. The editor never runs the race loader that
 * allocates it. */
short* gdx_test_drive_cpu_line(void);

#ifdef __cplusplus
}
#endif

#endif
