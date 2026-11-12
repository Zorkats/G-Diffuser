/* port/gdx_net_rules_audit.c -- the classification table of leftover L1 (plan decision 44).
 * Copyright (c) Zorkats
 *
 * One row per setting that racer, CPU or course code reads, or whose name sits under a gameplay
 * namespace. port/tests/gdx_net_rules_audit_tests.cpp keeps this table in step with the source.
 */

#include "gdx_net_rules_audit.h"

#include <stddef.h>
#include <string.h>

#define G "gEnhancements.Gameplay."

static const GdxNetRuleEntry kRules[] = {
    /* GRID: the host's value is every racer's value. */
    { G "IgnoreTrackSettings", GDX_NET_RULE_GRID, GDX_NET_RULE_SCANNED, "O3",
      "racer.c and cpu.c take the course's own physics flags or not" },
    { G "LavaNoKill", GDX_NET_RULE_GRID, GDX_NET_RULE_SCANNED, "O3", "whether lava retires a machine" },
    { G "MagnetPullRange", GDX_NET_RULE_GRID, GDX_NET_RULE_SCANNED, "O3",
      "Racer_MagnetSetting in racer.c; the CPU native path reads it too" },
    { G "MagnetPullStrength", GDX_NET_RULE_GRID, GDX_NET_RULE_BY_HAND, "O3",
      "Racer_MagnetSetting in racer.c takes the name as an argument" },
    { "gEnhancements.Tuning.BoostDuration", GDX_NET_RULE_GRID, GDX_NET_RULE_SCANNED, "O4.4",
      "every racer's boost length (enhancements/tuning/BoostDuration.cpp); carried in the grid since O4.4" },
    { "gEnhancements.CourseEdit.ExtendedHeight", GDX_NET_RULE_GRID, GDX_NET_RULE_SCANNED, "O4.8",
      "gdx_course_bounds.c: the height ceiling decides whether a course passes its checksum" },

    /* HOST_CPU: only the host runs the CPUs. */
    { G "AiAggression", GDX_NET_RULE_HOST_CPU, GDX_NET_RULE_SCANNED, "D28.1", "how hard CPUs attack" },
    { G "CpuTrackHazards", GDX_NET_RULE_HOST_CPU, GDX_NET_RULE_SCANNED, "D28.1",
      "whether CPUs avoid hazards; the host's CPU code only" },
    { "gEnhancements.CustomGrid.Roster", GDX_NET_RULE_HOST_CPU, GDX_NET_RULE_BY_HAND, "D28.1",
      "which machines fill the CPU seats; read through GDX_CUSTOM_GRID_CVAR" },

    /* PERSONAL: input feel. */
    { "gEnhancements.Input.MouseSteering", GDX_NET_RULE_PERSONAL, GDX_NET_RULE_SCANNED, NULL,
      "the cursor steers the local player's machine" },
    { "gEnhancements.Input.MouseSteeringSensitivity", GDX_NET_RULE_PERSONAL, GDX_NET_RULE_SCANNED, NULL,
      "the cursor steering scale" },
    { "gEnhancements.Input.SteeringExponentPort1", GDX_NET_RULE_PERSONAL, GDX_NET_RULE_BY_HAND, NULL,
      "the stick curve, from the table in gdx_steering_curve.c" },
    { "gEnhancements.Input.SteeringExponentPort2", GDX_NET_RULE_PERSONAL, GDX_NET_RULE_BY_HAND, NULL,
      "the stick curve, from the table in gdx_steering_curve.c" },
    { "gEnhancements.Input.SteeringExponentPort3", GDX_NET_RULE_PERSONAL, GDX_NET_RULE_BY_HAND, NULL,
      "the stick curve, from the table in gdx_steering_curve.c" },
    { "gEnhancements.Input.SteeringExponentPort4", GDX_NET_RULE_PERSONAL, GDX_NET_RULE_BY_HAND, NULL,
      "the stick curve, from the table in gdx_steering_curve.c" },
    { "gEnhancements.Input.Profile", GDX_NET_RULE_PERSONAL, GDX_NET_RULE_BY_HAND, NULL,
      "the button layout, through GDX_INPUT_PROFILE_CVAR" },

    /* LOCAL: what one copy shows or keeps. */
    { G "RandomOpponentColors", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "machine colours on this screen" },
    { G "SideAttackColor", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "a draw colour" },
    { G "SkippableTransitions", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "menu transitions on this copy" },
    { G "AutosaveOnRecord", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL,
      "a save setting; a room race writes no saves (ODD 8.1.5)" },
    { G "BoostColor", GDX_NET_RULE_LOCAL, GDX_NET_RULE_BY_HAND, NULL, "a trail colour, through a racer.c macro" },
    { G "DashPadColor", GDX_NET_RULE_LOCAL, GDX_NET_RULE_BY_HAND, NULL, "a trail colour, through a racer.c macro" },
    { G "BoostIdleColor", GDX_NET_RULE_LOCAL, GDX_NET_RULE_BY_HAND, NULL, "a trail colour, through a racer.c macro" },
    { G "GhostBoostColor", GDX_NET_RULE_LOCAL, GDX_NET_RULE_BY_HAND, NULL, "a trail colour, through a racer.c macro" },
    { "gEnhancements.Graphics.FancyLighting", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "drawing only" },
    { "gEnhancements.Graphics.ForceDrawMachineShad", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "drawing only" },
    { "gEnhancements.Graphics.ForceMaxMachineLOD", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "drawing only" },
    { "gEnhancements.Graphics.MachineBoostLODDistMod", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "drawing only" },
    { "gEnhancements.Graphics.MachineLODDistMod", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "drawing only" },
    { "gEnhancements.Graphics.TrackDepthTest", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "drawing only" },
    { "gEnhancements.CourseEdit.KeepStaffTime", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL,
      "which record a saved course keeps" },
    { "gEnhancements.Practice.ShowLapDeltas", GDX_NET_RULE_LOCAL, GDX_NET_RULE_SCANNED, NULL, "a HUD readout" },

    /* NOT_IN_ROOM: a room never reaches the code, or overrides it. */
    { "gEnhancements.SingleRace.Enabled", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "the room forces it through gdx_single_race_force_next (gdx_net_race_bridge.c)" },
    { "gEnhancements.SingleRace.Laps", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "the room forces the lap count" },
    { "gEnhancements.SingleRace.LapCount", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "the room forces the lap count" },
    { "gEnhancements.SingleRace.UnlimitedLaps", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "the room forces the lap count" },
    { "gEnhancements.SingleRace.CpuCount", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "the room forces the field" },
    { "gEnhancements.SingleRace.RandomRivals", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "the room forces the field" },
    { G "DeathRaceLaps", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Death Race only" },
    { G "DeathRaceCourseSelect", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Death Race only" },
    { G "TestDriveCpus", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Course Edit test drive only" },
    { G "TestDriveDifficulty", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Course Edit test drive only" },
    { G "TestDriveEngine", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Course Edit test drive only" },
    { G "TestDriveMachine", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Course Edit test drive only" },
    { G "CourseEditOfficialBack", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Course Edit only" },
    { G "CourseEditEndingVenue", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "the Course Edit preview loader (segment.c)" },
    { G "ReduceEditorFlashing", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Course Edit only" },
    { G "SaveCupProgress", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "Grand Prix only; a room race writes no saves" },
    { G "SkippableEnding", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL, "Grand Prix ending only" },
    { "gEnhancements.Multiplayer.SplitGrandPrix", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "split-screen Grand Prix only" },
    { "gEnhancements.Multiplayer.SplitGrandPrixField", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "split-screen Grand Prix only" },
    { "gEnhancements.Practice.PhotoMode", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_SCANNED, NULL,
      "needs the pause menu; a room race has none" },
    { "gEnhancements.Hacks.Active", GDX_NET_RULE_NOT_IN_ROOM, GDX_NET_RULE_BY_HAND, NULL,
      "a latched ROM hack cannot host or join (decision 47, gdx_lan_lobby.cpp Launch)" },
};

#undef G

int gdx_net_rules_audit_count(void) {
    return (int)(sizeof(kRules) / sizeof(kRules[0]));
}

const GdxNetRuleEntry* gdx_net_rules_audit_entry(int index) {
    if (index < 0 || index >= gdx_net_rules_audit_count()) {
        return NULL;
    }
    return &kRules[index];
}

const GdxNetRuleEntry* gdx_net_rules_audit_find(const char* name) {
    int i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < gdx_net_rules_audit_count(); i++) {
        if (strcmp(kRules[i].name, name) == 0) {
            return &kRules[i];
        }
    }
    return NULL;
}

int gdx_net_rules_audit_is_gameplay_name(const char* name) {
    static const char* const kPrefixes[] = {
        "gEnhancements.Gameplay.", "gEnhancements.Tuning.",   "gEnhancements.SingleRace.",
        "gEnhancements.Multiplayer.", "gEnhancements.Practice.", "gEnhancements.Hacks.",
        "gEnhancements.CustomGrid.",
    };
    size_t i;
    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); i++) {
        if (strncmp(name, kPrefixes[i], strlen(kPrefixes[i])) == 0) {
            return 1;
        }
    }
    return 0;
}

const char* gdx_net_rules_audit_class_name(GdxNetRuleClass cls) {
    switch (cls) {
        case GDX_NET_RULE_GRID:
            return "grid";
        case GDX_NET_RULE_HOST_CPU:
            return "host cpu";
        case GDX_NET_RULE_PERSONAL:
            return "personal";
        case GDX_NET_RULE_LOCAL:
            return "local";
        case GDX_NET_RULE_NOT_IN_ROOM:
            return "not in room";
        default:
            return "unknown";
    }
}
