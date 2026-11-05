// port/tests/gdx_course_sign_tests.c: checks for port/gdx_course_sign.c. Plain C with a CHECK macro and no framework.
// Prints one PASS or FAIL line per check. Exits 0 only when every check passes.

#include "gdx_course_sign.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define COURSE_BYTES 2016
#define COURSE_SEED 0x2545F491u

// RFC 8032 section 7.1: TEST 1 (empty message) and TEST 2 (message 0x72).
static const char kRfc1Seed[] = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char kRfc1Public[] = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char kRfc1Signature[] = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
static const char kRfc2Seed[] = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
static const char kRfc2Public[] = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
static const char kRfc2Signature[] = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";

static int sFailures = 0;

static void record(int ok, const char* text) {
    if (ok) {
        printf("PASS: %s\n", text);
    } else {
        printf("FAIL: %s\n", text);
        sFailures++;
    }
}

static void record_named(int ok, const char* name, const char* what) {
    char text[160];

    snprintf(text, sizeof text, "%s: %s", name, what);
    record(ok, text);
}

#define CHECK(cond, text) record((cond) != 0, (text))

static int hex_digit(char c) {
    if ((c >= '0') && (c <= '9')) {
        return c - '0';
    }
    if ((c >= 'a') && (c <= 'f')) {
        return c - 'a' + 10;
    }
    if ((c >= 'A') && (c <= 'F')) {
        return c - 'A' + 10;
    }
    return -1;
}

// Decodes exactly outLength bytes. Returns 0, or -1 when the string has the wrong length or a non-hex digit.
static int from_hex(const char* hex, unsigned char* out, size_t outLength) {
    size_t i;
    int high;
    int low;

    if (strlen(hex) != 2 * outLength) {
        return -1;
    }
    for (i = 0; i < outLength; i++) {
        high = hex_digit(hex[2 * i]);
        low = hex_digit(hex[2 * i + 1]);
        if ((high < 0) || (low < 0)) {
            return -1;
        }
        out[i] = (unsigned char) ((high << 4) | low);
    }
    return 0;
}

// Fills course with xorshift32 output, so the fake course is the same on every run.
static void fill_course(unsigned char* course, size_t length, uint32_t state) {
    size_t i;

    for (i = 0; i < length; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        course[i] = (unsigned char) (state & 0xFFu);
    }
}

typedef struct Fixture {
    unsigned char course[COURSE_BYTES];
    unsigned char digest[GDX_SIGN_DIGEST_BYTES];
    unsigned char publicA[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretA[GDX_SIGN_SECRET_BYTES];
    unsigned char publicB[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretB[GDX_SIGN_SECRET_BYTES];
    unsigned char value[GDX_SIGN_VALUE_BYTES];
} Fixture;

static Fixture sFix;

// Builds the shared fixture: a fake course, its digest, two key pairs, and a value signed as "Zor" by key A.
static int make_fixture(Fixture* f) {
    unsigned char seedA[GDX_SIGN_SEED_BYTES];
    unsigned char seedB[GDX_SIGN_SEED_BYTES];

    if (from_hex(kRfc1Seed, seedA, GDX_SIGN_SEED_BYTES) != 0) {
        return -1;
    }
    if (from_hex(kRfc2Seed, seedB, GDX_SIGN_SEED_BYTES) != 0) {
        return -1;
    }
    fill_course(f->course, COURSE_BYTES, COURSE_SEED);
    if (gdx_sign_key_from_seed(seedA, f->publicA, f->secretA) != 0) {
        return -1;
    }
    if (gdx_sign_key_from_seed(seedB, f->publicB, f->secretB) != 0) {
        return -1;
    }
    if (gdx_sign_course_digest(f->course, COURSE_BYTES, f->digest) != 0) {
        return -1;
    }
    if (gdx_sign_course(f->secretA, "Zor", f->digest, f->value) != 0) {
        return -1;
    }
    return 0;
}

// Checks key_from_seed and the secret key layout against one RFC 8032 vector, and signs with Monocypher directly.
static void check_rfc_vector(const char* name, const char* seedHex, const char* publicHex, const char* signatureHex,
                             const unsigned char* message, size_t messageLength) {
    unsigned char seed[GDX_SIGN_SEED_BYTES];
    unsigned char seedBefore[GDX_SIGN_SEED_BYTES];
    unsigned char expectPublic[GDX_SIGN_PUBLIC_BYTES];
    unsigned char expectSignature[64];
    unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretKey[GDX_SIGN_SECRET_BYTES];
    unsigned char signature[64];

    record_named(from_hex(seedHex, seed, GDX_SIGN_SEED_BYTES) == 0
                     && from_hex(publicHex, expectPublic, GDX_SIGN_PUBLIC_BYTES) == 0
                     && from_hex(signatureHex, expectSignature, 64) == 0,
                 name, "vectors decode");
    memcpy(seedBefore, seed, GDX_SIGN_SEED_BYTES);
    record_named(gdx_sign_key_from_seed(seed, publicKey, secretKey) == 0, name, "key_from_seed returns 0");
    record_named(memcmp(publicKey, expectPublic, GDX_SIGN_PUBLIC_BYTES) == 0, name, "public key matches");
    record_named(memcmp(seed, seedBefore, GDX_SIGN_SEED_BYTES) == 0, name, "seed is unchanged");
    record_named(memcmp(secretKey, seed, GDX_SIGN_SEED_BYTES) == 0
                     && memcmp(secretKey + GDX_SIGN_SEED_BYTES, publicKey, GDX_SIGN_PUBLIC_BYTES) == 0,
                 name, "secret key is seed then public key");
    crypto_ed25519_sign(signature, secretKey, message, messageLength);
    record_named(memcmp(signature, expectSignature, 64) == 0, name, "signature matches (Monocypher direct)");
    record_named(crypto_ed25519_check(expectSignature, expectPublic, message, messageLength) == 0, name,
                 "Monocypher accepts the vector");
}

static void test_rfc8032(void) {
    static const unsigned char kNoMessage[1] = {0};
    static const unsigned char kOneByte[1] = {0x72};

    check_rfc_vector("RFC 8032 test 1", kRfc1Seed, kRfc1Public, kRfc1Signature, kNoMessage, 0);
    check_rfc_vector("RFC 8032 test 2", kRfc2Seed, kRfc2Public, kRfc2Signature, kOneByte, 1);
}

static void test_round_trip(void) {
    CHECK(memcmp(sFix.value, sFix.publicA, GDX_SIGN_PUBLIC_BYTES) == 0, "value starts with the signer's public key");
    CHECK(gdx_sign_verify(sFix.value, "Zor", sFix.digest) == 1, "2016-byte course signed as Zor verifies");
}

static void test_tamper(void) {
    unsigned char course[COURSE_BYTES];
    unsigned char digest[GDX_SIGN_DIGEST_BYTES];
    unsigned char value[GDX_SIGN_VALUE_BYTES];
    int accepted;
    int i;

    memcpy(course, sFix.course, COURSE_BYTES);
    course[1000] ^= 0x01;
    gdx_sign_course_digest(course, COURSE_BYTES, digest);
    CHECK(gdx_sign_verify(sFix.value, "Zor", digest) == 0, "one flipped course byte (digest recomputed) fails verify");
    CHECK(gdx_sign_verify(sFix.value, "Zor ", sFix.digest) == 0, "author 'Zor ' fails verify");
    CHECK(gdx_sign_verify(sFix.value, "Zoe", sFix.digest) == 0, "author 'Zoe' fails verify");

    accepted = 0;
    for (i = 0; i < 64; i++) {
        memcpy(value, sFix.value, GDX_SIGN_VALUE_BYTES);
        value[GDX_SIGN_PUBLIC_BYTES + i] ^= (unsigned char) (1u << (i % 8));
        if (gdx_sign_verify(value, "Zor", sFix.digest) != 0) {
            accepted++;
        }
    }
    CHECK(accepted == 0, "a flipped bit in each of the 64 signature bytes fails verify");

    accepted = 0;
    for (i = 0; i < GDX_SIGN_PUBLIC_BYTES; i++) {
        memcpy(value, sFix.value, GDX_SIGN_VALUE_BYTES);
        value[i] ^= (unsigned char) (1u << (i % 8));
        if (gdx_sign_verify(value, "Zor", sFix.digest) != 0) {
            accepted++;
        }
    }
    CHECK(accepted == 0, "a flipped bit in each of the 32 public key bytes fails verify");

    CHECK(gdx_sign_course(sFix.secretB, "Zor", sFix.digest, value) == 0, "key B signs the same name and digest");
    CHECK(gdx_sign_verify(value, "Zor", sFix.digest) == 1, "key B value verifies before the splice");
    memcpy(value, sFix.publicA, GDX_SIGN_PUBLIC_BYTES);
    CHECK(gdx_sign_verify(value, "Zor", sFix.digest) == 0,
          "key B signature with key A public key spliced in fails verify");
}

static void test_name_rules(void) {
    unsigned char value[GDX_SIGN_VALUE_BYTES];
    int stillUntouched;
    int i;

    memset(value, 0xAA, sizeof value);
    CHECK(gdx_sign_course(sFix.secretA, "", sFix.digest, value) == -2, "empty name gives -2");
    CHECK(gdx_sign_course(sFix.secretA, " ", sFix.digest, value) == -2, "a single space gives -2");
    CHECK(gdx_sign_course(sFix.secretA, " Zor", sFix.digest, value) == -2, "leading space gives -2");
    CHECK(gdx_sign_course(sFix.secretA, "Zor ", sFix.digest, value) == -2, "trailing space gives -2");
    CHECK(gdx_sign_course(sFix.secretA, "ABCDEFGHIJKLMNOPQRSTU", sFix.digest, value) == -2, "21 characters give -2");
    CHECK(gdx_sign_course(sFix.secretA, "Zor\x7f", sFix.digest, value) == -2, "a 0x7F byte gives -2");
    CHECK(gdx_sign_course(sFix.secretA, "Zo\tr", sFix.digest, value) == -2, "a tab gives -2");

    stillUntouched = 1;
    for (i = 0; i < GDX_SIGN_VALUE_BYTES; i++) {
        if (value[i] != 0xAA) {
            stillUntouched = 0;
        }
    }
    CHECK(stillUntouched, "value is not written when the name is rejected");

    CHECK(gdx_sign_course(sFix.secretA, "Zo r", sFix.digest, value) == 0, "a space inside the name is allowed");
    CHECK(gdx_sign_course(sFix.secretA, "ABCDEFGHIJKLMNOPQRST", sFix.digest, value) == 0,
          "a 20-character name is accepted");
    CHECK(gdx_sign_verify(value, "ABCDEFGHIJKLMNOPQRST", sFix.digest) == 1, "the 20-character name verifies");
    CHECK(gdx_sign_verify(value, "ABCDEFGHIJKLMNOPQRS", sFix.digest) == 0,
          "a 19-character prefix of that name fails verify");
}

static void test_digest(void) {
    unsigned char course[COURSE_BYTES];
    unsigned char first[GDX_SIGN_DIGEST_BYTES];
    unsigned char second[GDX_SIGN_DIGEST_BYTES];
    unsigned char plain[GDX_SIGN_DIGEST_BYTES];
    unsigned char expect[GDX_SIGN_DIGEST_BYTES];
    unsigned char prefixed[COURSE_BYTES + 5];

    memcpy(course, sFix.course, COURSE_BYTES);
    CHECK(gdx_sign_course_digest(course, COURSE_BYTES, first) == 0, "course_digest returns 0");
    CHECK(gdx_sign_course_digest(course, COURSE_BYTES, second) == 0 && memcmp(first, second, GDX_SIGN_DIGEST_BYTES) == 0,
          "same bytes give the same digest");

    course[777] ^= 0x10;
    gdx_sign_course_digest(course, COURSE_BYTES, second);
    CHECK(memcmp(first, second, GDX_SIGN_DIGEST_BYTES) != 0, "one changed byte changes the digest");
    course[777] ^= 0x10;

    crypto_blake2b(plain, GDX_SIGN_DIGEST_BYTES, course, COURSE_BYTES);
    CHECK(memcmp(first, plain, GDX_SIGN_DIGEST_BYTES) != 0,
          "digest differs from plain BLAKE2b-256 of the bytes without the GDXC1 prefix");

    memcpy(prefixed, "GDXC1", 5);
    memcpy(prefixed + 5, course, COURSE_BYTES);
    crypto_blake2b(expect, GDX_SIGN_DIGEST_BYTES, prefixed, COURSE_BYTES + 5);
    CHECK(memcmp(first, expect, GDX_SIGN_DIGEST_BYTES) == 0,
          "digest equals one-shot BLAKE2b-256 of GDXC1 followed by the course bytes");

    crypto_blake2b(expect, GDX_SIGN_DIGEST_BYTES, (const unsigned char*) "GDXC1", 5);
    CHECK(gdx_sign_course_digest(NULL, 0, second) == 0 && memcmp(second, expect, GDX_SIGN_DIGEST_BYTES) == 0,
          "an empty course hashes only the prefix");
}

static void test_fingerprint(void) {
    char fp[20];
    char again[20];
    char expect[20];
    unsigned char hash[GDX_SIGN_DIGEST_BYTES];
    int shapeOk;
    int i;

    memset(fp, 'X', sizeof fp);
    gdx_sign_fingerprint(sFix.publicA, fp);
    CHECK((fp[19] == '\0') && (strlen(fp) == 19), "fingerprint is 19 characters plus NUL");

    shapeOk = 1;
    for (i = 0; i < 19; i++) {
        if ((i == 4) || (i == 9) || (i == 14)) {
            if (fp[i] != '-') {
                shapeOk = 0;
            }
        } else if (!(((fp[i] >= '0') && (fp[i] <= '9')) || ((fp[i] >= 'A') && (fp[i] <= 'F')))) {
            shapeOk = 0;
        }
    }
    CHECK(shapeOk, "dashes at 4, 9 and 14, uppercase hex elsewhere");

    crypto_blake2b(hash, GDX_SIGN_DIGEST_BYTES, sFix.publicA, GDX_SIGN_PUBLIC_BYTES);
    snprintf(expect, sizeof expect, "%02X%02X-%02X%02X-%02X%02X-%02X%02X", hash[0], hash[1], hash[2], hash[3],
             hash[4], hash[5], hash[6], hash[7]);
    CHECK((fp[19] == '\0') && (strcmp(fp, expect) == 0),
          "fingerprint is the first 8 bytes of BLAKE2b-256 of the public key");

    gdx_sign_fingerprint(sFix.publicA, again);
    CHECK((again[19] == '\0') && (strcmp(fp, again) == 0), "fingerprint is stable for the same key");
    gdx_sign_fingerprint(sFix.publicB, again);
    CHECK((again[19] == '\0') && (strcmp(fp, again) != 0), "another key gives another fingerprint");

    memset(fp, 'X', sizeof fp);
    gdx_sign_fingerprint(NULL, fp);
    CHECK(fp[0] == '\0', "NULL key writes an empty string");
}

static void test_null_arguments(void) {
    unsigned char seed[GDX_SIGN_SEED_BYTES];
    unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretKey[GDX_SIGN_SECRET_BYTES];
    unsigned char digest[GDX_SIGN_DIGEST_BYTES];
    unsigned char value[GDX_SIGN_VALUE_BYTES];
    char fp[20];

    from_hex(kRfc1Seed, seed, GDX_SIGN_SEED_BYTES);
    CHECK(gdx_sign_key_from_seed(NULL, publicKey, secretKey) == -1, "key_from_seed: NULL seed gives -1");
    CHECK(gdx_sign_key_from_seed(seed, NULL, secretKey) == -1, "key_from_seed: NULL public key gives -1");
    CHECK(gdx_sign_key_from_seed(seed, publicKey, NULL) == -1, "key_from_seed: NULL secret key gives -1");

    CHECK(gdx_sign_course_digest(NULL, 5, digest) == -1, "course_digest: NULL course with length 5 gives -1");
    CHECK(gdx_sign_course_digest(sFix.course, 5, NULL) == -1, "course_digest: NULL digest gives -1");
    CHECK(gdx_sign_course_digest(NULL, 0, digest) == 0, "course_digest: NULL course with length 0 is allowed");

    CHECK(gdx_sign_course(NULL, "Zor", digest, value) == -1, "sign_course: NULL secret key gives -1");
    CHECK(gdx_sign_course(sFix.secretA, NULL, digest, value) == -1, "sign_course: NULL author gives -1");
    CHECK(gdx_sign_course(sFix.secretA, "Zor", NULL, value) == -1, "sign_course: NULL digest gives -1");
    CHECK(gdx_sign_course(sFix.secretA, "Zor", digest, NULL) == -1, "sign_course: NULL value gives -1");

    CHECK(gdx_sign_verify(NULL, "Zor", digest) == 0, "verify: NULL value gives 0");
    CHECK(gdx_sign_verify(sFix.value, NULL, digest) == 0, "verify: NULL author gives 0");
    CHECK(gdx_sign_verify(sFix.value, "Zor", NULL) == 0, "verify: NULL digest gives 0");

    memset(fp, 'X', sizeof fp);
    gdx_sign_fingerprint(NULL, fp);
    CHECK(fp[0] == '\0', "fingerprint: NULL key writes an empty string");
    gdx_sign_fingerprint(sFix.publicA, NULL);
    CHECK(1, "fingerprint: NULL out returns without a crash");
}

int main(void) {
    test_rfc8032();
    CHECK(make_fixture(&sFix) == 0, "fixture: keys, digest and signed value are built");
    test_round_trip();
    test_tamper();
    test_name_rules();
    test_digest();
    test_fingerprint();
    test_null_arguments();

    if (sFailures == 0) {
        printf("all checks passed: 0 failure(s)\n");
        return 0;
    }
    printf("checks failed: %d failure(s)\n", sFailures);
    return 1;
}
