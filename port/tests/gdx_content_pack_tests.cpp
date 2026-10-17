// port/tests/gdx_content_pack_tests.cpp -- unit tests for .gdxp share packages (gdx_content_pack.h).
// Links gdx_content_pack.cpp and libzip (through libultraship). Every file lives in a fresh folder
// under the system temp directory, removed at the end.

#include "../gdx_content_pack.h"

#include <zip.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

int sFailures;

void check(bool condition, const char* label) {
    if (condition) {
        std::printf("PASS: %s\n", label);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Bytes that do not compress, so a stored member keeps them verbatim in the archive.
std::string noise(size_t length, unsigned seed) {
    std::string out(length, '\0');
    for (size_t i = 0; i < length; i++) {
        seed = seed * 1103515245u + 12345u;
        out[i] = static_cast<char>((seed >> 16) & 0xFF);
    }
    return out;
}

// Writes a zip with exactly these members, whatever their names, the way a hostile tool would.
bool make_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& members) {
    int error = 0;
    zip_t* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);

    if (archive == nullptr) {
        return false;
    }
    for (const auto& member : members) {
        zip_source_t* data = zip_source_buffer(archive, member.second.data(), member.second.size(), 0);
        if (data == nullptr || zip_file_add(archive, member.first.c_str(), data, ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(data);
            zip_discard(archive);
            return false;
        }
    }
    return zip_close(archive) == 0;
}

int extract(const fs::path& pack, const fs::path& dest, int withAudio, std::string* container = nullptr) {
    char name[GDX_CONTENT_PACK_NAME_MAX + 1];
    const int rc = gdx_content_pack_extract(pack.string().c_str(), dest.string().c_str(), withAudio, name,
                                            static_cast<int>(sizeof name));
    if (container != nullptr) {
        *container = name;
    }
    return rc;
}

size_t file_count(const fs::path& dir) {
    std::error_code ec;
    size_t count = 0;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        count++;
    }
    return count;
}

const std::string kContainer = "GXC1 container bytes";
const std::string kExtension = "GXE1 extension bytes";
const std::string kTitle = "GXT1 title bytes";
const std::string kSong = noise(200000, 7);

void stage_export(const fs::path& dir) {
    fs::create_directories(dir);
    write_file(dir / "Track One.gdxc", kContainer);
    write_file(dir / "Track One.gdxe", kExtension);
    write_file(dir / "Track One.gdxt", kTitle);
    write_file(dir / "ocean_theme.ogg", kSong);
}

void test_round_trip(const fs::path& root) {
    const fs::path stage = root / "stage";
    const fs::path pack = root / "Track One.gdxp";
    const fs::path out = root / "out";
    std::string container;

    stage_export(stage);
    check(gdx_content_pack_write_dir(pack.string().c_str(), stage.string().c_str()) == GDX_CONTENT_PACK_OK,
          "write_dir packs a container, its sidecars and a song");
    check(extract(pack, out, 1, &container) == GDX_CONTENT_PACK_OK && container == "Track One.gdxc",
          "extract accepts the package and names its container");
    check(read_file(out / "Track One.gdxc") == kContainer && read_file(out / "Track One.gdxe") == kExtension &&
              read_file(out / "Track One.gdxt") == kTitle && read_file(out / "ocean_theme.ogg") == kSong,
          "extract restores every member byte for byte");
    check(file_count(out) == 4, "extract writes nothing besides the members");

    int error = 0;
    zip_t* archive = zip_open(pack.string().c_str(), ZIP_RDONLY, &error);
    zip_stat_t st;
    zip_stat_init(&st);
    check(archive != nullptr && zip_stat(archive, "ocean_theme.ogg", 0, &st) == 0 &&
              (st.valid & ZIP_STAT_COMP_METHOD) != 0 && st.comp_method == ZIP_CM_STORE,
          "write_dir stores audio without compressing it");
    if (archive != nullptr) {
        zip_discard(archive);
    }

    const fs::path again = root / "again.gdxp";
    check(gdx_content_pack_write_dir(again.string().c_str(), stage.string().c_str()) == GDX_CONTENT_PACK_OK &&
              read_file(again).size() == read_file(pack).size(),
          "two packages of the same folder have the same size");

    check(extract(pack, out, 0, &container) == GDX_CONTENT_PACK_OK && fs::exists(out / "Track One.gdxc") &&
              fs::exists(out / "Track One.gdxe") && !fs::exists(out / "ocean_theme.ogg"),
          "extract without audio leaves the song out and clears the earlier one");

    write_file(out / "stale.gdxc", "old");
    check(extract(pack, out, 0) == GDX_CONTENT_PACK_OK && !fs::exists(out / "stale.gdxc"),
          "extract clears files left in the destination first");
}

void test_write_rules(const fs::path& root) {
    const fs::path pack = root / "w.gdxp";
    const fs::path unknown = root / "unknown";
    stage_export(unknown);
    write_file(unknown / "notes.txt", "x");
    check(gdx_content_pack_write_dir(pack.string().c_str(), unknown.string().c_str()) == GDX_CONTENT_PACK_ERR_MEMBER,
          "write_dir refuses a folder holding an unknown file type");

    const fs::path empty = root / "nocontainer";
    fs::create_directories(empty);
    write_file(empty / "song.ogg", "x");
    check(gdx_content_pack_write_dir(pack.string().c_str(), empty.string().c_str()) == GDX_CONTENT_PACK_ERR_CONTAINER,
          "write_dir refuses a folder with no .gdxc");
    check(!fs::exists(pack), "a refused write_dir leaves no package");

    check(gdx_content_pack_write_dir(pack.string().c_str(), (root / "missing").string().c_str()) ==
              GDX_CONTENT_PACK_ERR_IO,
          "write_dir reports a missing folder as an I/O error");
}

void test_hostile(const fs::path& root) {
    const fs::path pack = root / "h.gdxp";
    const fs::path out = root / "hostile-out";
    const std::string c = kContainer;

    struct Case {
        std::vector<std::pair<std::string, std::string>> members;
        int expected;
        const char* label;
    };
    std::vector<std::pair<std::string, std::string>> many = { { "a.gdxc", c } };
    for (int i = 0; i < GDX_CONTENT_PACK_MAX_MEMBERS; i++) {
        many.push_back({ "song" + std::to_string(i) + ".ogg", "x" });
    }
    const Case cases[] = {
        { { { "../evil.gdxc", c } }, GDX_CONTENT_PACK_ERR_MEMBER, "extract refuses a parent-folder member" },
        { { { "a.gdxc", c }, { "sub/song.ogg", "x" } }, GDX_CONTENT_PACK_ERR_MEMBER,
          "extract refuses a member inside a folder" },
        { { { "a.gdxc", c }, { "sub\\song.ogg", "x" } }, GDX_CONTENT_PACK_ERR_MEMBER,
          "extract refuses a backslash path" },
        { { { "C:a.gdxc", c } }, GDX_CONTENT_PACK_ERR_MEMBER, "extract refuses a drive-relative name" },
        { { { "a.gdxc", c }, { "run.exe", "x" } }, GDX_CONTENT_PACK_ERR_MEMBER, "extract refuses an unknown type" },
        { { { "a.gdxc", c }, { ".hidden.ogg", "x" } }, GDX_CONTENT_PACK_ERR_MEMBER,
          "extract refuses a hidden member" },
        { { { "a.gdxc", c }, { "song.ogg.", "x" } }, GDX_CONTENT_PACK_ERR_MEMBER,
          "extract refuses a name ending in a dot" },
        { { { "CON.gdxc", c } }, GDX_CONTENT_PACK_ERR_MEMBER, "extract refuses a Windows device name" },
        { { { "lpt1.song.ogg", "x" }, { "a.gdxc", c } }, GDX_CONTENT_PACK_ERR_MEMBER,
          "extract refuses a device name before any dot" },
        { { { "song.ogg", "x" } }, GDX_CONTENT_PACK_ERR_CONTAINER, "extract refuses a package with no .gdxc" },
        { { { "a.gdxc", c }, { "b.gdxc", c } }, GDX_CONTENT_PACK_ERR_CONTAINER,
          "extract refuses a package with two .gdxc" },
        { { { "a.gdxc", c }, { "b.gdxe", "x" } }, GDX_CONTENT_PACK_ERR_STEM,
          "extract refuses a sidecar with another stem" },
        { { { "a.gdxc", c }, { "A.gdxe", "x" } }, GDX_CONTENT_PACK_ERR_STEM,
          "extract refuses a sidecar whose stem differs only in case" },
        { { { "a.gdxc", c }, { "song.ogg", "x" }, { "SONG.OGG", "y" } }, GDX_CONTENT_PACK_ERR_DUPLICATE,
          "extract refuses two names that differ only in case" },
        { many, GDX_CONTENT_PACK_ERR_TOO_MANY, "extract refuses more than the member limit" },
        { { { "a.gdxc", c }, { "a.gdxe", std::string(GDX_CONTENT_PACK_MAX_DATA_BYTES + 1, '\0') } },
          GDX_CONTENT_PACK_ERR_TOO_LARGE, "extract refuses a data member over its cap" },
    };

    for (const Case& test : cases) {
        fs::create_directories(out);
        write_file(out / "keep.gdxc", "earlier");
        const bool built = make_zip(pack, test.members);
        const int rc = extract(pack, out, 1);
        if (!built || rc != test.expected) {
            std::fprintf(stderr, "  built %d, rc %d (%s)\n", built ? 1 : 0, rc, gdx_content_pack_error_string(rc));
        }
        check(built && rc == test.expected && fs::exists(out / "keep.gdxc") && file_count(out) == 1 &&
                  !fs::exists(root / "evil.gdxc"),
              test.label);
    }
}

void test_damaged(const fs::path& root) {
    const fs::path stage = root / "stage-d";
    const fs::path pack = root / "d.gdxp";
    const fs::path out = root / "damaged-out";

    write_file(root / "plain.gdxp", "this is not a zip archive at all");
    check(extract(root / "plain.gdxp", out, 1) == GDX_CONTENT_PACK_ERR_NOT_ZIP, "extract refuses a file that is not a zip");
    check(extract(root / "absent.gdxp", out, 1) == GDX_CONTENT_PACK_ERR_IO, "extract reports a missing package as an I/O error");

    stage_export(stage);
    gdx_content_pack_write_dir(pack.string().c_str(), stage.string().c_str());
    std::string bytes = read_file(pack);
    const size_t at = bytes.find(kSong.substr(1000, 64));
    check(at != std::string::npos, "the stored song bytes are found in the package");
    if (at != std::string::npos) {
        bytes[at + 10] = static_cast<char>(bytes[at + 10] ^ 0x5A);
        write_file(pack, bytes);
        check(extract(pack, out, 1) == GDX_CONTENT_PACK_ERR_CORRUPT && file_count(out) == 0,
              "extract refuses a damaged song and leaves the destination empty");
        check(extract(pack, out, 0) == GDX_CONTENT_PACK_OK,
              "extract without audio does not read the damaged song");
    }
}

void test_helpers(const fs::path& root) {
    const fs::path dir = root / "clear";
    fs::create_directories(dir / "inner");
    write_file(dir / "a.gdxc", "x");
    write_file(dir / "inner" / "b.gdxc", "x");
    check(gdx_content_pack_clear_dir(dir.string().c_str(), 1) == GDX_CONTENT_PACK_OK && !fs::exists(dir / "a.gdxc") &&
              fs::exists(dir / "inner" / "b.gdxc"),
          "clear_dir deletes direct files and never recurses");
    fs::remove_all(dir / "inner");
    check(gdx_content_pack_clear_dir(dir.string().c_str(), 1) == GDX_CONTENT_PACK_OK && !fs::exists(dir),
          "clear_dir removes the emptied folder when asked");
    check(gdx_content_pack_clear_dir((root / "made").string().c_str(), 0) == GDX_CONTENT_PACK_OK &&
              fs::is_directory(root / "made"),
          "clear_dir creates a missing folder");

    check(gdx_content_pack_is_package_name("Track.gdxp") == 1 && gdx_content_pack_is_package_name("TRACK.GDXP") == 1,
          "is_package_name accepts .gdxp in any case");
    check(gdx_content_pack_is_package_name(".gdxp") == 0 && gdx_content_pack_is_package_name("a.gdxc") == 0 &&
              gdx_content_pack_is_package_name(nullptr) == 0,
          "is_package_name rejects other names");
    for (int rc = GDX_CONTENT_PACK_OK; rc >= GDX_CONTENT_PACK_ERR_CORRUPT; rc--) {
        if (std::strcmp(gdx_content_pack_error_string(rc), "unknown package error") == 0) {
            check(false, "error_string names every error code");
            return;
        }
    }
    check(true, "error_string names every error code");
}

} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("gdx_content_pack_tests_" + std::to_string(stamp));

    fs::create_directories(root);
    test_round_trip(root);
    test_write_rules(root);
    test_hostile(root);
    test_damaged(root);
    test_helpers(root);
    std::error_code ec;
    fs::remove_all(root, ec);

    if (sFailures != 0) {
        std::fprintf(stderr, "%d content pack test(s) failed\n", sFailures);
        return 1;
    }
    std::printf("All content pack tests passed\n");
    return 0;
}
