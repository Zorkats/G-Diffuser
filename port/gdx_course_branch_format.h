/* port/gdx_course_branch_format.h -- course fork data model, GXB1 block, GBS1 state file.
 *
 * A fork leaves the main ring after ring control point F and rejoins at ring control point M.
 * The main ring between them (points F+1..M-1) is path A. Path B is a separate chain of nodes
 * stored here. Stock code and older builds only see the ring, so they race path A.
 *
 * This header does not include decomp/include/fzx_course.h. CourseDataDisk is 0x7E0 bytes with no
 * spare bytes, and a port header included from the game must not pull decomp headers. The node
 * fields match CourseData's per-point fields anyway: position, radii, trackSegmentInfo,
 * bankAngle, and the nine part bytes (pit, dash, dirt, ice, jump, landmine, gate, building, sign).
 *
 * The in-memory structs are not the file layout. Compilers may pad them. Every byte on disk is
 * written by hand, little-endian.
 *
 * GXB1 block (little-endian):
 *
 *   0x00  magic            "GXB1"
 *   0x04  version          u16, 1, 2, 3 or 4
 *   0x06  forkCount        u8, 0..GDX_BRANCH_MAX_FORKS
 *   0x07  ringCount        u8
 *   0x08  bindCrc          u32, CRC-32 of the bytes this block is bound to
 *   0x0C  courseChecksum   u32, the course checksum the forks belong to
 *   0x10  forks            forkCount x {
 *                            u8 forkPoint, u8 mergePoint, u8 flags, u8 nodeCount,
 *                            [s32 firstInfo, version >= 2 with flag bit1],
 *                            [six s8 segment parts + five u8 extras, version >= 3 with flag bit2],
 *                            nodeCount x 32-byte node }
 *   after forks           [u32 baseCrc, u8 extendedCount, extendedCount x node, version 4]
 *   end-4 crc32            u32, CRC-32 over every byte before it (IEEE, zlib polynomial)
 *
 * A node is 32 bytes: f32 x, f32 y, f32 z, s16 radiusLeft, s16 radiusRight, s32 trackSegmentInfo,
 * s16 bankAngle, then pit, dash, dirt, ice, jump, landmine, gate, building, sign as s8, then one
 * pad byte that must be 0. Floats are their IEEE-754 bits, written little-endian.
 *
 * Forks are stored in increasing forkPoint order. S71's overlap check is
 * mergePoint <= next forkPoint, so two forks may meet at one ring point and may not overlap.
 * Flag bit0 is GDX_BRANCH_FLAG_SYMMETRIC. Version 1 defines no other flag bit.
 *
 * The segment from F to the first node belongs to one path, but its style sits on F, a point both paths
 * share. Flag bit1 (GDX_BRANCH_FLAG_FIRST_INFO, version 2) gives the stored path its own value: firstInfo
 * is the trackSegmentInfo of the stored path's first segment. Without the bit both paths use F's value.
 * Flag bit2 (FIRST_VALUES, version 3) stores pit/dash/dirt/ice/jump/landmine and first-segment extras.
 * Gate/building/sign belong to the shared point, not either outgoing segment.
 * Encode selects the oldest version able to represent the set.
 *
 * GBS1 state file, separate from GXS1 on purpose (the GXS1 decoder rejects unknown flags, so an
 * older build must never see fork bytes inside the element store):
 *
 *   0x00  magic         "GBS1"
 *   0x04  version       u16 = 1
 *   0x06  entryCount    u16, 0..GDX_BRANCH_STORE_SLOTS
 *   0x08  fileLength    u32, every byte of the file including the trailing CRC
 *   0x0C  entries       entryCount x {
 *                         u8 idLen (1..25), courseId[idLen] (printable ASCII, no NUL),
 *                         u16 blockLen, GXB1 block[blockLen] }
 *   end-4 crc32         u32, CRC-32 over every byte before it
 *
 * Each GXB1 block is bound to the CRC-32 of its course id bytes (the idLen bytes, no NUL).
 * A course with neither forks nor extended points is absent from the file.
 */

#ifndef GDX_COURSE_BRANCH_FORMAT_H
#define GDX_COURSE_BRANCH_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_BRANCH_EXTRA_POINTS 63
#define GDX_BRANCH_MAX_FORKS 4
#define GDX_BRANCH_MAX_NODES 32
#define GDX_BRANCH_MAX_TOTAL_POINTS 127
#define GDX_BRANCH_FLAG_SYMMETRIC 0x01u
#define GDX_BRANCH_FLAG_FIRST_INFO 0x02u
#define GDX_BRANCH_FLAG_FIRST_VALUES 0x04u

/* Same limit as the registry store: course id "NAME.CRSD", 20 + '.' + 4. */
#define GDX_BRANCH_COURSE_ID_MAX 25
#define GDX_BRANCH_STORE_SLOTS 128

#define GDX_BRANCH_BLOCK_VERSION 4
#define GDX_BRANCH_BLOCK_VERSION_V3 3
#define GDX_BRANCH_BLOCK_VERSION_V2 2
#define GDX_BRANCH_BLOCK_VERSION_V1 1
#define GDX_BRANCH_BLOCK_HEADER_SIZE 16
#define GDX_BRANCH_NODE_BYTES 32
#define GDX_BRANCH_FIRST_VALUES_BYTES 11
#define GDX_BRANCH_FORK_WIRE_MAX (4 + 4 + GDX_BRANCH_FIRST_VALUES_BYTES + GDX_BRANCH_MAX_NODES * GDX_BRANCH_NODE_BYTES)
#define GDX_BRANCH_BLOCK_MIN_BYTES (GDX_BRANCH_BLOCK_HEADER_SIZE + 4)
#define GDX_BRANCH_BLOCK_MAX_BYTES \
    (GDX_BRANCH_BLOCK_HEADER_SIZE + GDX_BRANCH_MAX_FORKS * GDX_BRANCH_FORK_WIRE_MAX + 5 + GDX_BRANCH_EXTRA_POINTS * GDX_BRANCH_NODE_BYTES + 4)

#define GDX_BRANCH_FILE_VERSION 1
#define GDX_BRANCH_FILE_HEADER_SIZE 12
#define GDX_BRANCH_FILE_MIN_BYTES (GDX_BRANCH_FILE_HEADER_SIZE + 4)
#define GDX_BRANCH_FILE_ENTRY_MAX (1 + GDX_BRANCH_COURSE_ID_MAX + 2 + GDX_BRANCH_BLOCK_MAX_BYTES)
#define GDX_BRANCH_FILE_MAX_BYTES \
    (GDX_BRANCH_FILE_HEADER_SIZE + GDX_BRANCH_STORE_SLOTS * GDX_BRANCH_FILE_ENTRY_MAX + 4)

/* In-memory and codec return codes. Decode of a bad block returns one of these and zeroes out. */
#define GDX_BRANCH_OK 0
#define GDX_BRANCH_ERR_ARGS (-1)       /* NULL pointer or otherwise unusable argument */
#define GDX_BRANCH_ERR_FULL (-2)       /* buffer, store, or node list is full */
#define GDX_BRANCH_ERR_INDEX (-3)      /* node or fork index is out of range */
#define GDX_BRANCH_ERR_COUNT (-4)      /* forkCount above 4, or nodeCount outside 1..32 */
#define GDX_BRANCH_ERR_SPAN (-5)       /* fork touches point 0, wraps, or leaves the ring */
#define GDX_BRANCH_ERR_OVERLAP (-6)    /* forks overlap, share a fork point, or are not sorted */
#define GDX_BRANCH_ERR_TOTAL (-7)      /* ring points plus every B node exceed 127 */
#define GDX_BRANCH_ERR_SYMMETRIC (-8)  /* symmetric nodeCount is not the path A length */
#define GDX_BRANCH_ERR_FLOAT (-9)      /* a used node position is not finite */
#define GDX_BRANCH_ERR_FLAGS (-10)     /* a flag bit the block's version does not define is set */
#define GDX_BRANCH_ERR_RING (-11)      /* ringCount is negative or does not match the set */

#define GDX_BRANCH_ERR_TRUNCATED (-20) /* shorter than the minimum block */
#define GDX_BRANCH_ERR_MAGIC (-21)     /* not "GXB1" */
#define GDX_BRANCH_ERR_VERSION (-22)   /* version is not 1, 2, 3 or 4 */
#define GDX_BRANCH_ERR_CRC (-23)       /* trailing CRC-32 does not match the bytes */
#define GDX_BRANCH_ERR_BINDING (-24)   /* block is bound to a different CRC-32 */
#define GDX_BRANCH_ERR_LAYOUT (-25)    /* fork bytes do not fill the block, or a pad byte is not 0 */

/* GBS1 file return codes. A nested GXB1 failure is returned as the block code, not rewritten. */
#define GDX_BRANCH_FILE_ERR_TRUNCATED (-40) /* shorter than the header plus trailing CRC */
#define GDX_BRANCH_FILE_ERR_MAGIC (-41)     /* not "GBS1" */
#define GDX_BRANCH_FILE_ERR_VERSION (-42)   /* version is not 1 */
#define GDX_BRANCH_FILE_ERR_COUNT (-43)     /* entryCount above GDX_BRANCH_STORE_SLOTS */
#define GDX_BRANCH_FILE_ERR_LENGTH (-44)    /* fileLength or an entry size does not match the bytes */
#define GDX_BRANCH_FILE_ERR_CRC (-45)       /* trailing CRC-32 does not match the bytes */
#define GDX_BRANCH_FILE_ERR_ENTRY (-46)     /* bad course id, repeated id, or a forkless entry */

/* Values of one point that the course data has no room for (gdx_course_settings.h). 0 is "not set". */
typedef struct GdxPointExtras {
    uint8_t pitWidth;
    uint8_t dirtWidth;
    uint8_t iceWidth;
    uint8_t magnetStrength;
    uint8_t magnetRange;
    /* The dash pad at this point: GDX_SETTINGS_PAD_* kind, width percent, and a Super pad's boost. GXK1 version 2
     * stores them. GXB1 does not, so a fork's first segment always has a normal pad. */
    uint8_t padKind;
    uint8_t padWidth;
    uint8_t padBoost;
    /* The wind on the segment that starts here: 0 none, 1..100 percent. windAngle is the compass angle 0..359 the
     * wind blows toward, 0 unless windStrength is 1..100. GXK1 version 3 stores them; GXB1 does not, so a fork's
     * first segment never has wind. */
    uint8_t windStrength;
    uint16_t windAngle;
} GdxPointExtras;

/* One path B control point. Fixed size, no pointers. Not a file overlay. */
typedef struct GdxBranchNode {
    float pos[3];
    int16_t radiusLeft;
    int16_t radiusRight;
    int32_t trackSegmentInfo;
    int16_t bankAngle;
    int8_t pit;
    int8_t dash;
    int8_t dirt;
    int8_t ice;
    int8_t jump;
    int8_t landmine;
    int8_t gate;
    int8_t building;
    int8_t sign;
    GdxPointExtras extras; /* The editor's copy, so it follows the node. GXK1 stores it, never GXB1. */
} GdxBranchNode;

typedef struct GdxBranchFork {
    uint8_t forkPoint;
    uint8_t mergePoint;
    uint8_t flags;
    uint8_t nodeCount;
    int32_t firstInfo; /* trackSegmentInfo of the stored path's first segment, with GDX_BRANCH_FLAG_FIRST_INFO */
    int8_t firstParts[6];
    GdxPointExtras firstExtras;
    GdxBranchNode nodes[GDX_BRANCH_MAX_NODES];
} GdxBranchFork;

typedef struct GdxBranchSet {
    uint32_t courseChecksum;
    uint8_t ringCount;
    uint8_t forkCount;
    GdxBranchFork forks[GDX_BRANCH_MAX_FORKS];
    uint32_t baseCrc;
    uint8_t extendedCount;
    GdxBranchNode extended[GDX_BRANCH_EXTRA_POINTS];
} GdxBranchSet;

/* ---------------------------------------------------------------------------------
 * Validation. ringCount is the live ring length and must equal set->ringCount. The rules are
 * S71: forkCount <= 4, 1 <= forkPoint < mergePoint <= ringCount - 1, nodeCount 1..32, forks
 * sorted by forkPoint with each mergePoint <= the next forkPoint, ringCount + node counts <= 64,
 * a symmetric fork's nodeCount equal to mergePoint - forkPoint - 1, and every used position finite.
 * Returns GDX_BRANCH_OK, or a negative code. reason receives a short English string when cap > 0.
 * reason may be NULL.
 * ------------------------------------------------------------------------------- */

int gdx_branch_validate(const GdxBranchSet* set, int ringCount, char* reason, size_t cap);

/* ---------------------------------------------------------------------------------
 * Editor index maintenance. Neither call edits node lists or ringCount. The caller updates
 * ringCount, and pairs a new path A point with a B node when an insert lands inside a symmetric
 * fork. Both return how many forks were removed (insert removes none).
 * ------------------------------------------------------------------------------- */

/* A ring point was inserted at newIndex. Every forkPoint and mergePoint >= newIndex shifts up. */
int gdx_branch_on_ring_insert(GdxBranchSet* set, int newIndex);

/* A ring point was deleted at deletedIndex. Endpoints at that index drop the fork. A symmetric
 * fork whose path A loses a point loses GDX_BRANCH_FLAG_SYMMETRIC. Later endpoints shift down. */
int gdx_branch_on_ring_delete(GdxBranchSet* set, int deletedIndex);

/* Inserts a copy of node at index `at` (0..nodeCount, nodeCount appends). Refuses a 33rd node. */
int gdx_branch_insert_node(GdxBranchFork* fork, int at, const GdxBranchNode* node);

/* Deletes node `at`. Refuses to delete the last node. */
int gdx_branch_delete_node(GdxBranchFork* fork, int at);

/* Index of the fork whose closed span [forkPoint, mergePoint] contains ringPoint, or -1.
 * When two forks meet at one point, the earlier fork is returned. */
int gdx_branch_find_fork(const GdxBranchSet* set, int ringPoint);

/* Writes "<n>-A" for a ring point strictly inside a fork's path A, otherwise "<n>".
 * n is the ring index. Returns GDX_BRANCH_OK, or a negative code if out cannot hold the text. */
int gdx_branch_label_ring(const GdxBranchSet* set, int ringPoint, char* out, size_t cap);

/* Writes "<forkPoint + 1 + node>-B". fork and node must name a stored node. */
int gdx_branch_label_node(const GdxBranchSet* set, int fork, int node, char* out, size_t cap);

/* ---------------------------------------------------------------------------------
 * GXB1 codec. length is the exact block size, not a buffer capacity. encode validates the set
 * against set->ringCount and writes nothing on error. decode zeroes out on every failure.
 * ------------------------------------------------------------------------------- */

int gdx_branch_encode(const GdxBranchSet* set, uint32_t bindCrc, unsigned char* out, size_t cap);
int gdx_branch_decode(const unsigned char* data, size_t length, uint32_t bindCrc, GdxBranchSet* out);

/* ---------------------------------------------------------------------------------
 * In-memory store, keyed by courseId "NAME.CRSD" (1..25 printable ASCII characters), up to
 * 128 courses. Single-threaded, same as the registry store. A set with forkCount 0 removes the
 * entry. An invalid set is refused and the store is left unchanged.
 * ------------------------------------------------------------------------------- */

int gdx_branch_store_set(const char* courseId, const GdxBranchSet* set);
int gdx_branch_store_get(const char* courseId, GdxBranchSet* out); /* 1 if found, else 0 */

/* Moves oldId's set onto newId, replacing what newId held, then drops oldId. If oldId has no
 * set, newId is cleared too (same rule as the registry rename hook). A same-id rename is a no-op.
 * Returns GDX_BRANCH_OK or GDX_BRANCH_ERR_ARGS. */
int gdx_branch_store_rename(const char* oldId, const char* newId);
void gdx_branch_store_clear(void);
unsigned long gdx_branch_store_revision(void);

/* Encodes the store as GBS1. Returns the file length, or a negative code. Nothing is written
 * on error. restore validates the whole file first and replaces the store only when it is wholly
 * valid. On any error the store and its revision are unchanged. */
int gdx_branch_store_encode_file(unsigned char* out, size_t cap);
int gdx_branch_store_restore_file(const unsigned char* data, size_t length);

/* ---------------------------------------------------------------------------------
 * Active set for the course being raced or edited. Game thread only. The pointer from
 * gdx_branch_active_set stays valid until the next gdx_branch_set_active. NULL clears.
 * This does not read or write the course store.
 * ------------------------------------------------------------------------------- */

const GdxBranchSet* gdx_branch_active_set(void);
void gdx_branch_set_active(const GdxBranchSet* set);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BRANCH_FORMAT_H */
