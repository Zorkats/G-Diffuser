/* port/gdx_update.h -- in-game update check, download and apply. */

#ifndef GDX_UPDATE_H
#define GDX_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GdxUpdateState {
    GDX_UPDATE_IDLE = 0,
    GDX_UPDATE_CHECKING,
    GDX_UPDATE_UP_TO_DATE,
    GDX_UPDATE_AVAILABLE,
    GDX_UPDATE_DOWNLOADING,
    GDX_UPDATE_VERIFYING,
    GDX_UPDATE_READY,
    GDX_UPDATE_ERROR,
    GDX_UPDATE_DISABLED
} GdxUpdateState;

/* releaseNotes holds at most 8 KB including the terminating NUL. */
typedef struct GdxUpdateStatus {
    GdxUpdateState state;
    char latestVersion[128];
    char releaseNotes[8192];
    uint64_t bytesDone;
    uint64_t bytesTotal;
    char errorMessage[256];
} GdxUpdateStatus;

void gdx_update_start_check(int manual);
void gdx_update_poll(GdxUpdateStatus* out);
void gdx_update_start_download(void);
void gdx_update_apply_and_restart(void);
void gdx_update_cleanup_on_boot(void);

/* HTML page for this channel, not the API URL. */
const char* gdx_update_release_page_url(void);

/* -1 if a is older, 0 if equal or not dotted integers, 1 if a is newer.
   One leading v is ignored. A missing trailing component counts as 0. */
int gdx_update_compare_stable(const char* a, const char* b);

/* dateOut needs 9 bytes, hashOut needs 41. Returns 0 on success, -1 otherwise. */
int gdx_update_parse_nightly(const char* version, char* dateOut, char* hashOut);

/* 1 when assetName is the release package for platform, otherwise 0. */
int gdx_update_asset_matches(const char* assetName, const char* platform);

/* Writes 64 lowercase hex digits and a NUL. Returns 0 if assetName has a line, -1 if not. */
int gdx_update_find_sha256(const char* sumsText, const char* assetName, char out[65]);

/* FIPS 180-4 digest. data may be NULL when len is 0. */
void gdx_update_sha256(const void* data, size_t len, unsigned char out[32]);

#ifdef __cplusplus
}
#endif

#endif
