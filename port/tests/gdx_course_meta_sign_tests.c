/* Unit tests for port/gdx_course_meta_sign.c, compiled UNMODIFIED with gdx_course_sign.c, gdx_course_meta.c,
 * gdx_course_registry.c and libultraship's Monocypher. */
#include "../gdx_course_meta_sign.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

#define CHECK(condition, what)                                          \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAILED line %d: %s\n", __LINE__, what);             \
            sFailures++;                                                \
        }                                                               \
    } while (0)

static unsigned char sSeedA[GDX_SIGN_SEED_BYTES];
static unsigned char sSeedB[GDX_SIGN_SEED_BYTES];
static unsigned char sDigest[GDX_SIGN_DIGEST_BYTES];
static unsigned char sOtherDigest[GDX_SIGN_DIGEST_BYTES];

static void setup(void) {
    static const unsigned char course[] = "a track the author made";
    static const unsigned char edited[] = "a track the author made!";
    int i;

    for (i = 0; i < GDX_SIGN_SEED_BYTES; i++) {
        sSeedA[i] = (unsigned char) (i + 1);
        sSeedB[i] = (unsigned char) (0xF0 - i);
    }
    gdx_sign_course_digest(course, sizeof(course) - 1, sDigest);
    gdx_sign_course_digest(edited, sizeof(edited) - 1, sOtherDigest);
}

static GdxCourseMeta made_here(const char* author) {
    GdxCourseMeta meta;

    memset(&meta, 0, sizeof(meta));
    snprintf(meta.author, sizeof(meta.author), "%s", author);
    meta.madeHere = 1;
    return meta;
}

static void test_signing_rule(void) {
    GdxCourseMeta meta = made_here("Zorkats");
    GdxCourseMeta other;
    unsigned char seedCopy[GDX_SIGN_SEED_BYTES];
    unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretKey[GDX_SIGN_SECRET_BYTES];

    memcpy(seedCopy, sSeedA, sizeof(seedCopy));
    CHECK(gdx_meta_sign_for_export(&meta, sDigest, "Zorkats", sSeedA) == GDX_META_SIGN_SIGNED,
          "the author signs a track made here");
    CHECK(memcmp(seedCopy, sSeedA, sizeof(seedCopy)) == 0, "signing leaves the seed alone");
    CHECK(meta.hasSignature == 1, "the signature flag is set");
    CHECK(gdx_meta_validate(&meta) == GDX_META_OK, "a signed meta is a legal meta");
    CHECK(gdx_sign_verify(meta.signature, "Zorkats", sDigest), "the signature checks for the author");
    gdx_sign_key_from_seed(sSeedA, publicKey, secretKey);
    CHECK(memcmp(meta.signature, publicKey, GDX_SIGN_PUBLIC_BYTES) == 0, "the value names the signer's key");

    other = meta;
    CHECK(gdx_meta_sign_for_export(&other, sDigest, "Zorkats", sSeedB) == GDX_META_SIGN_KEPT,
          "a valid signature is kept, not re-signed by another key");
    CHECK(memcmp(other.signature, meta.signature, GDX_META_SIGNATURE_BYTES) == 0, "the kept signature is unchanged");

    other = made_here("Zorkats");
    CHECK(gdx_meta_sign_for_export(&other, sDigest, "Heebeejeebees", sSeedA) == GDX_META_SIGN_NONE,
          "nobody signs another person's track");
    CHECK(other.hasSignature == 0, "a refused sign writes nothing");
    other = made_here("Zorkats");
    other.madeHere = 0;
    CHECK(gdx_meta_sign_for_export(&other, sDigest, "Zorkats", sSeedA) == GDX_META_SIGN_NONE,
          "a track from elsewhere is not signed, even under the same name");
    other = made_here("Zorkats");
    CHECK(gdx_meta_sign_for_export(&other, sDigest, "Zorkats", NULL) == GDX_META_SIGN_NONE, "no seed, no sign");
    CHECK(gdx_meta_sign_for_export(&other, sDigest, NULL, sSeedA) == GDX_META_SIGN_NONE, "no signer, no sign");
    CHECK(gdx_meta_sign_for_export(&other, NULL, "Zorkats", sSeedA) == GDX_META_SIGN_NONE, "no content, no sign");
    other = made_here("Zorkats");
    other.author[0] = '\0';
    CHECK(gdx_meta_sign_for_export(&other, sDigest, "", sSeedA) == GDX_META_SIGN_NONE, "no author, no sign");
    other = made_here("zorkats");
    CHECK(gdx_meta_sign_for_export(&other, sDigest, "Zorkats", sSeedA) == GDX_META_SIGN_NONE,
          "the name match is exact");
    CHECK(gdx_meta_sign_for_export(NULL, sDigest, "Zorkats", sSeedA) == GDX_META_SIGN_NONE, "a NULL meta is ignored");
}

static void test_stale_signatures(void) {
    GdxCourseMeta meta = made_here("Zorkats");
    GdxCourseMeta copy;

    gdx_meta_sign_for_export(&meta, sDigest, "Zorkats", sSeedA);

    copy = meta;
    CHECK(gdx_meta_sign_check(&copy, sDigest) == GDX_META_SIGN_KEPT, "import keeps a signature that checks");
    copy = meta;
    CHECK(gdx_meta_sign_check(&copy, sOtherDigest) == GDX_META_SIGN_DROPPED, "an edited track drops it");
    CHECK((copy.hasSignature == 0) && (copy.signature[0] == 0) && (copy.signature[95] == 0),
          "a dropped signature is cleared");
    copy = meta;
    CHECK(gdx_meta_sign_check(&copy, NULL) == GDX_META_SIGN_DROPPED, "unreadable content drops it");
    copy = meta;
    snprintf(copy.author, sizeof(copy.author), "%s", "Heebeejeebees");
    CHECK(gdx_meta_sign_check(&copy, sDigest) == GDX_META_SIGN_DROPPED, "a changed Author drops it");
    copy = meta;
    copy.signature[GDX_SIGN_PUBLIC_BYTES + 5] ^= 0x10;
    CHECK(gdx_meta_sign_check(&copy, sDigest) == GDX_META_SIGN_DROPPED, "a forged signature byte drops it");
    copy = meta;
    copy.signature[3] ^= 0x01;
    CHECK(gdx_meta_sign_check(&copy, sDigest) == GDX_META_SIGN_DROPPED, "a swapped key drops it");

    copy = made_here("Zorkats");
    CHECK(gdx_meta_sign_check(&copy, sDigest) == GDX_META_SIGN_NONE, "an unsigned track is left alone");
    CHECK(gdx_meta_sign_check(NULL, sDigest) == GDX_META_SIGN_NONE, "a NULL meta is ignored");

    /* The author edits and re-exports: the stale signature goes and a fresh one comes. */
    copy = meta;
    CHECK(gdx_meta_sign_for_export(&copy, sOtherDigest, "Zorkats", sSeedA) == GDX_META_SIGN_SIGNED,
          "the author re-signs an edited track");
    CHECK(gdx_sign_verify(copy.signature, "Zorkats", sOtherDigest), "the new signature covers the edit");
    /* Someone else exports an edited copy: the signature just goes. */
    copy = meta;
    copy.madeHere = 0;
    CHECK(gdx_meta_sign_for_export(&copy, sOtherDigest, "Zorkats", sSeedA) == GDX_META_SIGN_DROPPED,
          "a stale signature on a track from elsewhere is dropped on export");
    CHECK(copy.hasSignature == 0, "and nothing replaces it");
}

static void test_fingerprint(void) {
    GdxCourseMeta meta = made_here("Zorkats");
    unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretKey[GDX_SIGN_SECRET_BYTES];
    char expected[20];
    char out[20];

    memset(out, 'x', sizeof(out));
    CHECK((gdx_meta_sign_fingerprint(&meta, out) == 0) && (out[0] == '\0'), "an unsigned track has no fingerprint");
    CHECK(gdx_meta_sign_fingerprint(NULL, out) == 0, "a NULL meta has no fingerprint");
    CHECK(gdx_meta_sign_fingerprint(&meta, NULL) == 0, "a NULL out is ignored");

    gdx_meta_sign_for_export(&meta, sDigest, "Zorkats", sSeedA);
    gdx_sign_key_from_seed(sSeedA, publicKey, secretKey);
    gdx_sign_fingerprint(publicKey, expected);
    CHECK((gdx_meta_sign_fingerprint(&meta, out) == 1) && (strcmp(out, expected) == 0) && (strlen(out) == 19),
          "the fingerprint is the signing key's");
}

int main(void) {
    setup();
    test_signing_rule();
    test_stale_signatures();
    test_fingerprint();
    if (sFailures != 0) {
        printf("%d failure(s)\n", sFailures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
