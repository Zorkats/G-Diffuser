#ifndef GDX_COURSE_EDITOR_LAYOUT_H
#define GDX_COURSE_EDITOR_LAYOUT_H

// Course Edit info panel, in 320x240 screen pixels. The right edge is stock; the panel is as wide as
// its content and no wider: the title sits flush right, and the count labels sit left, just clear of
// their count fields.
#define GDX_COURSE_INFO_RIGHT 296
#define GDX_COURSE_INFO_PAD 8
#define GDX_COURSE_INFO_GLYPH 8
// Where the course-length row's "M" ends (drawn at 0x11F, 6 pixels wide), so the title ends on it.
#define GDX_COURSE_INFO_TITLE_RIGHT 293
// Space between a count label and the start of its space-padded count field.
#define GDX_COURSE_INFO_LABEL_GAP 2
// Start of the node counter's field on the Size row.
#define GDX_COURSE_INFO_SIZE_FIELD_X 240

static inline int gdx_course_info_text_width(const unsigned char* text, int capacity) {
    int length = 0;

    while (length < capacity && text[length] != 0) {
        length++;
    }
    return length * GDX_COURSE_INFO_GLYPH;
}

static inline int gdx_course_info_title_x(const unsigned char* title, int capacity) {
    return GDX_COURSE_INFO_TITLE_RIGHT - gdx_course_info_text_width(title, capacity);
}

// The rightmost the panel may start for `label`, drawn at the panel's text column, to end
// GDX_COURSE_INFO_LABEL_GAP before its count field at fieldX.
static inline int gdx_course_info_label_limit(const unsigned char* label, int capacity, int fieldX) {
    return fieldX - GDX_COURSE_INFO_LABEL_GAP - gdx_course_info_text_width(label, capacity) - GDX_COURSE_INFO_PAD;
}

// The panel's left edge: far enough left for the title and for every label row, whose tightest
// gdx_course_info_label_limit the caller passes in.
static inline int gdx_course_info_left(const unsigned char* title, int capacity, int labelLimit) {
    int left = gdx_course_info_title_x(title, capacity) - GDX_COURSE_INFO_PAD;

    return (labelLimit < left) ? labelLimit : left;
}

#endif
