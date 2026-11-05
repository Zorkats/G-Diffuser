// port/gdx_course_sign.c: Ed25519 authorship signatures for course metadata. The signed message and the value record
// are described in gdx_course_sign.h.

#include "gdx_course_sign.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <string.h>

#define SIGN_MAGIC "GDXS1"
#define SIGN_MAGIC_BYTES 5
#define COURSE_TAG "GDXC1"
#define COURSE_TAG_BYTES 5
#define SIGN_MESSAGE_MAX (SIGN_MAGIC_BYTES + 1 + GDX_SIGN_NAME_MAX + GDX_SIGN_DIGEST_BYTES)
#define SECRET_PUBLIC_OFFSET 32
#define FINGERPRINT_BYTES 8

// Returns the length of a valid author name, or 0 when a rule is broken. The scan reads at most 21 bytes, so a long
// string is rejected early.
static size_t name_length(const char* name) {
    size_t length = 0;
    unsigned char c;

    while (name[length] != '\0') {
        c = (unsigned char) name[length];
        if ((length == GDX_SIGN_NAME_MAX) || (c < 0x20) || (c > 0x7E)) {
            return 0;
        }
        length++;
    }
    if ((length == 0) || (name[0] == ' ') || (name[length - 1] == ' ')) {
        return 0;
    }
    return length;
}

// Writes "GDXS1", nameLength, name and digest into message, and returns the message length. The name must be valid.
static size_t build_message(unsigned char message[SIGN_MESSAGE_MAX], const char* name, size_t nameLength,
                            const unsigned char digest[GDX_SIGN_DIGEST_BYTES]) {
    size_t pos;

    memcpy(message, SIGN_MAGIC, SIGN_MAGIC_BYTES);
    message[SIGN_MAGIC_BYTES] = (unsigned char) nameLength;
    memcpy(message + SIGN_MAGIC_BYTES + 1, name, nameLength);
    pos = SIGN_MAGIC_BYTES + 1 + nameLength;
    memcpy(message + pos, digest, GDX_SIGN_DIGEST_BYTES);
    return pos + GDX_SIGN_DIGEST_BYTES;
}

int gdx_sign_key_from_seed(const unsigned char seed[GDX_SIGN_SEED_BYTES],
                           unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES],
                           unsigned char secretKey[GDX_SIGN_SECRET_BYTES]) {
    unsigned char seedCopy[GDX_SIGN_SEED_BYTES];

    if ((seed == NULL) || (publicKey == NULL) || (secretKey == NULL)) {
        return -1;
    }
    // crypto_ed25519_key_pair wipes the seed it is given, so it gets a copy and the caller's seed stays intact.
    memcpy(seedCopy, seed, GDX_SIGN_SEED_BYTES);
    crypto_ed25519_key_pair(secretKey, publicKey, seedCopy);
    // Monocypher has wiped the copy already. The explicit wipe keeps the rule in this file.
    crypto_wipe(seedCopy, GDX_SIGN_SEED_BYTES);
    return 0;
}

int gdx_sign_course_digest(const unsigned char* course, size_t length, unsigned char digest[GDX_SIGN_DIGEST_BYTES]) {
    crypto_blake2b_ctx ctx;

    if ((digest == NULL) || ((course == NULL) && (length > 0))) {
        return -1;
    }
    // Incremental BLAKE2b: the prefix and the course go in as one message, so the course is never copied.
    crypto_blake2b_init(&ctx, GDX_SIGN_DIGEST_BYTES);
    crypto_blake2b_update(&ctx, (const unsigned char*) COURSE_TAG, COURSE_TAG_BYTES);
    if (length > 0) {
        crypto_blake2b_update(&ctx, course, length);
    }
    crypto_blake2b_final(&ctx, digest);
    return 0;
}

int gdx_sign_course(const unsigned char secretKey[GDX_SIGN_SECRET_BYTES], const char* author,
                    const unsigned char digest[GDX_SIGN_DIGEST_BYTES], unsigned char value[GDX_SIGN_VALUE_BYTES]) {
    unsigned char message[SIGN_MESSAGE_MAX];
    size_t nameLen;
    size_t messageLength;

    if ((secretKey == NULL) || (author == NULL) || (digest == NULL) || (value == NULL)) {
        return -1;
    }
    nameLen = name_length(author);
    if (nameLen == 0) {
        return -2;
    }
    messageLength = build_message(message, author, nameLen, digest);
    // Verified in monocypher-ed25519.c: crypto_ed25519_key_pair copies the public key to secret_key + 32, and
    // ed25519_dom_sign reads it from secret_key + 32. So secret key bytes 32..63 hold the public key.
    memcpy(value, secretKey + SECRET_PUBLIC_OFFSET, GDX_SIGN_PUBLIC_BYTES);
    crypto_ed25519_sign(value + GDX_SIGN_PUBLIC_BYTES, secretKey, message, messageLength);
    return 0;
}

int gdx_sign_verify(const unsigned char value[GDX_SIGN_VALUE_BYTES], const char* author,
                    const unsigned char digest[GDX_SIGN_DIGEST_BYTES]) {
    unsigned char message[SIGN_MESSAGE_MAX];
    size_t nameLen;
    size_t messageLength;

    if ((value == NULL) || (author == NULL) || (digest == NULL)) {
        return 0;
    }
    nameLen = name_length(author);
    if (nameLen == 0) {
        return 0;
    }
    messageLength = build_message(message, author, nameLen, digest);
    if (crypto_ed25519_check(value + GDX_SIGN_PUBLIC_BYTES, value, message, messageLength) == 0) {
        return 1;
    }
    return 0;
}

void gdx_sign_fingerprint(const unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES], char out[20]) {
    static const char hexDigits[] = "0123456789ABCDEF";
    unsigned char hash[GDX_SIGN_DIGEST_BYTES];
    size_t pos = 0;
    size_t i;

    if (out == NULL) {
        return;
    }
    if (publicKey == NULL) {
        out[0] = '\0';
        return;
    }
    crypto_blake2b(hash, GDX_SIGN_DIGEST_BYTES, publicKey, GDX_SIGN_PUBLIC_BYTES);
    for (i = 0; i < FINGERPRINT_BYTES; i++) {
        if ((i > 0) && ((i % 2) == 0)) {
            out[pos++] = '-';
        }
        out[pos++] = hexDigits[hash[i] >> 4];
        out[pos++] = hexDigits[hash[i] & 0x0F];
    }
    out[pos] = '\0';
}
