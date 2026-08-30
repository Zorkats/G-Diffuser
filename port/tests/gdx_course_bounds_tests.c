#include "../gdx_course_bounds.h"

#include <stdio.h>

static int sExtendedHeight;

int CVarGetInteger(const char* name, int defaultValue) {
    (void) name;
    (void) defaultValue;
    return sExtendedHeight;
}

static int expect_float(const char* label, float actual, float expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: got %.1f, expected %.1f\n", label, actual, expected);
        return 1;
    }
    printf("PASS: %s\n", label);
    return 0;
}

int main(void) {
    int failures = 0;

    failures += expect_float("retail-compatible floor", gdx_course_edit_min_y(), -250.0f);

    sExtendedHeight = 0;
    failures += expect_float("stock ceiling when disabled", gdx_course_edit_max_y(), 5000.0f);

    sExtendedHeight = 1;
    failures += expect_float("extended hard ceiling when enabled", gdx_course_edit_max_y(), 30000.0f);

    sExtendedHeight = -1;
    failures += expect_float("any nonzero value enables the ceiling", gdx_course_edit_max_y(), 30000.0f);

    if (failures != 0) {
        fprintf(stderr, "%d course-bound test(s) failed\n", failures);
        return 1;
    }
    printf("All course-bound tests passed\n");
    return 0;
}
