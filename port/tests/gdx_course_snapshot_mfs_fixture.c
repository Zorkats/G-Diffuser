#include "global.h"
#include "fzx_course.h"
#include "leo/mfs.h"
#include "gdx_course_snapshot_io.h"
#include "gdx_course_snapshot_native.h"

extern void* GdxSnapshotTest_Create(int count);
extern int gdx_course_title_set(const char*, const char*);
extern int gdx_course_title_get(const char*, char*, int);
extern int GdxSnapshotRecoveryTest_Flush(const void* bytes, unsigned int size, int fail);
extern int GdxSnapshotRecoveryTest_Verify(const void* bytes, unsigned int size);
OSThread* __osRunningThread = (OSThread*) 1;
MfsRamArea gMfsRamArea;
s32 gDirectoryEntryCount = 8;
u16 gWorkingDirectory = 3;
s32 gMfsError;
static u8 payload[32][sizeof(CourseContextDisk)];
static u8 previous[sizeof(CourseContextDisk)];
static CourseContext loaded;
static int allocation;
static int failSave;
static int failRead;
static int failFlush;

void GdxSnapshotMfsTest_SetThread(void* thread) {
    __osRunningThread = thread;
}

u16 Mfs_GetParentDir(u16 directory) {
    return 0;
}

u16 Mfs_GetFileIndex(u16 directory, char* name, char* extension) {
    int i;
    for (i = 0; i < gDirectoryEntryCount; ++i) {
        MfsRamDirectoryEntry* e = &gMfsRamArea.directoryEntry[i];
        if (e->attr && e->parentDirId == directory && !strcmp(e->name, name) && !strcmp(e->extension, extension)) {
            return i;
        }
    }
    return MFS_ENTRY_DOES_NOT_EXIST;
}

s32 Mfs_LoadFileInDir(u16 directory, char* name, char* extension, u8* out, s32 size) {
    const u16 entry = Mfs_GetFileIndex(directory, name, extension);
    if (failRead || entry == MFS_ENTRY_DOES_NOT_EXIST || size != sizeof(CourseContextDisk)) {
        failRead = 0;
        return -1;
    }
    memcpy(out, payload[gMfsRamArea.directoryEntry[entry].fileAllocationTableId], size);
    return 0;
}

s32 Mfs_BackupRamArea(void) {
    return 0;
}

int gdx_disk_save_recovery_flush(void) {
    const int fail = failFlush > 0;
    if (fail) {
        --failFlush;
    }
    return GdxSnapshotRecoveryTest_Flush(&gMfsRamArea, sizeof(gMfsRamArea), fail);
}

static int save(unsigned short directory, char* name, char* extension, unsigned char* bytes, unsigned int size, int attr, int copies, int flush) {
    u16 entry = Mfs_GetFileIndex(directory, name, extension);
    MfsRamDirectoryEntry* e;
    if (size != sizeof(CourseContextDisk) || allocation >= 31) {
        return -1;
    }
    if (failSave == 1) {
        failSave = 0;
        return -1;
    }
    if (entry == MFS_ENTRY_DOES_NOT_EXIST) {
        entry = 0;
    }
    e = &gMfsRamArea.directoryEntry[entry];
    memset(e, 0, sizeof(*e));
    e->attr = MFS_FILE_ATTR_FILE;
    e->parentDirId = directory;
    strcpy(e->name, name);
    strcpy(e->extension, extension);
    e->fileSize = size;
    e->fileAllocationTableId = ++allocation;
    memcpy(payload[allocation], bytes, size);
    if (failSave == 2) {
        failSave = 0;
        return -1;
    }
    if (failSave == 3) {
        failSave = 0;
        failRead = 1;
    }
    return 0;
}

#define CHECK(c) do { if (!(c)) { return __LINE__; } } while (0)
int GdxSnapshotMfsTest_Run(void) {
    int failure;
    char title[21];
    GdxBranchEditPoint tail[1];
    CHECK(gdx_course_title_set("MFS.CRSD", "Old generation") == 0);
    CHECK(gdx_course_snapshot_capture(GdxSnapshotTest_Create(64), previous, tail) == 65);
    CHECK(save(3, "MFS", "CRSD", previous, sizeof(previous), 0, 0xFF, 1) == 0);
    CHECK(gdx_snapshot_native_backup(3, "MFS", "CRSD"));
    CHECK(gdx_snapshot_validate(3, "MFS", "CRSD", previous));
    CHECK(gdx_snapshot_native_save(save, 3, "MFS", "CRSD", GdxSnapshotTest_Create(64),
                                    sizeof(CourseContext), 0, 0xFF, 1) == 0);
    CHECK(Mfs_LoadFileInDir(3, "MFS", "CRSD", previous, sizeof(previous)) == 0);
    for (failure = 1; failure <= 4; ++failure) {
        CHECK(gdx_course_title_set("MFS.CRSD", "New generation") == 0);
        failSave = failure < 4 ? failure : 0;
        failFlush = failure == 4;
        CHECK(gdx_snapshot_native_save(save, 3, "MFS", "CRSD", GdxSnapshotTest_Create(127),
                                        sizeof(CourseContext), 0, 0xFF, 1) == -1);
        CHECK(!memcmp(payload[gMfsRamArea.directoryEntry[0].fileAllocationTableId], previous, sizeof(previous)));
        CHECK(gMfsError == N64DD_ARGUMENT_ILLEGAL);
        CHECK(GdxSnapshotRecoveryTest_Verify(&gMfsRamArea, sizeof(gMfsRamArea)));
        CHECK(!gdx_snapshot_busy());
        CHECK(gdx_snapshot_load(3, "MFS", "CRSD", previous, &loaded));
        CHECK(loaded.courseData.controlPointCount == 64);
        CHECK(gdx_course_title_get("MFS.CRSD", title, sizeof(title)));
        CHECK(!strcmp(title, "Old generation"));
    }
    CHECK(gdx_course_title_set("MFS.CRSD", "New generation") == 0);
    CHECK(gdx_snapshot_native_save(save, 3, "MFS", "CRSD", GdxSnapshotTest_Create(127),
                                    sizeof(CourseContext), 0, 0xFF, 1) == 0);
    CHECK(Mfs_LoadFileInDir(3, "MFS", "CRSD", previous, sizeof(previous)) == 0);
    CHECK(gdx_snapshot_load(3, "MFS", "CRSD", previous, &loaded));
    CHECK(loaded.courseData.controlPointCount == 127);
    CHECK(gdx_course_title_get("MFS.CRSD", title, sizeof(title)));
    CHECK(!strcmp(title, "New generation"));
    CHECK(gdx_snapshot_native_backup_entry(0));
    CHECK(!gdx_snapshot_native_backup_entry(8));
    CHECK(gdx_snapshot_native_save(save, 3, "NEW", "CRSD", GdxSnapshotTest_Create(6),
                                    sizeof(CourseContext), 0, 0xFF, 1) == 0);
    CHECK(gdx_snapshot_begin(1));
    CHECK(gdx_snapshot_prepare(3, "IMPORT", "CRSE", GdxSnapshotTest_Create(127), NULL, previous));
    CHECK(gdx_snapshot_native_save(save, 3, "IMPORT", "CRSE", previous,
                                    sizeof(previous), 0, 0xFF, 1) == 0);
    CHECK(gdx_snapshot_busy());
    gdx_snapshot_end();
    CHECK(!gdx_snapshot_busy());
    CHECK(gdx_snapshot_load(3, "IMPORT", "CRSE", previous, &loaded));
    CHECK(loaded.courseData.controlPointCount == 127);
    return 0;
}

int GdxSnapshotMfsTest_RollbackFailure(void) {
    failFlush = 2;
    CHECK(gdx_snapshot_native_save(save, 3, "IMPORT", "CRSE", GdxSnapshotTest_Create(127),
                                    sizeof(CourseContext), 0, 0xFF, 1) == -1);
    CHECK(failFlush == 0);
    CHECK(!gdx_snapshot_busy());
    CHECK(gMfsError == N64DD_ARGUMENT_ILLEGAL);
    CHECK(gdx_snapshot_native_save(save, 3, "IMPORT", "CRSE", GdxSnapshotTest_Create(127),
                                    sizeof(CourseContext), 0, 0xFF, 1) == -1);
    CHECK(!gdx_snapshot_busy());
    return 0;
}
