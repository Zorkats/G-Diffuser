#define GDX_IMPORT_VALIDATION_TEST
#include "../gdx_content_import.c"
#include <assert.h>

int gGdxDevGateCache[GDX_GATE_COUNT];
void (*gdx_port_log_tap)(const char* message);

float gdx_course_edit_max_y(void) {
    return 5000.0f;
}

static void foreign_course_regression(void) {
    unsigned char raw[GDX_CONTENT_TRACK_PAYLOAD_SIZE] = {0};
    GdxImportCourseData course = {0};
    int invalid;
    uint32_t errors = 0;
    GdxForeignCourseDiagnostic info;
    unsigned char host[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
    unsigned char before[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
    course.creatorId = 4;
    course.controlPointCount = 3;
    course.controlPoint[0].pos.x = 1000.0f;
    course.controlPoint[1].pos.z = 500.0f;
    course.controlPoint[2].pos.y = 200.0f;
    course.controlPoint[0].radiusLeft = 250;
    course.controlPoint[1].radiusRight = 350;
    course.bankAngle[2] = -20;
    course.checksum = gdx_import_course_checksum(&course, &invalid);
    assert(!invalid);
    memcpy(raw, &course, sizeof(course));
    // The fixture writes canonical console fields independently of the import decoder.
    {
        int i, j;
        for (i = 0; i < 64; ++i) {
            const int offsets[] = {32 + i * 20, 36 + i * 20, 40 + i * 20, 48 + i * 20};
            for (j = 0; j < 4; ++j) {
                unsigned char* p = raw + offsets[j];
                unsigned char a = p[0], b = p[1];
                p[0] = p[3]; p[1] = p[2]; p[2] = b; p[3] = a;
            }
            {
                const int shorts[] = {44 + i * 20, 46 + i * 20, 0x520 + i * 2};
                for (j = 0; j < 3; ++j) {
                    unsigned char* p = raw + shorts[j];
                    unsigned char value = p[0];
                    p[0] = p[1];
                    p[1] = value;
                }
            }
        }
        raw[4] = (unsigned char)(course.checksum >> 24);
        raw[5] = (unsigned char)(course.checksum >> 16);
        raw[6] = (unsigned char)(course.checksum >> 8);
        raw[7] = (unsigned char)course.checksum;
    }
    memcpy(before, raw, sizeof(raw));
    errors = gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, host, &info);
    assert(errors == 0);
    assert(info.creator == 4 && info.count == 3 && info.checksumChecked);
    assert(info.storedChecksum == course.checksum && info.computedChecksum == course.checksum);
    assert(memcmp(raw, before, sizeof(raw)) == 0);
    assert(memcmp(host, &course, sizeof(course)) == 0);
    errors = gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_HOST_ORDER, host, &info);
    assert(errors != 0);
    memcpy(raw, before, sizeof(raw));
    raw[32] = 0x7F; raw[33] = 0xC0; raw[34] = 0; raw[35] = 0;
    errors = gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, host, &info);
    assert(errors & GDX_IMPORT_ERR_TRACK_GEOMETRY);
    memcpy(raw, before, sizeof(raw));
    {
        int i;
        for (i = 0; i < (int) sizeof(raw); i += 4) {
            raw[i] = before[i + 3]; raw[i + 1] = before[i + 2];
            raw[i + 2] = before[i + 1]; raw[i + 3] = before[i];
        }
        assert(gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, host, &info) != 0);
    }
    memcpy(raw, before, sizeof(raw));
    raw[0] = 7;
    errors = gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, host, &info);
    assert(errors == GDX_IMPORT_ERR_TRACK_CREATOR && info.creator == 7);
    raw[0] = 4;
    raw[1] = 127;
    errors = gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, host, &info);
    assert(errors & GDX_IMPORT_ERR_TRACK_GEOMETRY);
    raw[1] = 3;
    raw[8] = 0x80;
    errors = gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, host, &info);
    assert(errors & GDX_IMPORT_ERR_TRACK_CHECKSUM);
    raw[8] = 0;
    raw[0x5A1] = 127;
    errors = gdx_content_import_normalize_track(raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, host, &info);
    assert(errors & GDX_IMPORT_ERR_TRACK_CHECKSUM);
    assert(host[0x5A1] == 127 && info.sanitizedPits == 0);
    assert(gdx_content_import_normalize_track(raw, sizeof(raw) - 1, GDX_FOREIGN_BIG_ENDIAN, host, &info) ==
           GDX_IMPORT_ERR_BAD_SIZE);
    assert(gdx_content_import_normalize_track(raw, sizeof(raw), 99, host, &info) == GDX_IMPORT_ERR_ENCODING);
}

static void foreign_context_fields(void) {
    unsigned char raw[GDX_CONTENT_TRACK_PAYLOAD_SIZE + 1];
    unsigned char host[GDX_CONTENT_TRACK_PAYLOAD_SIZE + 1];
    unsigned char* data = raw + 1;
    unsigned char* output = host + 1;
    GdxForeignCourseDiagnostic info;
    uint32_t value;
    int i;
    memset(raw, 0x5A, sizeof(raw));
    memset(data, 0, 0x7E0);
    data[0] = 4;
    for (i = 0; i < 3; ++i) {
        unsigned char* ghost = data + 0x7E0 + i * 0x3FC0;
        ghost[0] = 0x12; ghost[1] = 0x34;
        ghost[4] = 0x01; ghost[5] = 0x02; ghost[6] = 0x03; ghost[7] = 0x04;
        ghost[0x44] = 0; ghost[0x45] = 0; ghost[0x46] = 3; ghost[0x47] = 0xE8;
    }
    data[0xC750] = 0; data[0xC751] = 0; data[0xC752] = 3; data[0xC753] = 0xE8;
    memcpy(data + 0xC754, "ABCD", 4);
    assert(gdx_content_import_normalize_track(data, GDX_CONTENT_TRACK_PAYLOAD_SIZE,
                                               GDX_FOREIGN_BIG_ENDIAN, output, &info) == 0);
    for (i = 0; i < 3; ++i) {
        uint16_t small;
        unsigned char* ghost = output + 0x7E0 + i * 0x3FC0;
        memcpy(&small, ghost, 2);
        assert(small == 0x1234);
        memcpy(&value, ghost + 4, 4);
        assert(value == 0x01020304);
        memcpy(&value, ghost + 0x44, 4);
        assert(value == 1000);
        assert(memcmp(ghost + 0x20, data + 0x7E0 + i * 0x3FC0 + 0x20, 0x20) == 0);
        assert(memcmp(ghost + 0x60, data + 0x7E0 + i * 0x3FC0 + 0x60, 0x3F60) == 0);
    }
    memcpy(&value, output + 0xC750, 4);
    assert(value == 1000);
    assert(memcmp(output + 0xC754, data + 0xC754, 0xDC) == 0);
    assert(raw[0] == 0x5A);
    data[1] = 1;
    data[7] = 1;
    data[0x5A0] = 127;
    assert(gdx_content_import_normalize_track(data, GDX_CONTENT_TRACK_PAYLOAD_SIZE,
                                               GDX_FOREIGN_BIG_ENDIAN, output, &info) == 0);
    assert(info.sanitizedPits == 1 && output[0x5A0] == 255 && data[0x5A0] == 127);

}

int main(void) {
    foreign_course_regression();
    foreign_context_fields();
    unsigned char bytes[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
    unsigned char before[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
    GdxImportCourseData* course = (GdxImportCourseData*) bytes;
    uint32_t errors;
    int count;

    for (count = -128; count <= 127; count++) {
        if (count >= 0 && count <= 64) {
            continue;
        }
        memset(bytes, 0x3F, sizeof(bytes));
        course->controlPointCount = (int8_t) count;
        memcpy(before, bytes, sizeof(bytes));
        errors = 0;
        gdx_import_validate_track(bytes, &errors);
        assert(errors & GDX_IMPORT_ERR_TRACK_GEOMETRY);
        assert(memcmp(before, bytes, sizeof(bytes)) == 0);
    }
    for (count = 0; count <= 64; count++) {
        int i;
        int geometryError;
        uint32_t expected;

        memset(bytes, 0, sizeof(bytes));
        course->controlPointCount = (int8_t) count;
        course->creatorId = GDX_IMPORT_CREATOR_NINTENDO;
        memset(course->pit, -1, sizeof(course->pit));
        if (count) {
            course->pit[count - 1] = 127;
        }
        errors = 0;
        gdx_import_validate_track(bytes, &errors);
        for (i = 0; i < count; i++) {
            assert(course->pit[i] == -1);
        }
        expected = gdx_import_course_checksum(course, &geometryError);
        assert(!geometryError);
        if (count) {
            assert(course->checksum == expected);
            assert(errors == 0);
        }
    }
    memset(bytes, 0, sizeof(bytes));
    course->creatorId = GDX_IMPORT_CREATOR_NINTENDO;
    course->flag = 0x80;
    memcpy(before, bytes, sizeof(bytes));
    errors = 0;
    gdx_import_validate_track(bytes, &errors);
    assert(errors & GDX_IMPORT_ERR_TRACK_CHECKSUM);
    assert(memcmp(before, bytes, sizeof(bytes)) == 0);
    puts("All imported-count and pit sanitization tests passed");
    return 0;
}
