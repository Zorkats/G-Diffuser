/* port/gdx_achievements.cpp -- F10 playtime tracker + local achievements.
 *
 * See gdx_achievements.h for the API contract and the retroactive-grant policy.
 *
 * PORT/DECOMP BOUNDARY: like gdx_palette.cpp, this TU deliberately does NOT include
 * decomp headers; it mirrors the SaveContext field layout it reads and static_asserts
 * the sizes/offsets against decomp/include/fzx_save.h, turning layout drift into a
 * compile error. Only profileSaves[0] (the live profile; [1] is the backup copy) and
 * cupSave are read. Nothing here ever writes gSaveContext.
 *
 * Field encodings (verified against decomp/src/overlays/ovl_i2/save.c + game/common.c):
 *  - cupDifficultiesCleared[4]: COUNT of difficulties cleared per cup, indexed
 *    JACK/QUEEN/KING/JOKER (save.c:1266-1268, common.c:315). Difficulties unlock in
 *    order, so count >= n means the first n difficulties (0=Novice..3=Master) cleared.
 *  - ddCups.cupCompletion[character * 2 + ddCup], ddCup 0=DD_1/1=DD_2: per-character
 *    bitmask, bit (difficulty * 2) set on cup clear (save.c:760-762).
 *  - ddCups.staffGhostCompletion: bit (courseIndex - COURSE_SILENCE_3) per beaten DD
 *    staff ghost, 12 courses, masked 0xFFF (save.c:2520-2533).
 *  - deathRace.timeRecord[0] / courses[i].bestTime: milliseconds; MAX_TIMER
 *    (3599999) is the unset sentinel (save.c:1109-1134). Zero is also treated as
 *    unset: pre-boot BSS is all-zero and a 0 ms record is impossible.
 *  - cupSave.cupCompletion[difficulty][character / 3]: u16, bit
 *    ((character % 3) * 5 + cupType) per GP cup won (save.c:778-779); cupType is the
 *    Cup enum 0..4 = JACK, QUEEN, KING, JOKER, X.
 *
 * Persistence format (saves/achievements.txt, INI-ish lines, tolerant reader):
 *     version=1
 *     playtime_seconds=<uint64>
 *     unlock=<id> <unix_ts>
 * Unknown lines and unknown achievement IDs are skipped on load (a renamed/removed
 * achievement silently disappears instead of failing the load). Writes go to
 * achievements.txt.tmp and are renamed over the live file, mirroring
 * sram_buffer.cpp's crash-safe flush.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fscanf below; harmless on non-MSVC */

#include "gdx_achievements.h"

#include "port_log.h" // gdx_port_logf (static inline; an extern declaration does not link)

#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

extern "C" int CVarGetInteger(const char* name, int defaultValue);

/* gdx_menu.cpp:176, declared in gdx_menu_internal.h inside namespace gdxmenu. Read live each
 * tick; handles a not-yet-constructed GUI by returning false, so early boot ticks simply count
 * as "menu closed". Declared here rather than including the internal header, which pulls the
 * menu's data model into this TU for one function. */
namespace gdxmenu {
bool GdxWindowVisible(const char* name);
}
using gdxmenu::GdxWindowVisible;

namespace {

/* ------------------------------------------------------------------ save mirror */

constexpr int32_t kMaxTimer = 60 * 60 * 1000 - 1; // MAX_TIMER, decomp/include/macros.h:25

enum {
    kCupJack = 0,
    kCupQueen = 1,
    kCupKing = 2,
    kCupJoker = 3,
    kCupX = 4, // cupSave stride covers 0..4; cupDifficultiesCleared only persists 0..3
};

struct GdxSaveSettingsMirror {
    uint8_t fileName[8];
    uint8_t settings;
    uint8_t customUnlocks;
    uint8_t cupDifficultiesCleared[4];
    uint16_t checksum;
};

struct GdxDeathRaceMirror {
    uint16_t checksum;
    int16_t unk_02;
    int32_t timeRecord[1];
    uint8_t unk_08[8];
};

/* Only the prefix up to bestTime is mirrored; the name/machine payload past it is
 * never read here. */
struct GdxCourseRecordsMirror {
    uint16_t checksum;
    int16_t unk_02;
    int32_t timeRecord[5];
    float engines[5];
    float maxSpeed;
    int32_t bestTime;
    uint8_t rest[0x110 - 0x34];
};

struct GdxDDCupsMirror {
    uint16_t checksum;
    uint16_t staffGhostCompletion;
    uint8_t cupCompletion[30 * 2];
};

struct GdxProfileSaveMirror {
    GdxSaveSettingsMirror saveSettings;
    GdxDeathRaceMirror deathRace;
    GdxCourseRecordsMirror courses[24];
    GdxDDCupsMirror ddCups; // EXPANSION_KIT build: the editCup/ddCups union is ddCups
};

struct GdxCupSaveMirror {
    uint16_t checksum;
    int8_t unk_02[0xE];
    uint16_t cupCompletion[4][10];
};

struct GdxSaveContextMirror {
    GdxProfileSaveMirror profileSaves[2];
    uint8_t ghostAndCharacters[0x3FC0 + 24 * 0x80]; // GhostSave + characterSaves[24]
    GdxCupSaveMirror cupSave;
    uint8_t tail[0x20];
};

static_assert(sizeof(GdxSaveSettingsMirror) == 0x10, "SaveSettings layout drift vs fzx_save.h");
static_assert(sizeof(GdxDeathRaceMirror) == 0x10, "SaveDeathRace layout drift vs fzx_save.h");
static_assert(offsetof(GdxCourseRecordsMirror, bestTime) == 0x30, "SaveCourseRecords layout drift");
static_assert(sizeof(GdxCourseRecordsMirror) == 0x110, "SaveCourseRecords layout drift vs fzx_save.h");
static_assert(sizeof(GdxDDCupsMirror) == 0x40, "SaveDDCups layout drift vs fzx_save.h");
static_assert(sizeof(GdxProfileSaveMirror) == 0x19E0, "ProfileSave layout drift vs fzx_save.h");
static_assert(offsetof(GdxProfileSaveMirror, ddCups) == 0x19A0, "ProfileSave union offset drift");
static_assert(sizeof(GdxCupSaveMirror) == 0x60, "CupSave layout drift vs fzx_save.h");
static_assert(offsetof(GdxSaveContextMirror, cupSave) == 0x7F80, "SaveContext cupSave offset drift");
static_assert(sizeof(GdxSaveContextMirror) == 0x8000, "SaveContext layout drift vs fzx_save.h");

} // namespace

extern "C" GdxSaveContextMirror gSaveContext; // decomp/src/overlays/ovl_i2/save.c

namespace {

/* ------------------------------------------------------------ achievement table */

const GdxProfileSaveMirror& LiveProfile() {
    return gSaveContext.profileSaves[0];
}

/* A live save always carries a non-zero profile file name (Save_InitSaveSettings
 * writes the default even for a fresh file); all-zero means pre-Save_Init BSS. */
bool SaveLooksLive() {
    const uint8_t* fileName = gSaveContext.profileSaves[0].saveSettings.fileName;
    for (int i = 0; i < 8; i++) {
        if (fileName[i] != 0) {
            return true;
        }
    }
    return false;
}

bool TimerRecordValid(int32_t ms) {
    return ms > 0 && ms < kMaxTimer;
}

bool CupDifficultyCountAtLeast(int cup, int count) {
    return LiveProfile().saveSettings.cupDifficultiesCleared[cup] >= count;
}

bool AllGpCupsAtLeast(int count) {
    for (int cup = kCupJack; cup <= kCupJoker; cup++) {
        if (!CupDifficultyCountAtLeast(cup, count)) {
            return false;
        }
    }
    return true;
}

bool DdCupAnyDifficulty(int ddCup, int difficultyMask) {
    const uint8_t* completion = LiveProfile().ddCups.cupCompletion;
    for (int character = 0; character < 30; character++) {
        if ((completion[character * 2 + ddCup] & difficultyMask) != 0) {
            return true;
        }
    }
    return false;
}

int StaffGhostCount() {
    uint16_t bits = LiveProfile().ddCups.staffGhostCompletion & 0xFFF;
    int count = 0;
    while (bits != 0) {
        count += bits & 1;
        bits >>= 1;
    }
    return count;
}

bool CupSaveBit(int difficulty, int character, int cupType) {
    const uint16_t slot = gSaveContext.cupSave.cupCompletion[difficulty][character / 3];
    return ((slot >> ((character % 3) * 5 + cupType)) & 1) != 0;
}

int CourseRecordCount() {
    int count = 0;
    for (int i = 0; i < 24; i++) {
        if (TimerRecordValid(LiveProfile().courses[i].bestTime)) {
            count++;
        }
    }
    return count;
}

constexpr int kAnyDifficultyBits = 0x55;  // bits 0/2/4/6 = Novice..Master
constexpr int kMasterBit = 1 << (3 * 2);

bool EvalJackClear(uint64_t) { return CupDifficultyCountAtLeast(kCupJack, 1); }
bool EvalQueenClear(uint64_t) { return CupDifficultyCountAtLeast(kCupQueen, 1); }
bool EvalKingClear(uint64_t) { return CupDifficultyCountAtLeast(kCupKing, 1); }
bool EvalJokerClear(uint64_t) { return CupDifficultyCountAtLeast(kCupJoker, 1); }
bool EvalJackMaster(uint64_t) { return CupDifficultyCountAtLeast(kCupJack, 4); }
bool EvalQueenMaster(uint64_t) { return CupDifficultyCountAtLeast(kCupQueen, 4); }
bool EvalKingMaster(uint64_t) { return CupDifficultyCountAtLeast(kCupKing, 4); }
bool EvalJokerMaster(uint64_t) { return CupDifficultyCountAtLeast(kCupJoker, 4); }
bool EvalAllExpert(uint64_t) { return AllGpCupsAtLeast(3); }
bool EvalAllMaster(uint64_t) { return AllGpCupsAtLeast(4); }

bool EvalXCupClear(uint64_t) {
    for (int difficulty = 0; difficulty < 4; difficulty++) {
        for (int character = 0; character < 30; character++) {
            if (CupSaveBit(difficulty, character, kCupX)) {
                return true;
            }
        }
    }
    return false;
}

bool EvalVersatile5(uint64_t) {
    int charactersWithWin = 0;
    for (int character = 0; character < 30; character++) {
        for (int difficulty = 0; difficulty < 4; difficulty++) {
            const uint16_t slot = gSaveContext.cupSave.cupCompletion[difficulty][character / 3];
            if (((slot >> ((character % 3) * 5)) & 0x1F) != 0) {
                charactersWithWin++;
                break;
            }
        }
    }
    return charactersWithWin >= 5;
}

bool EvalDd1Clear(uint64_t) { return DdCupAnyDifficulty(0, kAnyDifficultyBits); }
bool EvalDd2Clear(uint64_t) { return DdCupAnyDifficulty(1, kAnyDifficultyBits); }
bool EvalDd1Master(uint64_t) { return DdCupAnyDifficulty(0, kMasterBit); }
bool EvalDd2Master(uint64_t) { return DdCupAnyDifficulty(1, kMasterBit); }

bool EvalGhostFirst(uint64_t) { return StaffGhostCount() >= 1; }
bool EvalGhostHalf(uint64_t) { return StaffGhostCount() >= 6; }
bool EvalGhostAll(uint64_t) { return StaffGhostCount() >= 12; }

bool EvalDeathRaceFirst(uint64_t) { return TimerRecordValid(LiveProfile().deathRace.timeRecord[0]); }
bool EvalDeathRaceSwift(uint64_t) {
    const int32_t best = LiveProfile().deathRace.timeRecord[0];
    return TimerRecordValid(best) && best <= 90000; // 1:30.000
}

bool EvalTtFirst(uint64_t) { return CourseRecordCount() >= 1; }
bool EvalTtHalf(uint64_t) { return CourseRecordCount() >= 12; }
bool EvalTtAll(uint64_t) { return CourseRecordCount() >= 24; }

bool EvalPlaytime1h(uint64_t playtimeSec) { return playtimeSec >= 1ull * 3600; }
bool EvalPlaytime10h(uint64_t playtimeSec) { return playtimeSec >= 10ull * 3600; }

struct AchievementDef {
    const char* id;
    const char* name;
    const char* description;
    /* True when the unlock condition holds in the current save/playtime state.
     * Unlocks latch, so re-true predicates on already-unlocked entries are free. */
    bool (*eval)(uint64_t playtimeSeconds);
    /* Save-reading predicates are deferred until a live save has been observed;
     * playtime predicates run from the first tick. */
    bool needsLiveSave;
};

constexpr AchievementDef kAchievements[] = {
    { "gp.jack.clear", "Jack Cup Clear", "Clear the Jack Cup on any difficulty.", EvalJackClear, true },
    { "gp.queen.clear", "Queen Cup Clear", "Clear the Queen Cup on any difficulty.", EvalQueenClear, true },
    { "gp.king.clear", "King Cup Clear", "Clear the King Cup on any difficulty.", EvalKingClear, true },
    { "gp.joker.clear", "Joker Cup Clear", "Clear the Joker Cup on any difficulty.", EvalJokerClear, true },
    { "gp.jack.master", "Jack Cup Master", "Clear the Jack Cup on Master difficulty.", EvalJackMaster, true },
    { "gp.queen.master", "Queen Cup Master", "Clear the Queen Cup on Master difficulty.", EvalQueenMaster, true },
    { "gp.king.master", "King Cup Master", "Clear the King Cup on Master difficulty.", EvalKingMaster, true },
    { "gp.joker.master", "Joker Cup Master", "Clear the Joker Cup on Master difficulty.", EvalJokerMaster, true },
    { "gp.all_expert", "Expert Champion", "Clear every Grand Prix cup on Expert or higher.", EvalAllExpert, true },
    { "gp.all_master", "Master of Masters", "Clear every Grand Prix cup on Master difficulty.", EvalAllMaster, true },
    { "x.clear", "X Cup Clear", "Win the X Cup with any machine.", EvalXCupClear, true },
    { "gp.versatile5", "Versatile Racer", "Win Grand Prix cups with 5 different machines.", EvalVersatile5, true },
    { "dd1.clear", "DD-1 Cup Clear", "Clear the DD-1 Cup on any difficulty.", EvalDd1Clear, true },
    { "dd2.clear", "DD-2 Cup Clear", "Clear the DD-2 Cup on any difficulty.", EvalDd2Clear, true },
    { "dd1.master", "DD-1 Cup Master", "Clear the DD-1 Cup on Master difficulty.", EvalDd1Master, true },
    { "dd2.master", "DD-2 Cup Master", "Clear the DD-2 Cup on Master difficulty.", EvalDd2Master, true },
    { "ghost.first", "Ghost Buster", "Beat a staff ghost on any DD course.", EvalGhostFirst, true },
    { "ghost.half", "Ghost Hunter", "Beat staff ghosts on 6 DD courses.", EvalGhostHalf, true },
    { "ghost.all", "Ghost Legend", "Beat the staff ghosts on all 12 DD courses.", EvalGhostAll, true },
    { "deathrace.first", "Death Race Survivor", "Set a Death Race time record.", EvalDeathRaceFirst, true },
    { "deathrace.swift", "Speed Killer", "Finish the Death Race in 1:30.000 or better.", EvalDeathRaceSwift, true },
    { "tt.first", "First Record", "Set a course time record on any course.", EvalTtFirst, true },
    { "tt.half", "Record Collector", "Hold course records on 12 courses.", EvalTtHalf, true },
    { "tt.all", "Record Master", "Hold course records on all 24 courses.", EvalTtAll, true },
    { "playtime.1h", "Getting Started", "Play for a total of 1 hour.", EvalPlaytime1h, false },
    { "playtime.10h", "Dedicated Pilot", "Play for a total of 10 hours.", EvalPlaytime10h, false },
};

constexpr size_t kAchievementCount = sizeof(kAchievements) / sizeof(kAchievements[0]);

/* -------------------------------------------------------------------- state */

bool sInitialized = false;
bool sUnlocked[kAchievementCount] = {};
int64_t sUnlockTime[kAchievementCount] = {};

uint64_t sTotalPlaytimeSeconds = 0;   // persisted
uint64_t sSessionPlaytimeSeconds = 0; // process-local
double sPlaytimeCarrySeconds = 0.0;   // sub-second remainder between ticks

bool sSawLiveSave = false; // retroactive silent grant happens on the transition to true

size_t sToastQueue[kAchievementCount];
size_t sToastCount = 0;

bool sDirty = false;
std::chrono::steady_clock::time_point sLastTick;
bool sHaveLastTick = false;
std::chrono::steady_clock::time_point sLastPersist;

constexpr const char* kGateCvar = "gEnhancements.Achievements.Enabled";
constexpr const char* kMenuWindowName = "G-Diffuser Menu"; // gdx_menu.cpp:436
constexpr const char* kFileName = "achievements.txt";
constexpr double kMaxTickDeltaSeconds = 1.0;  // larger gaps (debugger, suspend) are discarded
constexpr double kPersistIntervalSeconds = 30.0;

/* --------------------------------------------------------------- path + file IO */

/* saves/ next to fzerox.sav: exe directory on Windows, CWD on POSIX, matching
 * sram_buffer.cpp's gdx_sram_path (minus its legacy migration -- this file is new). */
bool ResolvePath(char* outPath, size_t outCap) {
    if (outPath == nullptr || outCap == 0) {
        return false;
    }
#ifdef _WIN32
    char exePath[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exePath, (DWORD) sizeof(exePath));
    if (n == 0 || n >= sizeof(exePath)) {
        return false;
    }
    char* slash = strrchr(exePath, '\\');
    if (slash == nullptr) {
        return false;
    }
    slash[1] = '\0';
    if (strlen(exePath) + strlen("saves\\") + strlen(kFileName) + 1 > outCap) {
        return false;
    }
    strcpy(outPath, exePath);
    strcat(outPath, "saves");
    if (!CreateDirectoryA(outPath, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    strcat(outPath, "\\");
    strcat(outPath, kFileName);
    return true;
#else
    if (strlen("saves/") + strlen(kFileName) + 1 > outCap) {
        return false;
    }
    if (mkdir("saves", 0755) != 0 && errno != EEXIST) {
        return false;
    }
    strcpy(outPath, "saves/");
    strcat(outPath, kFileName);
    return true;
#endif
}

void Persist() {
    char path[1024];
    if (!ResolvePath(path, sizeof(path))) {
        gdx_port_logf("[achievements] WARNING: could not resolve state file path; state not persisted.\n");
        return;
    }

    char tempPath[1024 + 8];
    if (strlen(path) + 4 >= sizeof(tempPath)) {
        gdx_port_logf("[achievements] WARNING: state path too long for temp file; state not persisted.\n");
        return;
    }
    strcpy(tempPath, path);
    strcat(tempPath, ".tmp");

    FILE* f = fopen(tempPath, "wb");
    if (f == nullptr) {
        gdx_port_logf("[achievements] WARNING: failed to open %s for writing; state not persisted.\n", tempPath);
        return;
    }
    bool ok = fprintf(f, "version=1\nplaytime_seconds=%llu\n",
                      (unsigned long long) sTotalPlaytimeSeconds) >= 0;
    for (size_t i = 0; i < kAchievementCount && ok; i++) {
        if (sUnlocked[i]) {
            ok = fprintf(f, "unlock=%s %lld\n", kAchievements[i].id, (long long) sUnlockTime[i]) >= 0;
        }
    }
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[achievements] WARNING: failed writing %s; state not persisted.\n", tempPath);
        remove(tempPath);
        return;
    }
#ifdef _WIN32
    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
    if (rename(tempPath, path) != 0) { // atomic within a filesystem
#endif
        gdx_port_logf("[achievements] WARNING: could not replace %s; state not persisted.\n", path);
        remove(tempPath);
        return;
    }
    sDirty = false;
    sLastPersist = std::chrono::steady_clock::now();
}

void LoadState() {
    sTotalPlaytimeSeconds = 0;
    for (size_t i = 0; i < kAchievementCount; i++) {
        sUnlocked[i] = false;
        sUnlockTime[i] = 0;
    }

    char path[1024];
    if (!ResolvePath(path, sizeof(path))) {
        gdx_port_logf("[achievements] WARNING: could not resolve state file path; starting fresh.\n");
        return;
    }
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return; // no state file yet: fresh state is valid
    }

    char line[192];
    while (fgets(line, sizeof(line), f) != nullptr) {
        unsigned long long playtime;
        if (sscanf(line, "playtime_seconds=%llu", &playtime) == 1) {
            sTotalPlaytimeSeconds = (uint64_t) playtime;
            continue;
        }
        if (strncmp(line, "unlock=", 7) == 0) {
            char id[64];
            long long ts;
            if (sscanf(line + 7, "%63s %lld", id, &ts) != 2) {
                continue;
            }
            for (size_t i = 0; i < kAchievementCount; i++) {
                if (strcmp(id, kAchievements[i].id) == 0) {
                    sUnlocked[i] = true;
                    sUnlockTime[i] = (int64_t) ts;
                    break;
                }
            }
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------- internals */

void FillInfo(size_t index, GdxAchievementInfo* out) {
    out->id = kAchievements[index].id;
    out->name = kAchievements[index].name;
    out->description = kAchievements[index].description;
    out->unlocked = sUnlocked[index];
    out->unlockUnixTime = sUnlocked[index] ? sUnlockTime[index] : 0;
}

void Unlock(size_t index, bool silent) {
    sUnlocked[index] = true;
    sUnlockTime[index] = (int64_t) time(nullptr);
    sDirty = true;
    if (!silent && sToastCount < kAchievementCount) {
        sToastQueue[sToastCount++] = index;
    }
    gdx_port_logf("[achievements] unlocked %s (%s)%s\n", kAchievements[index].id, kAchievements[index].name,
                  silent ? " [retroactive]" : "");
}

void Evaluate(uint64_t playtimeSeconds, bool silentGrant) {
    for (size_t i = 0; i < kAchievementCount; i++) {
        if (sUnlocked[i]) {
            continue;
        }
        if (kAchievements[i].needsLiveSave && !sSawLiveSave) {
            continue;
        }
        // The silent retro-grant covers only save-reading predicates; a playtime
        // achievement crossing its threshold in that same first tick still toasts.
        if (kAchievements[i].eval(playtimeSeconds)) {
            Unlock(i, silentGrant && kAchievements[i].needsLiveSave);
        }
    }
}

void EnsureInit() {
    if (!sInitialized) {
        GdxAchievements_Init();
    }
}

} // namespace

/* -------------------------------------------------------------------- public API */

void GdxAchievements_Init(void) {
    if (sInitialized) {
        return;
    }
    sInitialized = true;
    LoadState();
    sLastPersist = std::chrono::steady_clock::now();
}

void GdxAchievements_Shutdown(void) {
    EnsureInit();
    if (sDirty) {
        Persist();
    }
}

void GdxAchievements_Tick(void) {
    EnsureInit();

    const auto now = std::chrono::steady_clock::now();
    const double deltaSeconds =
        sHaveLastTick ? std::chrono::duration<double>(now - sLastTick).count() : 0.0;
    sLastTick = now;
    sHaveLastTick = true;

    if (CVarGetInteger(kGateCvar, 1) == 0) {
        return; // gate off: no playtime, no evaluation; the delta was already consumed
    }

    // Menu-open time is not playtime; implausible deltas (debugger breaks, system
    // suspend) are dropped rather than clamped so they cannot inflate the total.
    if (deltaSeconds > 0.0 && deltaSeconds <= kMaxTickDeltaSeconds && !GdxWindowVisible(kMenuWindowName)) {
        sPlaytimeCarrySeconds += deltaSeconds;
        const uint64_t wholeSeconds = (uint64_t) sPlaytimeCarrySeconds;
        if (wholeSeconds > 0) {
            sPlaytimeCarrySeconds -= (double) wholeSeconds;
            sTotalPlaytimeSeconds += wholeSeconds;
            sSessionPlaytimeSeconds += wholeSeconds;
            sDirty = true;
        }
    }

    // First tick with a live save: grant everything already earned, silently.
    const bool firstLiveEvaluation = !sSawLiveSave && SaveLooksLive();
    if (firstLiveEvaluation) {
        sSawLiveSave = true;
    }
    Evaluate(sTotalPlaytimeSeconds, /*silentGrant=*/ firstLiveEvaluation);

    // Persist promptly after an unlock burst, otherwise debounce playtime writes.
    if (sDirty) {
        const double sincePersist = std::chrono::duration<double>(now - sLastPersist).count();
        if (sToastCount > 0 || sincePersist >= kPersistIntervalSeconds) {
            Persist();
        }
    }
}

size_t GdxAchievements_Count(void) {
    return kAchievementCount;
}

bool GdxAchievements_Get(size_t index, GdxAchievementInfo* out) {
    EnsureInit();
    if (out == nullptr || index >= kAchievementCount) {
        return false;
    }
    FillInfo(index, out);
    return true;
}

uint64_t GdxAchievements_GetTotalPlaytimeSeconds(void) {
    EnsureInit();
    return sTotalPlaytimeSeconds;
}

uint64_t GdxAchievements_GetSessionPlaytimeSeconds(void) {
    EnsureInit();
    return sSessionPlaytimeSeconds;
}

size_t GdxAchievements_DrainNewlyUnlocked(GdxAchievementInfo* out, size_t capacity) {
    EnsureInit();
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    const size_t n = sToastCount < capacity ? sToastCount : capacity;
    for (size_t i = 0; i < n; i++) {
        FillInfo(sToastQueue[i], &out[i]);
    }
    // Preserve any entries beyond capacity for the next drain rather than dropping them.
    for (size_t i = n; i < sToastCount; i++) {
        sToastQueue[i - n] = sToastQueue[i];
    }
    sToastCount -= n;
    return n;
}

void GdxAchievements_GetFilePath(char* outPath, size_t outCap) {
    if (outPath == nullptr || outCap == 0) {
        return;
    }
    if (!ResolvePath(outPath, outCap)) {
        outPath[0] = '\0';
    }
}
