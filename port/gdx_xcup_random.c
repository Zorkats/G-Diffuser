/* port/gdx_xcup_random.c -- the X Cup random course toggle and the saved copy of a generated course.
 * See gdx_xcup_random.h for the contract.
 */

#include "gdx_xcup_random.h"

#include "gdx_content_io.h"
#include "gdx_xcup_seed.h"

#include <string.h>

extern int CVarGetInteger(const char* name, int defaultValue);

/* COURSE_X_1 and the DD cups of the Cups enum, which this file can't include. */
#define GDX_XCUP_FIRST_COURSE 48
#define GDX_XCUP_DD_1_CUP 6
#define GDX_XCUP_DD_2_CUP 7

static int sDeck[GDX_XCUP_CUP_SIZE] = { -1, -1, -1, -1, -1, -1 };
static int sActiveCourse = -1;

static uint8_t sGeneratedPayload[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
static int sGeneratedValid = 0;

static int gdx_xcup_random_pool_course(int position) {
    if (position < GDX_XCUP_RANDOM_STOCK_COUNT) {
        return position;
    }
    return GDX_XCUP_RANDOM_DD_FIRST + (position - GDX_XCUP_RANDOM_STOCK_COUNT);
}

static int gdx_xcup_random_in_deck(int course) {
    int i;
    for (i = 0; i < GDX_XCUP_CUP_SIZE; i++) {
        if (sDeck[i] == course) {
            return 1;
        }
    }
    return 0;
}

int gdx_xcup_random_enabled(void) {
    return CVarGetInteger(GDX_XCUP_RANDOM_CVAR, 0) != 0;
}

int gdx_xcup_random_pick(int slot, int newCup, uint32_t roll, int ddAvailable) {
    int poolSize = GDX_XCUP_RANDOM_STOCK_COUNT + (ddAvailable ? GDX_XCUP_RANDOM_DD_COUNT : 0);
    int freeCount = 0;
    int skip;
    int i;

    if (slot < 0 || slot >= GDX_XCUP_CUP_SIZE) {
        return -1;
    }
    sGeneratedValid = 0;
    if (newCup) {
        for (i = 0; i < GDX_XCUP_CUP_SIZE; i++) {
            sDeck[i] = -1;
        }
    }
    if (sDeck[slot] < 0) {
        for (i = 0; i < poolSize; i++) {
            if (!gdx_xcup_random_in_deck(gdx_xcup_random_pool_course(i))) {
                freeCount++;
            }
        }
        /* The pool is at least 24 and the deck holds at most 5 others, so freeCount is never 0. */
        skip = (int) (roll % (uint32_t) freeCount);
        for (i = 0; i < poolSize; i++) {
            int course = gdx_xcup_random_pool_course(i);
            if (!gdx_xcup_random_in_deck(course) && skip-- == 0) {
                sDeck[slot] = course;
                break;
            }
        }
    }
    sActiveCourse = sDeck[slot];
    return sActiveCourse;
}

void gdx_xcup_random_clear_active(void) {
    sActiveCourse = -1;
}

int gdx_xcup_random_active_course(void) {
    return sActiveCourse;
}

int gdx_xcup_real_course(int courseIndex) {
    if (sActiveCourse >= 0 && courseIndex >= GDX_XCUP_FIRST_COURSE &&
        courseIndex < GDX_XCUP_FIRST_COURSE + GDX_XCUP_CUP_SIZE) {
        return sActiveCourse;
    }
    return courseIndex;
}

int gdx_xcup_real_cup(int cupType, int courseIndex) {
    int course = gdx_xcup_real_course(courseIndex);

    if (course == courseIndex) {
        return cupType;
    }
    if (course < GDX_XCUP_RANDOM_STOCK_COUNT) {
        return course / GDX_XCUP_CUP_SIZE;
    }
    return course < GDX_XCUP_RANDOM_DD_FIRST + GDX_XCUP_CUP_SIZE ? GDX_XCUP_DD_1_CUP : GDX_XCUP_DD_2_CUP;
}

void gdx_xcup_generated_capture(const void* courseData, size_t size) {
    if (courseData == NULL || size > sizeof(sGeneratedPayload)) {
        return;
    }
    /* The tail is zero: the same image the editor writes for a track that has never been raced. */
    memset(sGeneratedPayload, 0, sizeof(sGeneratedPayload));
    memcpy(sGeneratedPayload, courseData, size);
    sGeneratedValid = 1;
}

const uint8_t* gdx_xcup_generated_payload(void) {
    return sGeneratedValid ? sGeneratedPayload : NULL;
}
