/* port/gdx_course_meta_sign.c: see gdx_course_meta_sign.h. */

#include "gdx_course_meta_sign.h"

#include <string.h>

#if GDX_META_SIGNATURE_BYTES != GDX_SIGN_VALUE_BYTES
#error "the GXM1 signature record must hold one gdx_course_sign value"
#endif

static void wipe(void* p, size_t n) {
    volatile unsigned char* bytes = (volatile unsigned char*) p;

    while (n-- > 0) {
        *bytes++ = 0;
    }
}

static void drop(GdxCourseMeta* meta) {
    meta->hasSignature = 0;
    memset(meta->signature, 0, sizeof(meta->signature));
}

int gdx_meta_sign_check(GdxCourseMeta* meta, const unsigned char* digest) {
    if ((meta == NULL) || !meta->hasSignature) {
        return GDX_META_SIGN_NONE;
    }
    if ((digest != NULL) && gdx_sign_verify(meta->signature, meta->author, digest)) {
        return GDX_META_SIGN_KEPT;
    }
    drop(meta);
    return GDX_META_SIGN_DROPPED;
}

int gdx_meta_sign_for_export(GdxCourseMeta* meta, const unsigned char* digest, const char* signerName,
                             const unsigned char* seed) {
    unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretKey[GDX_SIGN_SECRET_BYTES];
    unsigned char value[GDX_SIGN_VALUE_BYTES];
    const int checked = gdx_meta_sign_check(meta, digest);
    int rc;

    if ((checked == GDX_META_SIGN_KEPT) || (meta == NULL)) {
        return checked;
    }
    if ((digest == NULL) || (seed == NULL) || (signerName == NULL) || !meta->madeHere ||
        (meta->author[0] == '\0') || (strcmp(signerName, meta->author) != 0)) {
        return checked;
    }
    if (gdx_sign_key_from_seed(seed, publicKey, secretKey) != 0) {
        return checked;
    }
    rc = gdx_sign_course(secretKey, meta->author, digest, value);
    wipe(secretKey, sizeof(secretKey));
    if (rc != 0) {
        return checked;
    }
    memcpy(meta->signature, value, sizeof(meta->signature));
    meta->hasSignature = 1;
    return GDX_META_SIGN_SIGNED;
}

void gdx_meta_sign_digest_visit(const unsigned char* bytes, unsigned long length, void* ctx) {
    GdxMetaSignDigest* d = (GdxMetaSignDigest*) ctx;

    if (d != NULL) {
        d->ok = gdx_sign_course_digest(bytes, (size_t) length, d->digest) == 0;
    }
}

int gdx_meta_sign_fingerprint(const GdxCourseMeta* meta, char out[20]) {
    if (out == NULL) {
        return 0;
    }
    out[0] = '\0';
    if ((meta == NULL) || !meta->hasSignature) {
        return 0;
    }
    gdx_sign_fingerprint(meta->signature, out);
    return 1;
}
