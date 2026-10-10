#ifndef GDX_COURSE_EDIT_MENU_H
#define GDX_COURSE_EDIT_MENU_H

static inline int gdx_course_edit_list_scroll(int offset, int count, int spacing, int steps, int visible) {
    int maxRows = count > visible ? count - visible : 0;
    int row;
    if (spacing <= 0) {
        return 0;
    }
    row = offset / spacing + steps;
    if (row < 0) {
        row = 0;
    } else if (row > maxRows) {
        row = maxRows;
    }
    return row * spacing;
}

static inline int gdx_course_edit_menu_scroll(int offset, int count, int spacing, int steps) {
    return gdx_course_edit_list_scroll(offset, count, spacing, steps, 10);
}

static inline int gdx_course_edit_menu_visible_row(int row, int count, int spacing, int offset) {
    int first = spacing > 0 ? offset / spacing : 0;
    return row >= 0 && row < count && (count <= 10 || spacing <= 0 || (row >= first && row < first + 10));
}

static inline int gdx_course_edit_menu_arrow_hit(int left, int top, int spacing, int count,
                                                int offset, int x, int y) {
    return count > 10 && spacing > 0 && offset < (count - 10) * spacing &&
        x >= left + 8 && x < left + 40 && y >= top + 10 * spacing && y < top + 10 * spacing + 16;
}

#endif
