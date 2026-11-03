#include "global.h"
#include "fzx_course.h"
#include "leo/mfs.h"
#include "gdx_course_disk.h"
#include "gdx_course_snapshot_io.h"

extern OSThread* __osRunningThread;
extern int gdx_disk_save_recovery_flush(void);
static u8 sBase[sizeof(CourseContextDisk) + BLK_SIZE_ZONE0];
static u8 sPrevious[sizeof(CourseContextDisk) + BLK_SIZE_ZONE0];
static u8 sVerified[sizeof(CourseContextDisk) + BLK_SIZE_ZONE0];
static MfsRamArea sBefore;
static s32 sHealthy = true;

void* gdx_snapshot_native_thread(void) {
    return __osRunningThread;
}

static u16 snapshot_directory(u16 directory) {
    if (directory == MFS_ENTRY_WORKING_DIR) {
        return gWorkingDirectory;
    }
    if (directory == MFS_ENTRY_WORKING_PARENT_DIR) {
        return Mfs_GetParentDir(MFS_ENTRY_WORKING_DIR);
    }
    return directory;
}

static int snapshot_extension(const char* extension) {
    return extension &&
        (((extension[0] == 'C') && extension[1] == 'R' && extension[2] == 'S' &&
          (extension[3] == 'D' || extension[3] == 'E')) ||
         (extension[0] == 'G' && extension[1] == 'O' && extension[2] == 'S' && extension[3] == 'T')) &&
        extension[4] == 0;
}

static int snapshot_read(u16 directory, const char* name, const char* extension, u8* out) {
    u16 entry = Mfs_GetFileIndex(directory, (char*) name, (char*) extension);
    if (entry == MFS_ENTRY_DOES_NOT_EXIST) {
        return 0;
    }
    if (gMfsRamArea.directoryEntry[entry].fileSize != sizeof(CourseContextDisk) ||
        Mfs_LoadFileInDir(directory, (char*) name, (char*) extension, out, sizeof(CourseContextDisk)) != 0) {
        return -1;
    }
    return 1;
}

int gdx_snapshot_native_save(GdxSnapshotSave save, u16 directory, char* name, char* extension, u8* bytes,
                              u32 size, int attr, int copies, int writeChanges) {
    int previous;
    int result = -1;
    const int runtime = gdx_course_context_file(extension, size);
    const int raw = snapshot_extension(extension) && size == sizeof(CourseContextDisk);
    if (!runtime && !raw) {
        return save(directory, name, extension, bytes, size, attr, copies, writeChanges);
    }
    if (!sHealthy || !gdx_snapshot_begin(true)) {
        gMfsError = N64DD_ARGUMENT_ILLEGAL;
        return -1;
    }
    directory = snapshot_directory(directory);
    previous = snapshot_read(directory, name, extension, sPrevious);
    memset(sBase, 0, sizeof(sBase));
    if (previous < 0 || (runtime && !gdx_snapshot_prepare(directory, name, extension, bytes,
                                                         previous ? sPrevious : NULL, sBase))) {
        goto done;
    }
    if (!runtime) {
        if ((previous && !gdx_snapshot_validate(directory, name, extension, sPrevious)) ||
            !gdx_snapshot_validate(directory, name, extension, bytes)) {
            goto done;
        }
        memcpy(sBase, bytes, sizeof(CourseContextDisk));
    }
    memcpy(&sBefore, &gMfsRamArea, sizeof(sBefore));
    result = save(directory, name, extension, sBase, sizeof(CourseContextDisk), attr, copies, writeChanges);
    if (result == 0 && snapshot_read(directory, name, extension, sVerified) == 1 &&
        memcmp(sBase, sVerified, sizeof(CourseContextDisk)) == 0 &&
        (!writeChanges || gdx_disk_save_recovery_flush())) {
        goto done;
    }
    // The stock overwrite path can hide a failed delete/rename. Keep its old FAT authoritative.
    memcpy(&gMfsRamArea, &sBefore, sizeof(sBefore));
    if (Mfs_BackupRamArea() < 0 || (writeChanges && !gdx_disk_save_recovery_flush())) {
        sHealthy = false;
    }
    result = -1;
done:
    gdx_snapshot_end();
    if (result != 0) {
        gMfsError = N64DD_ARGUMENT_ILLEGAL;
    }
    return result;
}

int gdx_snapshot_native_backup(u16 directory, const char* name, const char* extension) {
    int result;
    if (!snapshot_extension(extension)) {
        return 1;
    }
    if (!gdx_snapshot_begin(false)) {
        return 0;
    }
    directory = snapshot_directory(directory);
    result = snapshot_read(directory, name, extension, sVerified);
    result = result == 0 || (result == 1 && gdx_snapshot_validate(directory, name, extension, sVerified));
    gdx_snapshot_end();
    return result;
}

int gdx_snapshot_native_alias(u16 entry, const char* newName, const char* newExtension, u16 targetDirectory) {
    MfsRamDirectoryEntry source;
    char name[21];
    char extension[6];
    int result;
    if (entry >= gDirectoryEntryCount) {
        return 0;
    }
    source = gMfsRamArea.directoryEntry[entry];
    memcpy(extension, source.extension, 5);
    extension[5] = 0;
    if (!snapshot_extension(extension)) {
        return 1;
    }
    if (!snapshot_extension(newExtension) || !gdx_snapshot_begin(true)) {
        return 0;
    }
    memcpy(name, source.name, 20);
    name[20] = 0;
    result = snapshot_read(source.parentDirId, name, extension, sVerified) == 1 &&
        gdx_snapshot_alias(source.parentDirId, name, extension, sVerified,
                            snapshot_directory(targetDirectory), newName, newExtension);
    gdx_snapshot_end();
    return result;
}

int gdx_snapshot_native_backup_entry(u16 entry) {
    char name[21];
    char extension[6];
    if (entry >= gDirectoryEntryCount) {
        return 0;
    }
    memcpy(name, gMfsRamArea.directoryEntry[entry].name, 20);
    name[20] = 0;
    memcpy(extension, gMfsRamArea.directoryEntry[entry].extension, 5);
    extension[5] = 0;
    return gdx_snapshot_native_backup(gMfsRamArea.directoryEntry[entry].parentDirId, name, extension);
}
