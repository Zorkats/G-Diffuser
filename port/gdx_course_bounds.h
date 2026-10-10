/* port/gdx_course_bounds.h -- Course Edit control-point coordinate bounds.
 *
 * Rationale and provenance live in gdx_course_bounds.c. Decomp translation units declare these
 * entry points inline with `extern` rather than including this header, matching the convention
 * already used for the port hooks in decomp/src/overlays/course_edit/19DD60.c.
 */
#ifndef GDX_COURSE_BOUNDS_H
#define GDX_COURSE_BOUNDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The save validator's own limits (decomp/src/game/course.c, Course_CalculateChecksum's
 * EXPANSION_KIT branch). Stock Course Edit is stricter on the low end only. */
#define GDX_COURSE_VALIDATOR_MIN_Y (-250.0f)
#define GDX_COURSE_VALIDATOR_MAX_Y (5000.0f)
#define GDX_COURSE_EXTENDED_MAX_Y (30000.0f)
#define GDX_COURSE_VALIDATOR_MAX_XZ (15000.0f)

/* Lowest y a control point may occupy in Course Edit. */
float gdx_course_edit_min_y(void);

/* Highest y accepted by Course Edit, save validation, and content import. */
float gdx_course_edit_max_y(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BOUNDS_H */
