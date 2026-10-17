/* port/gdx_xcup_seed.c -- X Cup seed code encode/decode, the queue of codes to use, and the
 * record of the codes used for the current cup.
 *
 * See gdx_xcup_seed.h for the contract. The four u32 are serialized big-endian into 16 bytes
 * (byte order is part of the CODE format, not of host memory, so codes are portable across
 * hosts), then base32-encoded 5 bits at a time with a Crockford-style alphabet that drops
 * I/L/O/U to avoid look-alikes and accidental words.
 */

#include "gdx_xcup_seed.h"

#include <string.h>

/* course.c:66-69: the seed state Course_GenerateRandomCourse captured for the last generated
 * course (plain file-scope globals, not static, so extern works). */
extern int32_t sRandomCourseInitSeed1;
extern int32_t sRandomCourseInitMask1;
extern int32_t sRandomCourseInitSeed2;
extern int32_t sRandomCourseInitMask2;

/* Set once any values have been captured; the BSS-zero state is indistinguishable from a real
 * all-zero seed, and all-zero masks make the LCG degenerate, so "generated yet" is tracked here
 * instead of by inspecting the values. The flag is set by gdx_xcup_note_seed_captured. */
static int sSeedCaptured = 0;

/* Codes waiting to be used, oldest first. Index 0 is the entry the next course takes. */
static int32_t sQueue[GDX_XCUP_QUEUE_CAPACITY][4];
static int sQueueCount = 0;

/* Seeds used for each course of the current cup, and which slots hold a real record. */
static int32_t sCup[GDX_XCUP_CUP_SIZE][4];
static int sCupValid[GDX_XCUP_CUP_SIZE];

static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static void gdx_xcup_serialize(const int32_t seed[4], unsigned char out[16]) {
    int i;
    int b;
    for (i = 0; i < 4; i++) {
        uint32_t v = (uint32_t) seed[i];
        for (b = 0; b < 4; b++) {
            out[i * 4 + b] = (unsigned char) (v >> (24 - b * 8));
        }
    }
}

static int gdx_xcup_deserialize(const unsigned char in[16], int32_t seed[4]) {
    int i;
    int b;
    for (i = 0; i < 4; i++) {
        uint32_t v = 0;
        for (b = 0; b < 4; b++) {
            v = (v << 8) | in[i * 4 + b];
        }
        seed[i] = (int32_t) v;
    }
    return 1;
}

static void gdx_xcup_encode(const unsigned char data[16], char* out, size_t outCap) {
    /* 128 bits -> 26 digits; dash after every 5 digits. */
    int bitPos = 0;
    int digit = 0;
    size_t o = 0;

    while (digit < 26 && o + 1 < outCap) {
        int value = 0;
        int k;
        for (k = 0; k < 5; k++) {
            int srcBit = bitPos + k;
            int bit = 0;
            if (srcBit < 128) {
                bit = (data[srcBit / 8] >> (7 - (srcBit % 8))) & 1;
            }
            value = (value << 1) | bit;
        }
        if (digit > 0 && digit % 5 == 0 && o + 1 < outCap) {
            out[o++] = '-';
        }
        if (o + 1 >= outCap) {
            break;
        }
        out[o++] = kAlphabet[value];
        bitPos += 5;
        digit++;
    }
    out[o] = '\0';
}

static int gdx_xcup_decode(const char* text, unsigned char data[16]) {
    int digit = 0;
    int bitPos = 0;
    size_t i;

    memset(data, 0, 16);
    for (i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        int value = -1;
        int k;

        if (c == '-' || c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        /* Crockford aliases: I/L -> 1, O -> 0. U stays invalid. */
        if (c == 'I' || c == 'L') {
            c = '1';
        } else if (c == 'O') {
            c = '0';
        }
        for (k = 0; k < 32; k++) {
            if (kAlphabet[k] == c) {
                value = k;
                break;
            }
        }
        if (value < 0 || digit >= 26) {
            return 0;
        }
        for (k = 0; k < 5; k++) {
            int dstBit = bitPos + k;
            if (dstBit < 128 && ((value >> (4 - k)) & 1)) {
                data[dstBit / 8] |= (unsigned char) (1 << (7 - (dstBit % 8)));
            }
        }
        bitPos += 5;
        digit++;
    }
    return digit == 26;
}

static void gdx_xcup_format_seed(const int32_t seed[4], char* out, size_t outCap) {
    unsigned char data[16];

    gdx_xcup_serialize(seed, data);
    gdx_xcup_encode(data, out, outCap);
}

/* Separators between codes in a pasted list. Dashes are not separators: they belong to a code. */
static int gdx_xcup_is_separator(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';';
}

int gdx_xcup_current_seed_code(char* out, size_t outCap) {
    int32_t seed[4];

    if (out == NULL || outCap == 0) {
        return 0;
    }
    if (!sSeedCaptured) {
        out[0] = '\0';
        return 0;
    }
    seed[0] = sRandomCourseInitSeed1;
    seed[1] = sRandomCourseInitMask1;
    seed[2] = sRandomCourseInitSeed2;
    seed[3] = sRandomCourseInitMask2;
    gdx_xcup_format_seed(seed, out, outCap);
    return 1;
}

int gdx_xcup_set_pending_code(const char* text) {
    unsigned char data[16];

    if (text == NULL || sQueueCount >= GDX_XCUP_QUEUE_CAPACITY || !gdx_xcup_decode(text, data)) {
        return 0;
    }
    gdx_xcup_deserialize(data, sQueue[sQueueCount]);
    sQueueCount++;
    return 1;
}

void gdx_xcup_queue_add_text(const char* text, int* added, int* rejected) {
    int nAdded = 0;
    int nRejected = 0;
    const char* p = text;

    while (text != NULL && *p != '\0') {
        char token[64];
        size_t len = 0;
        int tooLong = 0;

        while (*p != '\0' && gdx_xcup_is_separator(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        while (*p != '\0' && !gdx_xcup_is_separator(*p)) {
            if (len + 1 < sizeof(token)) {
                token[len++] = *p;
            } else {
                tooLong = 1;
            }
            p++;
        }
        token[len] = '\0';
        if (!tooLong && gdx_xcup_set_pending_code(token)) {
            nAdded++;
        } else {
            nRejected++;
        }
    }
    if (added != NULL) {
        *added = nAdded;
    }
    if (rejected != NULL) {
        *rejected = nRejected;
    }
}

int gdx_xcup_queue_count(void) {
    return sQueueCount;
}

int gdx_xcup_queue_code(int index, char* out, size_t outCap) {
    if (out == NULL || outCap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (index < 0 || index >= sQueueCount || outCap < GDX_XCUP_CODE_BUF_SIZE) {
        return 0;
    }
    gdx_xcup_format_seed(sQueue[index], out, outCap);
    return 1;
}

void gdx_xcup_queue_remove(int index) {
    int i;

    if (index < 0 || index >= sQueueCount) {
        return;
    }
    for (i = index; i + 1 < sQueueCount; i++) {
        memcpy(sQueue[i], sQueue[i + 1], sizeof(sQueue[i]));
    }
    sQueueCount--;
}

void gdx_xcup_queue_clear(void) {
    sQueueCount = 0;
}

int gdx_xcup_has_pending_seed(void) {
    return sQueueCount > 0;
}

int gdx_xcup_consume_pending_seed(int32_t* seed1, int32_t* mask1, int32_t* seed2, int32_t* mask2) {
    if (sQueueCount == 0) {
        return 0;
    }
    *seed1 = sQueue[0][0];
    *mask1 = sQueue[0][1];
    *seed2 = sQueue[0][2];
    *mask2 = sQueue[0][3];
    gdx_xcup_queue_remove(0);
    return 1;
}

void gdx_xcup_note_seed_captured(int slot, int newCup) {
    int32_t seed[4];
    int i;

    sSeedCaptured = 1;
    if (slot < 0 || slot >= GDX_XCUP_CUP_SIZE) {
        return;
    }
    if (newCup) {
        for (i = 0; i < GDX_XCUP_CUP_SIZE; i++) {
            sCupValid[i] = 0;
        }
    }
    seed[0] = sRandomCourseInitSeed1;
    seed[1] = sRandomCourseInitMask1;
    seed[2] = sRandomCourseInitSeed2;
    seed[3] = sRandomCourseInitMask2;
    memcpy(sCup[slot], seed, sizeof(seed));
    sCupValid[slot] = 1;
}

int gdx_xcup_cup_codes(char* out, size_t outCap) {
    int slot;
    int written = 0;
    size_t used = 0;

    if (out == NULL || outCap == 0) {
        return 0;
    }
    out[0] = '\0';
    for (slot = 0; slot < GDX_XCUP_CUP_SIZE; slot++) {
        char code[GDX_XCUP_CODE_BUF_SIZE];
        size_t codeLen;
        size_t sepLen;

        if (!sCupValid[slot]) {
            continue;
        }
        gdx_xcup_format_seed(sCup[slot], code, sizeof(code));
        codeLen = strlen(code);
        sepLen = written > 0 ? 1 : 0;
        if (used + sepLen + codeLen + 1 > outCap) {
            break;
        }
        if (sepLen) {
            out[used++] = '\n';
        }
        memcpy(out + used, code, codeLen + 1);
        used += codeLen;
        written++;
    }
    return written;
}
