#ifndef GDX_COURSE_EDIT_INPUT_H
#define GDX_COURSE_EDIT_INPUT_H

#ifdef GLOBAL_H
typedef s32 GdxCourseEditInt32;
typedef u64 GdxCourseEditMask;
#else
#include <stdint.h>
typedef int32_t GdxCourseEditInt32;
typedef uint64_t GdxCourseEditMask;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxCourseEditVec3 {
    float x;
    float y;
    float z;
} GdxCourseEditVec3;

// Results of gdx_course_edit_native_line_up_scan. TOO_FEW: no run of 3 or more consecutive selected points.
// TOO_CLOSE: the line-up would put two neighbouring points closer than 498 units, or the run has no line.
#define GDX_COURSE_EDIT_LINE_UP_OK 0
#define GDX_COURSE_EDIT_LINE_UP_TOO_FEW 1
#define GDX_COURSE_EDIT_LINE_UP_TOO_CLOSE 2
#define GDX_COURSE_EDIT_LINE_UP_MAX_POINTS 64

// The run that the line-up would use, in ring order from start to end, and where each point of it would land.
typedef struct GdxCourseEditLineUpPreview {
    GdxCourseEditInt32 count;
    GdxCourseEditVec3 start;
    GdxCourseEditVec3 end;
    GdxCourseEditVec3 landing[GDX_COURSE_EDIT_LINE_UP_MAX_POINTS];
} GdxCourseEditLineUpPreview;

void gdx_course_edit_input_request_drag_start(GdxCourseEditInt32 point_index);
GdxCourseEditInt32 gdx_course_edit_input_take_drag_start(GdxCourseEditInt32* point_index);

void gdx_course_edit_input_request_drag_commit(void);
GdxCourseEditInt32 gdx_course_edit_input_take_drag_commit(void);

void gdx_course_edit_input_request_drag_cancel(void);
GdxCourseEditInt32 gdx_course_edit_input_take_drag_cancel(void);

void gdx_course_edit_input_request_delete(GdxCourseEditMask selection, GdxCourseEditInt32 count);
GdxCourseEditInt32 gdx_course_edit_input_take_delete(GdxCourseEditMask selection, GdxCourseEditInt32 count);
void gdx_course_edit_input_request_selection(GdxCourseEditInt32 point, GdxCourseEditInt32 toggle);
GdxCourseEditInt32 gdx_course_edit_input_take_selection(GdxCourseEditInt32* point, GdxCourseEditInt32* toggle);
void gdx_course_edit_input_request_selection_add_mask(GdxCourseEditMask mask);
GdxCourseEditInt32 gdx_course_edit_input_take_selection_add_mask(GdxCourseEditMask* mask);
GdxCourseEditInt32 gdx_course_edit_input_drag_start_pending(void);

GdxCourseEditInt32 gdx_course_edit_input_test_esc(GdxCourseEditInt32 down, GdxCourseEditInt32 active);
GdxCourseEditInt32 gdx_course_edit_input_take_test_esc(GdxCourseEditInt32 active);
void gdx_course_edit_input_clear_test_esc(void);

void gdx_course_edit_input_request_move_target(GdxCourseEditInt32 move_option, GdxCourseEditVec3 target);
GdxCourseEditInt32 gdx_course_edit_input_take_move_target(GdxCourseEditInt32* move_option,
                                                          GdxCourseEditVec3* target);

void gdx_course_edit_input_request_scalar_target(GdxCourseEditInt32 move_option,
                                                 GdxCourseEditInt32 target_offset);
GdxCourseEditInt32 gdx_course_edit_input_take_scalar_target(GdxCourseEditInt32* move_option,
                                                            GdxCourseEditInt32* target_offset);

void gdx_course_edit_input_request_line_up(void);
GdxCourseEditInt32 gdx_course_edit_input_take_line_up(void);

void gdx_course_edit_input_clear(void);
void gdx_course_edit_input_clear_drag(void);
void gdx_course_edit_native_update(void);
const void* gdx_course_edit_native_menu_id(void);

GdxCourseEditInt32 gdx_course_edit_native_context_state(void);
GdxCourseEditInt32 gdx_course_edit_native_grab_state(void);
GdxCourseEditInt32 gdx_course_edit_native_mouse_owned(void);
GdxCourseEditInt32 gdx_course_edit_native_help_active(void);
GdxCourseEditInt32 gdx_course_edit_native_active_point(void);
GdxCourseEditInt32 gdx_course_edit_native_point_count(void);
GdxCourseEditInt32 gdx_course_edit_native_point_selected(GdxCourseEditInt32 point_index);
GdxCourseEditInt32 gdx_course_edit_native_add_selection_mask(GdxCourseEditMask mask);
GdxCourseEditMask gdx_course_edit_native_selection_mask(void);
void gdx_course_edit_native_set_selection_mask(GdxCourseEditMask mask);
GdxCourseEditInt32 gdx_course_edit_native_point_position(GdxCourseEditInt32 point_index,
                                                          GdxCourseEditVec3* position);
GdxCourseEditInt32 gdx_course_edit_native_line_up_scan(GdxCourseEditLineUpPreview* preview);
void gdx_course_edit_native_apply_line_up(void);
void gdx_course_edit_native_play_refusal(void);

#ifdef __cplusplus
}
#endif

#endif
