#include "gdx_content_import.h"
#include "gdx_course_snapshot_io.h"
#include "gdx_edit_cup.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { GDX_IMPORT_DONE = 2 };
static uint16_t gWorkingDirectory = 7;
static int32_t gMfsError;
static char gEditCupTrackNames[GDX_EDIT_CUP_ROWS][GDX_EDIT_CUP_NAME_SIZE];
static int32_t sImportState, sImportRc, sImportMfsError, sImportCupCleared;
static char sImportName[21] = "TRACK", sImportExtension[5] = "CRSE";
static int32_t sImportContentType = GDX_CONTENT_TYPE_TRACK;
static uint32_t sImportPayloadSize = GDX_CONTENT_TRACK_PAYLOAD_SIZE;
static uint8_t sImportPayload[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
static int32_t sBundleTrackCount = 1, sBundleFailedIndex;
static char sBundleNames[GDX_CONTENT_BUNDLE_MAX_TRACKS][21] = { "TRACK" };
static uint8_t sBundleCent[GDX_EDIT_CUP_WIDE_SIZE];
static uint8_t sBundlePayloads[GDX_CONTENT_BUNDLE_MAX_TRACKS][GDX_CONTENT_TRACK_PAYLOAD_SIZE];
static int deleteError, centError, saveCalls, restores, ends;

int gdx_snapshot_begin(int writing) {
    return writing;
}
void gdx_snapshot_end(void) {
    ++ends;
}
static int gdx_import_prepare_snapshot(int index) {
    return 1;
}
int gdx_snapshot_import_check(unsigned short directory, const char* const* names,
                               const void* const* bases, int count) {
    return 1;
}
int gdx_snapshot_import_restore(unsigned short directory, const char* name, const char* extension,
                                 const void* base) {
    ++restores;
    return 1;
}
static int32_t Mfs_DeleteFileInDir(uint16_t directory, char* name, char* extension, int changes) {
    gMfsError = deleteError;
    return deleteError ? -1 : 0;
}
static int32_t Mfs_SaveFile(uint16_t directory, char* name, char* extension, uint8_t* bytes,
                            uint32_t size, int32_t attr, int32_t copies, int changes) {
    ++saveCalls;
    gMfsError = strcmp(extension, "CENT") == 0 ? centError : 0;
    return gMfsError ? -1 : 0;
}

#include "../gdx_content_import_disk.inc"

static void reset(void) {
    deleteError = centError = saveCalls = restores = ends = 0;
    memset(gEditCupTrackNames, 0, sizeof(gEditCupTrackNames));
    memcpy(gEditCupTrackNames[0], "TRACK", 6);
    sImportState = sImportRc = sImportMfsError = sImportCupCleared = 0;
}

int main(void) {
    reset();
    deleteError = 0xF1;
    GdxContentImport_RunOnDiskThread();
    assert(sImportRc != 0 && saveCalls == 0 && sImportMfsError == 0xF1 && ends == 1);
    reset();
    deleteError = 0xF2;
    GdxContentImport_RunOnDiskThread();
    assert(sImportRc == 0 && saveCalls == 2 && restores == 1 && ends == 1);
    reset();
    centError = 0xF1;
    GdxContentImport_RunOnDiskThread();
    assert(sImportRc != 0 && sImportMfsError == 0xF1 && restores == 1 && ends == 1);
    assert(strcmp(gEditCupTrackNames[0], "TRACK") == 0 && sImportCupCleared == 0);
    reset();
    deleteError = 0xF1;
    GdxContentBundle_RunOnDiskThread();
    assert(sImportRc != 0 && saveCalls == 0 && sBundleFailedIndex == 0 && ends == 1);
    reset();
    centError = 0xF1;
    GdxContentBundle_RunOnDiskThread();
    assert(sImportRc != 0 && sBundleFailedIndex == 1 && restores == 1 && ends == 1);
    assert(strcmp(gEditCupTrackNames[0], "TRACK") == 0);
    puts("All native import disk failure tests passed");
    return 0;
}
