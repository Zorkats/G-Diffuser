// Decoder wrapper for port/gdx_music_stream.cpp. This is the only translation unit that holds the
// vendored implementations: stb_vorbis (Ogg Vorbis), dr_flac and dr_wav. Their provenance is in
// port/third_party/stb_vorbis/README.md and port/third_party/dr_libs/README.md. Warnings are
// suppressed for this file alone (port/third_party/music_decoders/CMakeLists.txt).

#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#include "../dr_libs/dr_flac.h"
#include "../dr_libs/dr_wav.h"
#include "../stb_vorbis/stb_vorbis.c"
#include "decoders.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum GdxDecoderKind {
    GDX_DECODER_OGG,
    GDX_DECODER_FLAC,
    GDX_DECODER_WAV
} GdxDecoderKind;

struct GdxMusicDecoder {
    GdxDecoderKind kind;
    uint32_t channels;
    stb_vorbis* ogg;
    drflac* flac;
    drwav wav;
};

// Loop tags found while a file is opened. Vorbis and FLAC carry LOOPSTART and LOOPLENGTH as
// comments in frames. WAV carries RIFF smpl loop offsets, first and last inclusive.
typedef struct GdxTagScan {
    int haveStart;
    int haveLength;
    uint64_t start;
    uint64_t length;
    int haveGain;
    int havePeak;
    double gain;
    double peak;
} GdxTagScan;

static int gdx_parse_u64(const char* s, size_t n, uint64_t* out) {
    uint64_t v = 0;
    size_t i;

    if (n == 0 || n > 20) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint64_t digit;
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        digit = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - digit) / 10u) {
            return 0;
        }
        v = v * 10u + digit;
    }
    *out = v;
    return 1;
}

// A decimal number as ReplayGain writes it: optional spaces and sign, digits with an optional fraction,
// then optional spaces and an optional "dB" unit. Read by hand so the C locale's decimal point does not
// matter.
static int gdx_parse_decimal(const char* s, size_t n, double* out) {
    size_t i = 0;
    double value = 0.0;
    double scale = 0.1;
    int negative = 0;
    int digits = 0;

    while (i < n && s[i] == ' ') {
        i++;
    }
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        negative = s[i] == '-';
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        value = value * 10.0 + (double)(s[i] - '0');
        digits++;
        i++;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            value += scale * (double)(s[i] - '0');
            scale *= 0.1;
            digits++;
            i++;
        }
    }
    if (digits == 0 || digits > 24) {
        return 0;
    }
    while (i < n && s[i] == ' ') {
        i++;
    }
    if (i + 2 <= n && toupper((unsigned char)s[i]) == 'D' && toupper((unsigned char)s[i + 1]) == 'B') {
        i += 2;
    }
    while (i < n && s[i] == ' ') {
        i++;
    }
    if (i != n) {
        return 0;
    }
    *out = negative ? -value : value;
    return 1;
}

// True when the comment is KEY=value with KEY matched case-insensitively. key is upper case.
static int gdx_tag_is(const char* s, size_t n, const char* key) {
    size_t k = strlen(key);
    size_t i;

    if (n <= k) {
        return 0;
    }
    for (i = 0; i < k; i++) {
        if (toupper((unsigned char)s[i]) != (unsigned char)key[i]) {
            return 0;
        }
    }
    return s[k] == '=';
}

static void gdx_tag_scan_one(GdxTagScan* scan, const char* s, size_t n) {
    static const char kStart[] = "LOOPSTART";
    static const char kLength[] = "LOOPLENGTH";
    static const char kGain[] = "REPLAYGAIN_TRACK_GAIN";
    static const char kPeak[] = "REPLAYGAIN_TRACK_PEAK";
    double value = 0.0;

    if (gdx_tag_is(s, n, kStart)) {
        size_t k = strlen(kStart);
        if (gdx_parse_u64(s + k + 1, n - k - 1, &scan->start)) {
            scan->haveStart = 1;
        }
    } else if (gdx_tag_is(s, n, kLength)) {
        size_t k = strlen(kLength);
        if (gdx_parse_u64(s + k + 1, n - k - 1, &scan->length)) {
            scan->haveLength = 1;
        }
    } else if (gdx_tag_is(s, n, kGain)) {
        size_t k = strlen(kGain);
        if (gdx_parse_decimal(s + k + 1, n - k - 1, &value) && value >= -60.0 && value <= 60.0) {
            scan->gain = value;
            scan->haveGain = 1;
        }
    } else if (gdx_tag_is(s, n, kPeak)) {
        size_t k = strlen(kPeak);
        if (gdx_parse_decimal(s + k + 1, n - k - 1, &value) && value > 0.0 && value <= 100.0) {
            scan->peak = value;
            scan->havePeak = 1;
        }
    }
}

static void gdx_level_from_tags(const GdxTagScan* scan, GdxMusicInfo* info) {
    info->hasGain = scan->haveGain;
    info->gainDb = scan->haveGain ? (float)scan->gain : 0.0f;
    info->hasPeak = scan->havePeak;
    info->peak = scan->havePeak ? (float)scan->peak : 0.0f;
}

// Applies the Vorbis-style tags when both are present and fit inside the file. Otherwise the loop
// is the whole file, which is the documented fallback.
static void gdx_loop_from_tags(const GdxTagScan* scan, GdxMusicInfo* info) {
    info->loopStart = 0;
    info->loopEnd = info->totalFrames;
    info->hasLoopTags = 0;
    if (scan->haveStart && scan->haveLength && scan->length > 0 && scan->start < info->totalFrames &&
        scan->length <= info->totalFrames - scan->start) {
        info->loopStart = scan->start;
        info->loopEnd = scan->start + scan->length;
        info->hasLoopTags = 1;
    }
}

static void gdx_flac_on_meta(void* user, drflac_metadata* md) {
    GdxTagScan* scan = (GdxTagScan*)user;
    drflac_vorbis_comment_iterator it;
    const char* text;
    drflac_uint32 len = 0;

    if (md->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        return;
    }
    drflac_init_vorbis_comment_iterator(&it, md->data.vorbis_comment.commentCount, md->data.vorbis_comment.pComments);
    while ((text = drflac_next_vorbis_comment(&it, &len)) != NULL) {
        gdx_tag_scan_one(scan, text, (size_t)len);
    }
}

static void gdx_decoder_free(GdxMusicDecoder* dec) {
    free(dec);
}

static GdxMusicDecoder* gdx_open_ogg(const char* path, GdxMusicInfo* info) {
    GdxMusicDecoder* dec;
    stb_vorbis_info vi;
    stb_vorbis_comment comments;
    GdxTagScan scan;
    int err = 0;
    int i;

    dec = (GdxMusicDecoder*)calloc(1, sizeof(*dec));
    if (dec == NULL) {
        return NULL;
    }
    dec->ogg = stb_vorbis_open_filename(path, &err, NULL);
    if (dec->ogg == NULL) {
        gdx_decoder_free(dec);
        return NULL;
    }
    vi = stb_vorbis_get_info(dec->ogg);
    if (vi.channels < 1 || vi.channels > 2) {
        stb_vorbis_close(dec->ogg);
        gdx_decoder_free(dec);
        return NULL;
    }
    dec->kind = GDX_DECODER_OGG;
    dec->channels = (uint32_t)vi.channels;
    info->sampleRate = (uint32_t)vi.sample_rate;
    info->channels = dec->channels;
    info->totalFrames = (uint64_t)stb_vorbis_stream_length_in_samples(dec->ogg);

    memset(&scan, 0, sizeof(scan));
    comments = stb_vorbis_get_comment(dec->ogg);
    for (i = 0; i < comments.comment_list_length; i++) {
        gdx_tag_scan_one(&scan, comments.comment_list[i], strlen(comments.comment_list[i]));
    }
    gdx_loop_from_tags(&scan, info);
    gdx_level_from_tags(&scan, info);
    return dec;
}

static GdxMusicDecoder* gdx_open_flac(const char* path, GdxMusicInfo* info) {
    GdxMusicDecoder* dec;
    GdxTagScan scan;

    memset(&scan, 0, sizeof(scan));
    dec = (GdxMusicDecoder*)calloc(1, sizeof(*dec));
    if (dec == NULL) {
        return NULL;
    }
    dec->flac = drflac_open_file_with_metadata(path, gdx_flac_on_meta, &scan, NULL);
    if (dec->flac == NULL) {
        gdx_decoder_free(dec);
        return NULL;
    }
    if (dec->flac->channels < 1 || dec->flac->channels > 2) {
        drflac_close(dec->flac);
        gdx_decoder_free(dec);
        return NULL;
    }
    dec->kind = GDX_DECODER_FLAC;
    dec->channels = dec->flac->channels;
    info->sampleRate = dec->flac->sampleRate;
    info->channels = dec->channels;
    info->totalFrames = dec->flac->totalPCMFrameCount;
    gdx_loop_from_tags(&scan, info);
    gdx_level_from_tags(&scan, info);
    return dec;
}

static GdxMusicDecoder* gdx_open_wav(const char* path, GdxMusicInfo* info) {
    GdxMusicDecoder* dec;
    uint32_t m;
    int found = 0;
    uint64_t first = 0;
    uint64_t last = 0;

    dec = (GdxMusicDecoder*)calloc(1, sizeof(*dec));
    if (dec == NULL) {
        return NULL;
    }
    if (!drwav_init_file_with_metadata(&dec->wav, path, drwav_metadata_type_smpl | drwav_metadata_type_bext, NULL)) {
        gdx_decoder_free(dec);
        return NULL;
    }
    if (dec->wav.channels < 1 || dec->wav.channels > 2) {
        drwav_uninit(&dec->wav);
        gdx_decoder_free(dec);
        return NULL;
    }
    for (m = 0; m < dec->wav.metadataCount; m++) {
        const drwav_metadata* md = &dec->wav.pMetadata[m];
        if (!found && md->type == drwav_metadata_type_smpl && md->data.smpl.sampleLoopCount > 0 &&
            md->data.smpl.pLoops != NULL) {
            first = md->data.smpl.pLoops[0].firstSampleOffset;
            last = md->data.smpl.pLoops[0].lastSampleOffset;
            found = 1;
        }
        // bext version 2 (EBU Tech 3285): loudness in LUFS and true peak in dBTP, both times 100 as signed
        // 16-bit values, 0x7FFF when not measured.
        if (md->type == drwav_metadata_type_bext && md->data.bext.version >= 2) {
            const int16_t loudness = (int16_t)md->data.bext.loudnessValue;
            const int16_t peak = (int16_t)md->data.bext.maxTruePeakLevel;
            if (loudness != 0x7FFF) {
                info->hasGain = 1;
                info->gainDb = GDX_MUSIC_REPLAYGAIN_REFERENCE_LUFS - (float)loudness / 100.0f;
            }
            if (peak != 0x7FFF) {
                info->hasPeak = 1;
                info->peak = powf(10.0f, (float)peak / 2000.0f);
            }
        }
    }

    dec->kind = GDX_DECODER_WAV;
    dec->channels = dec->wav.channels;
    info->sampleRate = dec->wav.sampleRate;
    info->channels = dec->channels;
    info->totalFrames = dec->wav.totalPCMFrameCount;
    info->loopStart = 0;
    info->loopEnd = info->totalFrames;
    info->hasLoopTags = 0;
    if (found && last >= first && last < info->totalFrames) {
        // The RIFF end offset is inclusive, so the loop covers one more frame than the difference.
        info->loopStart = first;
        info->loopEnd = last + 1;
        info->hasLoopTags = 1;
    }
    return dec;
}

static int gdx_path_has_ext(const char* path, const char* ext) {
    size_t pl = strlen(path);
    size_t el = strlen(ext);
    size_t i;

    if (pl < el) {
        return 0;
    }
    for (i = 0; i < el; i++) {
        if (tolower((unsigned char)path[pl - el + i]) != (unsigned char)ext[i]) {
            return 0;
        }
    }
    return 1;
}

GdxMusicDecoder* gdx_music_decoder_open(const char* path, GdxMusicInfo* info) {
    GdxMusicDecoder* dec = NULL;

    if (path == NULL || info == NULL) {
        return NULL;
    }
    memset(info, 0, sizeof(*info));
    if (gdx_path_has_ext(path, ".ogg")) {
        dec = gdx_open_ogg(path, info);
    } else if (gdx_path_has_ext(path, ".flac")) {
        dec = gdx_open_flac(path, info);
    } else if (gdx_path_has_ext(path, ".wav")) {
        dec = gdx_open_wav(path, info);
    }
    if (dec != NULL && (info->totalFrames == 0 || info->sampleRate == 0)) {
        gdx_music_decoder_close(dec);
        return NULL;
    }
    return dec;
}

uint64_t gdx_music_decoder_read(GdxMusicDecoder* dec, float* out, uint64_t frames) {
    int got;

    if (dec == NULL || out == NULL || frames == 0) {
        return 0;
    }
    switch (dec->kind) {
        case GDX_DECODER_OGG:
            if (frames > 1u << 20) {
                frames = 1u << 20;
            }
            got = stb_vorbis_get_samples_float_interleaved(dec->ogg, (int)dec->channels, out,
                                                           (int)(frames * dec->channels));
            return got > 0 ? (uint64_t)got : 0;
        case GDX_DECODER_FLAC:
            return drflac_read_pcm_frames_f32(dec->flac, frames, out);
        case GDX_DECODER_WAV:
            return drwav_read_pcm_frames_f32(&dec->wav, frames, out);
    }
    return 0;
}

float gdx_music_level_gain(const GdxMusicInfo* info) {
    float db;
    float gain;

    if (info == NULL || !info->hasGain) {
        return 1.0f;
    }
    db = info->gainDb + (GDX_MUSIC_STOCK_LUFS - GDX_MUSIC_REPLAYGAIN_REFERENCE_LUFS);
    if (db < -60.0f) {
        db = -60.0f;
    }
    if (db > 24.0f) {
        db = 24.0f;
    }
    gain = powf(10.0f, db / 20.0f);
    if (info->hasPeak && info->peak > 0.0f) {
        if (gain * info->peak > 1.0f) {
            gain = 1.0f / info->peak;
        }
    } else if (gain > 1.0f) {
        gain = 1.0f;
    }
    return gain;
}

int gdx_music_decoder_seek(GdxMusicDecoder* dec, uint64_t frame) {
    if (dec == NULL) {
        return 0;
    }
    switch (dec->kind) {
        case GDX_DECODER_OGG:
            return stb_vorbis_seek(dec->ogg, (unsigned int)frame) != 0;
        case GDX_DECODER_FLAC:
            return drflac_seek_to_pcm_frame(dec->flac, frame) != 0;
        case GDX_DECODER_WAV:
            return drwav_seek_to_pcm_frame(&dec->wav, frame) != 0;
    }
    return 0;
}

void gdx_music_decoder_close(GdxMusicDecoder* dec) {
    if (dec == NULL) {
        return;
    }
    switch (dec->kind) {
        case GDX_DECODER_OGG:
            stb_vorbis_close(dec->ogg);
            break;
        case GDX_DECODER_FLAC:
            drflac_close(dec->flac);
            break;
        case GDX_DECODER_WAV:
            drwav_uninit(&dec->wav);
            break;
    }
    gdx_decoder_free(dec);
}
