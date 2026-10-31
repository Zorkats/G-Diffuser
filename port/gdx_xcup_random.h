/* port/gdx_xcup_random.h -- the X Cup random course toggle and the saved copy of a generated course.
 *
 * Toggle: while it is on, a Grand Prix X Cup slot loads a real course picked at random instead of
 * generating one. The slot keeps its number. CPU racers and background sprites read their course
 * tables through gdx_xcup_real_course, so they use the real course's data.
 *
 * Deck: the picks of the current cup, one per X Cup course, all different. A re-do of a course takes
 * its slot's pick again. The deck is not saved, so a cup resumed with Continue Cup starts with an
 * empty deck and can repeat a course it already raced.
 *
 * Generated course: a copy of the last course the Grand Prix generated, as a CourseContext image
 * with no ghosts and no records. The menu saves it as a Course Edit track.
 */

#ifndef GDX_XCUP_RANDOM_H
#define GDX_XCUP_RANDOM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_XCUP_RANDOM_CVAR "gEnhancements.Gameplay.XCupRandomCourses"

/* Courses 0 to 23 are the stock cups. Courses 30 to 41 are the two Expansion Kit cups. */
#define GDX_XCUP_RANDOM_STOCK_COUNT 24
#define GDX_XCUP_RANDOM_DD_FIRST 30
#define GDX_XCUP_RANDOM_DD_COUNT 12

/* Nonzero while the toggle is on. */
int gdx_xcup_random_enabled(void);

/* Decomp hook: picks the real course for an X Cup slot (0 to 5) and makes it the active one. newCup
 * empties the deck first. roll chooses among the courses the deck has not used, and is ignored when
 * the slot already has a pick. ddAvailable adds the Expansion Kit courses to the pool. Returns the
 * course index, or -1 for a slot out of range (nothing changes). Drops the generated course copy. */
int gdx_xcup_random_pick(int slot, int newCup, uint32_t roll, int ddAvailable);

/* Decomp hook: the current course was generated, so no real course is active. */
void gdx_xcup_random_clear_active(void);

/* The real course an X Cup slot is racing, or -1 when none is active. */
int gdx_xcup_random_active_course(void);

/* The course whose tables apply to courseIndex: the active real course on an X Cup slot, otherwise
 * courseIndex itself. */
int gdx_xcup_real_course(int courseIndex);

/* The same for the cup: the active real course's cup on an X Cup slot, otherwise cupType. */
int gdx_xcup_real_cup(int cupType, int courseIndex);

/* Decomp hook: copies a generated CourseData into the saved image. Refused when it is larger than
 * the image. */
void gdx_xcup_generated_capture(const void* courseData, size_t size);

/* The saved image, GDX_CONTENT_TRACK_PAYLOAD_SIZE bytes, or NULL when there is none. */
const uint8_t* gdx_xcup_generated_payload(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_XCUP_RANDOM_H */
