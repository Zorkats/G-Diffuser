#include "global.h"
#include "fzx_course.h"
#include "leo/mfs.h"
#include "gdx_edit_cup.h"

extern int gdx_recovery_native_prepare(void);
extern int gdx_recovery_native_restore_ready(const char*, const char*, const u8*, size_t, int);
extern int gdx_recovery_native_read(const char*, const char*, u8*, size_t, size_t*);
extern int gdx_recovery_native_write(const char*, const char*, const u8*, size_t);
extern int gdx_recovery_native_cups(u8*, u8*);
extern int gdx_recovery_native_cup_slot(int, int, const u8*, size_t);

MfsRamArea gMfsRamArea;
u16 gWorkingDirectory = 7;
s32 gDirectoryEntryCount = 8;
s32 gMfsError;
OSThread* __osRunningThread;
char gEditCupTrackNames[24][21];
static u8 payloads[64][0xC830];
static u8 track[16 + 0xC830];
static u8 roundtrip[16 + 0xC830];
static MfsRamArea before;
static s32 allocation;
static s32 failSave;
static int snapshotPresent;
static s32 failDelete;
static s32 failBackup;
static s32 savedAttr;
static s32 prepared = 0;
static s32 freeBytes = 1000000;

s32 Mfs_RamGetFreeSize(void) {
    return freeBytes;
}

float gdx_course_edit_max_y(void) {
    return 5000.0f;
}

s32 func_i1_80404204(void) {
    return 0;
}

s32 func_i1_8040428C(void) {
    return 0;
}

s32 Mfs_GetFilesPreparation(u16 directory) {
    return prepared;
}

u16 Mfs_GetFileIndex(u16 directory, char* name, char* extension) {
    s32 i;
    for (i = 0; i < 8; ++i) {
        MfsRamDirectoryEntry* e = &gMfsRamArea.directoryEntry[i];
        if (e->attr && e->parentDirId == directory && strcmp(e->name, name) == 0 &&
            strcmp(e->extension, extension) == 0) {
            return (u16) i;
        }
    }
    return MFS_ENTRY_DOES_NOT_EXIST;
}

s32 Mfs_LoadFileInDir(u16 directory, char* name, char* extension, u8* out, s32 size) {
    u16 index = Mfs_GetFileIndex(directory, name, extension);
    if (index == MFS_ENTRY_DOES_NOT_EXIST || size != gMfsRamArea.directoryEntry[index].fileSize) {
        return -1;
    }
    memcpy(out, payloads[gMfsRamArea.directoryEntry[index].fileAllocationTableId], size);
    return 0;
}

s32 Mfs_BackupRamArea(void) {
    if (failBackup) {
        failBackup--;
        return -1;
    }
    return 0;
}

s32 Mfs_SaveFile(u16 directory, char* name, char* extension, u8* bytes, u32 size, s32 attr,
                 s32 copyCount, bool writeChanges) {
    s32 i;
    u16 index;
    MfsRamDirectoryEntry* e;
    savedAttr = attr;
    if (failSave == 1) {
        return -1;
    }
    index = Mfs_GetFileIndex(directory, name, extension);
    if (index == MFS_ENTRY_DOES_NOT_EXIST) {
        for (i = 0; i < 8; ++i) {
            if (!gMfsRamArea.directoryEntry[i].attr) {
                index = (u16) i;
                break;
            }
        }
    }
    if (index == MFS_ENTRY_DOES_NOT_EXIST || allocation >= 64 || size > 0xC830) {
        return -1;
    }
    e = &gMfsRamArea.directoryEntry[index];
    memset(e, 0, sizeof(*e));
    e->attr = (u16) (attr | MFS_FILE_ATTR_FILE);
    e->parentDirId = directory;
    strcpy(e->name, name);
    strcpy(e->extension, extension);
    e->fileSize = size;
    e->fileAllocationTableId = (u16) allocation++;
    e->copyCount = (u8) copyCount;
    memcpy(payloads[e->fileAllocationTableId], bytes, size);
    if (failSave == 2) {
        return -1;
    }
    if (failSave == 3) {
        payloads[e->fileAllocationTableId][0] ^= 0xFF;
    }
    return writeChanges ? Mfs_BackupRamArea() : 0;
}

s32 Mfs_DeleteFileInDir(u16 directory, char* name, char* extension, bool writeChanges) {
    u16 index = Mfs_GetFileIndex(directory, name, extension);
    if (index == MFS_ENTRY_DOES_NOT_EXIST) {
        return -1;
    }
    gMfsRamArea.directoryEntry[index].attr = 0;
    if (failDelete) {
        return -1;
    }
    return writeChanges ? Mfs_BackupRamArea() : 0;
}

int GdxRecoveryTest_Native(void) {
    u8 cent[216];
    u8 cenl[508];
    size_t size = 0;
    s32 failure;
    MfsRamDirectoryEntry* e;
    memset(track, 0, sizeof(track));
    track[0] = (MFS_FILE_ATTR_FILE | MFS_FILE_ATTR_ENCODE) & 0xFF;
    track[1] = (MFS_FILE_ATTR_FILE | MFS_FILE_ATTR_ENCODE) >> 8;
    track[8] = 42;
    track[9] = 19;
    track[10] = 17;
    track[11] = 91;
    track[16] = CREATOR_NINTENDO;
    track[16 + 0x7E0] = 77;
    {
        CourseDataDisk disk;
        memset(&disk, 0, sizeof(disk));
        disk.creatorId = CREATOR_NINTENDO;
        disk.controlPointCount = 1;
        disk.bankAngle[0] = 5;
        disk.dash[0] = 2;
        disk.checksum = 1u + 5u * 0x93DEu + 2u * 0x10u;
        memcpy(track + 16, &disk, sizeof(disk));
        freeBytes = 1000000;
        if (!gdx_recovery_native_restore_ready("TRACK", "CRSE", track, sizeof(track), 0)) {
            return 18;
        }
        memset(track + 16, 0, sizeof(disk));
        track[16] = CREATOR_NINTENDO;
    }
    track[16 + 8] |= 0x80;
    if (gdx_recovery_native_restore_ready("TRACK", "CRSE", track, sizeof(track), 0)) {
        return 19;
    }
    snapshotPresent = 1;
    if (!gdx_recovery_native_restore_ready("TRACK", "CRSE", track, sizeof(track), 0) ||
        gdx_recovery_native_restore_ready("OTHER", "CRSE", track, sizeof(track), 0) ||
        gdx_recovery_native_restore_ready("TRACK", "CRSD", track, sizeof(track), 0)) {
        return 20;
    }
    snapshotPresent = 0;
    track[16 + 8] &= ~0x80;
    freeBytes = 0;
    before = gMfsRamArea;
    if (gdx_recovery_native_restore_ready("TRACK", "CRSE", track, sizeof(track), 3) ||
        memcmp(&before, &gMfsRamArea, sizeof(before))) {
        return 15;
    }
    freeBytes = 1000000;
    if (!gdx_recovery_native_restore_ready("TRACK", "CRSE", track, sizeof(track), 3)) {
        return 16;
    }
    gDirectoryEntryCount = 3;
    if (gdx_recovery_native_restore_ready("TRACK", "CRSE", track, sizeof(track), 3)) {
        return 17;
    }
    gDirectoryEntryCount = 8;
    if (!gdx_recovery_native_prepare() || !gdx_recovery_native_write("TRACK", "CRSE", track, sizeof(track)) ||
        !(savedAttr & MFS_FILE_ATTR_ENCODE) ||
        !gdx_recovery_native_read("TRACK", "CRSE", roundtrip, sizeof(roundtrip), &size) ||
        size != sizeof(track) || memcmp(track, roundtrip, size)) {
        return 1;
    }
    e = &gMfsRamArea.directoryEntry[Mfs_GetFileIndex(7, "TRACK", "CRSE")];
    if (e->copyCount != 42 || e->renewalCounter != 19) {
        return 2;
    }
    failDelete = 1;
    before = gMfsRamArea;
    if (gdx_recovery_native_write("TRACK", "CRSE", NULL, 0) ||
        memcmp(&before, &gMfsRamArea, sizeof(before))) {
        return 3;
    }
    failDelete = 0;
    if (!gdx_recovery_native_write("TRACK", "CRSE", NULL, 0)) {
        return 4;
    }
    track[17] = 127;
    if (gdx_recovery_native_write("TRACK", "CRSE", track, sizeof(track))) {
        return 13;
    }
    track[17] = 0;
    track[20] = 1;
    if (gdx_recovery_native_write("TRACK", "CRSE", track, sizeof(track))) {
        return 14;
    }
    track[20] = 0;
    for (failure = 1; failure <= 3; ++failure) {
        before = gMfsRamArea;
        failSave = failure;
        if (gdx_recovery_native_write("TRACK", "CRSE", track, sizeof(track)) ||
            memcmp(&before, &gMfsRamArea, sizeof(before))) {
            return 5;
        }
    }
    failSave = 0;
    failBackup = 1;
    before = gMfsRamArea;
    if (gdx_recovery_native_write("TRACK", "CRSE", track, sizeof(track)) ||
        memcmp(&before, &gMfsRamArea, sizeof(before))) {
        return 6;
    }
    if (!gdx_recovery_native_write("TRACK", "CRSE", track, sizeof(track))) {
        return 7;
    }
    memset(cent, 0, sizeof(cent));
    memset(cenl, 0, sizeof(cenl));
    memcpy(cenl, "GCL1", 4);
    memcpy(cent, "OTHER", 6);
    memcpy(cenl + 4 + 23 * 21, "LONG TRACK NAME", 16);
    if (!gdx_recovery_native_cup_slot(0, 0, cent, 9) ||
        !gdx_recovery_native_cup_slot(1, 23, cenl + 4 + 23 * 21, 21)) {
        return 8;
    }
    for (failure = 1; failure <= 3; ++failure) {
        before = gMfsRamArea;
        failSave = failure;
        if (gdx_recovery_native_cup_slot(0, 0, NULL, 0) ||
            memcmp(&before, &gMfsRamArea, sizeof(before))) {
            return 9;
        }
    }
    failSave = 0;
    if (!gdx_recovery_native_cup_slot(1, 23, NULL, 0) ||
        Mfs_GetFileIndex(7, "CRS_ENTRY", "CENL") != MFS_ENTRY_DOES_NOT_EXIST ||
        !gdx_recovery_native_cups(cent, cenl) || strcmp((char*) cent, "OTHER") || cenl[4 + 23 * 21]) {
        return 10;
    }
    prepared = -1;
    gMfsError = N64DD_NOT_FOUND;
    if (gdx_recovery_native_prepare()) {
        return 11;
    }
    prepared = 0;
    failDelete = 1;
    failBackup = 2;
    if (gdx_recovery_native_write("TRACK", "CRSE", NULL, 0) || gdx_recovery_native_prepare()) {
        return 12;
    }
    return 0;
}

int gdx_snapshot_validate(unsigned short directory, const char* name, const char* extension, const void* base) {
    return snapshotPresent && directory == 7 && strcmp(name, "TRACK") == 0 && strcmp(extension, "CRSE") == 0;
}
