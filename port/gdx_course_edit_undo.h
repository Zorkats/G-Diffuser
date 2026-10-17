// port/gdx_course_edit_undo.h -- Course Edit multi-level undo and redo.
//
// The decomp backup (func_xk2_800EF78C) and restore (func_xk2_800EF8B0) in course_edit/1A1240.c call into
// this module under PORT. A push stores the backup in a 32-entry history pool. A restore pops the newest
// entry, or steps onto a redo entry. Native and mouse code queue steps and row jumps through the request
// functions. gdx_course_edit_undo_tick applies at most one restore per call, so a long jump spreads over
// frames and each venue reload runs on its own frame.

#ifndef GDX_COURSE_EDIT_UNDO_H
#define GDX_COURSE_EDIT_UNDO_H

#ifdef __cplusplus
extern "C" {
#endif

// Snapshots kept across the undo and redo stacks together.
#define GDX_COURSE_EDIT_UNDO_CAPACITY 32

// Bytes per History row text, terminator included.
#define GDX_COURSE_EDIT_UNDO_TEXT_CAP 48

// Labels shown in the History panel. Set with gdx_course_edit_undo_set_pending_label before a backup.
typedef enum GdxCourseEditUndoLabel {
    GDX_COURSE_EDIT_UNDO_LABEL_EDIT = 0,
    GDX_COURSE_EDIT_UNDO_LABEL_SELECT,
    GDX_COURSE_EDIT_UNDO_LABEL_MOVE,
    GDX_COURSE_EDIT_UNDO_LABEL_DELETE,
    GDX_COURSE_EDIT_UNDO_LABEL_LINE_UP,
    GDX_COURSE_EDIT_UNDO_LABEL_SPLIT,
    GDX_COURSE_EDIT_UNDO_LABEL_OPTION,
    GDX_COURSE_EDIT_UNDO_LABEL_TEST_RUN,
    GDX_COURSE_EDIT_UNDO_LABEL_LOAD,
    GDX_COURSE_EDIT_UNDO_LABEL_COUNT
} GdxCourseEditUndoLabel;

// Decomp hooks, called from course_edit/1A1240.c under PORT. That file declares each one inline.
void gdx_course_edit_undo_push(void);
int gdx_course_edit_undo_begin_restore(void);
void gdx_course_edit_undo_end_restore(void);
int gdx_course_edit_undo_depth(void);
int gdx_course_edit_undo_play_restore_sound(void);
void gdx_course_edit_undo_clear(void);

// Labels and step requests. The last request before a tick wins.
// Row numbers match the History panel: undo rows 0..undo_count-1 (oldest first), then redo rows
// undo_count..undo_count+redo_count-1 (nearest first).
void gdx_course_edit_undo_set_pending_label(int label);
void gdx_course_edit_undo_request_undo(void);
void gdx_course_edit_undo_request_redo(void);
void gdx_course_edit_undo_request_row(int row);
void gdx_course_edit_undo_tick(void);

// Read-only accessors for the History panel.
int gdx_course_edit_undo_undo_count(void);
int gdx_course_edit_undo_redo_count(void);
int gdx_course_edit_undo_row_label(int row);
const char* gdx_course_edit_undo_label_text(int label);
// Row text is the label derived at push time from the overlay state, or the label text when none is derived.
// Row points are the control points in the state that the row restores. Both return "" or 0 for a bad row.
const char* gdx_course_edit_undo_row_text(int row);
int gdx_course_edit_undo_row_points(int row);

#ifdef __cplusplus
}
#endif

#endif
