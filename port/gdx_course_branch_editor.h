/* port/gdx_course_branch_editor.h -- Course Edit hooks of the fork editor (gdx_course_branch_editor.c). */

#ifndef GDX_COURSE_BRANCH_EDITOR_H
#define GDX_COURSE_BRANCH_EDITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Ring changes, called after the editor ring changed. */
void gdx_course_branch_editor_on_insert(int newIndex);
void gdx_course_branch_editor_on_delete(int index);
void gdx_course_branch_editor_on_rotate(int newStart);
void gdx_course_branch_editor_on_reverse(void);
void gdx_course_branch_editor_on_translate(float dx, float dz);

void gdx_course_branch_editor_rebuilt(void);
void gdx_course_branch_editor_force_path_a(void);
void gdx_course_branch_editor_saved(void);
void gdx_course_branch_editor_loaded(void);
void gdx_course_branch_editor_new_course(void);
void gdx_course_branch_editor_tools(void);
void gdx_course_branch_editor_active_label(char* out, int cap);

/* Marks the editor overlay (port/gdx_course_edit_mouse.cpp) draws over the course, in world space: the
 * segment a first Split press picked, and for each fork its fork point, the nodes of the path not showing and
 * its merge point. Marks sharing a chain are joined by a line, in order. Returns how many were written. */
#define GDX_BRANCH_MARK_PICK 0
#define GDX_BRANCH_MARK_END 1
#define GDX_BRANCH_MARK_NODE 2

typedef struct GdxBranchMark {
    float pos[3];
    int kind;
    int chain;
} GdxBranchMark;

int gdx_course_branch_editor_marks(GdxBranchMark* out, int cap);

/* 0 when the course has no forks, else 1 while path A shows and 2 while path B shows. */
int gdx_course_branch_editor_path_showing(void);

/* The node of the path not showing under game-space (x, y), for tools that select a single point. Returns 1 and
 * fills fork and node, or 0 when there is none or a ring point is under the cursor too. */
int gdx_course_branch_editor_stored_node_at(float x, float y, int* fork, int* node);

/* Switches to the path holding that node and selects it. Runs on the game thread. */
void gdx_course_branch_editor_show_node(int fork, int node);

/* The settings panel (gdx_course_edit_parts_panel.cpp). The point being edited, or -1 when none is. */
int gdx_course_branch_editor_active_point(void);

#define GDX_BRANCH_EDITOR_PART_PIT (1 << 0) /* a pit or lava part, which has a width */
#define GDX_BRANCH_EDITOR_PART_MAGNET (1 << 1)
#define GDX_BRANCH_EDITOR_PART_DIRT (1 << 2)
#define GDX_BRANCH_EDITOR_PART_ICE (1 << 3)

/* GDX_BRANCH_EDITOR_PART_* of a point. */
int gdx_course_branch_editor_point_parts(int point);

/* Applies a change the panel queued (gdx_course_edit_input_request_extra). Runs on the game thread. */
void gdx_course_branch_editor_apply_extra(void);

/* Controller press at the cursor, before the stock pick: switches paths when a stored node is under the cursor,
 * so the stock pick then takes that node. */
void gdx_course_branch_editor_press_at_cursor(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BRANCH_EDITOR_H */
