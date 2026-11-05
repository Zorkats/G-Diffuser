// Converts an emulator SRAM save into G-Diffuser's native fzerox.sav image.
//
// The converter is pure: bytes in, bytes out. It does no file I/O and reads no game state. The
// caller reads the source file, calls gdx_save_import_convert, and writes the 0x8000-byte result
// only when the call returns 1.
#ifndef GDX_SAVE_IMPORT_H
#define GDX_SAVE_IMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

// Size of the native SaveContext image. fzerox.sav is exactly this many bytes.
#define GDX_SAVE_IMPORT_SRAM_SIZE 0x8000

// RetroArch parallel-n64 .srm image: eeprom, four mempacks, SRAM, then flash RAM, with DD left out.
// The SRAM block starts at 0x20800. This layout comes from the parallel-n64 source for one core,
// not from a real file, so it stays an assumption until a fixture confirms it.
#define GDX_SAVE_IMPORT_SRM_SIZE 0x48800
#define GDX_SAVE_IMPORT_SRM_BLOCK_OFFSET 0x20800

// Bits in GdxSaveImportInfo.validSections. A bit is set only when every instance of its section
// passes. The SETTINGS bit also requires the "F-ZERO X" name in both profile copies.
#define GDX_SAVE_IMPORT_SECTION_SETTINGS 0x01u
#define GDX_SAVE_IMPORT_SECTION_DEATH_RACE 0x02u
#define GDX_SAVE_IMPORT_SECTION_COURSES 0x04u
#define GDX_SAVE_IMPORT_SECTION_DD_CUPS 0x08u
#define GDX_SAVE_IMPORT_SECTION_GHOST_RECORD 0x10u
#define GDX_SAVE_IMPORT_SECTION_GHOST_DATA 0x20u
#define GDX_SAVE_IMPORT_SECTION_CHARACTERS 0x40u
#define GDX_SAVE_IMPORT_SECTION_CUP 0x80u
#define GDX_SAVE_IMPORT_SECTION_ALL 0xFFu

// Which input layout matched. NONE means the conversion failed.
typedef enum GdxSaveImportFormat {
    GDX_SAVE_IMPORT_FORMAT_NONE = 0,
    GDX_SAVE_IMPORT_FORMAT_NATIVE,
    GDX_SAVE_IMPORT_FORMAT_SRM_PARALLEL,
    GDX_SAVE_IMPORT_FORMAT_SRA_BIG_ENDIAN,
    GDX_SAVE_IMPORT_FORMAT_SRA_WORD_SWAPPED
} GdxSaveImportFormat;

typedef enum GdxSaveImportOpKind {
    GDX_SAVE_IMPORT_OP_RUN = 0,
    GDX_SAVE_IMPORT_OP_REPEAT
} GdxSaveImportOpKind;

// One step of the layout table. RUN covers count elements of elemSize bytes (1, 2 or 4).
// REPEAT covers count copies of sub, each stride bytes after the previous one. sub must span
// exactly stride bytes.
typedef struct GdxSaveImportOp {
    GdxSaveImportOpKind kind;
    unsigned long offset;
    unsigned long count;
    unsigned long elemSize;
    unsigned long stride;
    const struct GdxSaveImportOp* sub;
    unsigned long subCount;
} GdxSaveImportOp;

typedef struct GdxSaveImportInfo {
    GdxSaveImportFormat format;
    unsigned int validSections;
} GdxSaveImportInfo;

// Accepts a 0x8000-byte SRAM image (native, big-endian, or word-swapped) or a 0x48800-byte
// RetroArch .srm. On success it returns 1, writes GDX_SAVE_IMPORT_SRAM_SIZE bytes to out, and sets
// validSections to GDX_SAVE_IMPORT_SECTION_ALL. Emulator formats come from the cartridge game, so
// their profile union (cartridge Edit Cup) is cleared to empty DD cups; a native image is copied
// as is. On failure it returns 0, leaves out untouched,
// sets format to NONE, and reports in validSections the best partial result seen, for diagnostics
// only. in and out may alias. info may be NULL.
int gdx_save_import_convert(const unsigned char* in, unsigned long inSize, unsigned char* out,
                            GdxSaveImportInfo* info);

// Returns 1 when ops tile [0, totalSize) exactly: each op starts where the previous one ended, and
// each REPEAT's sub-table spans its stride.
int gdx_save_import_check_layout(const GdxSaveImportOp* ops, unsigned long opCount,
                                 unsigned long totalSize);

// Returns 1 when the built-in layout table passes gdx_save_import_check_layout over the full image.
int gdx_save_import_layout_is_consistent(void);

#ifdef __cplusplus
}
#endif

#endif
