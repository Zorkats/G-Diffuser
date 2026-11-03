#include "port_log.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
static fs::path sRoot;
static const char* test_path(char* out, size_t cap, const char* relative) {
    const auto path = (sRoot / relative).string();
    assert(path.size() < cap);
    std::snprintf(out, cap, "%s", path.c_str());
    return out;
}

#define gdx_exe_relative_path test_path
#define GDX_MUSIC_STREAM_TEST 1
#include "../gdx_music_stream.cpp"
#include "../gdx_course_music.cpp"
#undef gdx_exe_relative_path

extern "C" int CVarGetInteger(const char*, int fallback) {
    return fallback;
}
extern "C" float Audio_PortBgmStreamFade(void) {
    return 1.0f;
}
extern "C" float Audio_PortBgmPreviewGain(void) {
    return 1.0f;
}
extern "C" int gdx_course_boot(void) {
    gdx_course_music_scan_files();
    return 0;
}
extern "C" int gdx_course_state_flush(void) {
    return 0;
}

static void write(const fs::path& path, const char* bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    assert(file.good());
}

static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static const GdxCourseElement* song(const char* key) {
    return gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, key);
}

static void rescan() {
    std::vector<unsigned char> state(GDX_COURSE_STATE_MAX_BYTES);
    const int length = gdx_course_store_encode_file(state.data(), state.size());
    assert(length > 0);
    gdx_course_registry_clear_extensions();
    sScanned = false;
    gdx_course_music_scan();
    assert(gdx_course_store_restore_file(state.data(), length) >= 0);
}

static void expectSource(const char* key, const fs::path& expected) {
    const auto* entry = song(key);
    assert(entry && entry->sourcePath);
    assert(fs::path(entry->sourcePath) == expected);
}

static int songIndex(const char* key) {
    for (int i = 0; i < gdx_course_music_count(); ++i) {
        if (music_entry(i) == song(key)) {
            return i;
        }
    }
    return -1;
}

static void testDetection() {
    write(sRoot / "music/Racing Song.flac", "canonical course");
    write(sRoot / "music/bgm_select.wav", "canonical menu");
    write(sRoot / "mods/music/bgm_select.ogg", "legacy menu");
    write(sRoot / "mods/music/custom/Racing Song.ogg", "legacy course override");
    write(sRoot / "music/custom/Racing Song.ogg", "legacy course");
    write(sRoot / "music/Format.wav", "wav");
    write(sRoot / "music/Format.FLAC", "flac");
    write(sRoot / "music/Format.OGG", "ogg");
    write(sRoot / "music/bgm_my_song.ogg", "not a reserved slot");
    write(sRoot / "music/bgm_1e.ogg", "numeric global slot");
    write(sRoot / "music/bgm_63.ogg", "99 is not a playable slot");
    write(sRoot / "music/ignored.mp3", "unsupported");
    write(sRoot / "music/nested/hidden.ogg", "not recursive");
    fs::create_directories(sRoot / "music/directory.ogg");
    rescan();
    expectSource("bgm.custom.racing_song", sRoot / "music/Racing Song.flac");
    expectSource("bgm.custom.format", sRoot / "music/Format.OGG");
    assert(song("bgm.custom.bgm_my_song"));
    assert(song("bgm.custom.bgm_63"));
    assert(!song("bgm.custom.bgm_select"));
    assert(!song("bgm.custom.bgm_1e"));
    assert(!song("bgm.custom.ignored"));
    assert(!song("bgm.custom.hidden"));
    assert(!song("bgm.custom.directory"));
    const int count = gdx_course_music_count();
    assert(gdx_course_music_scan() == count);
    char path[1024];
    assert(gdx_music_stream_find_file(15, path, sizeof path));
    assert(fs::path(path) == sRoot / "music/bgm_select.wav");
    assert(gdx_music_stream_find_file(30, path, sizeof path));
    assert(fs::path(path) == sRoot / "music/bgm_1e.ogg");
    char small[4] = "abc";
    assert(!gdx_music_stream_find_file(15, small, sizeof small));
    assert(std::strcmp(small, "abc") == 0);
    assert(!gdx_music_stream_find_file(15, nullptr, 0));
    assert(!gdx_music_stream_find_file(99, path, sizeof path));

    write(sRoot / "music/BGM_SELECT.FLAC", "flac menu");
    assert(gdx_music_stream_find_file(15, path, sizeof path));
    assert(fs::path(path) == sRoot / "music/BGM_SELECT.FLAC");
    write(sRoot / "music/bgm_select.ogg", "ogg menu");
    assert(gdx_music_stream_find_file(15, path, sizeof path));
    assert(fs::path(path) == sRoot / "music/bgm_select.ogg");
    fs::remove(sRoot / "music/bgm_select.ogg");
    fs::remove(sRoot / "music/BGM_SELECT.FLAC");
    fs::remove(sRoot / "music/bgm_select.wav");
    assert(gdx_music_stream_find_file(15, path, sizeof path));
    assert(fs::path(path) == sRoot / "mods/music/bgm_select.ogg");

    const int index = songIndex("bgm.custom.racing_song");
    assert(index >= 0);
    assert(gdx_course_music_set_choice("TEST.CRSD", index) == 0);
    assert(gdx_course_music_preview(index));
    assert(fs::path(shared().pendingPath) == sRoot / "music/Racing Song.flac");
    gdx_music_stream_bgm_stop();
    gdx_course_music_arm_for_course("TEST.CRSD", 0);
    assert(fs::path(courseOverride().path) == sRoot / "music/Racing Song.flac");
    fs::remove(sRoot / "music/Racing Song.flac");
    rescan();
    expectSource("bgm.custom.racing_song", sRoot / "mods/music/custom/Racing Song.ogg");
    assert(gdx_course_music_choice("TEST.CRSD") >= 0);
    fs::remove(sRoot / "mods/music/custom/Racing Song.ogg");
    rescan();
    expectSource("bgm.custom.racing_song", sRoot / "music/custom/Racing Song.ogg");
    assert(gdx_course_music_choice("TEST.CRSD") >= 0);

    fs::create_directories(sRoot / "export");
    assert(gdx_course_music_export_course("TEST.CRSD", (sRoot / "export").string().c_str()) == 1);
    assert(read(sRoot / "export/racing_song.ogg") == "legacy course");
}

static void testImport() {
    const auto package = sRoot / "incoming/track.gdxc";
    char key[GDX_COURSE_KEY_MAX + 1];
    write(package.parent_path() / "arrival.WAV", "imported");
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.arrival", key, sizeof key) == 1);
    assert(std::strcmp(key, "bgm.custom.arrival") == 0);
    expectSource(key, sRoot / "music/arrival.wav");
    assert(read(sRoot / "music/arrival.wav") == "imported");
    assert(!fs::exists(sRoot / "music/custom/arrival.wav"));
    assert(!fs::exists(sRoot / "mods/music/custom/arrival.wav"));
    assert(gdx_course_music_adopt(package.string().c_str(), key, key, sizeof key) == 1);
    write(package.parent_path() / "arrival.WAV", "different import");
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.arrival", key, sizeof key) == 1);
    assert(std::strcmp(key, "bgm.custom.arrival") != 0);
    assert(read(sRoot / "music/arrival.wav") == "imported");
    assert(read(song(key)->sourcePath) == "different import");
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.arrival", key, sizeof key) == 1);
    fs::create_directories(sRoot / "roundtrip");
    assert(gdx_course_music_export_key(key, (sRoot / "roundtrip").string().c_str()) == 1);
    const std::string fingerprintKey = key;
    const auto exported = sRoot / "roundtrip" / (key_suffix(key) + ".wav");
    assert(read(exported) == "different import");
    fs::remove(song(key)->sourcePath);
    rescan();
    assert(gdx_course_music_adopt((sRoot / "roundtrip/track.gdxc").string().c_str(),
                                 fingerprintKey.c_str(), key, sizeof key) == 1);
    assert(fingerprintKey == key);
    assert(read(song(key)->sourcePath) == "different import");

    write(package.parent_path() / "bgm_select.flac", "legacy named course");
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.bgm_select", key, sizeof key) == 1);
    assert(std::strcmp(key, "bgm.custom.bgm_select") != 0);
    assert(!gdx::music::IsReplacementStem(key_suffix(key)));
    assert(!fs::exists(sRoot / "music/bgm_select.flac"));
    assert(read(song(key)->sourcePath) == "legacy named course");
    assert(read(sRoot / "mods/music/bgm_select.ogg") == "legacy menu");

    write(sRoot / "music/custom/bgm_title.ogg", "existing legacy course");
    write(package.parent_path() / "bgm_title.ogg", "existing legacy course");
    rescan();
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.bgm_title", key, sizeof key) == 1);
    assert(std::strcmp(key, "bgm.custom.bgm_title") == 0);
    assert(!fs::exists(sRoot / "music/bgm_title.ogg"));
    expectSource(key, sRoot / "music/custom/bgm_title.ogg");
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.missing", key, sizeof key) == 0);
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.arrival", key, sizeof key) == 1);
    assert(!gdx_course_music_adopt(package.string().c_str(), "bgm.custom../escape", key, sizeof key));
    assert(!gdx_course_music_adopt(package.string().c_str(), "bgm.custom.", key, sizeof key));

    write(package.parent_path() / "large.wav", "");
    fs::resize_file(package.parent_path() / "large.wav", 33u * 1024u * 1024u);
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.large", key, sizeof key) == -1);
    assert(!fs::exists(sRoot / "music/large.wav"));
    write(package.parent_path() / "blocked.wav", "cannot import");
    fs::create_directories(sRoot / "music/blocked.wav");
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.blocked", key, sizeof key) == -1);
    assert(fs::is_directory(sRoot / "music/blocked.wav"));

    const std::string collisionBytes = "collision incoming";
    write(package.parent_path() / "collision.wav", collisionBytes.c_str());
    write(sRoot / "music/collision.wav", "original occupant");
    char collisionKey[GDX_COURSE_KEY_MAX + 1];
    assert(gdx_course_registry_music_fingerprint_key("bgm.custom.collision",
        gdx_course_registry_crc32(reinterpret_cast<const unsigned char*>(collisionBytes.data()), collisionBytes.size()), collisionKey, sizeof collisionKey));
    write(sRoot / "music" / (key_suffix(collisionKey) + ".ogg"), "fingerprint already occupied");
    rescan();
    assert(gdx_course_music_adopt(package.string().c_str(), "bgm.custom.collision", key, sizeof key) == -1);
    assert(read(sRoot / "music/collision.wav") == "original occupant");
    assert(read(song(collisionKey)->sourcePath) == "fingerprint already occupied");
    assert(read(package.parent_path() / "collision.wav") == collisionBytes);
}

static void testReservedNames() {
    for (int id = 0; id < 256; ++id) {
        char key[32];
        if (gdx_music_stream_key_for_bgm(id, key, sizeof key)) {
            assert(gdx::music::IsReplacementStem(key));
        }
    }
    assert(gdx::music::IsReplacementStem("BGM_SELECT"));
    assert(gdx::music::IsReplacementStem("bgm_7fffffff"));
    for (const char* name : {"bgm_custom", "bgm_", "bgm_00", "bgm_01e", "bgm_63", "bgm_80000000", "bgm_select-looped"}) {
        assert(!gdx::music::IsReplacementStem(name));
    }
}

static void testCleanImport() {
    const auto originalRoot = sRoot;
    sRoot /= "fresh";
    rescan();
    char path[1024];
    assert(!gdx_music_stream_find_file(15, path, sizeof path));
    write(sRoot / "incoming/new_song.ogg", "fresh import");
    const auto cwd = fs::current_path();
    fs::current_path(sRoot / "incoming");
    char key[GDX_COURSE_KEY_MAX + 1];
    assert(gdx_course_music_adopt((sRoot / "incoming/track.gdxc").string().c_str(),
                                 "bgm.custom.new_song", key, sizeof key) == 1);
    fs::current_path(cwd);
    assert(read(sRoot / "music/new_song.ogg") == "fresh import");
    assert(!fs::exists(sRoot / "music/custom"));
    assert(!fs::exists(sRoot / "mods/music"));
    write(sRoot / "music/bgm_select.wav", "menu must remain");
    write(sRoot / "incoming/bgm_select.wav", "course must stay separate");
    assert(gdx_course_music_adopt((sRoot / "incoming/track.gdxc").string().c_str(),
                                 "bgm.custom.bgm_select", key, sizeof key) == 1);
    assert(read(sRoot / "music/bgm_select.wav") == "menu must remain");
    assert(read(song(key)->sourcePath) == "course must stay separate");
    assert(gdx_music_stream_find_file(15, path, sizeof path));
    assert(read(path) == "menu must remain");
    sRoot = originalRoot;
}

int main() {
    sRoot = fs::temp_directory_path() /
        ("gdx-music-folders-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    testDetection();
    testImport();
    testReservedNames();
    testCleanImport();
    std::error_code ec;
    fs::remove_all(sRoot, ec);
    assert(!ec);
    std::cout << "All music folder tests passed\n";
}
