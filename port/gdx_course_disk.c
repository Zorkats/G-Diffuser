#include "global.h"
#include "fzx_course.h"
#include "gdx_course_disk.h"

s32 gdx_course_from_disk(CourseData* out, const CourseDataDisk* disk) {
    if (out == NULL || disk == NULL || disk->controlPointCount < 0 || disk->controlPointCount > 64) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out, disk, 32);
    memcpy(out->controlPoint, disk->controlPoint, sizeof(disk->controlPoint));
    memcpy(out->bankAngle, disk->bankAngle, sizeof(disk->bankAngle));
    memcpy(out->pit, disk->pit, sizeof(disk->pit));
    memcpy(out->dash, disk->dash, sizeof(disk->dash));
    memcpy(out->dirt, disk->dirt, sizeof(disk->dirt));
    memcpy(out->ice, disk->ice, sizeof(disk->ice));
    memcpy(out->jump, disk->jump, sizeof(disk->jump));
    memcpy(out->landmine, disk->landmine, sizeof(disk->landmine));
    memcpy(out->gate, disk->gate, sizeof(disk->gate));
    memcpy(out->building, disk->building, sizeof(disk->building));
    memcpy(out->sign, disk->sign, sizeof(disk->sign));
    return 1;
}

s32 gdx_course_to_disk(CourseDataDisk* out, const CourseData* course) {
    if (out == NULL || course == NULL || course->controlPointCount < 0 || course->controlPointCount > 64) {
        return 0;
    }
    memcpy(out, course, 32);
    memcpy(out->controlPoint, course->controlPoint, sizeof(out->controlPoint));
    memcpy(out->bankAngle, course->bankAngle, sizeof(out->bankAngle));
    memcpy(out->pit, course->pit, sizeof(out->pit));
    memcpy(out->dash, course->dash, sizeof(out->dash));
    memcpy(out->dirt, course->dirt, sizeof(out->dirt));
    memcpy(out->ice, course->ice, sizeof(out->ice));
    memcpy(out->jump, course->jump, sizeof(out->jump));
    memcpy(out->landmine, course->landmine, sizeof(out->landmine));
    memcpy(out->gate, course->gate, sizeof(out->gate));
    memcpy(out->building, course->building, sizeof(out->building));
    memcpy(out->sign, course->sign, sizeof(out->sign));
    return 1;
}

s32 gdx_course_context_from_disk(CourseContext* out, const CourseContextDisk* disk) {
    if (out == NULL || disk == NULL || !gdx_course_from_disk(&out->courseData, &disk->courseData)) {
        return 0;
    }
#ifdef EXPANSION_KIT
    memcpy(out->ghostSave, disk->ghostSave, sizeof(disk->ghostSave));
    out->saveCourseRecord = disk->saveCourseRecord;
#endif
    return 1;
}

s32 gdx_course_context_to_disk(CourseContextDisk* out, const CourseContext* course) {
    if (out == NULL || course == NULL || !gdx_course_to_disk(&out->courseData, &course->courseData)) {
        return 0;
    }
#ifdef EXPANSION_KIT
    memcpy(out->ghostSave, course->ghostSave, sizeof(out->ghostSave));
    out->saveCourseRecord = course->saveCourseRecord;
#endif
    return 1;
}

s32 gdx_course_context_file(const char* extension, u32 size) {
    if (extension == NULL || size != sizeof(CourseContext)) {
        return 0;
    }
    return ((extension[0] == 'C' && extension[1] == 'R' && extension[2] == 'S' && (extension[3] == 'D' || extension[3] == 'E')) ||
            (extension[0] == 'G' && extension[1] == 'O' && extension[2] == 'S' && extension[3] == 'T')) &&
           extension[4] == 0;
}
