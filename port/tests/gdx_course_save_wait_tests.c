#include "leo/mfs.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define PRINTF(...) ((void)0)

static jmp_buf fatal;
static int injectedError;
static int attempts;
static int completions;
static int retries;
static int pauseDepth;
static int busyFirst;
OSMesgQueue gMFSMesgQ;
s32 gMfsError;
s32 sSLLeoError;
bool D_8076CBB0;

s32 osRecvMesg(OSMesgQueue* queue, OSMesg* message, s32 flags) {
    assert(queue == &gMFSMesgQ && flags == OS_MESG_NOBLOCK);
    return 0;
}
s32 osSendMesg(OSMesgQueue* queue, OSMesg message, s32 flags) {
    assert(queue == &gMFSMesgQ && flags == OS_MESG_BLOCK);
    ++completions;
    return 0;
}
void func_80704068(void) {
    ++pauseDepth;
}
void func_807040C8(void) {
    assert(pauseDepth > 0);
    --pauseDepth;
}
void func_80707B08(void) {
}
void SLMFSNewDisk(void) {
    assert(0 && "Refused saves must not initialize a disk");
}
void func_80704CE0(void) {
    ++retries;
}
void func_8070F8A4(s32 error, s32 mode) {
    fprintf(stderr, "Native save refusal entered fatal disk handler: %d, mode %d\n", error, mode);
    longjmp(fatal, 1);
}
s32 SLCheckDiskInsert(void) {
    assert(0);
    return 0;
}
s32 func_80707780(void) {
    assert(0);
    return 0;
}
void func_80703948(void) {
    assert(0);
}
void SLMFSRecoverManageArea(void) {
    assert(0);
}
void func_807038B0(void) {
    assert(0);
}
void func_80706518(s32 a, s32 b, char* c) {
    assert(0);
}
s32 Mfs_SaveFile(u16 dir, char* name, char* ext, u8* buf, u32 size, s32 attr, s32 copies, bool write) {
    ++attempts;
    gMfsError = busyFirst && attempts == 1 ? LEO_ERROR_BUSY : injectedError;
    return gMfsError ? -1 : 0;
}
s32 Mfs_DeleteFileInDir(u16 dir, char* name, char* ext, bool write) {
    ++attempts;
    gMfsError = busyFirst && attempts == 1 ? LEO_ERROR_BUSY : injectedError;
    return gMfsError ? -1 : 0;
}

#include "../../decomp/src/sys/disk/mfs_delete_wait.inc"
#include "../../decomp/src/sys/disk/mfs_save_wait.inc"
#include "../../decomp/src/overlays/course_edit/course_save_write.inc"

static void reset(int error) {
    injectedError = error;
    gMfsError = 0;
    attempts = completions = retries = pauseDepth = busyFirst = 0;
    D_8076CBB0 = false;
}

static struct { int unk_08; } D_800D6CA0;
static int D_80794E14;
static int D_80119880;
static int gCourseEditFileOption;
static struct { struct { int flag; } courseData; } context, D_xk2_800F7408;
static struct { char name[21]; } D_xk1_8003A598;
#define COURSE_CONTEXT() (&context)
static int cleanups;
static int notices;
static int unlocked;

static void func_xk2_800EBFE8(char* name) {
    ++cleanups;
}
static void func_8070405C(bool enabled) {
    unlocked += enabled;
}
static void gdx_course_edit_flash_message(const char* text) {
    assert(strstr(text, "not saved") && strstr(text, "log"));
    ++notices;
}
static void complete(void) {
    switch (D_800D6CA0.unk_08) {
#include "../../decomp/src/overlays/course_edit/course_save_complete.inc"
        default:
            break;
    }
}
static int editorRefusalTests(void) {
    for (int contextIndex = 0; contextIndex < 3; ++contextIndex) {
        for (int flag = 0; flag < 2; ++flag) {
            D_800D6CA0.unk_08 = 0x12;
            D_80119880 = contextIndex == 0 ? -1 : contextIndex == 1 ? 1 : 9;
            context.courseData.flag = D_xk2_800F7408.courseData.flag = flag;
            gMfsError = N64DD_ARGUMENT_ILLEGAL;
            cleanups = notices = unlocked = 0;
            D_80794E14 = 1;
            complete();
            assert(D_800D6CA0.unk_08 == 0x12 && !notices && !unlocked && !cleanups);
            D_80794E14 = 0;
            complete();
            assert(D_800D6CA0.unk_08 == 0 && !cleanups && notices == 1 && unlocked == 1);
            assert(gMfsError == N64DD_ARGUMENT_ILLEGAL && gCourseEditFileOption == -1);
            D_800D6CA0.unk_08 = 0x12;
            gMfsError = LEO_ERROR_GOOD;
            cleanups = notices = unlocked = 0;
            complete();
            assert(D_800D6CA0.unk_08 == (flag ? 0 : 0x22));
            assert(cleanups == !flag && !notices && !unlocked);
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "ui") == 0) {
        return editorRefusalTests();
    }
    u8 bytes[16] = { 0 };
    if (setjmp(fatal)) {
        return 1;
    }
    const int terminalErrors[] = { N64DD_AREA_LACKED, N64DD_NOT_FOUND, N64DD_DISK_DAMAGED,
        N64DD_ARGUMENT_ILLEGAL, N64DD_DISKID_ILLEGAL, N64DD_READ_ONLY_MEDIA, N64DD_MANAGER_NOT_CREATED };
    for (unsigned int i = 0; i < sizeof(terminalErrors) / sizeof(terminalErrors[0]); ++i) {
        reset(terminalErrors[i]);
        SLMFSSave(0, "TEST", "CRSD", bytes, sizeof(bytes), 0, 255, true);
        assert(attempts == 1 && completions == 1 && retries == 0 && pauseDepth == 0);
        assert(gMfsError == terminalErrors[i]);
        reset(terminalErrors[i]);
        SLMFSDeleteFile(0, "TEST", "CRSE", true);
        assert(attempts == 1 && completions == 0 && retries == 0 && pauseDepth == 0);
        assert(gMfsError == terminalErrors[i]);
    }
    reset(N64DD_ARGUMENT_ILLEGAL);
    func_xk2_800EB938(0, "TEST", "CRSD", bytes, sizeof(bytes), 0, 255, true);
    assert(attempts == 1 && completions == 0 && gMfsError == N64DD_ARGUMENT_ILLEGAL);
    reset(N64DD_ARGUMENT_ILLEGAL);
    SLMFSSave(0, "TEST", "CRSD", bytes, sizeof(bytes), 0, 255, true);
    assert(attempts == 1 && completions == 1 && retries == 0 && pauseDepth == 0);
    assert(gMfsError == N64DD_ARGUMENT_ILLEGAL);
    for (int write = 0; write < 2; ++write) {
        reset(N64DD_ARGUMENT_ILLEGAL);
        SLMFSDeleteFile(0, "TEST", "CRSE", write);
        assert(attempts == 1 && completions == 0 && retries == 0 && pauseDepth == 0);
        assert(gMfsError == N64DD_ARGUMENT_ILLEGAL);
        reset(LEO_ERROR_GOOD);
        SLMFSSave(0, "TEST", "CRSD", bytes, sizeof(bytes), 0, 255, write);
        assert(attempts == 1 && completions == 1 && gMfsError == 0);
        reset(LEO_ERROR_GOOD);
        busyFirst = true;
        SLMFSSave(0, "TEST", "CRSD", bytes, sizeof(bytes), 0, 255, write);
        assert(attempts == 2 && retries == 1 && completions == 1 && pauseDepth == 0);
        reset(N64DD_NOT_FOUND);
        SLMFSDeleteFile(0, "TEST", "CRSE", write);
        assert(attempts == 1 && completions == 0 && retries == 0);
    }
    assert(editorRefusalTests() == 0);
    puts("All native save refusal/wait tests passed");
    return 0;
}
