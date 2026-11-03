#ifndef GDX_COURSE_POINT_VALIDATION_H
#define GDX_COURSE_POINT_VALIDATION_H

// Plain types keep the native disk adapter and portable codecs on the same enum bounds.
static int gdx_course_style_valid(int info) {
    static const unsigned char counts[8] = {7, 3, 4, 4, 4, 4, 0, 3};
    unsigned int type = (unsigned int)info & 63u;
    unsigned int shape = ((unsigned int)info >> 6) & 7u;
    return type == 63u || type < counts[shape];
}

static int gdx_course_parts_valid(const signed char* parts, int count) {
    static const unsigned char limits[9] = {11, 3, 4, 4, 3, 3, 3, 15, 5};
    int i;
    for (i = 0; i < count; ++i) {
        if (parts[i] < -1 || parts[i] >= limits[i]) {
            return 0;
        }
    }
    return 1;
}

#endif
