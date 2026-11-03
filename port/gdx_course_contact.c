#include "global.h"
#include "gdx_course_contact.h"

s32 gdx_course_contact_project(RacerSegmentPositionInfo* out, const RacerSegmentPositionInfo* source,
                               CourseSegment* segment, s32 steps) {
    if (out == NULL || source == NULL || segment == NULL || steps < 2 || steps > 32 ||
        !(source->segmentTValue >= 0.0f && source->segmentTValue <= 1.0f)) {
        return -1;
    }
    *out = *source;
    out->courseSegment = segment;
    // Newton's first step must use the destination curve, not the previous lane's frame.
    Course_SplineGetPosition(segment, out->segmentTValue, &out->segmentPos);
    out->segmentForwardMagnitude = Course_SplineGetTangent(segment, out->segmentTValue, &out->segmentForward);
    if (!(out->segmentForwardMagnitude > 0.001f && out->segmentForwardMagnitude < 1000000.0f)) {
        return -1;
    }
    out->lastGroundedPos = out->segmentPos;
    return func_8009EBEC(out, source->pos.x, source->pos.y, source->pos.z, steps, -1.0f);
}
