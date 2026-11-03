#pragma once
#include "gdx_course_branch_edit.h"

#ifdef __cplusplus
extern "C" {
#endif
int gdx_course_data_checksum(void* courseData);
unsigned int gdx_course_snapshot_runtime_bytes(void);
// Capture returns count + 1 so an empty native course remains distinguishable from failure.
int gdx_course_snapshot_capture(const void* runtime, void* disk, GdxBranchEditPoint* tail);
int gdx_course_snapshot_expand(void* runtime, const void* disk, const GdxBranchEditPoint* tail, int pointCount,
                               unsigned int nativeChecksum);
#ifdef __cplusplus
}
#endif
