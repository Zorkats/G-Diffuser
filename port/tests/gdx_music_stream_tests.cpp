// port/tests/gdx_music_stream_tests.cpp -- 6A loop tags, loop wrap and BGM keys, checked against the
// music test assets in devdocs/test-assets/feature-requests/music. The module is linked with a CVar
// stub and a stub for the decomp fade getter, so only the decoders and the module are exercised.

#include "gdx_music_stream.h"
#include "third_party/music_decoders/decoders.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Defined in libultraship in the game; the test takes the default.
extern "C" int CVarGetInteger(const char* name, int defaultValue) {
    (void)name;
    return defaultValue;
}

// Defined in decomp/src/audio/disk/external.c; the test runs at full stock fade.
extern "C" float Audio_PortBgmStreamFade(void) {
    return 1.0f;
}

namespace {

int sFailures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++sFailures;                                                             \
        }                                                                            \
    } while (0)

#define CHECK_EQ(actual, expected)                                                   \
    do {                                                                             \
        const unsigned long long a_ = static_cast<unsigned long long>(actual);       \
        const unsigned long long e_ = static_cast<unsigned long long>(expected);     \
        if (a_ != e_) {                                                              \
            std::printf("FAIL %s:%d: %s = %llu, expected %llu\n", __FILE__, __LINE__, \
                        #actual, a_, e_);                                            \
            ++sFailures;                                                             \
        }                                                                            \
    } while (0)

// Opens a test asset, reads a block, then seeks to the loop start and reads again. Fills info.
bool openAsset(const char* name, GdxMusicInfo* info) {
    const std::string path = std::string(GDX_TEST_MUSIC_DIR) + "/" + name;
    GdxMusicDecoder* dec = gdx_music_decoder_open(path.c_str(), info);
    if (dec == nullptr) {
        std::printf("FAIL could not open %s\n", path.c_str());
        ++sFailures;
        return false;
    }
    float block[4096 * 2];
    CHECK_EQ(gdx_music_decoder_read(dec, block, 4096), 4096);
    CHECK(gdx_music_decoder_seek(dec, info->loopStart) != 0);
    CHECK_EQ(gdx_music_decoder_read(dec, block, 4096), 4096);
    gdx_music_decoder_close(dec);
    return true;
}

void testOggLoopTags() {
    GdxMusicInfo info;
    if (!openAsset("loop_test_44k.ogg", &info)) {
        return;
    }
    CHECK_EQ(info.sampleRate, 44100);
    CHECK_EQ(info.channels, 2);
    CHECK_EQ(info.hasLoopTags, 1);
    CHECK_EQ(info.loopStart, 132300);
    CHECK_EQ(info.loopEnd - info.loopStart, 352800);
    CHECK(info.loopEnd <= info.totalFrames);
}

void testFlacLoopTags() {
    GdxMusicInfo info;
    if (!openAsset("loop_test_44k.flac", &info)) {
        return;
    }
    CHECK_EQ(info.sampleRate, 44100);
    CHECK_EQ(info.channels, 2);
    CHECK_EQ(info.hasLoopTags, 1);
    CHECK_EQ(info.loopStart, 132300);
    CHECK_EQ(info.loopEnd - info.loopStart, 352800);
    CHECK(info.loopEnd <= info.totalFrames);
}

void testWavSmplLoop() {
    GdxMusicInfo info;
    if (!openAsset("loop_test_32k.wav", &info)) {
        return;
    }
    CHECK_EQ(info.sampleRate, 32000);
    CHECK_EQ(info.hasLoopTags, 1);
    // The smpl chunk is 96000 to 351999 inclusive, so the exclusive end is 352000.
    CHECK_EQ(info.loopStart, 96000);
    CHECK_EQ(info.loopEnd, 352000);
    CHECK(info.loopEnd <= info.totalFrames);
}

void testNoLoopTagsLoopWholeFile() {
    GdxMusicInfo info;
    if (!openAsset("no_loop_tags.ogg", &info)) {
        return;
    }
    CHECK_EQ(info.hasLoopTags, 0);
    CHECK_EQ(info.loopStart, 0);
    CHECK(info.totalFrames > 0);
    CHECK_EQ(info.loopEnd, info.totalFrames);
}

void testLoopWrapPastLoopEnd() {
    // Ogg and FLAC loop: start 132300, end 485100.
    CHECK_EQ(gdx_music_loop_wrap(485099, 132300, 485100), 485099);
    CHECK_EQ(gdx_music_loop_wrap(485100, 132300, 485100), 132300);
    CHECK_EQ(gdx_music_loop_wrap(485105, 132300, 485100), 132305);
    CHECK_EQ(gdx_music_loop_wrap(485100 + 352800, 132300, 485100), 132300);
    CHECK_EQ(gdx_music_loop_wrap(485100 + 352800 + 7, 132300, 485100), 132307);

    // WAV loop: start 96000, end 352000 (exclusive).
    CHECK_EQ(gdx_music_loop_wrap(352000, 96000, 352000), 96000);
    CHECK_EQ(gdx_music_loop_wrap(352003, 96000, 352000), 96003);

    // Whole-file loop: start 0, end N.
    CHECK_EQ(gdx_music_loop_wrap(1000, 0, 1000), 0);
    CHECK_EQ(gdx_music_loop_wrap(1001, 0, 1000), 1);
}

void expectKey(int bgmId, const char* expected) {
    char key[32] = "";
    const int found = gdx_music_stream_key_for_bgm(bgmId, key, sizeof key);
    if (expected == nullptr) {
        CHECK_EQ(found, 0);
        return;
    }
    CHECK_EQ(found, 1);
    if (std::strcmp(key, expected) != 0) {
        std::printf("FAIL bgm %d key \"%s\", expected \"%s\"\n", bgmId, key, expected);
        ++sFailures;
    }
}

void testBgmKeys() {
    expectKey(0, "bgm_mute_city");
    expectKey(14, "bgm_title");
    expectKey(18, "bgm_course_editor");
    expectKey(21, "bgm_21");
    expectKey(29, "bgm_deathrace2");
    expectKey(30, "bgm_1e");
    expectKey(99, nullptr);
}

}  // namespace

int main() {
    testOggLoopTags();
    testFlacLoopTags();
    testWavSmplLoop();
    testNoLoopTagsLoopWholeFile();
    testLoopWrapPastLoopEnd();
    testBgmKeys();

    if (sFailures == 0) {
        std::printf("gdx_music_stream_tests: all checks passed\n");
        return 0;
    }
    std::printf("gdx_music_stream_tests: %d check(s) failed\n", sFailures);
    return 1;
}
