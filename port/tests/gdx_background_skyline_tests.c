#define GDX_BACKGROUND_SKYLINE_TEST 1

#include "../../decomp/src/overlays/ovl_i3/background.c"

extern int printf(const char* format, ...);

static int expect_selection(const char* label, s32 courseIndex, s32 venueType, bool inCourseEditTestRun,
                            s32 overallPosition, BackgroundSpriteSelection expected) {
    s32 actual = (s32)Background_SelectSpriteSelection(courseIndex, venueType, inCourseEditTestRun,
                                                        overallPosition);

    if (actual == (s32)expected) {
        return 0;
    }
    printf("%s: expected %ld, got %ld\n", label, (long)(s32)expected, (long)actual);
    return 1;
}

int main(void) {
    int failures = 0;

    failures += expect_selection("ordinary index-0 race uses its course list", COURSE_MUTE_CITY, VENUE_MUTE_CITY,
                                 false, 0, BACKGROUND_SPRITE_SELECTION_COURSE);
    failures += expect_selection("index-0 hack with ending venue keeps its course list", COURSE_MUTE_CITY,
                                 VENUE_ENDING, false, 0, BACKGROUND_SPRITE_SELECTION_COURSE);
    failures += expect_selection("Course Edit ending venue test run uses ceremony list", COURSE_MUTE_CITY,
                                 VENUE_ENDING, true, 0, BACKGROUND_SPRITE_SELECTION_ENDING);
    failures += expect_selection("non-ending Course Edit test run suppresses sprites", COURSE_MUTE_CITY,
                                 VENUE_MUTE_CITY, true, 0, BACKGROUND_SPRITE_SELECTION_NONE);
    failures += expect_selection("ending test-run precedence covers X courses", COURSE_X_1, VENUE_ENDING, true, 0,
                                 BACKGROUND_SPRITE_SELECTION_ENDING);
    failures += expect_selection("non-ending X course keeps random selection", COURSE_X_1, VENUE_MUTE_CITY, false, 0,
                                 BACKGROUND_SPRITE_SELECTION_RANDOM);
    failures += expect_selection("actual ceremony top-three uses ceremony list", COURSE_ENDING, VENUE_ENDING, false,
                                 3, BACKGROUND_SPRITE_SELECTION_ENDING);
    failures += expect_selection("actual ceremony fourth place suppresses sprites", COURSE_ENDING, VENUE_ENDING,
                                 false, 4, BACKGROUND_SPRITE_SELECTION_NONE);
    failures += expect_selection("venue fallback remains available outside index-0 hacks", COURSE_EDIT_1,
                                 VENUE_ENDING, false, 0, BACKGROUND_SPRITE_SELECTION_ENDING);

    if (failures != 0) {
        printf("%d background skyline test(s) failed\n", failures);
        return 1;
    }
    printf("All background skyline tests passed\n");
    return 0;
}
