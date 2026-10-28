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

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BRANCH_EDITOR_H */
