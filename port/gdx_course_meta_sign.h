/* port/gdx_course_meta_sign.h: the signature rules of creator metadata (plan item CM5), between the GXM1 codec
 * (gdx_course_meta.h) and the Ed25519 primitive (gdx_course_sign.h). Pure: no file IO, no RNG.
 *
 * The digest is gdx_sign_course_digest over the content bytes of the track (gdx::snapshot::content), so racing a
 * track, renaming it or moving it to another slot leaves a signature valid. Editing the track does not.
 *
 * Export: a signature that checks is kept, one that does not is dropped. A track with no signature is signed when
 * it was made here and the signing profile's name is its Author, so nobody signs another person's track.
 * Import: a signature that does not check is dropped, and the track installs without it.
 *
 * A valid signature proves that one key signed this name and this track. It does not prove who holds the key. */

#ifndef GDX_COURSE_META_SIGN_H
#define GDX_COURSE_META_SIGN_H

#include "gdx_course_meta.h"
#include "gdx_course_sign.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_META_SIGN_NONE 0    /* no signature, and none was added */
#define GDX_META_SIGN_KEPT 1    /* the signature checks and stays */
#define GDX_META_SIGN_SIGNED 2  /* the track was signed by this call */
#define GDX_META_SIGN_DROPPED 3 /* the signature did not check and was removed */

/* Keeps or drops the signature of meta against digest (NULL when the content could not be read, which drops it).
 * Returns NONE, KEPT or DROPPED. A NULL meta returns NONE. */
int gdx_meta_sign_check(GdxCourseMeta* meta, const unsigned char* digest);

/* gdx_meta_sign_check, then signs meta when it has no signature, madeHere is set, signerName equals the Author and
 * seed is not NULL. The secret key never leaves this call and is wiped before it returns. Returns NONE, KEPT,
 * SIGNED or DROPPED. An invalid signature replaced by a new one reports SIGNED. */
int gdx_meta_sign_for_export(GdxCourseMeta* meta, const unsigned char* digest, const char* signerName,
                             const unsigned char* seed);

/* Digest of a track's content bytes. gdx_meta_sign_digest_visit fits GdxSnapshotContentVisit
 * (gdx_course_snapshot_io.h): pass a GdxMetaSignDigest as ctx, with ok set to 0. */
typedef struct GdxMetaSignDigest {
    unsigned char digest[GDX_SIGN_DIGEST_BYTES];
    int ok;
} GdxMetaSignDigest;
void gdx_meta_sign_digest_visit(const unsigned char* bytes, unsigned long length, void* ctx);

/* Writes the fingerprint of the signing key ("1A2B-3C4D-5E6F-7081") and returns 1 when meta is signed. Writes ""
 * and returns 0 otherwise. */
int gdx_meta_sign_fingerprint(const GdxCourseMeta* meta, char out[20]);

#ifdef __cplusplus
}
#endif

#endif
