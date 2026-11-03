#define GDX_MUSIC_STREAM_TEST 1
#include "../gdx_music_stream.cpp"

#include <cassert>

static float sFade = 0.0f;
static float sPreviewGain = 1.0f;
static int sMuted = 0;
static float sVolume = 1.0f;
static int sDuringOpen = 0;
static int sDuringRead = 0;
static int sCloses = 0;

struct GdxMusicDecoder {};
static GdxMusicDecoder sDecoder;

extern "C" GdxMusicDecoder* gdx_music_decoder_open(const char*, GdxMusicInfo* info) {
    *info = GdxMusicInfo{32000, 1, 32000, 0, 32000, 0};
    const int action = sDuringOpen;
    sDuringOpen = 0;
    if (action == 1) {
        gdx_music_stream_bgm_stop();
    } else if (action == 2) {
        gdx_music_stream_play_file("new.wav", "new");
    }
    return action == 3 ? nullptr : &sDecoder;
}

extern "C" uint64_t gdx_music_decoder_read(GdxMusicDecoder*, float* out, uint64_t frames) {
    if (sDuringRead) {
        sDuringRead = 0;
        gdx_music_stream_bgm_stop();
    }
    std::fill(out, out + frames, 0.25f);
    return frames;
}

extern "C" int gdx_music_decoder_seek(GdxMusicDecoder*, uint64_t) {
    return 1;
}

extern "C" void gdx_music_decoder_close(GdxMusicDecoder*) {
    ++sCloses;
}

extern "C" int CVarGetInteger(const char*, int fallback) {
    return fallback;
}

extern "C" float Audio_PortBgmStreamFade(void) {
    return sFade;
}

extern "C" float Audio_PortBgmPreviewGain(void) {
    return sPreviewGain;
}

extern "C" void gdx_audio_seq_player_stream_mute(int muted) {
    sMuted = muted;
}

extern "C" float gdx_audio_music_slider_scale(void) {
    return sVolume;
}

static int sample() {
    int16_t out[64]{};
    assert(gdx_music_stream_mix_s16(out, 32, 32000) == 1);
    return out[0];
}

static void load(bool preview) {
    assert(queueStream("test.wav", "test", preview));
    workerTick(false);
    workerTick(false);
    assert(gdx_music_stream_is_mixing());
    assert(sMuted);
}

int main() {
    Shared& s = shared();
    assert(gdx_music_stream_play_file("test.wav", "editor"));
    workerTick(false);
    workerTick(false);
    assert(sample() == 8192);
    sPreviewGain = 0.4f;
    assert(sample() == 3277);
    sVolume = 0.0f;
    assert(sample() == 0);
    sVolume = 0.5f;
    sPreviewGain = 1.0f;
    assert(sample() == 4096);
    sVolume = 1.0f;
    gdx_music_stream_bgm_stop();
    assert(!gdx_music_stream_is_mixing() && !sMuted);

    load(false);
    assert(sample() == 0);
    sFade = 0.5f;
    assert(sample() == 4096);
    gdx_music_stream_bgm_stop();
    assert(s.state.load() == kDraining);
    assert(sample() == 4096);
    sFade = 0;
    int16_t out[2]{};
    assert(!gdx_music_stream_mix_s16(out, 1, 32000));
    workerTick(false);
    assert(s.state.load() == kIdle && !sMuted);

    sDuringOpen = 1;
    queueStream("canceled.wav", "cancel", true);
    workerTick(false);
    assert(session().dec == nullptr && s.state.load() == kIdle && !sMuted);
    sDuringOpen = 2;
    queueStream("old.wav", "old", false);
    workerTick(false);
    assert(session().dec == nullptr && s.state.load() == kLoading && sMuted);
    workerTick(false);
    workerTick(false);
    assert(sample() == 8192);

    s.mixing.store(true);
    queueStream("held.wav", "held", true);
    const uint32_t epoch = s.epoch.load();
    workerTick(false);
    assert(s.hasPending && s.pendingPath == "held.wav" && s.epoch.load() == epoch);
    s.mixing.store(false);
    workerTick(false);
    workerTick(false);
    assert(sample() == 8192);

    queueStream("read.wav", "read", true);
    workerTick(false);
    const uint64_t before = s.written.load();
    sDuringRead = 1;
    workerTick(false);
    assert(s.written.load() == before && s.state.load() == kIdle && !sMuted);
    sDuringOpen = 3;
    queueStream("bad.wav", "bad", true);
    workerTick(false);
    assert(s.state.load() == kIdle && !sMuted);
    load(true);
    gdx_music_stream_bgm_start(99);
    assert(s.state.load() == kIdle && !sMuted);
    workerTick(false);
    assert(session().dec == nullptr && sCloses >= 6);
    std::puts("gdx_music_lifecycle_tests: passed");
}
