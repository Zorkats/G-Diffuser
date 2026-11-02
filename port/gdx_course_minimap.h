#ifndef GDX_COURSE_MINIMAP_H
#define GDX_COURSE_MINIMAP_H

#include "gdx_course_branch_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MINIMAP_DIMENSION 64
#define GDX_PREVIEW_STEPS 12
#define GDX_PREVIEW_CAPACITY ((GDX_BRANCH_MAX_TOTAL_POINTS + GDX_BRANCH_MAX_FORKS) * GDX_PREVIEW_STEPS + GDX_BRANCH_MAX_FORKS)

// A negative next index ends an open branch instead of connecting unrelated paths.
int gdx_course_preview_build(const float* x, const float* z, int count, const GdxBranchSet* forks,
                             int includeForks, float* outX, float* outZ, int* next, int capacity);
void gdx_course_minimap_plot(unsigned char* texture, float x, float z, float scale, int fill);

#ifdef __cplusplus
}
#endif
#endif
