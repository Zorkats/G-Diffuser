#define GDX_IMPORT_VALIDATION_TEST
#include "../gdx_content_import.c"
#include <assert.h>

uint16_t gWorkingDirectory;
int32_t gMfsError;
int32_t gDirectoryEntryCount;
char gEditCupTrackNames[GDX_EDIT_CUP_ROWS][GDX_EDIT_CUP_NAME_SIZE];
uint8_t sMachineStatWeightings[3][5];
int gGdxDevGateCache[GDX_GATE_COUNT];
void (*gdx_port_log_tap)(const char* message);
static int busy, recovery, gameplay, queued, enqueueFailure;

int CVarGetInteger(const char* name, int32_t fallback) {
    return fallback;
}
float gdx_course_edit_max_y(void) {
    return 5000.0f;
}
int gdx_content_mfs_busy(void) {
    return busy;
}
int gdx_recovery_blocks_writes(void) {
    return recovery;
}
int gdx_input_in_gameplay(void) {
    return gameplay;
}
int gdx_content_count_extension_class(const char* prefix) {
    return 0;
}
int gdx_content_file_exists(const char* name, const char* extension) {
    return 0;
}
int32_t Mfs_ValidateFileName(char* name) {
    return strcmp(name, "TRACK") != 0;
}
int32_t Mfs_RamGetFreeSize(void) {
    return 1000000;
}
int32_t GdxContentImport_EnqueueDiskOp(void) {
    ++queued;
    return enqueueFailure;
}

enum { GDX_IMPORT_IDLE, GDX_IMPORT_PENDING, GDX_CUP_PENDING = 1 };
static int sImportState, sCupState, sImportSnapshot;
static int sImportRc, sImportMfsError, sImportCupCleared;
static uint8_t sImportPayload[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
static uint32_t sImportPayloadSize;
static char sImportName[21], sImportExtension[6], sImportTitle[GDX_COURSE_TITLE_MAX + 1];
static int32_t sImportContentType;
static GdxContentImportExtension sImportExtInfo;
static GdxCourseExtBlock sImportSidecarBlock;
static unsigned char sImportBranch[1];
static GdxCourseMeta sImportMeta[GDX_CONTENT_BUNDLE_MAX_TRACKS];

static void gdx_import_validate_machine(const uint8_t* payload, uint32_t* errors) {
    assert(!"machine validation is outside this track-only admission harness");
}

#include "../gdx_content_import_direct.inc"

int main(void) {
    uint8_t raw[GDX_CONTENT_TRACK_PAYLOAD_SIZE] = {0};
    GdxContentImportEntry row;
    int rc;
    // Three zero-valued points have the independent native checksum 3.
    raw[0] = 4;
    raw[1] = 3;
    raw[7] = 3;
    gdx_content_import_validate_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                               raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, &row);
    assert(row.errors == 0 && row.foreign.storedChecksum == 3);
    assert(raw[7] == 3 && queued == 0);
    rc = gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                  raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN);
    assert(rc == GDX_CONTENT_OK && queued == 1);
    assert(((GdxImportCourseData*)sImportPayload)->checksum == 3);
    assert(sImportPayloadSize == sizeof(raw) && sImportSnapshot == 0);
    sImportState = GDX_IMPORT_IDLE;
    raw[0] = 9;
    gdx_content_import_validate_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                               raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN, &row);
    assert(row.errors == GDX_IMPORT_ERR_TRACK_CREATOR && row.foreign.creator == 9);
    rc = gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                  raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN);
    assert(rc == GDX_CONTENT_IMPORT_ERR_VALIDATION && queued == 1);
    raw[0] = 4;
    raw[8] = 0x80;
    rc = gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                  raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN);
    assert(rc == GDX_CONTENT_IMPORT_ERR_VALIDATION && queued == 1);
    raw[8] = 0;
    raw[7] = 4;
    assert(gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                      raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN) ==
           GDX_CONTENT_IMPORT_ERR_VALIDATION);
    assert(queued == 1);
    raw[7] = 3;
    sImportState = GDX_IMPORT_PENDING;
    assert(gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                      raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN) ==
           GDX_CONTENT_IMPORT_ERR_PENDING);
    sImportState = GDX_IMPORT_IDLE;
    sCupState = GDX_CUP_PENDING;
    assert(gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                      raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN) ==
           GDX_CONTENT_IMPORT_ERR_PENDING);
    sCupState = 0;
    busy = 1;
    assert(gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK, raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN) == GDX_CONTENT_ERR_BUSY);
    busy = 0;
    recovery = 1;
    assert(gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK, raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN) == GDX_CONTENT_ERR_BUSY);
    recovery = 0;
    gameplay = 1;
    assert(gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK, raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN) == GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY);
    gameplay = 0;
    assert(queued == 1);
    enqueueFailure = 1;
    assert(gdx_content_import_begin_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                      raw, sizeof(raw), GDX_FOREIGN_BIG_ENDIAN) ==
           GDX_CONTENT_ERR_BUSY);
    assert(sImportState == GDX_IMPORT_IDLE && queued == 2);
    enqueueFailure = 0;
    gdx_content_import_validate_foreign_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                                 sImportPayload, sizeof(raw), GDX_FOREIGN_HOST_ORDER, &row);
    assert(row.errors == 0 && row.foreign.encoding == GDX_FOREIGN_HOST_ORDER);
    gdx_content_import_validate_payload("TRACK", "CRSD", GDX_CONTENT_TYPE_TRACK,
                                         sImportPayload, sizeof(raw), &row);
    assert(row.errors == 0 && !row.foreign.present);
    puts("gdx_foreign_import_tests: admission, normalized staging, and no-queue guards passed");
    return 0;
}
