#ifndef GDX_COURSE_EDITOR_LAYOUT_H
#define GDX_COURSE_EDITOR_LAYOUT_H

static inline int gdx_course_info_left(const unsigned char* title, int capacity) {
    int length = 0;
    int width;

    while (length < capacity && title[length] != 0) {
        length++;
    }
    width = length * 8 + 16;
    if (width < 128) {
        width = 128;
    }
    return 296 - width;
}

#endif
