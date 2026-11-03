#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gdx_course_branch_edit.h"

#define EDITOR_LABEL_CAP 96
static GdxBranchEditPoint sPoints[GDX_BRANCH_EDIT_MAX_POINTS];
static struct { int controlPointCount; } D_802CB6D0;
static struct { struct { int checksum; } courseData; } context;
#define COURSE_CONTEXT() (&context)
static int identityOk = 1;
static int commitResult;
static int flushResult;
static int commits;
static int flushes;
static int messages;

static void editor_export(void) {
}

static int gdx_course_state_edit_file_id(char* out, int capacity) {
    snprintf(out, capacity, "TEST.CRSD");
    return identityOk;
}

int gdx_branch_edit_commit(const char* id, const GdxBranchEditPoint* points, int count,
                           unsigned int checksum, char* reason, int capacity) {
    assert(strcmp(id, "TEST.CRSD") == 0 && points == sPoints);
    assert(count == 127 && checksum == 42);
    snprintf(reason, capacity, "fixture");
    ++commits;
    return commitResult;
}

static int gdx_course_state_flush(void) {
    ++flushes;
    return flushResult;
}

static void gdx_course_edit_flash_message(const char* text) {
    assert(text && *text);
    ++messages;
}

#include "../gdx_course_branch_save.inc"

int main(void) {
    D_802CB6D0.controlPointCount = 127;
    context.courseData.checksum = 42;
    flushResult = -1;
    assert(!gdx_course_branch_editor_saved());
    assert(commits == 1 && flushes == 1 && messages == 1);
    flushResult = 0;
    assert(gdx_course_branch_editor_saved());
    flushResult = 1;
    assert(gdx_course_branch_editor_saved());
    commitResult = -1;
    assert(!gdx_course_branch_editor_saved());
    assert(flushes == 3 && messages == 2);
    identityOk = 0;
    assert(!gdx_course_branch_editor_saved());
    assert(commits == 4 && flushes == 3);
    puts("All native editor save admission tests passed");
    return 0;
}
