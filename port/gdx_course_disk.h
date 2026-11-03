#ifndef GDX_COURSE_DISK_H
#define GDX_COURSE_DISK_H

// Include global.h and fzx_course.h before this header.
s32 gdx_course_from_disk(CourseData* out, const CourseDataDisk* disk);
s32 gdx_course_to_disk(CourseDataDisk* out, const CourseData* course);
s32 gdx_course_context_from_disk(CourseContext* out, const CourseContextDisk* disk);
s32 gdx_course_context_to_disk(CourseContextDisk* out, const CourseContext* course);
s32 gdx_course_context_file(const char* extension, u32 size);

#endif
