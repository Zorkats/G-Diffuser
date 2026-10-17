// port/gdx_course_edit_undo.c -- Course Edit multi-level undo and redo.
//
// The decomp backup (func_xk2_800EF78C) copies one whole course snapshot into the backup globals, and the
// restore (func_xk2_800EF8B0) copies them back. A snapshot holds the selection mask, the file name, the
// segment table, the course data, and the active point. This file keeps up to
// GDX_COURSE_EDIT_UNDO_CAPACITY snapshots in one pool, indexed by an undo stack and a redo stack.
//
// A step swaps a pool slot with the live course through the backup globals. The slot moves to the other
// stack and keeps its label. The restore body then copies the backup into the live course.
//
// Entry kinds decide what a restore does with the entry it pops:
//   USER  An ordinary edit. Restoring it moves the live state onto the redo stack.
//   TEST  The state saved when a test drive starts. The test-run exit restores it, and it never goes to redo.
//   LOAD  The state saved before a course load. A checksum rollback while the load confirm is up
//         (unk_08 == 0x13) pops it without redo. Otherwise it acts as USER.
// A push made during a test run is ignored while a TEST entry is on top, so the pre-test state is kept.
// A user push clears the redo stack. A TEST push leaves the redo stack alone.

#include "global.h"
#include "fzx_expansion_kit.h"
#include "fzx_course.h"
#include "gdx_course_edit_input.h"
#include "gdx_course_edit_undo.h"

extern unk_800D6CA0 D_800D6CA0;
extern unk_80128690 D_80128690[64];
extern unk_80128690 D_xk2_80128990[64];
extern unk_807B3C20 D_802CB6D0;
extern unk_807B3C20 D_807B6528;
extern u8 D_80030060[GDX_EK_FILE_NAME_MAX + 1];
extern u8 D_8003006C[GDX_EK_FILE_NAME_MAX + 1];
extern CourseData D_8010CF50;
extern s32 D_xk2_80119800;
extern s32 D_xk2_80104BC0;
extern s32 gCreateOption;
extern s32 gMoveOption;
extern s32 gPointOption;
extern s32 gDesignStyleOption;
extern s32 gPartsStyleOption;
extern bool gInCourseEditTestRun;
extern void func_xk2_800EF8B0(void);
// The decomp libc/string.h declares memcpy but not memcmp, and the host <string.h> clashes with it.
extern int memcmp(const void* s1, const void* s2, size_t n);

#define GDX_CE_UNDO_KIND_USER 0
#define GDX_CE_UNDO_KIND_TEST 1
#define GDX_CE_UNDO_KIND_LOAD 2

#define GDX_CE_UNDO_REQUEST_NONE 0
#define GDX_CE_UNDO_REQUEST_UNDO 1
#define GDX_CE_UNDO_REQUEST_REDO 2
#define GDX_CE_UNDO_REQUEST_ROW 3

// unk_08 while the load confirm is up (course_edit/19DD60.c, case 0x13).
#define GDX_CE_UNDO_LOAD_CONFIRM_STATE 0x13
// unk_08 values that refuse a step from a request. These match the B path in course_edit/188850.c.
#define GDX_CE_UNDO_REFUSE_STATE_A 1
#define GDX_CE_UNDO_REFUSE_STATE_CLEAR 0x11

// Offset of controlPointCount (s32) in the backup segment table, unk_807B3C20.
#define GDX_CE_UNDO_POINT_COUNT_OFFSET 0x2900

typedef struct GdxCourseEditUndoSnapshot {
    unsigned char selection[sizeof(unk_80128690) * 64];
    unsigned char fileName[GDX_EK_FILE_NAME_MAX + 1];
    unsigned char segments[sizeof(unk_807B3C20)];
    unsigned char courseData[sizeof(CourseData)];
    unsigned char activePoint[sizeof(s32)];
} GdxCourseEditUndoSnapshot;

typedef struct GdxCourseEditUndoSlot {
    GdxCourseEditUndoSnapshot snapshot;
    unsigned char kind;
    unsigned char label;
    // Control points in the snapshot, shown in the History row tooltip.
    int points;
    // Row text: the derived label, or the label text when nothing is derived.
    char text[GDX_COURSE_EDIT_UNDO_TEXT_CAP];
} GdxCourseEditUndoSlot;

// Every field is sized from the host struct it copies, so the pool follows the host layout. On x64 a
// snapshot is a little over 13 KB and the pool about 0.5 MB.
static GdxCourseEditUndoSlot gSlots[GDX_COURSE_EDIT_UNDO_CAPACITY];

static int gSlotUsed[GDX_COURSE_EDIT_UNDO_CAPACITY];
// Undo stack. Index 0 is the oldest entry and index gUndoCount - 1 is the newest.
static int gUndoSlots[GDX_COURSE_EDIT_UNDO_CAPACITY];
static int gUndoCount;
// Redo stack. Index gRedoCount - 1 is the nearest entry and index 0 is the farthest.
static int gRedoSlots[GDX_COURSE_EDIT_UNDO_CAPACITY];
static int gRedoCount;

static int gPendingLabel = -1;
// Set by begin_restore and cleared by end_restore. Pushes made while it is set are ignored.
static int gRestoreActive;
// Set by step_once around the restore call, so begin_restore knows which stack to step.
static int gRestoreIsRedo;
// Set by begin_undo and begin_redo when a swap really happened.
static int gStepStarted;
// Set while a jump step runs. A restore that arrives without it (the B path) cancels any pending jump.
static int gJumpStepping;
// Undo stack count that a jump is heading for, or -1 when no jump is pending.
static int gJumpTarget = -1;
// Set for every jump step except the last, so only the final step plays the restore sound.
static int gSuppressSound;

static int gRequest = GDX_CE_UNDO_REQUEST_NONE;
static int gRequestRow;

static int current_kind(void) {
    if (gInCourseEditTestRun) {
        return GDX_CE_UNDO_KIND_TEST;
    }
    if (D_800D6CA0.unk_08 == GDX_CE_UNDO_LOAD_CONFIRM_STATE) {
        return GDX_CE_UNDO_KIND_LOAD;
    }
    return GDX_CE_UNDO_KIND_USER;
}

static int label_for_kind(int kind) {
    if (kind == GDX_CE_UNDO_KIND_TEST) {
        return GDX_COURSE_EDIT_UNDO_LABEL_TEST_RUN;
    }
    if (kind == GDX_CE_UNDO_KIND_LOAD) {
        return GDX_COURSE_EDIT_UNDO_LABEL_LOAD;
    }
    if (gPendingLabel >= 0) {
        return gPendingLabel;
    }
    return GDX_COURSE_EDIT_UNDO_LABEL_EDIT;
}

static int snapshot_point_count(const GdxCourseEditUndoSnapshot* snap) {
    int count = 0;

    memcpy(&count, snap->segments + GDX_CE_UNDO_POINT_COUNT_OFFSET, sizeof(count));
    return count;
}

static int snapshot_active_point(const GdxCourseEditUndoSnapshot* snap) {
    int point = 0;

    memcpy(&point, snap->activePoint, sizeof(point));
    return point;
}

// Text helpers. Output stops one byte short of GDX_COURSE_EDIT_UNDO_TEXT_CAP and always ends with a terminator.
static void text_put_char(char* text, int* pos, char c) {
    if (*pos < GDX_COURSE_EDIT_UNDO_TEXT_CAP - 1) {
        text[*pos] = c;
        (*pos)++;
    }
    text[*pos] = '\0';
}

static void text_put(char* text, int* pos, const char* src) {
    while (*src != '\0') {
        text_put_char(text, pos, *src);
        src++;
    }
    text[*pos] = '\0';
}

// Writes a non-negative decimal value.
static void text_put_index(char* text, int* pos, int value) {
    char digits[12];
    int n = 0;

    do {
        digits[n] = (char) ('0' + (value % 10));
        n++;
        value /= 10;
    } while (value > 0 && n < 12);
    while (n > 0) {
        n--;
        text_put_char(text, pos, digits[n]);
    }
}

// Move tool names by gMoveOption. Entries 0 to 4 name the active point, and the tail follows it.
static const char* const kMoveLabelHead[] = {
    "Move point", "Move point", "Width point", "Bank point", "Center point", "Delete points", "Line up points",
};
static const char* const kMoveLabelTail[] = {
    " (x and z)", " (height)", "", "", "", "", "",
};
// Point menu names by gPointOption, used under CREATE_OPTION_COURSE.
static const char* const kPointLabelName[] = {
    "Add point", "Move start", "Center course", "Reverse course", "Clear course",
};
static const char* const kDesignLabelName[] = {
    "Road", "High rails", "No rails", "Tunnel", "Pipe", "Half pipe", "Cylinder", "Gap", "Loop", "Split",
};
static const char* const kPartsLabelName[] = {
    "Energy patch", "Dash plate", "Dirt patch", "Ice patch", "Jump plate", "Trap", "Gate", "Building", "Sign",
};

// Derives a History label from the state at push time. The backup holds the state before the edit, and the option
// globals still name the tool that made it. Returns 1 when text was written, or 0 when the caller should use the
// label text instead.
static int derive_context_text(char* text, const GdxCourseEditUndoSnapshot* snap) {
    int pos = 0;
    int point = snapshot_active_point(snap);
    int hasPoint = (point >= 0 && point < snapshot_point_count(snap));
    int moveOption = gMoveOption;
    int designOption = gDesignStyleOption;
    int partsOption = gPartsStyleOption;

    text[0] = '\0';
    if (gCreateOption == CREATE_OPTION_COURSE) {
        if (gPointOption < 0 || gPointOption >= (int) (sizeof(kPointLabelName) / sizeof(kPointLabelName[0]))) {
            return 0;
        }
        text_put(text, &pos, kPointLabelName[gPointOption]);
        return 1;
    }
    if (gCreateOption == CREATE_OPTION_POINT) {
        if (moveOption < 0 || moveOption >= (int) (sizeof(kMoveLabelHead) / sizeof(kMoveLabelHead[0]))) {
            return 0;
        }
        text_put(text, &pos, kMoveLabelHead[moveOption]);
        if (moveOption <= MOVE_OPTION_CENTER && hasPoint) {
            text_put_char(text, &pos, ' ');
            text_put_index(text, &pos, point + 1);
        }
        text_put(text, &pos, kMoveLabelTail[moveOption]);
        return 1;
    }
    if (gCreateOption == CREATE_OPTION_DESIGN) {
        if (designOption < 0 || designOption >= (int) (sizeof(kDesignLabelName) / sizeof(kDesignLabelName[0]))) {
            return 0;
        }
        text_put(text, &pos, "Design: ");
        text_put(text, &pos, kDesignLabelName[designOption]);
    } else if (gCreateOption == CREATE_OPTION_PARTS) {
        if (partsOption < 0 || partsOption >= (int) (sizeof(kPartsLabelName) / sizeof(kPartsLabelName[0]))) {
            return 0;
        }
        text_put(text, &pos, "Parts: ");
        text_put(text, &pos, kPartsLabelName[partsOption]);
    } else if (gCreateOption == CREATE_OPTION_BACKGROUND) {
        text_put(text, &pos, "Option: background");
        return 1;
    } else if (gCreateOption == CREATE_OPTION_BGM) {
        text_put(text, &pos, "Option: music");
        return 1;
    } else {
        return 0;
    }
    if (hasPoint) {
        text_put(text, &pos, " on segment ");
        text_put_index(text, &pos, point + 1);
    }
    return 1;
}

// Sets the row text of a new entry. Only an ordinary edit with no pending label can take a derived label.
static void set_slot_text(GdxCourseEditUndoSlot* entry, int kind) {
    int pos = 0;

    if (kind == GDX_CE_UNDO_KIND_USER && gPendingLabel < 0 &&
        derive_context_text(entry->text, &entry->snapshot) != 0) {
        return;
    }
    text_put(entry->text, &pos, gdx_course_edit_undo_label_text(entry->label));
}

static void sync_flag(void) {
    if (gUndoCount > 0) {
        D_xk2_80104BC0 = 1;
    } else {
        D_xk2_80104BC0 = 0;
    }
}

static int find_free_slot(void) {
    int i;

    for (i = 0; i < GDX_COURSE_EDIT_UNDO_CAPACITY; i++) {
        if (gSlotUsed[i] == 0) {
            return i;
        }
    }
    return -1;
}

static void snapshot_from_live(GdxCourseEditUndoSnapshot* snap) {
    memcpy(snap->selection, D_80128690, sizeof(snap->selection));
    memcpy(snap->fileName, D_80030060, sizeof(snap->fileName));
    memcpy(snap->segments, &D_802CB6D0, sizeof(snap->segments));
    memcpy(snap->courseData, &COURSE_CONTEXT()->courseData, sizeof(snap->courseData));
    memcpy(snap->activePoint, &D_800D6CA0.unk_0C, sizeof(snap->activePoint));
}

static void snapshot_from_backup(GdxCourseEditUndoSnapshot* snap) {
    memcpy(snap->selection, D_xk2_80128990, sizeof(snap->selection));
    memcpy(snap->fileName, D_8003006C, sizeof(snap->fileName));
    memcpy(snap->segments, &D_807B6528, sizeof(snap->segments));
    memcpy(snap->courseData, &D_8010CF50, sizeof(snap->courseData));
    memcpy(snap->activePoint, &D_xk2_80119800, sizeof(snap->activePoint));
}

static void snapshot_to_backup(const GdxCourseEditUndoSnapshot* snap) {
    memcpy(D_xk2_80128990, snap->selection, sizeof(snap->selection));
    memcpy(D_8003006C, snap->fileName, sizeof(snap->fileName));
    memcpy(&D_807B6528, snap->segments, sizeof(snap->segments));
    memcpy(&D_8010CF50, snap->courseData, sizeof(snap->courseData));
    memcpy(&D_xk2_80119800, snap->activePoint, sizeof(snap->activePoint));
}

static int snapshot_matches_live(const GdxCourseEditUndoSnapshot* snap) {
    if (memcmp(snap->selection, D_80128690, sizeof(snap->selection)) != 0) {
        return 0;
    }
    if (memcmp(snap->fileName, D_80030060, sizeof(snap->fileName)) != 0) {
        return 0;
    }
    if (memcmp(snap->segments, &D_802CB6D0, sizeof(snap->segments)) != 0) {
        return 0;
    }
    if (memcmp(snap->courseData, &COURSE_CONTEXT()->courseData, sizeof(snap->courseData)) != 0) {
        return 0;
    }
    if (memcmp(snap->activePoint, &D_800D6CA0.unk_0C, sizeof(snap->activePoint)) != 0) {
        return 0;
    }
    return 1;
}

static void drop_undo_top(void) {
    int slot;

    slot = gUndoSlots[gUndoCount - 1];
    gSlotUsed[slot] = 0;
    gUndoCount--;
}

static void drop_redo_top(void) {
    int slot;

    slot = gRedoSlots[gRedoCount - 1];
    gSlotUsed[slot] = 0;
    gRedoCount--;
}

static void drop_redo_all(void) {
    while (gRedoCount > 0) {
        drop_redo_top();
    }
}

static void evict_bottom_undo(void) {
    int i;

    gSlotUsed[gUndoSlots[0]] = 0;
    for (i = 0; i + 1 < gUndoCount; i++) {
        gUndoSlots[i] = gUndoSlots[i + 1];
    }
    gUndoCount--;
}

static void evict_bottom_redo(void) {
    int i;

    gSlotUsed[gRedoSlots[0]] = 0;
    for (i = 0; i + 1 < gRedoCount; i++) {
        gRedoSlots[i] = gRedoSlots[i + 1];
    }
    gRedoCount--;
}

// Drops newest undo entries that are plain edits and already match the live course. Such an entry would make
// a step look like it did nothing. TEST and LOAD entries are never dropped here.
static void prune_undo_noops(void) {
    int slot;

    while (gUndoCount > 0) {
        slot = gUndoSlots[gUndoCount - 1];
        if (gSlots[slot].kind != GDX_CE_UNDO_KIND_USER) {
            break;
        }
        if (snapshot_matches_live(&gSlots[slot].snapshot) == 0) {
            break;
        }
        drop_undo_top();
    }
}

static void prune_redo_noops(void) {
    int slot;

    while (gRedoCount > 0) {
        slot = gRedoSlots[gRedoCount - 1];
        if (gSlots[slot].kind != GDX_CE_UNDO_KIND_USER) {
            break;
        }
        if (snapshot_matches_live(&gSlots[slot].snapshot) == 0) {
            break;
        }
        drop_redo_top();
    }
}

// Pops the newest undo entry into the backup. Unless the entry is a TEST entry or a LOAD rollback, the live
// course goes into the same slot, which then sits on the redo stack under the same label.
static int begin_undo(void) {
    int slot;
    int kind;
    int keepForRedo;

    if (gUndoCount <= 0) {
        return 0;
    }
    slot = gUndoSlots[gUndoCount - 1];
    kind = gSlots[slot].kind;
    keepForRedo = 1;
    if (kind == GDX_CE_UNDO_KIND_TEST) {
        keepForRedo = 0;
    }
    if (kind == GDX_CE_UNDO_KIND_LOAD && D_800D6CA0.unk_08 == GDX_CE_UNDO_LOAD_CONFIRM_STATE) {
        keepForRedo = 0;
    }
    snapshot_to_backup(&gSlots[slot].snapshot);
    gUndoCount--;
    if (keepForRedo != 0) {
        snapshot_from_live(&gSlots[slot].snapshot);
        gSlots[slot].points = snapshot_point_count(&gSlots[slot].snapshot);
        gSlots[slot].kind = GDX_CE_UNDO_KIND_USER;
        gRedoSlots[gRedoCount] = slot;
        gRedoCount++;
    } else {
        gSlotUsed[slot] = 0;
    }
    gRestoreActive = 1;
    gStepStarted = 1;
    return 1;
}

// Pops the newest redo entry into the backup. The live course goes into the same slot, which moves to the undo
// stack under the same label.
static int begin_redo(void) {
    int slot;

    if (gRedoCount <= 0) {
        return 0;
    }
    slot = gRedoSlots[gRedoCount - 1];
    gRedoCount--;
    snapshot_to_backup(&gSlots[slot].snapshot);
    snapshot_from_live(&gSlots[slot].snapshot);
    gSlots[slot].points = snapshot_point_count(&gSlots[slot].snapshot);
    gSlots[slot].kind = GDX_CE_UNDO_KIND_USER;
    gUndoSlots[gUndoCount] = slot;
    gUndoCount++;
    gRestoreActive = 1;
    gStepStarted = 1;
    return 1;
}

int gdx_course_edit_undo_begin_restore(void) {
    int started;

    if (gJumpStepping == 0) {
        gJumpTarget = -1;
        if (gRestoreIsRedo != 0) {
            prune_redo_noops();
        } else {
            prune_undo_noops();
        }
        sync_flag();
    }
    gStepStarted = 0;
    if (gRestoreIsRedo != 0) {
        started = begin_redo();
    } else {
        started = begin_undo();
    }
    return started;
}

void gdx_course_edit_undo_end_restore(void) {
    gRestoreActive = 0;
}

int gdx_course_edit_undo_depth(void) {
    return gUndoCount;
}

int gdx_course_edit_undo_play_restore_sound(void) {
    if (gSuppressSound != 0) {
        return 0;
    }
    return 1;
}

void gdx_course_edit_undo_clear(void) {
    int i;

    for (i = 0; i < GDX_COURSE_EDIT_UNDO_CAPACITY; i++) {
        gSlotUsed[i] = 0;
    }
    gUndoCount = 0;
    gRedoCount = 0;
    gPendingLabel = -1;
    gJumpTarget = -1;
    gRequest = GDX_CE_UNDO_REQUEST_NONE;
    gRequestRow = 0;
    gSuppressSound = 0;
    sync_flag();
}

void gdx_course_edit_undo_push(void) {
    int kind;
    int slot;

    if (gRestoreActive != 0) {
        return;
    }
    kind = current_kind();
    if (kind == GDX_CE_UNDO_KIND_TEST) {
        if (gUndoCount > 0 && gSlots[gUndoSlots[gUndoCount - 1]].kind == GDX_CE_UNDO_KIND_TEST) {
            return;
        }
    } else {
        drop_redo_all();
    }
    gJumpTarget = -1;
    while (find_free_slot() < 0) {
        if (gUndoCount > 0) {
            evict_bottom_undo();
        } else {
            evict_bottom_redo();
        }
    }
    slot = find_free_slot();
    snapshot_from_backup(&gSlots[slot].snapshot);
    gSlots[slot].kind = (unsigned char)kind;
    gSlots[slot].label = (unsigned char)label_for_kind(kind);
    gSlots[slot].points = snapshot_point_count(&gSlots[slot].snapshot);
    set_slot_text(&gSlots[slot], kind);
    gSlotUsed[slot] = 1;
    gUndoSlots[gUndoCount] = slot;
    gUndoCount++;
    gPendingLabel = -1;
    sync_flag();
}

void gdx_course_edit_undo_set_pending_label(int label) {
    if (label < 0 || label >= GDX_COURSE_EDIT_UNDO_LABEL_COUNT) {
        gPendingLabel = -1;
        return;
    }
    gPendingLabel = label;
}

void gdx_course_edit_undo_request_undo(void) {
    gRequest = GDX_CE_UNDO_REQUEST_UNDO;
}

void gdx_course_edit_undo_request_redo(void) {
    gRequest = GDX_CE_UNDO_REQUEST_REDO;
}

void gdx_course_edit_undo_request_row(int row) {
    gRequest = GDX_CE_UNDO_REQUEST_ROW;
    gRequestRow = row;
}

int gdx_course_edit_undo_undo_count(void) {
    return gUndoCount;
}

int gdx_course_edit_undo_redo_count(void) {
    return gRedoCount;
}

// Returns the pool slot shown on a History row, or -1 for a row outside the two stacks.
static int slot_for_row(int row) {
    int redoIndex;

    if (row < 0 || row >= gUndoCount + gRedoCount) {
        return -1;
    }
    if (row < gUndoCount) {
        return gUndoSlots[row];
    }
    redoIndex = gRedoCount - 1 - (row - gUndoCount);
    return gRedoSlots[redoIndex];
}

int gdx_course_edit_undo_row_label(int row) {
    int slot = slot_for_row(row);

    if (slot < 0) {
        return -1;
    }
    return gSlots[slot].label;
}

const char* gdx_course_edit_undo_row_text(int row) {
    int slot = slot_for_row(row);

    if (slot < 0) {
        return "";
    }
    return gSlots[slot].text;
}

int gdx_course_edit_undo_row_points(int row) {
    int slot = slot_for_row(row);

    if (slot < 0) {
        return 0;
    }
    return gSlots[slot].points;
}

const char* gdx_course_edit_undo_label_text(int label) {
    static const char* const kLabelText[GDX_COURSE_EDIT_UNDO_LABEL_COUNT] = {
        "Edit", "Select", "Move", "Delete", "Line up", "Split", "Option", "Test run", "Load course",
    };

    if (label < 0 || label >= GDX_COURSE_EDIT_UNDO_LABEL_COUNT) {
        return "Unknown";
    }
    return kLabelText[label];
}

// A step is refused during a test run, while the mouse owns a drag, and in the two states the B path refuses.
// A refused step is silent. A step that the restore guard refuses plays the refusal sound instead.
static int can_step(void) {
    if (gInCourseEditTestRun) {
        return 0;
    }
    if (D_800D6CA0.unk_08 == GDX_CE_UNDO_REFUSE_STATE_A || D_800D6CA0.unk_08 == GDX_CE_UNDO_REFUSE_STATE_CLEAR) {
        return 0;
    }
    if (gdx_course_edit_native_mouse_owned() != 0) {
        return 0;
    }
    return 1;
}

// Runs one restore. The decomp guard may refuse it, in which case nothing moves and the result is 0.
static int step_once(int redo) {
    gStepStarted = 0;
    if (redo != 0) {
        gRestoreIsRedo = 1;
    } else {
        gRestoreIsRedo = 0;
    }
    func_xk2_800EF8B0();
    gRestoreIsRedo = 0;
    return gStepStarted;
}

// Maps a panel row to the undo count it reaches. An undo row i reaches i. A redo row r reaches r + 1, because
// the redo rows start at the current undo count.
static int row_to_target(int row) {
    if (row < 0 || row >= gUndoCount + gRedoCount) {
        return -1;
    }
    if (row < gUndoCount) {
        return row;
    }
    return row + 1;
}

// Applies one step toward gJumpTarget. Every step of a jump restores a whole venue, so a jump takes one step per
// call. Only the last step plays the restore sound.
static void jump_tick(void) {
    int redo;
    int distance;
    int changed;

    if (gJumpTarget < 0) {
        return;
    }
    if (gUndoCount == gJumpTarget) {
        gJumpTarget = -1;
        return;
    }
    if (can_step() == 0) {
        gJumpTarget = -1;
        return;
    }
    redo = (gUndoCount < gJumpTarget) ? 1 : 0;
    if (redo != 0) {
        distance = gJumpTarget - gUndoCount;
    } else {
        distance = gUndoCount - gJumpTarget;
    }
    gSuppressSound = (distance > 1) ? 1 : 0;
    gJumpStepping = 1;
    changed = step_once(redo);
    gJumpStepping = 0;
    gSuppressSound = 0;
    if (changed == 0) {
        gJumpTarget = -1;
        Audio_TriggerSystemSE(NA_SE_32);
        return;
    }
    if (gUndoCount == gJumpTarget) {
        gJumpTarget = -1;
    }
}

void gdx_course_edit_undo_tick(void) {
    int request;
    int row;

    request = gRequest;
    row = gRequestRow;
    gRequest = GDX_CE_UNDO_REQUEST_NONE;
    if (request == GDX_CE_UNDO_REQUEST_NONE) {
        jump_tick();
        return;
    }
    gJumpTarget = -1;
    if (can_step() == 0) {
        return;
    }
    if (request == GDX_CE_UNDO_REQUEST_UNDO) {
        prune_undo_noops();
        gJumpTarget = (gUndoCount > 0) ? gUndoCount - 1 : -1;
    } else if (request == GDX_CE_UNDO_REQUEST_REDO) {
        prune_redo_noops();
        gJumpTarget = (gRedoCount > 0) ? gUndoCount + 1 : -1;
    } else {
        gJumpTarget = row_to_target(row);
    }
    sync_flag();
    if (gJumpTarget < 0) {
        Audio_TriggerSystemSE(NA_SE_32);
        return;
    }
    jump_tick();
}
