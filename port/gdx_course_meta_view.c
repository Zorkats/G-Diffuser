/* port/gdx_course_meta_view.c -- see gdx_course_meta_view.h. */

#include "gdx_course_meta_view.h"

#include "gdx_course_meta.h"
#include "gdx_course_music.h"
#include "gdx_course_splits_view.h"
#include "gdx_course_state.h"
#include "gdx_profile_state.h"
#include "port_log.h"

#include <string.h>

/* decomp/src/game/racer.c: the course file id of the course about to start, 1 when out holds one. */
extern int gdx_racer_course_file_id(char* out, int outCap);
extern int CVarGetInteger(const char* name, int defaultValue); // libultraship consolevariablebridge.h

/* The meta of the raced course, empty when it has none. 0 when no course file is raced. */
static int raced_meta(char* courseId, int idCap, GdxCourseMeta* meta) {
    if (!gdx_racer_course_file_id(courseId, idCap)) {
        return 0;
    }
    if (!gdx_meta_store_get(courseId, meta)) {
        memset(meta, 0, sizeof(*meta));
    }
    return 1;
}

int gdx_meta_view_cpu_target(int raceLaps, int difficulty, int playerRecord, const unsigned int* stockTargets,
                             const int* stockStaff, int courseCount) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta meta;

    if (!raced_meta(courseId, sizeof(courseId), &meta)) {
        return -1;
    }
    return gdx_meta_cpu_target(&meta, raceLaps, difficulty, playerRecord, (const uint32_t*) stockTargets,
                               (const int32_t*) stockStaff, courseCount);
}

void gdx_meta_view_editor_saved(void) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta meta;
    int rc;

    if (!gdx_course_state_edit_file_id(courseId, sizeof(courseId))) {
        return;
    }
    if (!gdx_meta_store_get(courseId, &meta)) {
        memset(&meta, 0, sizeof(meta));
    }
    /* The active profile is the creator. With no profile the name fields stay as they are. Enhancements >
     * Course Edit > Keep staff time after editing decides whether the staff run survives the save. */
    gdx_meta_apply_editor_save(&meta, gdx_profile_state_active_name(),
                               CVarGetInteger("gEnhancements.CourseEdit.KeepStaffTime", 1) != 0);
    rc = gdx_meta_store_set(courseId, &meta);
    if (rc != GDX_META_OK) {
        gdx_port_logf("[meta] %s: editor save not recorded (%d)\n", courseId, rc);
        return;
    }
    if (gdx_course_state_flush() < 0) {
        gdx_port_logf("[meta] %s: state file not written, kept for the next flush\n", courseId);
    }
}

int gdx_meta_view_clear_staff(const char* courseId) {
    GdxCourseMeta meta;
    int rc;

    if ((courseId == NULL) || !gdx_meta_store_get(courseId, &meta) || (meta.staffTime == 0)) {
        return 0;
    }
    gdx_meta_clear_staff(&meta);
    rc = gdx_meta_store_set(courseId, &meta);
    if (rc != GDX_META_OK) {
        gdx_port_logf("[meta] %s: staff time not cleared (%d)\n", courseId, rc);
        return 0;
    }
    if (gdx_course_state_flush() < 0) {
        gdx_port_logf("[meta] %s: state file not written, kept for the next flush\n", courseId);
    }
    gdx_port_logf("[meta] %s: staff time cleared\n", courseId);
    return 1;
}

int gdx_meta_view_staff_offer(int raceLaps, int raceTime) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta meta;

    if ((raceTime <= 0) || !raced_meta(courseId, sizeof(courseId), &meta)) {
        return 0;
    }
    return gdx_meta_staff_offer(&meta, raceLaps, (uint32_t) raceTime);
}

int gdx_meta_view_set_staff(int raceLaps, int raceTime) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta meta;
    GdxCourseSplits splits;
    int rc;

    if ((raceTime <= 0) || !raced_meta(courseId, sizeof(courseId), &meta) ||
        !gdx_meta_staff_offer(&meta, raceLaps, (uint32_t) raceTime)) {
        return 0;
    }
    rc = gdx_meta_set_staff(&meta, raceLaps, (uint32_t) raceTime);
    if (rc == GDX_META_OK) {
        /* The run's splits go with it, so Expert and Master CPUs can follow the creator's pace. */
        if (!gdx_splits_view_finished(raceLaps, raceTime, &splits) ||
            (gdx_meta_set_splits(&meta, &splits) != GDX_META_OK)) {
            memset(&splits, 0, sizeof(splits));
        }
        rc = gdx_meta_store_set(courseId, &meta);
    }
    if (rc != GDX_META_OK) {
        gdx_port_logf("[meta] %s: staff time not stored (%d)\n", courseId, rc);
        return 0;
    }
    if (gdx_course_state_flush() < 0) {
        gdx_port_logf("[meta] %s: state file not written, kept for the next flush\n", courseId);
    }
    gdx_port_logf("[meta] %s: staff time %d over %d laps, splits: %d laps, %d checkpoints\n", courseId, raceTime,
                  raceLaps, splits.laps, splits.nodes);
    return 1;
}

/* Font_GetCharIndex (decomp/src/overlays/ovl_i2/font.c) draws these as themselves. Anything else would show
 * another glyph. Lowercase is left out: the caption fonts put kana where 'a' to 'z' land. */
static int font_char_ok(char c) {
    if (c == '\0') {
        return 0;
    }
    return ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (strchr(" '\"&+-:.,()", c) != NULL);
}

/* The caption form of c: uppercase, like the course names beside it, or a space when the font has no glyph. */
static char caption_char(char c) {
    if ((c >= 'a') && (c <= 'z')) {
        c = (char) (c - 'a' + 'A');
    }
    return font_char_ok(c) ? c : ' ';
}

static int append(char* out, int cap, int pos, const char* text) {
    while ((*text != '\0') && (pos < cap - 1)) {
        out[pos++] = caption_char(*text);
        text++;
    }
    out[pos] = '\0';
    return pos;
}

int gdx_meta_view_race_caption(char* out, int cap) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta meta;
    int pos = 0;

    if ((out == NULL) || (cap <= 0)) {
        return 0;
    }
    out[0] = '\0';
    if (!raced_meta(courseId, sizeof(courseId), &meta) || (meta.author[0] == '\0')) {
        return 0;
    }
    pos = append(out, cap, pos, "BY ");
    return append(out, cap, pos, meta.author);
}

int gdx_meta_view_caption(const char* trackName, int line, char* out, int cap) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    char time[16];
    GdxCourseMeta meta;
    int pos = 0;

    if ((out == NULL) || (cap <= 0)) {
        return 0;
    }
    out[0] = '\0';
    if ((trackName == NULL) || (trackName[0] == '\0') ||
        !gdx_course_music_course_id(trackName, "CRSD", courseId, sizeof(courseId)) ||
        !gdx_meta_store_get(courseId, &meta)) {
        return 0;
    }
    if ((line == GDX_META_CAPTION_AUTHOR) || (line == GDX_META_CAPTION_AUTHOR_SIGNED)) {
        if ((meta.author[0] != '\0') && ((line == GDX_META_CAPTION_AUTHOR) || meta.hasSignature)) {
            pos = append(out, cap, pos, "BY ");
            pos = append(out, cap, pos, meta.author);
            if (line == GDX_META_CAPTION_AUTHOR_SIGNED) {
                pos = append(out, cap, pos, " - SIGNED");
            }
        }
    } else if ((line == GDX_META_CAPTION_STAFF) && (meta.staffTime != 0) &&
               (gdx_meta_format_time(meta.staffTime, time, sizeof(time)) > 0)) {
        pos = append(out, cap, pos, "STAFF ");
        pos = append(out, cap, pos, time);
        if (meta.staffLaps != GDX_META_RECORD_LAPS) {
            char laps[8];
            int n = 0;

            laps[n++] = ' ';
            if (meta.staffLaps >= 10) {
                laps[n++] = (char) ('0' + meta.staffLaps / 10);
            }
            laps[n++] = (char) ('0' + meta.staffLaps % 10);
            memcpy(laps + n, " LAPS", 6);
            pos = append(out, cap, pos, laps);
        }
    }
    return pos;
}
