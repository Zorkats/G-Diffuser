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

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BRANCH_EDITOR_H */
