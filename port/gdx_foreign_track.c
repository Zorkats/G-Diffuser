// Copyright (c) Zorkats

#include "gdx_foreign_track.h"

#include <string.h>

typedef char gdx_foreign_size_check_point[(sizeof(GdxForeignControlPoint) == 0x14) ? 1 : -1];
typedef char gdx_foreign_size_check_course[(sizeof(GdxForeignCourseData) == GDX_FOREIGN_TRACK_COURSE_SIZE) ? 1 : -1];
typedef char gdx_foreign_size_check_bgm[(offsetof(GdxForeignCourseData, bgm) == 0x1F) ? 1 : -1];

// TRACK_JOIN_MASK | TRACK_FORM_MASK | TRACK_FLAG_CONTINUOUS (fzx_course.h:327,344,350): the bits
// Course_CalculateChecksum clears before summing.
#define GDX_FOREIGN_CHECKSUM_MASK_CLEAR (0x600 | 0x38000 | 0x40000000)

#define GDX_FOREIGN_PIT_MIN (-1)
#define GDX_FOREIGN_PIT_MAX 10  // PIT_MAX - 1; PIT_MAX is 11 in fzx_course.h

int gdx_foreign_track_float_invalid(const float* value) {
    uint32_t bits;
    int32_t exponent;

    memcpy(&bits, value, sizeof(bits));
    exponent = (int32_t) ((bits & 0x7F800000u) >> 0x17) - 0x7F;
    if (((-0x7F < exponent) && (exponent < 0x80)) || (bits == 0)) {
        return 0;
    }
    return 1;
}

uint32_t gdx_foreign_track_checksum(const GdxForeignCourseData* course, float maxY, int* geometryError) {
    int32_t i;
    uint32_t checksum = (uint32_t) course->controlPointCount;

    *geometryError = 0;
    for (i = 0; i < course->controlPointCount; i++) {
        const GdxForeignControlPoint* point = &course->controlPoint[i];
        int32_t trackSegmentInfo = point->trackSegmentInfo & ~GDX_FOREIGN_CHECKSUM_MASK_CLEAR;

        if (gdx_foreign_track_float_invalid(&point->pos.x) || point->pos.x < -15000.0f || point->pos.x > 15000.0f ||
            gdx_foreign_track_float_invalid(&point->pos.y) || point->pos.y < GDX_FOREIGN_TRACK_MIN_Y ||
            point->pos.y > maxY || gdx_foreign_track_float_invalid(&point->pos.z) || point->pos.z < -15000.0f ||
            point->pos.z > 15000.0f) {
            *geometryError = 1;
            return 0;
        }

        checksum += (uint32_t) (int32_t) ((point->pos.x + ((1.1f + (0.7f * i)) * point->pos.y)) +
                                          ((2.2f + (1.2f * i)) * point->pos.z * (4.4f + (0.9f * i))) +
                                          point->radiusLeft + ((5.5f + (0.8f * i)) * point->radiusRight * 4.8f)) +
                    (uint32_t) trackSegmentInfo * (uint32_t) (0xFE - i) +
                    (uint32_t) (course->bankAngle[i] * (0x93DE - i * 2));
    }

    for (i = 0; i < course->controlPointCount; i++) {
        checksum += (uint32_t) (course->pit[i] * i);
        checksum += (uint32_t) (course->dash[i] * (i + 0x10));
        checksum += (uint32_t) (course->dirt[i] * (i + 0x80));
        checksum += (uint32_t) (course->ice[i] * (i + 0x100));
        checksum += (uint32_t) (course->jump[i] * (i + 0x800));
        checksum += (uint32_t) (course->landmine[i] * (i + 0x1000));
        checksum += (uint32_t) (course->gate[i] * (i + 0x8000));
        checksum += (uint32_t) (course->building[i] * (i + 0x10000));
        checksum += (uint32_t) (course->sign[i] * (i + 0x80000));
    }

    return checksum;
}

int gdx_foreign_track_sanitize_pits(uint8_t* payload, float maxY, GdxForeignPitLog log, void* logContext) {
    GdxForeignCourseData course;
    int clamped = 0;
    int geometryError;
    int i;
    uint32_t checksum;

    memcpy(&course, payload, sizeof(course));
    for (i = 0; i < course.controlPointCount && i < 64; i++) {
        if (course.pit[i] < GDX_FOREIGN_PIT_MIN || course.pit[i] > GDX_FOREIGN_PIT_MAX) {
            if (log != NULL) {
                log(logContext, i, course.pit[i]);
            }
            course.pit[i] = -1;
            clamped++;
        }
    }

    if (clamped != 0) {
        checksum = gdx_foreign_track_checksum(&course, maxY, &geometryError);
        if (geometryError == 0) {
            course.checksum = checksum;
            memcpy(payload, &course, sizeof(course));
        }
    }

    return clamped;
}

static void gdx_foreign_field16(uint8_t* p) {
    uint16_t value = (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
    memcpy(p, &value, sizeof(value));
}

static void gdx_foreign_field32(uint8_t* p) {
    uint32_t value = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
    memcpy(p, &value, sizeof(value));
}

uint32_t gdx_foreign_track_normalize(const uint8_t* raw, int32_t size, int encoding, uint8_t* host, float maxY,
                                     GdxForeignCourseDiagnostic* diagnostic, GdxForeignPitLog log,
                                     void* logContext) {
    GdxForeignCourseDiagnostic info;
    GdxForeignCourseData course;
    uint32_t errors = 0;
    int invalid, i, j;

    memset(&info, 0, sizeof(info));
    if (diagnostic != NULL) {
        *diagnostic = info;
    }
    if (raw == NULL || host == NULL || size != GDX_FOREIGN_TRACK_PAYLOAD_SIZE) {
        return GDX_FOREIGN_TRACK_ERR_BAD_SIZE;
    }
    if (encoding != GDX_FOREIGN_HOST_ORDER && encoding != GDX_FOREIGN_BIG_ENDIAN) {
        return GDX_FOREIGN_TRACK_ERR_ENCODING;
    }
    info.present = 1;
    info.encoding = encoding;
    info.rawCreator = raw[0];
    info.rawCount = (int8_t) raw[1];
    memmove(host, raw, (size_t) size);
    if (encoding == GDX_FOREIGN_BIG_ENDIAN) {
        gdx_foreign_field32(host + 4);
        for (i = 0; i < 64; ++i) {
            uint8_t* point = host + 32 + i * 20;
            gdx_foreign_field32(point);
            gdx_foreign_field32(point + 4);
            gdx_foreign_field32(point + 8);
            gdx_foreign_field16(point + 12);
            gdx_foreign_field16(point + 14);
            gdx_foreign_field32(point + 16);
            gdx_foreign_field16(host + 0x520 + i * 2);
        }
        // Ghost and record numeric fields share the native disk layout; replay input and names are bytes.
        for (i = 0; i < 3; ++i) {
            uint8_t* ghost = host + 0x7E0 + i * 0x3FC0;
            gdx_foreign_field16(ghost);
            gdx_foreign_field16(ghost + 2);
            for (j = 4; j < 16; j += 4) {
                gdx_foreign_field32(ghost + j);
            }
            gdx_foreign_field16(ghost + 16);
            gdx_foreign_field16(ghost + 0x40);
            gdx_foreign_field16(ghost + 0x42);
            for (j = 0x44; j < 0x60; j += 4) {
                gdx_foreign_field32(ghost + j);
            }
        }
        gdx_foreign_field16(host + 0xC720);
        gdx_foreign_field16(host + 0xC722);
        for (i = 0xC724; i < 0xC754; i += 4) {
            gdx_foreign_field32(host + i);
        }
    }
    memcpy(&course, host, sizeof(course));
    info.creator = course.creatorId;
    info.count = course.controlPointCount;
    info.storedChecksum = course.checksum;
    if (course.flag & 0x80) {
        errors |= GDX_FOREIGN_TRACK_ERR_CHECKSUM;
    }
    if (course.controlPointCount < 0 || course.controlPointCount > 64) {
        errors |= GDX_FOREIGN_TRACK_ERR_GEOMETRY;
    } else {
        info.computedChecksum = gdx_foreign_track_checksum(&course, maxY, &invalid);
        info.checksumChecked = !invalid;
        if (invalid) {
            errors |= GDX_FOREIGN_TRACK_ERR_GEOMETRY;
        } else if (info.computedChecksum != course.checksum) {
            errors |= GDX_FOREIGN_TRACK_ERR_CHECKSUM;
        }
    }
    if (course.creatorId != GDX_FOREIGN_CREATOR_NINTENDO) {
        errors |= GDX_FOREIGN_TRACK_ERR_CREATOR;
    }
    if (course.bgm > GDX_FOREIGN_BGM_MAX) {
        errors |= GDX_FOREIGN_TRACK_ERR_BGM;
    }
    // Legacy pit repair remains allowed only after the original foreign checksum has passed.
    if (errors == 0) {
        info.sanitizedPits = gdx_foreign_track_sanitize_pits(host, maxY, log, logContext);
    }
    if (diagnostic != NULL) {
        *diagnostic = info;
    }
    return errors;
}
