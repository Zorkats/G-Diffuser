/* port/gdx_course_splits_course.c -- the decomp course data gdx_course_splits_view.c needs. See
 * gdx_course_splits_view.h. */

#include "global.h"
#include "fzx_game.h"
#include "fzx_course.h"
#include "gdx_course_splits_view.h"

extern CourseInfo* gCurrentCourseInfo;
extern s32 gGameMode;
extern s32 gNumPlayers;
extern s32 gTotalLapCount;

int gdx_splits_course_nodes(float* nodeAt, int cap) {
    const CourseInfo* info = gCurrentCourseInfo;
    f32 last = 0.0f;
    s32 count;
    s32 i;

    if ((nodeAt == NULL) || (info == NULL) || (info->courseSegments == NULL) || !(info->length > 0.0f)) {
        return 0;
    }
    /* Segment 0 starts at the line, so every later segment start is a checkpoint. */
    count = info->segmentCount - 1;
    if ((count < 1) || (count > cap)) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        f32 at = info->courseSegments[i + 1].lengthFromStart / info->length;

        if (!(at > last) || !(at < 1.0f)) {
            return 0;
        }
        nodeAt[i] = at;
        last = at;
    }
    return count;
}

int gdx_splits_course_recordable(void) {
    return (gGameMode == GAMEMODE_TIME_ATTACK) && (gNumPlayers == 1);
}

int gdx_splits_course_laps(void) {
    return gTotalLapCount;
}
