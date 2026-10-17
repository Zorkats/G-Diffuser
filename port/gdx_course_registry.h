/* port/gdx_course_registry.h -- Course Edit element registry and the GXE1 course element sidecar.
 *
 * Why element IDs never go into CourseData: CourseData (decomp/include/fzx_course.h, 0x7E0 bytes,
 * no spare bytes) is shared by the ROM, the EK save, the 64DD save and .gdxc. The native load
 * rejects checksum mismatches and BGM IDs above BGM_NEW_04 (decomp/src/overlays/course_edit/
 * 19C470.c:160-167), so a pack-defined element cannot be written there without breaking the other
 * formats. Every element therefore has a stable string key, and CourseData keeps a valid stock ID
 * as the fallback for it.
 *
 * Model:
 *   - Five kinds. Their numbers are part of the GXE1 format and never change.
 *   - Stock entries: one per stock numeric ID of the kind, with a fixed key. Their fallback is
 *     their own ID.
 *   - Extension entries: registered at runtime with a key, a display name and a fallback that must
 *     be a valid stock ID of the same kind. An extension entry never owns a numeric ID.
 *
 * Per-course store: for each course (its MFS name and extension, "NAME.CRSD") the store keeps at
 * most one extension key per (kind, scope). Scope is GDX_COURSE_SCOPE_COURSE or a control point
 * index. The store is in memory only and is used from the menu thread.
 *
 * GXE1 block (the .gdxe sidecar of a .gdxc; little-endian, hand-packed, never a struct overlay):
 *
 *   0x00  magic         "GXE1" (4 bytes)
 *   0x04  version       u16 = 1
 *   0x06  count         u16, 0..GDX_COURSE_EXT_MAX_RECORDS
 *   0x08  payloadCrc32  u32, CRC-32 of the .gdxc payload this block belongs to
 *   0x0C  records       count x { kind u8, scope u8, fallback u8, keyLen u8, key[keyLen] }
 *   end-4 crc32         u32, CRC-32 over every byte before it (IEEE, zlib polynomial)
 *
 * scope 0xFF means course-wide; any other value is a control point index 0..63. Keys use the
 * characters [a-z0-9._-]. A (kind, scope) pair must be unique within one block.
 *
 * Track titles (Edit Cup 8A): a long-named track that sits in an 8-character Edit Cup slot has a
 * short MFS name on disk. Its full name is the title. Titles are never part of the GXE1 block,
 * because the GXE1 decoder rejects any record it does not know and would reject the whole sidecar
 * in older builds. A title travels in a separate GXT1 block, <stem>.gdxt, next to the .gdxc. The
 * container flag GDX_CONTENT_FLAG_TITLE says it exists, and older readers ignore both the flag bit
 * and the file.
 *
 *   0x00  magic         "GXT1" (4 bytes)
 *   0x04  version       u16 = 1
 *   0x06  titleLen      u16, 1..GDX_COURSE_TITLE_MAX
 *   0x08  payloadCrc32  u32, CRC-32 of the .gdxc payload this block belongs to
 *   0x0C  title         titleLen printable ASCII bytes (no NUL)
 *   end-4 crc32         u32, CRC-32 over every byte before it
 */

#ifndef GDX_COURSE_REGISTRY_H
#define GDX_COURSE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GdxCourseKind {
    GDX_COURSE_KIND_VENUE = 0,        /* Venue, decomp/include/fzx_course.h:105-117 */
    GDX_COURSE_KIND_SKYBOX = 1,       /* Skybox, decomp/include/fzx_course.h:119-128 */
    GDX_COURSE_KIND_BGM = 2,          /* raw BgmId, decomp/include/sfx.h:98-115 (not the menu order) */
    GDX_COURSE_KIND_PART_STYLE = 3,   /* TrackPartStyle, decomp/include/fzx_expansion_kit.h:53-63 */
    GDX_COURSE_KIND_TRACK_EFFECT = 4, /* CourseEffects, decomp/include/fzx_racer.h:16-25 */
    GDX_COURSE_KIND_COUNT = 5
} GdxCourseKind;

/* Limits. Keys and names are bounded so every buffer in the module is fixed size. */
#define GDX_COURSE_KEY_MAX 48           /* GXE1 keyLen is u8; 1..48 characters */
#define GDX_COURSE_DISPLAY_MAX 64       /* printable ASCII display name */
#define GDX_COURSE_FILE_ID_MAX 25       /* course id "NAME.EXT": 20 + '.' + 4 */
#define GDX_COURSE_EXT_PER_KIND 32      /* runtime-registered entries per kind */
#define GDX_COURSE_EXT_MAX_RECORDS 128  /* records per course and per GXE1 block */
#define GDX_COURSE_SCOPE_POINTS 64      /* CourseData control points (controlPoint[64]) */
#define GDX_COURSE_SCOPE_COURSE 0xFF    /* scope value for course-wide records */
/* Courses that may hold extension keys at once. 128 is one slot per course the Content Library can
 * list (it shows at most 100 CRSD files per class, A8140.c:316) plus room for the Edit Cup. Each
 * slot is about 8 KB, so the store is about 1 MB. Records of deleted courses are not pruned until
 * the rename and delete hooks land (see the report of plan slice 9); a full store refuses new keys
 * with GDX_COURSE_ERR_FULL and never drops a saved record. */
#define GDX_COURSE_STORE_SLOTS 128
#define GDX_COURSE_DEFAULT_STOCK_ID 0   /* resolve() answer for an unknown key */
#define GDX_COURSE_PATH_MAX 512         /* source file of a file-backed (7A music) extension entry */
#define GDX_COURSE_MUSIC_KEY_PREFIX "bgm.custom." /* 11 characters; the key is this prefix plus the stem */
#define GDX_COURSE_TITLE_MAX 20         /* a track title is an MFS name: 20 characters */
#define GDX_COURSE_TITLE_SLOTS 128      /* courses that may carry a session title at once */
#define GDX_COURSE_SHORT_NAME_MAX 8     /* an Edit Cup slot holds 8 characters plus NUL */
#define GDX_COURSE_TITLE_VERSION 1
#define GDX_COURSE_TITLE_HEADER_SIZE 12
#define GDX_COURSE_TITLE_MAX_BYTES (GDX_COURSE_TITLE_HEADER_SIZE + GDX_COURSE_TITLE_MAX + 4)

/* GXE1 layout constants (see the header comment). */
#define GDX_COURSE_EXT_VERSION 1
#define GDX_COURSE_EXT_HEADER_SIZE 12
#define GDX_COURSE_EXT_MIN_BYTES (GDX_COURSE_EXT_HEADER_SIZE + 4)
#define GDX_COURSE_EXT_MAX_BYTES \
    (GDX_COURSE_EXT_HEADER_SIZE + GDX_COURSE_EXT_MAX_RECORDS * (4 + GDX_COURSE_KEY_MAX) + 4)

/* Registry and store return codes. */
#define GDX_COURSE_OK 0
#define GDX_COURSE_ERR_ARGS (-1)          /* NULL pointer or otherwise unusable argument */
#define GDX_COURSE_ERR_KIND (-2)          /* kind outside GDX_COURSE_KIND_* */
#define GDX_COURSE_ERR_KEY (-3)           /* empty, too long, or characters outside [a-z0-9._-] */
#define GDX_COURSE_ERR_NAME (-4)          /* display name empty, too long, or not printable */
#define GDX_COURSE_ERR_FALLBACK (-5)      /* fallback is not a stock ID of the kind */
#define GDX_COURSE_ERR_DUPLICATE (-6)     /* key already registered for this kind */
#define GDX_COURSE_ERR_FULL (-7)          /* a bounded capacity is reached */
#define GDX_COURSE_ERR_UNKNOWN (-8)       /* key or record not found */
#define GDX_COURSE_ERR_NOT_EXTENSION (-9) /* a stock key was given where an extension key is needed */
#define GDX_COURSE_ERR_SCOPE (-10)        /* scope is neither course-wide nor a point index 0..63 */
#define GDX_COURSE_ERR_PATH (-11)         /* source path empty or not shorter than GDX_COURSE_PATH_MAX */

/* GXE1 decode return codes. Every rejection is one of these; nothing partial is returned. */
#define GDX_COURSE_EXT_ERR_TRUNCATED (-20) /* shorter than the minimum block */
#define GDX_COURSE_EXT_ERR_MAGIC (-21)     /* not "GXE1" */
#define GDX_COURSE_EXT_ERR_VERSION (-22)   /* version is not 1 */
#define GDX_COURSE_EXT_ERR_COUNT (-23)     /* count above GDX_COURSE_EXT_MAX_RECORDS */
#define GDX_COURSE_EXT_ERR_CRC (-24)       /* trailing CRC-32 does not match the bytes */
#define GDX_COURSE_EXT_ERR_BINDING (-25)   /* block belongs to a different payload CRC-32 */
#define GDX_COURSE_EXT_ERR_RECORD (-26)    /* a record has an invalid field or duplicate identity */
#define GDX_COURSE_EXT_ERR_LAYOUT (-27)    /* records overrun the block or bytes are left over */

/* GXT1 title decode return codes. */
#define GDX_COURSE_TITLE_ERR_TRUNCATED (-40) /* shorter than the minimum block */
#define GDX_COURSE_TITLE_ERR_MAGIC (-41)     /* not "GXT1" */
#define GDX_COURSE_TITLE_ERR_VERSION (-42)   /* version is not 1 */
#define GDX_COURSE_TITLE_ERR_LENGTH (-43)    /* titleLen outside 1..20 or not the block length */
#define GDX_COURSE_TITLE_ERR_CRC (-44)       /* trailing CRC-32 does not match the bytes */
#define GDX_COURSE_TITLE_ERR_BINDING (-45)   /* block belongs to a different payload CRC-32 */
#define GDX_COURSE_TITLE_ERR_TEXT (-46)      /* a title byte is not printable ASCII */

/* One registry entry as seen by callers. Stock entries point at static strings; extension entries
 * point at storage owned by the registry, valid until gdx_course_registry_clear_extensions(). */
typedef struct GdxCourseElement {
    const char* key;         /* stable identifier, for example "venue.mute_city" */
    const char* displayName; /* label for menus and messages */
    GdxCourseKind kind;
    int stockId;             /* stock entries: their own ID. Extension entries: the fallback ID */
    int isExtension;         /* 0 = stock, 1 = registered at runtime */
    const char* sourcePath;  /* file-backed extension entries (music): the file. NULL otherwise */
} GdxCourseElement;

/* One extension key attached to a course. */
typedef struct GdxCourseExtRecord {
    int kind;                             /* GdxCourseKind */
    int scope;                            /* GDX_COURSE_SCOPE_COURSE or a control point index */
    int fallbackId;                       /* stock ID the record falls back to */
    char key[GDX_COURSE_KEY_MAX + 1];
} GdxCourseExtRecord;

typedef struct GdxCourseExtBlock {
    int count;
    GdxCourseExtRecord records[GDX_COURSE_EXT_MAX_RECORDS];
} GdxCourseExtBlock;

/* ---------------------------------------------------------------------------------
 * Registry queries. Stock entries come first in every kind; extension entries follow in
 * registration order. Stock IDs are the table index (checked by the unit tests).
 * ------------------------------------------------------------------------------- */

int gdx_course_registry_stock_count(GdxCourseKind kind);
int gdx_course_registry_count(GdxCourseKind kind); /* stock + registered extensions */

/* NULL when index is out of range for the kind. */
const GdxCourseElement* gdx_course_registry_at(GdxCourseKind kind, int index);

/* NULL when the key is unknown. Matches stock and extension entries. */
const GdxCourseElement* gdx_course_registry_find_key(GdxCourseKind kind, const char* key);

/* Stock entry with this numeric ID, or NULL. Never returns an extension entry. */
const GdxCourseElement* gdx_course_registry_find_stock(GdxCourseKind kind, int stockId);

/* Returns 1 when key is a legal element key: 1..GDX_COURSE_KEY_MAX characters of [a-z0-9._-]. */
int gdx_course_key_is_valid(const char* key);

/* ---------------------------------------------------------------------------------
 * Runtime extension registration.
 * ------------------------------------------------------------------------------- */

/* Adds an extension entry. The key must be unique within the kind (stock keys included), the
 * display name must be printable ASCII of 1..GDX_COURSE_DISPLAY_MAX characters, and fallbackStockId
 * must be a stock ID of the kind. Returns GDX_COURSE_OK or a negative GDX_COURSE_ERR_* code. */
int gdx_course_registry_register(GdxCourseKind kind, const char* key, const char* displayName,
                                 int fallbackStockId);

/* Adds an extension entry backed by a file. Same rules as gdx_course_registry_register; sourcePath
 * must be non-empty and shorter than GDX_COURSE_PATH_MAX. The path is copied and reported through
 * the entry's sourcePath. Returns GDX_COURSE_OK or a negative GDX_COURSE_ERR_* code. */
int gdx_course_registry_register_file(GdxCourseKind kind, const char* key, const char* displayName,
                                      int fallbackStockId, const char* sourcePath);

/* Writes the registry key for a music file stem: GDX_COURSE_MUSIC_KEY_PREFIX followed by the stem
 * lowercased, with every character outside [a-z0-9._-] replaced by '_', cut so the whole key fits
 * GDX_COURSE_KEY_MAX. Returns 1 when out holds a valid key, 0 for an empty stem or a too small outCap. */
int gdx_course_registry_music_key(const char* fileStem, char* out, size_t outCap);

/* Writes the key an imported song takes when its own music key is already used by a different song:
 * musicKey with its body cut to leave room, then '_' and the eight lowercase hex digits of contentCrc.
 * The result is GDX_COURSE_KEY_MAX characters at most and is also a valid file stem, so a file named
 * after its suffix scans back to the same key. Returns 1 when out holds the key, 0 when musicKey is not
 * a music key or outCap is too small for the whole key. */
int gdx_course_registry_music_fingerprint_key(const char* musicKey, uint32_t contentCrc, char* out, size_t outCap);

/* Writes the menu label for a music file stem: uppercased, limited to the characters the game font
 * draws (A-Z, 0-9, space and ' & + - . , ( ) :), other characters turned into spaces, runs of spaces
 * collapsed, the ends trimmed and at most GDX_COURSE_DISPLAY_MAX characters kept. A stem with no
 * drawable character gives "CUSTOM". Returns 1 when out holds a label, 0 for NULL or a too small outCap. */
int gdx_course_registry_music_display(const char* fileStem, char* out, size_t outCap);

/* Returns the stock ID to use for key: the entry's own ID for a stock key, its fallback for an
 * extension key. An unknown key (or invalid kind) returns GDX_COURSE_DEFAULT_STOCK_ID (the first
 * stock entry of the kind) and sets *outMissing to 1. outMissing may be NULL. For a known key it
 * is set to 0. */
int gdx_course_registry_resolve(GdxCourseKind kind, const char* key, int* outMissing);

/* Drops every runtime extension entry and the whole per-course store. Used by the tests and for a
 * clean session. Stock entries are never affected. */
void gdx_course_registry_clear_extensions(void);

/* ---------------------------------------------------------------------------------
 * Per-course extension store (in memory, menu thread only). A course is identified by
 * courseId = "NAME.EXT" built from its MFS name and extension, for example "MYTRACK.CRSD".
 * ------------------------------------------------------------------------------- */

/* Sets the extension key for (kind, scope), replacing any key already set for that pair. The key
 * must be registered as an extension entry; the record takes that entry's fallback. */
int gdx_course_ext_set(const char* courseId, GdxCourseKind kind, int scope, const char* key);

/* Removes the record for (kind, scope). GDX_COURSE_ERR_UNKNOWN when none is set. */
int gdx_course_ext_clear(const char* courseId, GdxCourseKind kind, int scope);

/* Removes every record of the course. */
void gdx_course_ext_clear_course(const char* courseId);

/* Number of records set for the course (0 when the course has none). */
int gdx_course_ext_count(const char* courseId);

/* Records of the course, or NULL when it has none. *outCount receives the count. The pointer is
 * valid until the next call that changes the store. */
const GdxCourseExtRecord* gdx_course_ext_records(const char* courseId, int* outCount);

/* ---------------------------------------------------------------------------------
 * GXE1 codec. Pure byte functions; they check structure and CRC, not the registry (the importer
 * does the registry checks, see gdx_content_import.c).
 * ------------------------------------------------------------------------------- */

/* Encodes count records (0 is allowed) bound to payloadCrc32 into out. Returns the block length,
 * or a negative GDX_COURSE_ERR_* code when a record is invalid, a (kind, scope) pair repeats, or
 * the block does not fit in outCap. Nothing is written on error. */
int gdx_course_ext_encode(const GdxCourseExtRecord* records, int count, uint32_t payloadCrc32,
                          unsigned char* out, size_t outCap);

/* Parses and validates a GXE1 block that must belong to a payload with payloadCrc32. On success
 * out->count and out->records hold the records. On failure out->count is 0 and the records must
 * not be used. Returns GDX_COURSE_OK or a negative GDX_COURSE_EXT_ERR_* code. */
int gdx_course_ext_decode(const unsigned char* data, size_t length, uint32_t payloadCrc32,
                          GdxCourseExtBlock* out);

/* CRC-32 (IEEE 802.3, zlib polynomial 0xEDB88320). Same algorithm as gdx_content_crc32. */
uint32_t gdx_course_registry_crc32(const unsigned char* data, size_t length);

/* ---------------------------------------------------------------------------------
 * Edit Cup short names (8A). A long track keeps its MFS name on disk, and its cup slot holds a
 * short name made of the first 8 characters that MFS accepts. When that name is taken, the name
 * gets a "~N" suffix that replaces the trailing characters, so the total stays at 8. A character
 * is accepted when MFS would accept it as a single byte (printable ASCII; '/', '.' and ':' are
 * refused by MFS_VERSION_B, which the port builds).
 * ------------------------------------------------------------------------------- */

/* Writes the first free short name for longName into out (at least GDX_COURSE_SHORT_NAME_MAX + 1
 * bytes). taken lists the names already in use, compared exactly; a NULL entry is skipped. Returns
 * 1 when out holds a name, 0 when longName is empty or longer than GDX_COURSE_TITLE_MAX, has no
 * accepted character, outCap is too small, or every "~N" suffix up to 999 is taken. */
int gdx_course_short_name(const char* longName, const char* const* taken, int takenCount, char* out,
                          size_t outCap);

/* ---------------------------------------------------------------------------------
 * Session title store (menu thread, in memory). A course is identified by courseId = "NAME.EXT",
 * the same key as the extension store. A title is the full MFS name of a track whose file name
 * is a short cup name.
 * ------------------------------------------------------------------------------- */

/* Sets the title of a course, replacing any title already set. title must be printable ASCII of
 * 1..GDX_COURSE_TITLE_MAX characters. Returns GDX_COURSE_OK or GDX_COURSE_ERR_ARGS, _NAME, _FULL. */
int gdx_course_title_set(const char* courseId, const char* title);

/* Removes the title of a course. Does nothing when the course has none. */
void gdx_course_title_clear(const char* courseId);

/* Copies the title of a course into out (NUL-terminated). Returns 1 when the course has a title
 * and it fit, 0 otherwise. */
int gdx_course_title_get(const char* courseId, char* out, size_t outCap);

/* Copies the courseId of the first course whose title equals title into outCourseId. Returns 1
 * when one was found, 0 otherwise. */
int gdx_course_title_find_course(const char* title, char* outCourseId, size_t outCap);

/* ---------------------------------------------------------------------------------
 * GXT1 title codec (see the header comment). Pure byte functions, like the GXE1 codec.
 * ------------------------------------------------------------------------------- */

/* Encodes title bound to payloadCrc32 into out. Returns the block length, or a negative
 * GDX_COURSE_ERR_* code (_NAME for a bad title, _FULL when outCap is too small). Nothing is
 * written on error. */
int gdx_course_title_encode(const char* title, uint32_t payloadCrc32, unsigned char* out, size_t outCap);

/* Parses and validates a GXT1 block bound to payloadCrc32 and copies the title into outTitle
 * (NUL-terminated, outCap must hold the title plus NUL). Returns GDX_COURSE_OK or a negative
 * GDX_COURSE_TITLE_ERR_* code. outTitle is only written on success. */
int gdx_course_title_decode(const unsigned char* data, size_t length, uint32_t payloadCrc32, char* outTitle,
                            size_t outCap);

/* ---------------------------------------------------------------------------------
 * Restore and store-wide helpers (plan slice 9). Used by the GXS1 state file and by the import paths.
 * ------------------------------------------------------------------------------- */

/* Replaces every extension record of a course with records[0..count-1]. Records get the same field
 * checks as gdx_course_ext_encode but no registry check, so a key that is not registered in this
 * session survives a round trip. count 0 removes the course's records. The store is unchanged on error. */
int gdx_course_ext_restore(const char* courseId, const GdxCourseExtRecord* records, int count);

/* Rename hook (plan slice 9): moves the keys and the title of oldId to newId, replacing what newId held,
 * then clears oldId. Returns GDX_COURSE_OK, or a negative GDX_COURSE_ERR_* code (nothing moved on a
 * store-full error). Call gdx_course_state_flush afterwards. */
int gdx_course_store_rename(const char* oldId, const char* newId);

/* Empties the per-course extension store and the title store. Registry entries are not touched. */
void gdx_course_store_clear(void);

/* Counter that rises on every change to the extension or title store. The state file layer compares
 * it with the value it last saved, so an unchanged store is never rewritten. */
unsigned long gdx_course_store_revision(void);

/* ---------------------------------------------------------------------------------
 * GXS1 state file codec. One file holds, per course, an optional GXE1 block and an optional GXT1
 * block. Layout (little-endian):
 *
 *   0x00  magic        "GXS1"
 *   0x04  version      u16 = 1
 *   0x06  entryCount   u16, 0..GDX_COURSE_STATE_MAX_ENTRIES
 *   0x08  fileLength   u32, every byte of the file including the trailing CRC
 *   0x0C  entries      entryCount x {
 *                        idLen u8 (1..25), courseId[idLen] (printable, no NUL),
 *                        bindCrc u32, flags u8 (bit0 = GXE1 present, bit1 = GXT1 present),
 *                        if bit0: extLen u16, GXE1 block[extLen] (its payloadCrc32 must be bindCrc),
 *                        if bit1: titleLen u16, GXT1 block[titleLen] (its payloadCrc32 must be bindCrc) }
 *   end-4 crc32        u32, CRC-32 over every byte before it
 *
 * The store file uses bindCrc = CRC-32 of the courseId, so a block cannot move to another course.
 * The bundle companion uses bindCrc = the CRC-32 of the member's payload, the same binding GXE1 uses.
 * Unknown keys are kept: the decoder checks structure and CRC, not the registry. */

#define GDX_COURSE_STATE_VERSION 1
#define GDX_COURSE_STATE_HEADER_SIZE 12
#define GDX_COURSE_STATE_MAX_ENTRIES (GDX_COURSE_STORE_SLOTS + GDX_COURSE_TITLE_SLOTS)
#define GDX_COURSE_STATE_MAX_BYTES                                                                     \
    (GDX_COURSE_STATE_HEADER_SIZE +                                                                    \
     GDX_COURSE_STATE_MAX_ENTRIES *                                                                    \
         (1 + GDX_COURSE_FILE_ID_MAX + 4 + 1 + 2 + GDX_COURSE_EXT_MAX_BYTES + 2 + GDX_COURSE_TITLE_MAX_BYTES) + \
     4)

#define GDX_COURSE_STATE_ERR_TRUNCATED (-60) /* shorter than the header plus trailing CRC */
#define GDX_COURSE_STATE_ERR_MAGIC (-61)     /* not "GXS1" */
#define GDX_COURSE_STATE_ERR_VERSION (-62)   /* version is not 1 */
#define GDX_COURSE_STATE_ERR_COUNT (-63)     /* entryCount above GDX_COURSE_STATE_MAX_ENTRIES */
#define GDX_COURSE_STATE_ERR_LENGTH (-64)    /* fileLength, a size field or the layout does not match the bytes */
#define GDX_COURSE_STATE_ERR_CRC (-65)       /* trailing CRC-32 does not match the bytes */
#define GDX_COURSE_STATE_ERR_ENTRY (-66)     /* bad course id, unknown flags, repeated course id, or a bind mismatch */

typedef struct GdxCourseStateEntry {
    char courseId[GDX_COURSE_FILE_ID_MAX + 1];
    uint32_t bindCrc;
    const GdxCourseExtRecord* records; /* NULL or recordCount 0 when the course has no records */
    int recordCount;
    int hasTitle;
    char title[GDX_COURSE_TITLE_MAX + 1];
} GdxCourseStateEntry;

/* Called once per entry by gdx_course_state_decode. The entry and its records are valid only during the call.
 * Returning a non-zero code stops the walk and that code is returned by the decoder. */
typedef int (*GdxCourseStateVisit)(void* ctx, const GdxCourseStateEntry* entry);

/* Encodes count entries into out. Course ids must be unique. Returns the file length or a negative code.
 * On error out holds a partial file and must be discarded. */
int gdx_course_state_encode(const GdxCourseStateEntry* entries, int count, unsigned char* out, size_t outCap);

/* Validates the whole file first (CRC, layout, every nested block). Only when that passes and visit is
 * not NULL does it walk the entries and call visit for each one, in file order. Returns GDX_COURSE_OK or a
 * negative GDX_COURSE_STATE_ERR_* / GDX_COURSE_EXT_ERR_* / GDX_COURSE_TITLE_ERR_* code. */
int gdx_course_state_decode(const unsigned char* data, size_t length, GdxCourseStateVisit visit, void* ctx);

/* Encodes the whole in-memory store (records and titles) as a GXS1 file. Returns the length or a negative code. */
int gdx_course_store_encode_file(unsigned char* out, size_t outCap);

/* Validates a GXS1 file and, only if it is wholly valid and fits the store, replaces the store with its
 * contents. On any error the store is unchanged. */
int gdx_course_store_restore_file(const unsigned char* data, size_t length);

/* ---------------------------------------------------------------------------------
 * Pack manifest line (elements.txt). One element per line:
 *
 *   kind|key|displayName|fallbackStockId[|path]
 *
 * kind is one of venue, skybox, bgm, part, effect. The fifth field (a music path relative to the pack
 * folder, bgm only) is optional. Blank lines and lines starting with '#' are skipped. Lines longer than
 * GDX_COURSE_MANIFEST_LINE_MAX bytes, lines with a byte outside printable ASCII, and any field that
 * fails its rule are rejected with a reason that starts with "line N: ".
 * ------------------------------------------------------------------------------- */

#define GDX_COURSE_MANIFEST_LINE_MAX 256
#define GDX_COURSE_MANIFEST_REASON_MAX 96

#define GDX_COURSE_MANIFEST_SKIP 0           /* blank or comment line */
#define GDX_COURSE_MANIFEST_ERR_ARGS (-70)
#define GDX_COURSE_MANIFEST_ERR_LONG (-71)   /* line longer than GDX_COURSE_MANIFEST_LINE_MAX */
#define GDX_COURSE_MANIFEST_ERR_TEXT (-72)   /* byte outside printable ASCII */
#define GDX_COURSE_MANIFEST_ERR_FIELDS (-73) /* not 4 or 5 fields */
#define GDX_COURSE_MANIFEST_ERR_KIND (-74)
#define GDX_COURSE_MANIFEST_ERR_KEY (-75)
#define GDX_COURSE_MANIFEST_ERR_NAME (-76)
#define GDX_COURSE_MANIFEST_ERR_FALLBACK (-77)
#define GDX_COURSE_MANIFEST_ERR_PATH (-78)

typedef struct GdxCourseManifestEntry {
    GdxCourseKind kind;
    char key[GDX_COURSE_KEY_MAX + 1];
    char displayName[GDX_COURSE_DISPLAY_MAX + 1];
    int fallbackId;
    char path[GDX_COURSE_PATH_MAX]; /* empty unless the line has a fifth field */
} GdxCourseManifestEntry;

/* Parses one line without its newline (a trailing '\r' is allowed). lineNumber is used only in the reason
 * text. Returns 1 and fills out for an element line, GDX_COURSE_MANIFEST_SKIP for a blank or comment line,
 * or a negative GDX_COURSE_MANIFEST_ERR_* code with reason set. reason may be NULL when reasonCap is 0. */
int gdx_course_manifest_parse_line(const char* line, size_t lineLength, int lineNumber,
                                   GdxCourseManifestEntry* out, char* reason, size_t reasonCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_REGISTRY_H */
