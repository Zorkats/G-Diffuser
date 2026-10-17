/* port/gdx_content_pack.h -- .gdxp share packages: one file that carries a whole export.
 *
 * A track or cup export is a .gdxc container plus up to three sidecars beside it (<stem>.gdxe extension
 * keys, <stem>.gdxt title, <stem>.gdxs bundle companion) and the audio of its custom BGM choices. A
 * .gdxp is a plain zip of exactly those files, unchanged, so one file travels instead of several and an
 * older build can still import the files after unzipping them.
 *
 * Members must be plain file names (no folders), with a .gdxc, .gdxe, .gdxt, .gdxs, .ogg, .flac or .wav
 * extension. A package holds exactly one .gdxc, and every sidecar shares its stem. Sizes are capped
 * before anything is read: a data member at GDX_CONTENT_PACK_MAX_DATA_BYTES, an audio member at
 * GDX_CONTENT_PACK_MAX_AUDIO_BYTES (the 7A copy limit) and the whole package at
 * GDX_CONTENT_PACK_MAX_TOTAL_BYTES. Encrypted members are refused. The zip CRC of every extracted member
 * is checked by libzip.
 *
 * Plain int sizes keep this header free of <stddef.h>, which the decomp include path shadows.
 */

#ifndef GDX_CONTENT_PACK_H
#define GDX_CONTENT_PACK_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_CONTENT_PACK_EXT ".gdxp"
/* Working folders inside exports/. The leading dot keeps them out of the import list. */
#define GDX_CONTENT_PACK_EXPORT_WORK_DIR ".gdxp-export"
#define GDX_CONTENT_PACK_IMPORT_WORK_DIR ".gdxp-import"
#define GDX_CONTENT_PACK_MAX_MEMBERS 16
#define GDX_CONTENT_PACK_NAME_MAX 128
#define GDX_CONTENT_PACK_MAX_DATA_BYTES (4u * 1024u * 1024u)
#define GDX_CONTENT_PACK_MAX_AUDIO_BYTES (32u * 1024u * 1024u)
/* Six bundle tracks with one song each, plus the data members. */
#define GDX_CONTENT_PACK_MAX_TOTAL_BYTES (6u * GDX_CONTENT_PACK_MAX_AUDIO_BYTES + 4u * GDX_CONTENT_PACK_MAX_DATA_BYTES)

#define GDX_CONTENT_PACK_OK 0
#define GDX_CONTENT_PACK_ERR_ARGS (-1)
#define GDX_CONTENT_PACK_ERR_IO (-2)          /* a host file or folder could not be read or written */
#define GDX_CONTENT_PACK_ERR_NOT_ZIP (-3)     /* the package is not a readable zip */
#define GDX_CONTENT_PACK_ERR_MEMBER (-4)      /* unsafe or unknown member name, or an encrypted member */
#define GDX_CONTENT_PACK_ERR_CONTAINER (-5)   /* no .gdxc, or more than one */
#define GDX_CONTENT_PACK_ERR_STEM (-6)        /* a sidecar does not share the .gdxc stem */
#define GDX_CONTENT_PACK_ERR_TOO_LARGE (-7)   /* a member or the whole package is over its cap */
#define GDX_CONTENT_PACK_ERR_TOO_MANY (-8)    /* more than GDX_CONTENT_PACK_MAX_MEMBERS members */
#define GDX_CONTENT_PACK_ERR_DUPLICATE (-9)   /* two members with the same name, ignoring case */
#define GDX_CONTENT_PACK_ERR_CORRUPT (-10)    /* a member failed its zip CRC or size check */

/* Makes dir exist and deletes the regular files directly inside it (never recursing). With removeDir,
 * the then empty folder is removed as well. Returns GDX_CONTENT_PACK_OK or ERR_IO. */
int gdx_content_pack_clear_dir(const char* dir, int removeDir);

/* Writes every regular file directly inside dir into the package packPath, replacing it. The files
 * must form a valid package (same rules as extraction). Audio is stored, the rest is deflated. */
int gdx_content_pack_write_dir(const char* packPath, const char* dir);

/* Checks every member of packPath, then clears destDir (gdx_content_pack_clear_dir) and extracts the
 * members into it. withAudio 0 skips the audio members, for a listing that only reads the container
 * and its sidecars. outContainer receives the .gdxc member name. Nothing is extracted when a check
 * fails, and a failure during extraction clears destDir again. */
int gdx_content_pack_extract(const char* packPath, const char* destDir, int withAudio, char* outContainer,
                             int outCap);

/* 1 when name ends in ".gdxp", ignoring case. */
int gdx_content_pack_is_package_name(const char* name);

const char* gdx_content_pack_error_string(int rc);

#ifdef __cplusplus
}
#endif

#endif /* GDX_CONTENT_PACK_H */
