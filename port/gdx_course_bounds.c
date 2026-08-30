/* port/gdx_course_bounds.c -- Course Edit control-point coordinate bounds.
 *
 * The Expansion Kit enforces control-point limits in two places that do not agree with each
 * other:
 *
 *   Save validator  Course_CalculateChecksum, EXPANSION_KIT branch (decomp/src/game/course.c):
 *                   x and z within +/-15000, y within -250 .. 5000.
 *   Course Edit     decomp/src/overlays/course_edit/1A1240.c and 188850.c: the same x and z
 *                   range, but y within 0 .. 5000.
 *
 * The editor's floor is the odd one out, and it is wrong in a way that is observable in retail
 * content: Sector Beta ships a control point at y = -201, which the save validator accepts and
 * the renderer draws, but which the stock editor refuses. A player who opens that course in
 * Course Edit cannot round-trip it -- the point gets pushed to 0 and the dip under the track is
 * silently lost.
 *
 * This module owns the bounds the port's editor applies, so the three clamp sites stay in
 * agreement with each other and with the validator. Widening the floor to the validator's own
 * limit cannot invalidate anything: every value newly accepted here was already accepted by the
 * checksum path and already appears in shipped courses.
 *
 * Design record: devdocs/1.2.0-import-and-limits-scope.md (2026-08-27, W4).
 */

#include "gdx_course_bounds.h"

extern int CVarGetInteger(const char* name, int defaultValue);

float gdx_course_edit_min_y(void) {
    return GDX_COURSE_VALIDATOR_MIN_Y;
}

float gdx_course_edit_max_y(void) {
    if (CVarGetInteger("gEnhancements.CourseEdit.ExtendedHeight", 0) != 0) {
        return GDX_COURSE_EXTENDED_MAX_Y;
    }
    return GDX_COURSE_VALIDATOR_MAX_Y;
}
