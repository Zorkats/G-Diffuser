/* port/tests/gdx_export_all_tests.c -- what "Export everything" puts in its zip (gdx_export_all.h). */

#include "../gdx_export_all.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

#define CHECK(cond, text)                 \
    do {                                  \
        if (cond) {                       \
            printf("PASS: %s\n", (text)); \
        } else {                          \
            printf("FAIL: %s\n", (text)); \
            sFailures++;                  \
        }                                 \
    } while (0)

typedef struct {
    const char* path;
    unsigned group;
} Case;

/* A real install's files (a test US layout), each with the group it belongs to. */
static const Case kCases[] = {
    /* Tracks and snapshots. */
    { "course-elements.gdxs", GDX_EXPORT_TRACKS },
    { "course-forks.gbs", GDX_EXPORT_TRACKS },
    { "course-meta.gms", GDX_EXPORT_TRACKS },
    { "course-settings.gks", GDX_EXPORT_TRACKS },
    { "saves/baserom.translated.ek.ndd.gdd", GDX_EXPORT_TRACKS },
    { "saves/baserom.translated.ek.ndd.gdd.recovery/slot0.bin", GDX_EXPORT_TRACKS },
    { "saves/baserom.translated.ek.ndd.gdd.recovery.snapshots/TRACK.snap", GDX_EXPORT_TRACKS },
    { "exports/LKT.gdxc", GDX_EXPORT_TRACKS },
    { "exports/FORK TEST.gdxp", GDX_EXPORT_TRACKS },
    { "exports/LKT.gdxm", GDX_EXPORT_TRACKS },
    /* Profiles, saves, records and ghosts. */
    { "profiles.gps", GDX_EXPORT_PLAYER },
    { "gp_resume.bin", GDX_EXPORT_PLAYER },
    { "gp_resume-hack-Climax.bin", GDX_EXPORT_PLAYER },
    { "death_race_records.bin", GDX_EXPORT_PLAYER },
    { "death_race_records-hack-Legends.bin", GDX_EXPORT_PLAYER },
    { "saves/fzerox.sav", GDX_EXPORT_PLAYER },
    { "saves/fzerox-hack-Climax.sav", GDX_EXPORT_PLAYER },
    { "saves/achievements.txt", GDX_EXPORT_PLAYER },
    { "ghosts/ghost_7B4113C0_7305F6F62DCFF068.gdg", GDX_EXPORT_PLAYER },
    { "ghosts/selection.gds", GDX_EXPORT_PLAYER },
    /* Settings and mods. */
    { "gdiffuser.cfg.json", GDX_EXPORT_SETTINGS },
    { "imgui.ini", GDX_EXPORT_SETTINGS },
    { "palette.txt", GDX_EXPORT_SETTINGS },
    { "machine_stats.txt", GDX_EXPORT_SETTINGS },
    { "mods/fraudio-hd.o2r", GDX_EXPORT_SETTINGS },
    { "mods/test-seq-bigblue.o2r", GDX_EXPORT_SETTINGS },
    { "mods/a folder pack/workshop.json", GDX_EXPORT_SETTINGS },
    { "music/ERNST.wav", GDX_EXPORT_SETTINGS },
    { "MUSIC/Big Blue.ogg", GDX_EXPORT_SETTINGS },
    /* Signing keys. */
    { "profile-keys.gpk", GDX_EXPORT_KEYS },
    { "PROFILE-KEYS.GPK", GDX_EXPORT_KEYS },
    /* ROMs and what is built from them. */
    { "baserom.us.rev0.z64", 0 },
    { "davidzsm.z64", 0 },
    { "mods/hidden.n64", 0 },
    { "music/disk.ndd", 0 },
    { "fzerox.o2r", 0 },
    { "fzerox-disk.o2r", 0 },
    { "n64ddipl.o2r", 0 },
    { "mods/generic.o2r", 0 },
    { "mods/FZEROX.O2R", 0 },
    { "mods/~romhacks/Climax.o2r", 0 },
    { "mods/~ROMHACKS/Legends.o2r", 0 },
    { "exports/machines/machine_1_lod1_1/machine.obj", 0 },
    { "exports/machines/machine_1_lod1_1/material_0.png", 0 },
    /* Shipped with the game, or of this machine only. */
    { "gdiffuser.o2r", 0 },
    { "G-Diffuser/gdiffuser.o2r", 0 },
    { "gamecontrollerdb.txt", 0 },
    { "gdx_firstboot.cfg", 0 },
    { "gdx_extract_state.cfg", 0 },
    { "gdiffuser-shadercache-d3d11.bin", 0 },
    { "gdiffuser-run.log", 0 },
    { "crash-address.txt", 0 },
    { "logs/gdiffuser.log", 0 },
    { "dump/manifest.tsv", 0 },
    { "decomp-recipes/config.yml", 0 },
    { "shaders/default.shader.hlsl", 0 },
    { "fonts/Montserrat-Regular.ttf", 0 },
    { "G-Diffuser.exe", 0 },
    { "SDL2.dll", 0 },
    /* Copies, locks and leftovers. */
    { "saves/baserom.translated.ek.ndd.gdd.bak", 0 },
    { "saves/baserom.translated.ek.ndd.gdd.recovery.lock", 0 },
    { "saves/baserom.translated.ek.ndd.gdd.recovery.snapshots.lock", 0 },
    { "saves/other.txt", 0 },
    { "saves/deep/fzerox.sav", 0 },
    { "ghosts/old/ghost.gdg", 0 },
    { "ghosts/notes.txt", 0 },
    { "exports/notes.txt", 0 },
    { "exports/edit-cup.zip", 0 },
    { "exports/gdiffuser-export-20261106-010203.zip", 0 },
    { "exports/GDIFFUSER-EXPORT-20261106-010203.zip", 0 },
    { "exports/other/LKT.gdxc", 0 },
    /* Not paths an export would take. */
    { "", 0 },
    { "/profiles.gps", 0 },
    { "C:/G-Diffuser/profiles.gps", 0 },
    { "saves\\fzerox.sav", 0 },
    { "saves/../profile-keys.gpk", 0 },
    { "../profiles.gps", 0 },
    { "./profiles.gps", 0 },
    { "saves//fzerox.sav", 0 },
    { "music/", 0 },
};

static void test_classify(void) {
    size_t i;
    int wrong = 0;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        const unsigned got = gdx_export_classify(kCases[i].path);

        if (got != kCases[i].group) {
            printf("  \"%s\": group 0x%x, expected 0x%x\n", kCases[i].path, got, kCases[i].group);
            wrong++;
        }
    }
    CHECK(wrong == 0, "every file of a real install lands in its group, and ROM data in none");
    CHECK(gdx_export_classify(NULL) == 0, "a NULL path is never exported");
}

static void test_folders(void) {
    CHECK(gdx_export_folder_wanted(""), "the walk starts at the root");
    CHECK(gdx_export_folder_wanted("saves") && gdx_export_folder_wanted("ghosts") &&
              gdx_export_folder_wanted("exports") && gdx_export_folder_wanted("mods") &&
              gdx_export_folder_wanted("music"),
          "the folders that hold exported files are walked");
    CHECK(gdx_export_folder_wanted("saves/baserom.translated.ek.ndd.gdd.recovery") &&
              gdx_export_folder_wanted("saves/baserom.translated.ek.ndd.gdd.recovery.snapshots") &&
              gdx_export_folder_wanted("saves/baserom.translated.ek.ndd.gdd.recovery.snapshots/inner"),
          "track recovery and snapshot folders are walked");
    CHECK(gdx_export_folder_wanted("mods/a folder pack") && gdx_export_folder_wanted("music/cups"),
          "pack and music subfolders are walked");
    CHECK(!gdx_export_folder_wanted("mods/~romhacks") && !gdx_export_folder_wanted("mods/~ROMHACKS/x") &&
              !gdx_export_folder_wanted("dump") && !gdx_export_folder_wanted("dump/ek") &&
              !gdx_export_folder_wanted("logs") && !gdx_export_folder_wanted("decomp-recipes") &&
              !gdx_export_folder_wanted("G-Diffuser") && !gdx_export_folder_wanted("shaders") &&
              !gdx_export_folder_wanted("ghosts/old") && !gdx_export_folder_wanted("saves/deep") &&
              !gdx_export_folder_wanted("exports/other") && !gdx_export_folder_wanted("exports/machines"),
          "ROM hack builds, machine captures, dumps, logs and shipped folders are skipped");
    CHECK(!gdx_export_folder_wanted(NULL) && !gdx_export_folder_wanted("../saves") &&
              !gdx_export_folder_wanted("mods/../dump"),
          "a folder that climbs out is skipped");
}

static void test_names(void) {
    char name[64];
    char tiny[8];

    CHECK((gdx_export_file_name(2026, 11, 6, 1, 2, 3, name, sizeof(name)) == 36) &&
              (strcmp(name, "gdiffuser-export-20261106-010203.zip") == 0),
          "the export is named after its date");
    {
        char inExports[80];

        snprintf(inExports, sizeof(inExports), "exports/%s", name);
        CHECK(gdx_export_classify(inExports) == 0, "and the next export leaves it out");
    }
    CHECK((gdx_export_file_name(2026, 13, 6, 1, 2, 3, name, sizeof(name)) == -1) &&
              (gdx_export_file_name(2026, 11, 0, 1, 2, 3, name, sizeof(name)) == -1) &&
              (gdx_export_file_name(2026, 11, 6, 24, 2, 3, name, sizeof(name)) == -1) &&
              (gdx_export_file_name(1969, 11, 6, 1, 2, 3, name, sizeof(name)) == -1),
          "a date out of range gives no name");
    CHECK((gdx_export_file_name(2026, 11, 6, 1, 2, 3, tiny, sizeof(tiny)) == -1) && (tiny[0] == '\0'),
          "a buffer too small gives no name and an empty string");
    CHECK(gdx_export_file_name(2026, 11, 6, 1, 2, 3, NULL, 0) == -1, "no buffer, no name");

    CHECK((strcmp(gdx_export_group_name(GDX_EXPORT_TRACKS), "Tracks and snapshots") == 0) &&
              (strcmp(gdx_export_group_name(GDX_EXPORT_KEYS), "Signing keys") == 0) &&
              (strcmp(gdx_export_group_name(GDX_EXPORT_TRACKS | GDX_EXPORT_KEYS), "") == 0) &&
              (strcmp(gdx_export_group_name(0), "") == 0),
          "every group has its name and a mix has none");
}

int main(void) {
    test_classify();
    test_folders();
    test_names();
    if (sFailures != 0) {
        printf("%d check(s) failed\n", sFailures);
        return 1;
    }
    printf("all export checks passed\n");
    return 0;
}
