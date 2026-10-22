/* port/gdx_course_branch_format.c -- course fork model, GXB1 block, and GBS1 state file.
 *
 * Contract and layout live in gdx_course_branch_format.h. Pure C, no decomp headers, so the
 * standalone test can build this file with gdx_course_registry.c (CRC-32 only).
 */

#include "gdx_course_branch_format.h"

#include "gdx_course_registry.h"

#include <string.h>


/* ---------------------------------------------------------------------------------
 * Little-endian writers. Host endianness must not leak into the file, so float bits are
 * copied into an integer and then stored byte by byte.
 * ------------------------------------------------------------------------------- */

static void put_u16le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
}

static void put_u32le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

static uint32_t get_u16le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

static uint32_t get_u32le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void put_f32le(unsigned char* p, float v) {
    uint32_t bits;

    memcpy(&bits, &v, sizeof(bits));
    put_u32le(p, bits);
}

static float get_f32le(const unsigned char* p) {
    uint32_t bits;
    float v;

    bits = get_u32le(p);
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static void put_s16le(unsigned char* p, int16_t v) {
    put_u16le(p, (uint16_t) v);
}

static void put_s32le(unsigned char* p, int32_t v) {
    put_u32le(p, (uint32_t) v);
}

static int16_t get_s16le(const unsigned char* p) {
    return (int16_t) get_u16le(p);
}

static int32_t get_s32le(const unsigned char* p) {
    return (int32_t) get_u32le(p);
}

/* Exponent 0xFF is Inf or NaN. The bit test keeps the codec off libm. */
static int float_is_finite(float v) {
    uint32_t bits;

    memcpy(&bits, &v, sizeof(bits));
    if (((bits >> 23) & 0xFFu) == 0xFFu) {
        return 0;
    }
    return 1;
}

static int fail(char* reason, size_t cap, int code, const char* text) {
    size_t n;

    if (reason != NULL && cap > 0) {
        n = strlen(text);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(reason, text, n);
        reason[n] = '\0';
    }
    return code;
}

static int pass(char* reason, size_t cap) {
    if (reason != NULL && cap > 0) {
        reason[0] = '\0';
    }
    return GDX_BRANCH_OK;
}

static int bytes_fit(size_t pos, size_t end, size_t count) {
    if (pos > end) {
        return 0;
    }
    if (count > end - pos) {
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------------
 * S71.
 * ------------------------------------------------------------------------------- */

int gdx_branch_validate(const GdxBranchSet* set, int ringCount, char* reason, size_t cap) {
    int total;
    int i;

    if (set == NULL) {
        return fail(reason, cap, GDX_BRANCH_ERR_ARGS, "null set");
    }
    if (ringCount < 0) {
        return fail(reason, cap, GDX_BRANCH_ERR_RING, "ring count is negative");
    }
    /* The value that encode writes is set->ringCount. A different argument would validate
     * one ring and persist another. */
    if (ringCount > 255 || set->ringCount != (uint8_t) ringCount) {
        return fail(reason, cap, GDX_BRANCH_ERR_RING, "ring count does not match the set");
    }
    if (set->forkCount > GDX_BRANCH_MAX_FORKS) {
        return fail(reason, cap, GDX_BRANCH_ERR_COUNT, "fork count is above 4");
    }
    if (ringCount > GDX_BRANCH_MAX_TOTAL_POINTS) {
        return fail(reason, cap, GDX_BRANCH_ERR_TOTAL, "ring points plus branch nodes exceed 64");
    }

    total = ringCount;
    for (i = 0; i < (int) set->forkCount; i++) {
        const GdxBranchFork* fork = &set->forks[i];
        int j;

        if (fork->nodeCount < 1 || fork->nodeCount > GDX_BRANCH_MAX_NODES) {
            return fail(reason, cap, GDX_BRANCH_ERR_COUNT, "node count must be 1 to 32");
        }
        /* Version 1 defines only bit0. Ignoring another bit would drop it on the next encode. */
        if ((fork->flags & (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC) != 0) {
            return fail(reason, cap, GDX_BRANCH_ERR_FLAGS, "fork flags have an unknown bit");
        }
        if (fork->forkPoint == 0) {
            return fail(reason, cap, GDX_BRANCH_ERR_SPAN, "fork touches the start line");
        }
        if (fork->forkPoint >= fork->mergePoint) {
            return fail(reason, cap, GDX_BRANCH_ERR_SPAN, "fork wraps or fork point is not before merge");
        }
        if ((int) fork->mergePoint > ringCount - 1) {
            return fail(reason, cap, GDX_BRANCH_ERR_SPAN, "merge point is past the ring");
        }
        /* S71: sorted by forkPoint, each mergePoint <= the next forkPoint. Equality is a
         * meeting point, not an overlap. A shared fork point fails because merge > fork. */
        if (i > 0 && set->forks[i - 1].mergePoint > fork->forkPoint) {
            return fail(reason, cap, GDX_BRANCH_ERR_OVERLAP, "forks overlap or are not sorted");
        }
        if ((fork->flags & GDX_BRANCH_FLAG_SYMMETRIC) != 0) {
            int pathA = (int) fork->mergePoint - (int) fork->forkPoint - 1;
            if ((int) fork->nodeCount != pathA) {
                return fail(reason, cap, GDX_BRANCH_ERR_SYMMETRIC,
                            "symmetric fork node count does not match path A");
            }
        }
        for (j = 0; j < (int) fork->nodeCount; j++) {
            const GdxBranchNode* node = &fork->nodes[j];
            if (!float_is_finite(node->pos[0]) || !float_is_finite(node->pos[1]) ||
                !float_is_finite(node->pos[2])) {
                return fail(reason, cap, GDX_BRANCH_ERR_FLOAT, "a branch node position is not finite");
            }
        }
        total += (int) fork->nodeCount;
        if (total > GDX_BRANCH_MAX_TOTAL_POINTS) {
            return fail(reason, cap, GDX_BRANCH_ERR_TOTAL, "ring points plus branch nodes exceed 64");
        }
    }
    return pass(reason, cap);
}

/* ---------------------------------------------------------------------------------
 * Ring index maintenance. ringCount and the node lists stay with the caller.
 * ------------------------------------------------------------------------------- */

static void fork_remove_at(GdxBranchSet* set, int index) {
    int i;

    for (i = index; i < (int) set->forkCount - 1; i++) {
        set->forks[i] = set->forks[i + 1];
    }
    memset(&set->forks[set->forkCount - 1], 0, sizeof(set->forks[0]));
    set->forkCount--;
}

int gdx_branch_on_ring_insert(GdxBranchSet* set, int newIndex) {
    int i;

    if (set == NULL || newIndex < 0) {
        return 0;
    }
    /* An insert inside path A is still only a shift. The editor pairs the new ring point. */
    for (i = 0; i < (int) set->forkCount; i++) {
        GdxBranchFork* fork = &set->forks[i];
        if ((int) fork->forkPoint >= newIndex && fork->forkPoint < 255) {
            fork->forkPoint++;
        }
        if ((int) fork->mergePoint >= newIndex && fork->mergePoint < 255) {
            fork->mergePoint++;
        }
    }
    return 0;
}

int gdx_branch_on_ring_delete(GdxBranchSet* set, int deletedIndex) {
    int removed = 0;
    int i = 0;

    if (set == NULL || deletedIndex < 0) {
        return 0;
    }
    while (i < (int) set->forkCount) {
        GdxBranchFork* fork = &set->forks[i];
        /* Shifting an endpoint that was deleted would name a ring point that is gone. */
        if ((int) fork->forkPoint == deletedIndex || (int) fork->mergePoint == deletedIndex) {
            fork_remove_at(set, i);
            removed++;
            continue;
        }
        if ((fork->flags & GDX_BRANCH_FLAG_SYMMETRIC) != 0 && deletedIndex > (int) fork->forkPoint &&
            deletedIndex < (int) fork->mergePoint) {
            fork->flags = (uint8_t) (fork->flags & (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC);
        }
        if ((int) fork->forkPoint > deletedIndex) {
            fork->forkPoint--;
        }
        if ((int) fork->mergePoint > deletedIndex) {
            fork->mergePoint--;
        }
        i++;
    }
    return removed;
}

int gdx_branch_insert_node(GdxBranchFork* fork, int at, const GdxBranchNode* node) {
    int tail;

    if (fork == NULL || node == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    if (fork->nodeCount >= GDX_BRANCH_MAX_NODES) {
        return GDX_BRANCH_ERR_FULL;
    }
    if (at < 0 || at > (int) fork->nodeCount) {
        return GDX_BRANCH_ERR_INDEX;
    }
    tail = (int) fork->nodeCount - at;
    if (tail > 0) {
        memmove(&fork->nodes[at + 1], &fork->nodes[at], (size_t) tail * sizeof(fork->nodes[0]));
    }
    fork->nodes[at] = *node;
    fork->nodeCount++;
    return GDX_BRANCH_OK;
}

int gdx_branch_delete_node(GdxBranchFork* fork, int at) {
    int tail;

    if (fork == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    if (at < 0 || at >= (int) fork->nodeCount) {
        return GDX_BRANCH_ERR_INDEX;
    }
    if (fork->nodeCount <= 1) {
        return GDX_BRANCH_ERR_COUNT;
    }
    tail = (int) fork->nodeCount - at - 1;
    if (tail > 0) {
        memmove(&fork->nodes[at], &fork->nodes[at + 1], (size_t) tail * sizeof(fork->nodes[0]));
    }
    memset(&fork->nodes[fork->nodeCount - 1], 0, sizeof(fork->nodes[0]));
    fork->nodeCount--;
    return GDX_BRANCH_OK;
}

int gdx_branch_find_fork(const GdxBranchSet* set, int ringPoint) {
    int i;

    if (set == NULL || ringPoint < 0) {
        return -1;
    }
    for (i = 0; i < (int) set->forkCount; i++) {
        const GdxBranchFork* fork = &set->forks[i];
        if (ringPoint >= (int) fork->forkPoint && ringPoint <= (int) fork->mergePoint) {
            return i;
        }
    }
    return -1;
}

static int format_label(char* out, size_t cap, int value, const char* suffix) {
    char rev[16];
    char digits[16];
    int n = 0;
    int r = 0;
    int v;
    size_t suffixLen;
    size_t i;

    if (out == NULL || cap == 0 || value < 0) {
        return GDX_BRANCH_ERR_ARGS;
    }
    v = value;
    if (v == 0) {
        digits[n++] = '0';
    } else {
        while (v > 0) {
            rev[r++] = (char) ('0' + (v % 10));
            v /= 10;
        }
        while (r > 0) {
            digits[n++] = rev[--r];
        }
    }
    suffixLen = 0;
    if (suffix != NULL) {
        suffixLen = strlen(suffix);
    }
    if ((size_t) n + suffixLen + 1 > cap) {
        out[0] = '\0';
        return GDX_BRANCH_ERR_FULL;
    }
    for (i = 0; i < (size_t) n; i++) {
        out[i] = digits[i];
    }
    if (suffixLen > 0) {
        memcpy(out + n, suffix, suffixLen);
    }
    out[(size_t) n + suffixLen] = '\0';
    return GDX_BRANCH_OK;
}

int gdx_branch_label_ring(const GdxBranchSet* set, int ringPoint, char* out, size_t cap) {
    int i;

    if (set == NULL || ringPoint < 0) {
        return GDX_BRANCH_ERR_ARGS;
    }
    for (i = 0; i < (int) set->forkCount; i++) {
        const GdxBranchFork* fork = &set->forks[i];
        if (ringPoint > (int) fork->forkPoint && ringPoint < (int) fork->mergePoint) {
            return format_label(out, cap, ringPoint, "-A");
        }
    }
    return format_label(out, cap, ringPoint, NULL);
}

int gdx_branch_label_node(const GdxBranchSet* set, int fork, int node, char* out, size_t cap) {
    const GdxBranchFork* branch;
    int n;

    if (set == NULL || fork < 0 || fork >= (int) set->forkCount) {
        return GDX_BRANCH_ERR_INDEX;
    }
    branch = &set->forks[fork];
    if (node < 0 || node >= (int) branch->nodeCount) {
        return GDX_BRANCH_ERR_INDEX;
    }
    n = (int) branch->forkPoint + 1 + node;
    return format_label(out, cap, n, "-B");
}

/* ---------------------------------------------------------------------------------
 * GXB1. The in-memory struct is not written as a block: padding is not part of the format.
 * ------------------------------------------------------------------------------- */

static void put_node(unsigned char* p, const GdxBranchNode* node) {
    put_f32le(p + 0, node->pos[0]);
    put_f32le(p + 4, node->pos[1]);
    put_f32le(p + 8, node->pos[2]);
    put_s16le(p + 12, node->radiusLeft);
    put_s16le(p + 14, node->radiusRight);
    put_s32le(p + 16, node->trackSegmentInfo);
    put_s16le(p + 20, node->bankAngle);
    p[22] = (unsigned char) node->pit;
    p[23] = (unsigned char) node->dash;
    p[24] = (unsigned char) node->dirt;
    p[25] = (unsigned char) node->ice;
    p[26] = (unsigned char) node->jump;
    p[27] = (unsigned char) node->landmine;
    p[28] = (unsigned char) node->gate;
    p[29] = (unsigned char) node->building;
    p[30] = (unsigned char) node->sign;
    p[31] = 0;
}

static int get_node(const unsigned char* p, GdxBranchNode* node) {
    if (p[31] != 0) {
        return 0;
    }
    node->pos[0] = get_f32le(p + 0);
    node->pos[1] = get_f32le(p + 4);
    node->pos[2] = get_f32le(p + 8);
    node->radiusLeft = get_s16le(p + 12);
    node->radiusRight = get_s16le(p + 14);
    node->trackSegmentInfo = get_s32le(p + 16);
    node->bankAngle = get_s16le(p + 20);
    node->pit = (int8_t) p[22];
    node->dash = (int8_t) p[23];
    node->dirt = (int8_t) p[24];
    node->ice = (int8_t) p[25];
    node->jump = (int8_t) p[26];
    node->landmine = (int8_t) p[27];
    node->gate = (int8_t) p[28];
    node->building = (int8_t) p[29];
    node->sign = (int8_t) p[30];
    return 1;
}

static size_t block_size(const GdxBranchSet* set) {
    size_t need = GDX_BRANCH_BLOCK_HEADER_SIZE + 4;
    int i;

    for (i = 0; i < (int) set->forkCount; i++) {
        need += 4 + (size_t) set->forks[i].nodeCount * GDX_BRANCH_NODE_BYTES;
    }
    return need;
}

int gdx_branch_encode(const GdxBranchSet* set, uint32_t bindCrc, unsigned char* out, size_t cap) {
    size_t need;
    size_t pos;
    int rc;
    int i;
    int j;

    if (set == NULL || out == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    rc = gdx_branch_validate(set, (int) set->ringCount, NULL, 0);
    if (rc != GDX_BRANCH_OK) {
        return rc;
    }
    need = block_size(set);
    if (need > cap) {
        return GDX_BRANCH_ERR_FULL;
    }

    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'B';
    out[3] = '1';
    put_u16le(out + 4, GDX_BRANCH_BLOCK_VERSION);
    out[6] = set->forkCount;
    out[7] = set->ringCount;
    put_u32le(out + 8, bindCrc);
    put_u32le(out + 12, set->courseChecksum);

    pos = GDX_BRANCH_BLOCK_HEADER_SIZE;
    for (i = 0; i < (int) set->forkCount; i++) {
        const GdxBranchFork* fork = &set->forks[i];
        out[pos] = fork->forkPoint;
        out[pos + 1] = fork->mergePoint;
        out[pos + 2] = fork->flags;
        out[pos + 3] = fork->nodeCount;
        pos += 4;
        for (j = 0; j < (int) fork->nodeCount; j++) {
            put_node(out + pos, &fork->nodes[j]);
            pos += GDX_BRANCH_NODE_BYTES;
        }
    }
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

int gdx_branch_decode(const unsigned char* data, size_t length, uint32_t bindCrc, GdxBranchSet* out) {
    GdxBranchSet parsed;
    size_t end;
    size_t pos;
    unsigned forkCount;
    unsigned ringCount;
    int rc;
    int i;
    int j;

    if (out == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    /* A failed parse must not look like a fork the caller already had in out. */
    memset(out, 0, sizeof(*out));
    if (data == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    if (length < GDX_BRANCH_BLOCK_MIN_BYTES) {
        return GDX_BRANCH_ERR_TRUNCATED;
    }
    if (data[0] != 'G' || data[1] != 'X' || data[2] != 'B' || data[3] != '1') {
        return GDX_BRANCH_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != GDX_BRANCH_BLOCK_VERSION) {
        return GDX_BRANCH_ERR_VERSION;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_BRANCH_ERR_CRC;
    }
    if (get_u32le(data + 8) != bindCrc) {
        return GDX_BRANCH_ERR_BINDING;
    }

    forkCount = data[6];
    ringCount = data[7];
    if (forkCount > GDX_BRANCH_MAX_FORKS) {
        return GDX_BRANCH_ERR_LAYOUT;
    }

    memset(&parsed, 0, sizeof(parsed));
    parsed.courseChecksum = get_u32le(data + 12);
    parsed.ringCount = (uint8_t) ringCount;
    parsed.forkCount = (uint8_t) forkCount;
    pos = GDX_BRANCH_BLOCK_HEADER_SIZE;
    for (i = 0; i < (int) forkCount; i++) {
        unsigned nodeCount;
        GdxBranchFork* fork = &parsed.forks[i];

        if (!bytes_fit(pos, end, 4)) {
            return GDX_BRANCH_ERR_LAYOUT;
        }
        fork->forkPoint = data[pos];
        fork->mergePoint = data[pos + 1];
        fork->flags = data[pos + 2];
        nodeCount = data[pos + 3];
        fork->nodeCount = (uint8_t) nodeCount;
        pos += 4;
        if (nodeCount > GDX_BRANCH_MAX_NODES) {
            return GDX_BRANCH_ERR_LAYOUT;
        }
        if (!bytes_fit(pos, end, (size_t) nodeCount * GDX_BRANCH_NODE_BYTES)) {
            return GDX_BRANCH_ERR_LAYOUT;
        }
        for (j = 0; j < (int) nodeCount; j++) {
            if (!get_node(data + pos, &fork->nodes[j])) {
                return GDX_BRANCH_ERR_LAYOUT;
            }
            pos += GDX_BRANCH_NODE_BYTES;
        }
    }
    if (pos != end) {
        return GDX_BRANCH_ERR_LAYOUT;
    }
    rc = gdx_branch_validate(&parsed, (int) parsed.ringCount, NULL, 0);
    if (rc != GDX_BRANCH_OK) {
        return rc;
    }
    *out = parsed;
    return GDX_BRANCH_OK;
}

/* ---------------------------------------------------------------------------------
 * Store. A side buffer holds a file until every entry has decoded, so a bad file cannot
 * clear a store that was already loaded.
 * ------------------------------------------------------------------------------- */

typedef struct BranchSlot {
    int used;
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxBranchSet set;
} BranchSlot;

static BranchSlot sStore[GDX_BRANCH_STORE_SLOTS];
static BranchSlot sStage[GDX_BRANCH_STORE_SLOTS];
static unsigned long sRevision;
static GdxBranchSet sCurrent;
static int sCurrentValid;

static void store_touch(void) {
    sRevision++;
}

static int id_char_ok(unsigned char c) {
    if (c < 0x20 || c > 0x7E) {
        return 0;
    }
    return 1;
}

static int course_id_ok(const char* courseId) {
    size_t i = 0;

    if (courseId == NULL) {
        return 0;
    }
    while (i <= GDX_BRANCH_COURSE_ID_MAX && courseId[i] != '\0') {
        if (!id_char_ok((unsigned char) courseId[i])) {
            return 0;
        }
        i++;
    }
    if (i < 1 || i > GDX_BRANCH_COURSE_ID_MAX) {
        return 0;
    }
    return 1;
}

static int store_find(const BranchSlot* slots, const char* courseId) {
    int i;

    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        if (slots[i].used && strcmp(slots[i].courseId, courseId) == 0) {
            return i;
        }
    }
    return -1;
}

static int store_free(const BranchSlot* slots) {
    int i;

    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        if (!slots[i].used) {
            return i;
        }
    }
    return -1;
}

static void copy_id(char* dst, const char* src) {
    size_t n = 0;

    while (src[n] != '\0' && n < GDX_BRANCH_COURSE_ID_MAX) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

static void copy_set_clean(GdxBranchSet* dst, const GdxBranchSet* src) {
    int i;

    memset(dst, 0, sizeof(*dst));
    dst->courseChecksum = src->courseChecksum;
    dst->ringCount = src->ringCount;
    dst->forkCount = src->forkCount;
    for (i = 0; i < (int) src->forkCount && i < GDX_BRANCH_MAX_FORKS; i++) {
        int nodes = (int) src->forks[i].nodeCount;
        dst->forks[i].forkPoint = src->forks[i].forkPoint;
        dst->forks[i].mergePoint = src->forks[i].mergePoint;
        dst->forks[i].flags = src->forks[i].flags;
        dst->forks[i].nodeCount = src->forks[i].nodeCount;
        if (nodes > GDX_BRANCH_MAX_NODES) {
            nodes = GDX_BRANCH_MAX_NODES;
        }
        if (nodes > 0) {
            memcpy(dst->forks[i].nodes, src->forks[i].nodes, (size_t) nodes * sizeof(GdxBranchNode));
        }
    }
}

int gdx_branch_store_set(const char* courseId, const GdxBranchSet* set) {
    int index;
    int rc;

    if (!course_id_ok(courseId) || set == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    index = store_find(sStore, courseId);
    if (set->forkCount == 0) {
        if (index < 0) {
            return GDX_BRANCH_OK;
        }
        memset(&sStore[index], 0, sizeof(sStore[index]));
        store_touch();
        return GDX_BRANCH_OK;
    }
    rc = gdx_branch_validate(set, (int) set->ringCount, NULL, 0);
    if (rc != GDX_BRANCH_OK) {
        return rc;
    }
    if (index < 0) {
        index = store_free(sStore);
        if (index < 0) {
            return GDX_BRANCH_ERR_FULL;
        }
        memset(&sStore[index], 0, sizeof(sStore[index]));
        sStore[index].used = 1;
        copy_id(sStore[index].courseId, courseId);
    }
    copy_set_clean(&sStore[index].set, set);
    store_touch();
    return GDX_BRANCH_OK;
}

int gdx_branch_store_get(const char* courseId, GdxBranchSet* out) {
    int index;

    if (!course_id_ok(courseId) || out == NULL) {
        return 0;
    }
    index = store_find(sStore, courseId);
    if (index < 0) {
        return 0;
    }
    *out = sStore[index].set;
    return 1;
}

int gdx_branch_store_rename(const char* oldId, const char* newId) {
    int oldIndex;
    int newIndex;

    if (!course_id_ok(oldId) || !course_id_ok(newId)) {
        return GDX_BRANCH_ERR_ARGS;
    }
    if (strcmp(oldId, newId) == 0) {
        return GDX_BRANCH_OK;
    }
    oldIndex = store_find(sStore, oldId);
    newIndex = store_find(sStore, newId);
    if (oldIndex < 0 && newIndex < 0) {
        return GDX_BRANCH_OK;
    }
    if (oldIndex < 0) {
        memset(&sStore[newIndex], 0, sizeof(sStore[newIndex]));
        store_touch();
        return GDX_BRANCH_OK;
    }
    if (newIndex < 0) {
        copy_id(sStore[oldIndex].courseId, newId);
        store_touch();
        return GDX_BRANCH_OK;
    }
    sStore[newIndex].set = sStore[oldIndex].set;
    sStore[newIndex].used = 1;
    memset(&sStore[oldIndex], 0, sizeof(sStore[oldIndex]));
    store_touch();
    return GDX_BRANCH_OK;
}

void gdx_branch_store_clear(void) {
    memset(sStore, 0, sizeof(sStore));
    store_touch();
}

unsigned long gdx_branch_store_revision(void) {
    return sRevision;
}

static int encode_slot_block(const BranchSlot* slot, unsigned char* block, size_t cap) {
    size_t idLen;

    idLen = strlen(slot->courseId);
    return gdx_branch_encode(&slot->set, gdx_course_registry_crc32((const unsigned char*) slot->courseId, idLen),
                             block, cap);
}

int gdx_branch_store_encode_file(unsigned char* out, size_t cap) {
    unsigned char block[GDX_BRANCH_BLOCK_MAX_BYTES];
    size_t total;
    size_t pos;
    int count;
    int i;
    int written;

    if (out == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    count = 0;
    total = GDX_BRANCH_FILE_HEADER_SIZE + 4;
    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        size_t idLen;
        if (!sStore[i].used) {
            continue;
        }
        written = encode_slot_block(&sStore[i], block, sizeof(block));
        if (written < 0) {
            return written;
        }
        idLen = strlen(sStore[i].courseId);
        total += 1 + idLen + 2 + (size_t) written;
        count++;
    }
    if (total > cap) {
        return GDX_BRANCH_ERR_FULL;
    }

    pos = GDX_BRANCH_FILE_HEADER_SIZE;
    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        size_t idLen;
        if (!sStore[i].used) {
            continue;
        }
        written = encode_slot_block(&sStore[i], block, sizeof(block));
        if (written < 0) {
            return written;
        }
        idLen = strlen(sStore[i].courseId);
        out[pos] = (unsigned char) idLen;
        memcpy(out + pos + 1, sStore[i].courseId, idLen);
        pos += 1 + idLen;
        put_u16le(out + pos, (uint32_t) written);
        pos += 2;
        memcpy(out + pos, block, (size_t) written);
        pos += (size_t) written;
    }
    out[0] = 'G';
    out[1] = 'B';
    out[2] = 'S';
    out[3] = '1';
    put_u16le(out + 4, GDX_BRANCH_FILE_VERSION);
    put_u16le(out + 6, (uint32_t) count);
    put_u32le(out + 8, (uint32_t) (pos + 4));
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

static int file_read_entries(const unsigned char* data, size_t length, BranchSlot* slots) {
    uint32_t count;
    size_t end;
    size_t pos;
    int i;
    int j;

    if (data == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    if (length < GDX_BRANCH_FILE_MIN_BYTES) {
        return GDX_BRANCH_FILE_ERR_TRUNCATED;
    }
    if (data[0] != 'G' || data[1] != 'B' || data[2] != 'S' || data[3] != '1') {
        return GDX_BRANCH_FILE_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != GDX_BRANCH_FILE_VERSION) {
        return GDX_BRANCH_FILE_ERR_VERSION;
    }
    count = get_u16le(data + 6);
    if (count > GDX_BRANCH_STORE_SLOTS) {
        return GDX_BRANCH_FILE_ERR_COUNT;
    }
    if (get_u32le(data + 8) != (uint32_t) length) {
        return GDX_BRANCH_FILE_ERR_LENGTH;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_BRANCH_FILE_ERR_CRC;
    }

    memset(slots, 0, sizeof(BranchSlot) * (size_t) GDX_BRANCH_STORE_SLOTS);
    pos = GDX_BRANCH_FILE_HEADER_SIZE;
    for (i = 0; i < (int) count; i++) {
        size_t idLen;
        size_t blockLen;
        uint32_t bind;
        int rc;

        if (!bytes_fit(pos, end, 1)) {
            return GDX_BRANCH_FILE_ERR_LENGTH;
        }
        idLen = data[pos];
        pos++;
        if (idLen < 1 || idLen > GDX_BRANCH_COURSE_ID_MAX || !bytes_fit(pos, end, idLen + 2)) {
            return GDX_BRANCH_FILE_ERR_LENGTH;
        }
        for (j = 0; j < (int) idLen; j++) {
            if (!id_char_ok(data[pos + (size_t) j])) {
                return GDX_BRANCH_FILE_ERR_ENTRY;
            }
        }
        memcpy(slots[i].courseId, data + pos, idLen);
        slots[i].courseId[idLen] = '\0';
        for (j = 0; j < i; j++) {
            if (strcmp(slots[j].courseId, slots[i].courseId) == 0) {
                return GDX_BRANCH_FILE_ERR_ENTRY;
            }
        }
        pos += idLen;
        blockLen = get_u16le(data + pos);
        pos += 2;
        if (!bytes_fit(pos, end, blockLen)) {
            return GDX_BRANCH_FILE_ERR_LENGTH;
        }
        bind = gdx_course_registry_crc32((const unsigned char*) slots[i].courseId, idLen);
        rc = gdx_branch_decode(data + pos, blockLen, bind, &slots[i].set);
        if (rc != GDX_BRANCH_OK) {
            return rc;
        }
        /* store_set treats forkCount 0 as "delete". A file entry with no forks would break that. */
        if (slots[i].set.forkCount == 0) {
            return GDX_BRANCH_FILE_ERR_ENTRY;
        }
        slots[i].used = 1;
        pos += blockLen;
    }
    if (pos != end) {
        return GDX_BRANCH_FILE_ERR_LENGTH;
    }
    return GDX_BRANCH_OK;
}

int gdx_branch_store_restore_file(const unsigned char* data, size_t length) {
    int rc;

    rc = file_read_entries(data, length, sStage);
    if (rc != GDX_BRANCH_OK) {
        return rc;
    }
    memcpy(sStore, sStage, sizeof(sStore));
    store_touch();
    return GDX_BRANCH_OK;
}

const GdxBranchSet* gdx_branch_active_set(void) {
    if (!sCurrentValid) {
        return NULL;
    }
    return &sCurrent;
}

void gdx_branch_set_active(const GdxBranchSet* set) {
    if (set == NULL) {
        memset(&sCurrent, 0, sizeof(sCurrent));
        sCurrentValid = 0;
        return;
    }
    sCurrent = *set;
    sCurrentValid = 1;
}
