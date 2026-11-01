/* port/tests/gdx_course_branch_edit_tests.c -- standalone tests for gdx_course_branch_edit.c.
 *
 *   clang -std=c11 -Wall -Wextra port/gdx_course_branch_edit.c port/gdx_course_branch_format.c
 *       port/gdx_course_registry.c port/tests/gdx_course_branch_edit_tests.c -Iport -o gdx_branch_edit_tests
 */

#include "../gdx_course_branch_edit.h"
#include "../gdx_course_branch_format.h"

#include <stdio.h>
#include <string.h>

static int sFailures = 0;

#define CHECK(cond, text)                         \
    do {                                          \
        if (cond) {                               \
            printf("PASS: %s\n", text);           \
        } else {                                  \
            printf("FAIL: %s (line %d)\n", text, __LINE__); \
            sFailures++;                          \
        }                                         \
    } while (0)

static GdxBranchEditPoint sRing[GDX_BRANCH_EDIT_MAX_POINTS];
static int sCount;

/* Ring points carry their x as an id: point i sits at x = 1000 * i. */
static void make_ring(int count) {
    int i;

    memset(sRing, 0, sizeof(sRing));
    for (i = 0; i < count; i++) {
        sRing[i].pos[0] = 1000.0f * (float) i;
        sRing[i].radiusLeft = 100;
        sRing[i].radiusRight = 100;
        sRing[i].bankAngle = (short) i;
    }
    sCount = count;
}

/* Inserts a ring point after index `after`, as func_xk2_800E6CA8 does, then calls the hook. */
static void ring_insert(int after, float x) {
    int i;

    for (i = sCount; i > after + 1; i--) {
        sRing[i] = sRing[i - 1];
    }
    memset(&sRing[after + 1], 0, sizeof(sRing[0]));
    sRing[after + 1].pos[0] = x;
    sCount++;
    gdx_branch_edit_on_insert(after + 1, sRing, sCount);
}

static void ring_delete(int index) {
    int i;

    for (i = index; i + 1 < sCount; i++) {
        sRing[i] = sRing[i + 1];
    }
    sCount--;
    gdx_branch_edit_on_delete(index);
}

static const GdxBranchFork* fork_of(int f) {
    const GdxBranchSet* set = gdx_branch_active_set();

    return ((set != NULL) && (f < set->forkCount)) ? &set->forks[f] : NULL;
}

static void test_split_and_swap(void) {
    GdxBranchEditPoint nodeB;
    GdxBranchEditPoint before[GDX_BRANCH_EDIT_MAX_POINTS];
    char reason[96];
    int dropped;
    int count;
    const GdxBranchFork* fork;

    make_ring(8);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    CHECK(gdx_branch_edit_split_check(0, sCount, reason, sizeof(reason)) < 0, "split refuses the start segment");
    CHECK(gdx_branch_edit_split_check(7, sCount, reason, sizeof(reason)) < 0, "split refuses the last segment");
    CHECK(gdx_branch_edit_split_check(3, sCount, reason, sizeof(reason)) == 0, "split accepts segment 3");

    ring_insert(3, 3500.0f);
    memset(&nodeB, 0, sizeof(nodeB));
    nodeB.pos[0] = 3500.0f;
    nodeB.pos[2] = 900.0f;
    CHECK(gdx_branch_edit_split_commit(3, &nodeB) == 0, "split commit");
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->forkPoint == 3) && (fork->mergePoint == 5) && (fork->nodeCount == 1) &&
              (fork->flags & GDX_BRANCH_FLAG_SYMMETRIC),
          "split makes F=3 M=5 with one symmetric node");
    CHECK(gdx_branch_edit_sync(sCount) == 0, "split set survives sync");
    CHECK(gdx_branch_edit_split_check(4, sCount, reason, sizeof(reason)) == 1, "split inside a fork asks for a swap");

    /* Insert inside path A pairs a node. */
    ring_insert(4, 4500.0f);
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->mergePoint == 6) && (fork->nodeCount == 2) && (fork->flags & GDX_BRANCH_FLAG_SYMMETRIC),
          "insert in a symmetric path A adds a B node");
    CHECK((fork != NULL) && (fork->nodes[1].pos[0] == (3500.0f + 4000.0f) * 0.5f), "paired node sits between its neighbors");

    /* Swap to B, swap back: ring and set return. */
    memcpy(before, sRing, sizeof(before));
    count = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    CHECK((count == sCount) && (dropped == 0) && gdx_branch_edit_view_b(), "symmetric swap keeps the count");
    CHECK(sRing[4].pos[2] == 900.0f, "B view puts path B in the ring");
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->nodes[0].pos[0] == 3500.0f) && (fork->nodes[0].pos[2] == 0.0f),
          "B view stores path A as nodes");
    sCount = count;
    count = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    CHECK((count == sCount) && !gdx_branch_edit_view_b() && (memcmp(before, sRing, sizeof(before)) == 0),
          "swapping twice restores the ring");
}

static void test_link_and_asymmetric_swap(void) {
    GdxBranchEditPoint nodeB;
    int dropped;
    int count;
    const GdxBranchFork* fork;

    make_ring(10);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    ring_insert(2, 2500.0f); /* ring 11: F=2, M=4 */
    memset(&nodeB, 0, sizeof(nodeB));
    nodeB.pos[2] = 777.0f;
    gdx_branch_edit_split_commit(2, &nodeB);
    CHECK(gdx_branch_edit_link(0, 2, sCount) != 0, "link refuses a merge at the fork point");
    CHECK((gdx_branch_edit_link(0, 10, sCount) == 0) && (fork_of(0)->mergePoint == 10), "link to the last point");
    gdx_branch_edit_sync(sCount);
    CHECK(gdx_branch_edit_link(0, 11, sCount) != 0, "link refuses a merge past the ring");
    CHECK(gdx_branch_edit_link(0, 7, sCount) == 0, "link moves the merge to 7");
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->mergePoint == 7) && !(fork->flags & GDX_BRANCH_FLAG_SYMMETRIC),
          "linked fork is asymmetric");

    count = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    CHECK(count == sCount - 4 + 1, "asymmetric swap: 4 A points out, 1 B node in");
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->forkPoint == 2) && (fork->mergePoint == 4) && (fork->nodeCount == 4),
          "B view fork spans 2..4 and stores 4 A points");
    sCount = count;
    CHECK(gdx_branch_edit_sync(sCount) == 0, "B view set survives sync");
    count = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    sCount = count;
    fork = fork_of(0);
    CHECK((count == 11) && (fork != NULL) && (fork->mergePoint == 7) && (sRing[7].pos[0] == 6000.0f),
          "asymmetric swap back restores path A");
}

static void test_delete_rotate_reverse(void) {
    GdxBranchEditPoint nodeB;
    const GdxBranchFork* fork;

    make_ring(10);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    ring_insert(4, 4500.0f); /* F=4, M=6 */
    memset(&nodeB, 0, sizeof(nodeB));
    gdx_branch_edit_split_commit(4, &nodeB);

    ring_delete(1);
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->forkPoint == 3) && (fork->mergePoint == 5), "delete before the fork shifts it");
    CHECK(gdx_branch_edit_sync(sCount) == 0, "set survives sync after delete");

    gdx_branch_edit_on_rotate(1, sCount);
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->forkPoint == 2) && (fork->mergePoint == 4), "rotate shifts the fork");
    gdx_branch_edit_on_reverse(sCount);
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->forkPoint == (1 - 4 + sCount) % sCount) &&
              (fork->mergePoint == (1 - 2 + sCount) % sCount),
          "reverse maps F and M through (1 - k) mod n");

    ring_delete(fork != NULL ? fork->forkPoint : 0);
    CHECK(gdx_branch_edit_fork_count() == 0, "deleting the fork point removes the fork");

    gdx_branch_edit_split_commit(1, &nodeB);
    gdx_branch_edit_on_reverse(sCount);
    CHECK(gdx_branch_edit_fork_count() == 0, "reverse drops a fork whose point lands on the start");

    gdx_branch_edit_split_commit(4, &nodeB);
    CHECK(gdx_branch_edit_sync(sCount + 3) == 1, "a ring count the hooks missed drops the forks");
}

static void test_span_split(void) {
    GdxBranchEditPoint nodesB[4];
    char reason[96];
    float pos[GDX_BRANCH_EDIT_MAX_POINTS][3];
    int forkPoint;
    int mergePoint;
    int dropped;
    int count;
    int i;
    const GdxBranchFork* fork;

    make_ring(12);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    CHECK(gdx_branch_edit_span_check(0, 4, sCount, reason, sizeof(reason)) < 0, "span refuses the start point");
    CHECK(gdx_branch_edit_span_check(8, 12, sCount, reason, sizeof(reason)) < 0, "span refuses a merge at point 0");
    CHECK(gdx_branch_edit_span_check(3, 4, sCount, reason, sizeof(reason)) < 0, "span refuses an empty path A");
    CHECK(gdx_branch_edit_span_check(3, 7, sCount, reason, sizeof(reason)) == 0, "span accepts 3..7");

    memset(nodesB, 0, sizeof(nodesB));
    for (i = 0; i < 3; i++) {
        nodesB[i].pos[0] = 1000.0f * (float) (4 + i);
        nodesB[i].pos[2] = 900.0f;
    }
    CHECK(gdx_branch_edit_span_commit(3, 7, nodesB, 3) == 0, "span commit");
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->forkPoint == 3) && (fork->mergePoint == 7) && (fork->nodeCount == 3) &&
              (fork->flags & GDX_BRANCH_FLAG_SYMMETRIC),
          "span makes F=3 M=7 with three symmetric nodes");
    CHECK(gdx_branch_edit_sync(sCount) == 0, "span set survives sync");
    CHECK(gdx_branch_edit_span_check(5, 9, sCount, reason, sizeof(reason)) < 0, "span refuses crossing a fork");
    CHECK(gdx_branch_edit_span_check(7, 9, sCount, reason, sizeof(reason)) == 0, "span may start at a merge point");
    CHECK(gdx_branch_edit_span_check(1, 3, sCount, reason, sizeof(reason)) == 0, "span may end at a fork point");

    CHECK((gdx_branch_edit_fork_shape(0, &forkPoint, &mergePoint, pos, GDX_BRANCH_EDIT_MAX_POINTS) == 3) &&
              (forkPoint == 3) && (mergePoint == 7) && (pos[2][0] == 6000.0f) && (pos[2][2] == 900.0f),
          "fork shape reports the ends and path B");
    CHECK(gdx_branch_edit_fork_shape(1, &forkPoint, &mergePoint, pos, GDX_BRANCH_EDIT_MAX_POINTS) < 0,
          "fork shape refuses a missing fork");

    /* In the B view an insert grows path B only. */
    count = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    sCount = count;
    CHECK(gdx_branch_edit_view_b() && (sRing[4].pos[2] == 900.0f), "B view shows the span's path B");
    ring_insert(4, 4500.0f);
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->mergePoint == 8) && (fork->nodeCount == 3) &&
              !(fork->flags & GDX_BRANCH_FLAG_SYMMETRIC),
          "insert in the B view adds to path B only and makes the fork asymmetric");
    CHECK(gdx_branch_edit_span_check(9, 10, sCount, reason, sizeof(reason)) < 0, "span refuses the B view");
    count = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    sCount = count;
    fork = fork_of(0);
    CHECK((count == 12) && (fork != NULL) && (fork->mergePoint == 7) && (fork->nodeCount == 4) &&
              (fork->nodes[1].pos[0] == 4500.0f),
          "back in the A view path A is unchanged and path B has four nodes");
}

static void test_undo_state_and_commit(void) {
    unsigned char state[GDX_BRANCH_EDIT_STATE_BYTES];
    GdxBranchEditPoint nodeB;
    GdxBranchSet stored;
    char reason[96];

    make_ring(8);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    gdx_branch_edit_backup();
    ring_insert(2, 2500.0f);
    memset(&nodeB, 0, sizeof(nodeB));
    gdx_branch_edit_split_commit(2, &nodeB);
    gdx_branch_edit_state_save(state);
    CHECK(gdx_branch_edit_state_equal(state), "saved state equals live");
    gdx_branch_edit_restore();
    CHECK(gdx_branch_edit_fork_count() == 0, "restore brings back the backup");
    gdx_branch_edit_state_load(state);
    CHECK(gdx_branch_edit_fork_count() == 1, "state load brings the fork back");

    CHECK(gdx_branch_edit_commit("TEST.CRSD", sCount, 1234u, reason, sizeof(reason)) == 0, "commit stores the set");
    CHECK(gdx_branch_store_get("TEST.CRSD", &stored) && (stored.forkCount == 1) && (stored.ringCount == sCount),
          "store holds the committed set");
    gdx_branch_edit_clear();
    CHECK(gdx_branch_edit_select("TEST.CRSD", sCount) == 1, "select loads the stored set");
    CHECK(gdx_branch_edit_select("TEST.CRSD", sCount + 1) == 0, "select ignores a set for another ring length");
    gdx_branch_edit_clear();
    CHECK(gdx_branch_edit_commit("TEST.CRSD", sCount, 1234u, reason, sizeof(reason)) == 0 &&
              !gdx_branch_store_get("TEST.CRSD", &stored),
          "committing no forks removes the entry");
}

static int node_has_no_parts(const GdxBranchNode* node) {
    return (node->pit == GDX_BRANCH_EDIT_PART_NONE) && (node->dash == GDX_BRANCH_EDIT_PART_NONE) &&
           (node->dirt == GDX_BRANCH_EDIT_PART_NONE) && (node->ice == GDX_BRANCH_EDIT_PART_NONE) &&
           (node->jump == GDX_BRANCH_EDIT_PART_NONE) && (node->landmine == GDX_BRANCH_EDIT_PART_NONE) &&
           (node->gate == GDX_BRANCH_EDIT_PART_NONE) && (node->building == GDX_BRANCH_EDIT_PART_NONE) &&
           (node->sign == GDX_BRANCH_EDIT_PART_NONE);
}

static void test_new_nodes_have_no_parts(void) {
    GdxBranchEditPoint nodeB;
    int dropped;
    int i;
    const GdxBranchFork* fork;

    memset(&nodeB, 0, sizeof(nodeB));
    gdx_branch_edit_point_clear_parts(&nodeB);
    for (i = 0; i < GDX_BRANCH_EDIT_PARTS; i++) {
        CHECK(nodeB.parts[i] == GDX_BRANCH_EDIT_PART_NONE, "clearing a point removes each part");
    }

    make_ring(8);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    ring_insert(3, 3500.0f);
    gdx_branch_edit_split_commit(3, &nodeB);
    fork = fork_of(0);
    CHECK((fork != NULL) && node_has_no_parts(&fork->nodes[0]), "a split's path B node has no parts");

    /* The new path A point has parts here, and its partner on path B must not copy them. */
    for (i = sCount; i > 5; i--) {
        sRing[i] = sRing[i - 1];
    }
    memset(&sRing[5], 1, sizeof(sRing[5]));
    sRing[5].pos[0] = 4500.0f;
    sCount++;
    gdx_branch_edit_on_insert(5, sRing, sCount);
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->nodeCount == 2) && node_has_no_parts(&fork->nodes[1]),
          "an inserted point's partner on path B has no parts");

    sCount = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    CHECK((sRing[4].parts[0] == GDX_BRANCH_EDIT_PART_NONE) && (sRing[4].parts[8] == GDX_BRANCH_EDIT_PART_NONE),
          "path B shows in the ring with no parts");
    sCount = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
}

static void test_select_repairs_old_parts(void) {
    GdxBranchEditPoint nodes[2];
    char reason[96];
    const GdxBranchFork* fork;

    make_ring(10);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    memset(nodes, 0, sizeof(nodes));
    nodes[0].pos[2] = 900.0f; /* every part 0, as older builds saved it */
    nodes[1].pos[0] = 4000.0f;
    nodes[1].pos[2] = 900.0f;
    gdx_branch_edit_point_clear_parts(&nodes[1]);
    nodes[1].parts[0] = 0; /* one part the user placed */
    CHECK(gdx_branch_edit_span_commit(3, 6, nodes, 2) == 0, "span commit with old parts");
    CHECK(gdx_branch_edit_commit("OLD.CRSD", sCount, 1234u, reason, sizeof(reason)) == 0, "commit stores old parts");
    gdx_branch_edit_clear();
    CHECK(gdx_branch_edit_select("OLD.CRSD", sCount) == 1, "select loads old parts");
    fork = fork_of(0);
    CHECK((fork != NULL) && node_has_no_parts(&fork->nodes[0]), "select removes all nine parts set to 0");
    CHECK((fork != NULL) && (fork->nodes[1].pit == 0) && (fork->nodes[1].dash == GDX_BRANCH_EDIT_PART_NONE),
          "select keeps a part the user placed");
    gdx_branch_edit_clear();
    gdx_branch_edit_commit("OLD.CRSD", sCount, 1234u, reason, sizeof(reason));
}

static void test_translate(void) {
    GdxBranchEditPoint nodeB;
    GdxBranchEditPoint nodesB[3];
    int dropped;
    int i;
    const GdxBranchFork* fork;

    make_ring(16);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    memset(&nodeB, 0, sizeof(nodeB));
    nodeB.pos[0] = 3000.0f;
    nodeB.pos[1] = 50.0f;
    nodeB.pos[2] = 900.0f;
    memset(nodesB, 0, sizeof(nodesB));
    for (i = 0; i < 3; i++) {
        nodesB[i].pos[0] = 1000.0f * (float) (9 + i);
        nodesB[i].pos[2] = 900.0f;
    }
    gdx_branch_edit_span_commit(2, 4, &nodeB, 1);
    gdx_branch_edit_span_commit(8, 12, nodesB, 3);

    gdx_branch_edit_on_translate(-100.0f, 25.0f);
    fork = fork_of(0);
    CHECK((fork != NULL) && (fork->nodes[0].pos[0] == 2900.0f) && (fork->nodes[0].pos[1] == 50.0f) &&
              (fork->nodes[0].pos[2] == 925.0f),
          "translate moves a node on x and z and leaves y");
    fork = fork_of(1);
    CHECK((fork != NULL) && (fork->nodes[2].pos[0] == 10900.0f) && (fork->nodes[2].pos[2] == 925.0f),
          "translate moves the nodes of every fork");

    /* In the B view the stored nodes are path A. */
    for (i = 0; i < sCount; i++) {
        sRing[i].pos[0] -= 100.0f;
        sRing[i].pos[2] += 25.0f;
    }
    sCount = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    for (i = 0; i < sCount; i++) {
        sRing[i].pos[0] += 100.0f;
        sRing[i].pos[2] -= 25.0f;
    }
    gdx_branch_edit_on_translate(100.0f, -25.0f);
    fork = fork_of(0);
    CHECK((fork != NULL) && gdx_branch_edit_view_b() && (fork->nodes[0].pos[0] == 3000.0f) &&
              (fork->nodes[0].pos[2] == 0.0f),
          "translate in the B view moves the stored path A");
    sCount = gdx_branch_edit_swap(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    fork = fork_of(0);
    CHECK((sRing[3].pos[0] == 3000.0f) && (sRing[3].pos[2] == 0.0f) && (fork != NULL) &&
              (fork->nodes[0].pos[0] == 3000.0f) && (fork->nodes[0].pos[2] == 900.0f),
          "both paths are back in place after moving in both views");
}

static void test_swap_to_node(void) {
    GdxBranchEditPoint nodeB;
    GdxBranchEditPoint nodesB[3];
    int point = -1;
    int count;
    int i;

    make_ring(16);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    memset(&nodeB, 0, sizeof(nodeB));
    nodeB.pos[0] = 3000.0f;
    nodeB.pos[2] = 900.0f;
    memset(nodesB, 0, sizeof(nodesB));
    for (i = 0; i < 3; i++) {
        nodesB[i].pos[0] = 1000.0f * (float) (9 + i);
        nodesB[i].pos[2] = 900.0f;
    }
    gdx_branch_edit_span_commit(2, 4, &nodeB, 1);
    gdx_branch_edit_span_commit(8, 12, nodesB, 3);
    CHECK(gdx_branch_edit_link(0, 6, sCount) == 0, "fork 0 becomes asymmetric: 3 A points, 1 B node");

    CHECK(gdx_branch_edit_swap_to_node(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, 2, 0, &point) == -1,
          "swap to node refuses a fork that does not exist");
    CHECK(gdx_branch_edit_swap_to_node(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, 0, 1, &point) == -1,
          "swap to node refuses a node that does not exist");
    CHECK(!gdx_branch_edit_view_b() && (fork_of(0)->nodeCount == 1), "a refused swap to node changes nothing");

    /* Fork 1 sits after the asymmetric fork, so its index moves with the swap. */
    count = gdx_branch_edit_swap_to_node(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, 1, 2, &point);
    CHECK((count == 14) && gdx_branch_edit_view_b() && (point == 9), "swap to node reports the node's new index");
    sCount = count;
    CHECK((sRing[point].pos[0] == 11000.0f) && (sRing[point].pos[2] == 900.0f), "that index holds the node of path B");

    /* And back: the stored nodes are now path A. */
    count = gdx_branch_edit_swap_to_node(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, 0, 2, &point);
    sCount = count;
    CHECK((count == 16) && !gdx_branch_edit_view_b() && (point == 5) && (sRing[point].pos[0] == 5000.0f) &&
              (sRing[point].pos[2] == 0.0f),
          "swap to node goes back to path A the same way");

    /* A fork with no points on the showing path would be removed by a swap, so the switch is refused. */
    ring_delete(5);
    ring_delete(4);
    ring_delete(3);
    count = gdx_branch_edit_swap_to_node(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, 1, 0, &point);
    CHECK((count == -1) && (gdx_branch_edit_fork_count() == 2) && !gdx_branch_edit_view_b(),
          "swap to node refuses rather than remove a fork");

    /* A ring that grew after the fork was made can swap past the point limit, which is refused too. */
    make_ring(58);
    gdx_branch_edit_clear();
    gdx_branch_edit_sync(sCount);
    gdx_branch_edit_span_commit(8, 12, nodesB, 3);
    CHECK(gdx_branch_edit_link(0, 10, sCount) == 0, "fork stores 3 nodes over a 1 point interior");
    for (i = 0; i < 6; i++) {
        ring_insert(20, 20500.0f);
    }
    count = gdx_branch_edit_swap_to_node(sRing, sCount, GDX_BRANCH_EDIT_MAX_POINTS, 0, 0, &point);
    CHECK((count == -1) && !gdx_branch_edit_view_b() && (gdx_branch_edit_fork_count() == 1),
          "swap to node refuses a ring that would not fit");
}

int main(void) {
    test_split_and_swap();
    test_link_and_asymmetric_swap();
    test_delete_rotate_reverse();
    test_span_split();
    test_undo_state_and_commit();
    test_new_nodes_have_no_parts();
    test_select_repairs_old_parts();
    test_translate();
    test_swap_to_node();
    if (sFailures != 0) {
        printf("%d course branch edit test(s) failed\n", sFailures);
        return 1;
    }
    printf("All course branch edit tests passed\n");
    return 0;
}
