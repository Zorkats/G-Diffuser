/* port/tests/gdx_course_branch_format_tests.c -- unit tests for the course fork model, GXB1, and GBS1.
 * Standalone: links gdx_course_branch_format.c and gdx_course_registry.c.
 */

#include "../gdx_course_branch_format.h"
#include "../gdx_course_registry.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int sFailures;

static void check(int condition, const char* label) {
    if (condition) {
        printf("PASS: %s\n", label);
    } else {
        fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

static void put_u16(unsigned char* p, unsigned v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
}

static void put_u32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

static unsigned get_u32(const unsigned char* p) {
    return (unsigned) p[0] | ((unsigned) p[1] << 8) | ((unsigned) p[2] << 16) | ((unsigned) p[3] << 24);
}

static void set_bits(float* dst, uint32_t bits) {
    memcpy(dst, &bits, sizeof(bits));
}

static int same_float(float a, float b) {
    uint32_t ba;
    uint32_t bb;

    memcpy(&ba, &a, sizeof(ba));
    memcpy(&bb, &b, sizeof(bb));
    return ba == bb;
}

static int set_is_zero(const GdxBranchSet* set) {
    GdxBranchSet zero;

    memset(&zero, 0, sizeof(zero));
    return memcmp(set, &zero, sizeof(zero)) == 0;
}

static void fill_node(GdxBranchNode* node, float x) {
    memset(node, 0, sizeof(*node));
    node->pos[0] = x;
    node->pos[1] = 2.5f;
    set_bits(&node->pos[2], 0x80000000u); /* -0, must survive a round trip as bits */
    node->radiusLeft = -300;
    node->radiusRight = 400;
    node->trackSegmentInfo = -123456;
    node->bankAngle = -200;
    node->pit = -1;
    node->dash = 2;
    node->dirt = 3;
    node->ice = 4;
    node->jump = 5;
    node->landmine = 6;
    node->gate = 7;
    node->building = 8;
    node->sign = 9;
}

static void add_fork(GdxBranchSet* set, int forkPoint, int mergePoint, int flags, int nodes) {
    GdxBranchFork* fork;
    int i;

    fork = &set->forks[set->forkCount];
    memset(fork, 0, sizeof(*fork));
    fork->forkPoint = (uint8_t) forkPoint;
    fork->mergePoint = (uint8_t) mergePoint;
    fork->flags = (uint8_t) flags;
    fork->nodeCount = (uint8_t) nodes;
    for (i = 0; i < nodes && i < GDX_BRANCH_MAX_NODES; i++) {
        fill_node(&fork->nodes[i], (float) (i + 1));
    }
    set->forkCount++;
}

/* Symmetric fork at 1..4 (path A length 2) meeting an asymmetric fork at 4..8. */
static void make_basic(GdxBranchSet* set) {
    memset(set, 0, sizeof(*set));
    set->courseChecksum = 0xA1B2C3D4u;
    set->ringCount = 20;
    add_fork(set, 1, 4, GDX_BRANCH_FLAG_SYMMETRIC, 2);
    add_fork(set, 4, 8, 0, 1);
}

static int node_eq(const GdxBranchNode* a, const GdxBranchNode* b) {
    return same_float(a->pos[0], b->pos[0]) && same_float(a->pos[1], b->pos[1]) && same_float(a->pos[2], b->pos[2]) &&
           a->radiusLeft == b->radiusLeft && a->radiusRight == b->radiusRight &&
           a->trackSegmentInfo == b->trackSegmentInfo && a->bankAngle == b->bankAngle && a->pit == b->pit &&
           a->dash == b->dash && a->dirt == b->dirt && a->ice == b->ice && a->jump == b->jump &&
           a->landmine == b->landmine && a->gate == b->gate && a->building == b->building && a->sign == b->sign;
}

static int set_eq(const GdxBranchSet* a, const GdxBranchSet* b) {
    int i;
    int j;

    if (a->courseChecksum != b->courseChecksum || a->ringCount != b->ringCount || a->forkCount != b->forkCount) {
        return 0;
    }
    for (i = 0; i < (int) a->forkCount; i++) {
        if (a->forks[i].forkPoint != b->forks[i].forkPoint || a->forks[i].mergePoint != b->forks[i].mergePoint ||
            a->forks[i].flags != b->forks[i].flags || a->forks[i].nodeCount != b->forks[i].nodeCount) {
            return 0;
        }
        for (j = 0; j < (int) a->forks[i].nodeCount; j++) {
            if (!node_eq(&a->forks[i].nodes[j], &b->forks[i].nodes[j])) {
                return 0;
            }
        }
    }
    return 1;
}

static void expect_code(const char* label, const GdxBranchSet* set, int ring, int code, const char* word) {
    char reason[96];
    int rc;

    memset(reason, 'x', sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = '\0';
    rc = gdx_branch_validate(set, ring, reason, sizeof(reason));
    check(rc == code && reason[0] != '\0' && strstr(reason, word) != NULL, label);
}

/* ---------------------------------------------------------------------------------
 * Limits and S71.
 * ------------------------------------------------------------------------------- */

static void test_limits(void) {
    check(GDX_BRANCH_MAX_FORKS == 4, "fork cap is 4");
    check(GDX_BRANCH_MAX_NODES == 32, "node cap is 32");
    check(GDX_BRANCH_MAX_TOTAL_POINTS == 64, "total point cap is 64");
    check(GDX_BRANCH_FLAG_SYMMETRIC == 0x01u, "symmetric is flag bit0");
    check(GDX_BRANCH_NODE_BYTES == 32, "wire node is 32 bytes");
    check(GDX_BRANCH_COURSE_ID_MAX == GDX_COURSE_FILE_ID_MAX, "course id max matches the registry");
    check(GDX_BRANCH_STORE_SLOTS == GDX_COURSE_STORE_SLOTS, "store slots match the registry");
    check(GDX_BRANCH_BLOCK_MAX_BYTES == 16 + 4 * (4 + 32 * 32) + 4, "block max matches the wire formula");
    check(GDX_BRANCH_BLOCK_MAX_BYTES <= 65535, "a GXB1 block fits in the GBS1 u16 length");
    check(GDX_BRANCH_FILE_MAX_BYTES ==
              12 + 128 * (1 + 25 + 2 + GDX_BRANCH_BLOCK_MAX_BYTES) + 4,
          "file max matches the wire formula");
}

static void test_validate_accept(void) {
    GdxBranchSet set;
    GdxBranchNode poisoned;
    char reason[96];

    make_basic(&set);
    memset(reason, 'x', sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = '\0';
    check(gdx_branch_validate(&set, 20, reason, sizeof(reason)) == GDX_BRANCH_OK, "a meeting symmetric pair is valid");
    check(reason[0] == '\0', "a valid set clears the reason");
    check(gdx_branch_validate(&set, 20, NULL, 0) == GDX_BRANCH_OK, "a NULL reason does not crash");

    /* Asymmetric nodeCount need not equal the path A length (8 - 4 - 1 = 3, stored 1). */
    check(set.forks[1].nodeCount == 1, "the asymmetric fork keeps its own node count");

    memset(&set, 0, sizeof(set));
    set.ringCount = 62;
    add_fork(&set, 1, 4, 0, 2);
    check(gdx_branch_validate(&set, 62, NULL, 0) == GDX_BRANCH_OK, "ring points plus nodes may equal 64");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 1, 2, 0, 1);
    add_fork(&set, 2, 3, 0, 1);
    add_fork(&set, 3, 4, 0, 1);
    add_fork(&set, 4, 5, 0, 1);
    check(gdx_branch_validate(&set, 12, NULL, 0) == GDX_BRANCH_OK, "four forks that meet at endpoints are valid");

    memset(&set, 0, sizeof(set));
    check(gdx_branch_validate(&set, 0, NULL, 0) == GDX_BRANCH_OK, "an empty set on an empty ring is valid");
    set.ringCount = 64;
    check(gdx_branch_validate(&set, 64, NULL, 0) == GDX_BRANCH_OK, "an empty set on a full ring is valid");

    make_basic(&set);
    set_bits(&poisoned.pos[0], 0x7FC00000u);
    set.forks[0].nodes[2] = poisoned;
    set.forks[2].forkPoint = 9;
    check(gdx_branch_validate(&set, 20, NULL, 0) == GDX_BRANCH_OK,
          "unused nodes and unused fork slots are not validated");
}

static void test_validate_reject(void) {
    GdxBranchSet set;

    make_basic(&set);
    set.forkCount = 5;
    expect_code("forkCount above 4 is rejected", &set, 20, GDX_BRANCH_ERR_COUNT, "fork count");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 0, 3, 0, 1);
    expect_code("a fork on ring point 0 is rejected", &set, 10, GDX_BRANCH_ERR_SPAN, "start line");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 4, 4, 0, 1);
    expect_code("a fork point that is not before the merge is rejected", &set, 10, GDX_BRANCH_ERR_SPAN, "before");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 8, 3, 0, 1);
    expect_code("a fork that wraps the ring is rejected", &set, 10, GDX_BRANCH_ERR_SPAN, "wrap");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 10, 0, 1);
    expect_code("a merge point past the last ring point is rejected", &set, 10, GDX_BRANCH_ERR_SPAN, "past the ring");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, 0, 1);
    set.forks[0].nodeCount = 0;
    expect_code("a node count of 0 is rejected", &set, 10, GDX_BRANCH_ERR_COUNT, "node count");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, 0, 1);
    set.forks[0].nodeCount = 33;
    expect_code("a node count above 32 is rejected", &set, 10, GDX_BRANCH_ERR_COUNT, "node count");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 1, 6, 0, 1);
    add_fork(&set, 4, 8, 0, 1);
    expect_code("overlapping forks are rejected", &set, 12, GDX_BRANCH_ERR_OVERLAP, "overlap");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 5, 0, 1);
    add_fork(&set, 2, 8, 0, 1);
    expect_code("forks that share a fork point are rejected", &set, 12, GDX_BRANCH_ERR_OVERLAP, "overlap");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 6, 8, 0, 1);
    add_fork(&set, 1, 3, 0, 1);
    expect_code("forks that are not sorted by fork point are rejected", &set, 12, GDX_BRANCH_ERR_OVERLAP, "sorted");

    memset(&set, 0, sizeof(set));
    set.ringCount = 62;
    add_fork(&set, 1, 4, 0, 3);
    expect_code("ring points plus nodes above 64 are rejected", &set, 62, GDX_BRANCH_ERR_TOTAL, "exceed");

    memset(&set, 0, sizeof(set));
    set.ringCount = 65;
    expect_code("a ring longer than 64 is rejected", &set, 65, GDX_BRANCH_ERR_TOTAL, "exceed");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, GDX_BRANCH_FLAG_SYMMETRIC, 1);
    expect_code("a symmetric fork whose node count is not path A is rejected", &set, 10, GDX_BRANCH_ERR_SYMMETRIC,
                "path A");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, 0, 1);
    set_bits(&set.forks[0].nodes[0].pos[1], 0x7FC00000u);
    expect_code("a NaN position is rejected", &set, 10, GDX_BRANCH_ERR_FLOAT, "finite");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, 0, 1);
    set_bits(&set.forks[0].nodes[0].pos[0], 0x7F800000u);
    expect_code("a positive infinity position is rejected", &set, 10, GDX_BRANCH_ERR_FLOAT, "finite");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, 0, 1);
    set_bits(&set.forks[0].nodes[0].pos[2], 0xFF800000u);
    expect_code("a negative infinity position is rejected", &set, 10, GDX_BRANCH_ERR_FLOAT, "finite");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, 0x02, 1);
    expect_code("an unknown flag bit is rejected", &set, 10, GDX_BRANCH_ERR_FLAGS, "unknown");

    memset(&set, 0, sizeof(set));
    set.ringCount = 10;
    add_fork(&set, 1, 4, 0x03, 1);
    expect_code("symmetric plus an unknown flag bit is rejected", &set, 10, GDX_BRANCH_ERR_FLAGS, "unknown");

    make_basic(&set);
    expect_code("a negative ring count is rejected", &set, -1, GDX_BRANCH_ERR_RING, "negative");
    expect_code("a ring count that does not match the set is rejected", &set, 19, GDX_BRANCH_ERR_RING, "match");
    expect_code("a null set is rejected", NULL, 20, GDX_BRANCH_ERR_ARGS, "null");
}

/* ---------------------------------------------------------------------------------
 * Index maintenance and labels.
 * ------------------------------------------------------------------------------- */

static void test_ring_insert_delete(void) {
    GdxBranchSet set;
    GdxBranchSet other;

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);

    check(gdx_branch_on_ring_insert(&set, 0) == 0, "insert removes no forks");
    check(set.forks[0].forkPoint == 3 && set.forks[0].mergePoint == 7, "insert before the fork shifts both ends");
    check(set.forks[0].flags == GDX_BRANCH_FLAG_SYMMETRIC && set.forks[0].nodeCount == 3,
          "insert before the fork keeps symmetry and the node count");
    check(set.ringCount == 12, "insert does not edit ringCount");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    check(gdx_branch_on_ring_insert(&set, 2) == 0, "insert at the fork point removes nothing");
    check(set.forks[0].forkPoint == 3 && set.forks[0].mergePoint == 7, "insert at the fork point shifts both ends");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    check(gdx_branch_on_ring_insert(&set, 4) == 0, "insert inside path A removes nothing");
    check(set.forks[0].forkPoint == 2 && set.forks[0].mergePoint == 7, "insert inside path A shifts only the merge");
    check(set.forks[0].flags == GDX_BRANCH_FLAG_SYMMETRIC && set.forks[0].nodeCount == 3,
          "insert inside a symmetric path A only shifts; the caller pairs the node");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    gdx_branch_on_ring_insert(&set, 6);
    check(set.forks[0].forkPoint == 2 && set.forks[0].mergePoint == 7, "insert at the merge shifts only the merge");
    check(set.forks[0].flags == GDX_BRANCH_FLAG_SYMMETRIC, "insert at the merge does not clear symmetry");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    gdx_branch_on_ring_insert(&set, 9);
    check(set.forks[0].forkPoint == 2 && set.forks[0].mergePoint == 6, "insert after the fork shifts nothing");

    other = set;
    check(gdx_branch_on_ring_insert(NULL, 1) == 0, "insert on a null set removes nothing");
    check(gdx_branch_on_ring_insert(&set, -1) == 0 && set.forks[0].forkPoint == other.forks[0].forkPoint,
          "a negative insert index changes nothing");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    add_fork(&set, 8, 10, 0, 1);
    check(gdx_branch_on_ring_delete(&set, 2) == 1, "deleting the fork point removes that fork");
    check(set.forkCount == 1 && set.forks[0].forkPoint == 7 && set.forks[0].mergePoint == 9,
          "the surviving fork shifts down");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    check(gdx_branch_on_ring_delete(&set, 6) == 1 && set.forkCount == 0, "deleting the merge point removes the fork");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    check(gdx_branch_on_ring_delete(&set, 4) == 0, "deleting a path A point removes no fork");
    check(set.forkCount == 1 && set.forks[0].forkPoint == 2 && set.forks[0].mergePoint == 5,
          "deleting a path A point shifts the merge down");
    check(set.forks[0].flags == 0 && set.forks[0].nodeCount == 3,
          "a symmetric fork that loses a path A point loses the flag and keeps its nodes");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, 0, 1);
    gdx_branch_on_ring_delete(&set, 4);
    check(set.forks[0].flags == 0 && set.forks[0].mergePoint == 5, "an asymmetric path A delete only shifts");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    gdx_branch_on_ring_delete(&set, 1);
    check(set.forks[0].forkPoint == 1 && set.forks[0].mergePoint == 5 &&
              set.forks[0].flags == GDX_BRANCH_FLAG_SYMMETRIC,
          "delete before the fork shifts both ends and keeps symmetry");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    gdx_branch_on_ring_delete(&set, 8);
    check(set.forks[0].forkPoint == 2 && set.forks[0].mergePoint == 6, "delete after the fork changes nothing");

    memset(&set, 0, sizeof(set));
    set.ringCount = 12;
    add_fork(&set, 2, 4, 0, 1);
    add_fork(&set, 4, 8, 0, 1);
    check(gdx_branch_on_ring_delete(&set, 4) == 2 && set.forkCount == 0,
          "deleting a shared endpoint removes both forks");

    check(gdx_branch_on_ring_delete(NULL, 1) == 0, "delete on a null set removes nothing");
    check(gdx_branch_on_ring_delete(&set, -1) == 0, "a negative delete index removes nothing");
}

static void test_nodes_and_labels(void) {
    GdxBranchSet set;
    GdxBranchNode extra;
    GdxBranchFork full;
    char label[16];
    int i;

    make_basic(&set);
    check(gdx_branch_find_fork(&set, 1) == 0, "the fork point is inside the closed span");
    check(gdx_branch_find_fork(&set, 3) == 0, "a path A point finds its fork");
    check(gdx_branch_find_fork(&set, 4) == 0, "a shared endpoint returns the earlier fork");
    check(gdx_branch_find_fork(&set, 8) == 1, "the later merge point finds the later fork");
    check(gdx_branch_find_fork(&set, 0) == -1, "ring point 0 is in no fork");
    check(gdx_branch_find_fork(&set, 9) == -1, "a point past the merge is in no fork");
    check(gdx_branch_find_fork(NULL, 1) == -1, "find_fork on a null set is -1");
    check(gdx_branch_find_fork(&set, -1) == -1, "a negative ring point finds no fork");

    fill_node(&extra, 9.0f);
    check(gdx_branch_insert_node(&set.forks[1], 0, &extra) == GDX_BRANCH_OK, "insert node at the front");
    check(set.forks[1].nodeCount == 2 && same_float(set.forks[1].nodes[0].pos[0], 9.0f) &&
              same_float(set.forks[1].nodes[1].pos[0], 1.0f),
          "insert node shifts the old node up");
    check(gdx_branch_insert_node(&set.forks[1], 2, &extra) == GDX_BRANCH_OK && set.forks[1].nodeCount == 3,
          "insert node appends");
    check(gdx_branch_delete_node(&set.forks[1], 1) == GDX_BRANCH_OK && set.forks[1].nodeCount == 2 &&
              same_float(set.forks[1].nodes[1].pos[0], 9.0f),
          "delete node shifts the tail down");
    check(gdx_branch_delete_node(&set.forks[1], 0) == GDX_BRANCH_OK && set.forks[1].nodeCount == 1,
          "delete node may leave one node");
    check(gdx_branch_delete_node(&set.forks[1], 0) == GDX_BRANCH_ERR_COUNT && set.forks[1].nodeCount == 1,
          "delete node refuses the last node");
    check(gdx_branch_insert_node(NULL, 0, &extra) == GDX_BRANCH_ERR_ARGS, "insert node refuses a null fork");
    check(gdx_branch_insert_node(&set.forks[1], 0, NULL) == GDX_BRANCH_ERR_ARGS, "insert node refuses a null node");
    check(gdx_branch_insert_node(&set.forks[1], 3, &extra) == GDX_BRANCH_ERR_INDEX, "insert node refuses a gap");
    check(gdx_branch_delete_node(&set.forks[1], -1) == GDX_BRANCH_ERR_INDEX, "delete node refuses a negative index");
    check(gdx_branch_delete_node(NULL, 0) == GDX_BRANCH_ERR_ARGS, "delete node refuses a null fork");

    memset(&full, 0, sizeof(full));
    full.nodeCount = GDX_BRANCH_MAX_NODES;
    check(gdx_branch_insert_node(&full, 0, &extra) == GDX_BRANCH_ERR_FULL, "insert node refuses a 33rd node");

    make_basic(&set);
    check(gdx_branch_label_ring(&set, 0, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "0") == 0,
          "ring point 0 is labeled with its index");
    check(gdx_branch_label_ring(&set, 1, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "1") == 0,
          "a fork endpoint is not a path A label");
    check(gdx_branch_label_ring(&set, 3, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "3-A") == 0,
          "a path A point is labeled n-A");
    check(gdx_branch_label_ring(&set, 4, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "4") == 0,
          "a shared endpoint is not labeled as path A");
    check(gdx_branch_label_ring(&set, 6, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "6-A") == 0,
          "the second fork's path A is labeled n-A");
    check(gdx_branch_label_ring(&set, 9, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "9") == 0,
          "a point outside every fork is labeled with its index");
    check(gdx_branch_label_node(&set, 0, 0, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "2-B") == 0,
          "B node 0 is labeled forkPoint+1");
    check(gdx_branch_label_node(&set, 0, 1, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "3-B") == 0,
          "B node 1 is labeled forkPoint+2");
    check(gdx_branch_label_node(&set, 1, 0, label, sizeof(label)) == GDX_BRANCH_OK && strcmp(label, "5-B") == 0,
          "a later fork's B node uses that fork's fork point");

    check(gdx_branch_label_ring(&set, 3, label, 3) == GDX_BRANCH_ERR_FULL && label[0] == '\0',
          "a path A label that does not fit is refused");
    check(gdx_branch_label_ring(&set, 3, label, 4) == GDX_BRANCH_OK && strcmp(label, "3-A") == 0,
          "a path A label fits in 4 bytes including the NUL");
    check(gdx_branch_label_ring(NULL, 1, label, sizeof(label)) == GDX_BRANCH_ERR_ARGS, "label_ring refuses a null set");
    check(gdx_branch_label_ring(&set, -1, label, sizeof(label)) == GDX_BRANCH_ERR_ARGS,
          "label_ring refuses a negative index");
    check(gdx_branch_label_node(&set, 2, 0, label, sizeof(label)) == GDX_BRANCH_ERR_INDEX,
          "label_node refuses a fork index past forkCount");
    check(gdx_branch_label_node(&set, 0, 2, label, sizeof(label)) == GDX_BRANCH_ERR_INDEX,
          "label_node refuses a node index past nodeCount");
    check(gdx_branch_label_node(&set, 0, 0, NULL, 4) == GDX_BRANCH_ERR_ARGS, "label_node refuses a null buffer");

    memset(&set, 0, sizeof(set));
    set.ringCount = 40;
    add_fork(&set, 10, 20, 0, 1);
    for (i = 0; i < 32; i++) {
        fill_node(&extra, (float) i);
        if (i == 0) {
            set.forks[0].nodes[0] = extra;
        } else {
            check(gdx_branch_insert_node(&set.forks[0], i, &extra) == GDX_BRANCH_OK, "filling a fork stays in range");
        }
    }
    check(set.forks[0].nodeCount == 32, "a fork holds 32 nodes");
    check(gdx_branch_insert_node(&set.forks[0], 32, &extra) == GDX_BRANCH_ERR_FULL, "the 33rd node is refused");
}

/* ---------------------------------------------------------------------------------
 * GXB1.
 * ------------------------------------------------------------------------------- */

static unsigned char sBlock[GDX_BRANCH_BLOCK_MAX_BYTES];
static unsigned char sFile[GDX_BRANCH_FILE_MAX_BYTES];

static void reseal(unsigned char* buf, size_t len) {
    put_u32(buf + len - 4, gdx_course_registry_crc32(buf, len - 4));
}

static size_t craft_block(unsigned char* buf, size_t cap, unsigned forkCount, unsigned ringCount, uint32_t bind,
                          uint32_t checksum, const unsigned char* body, size_t bodyLen) {
    size_t total = 16 + bodyLen + 4;

    if (total > cap) {
        return 0;
    }
    memset(buf, 0, total);
    buf[0] = 'G';
    buf[1] = 'X';
    buf[2] = 'B';
    buf[3] = '1';
    put_u16(buf + 4, 1);
    buf[6] = (unsigned char) forkCount;
    buf[7] = (unsigned char) ringCount;
    put_u32(buf + 8, bind);
    put_u32(buf + 12, checksum);
    if (bodyLen > 0 && body != NULL) {
        memcpy(buf + 16, body, bodyLen);
    }
    put_u32(buf + 16 + bodyLen, gdx_course_registry_crc32(buf, 16 + bodyLen));
    return total;
}

static void expect_decode(const char* label, const unsigned char* data, size_t len, uint32_t bind, int code) {
    GdxBranchSet out;

    memset(&out, 0xA5, sizeof(out));
    check(gdx_branch_decode(data, len, bind, &out) == code && set_is_zero(&out), label);
}

static void test_block_round_trip(void) {
    GdxBranchSet set;
    GdxBranchSet out;
    GdxBranchNode hand;
    unsigned char body[4 + 32];
    const uint32_t bind = 0x01020304u;
    int len;
    int again;
    size_t crafted;

    make_basic(&set);
    set_bits(&set.forks[0].nodes[2].pos[0], 0x7FC00000u);
    memset(sBlock, 0x5A, sizeof(sBlock));
    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    check(len == 16 + (4 + 32 * 2) + (4 + 32) + 4, "encode length is the hand-packed size, not the struct size");
    check(sBlock[0] == 'G' && sBlock[1] == 'X' && sBlock[2] == 'B' && sBlock[3] == '1', "magic is GXB1");
    check(sBlock[4] == 1 && sBlock[5] == 0 && sBlock[6] == 2 && sBlock[7] == 20, "version, forkCount, ringCount");
    check(get_u32(sBlock + 8) == bind, "bindCrc is stored little-endian");
    check(get_u32(sBlock + 12) == 0xA1B2C3D4u, "courseChecksum is stored little-endian");
    check(get_u32(sBlock + (size_t) len - 4) == gdx_course_registry_crc32(sBlock, (size_t) len - 4),
          "the trailing CRC-32 covers every earlier byte");

    memset(&out, 0xA5, sizeof(out));
    check(gdx_branch_decode(sBlock, (size_t) len, bind, &out) == GDX_BRANCH_OK, "decode accepts the encoded block");
    check(set_eq(&set, &out), "round trip keeps checksum, spans, flags, and node fields");
    check(out.forks[0].nodes[2].pos[0] == 0.0f && out.forks[2].forkPoint == 0,
          "unused nodes and unused forks are not in the block");
    again = gdx_branch_encode(&out, bind, sBlock + 2000, sizeof(sBlock) - 2000);
    check(again == len && memcmp(sBlock, sBlock + 2000, (size_t) len) == 0, "encode is deterministic");

    memset(sBlock, 0x5A, 64);
    set.forks[0].forkPoint = 0;
    check(gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock)) == GDX_BRANCH_ERR_SPAN && sBlock[0] == 0x5A,
          "encode of an invalid set writes nothing");
    make_basic(&set);
    memset(sBlock, 0x5A, 64);
    check(gdx_branch_encode(&set, bind, sBlock, 8) == GDX_BRANCH_ERR_FULL && sBlock[0] == 0x5A,
          "encode into a short buffer writes nothing");
    check(gdx_branch_encode(NULL, bind, sBlock, sizeof(sBlock)) == GDX_BRANCH_ERR_ARGS, "encode refuses a null set");
    check(gdx_branch_encode(&set, bind, NULL, sizeof(sBlock)) == GDX_BRANCH_ERR_ARGS, "encode refuses a null buffer");

    memset(&set, 0, sizeof(set));
    set.ringCount = 8;
    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    check(len == 20 && gdx_branch_decode(sBlock, (size_t) len, bind, &out) == GDX_BRANCH_OK && out.forkCount == 0,
          "an empty set round trips as a 20 byte block");

    /* A block built without encode, so a shared pack/unpack bug cannot hide. */
    memset(&hand, 0, sizeof(hand));
    set_bits(&hand.pos[0], 0x3F800000u);
    set_bits(&hand.pos[1], 0xC0000000u);
    set_bits(&hand.pos[2], 0x3F000000u);
    hand.radiusLeft = -300;
    hand.radiusRight = 400;
    hand.trackSegmentInfo = -123456;
    hand.bankAngle = -200;
    hand.pit = -1;
    hand.dash = 2;
    hand.dirt = 3;
    hand.ice = 4;
    hand.jump = 5;
    hand.landmine = 6;
    hand.gate = 7;
    hand.building = 8;
    hand.sign = 9;
    body[0] = 1;
    body[1] = 4;
    body[2] = 0;
    body[3] = 1;
    put_u32(body + 4, 0x3F800000u);
    put_u32(body + 8, 0xC0000000u);
    put_u32(body + 12, 0x3F000000u);
    put_u16(body + 16, 0xFED4u);
    put_u16(body + 18, 400);
    put_u32(body + 20, 0xFFFE1DC0u);
    put_u16(body + 24, 0xFF38u);
    body[26] = 0xFF;
    body[27] = 2;
    body[28] = 3;
    body[29] = 4;
    body[30] = 5;
    body[31] = 6;
    body[32] = 7;
    body[33] = 8;
    body[34] = 9;
    body[35] = 0;
    crafted = craft_block(sBlock, sizeof(sBlock), 1, 10, bind, 0x11u, body, sizeof(body));
    check(crafted != 0 && gdx_branch_decode(sBlock, crafted, bind, &out) == GDX_BRANCH_OK, "a hand-packed node decodes");
    check(out.forkCount == 1 && out.ringCount == 10 && out.courseChecksum == 0x11u && node_eq(&out.forks[0].nodes[0], &hand),
          "hand-packed fields match, including negative integers");
}

static void test_block_reject(void) {
    GdxBranchSet set;
    unsigned char tiny[8];
    unsigned char shortBody[2];
    unsigned char countBody[4];
    const uint32_t bind = 0x01020304u;
    int len;
    size_t crafted;
    size_t i;

    make_basic(&set);
    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    check(len > 20, "reject tests start from a real block");

    memset(tiny, 0, sizeof(tiny));
    expect_decode("truncated: empty buffer", tiny, 0, bind, GDX_BRANCH_ERR_TRUNCATED);
    expect_decode("truncated: one byte short of the minimum", sBlock, 19, bind, GDX_BRANCH_ERR_TRUNCATED);
    expect_decode("null data", NULL, 32, bind, GDX_BRANCH_ERR_ARGS);
    check(gdx_branch_decode(sBlock, (size_t) len, bind, NULL) == GDX_BRANCH_ERR_ARGS, "decode refuses a null out");

    sBlock[0] = 'X';
    expect_decode("magic", sBlock, (size_t) len, bind, GDX_BRANCH_ERR_MAGIC);
    sBlock[0] = 'G';

    sBlock[4] = 2;
    reseal(sBlock, (size_t) len);
    expect_decode("version", sBlock, (size_t) len, bind, GDX_BRANCH_ERR_VERSION);
    sBlock[4] = 1;
    reseal(sBlock, (size_t) len);

    sBlock[16] ^= 0x01;
    expect_decode("CRC", sBlock, (size_t) len, bind, GDX_BRANCH_ERR_CRC);
    sBlock[16] ^= 0x01;
    expect_decode("binding", sBlock, (size_t) len, bind + 1u, GDX_BRANCH_ERR_BINDING);
    expect_decode("a longer buffer is not a block", sBlock, (size_t) len + 1, bind, GDX_BRANCH_ERR_CRC);

    shortBody[0] = 1;
    shortBody[1] = 4;
    crafted = craft_block(sBlock, sizeof(sBlock), 1, 10, bind, 1u, shortBody, sizeof(shortBody));
    expect_decode("layout: fork header is short", sBlock, crafted, bind, GDX_BRANCH_ERR_LAYOUT);

    crafted = craft_block(sBlock, sizeof(sBlock), 5, 10, bind, 1u, NULL, 0);
    expect_decode("layout: forkCount above 4", sBlock, crafted, bind, GDX_BRANCH_ERR_LAYOUT);

    countBody[0] = 1;
    countBody[1] = 4;
    countBody[2] = 0;
    countBody[3] = 33;
    crafted = craft_block(sBlock, sizeof(sBlock), 1, 10, bind, 1u, countBody, sizeof(countBody));
    expect_decode("layout: nodeCount above 32", sBlock, crafted, bind, GDX_BRANCH_ERR_LAYOUT);

    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    memmove(sBlock + (size_t) len - 3, sBlock + (size_t) len - 4, 4);
    sBlock[(size_t) len - 4] = 0x00;
    reseal(sBlock, (size_t) len + 1);
    expect_decode("layout: a stray byte before the CRC", sBlock, (size_t) len + 1, bind, GDX_BRANCH_ERR_LAYOUT);

    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    sBlock[16 + 4 + 31] = 1;
    reseal(sBlock, (size_t) len);
    expect_decode("layout: pad byte is not 0", sBlock, (size_t) len, bind, GDX_BRANCH_ERR_LAYOUT);

    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    sBlock[16] = 0;
    reseal(sBlock, (size_t) len);
    expect_decode("invalid set: fork point 0", sBlock, (size_t) len, bind, GDX_BRANCH_ERR_SPAN);

    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    put_u32(sBlock + 16 + 4, 0x7FC00000u);
    reseal(sBlock, (size_t) len);
    expect_decode("invalid set: NaN position", sBlock, (size_t) len, bind, GDX_BRANCH_ERR_FLOAT);

    countBody[3] = 0;
    crafted = craft_block(sBlock, sizeof(sBlock), 1, 10, bind, 1u, countBody, sizeof(countBody));
    expect_decode("invalid set: nodeCount 0 is a complete record that fails S71", sBlock, crafted, bind,
                  GDX_BRANCH_ERR_COUNT);

    /* Every rejection above zeroed out. One more explicit pass over a damaged CRC. */
    len = gdx_branch_encode(&set, bind, sBlock, sizeof(sBlock));
    for (i = 0; i < (size_t) len; i++) {
        sBlock[i] ^= 0xFF;
    }
    expect_decode("a fully flipped block is rejected and out is zero", sBlock, (size_t) len, bind,
                  GDX_BRANCH_ERR_MAGIC);
}

/* ---------------------------------------------------------------------------------
 * Store and GBS1.
 * ------------------------------------------------------------------------------- */

static size_t craft_file(unsigned char* buf, size_t cap, unsigned count, const unsigned char* body, size_t bodyLen) {
    size_t total = 12 + bodyLen + 4;

    if (total > cap) {
        return 0;
    }
    memset(buf, 0, total);
    buf[0] = 'G';
    buf[1] = 'B';
    buf[2] = 'S';
    buf[3] = '1';
    put_u16(buf + 4, 1);
    put_u16(buf + 6, count);
    put_u32(buf + 8, (unsigned) total);
    if (bodyLen > 0 && body != NULL) {
        memcpy(buf + 12, body, bodyLen);
    }
    put_u32(buf + 12 + bodyLen, gdx_course_registry_crc32(buf, 12 + bodyLen));
    return total;
}

static size_t append_entry(unsigned char* dst, size_t cap, size_t used, const char* id, const unsigned char* block,
                           size_t blockLen) {
    size_t idLen = strlen(id);
    size_t need = 1 + idLen + 2 + blockLen;

    if (used + need > cap) {
        return 0;
    }
    dst[used] = (unsigned char) idLen;
    memcpy(dst + used + 1, id, idLen);
    put_u16(dst + used + 1 + idLen, (unsigned) blockLen);
    memcpy(dst + used + 1 + idLen + 2, block, blockLen);
    return used + need;
}

static void load_keep(void) {
    GdxBranchSet set;

    gdx_branch_store_clear();
    make_basic(&set);
    check(gdx_branch_store_set("KEEP.CRSD", &set) == GDX_BRANCH_OK, "sentinel course is stored");
}

static void expect_file_reject(const char* label, const unsigned char* data, size_t len, int code) {
    unsigned long rev;
    GdxBranchSet got;
    int rc;

    rev = gdx_branch_store_revision();
    memset(&got, 0, sizeof(got));
    rc = gdx_branch_store_restore_file(data, len);
    check(rc == code && gdx_branch_store_revision() == rev && gdx_branch_store_get("KEEP.CRSD", &got) == 1 &&
              got.courseChecksum == 0xA1B2C3D4u,
          label);
}

static void test_store(void) {
    GdxBranchSet set;
    GdxBranchSet got;
    GdxBranchSet active;
    const GdxBranchSet* live;
    unsigned long rev;
    char id[16];
    char longId[GDX_BRANCH_COURSE_ID_MAX + 1];
    char tooLong[GDX_BRANCH_COURSE_ID_MAX + 2];
    int i;

    gdx_branch_store_clear();
    make_basic(&set);
    rev = gdx_branch_store_revision();
    check(gdx_branch_store_set(NULL, &set) == GDX_BRANCH_ERR_ARGS, "store_set refuses a null id");
    check(gdx_branch_store_set("TRACK.CRSD", NULL) == GDX_BRANCH_ERR_ARGS, "store_set refuses a null set");
    check(gdx_branch_store_set("", &set) == GDX_BRANCH_ERR_ARGS, "store_set refuses an empty id");
    memset(tooLong, 'A', sizeof(tooLong) - 1);
    tooLong[sizeof(tooLong) - 1] = '\0';
    check(gdx_branch_store_set(tooLong, &set) == GDX_BRANCH_ERR_ARGS, "store_set refuses a 26 character id");
    check(gdx_branch_store_set("BAD\tID.CRSD", &set) == GDX_BRANCH_ERR_ARGS, "store_set refuses a control byte");
    check(gdx_branch_store_revision() == rev, "a refused store_set does not bump the revision");

    memset(longId, 'B', GDX_BRANCH_COURSE_ID_MAX);
    longId[GDX_BRANCH_COURSE_ID_MAX] = '\0';
    check(gdx_branch_store_set(longId, &set) == GDX_BRANCH_OK, "store_set accepts a 25 character id");
    check(gdx_branch_store_get(longId, &got) == 1 && set_eq(&set, &got), "store_get copies the set");
    check(gdx_branch_store_get("MISSING.CRSD", &got) == 0, "store_get misses an unknown id");
    check(gdx_branch_store_get(longId, NULL) == 0, "store_get refuses a null out");

    set.courseChecksum = 0x55u;
    check(gdx_branch_store_set(longId, &set) == GDX_BRANCH_OK && gdx_branch_store_get(longId, &got) == 1 &&
              got.courseChecksum == 0x55u,
          "store_set replaces an existing course");
    set.forkCount = 0;
    check(gdx_branch_store_set(longId, &set) == GDX_BRANCH_OK && gdx_branch_store_get(longId, &got) == 0,
          "forkCount 0 removes the entry");
    rev = gdx_branch_store_revision();
    check(gdx_branch_store_set(longId, &set) == GDX_BRANCH_OK && gdx_branch_store_revision() == rev,
          "removing a missing entry does not bump the revision");

    make_basic(&set);
    set.forks[0].forkPoint = 0;
    rev = gdx_branch_store_revision();
    check(gdx_branch_store_set("TRACK.CRSD", &set) == GDX_BRANCH_ERR_SPAN && gdx_branch_store_revision() == rev &&
              gdx_branch_store_get("TRACK.CRSD", &got) == 0,
          "store_set refuses an invalid set and leaves the store unchanged");

    make_basic(&set);
    check(gdx_branch_store_set("OLD.CRSD", &set) == GDX_BRANCH_OK, "rename source is stored");
    check(gdx_branch_store_rename("OLD.CRSD", "NEW.CRSD") == GDX_BRANCH_OK, "rename moves the set");
    check(gdx_branch_store_get("OLD.CRSD", &got) == 0 && gdx_branch_store_get("NEW.CRSD", &got) == 1 && set_eq(&set, &got),
          "the new id holds the set and the old id is gone");
    rev = gdx_branch_store_revision();
    check(gdx_branch_store_rename("NEW.CRSD", "NEW.CRSD") == GDX_BRANCH_OK && gdx_branch_store_revision() == rev &&
              gdx_branch_store_get("NEW.CRSD", &got) == 1,
          "a same-id rename is a no-op");
    check(gdx_branch_store_rename(NULL, "X.CRSD") == GDX_BRANCH_ERR_ARGS, "rename refuses a null id");
    check(gdx_branch_store_set("OTHER.CRSD", &set) == GDX_BRANCH_OK, "rename destination exists");
    check(gdx_branch_store_rename("GONE.CRSD", "OTHER.CRSD") == GDX_BRANCH_OK &&
              gdx_branch_store_get("OTHER.CRSD", &got) == 0,
          "renaming a missing id clears the destination");

    gdx_branch_store_clear();
    make_basic(&set);
    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        snprintf(id, sizeof(id), "C%03d.CRSD", i);
        if (gdx_branch_store_set(id, &set) != GDX_BRANCH_OK) {
            sFailures++;
            fprintf(stderr, "FAIL: store fill %d\n", i);
            break;
        }
    }
    check(i == GDX_BRANCH_STORE_SLOTS, "the store holds 128 courses");
    check(gdx_branch_store_set("FULL.CRSD", &set) == GDX_BRANCH_ERR_FULL, "the 129th course is refused");
    check(gdx_branch_store_get("C000.CRSD", &got) == 1, "a full store keeps the first course");
    check(gdx_branch_store_get("FULL.CRSD", &got) == 0, "the refused course was not stored");

    make_basic(&active);
    active.courseChecksum = 0x77u;
    gdx_branch_set_active(&active);
    live = gdx_branch_active_set();
    check(live != NULL && live != &active && live->courseChecksum == 0x77u, "set_active copies the set");
    active.courseChecksum = 0;
    check(gdx_branch_active_set()->courseChecksum == 0x77u, "mutating the caller does not change the active set");
    gdx_branch_store_clear();
    check(gdx_branch_active_set() != NULL && gdx_branch_active_set()->courseChecksum == 0x77u,
          "clearing the store does not clear the active set");
    gdx_branch_set_active(NULL);
    check(gdx_branch_active_set() == NULL, "set_active NULL clears the active set");
}

static void test_file_round_trip(void) {
    GdxBranchSet one;
    GdxBranchSet two;
    GdxBranchSet got;
    int len;
    int again;

    gdx_branch_store_clear();
    make_basic(&one);
    one.courseChecksum = 0x111u;
    memset(&two, 0, sizeof(two));
    two.courseChecksum = 0x222u;
    two.ringCount = 16;
    add_fork(&two, 2, 6, GDX_BRANCH_FLAG_SYMMETRIC, 3);
    two.forks[0].nodes[0].pit = -5;
    check(gdx_branch_store_set("ONE.CRSD", &one) == GDX_BRANCH_OK, "file test stores the first course");
    check(gdx_branch_store_set("TWO.CRSD", &two) == GDX_BRANCH_OK, "file test stores the second course");

    len = gdx_branch_store_encode_file(sFile, sizeof(sFile));
    check(len > GDX_BRANCH_FILE_MIN_BYTES, "encode_file writes a GBS1 file");
    check(sFile[0] == 'G' && sFile[1] == 'B' && sFile[2] == 'S' && sFile[3] == '1', "file magic is GBS1");
    check(sFile[4] == 1 && sFile[5] == 0 && sFile[6] == 2 && sFile[7] == 0, "file version 1 and two entries");
    check(get_u32(sFile + 8) == (unsigned) len, "fileLength includes the trailing CRC");
    check(get_u32(sFile + (size_t) len - 4) == gdx_course_registry_crc32(sFile, (size_t) len - 4),
          "file CRC covers every earlier byte");
    again = gdx_branch_store_encode_file(sFile + 8000, sizeof(sFile) - 8000);
    check(again == len && memcmp(sFile, sFile + 8000, (size_t) len) == 0, "encode_file is deterministic");

    gdx_branch_store_clear();
    check(gdx_branch_store_get("ONE.CRSD", &got) == 0, "clear drops the stored courses");
    check(gdx_branch_store_restore_file(sFile, (size_t) len) == GDX_BRANCH_OK, "restore accepts the file");
    check(gdx_branch_store_get("ONE.CRSD", &got) == 1 && set_eq(&got, &one), "restore brings back the first course");
    check(gdx_branch_store_get("TWO.CRSD", &got) == 1 && set_eq(&got, &two), "restore brings back the second course");
    check(gdx_branch_store_get("KEEP.CRSD", &got) == 0, "restore does not invent a course");

    len = gdx_branch_store_encode_file(sFile, 8);
    check(len == GDX_BRANCH_ERR_FULL, "encode_file refuses a short buffer");
    check(gdx_branch_store_encode_file(NULL, sizeof(sFile)) == GDX_BRANCH_ERR_ARGS, "encode_file refuses a null buffer");

    gdx_branch_store_clear();
    len = gdx_branch_store_encode_file(sFile, sizeof(sFile));
    check(len == GDX_BRANCH_FILE_MIN_BYTES, "an empty store encodes as a header and CRC");
    check(gdx_branch_store_set("TEMP.CRSD", &one) == GDX_BRANCH_OK, "empty-file test stores a course");
    check(gdx_branch_store_restore_file(sFile, (size_t) len) == GDX_BRANCH_OK &&
              gdx_branch_store_get("TEMP.CRSD", &got) == 0,
          "restoring an empty file replaces the store");
}

static void test_file_reject(void) {
    GdxBranchSet set;
    unsigned char entry[GDX_BRANCH_BLOCK_MAX_BYTES + 64];
    unsigned char block[GDX_BRANCH_BLOCK_MAX_BYTES];
    unsigned char body[GDX_BRANCH_BLOCK_MAX_BYTES + 128];
    unsigned char rawId[8];
    size_t bodyLen;
    size_t fileLen;
    size_t used;
    int len;
    const uint32_t bind = 0;

    load_keep();
    make_basic(&set);
    len = gdx_branch_encode(&set, gdx_course_registry_crc32((const unsigned char*) "KEEP.CRSD", 9), block,
                            sizeof(block));
    check(len > 0, "file reject tests can encode a block");

    fileLen = craft_file(sFile, sizeof(sFile), 0, NULL, 0);
    sFile[0] = 'X';
    expect_file_reject("GBS1 magic", sFile, fileLen, GDX_BRANCH_FILE_ERR_MAGIC);

    fileLen = craft_file(sFile, sizeof(sFile), 0, NULL, 0);
    sFile[4] = 2;
    reseal(sFile, fileLen);
    expect_file_reject("GBS1 version", sFile, fileLen, GDX_BRANCH_FILE_ERR_VERSION);

    expect_file_reject("GBS1 truncated", sFile, 8, GDX_BRANCH_FILE_ERR_TRUNCATED);
    expect_file_reject("GBS1 null data", NULL, 16, GDX_BRANCH_ERR_ARGS);

    fileLen = craft_file(sFile, sizeof(sFile), 0, NULL, 0);
    sFile[fileLen - 4] ^= 0x01;
    expect_file_reject("GBS1 CRC", sFile, fileLen, GDX_BRANCH_FILE_ERR_CRC);

    fileLen = craft_file(sFile, sizeof(sFile), 0, NULL, 0);
    put_u32(sFile + 8, (unsigned) fileLen + 4);
    expect_file_reject("GBS1 fileLength mismatch", sFile, fileLen, GDX_BRANCH_FILE_ERR_LENGTH);

    fileLen = craft_file(sFile, sizeof(sFile), 129, NULL, 0);
    expect_file_reject("GBS1 entryCount above 128", sFile, fileLen, GDX_BRANCH_FILE_ERR_COUNT);

    rawId[0] = 0;
    fileLen = craft_file(sFile, sizeof(sFile), 1, rawId, 1);
    expect_file_reject("GBS1 id length 0", sFile, fileLen, GDX_BRANCH_FILE_ERR_LENGTH);

    rawId[0] = 4;
    rawId[1] = 'A';
    rawId[2] = 0x01;
    rawId[3] = 'B';
    rawId[4] = 'C';
    put_u16(rawId + 5, 0);
    fileLen = craft_file(sFile, sizeof(sFile), 1, rawId, 7);
    expect_file_reject("GBS1 non-printable id", sFile, fileLen, GDX_BRANCH_FILE_ERR_ENTRY);

    len = gdx_branch_encode(&set, gdx_course_registry_crc32((const unsigned char*) "SAME.CRSD", 9), block,
                            sizeof(block));
    used = append_entry(body, sizeof(body), 0, "SAME.CRSD", block, (size_t) len);
    used = append_entry(body, sizeof(body), used, "SAME.CRSD", block, (size_t) len);
    fileLen = craft_file(sFile, sizeof(sFile), 2, body, used);
    expect_file_reject("GBS1 duplicate course id", sFile, fileLen, GDX_BRANCH_FILE_ERR_ENTRY);

    /* Block bound to 0, not to the CRC of the id. */
    len = gdx_branch_encode(&set, bind, block, sizeof(block));
    used = append_entry(body, sizeof(body), 0, "KEEP.CRSD", block, (size_t) len);
    fileLen = craft_file(sFile, sizeof(sFile), 1, body, used);
    expect_file_reject("GBS1 block bound to the wrong id", sFile, fileLen, GDX_BRANCH_ERR_BINDING);

    len = gdx_branch_encode(&set, gdx_course_registry_crc32((const unsigned char*) "KEEP.CRSD", 9), block,
                            sizeof(block));
    block[16 + 4 + 31] = 7;
    reseal(block, (size_t) len);
    used = append_entry(body, sizeof(body), 0, "KEEP.CRSD", block, (size_t) len);
    fileLen = craft_file(sFile, sizeof(sFile), 1, body, used);
    expect_file_reject("GBS1 nested layout error", sFile, fileLen, GDX_BRANCH_ERR_LAYOUT);

    len = gdx_branch_encode(&set, gdx_course_registry_crc32((const unsigned char*) "KEEP.CRSD", 9), block,
                            sizeof(block));
    block[16] = 0;
    reseal(block, (size_t) len);
    used = append_entry(body, sizeof(body), 0, "KEEP.CRSD", block, (size_t) len);
    fileLen = craft_file(sFile, sizeof(sFile), 1, body, used);
    expect_file_reject("GBS1 nested invalid set", sFile, fileLen, GDX_BRANCH_ERR_SPAN);

    memset(&set, 0, sizeof(set));
    set.ringCount = 8;
    len = gdx_branch_encode(&set, gdx_course_registry_crc32((const unsigned char*) "EMPTY.CRSD", 10), block,
                            sizeof(block));
    used = append_entry(entry, sizeof(entry), 0, "EMPTY.CRSD", block, (size_t) len);
    fileLen = craft_file(sFile, sizeof(sFile), 1, entry, used);
    expect_file_reject("GBS1 rejects a forkless entry", sFile, fileLen, GDX_BRANCH_FILE_ERR_ENTRY);

    bodyLen = append_entry(body, sizeof(body), 0, "KEEP.CRSD", block, 10);
    fileLen = craft_file(sFile, sizeof(sFile), 1, body, bodyLen);
    expect_file_reject("GBS1 nested truncated block", sFile, fileLen, GDX_BRANCH_ERR_TRUNCATED);

    make_basic(&set);
    len = gdx_branch_encode(&set, gdx_course_registry_crc32((const unsigned char*) "ONE.CRSD", 8), block,
                            sizeof(block));
    used = append_entry(body, sizeof(body), 0, "ONE.CRSD", block, (size_t) len);
    fileLen = craft_file(sFile, sizeof(sFile), 2, body, used);
    expect_file_reject("GBS1 entryCount does not match the bytes", sFile, fileLen, GDX_BRANCH_FILE_ERR_LENGTH);
}

int main(void) {
    test_limits();
    test_validate_accept();
    test_validate_reject();
    test_ring_insert_delete();
    test_nodes_and_labels();
    test_block_round_trip();
    test_block_reject();
    test_store();
    test_file_round_trip();
    test_file_reject();

    if (sFailures != 0) {
        fprintf(stderr, "%d course branch test(s) failed\n", sFailures);
        return 1;
    }
    printf("All course branch tests passed\n");
    return 0;
}
