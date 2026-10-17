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
    }
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
    if (!drwav_init_file_with_metadata(&dec->wav, path, drwav_metadata_type_smpl, NULL)) {
        gdx_decoder_free(dec);
        return NULL;
    }
    if (dec->wav.channels < 1 || dec->wav.channels > 2) {
        drwav_uninit(&dec->wav);
        gdx_decoder_free(dec);
        return NULL;
    }
    for (m = 0; m < dec->wav.metadataCount && !found; m++) {
        const drwav_metadata* md = &dec->wav.pMetadata[m];
        if (md->type == drwav_metadata_type_smpl && md->data.smpl.sampleLoopCount > 0 && md->data.smpl.pLoops != NULL) {
            first = md->data.smpl.pLoops[0].firstSampleOffset;
            last = md->data.smpl.pLoops[0].lastSampleOffset;
            found = 1;
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
