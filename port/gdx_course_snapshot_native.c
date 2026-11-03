#include "global.h"
#include "fzx_course.h"
#include "gdx_course_disk.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_point_validation.h"

unsigned int gdx_course_snapshot_runtime_bytes(void) {
    return sizeof(CourseContext);
}

static void snapshot_node(const CourseData* course, int index, GdxBranchEditPoint* node) {
    memcpy(node->pos, &course->controlPoint[index].pos, sizeof(node->pos));
    node->radiusLeft = course->controlPoint[index].radiusLeft;
    node->radiusRight = course->controlPoint[index].radiusRight;
    node->trackSegmentInfo = course->controlPoint[index].trackSegmentInfo;
    node->bankAngle = course->bankAngle[index];
    node->parts[0] = course->pit[index];
    node->parts[1] = course->dash[index];
    node->parts[2] = course->dirt[index];
    node->parts[3] = course->ice[index];
    node->parts[4] = course->jump[index];
    node->parts[5] = course->landmine[index];
    node->parts[6] = course->gate[index];
    node->parts[7] = course->building[index];
    node->parts[8] = course->sign[index];
}

static int snapshot_values_valid(const CourseData* course) {
    GdxBranchEditPoint node;
    int i;
    if (course->creatorId != CREATOR_NINTENDO || course->bgm > 13 ||
        course->venue < 0 || course->venue > 10 || course->skybox < 0 || course->skybox > 7) {
        return 0;
    }
    for (i = 0; i < course->controlPointCount; ++i) {
        snapshot_node(course, i, &node);
        if (node.radiusLeft < 0 || node.radiusRight < 0 || !gdx_course_style_valid(node.trackSegmentInfo) ||
            !gdx_course_parts_valid(node.parts, 9)) {
            return 0;
        }
    }
    return 1;
}

int gdx_course_snapshot_capture(const void* runtime, void* disk, GdxBranchEditPoint* tail) {
    const CourseContext* source = (const CourseContext*) runtime;
    CourseContextDisk* target = (CourseContextDisk*) disk;
    CourseData first;
    int count;
    int i;

    if (source == NULL || target == NULL || tail == NULL) {
        return 0;
    }
    count = source->courseData.controlPointCount;
    if (count < 0 || count > GDX_BRANCH_EDIT_MAX_POINTS || (source->courseData.flag & 0x80) ||
        !snapshot_values_valid(&source->courseData)) {
        return 0;
    }
    first = source->courseData;
    if ((u32) gdx_course_data_checksum(&first) != first.checksum) {
        return 0;
    }
    first = source->courseData;
    first.controlPointCount = count > 64 ? 64 : count;
    if (!gdx_course_to_disk(&target->courseData, &first)) {
        return 0;
    }
#ifdef EXPANSION_KIT
    memcpy(target->ghostSave, source->ghostSave, sizeof(target->ghostSave));
    target->saveCourseRecord = source->saveCourseRecord;
#endif
    for (i = 64; i < count; ++i) {
        snapshot_node(&source->courseData, i, &tail[i - 64]);
    }
    return count + 1;
}

int gdx_course_snapshot_expand(void* runtime, const void* disk, const GdxBranchEditPoint* tail, int pointCount,
                               unsigned int nativeChecksum) {
    // Disk-thread callers do not yield here; retained scratch avoids exhausting the native fiber stack.
    static CourseContext restored;
    static CourseData checked;
    const CourseContextDisk* base = (const CourseContextDisk*) disk;
    int i;
    int count;

    if (runtime == NULL || disk == NULL) {
        return 0;
    }
    if (!gdx_course_context_from_disk(&restored, base)) {
        return 0;
    }
    count = pointCount;
    if (count < 0 || count > GDX_BRANCH_EDIT_MAX_POINTS ||
        base->courseData.controlPointCount != (count > 64 ? 64 : count) ||
        (count > 64 && tail == NULL)) {
        return 0;
    }
    restored.courseData.flag &= 0x7F;
    restored.courseData.controlPointCount = count;
    restored.courseData.checksum = nativeChecksum;
    for (i = 64; i < count; ++i) {
        const GdxBranchEditPoint* node = &tail[i - 64];
        ControlPoint* point = &restored.courseData.controlPoint[i];
        memcpy(&point->pos, node->pos, sizeof(node->pos));
        point->radiusLeft = node->radiusLeft;
        point->radiusRight = node->radiusRight;
        point->trackSegmentInfo = node->trackSegmentInfo;
        restored.courseData.bankAngle[i] = node->bankAngle;
        restored.courseData.pit[i] = node->parts[0];
        restored.courseData.dash[i] = node->parts[1];
        restored.courseData.dirt[i] = node->parts[2];
        restored.courseData.ice[i] = node->parts[3];
        restored.courseData.jump[i] = node->parts[4];
        restored.courseData.landmine[i] = node->parts[5];
        restored.courseData.gate[i] = node->parts[6];
        restored.courseData.building[i] = node->parts[7];
        restored.courseData.sign[i] = node->parts[8];
    }
    checked = restored.courseData;
    if (!snapshot_values_valid(&checked) ||
        (u32) gdx_course_data_checksum(&checked) != nativeChecksum) {
        return 0;
    }
    memcpy(runtime, &restored, sizeof(restored));
    return 1;
}
