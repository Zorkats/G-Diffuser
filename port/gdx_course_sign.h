// port/gdx_course_sign.h: Ed25519 authorship signatures for course metadata (plan item CM5). Pure crypto: no file IO,
// no RNG, no networking. The caller supplies seeds, keys and course bytes, and this module stores nothing.
//
// The signed message is these bytes, in order:
//
//   "GDXS1"        5 bytes, ASCII
//   nameLength     u8, 1..GDX_SIGN_NAME_MAX
//   name           nameLength bytes, printable ASCII (0x20..0x7E), no space at either end
//   digest         32 bytes, BLAKE2b-256 of "GDXC1" followed by the course bytes
//
// The "GDXC1" prefix keeps course digests apart from other BLAKE2b hashes. The digest of a course is not a plain
// BLAKE2b-256 of the same bytes.
//
// Why no CRC: CRC-32 collisions are easy to forge. A forger can edit a course and append four bytes that restore the
// CRC, so a CRC proves nothing about who made a track. Ed25519 (RFC 8032, SHA-512) resists that kind of forgery.
//
// What verification proves: the holder of the public key inside the value signed this exact name and digest. It does
// NOT prove that the name belongs to a person. Tying a name to a key needs a later step, such as trust on first use
// or a server. Until that step exists, a verified mark only says that one key signed the track.
//
// Value record, GDX_SIGN_VALUE_BYTES (96) bytes. It holds only the key and the signature. The caller takes the name
// and the digest from the track and passes them to gdx_sign_verify.
//
//   0x00  publicKey   32 bytes, Ed25519 public key
//   0x20  signature   64 bytes, Ed25519 signature of the signed message

#ifndef GDX_COURSE_SIGN_H
#define GDX_COURSE_SIGN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_SIGN_SEED_BYTES 32
#define GDX_SIGN_PUBLIC_BYTES 32
#define GDX_SIGN_SECRET_BYTES 64
#define GDX_SIGN_DIGEST_BYTES 32
#define GDX_SIGN_VALUE_BYTES 96
#define GDX_SIGN_NAME_MAX 20

// Derives the key pair from a 32-byte seed. The seed is not changed. Returns 0, or -1 for a NULL argument.
int gdx_sign_key_from_seed(const unsigned char seed[GDX_SIGN_SEED_BYTES],
                           unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES],
                           unsigned char secretKey[GDX_SIGN_SECRET_BYTES]);

// Writes BLAKE2b-256 of "GDXC1" followed by the course bytes. length 0 is allowed. Returns 0, or -1 when digest is
// NULL or when course is NULL with length above 0.
int gdx_sign_course_digest(const unsigned char* course, size_t length, unsigned char digest[GDX_SIGN_DIGEST_BYTES]);

// Writes value: the public key (secret key bytes 32..63), then the signature of the signed message. Returns 0,
// -1 for a NULL argument, or -2 for a bad name. value is not written on error.
int gdx_sign_course(const unsigned char secretKey[GDX_SIGN_SECRET_BYTES], const char* author,
                    const unsigned char digest[GDX_SIGN_DIGEST_BYTES], unsigned char value[GDX_SIGN_VALUE_BYTES]);

// Returns 1 when the signature in value checks against the public key in value for this author and digest.
// Returns 0 otherwise, including a bad name or a NULL argument.
int gdx_sign_verify(const unsigned char value[GDX_SIGN_VALUE_BYTES], const char* author,
                    const unsigned char digest[GDX_SIGN_DIGEST_BYTES]);

// Writes the fingerprint of publicKey to out: the first 8 bytes of BLAKE2b-256(publicKey) as uppercase hex in four
// groups of four, for example "1A2B-3C4D-5E6F-7081", plus a NUL. out needs 20 bytes. A NULL publicKey writes ""
// when out is not NULL.
void gdx_sign_fingerprint(const unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES], char out[20]);

#ifdef __cplusplus
}
#endif

#endif // GDX_COURSE_SIGN_H
