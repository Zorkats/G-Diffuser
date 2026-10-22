// port/tests/gdx_update_apply_tests.cpp -- unit tests for the gdx-update core.
// Links gdx_update_apply.cpp without GDX_UPDATE_HAVE_ARCHIVES, so no libzip, no zlib and no install
// folder are involved. Every file lives in a fresh folder under the system temp directory, removed
// at the end of the run.

#include "../updater/gdx_update_apply.h"

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

// The interface takes UTF-8 text, which is what an archive member carries. These tests build paths
// with std::filesystem and have to hand them over in that form.
std::string utf8(const fs::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.u8string();
#endif
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::error_code ec;

    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);

    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool has_message(const std::vector<std::string>& log, const std::string& needle) {
    for (const std::string& message : log) {
        if (message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

fs::path make_temp_root() {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path(ec) / ("gdx-upd-apply-" + std::to_string(stamp));

    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

// A filesystem that fails one move on demand. Everything else is the real thing, so a rollback runs
// against real files in a real temp folder rather than against a mock.
class FailingFs final : public GdxUpdFs {
public:
    FailingFs(GdxUpdFs* host, std::string failSourceName)
        : mHost(host), mFailSource(std::move(failSourceName)) {}

    bool exists(const std::string& path) override {
        return mHost->exists(path);
    }

    bool move(const std::string& from, const std::string& to) override {
        if (last_component(from) == mFailSource) {
            return false;
        }
        return mHost->move(from, to);
    }

    bool remove_all(const std::string& path) override {
        return mHost->remove_all(path);
    }

    bool create_dirs(const std::string& path) override {
        return mHost->create_dirs(path);
    }

private:
    static std::string last_component(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    GdxUpdFs* mHost;
    std::string mFailSource;
};

// One install folder, one staging folder and one backup folder, with the user data S24 protects
// already in place.
struct Layout {
    fs::path root;
    fs::path install;
    fs::path staging;
    fs::path backup;
};

Layout make_layout() {
    Layout layout;

    layout.root = make_temp_root();
    layout.install = layout.root / "install";
    layout.staging = layout.root / "update/staging";
    layout.backup = layout.root / "update/backup";
    fs::create_directories(layout.staging);
    return layout;
}

void stage_payload(const Layout& layout, const std::string& mainBytes, const std::string& helperBytes) {
    write_file(layout.staging / gdx_upd_main_exe_name(), mainBytes);
    write_file(layout.staging / gdx_upd_helper_exe_name(), helperBytes);
    write_file(layout.staging / "fonts/new.ttf", "new font");
    write_file(layout.staging / "shaders/one.spv", "shader");
    write_file(layout.staging / "notes.txt", "not part of the payload");
}

void stage_old_install(const Layout& layout) {
    write_file(layout.install / gdx_upd_main_exe_name(), "old main");
    write_file(layout.install / "fonts/old.ttf", "old font");
    write_file(layout.install / "saves/save1.bin", "user save");
}

std::vector<std::string> staged_items_of(const Layout& layout) {
    return std::vector<std::string>{ gdx_upd_main_exe_name(), "fonts", gdx_upd_helper_exe_name(), "shaders",
                                     "notes.txt" };
}

void test_member_paths_accepted() {
    std::string rel;
    std::string reason;

    check(gdx_upd_member_rel_path("G-Diffuser/G-Diffuser.exe", &rel, &reason) && rel == "G-Diffuser.exe",
          "member path accepts a top level executable");
    check(gdx_upd_member_rel_path("G-Diffuser/fonts/ui.ttf", &rel, &reason) && rel == "fonts/ui.ttf",
          "member path accepts a nested file");
    check(gdx_upd_member_rel_path("G-Diffuser/", &rel, &reason) && rel.empty(),
          "member path accepts the folder entry itself");
    check(gdx_upd_member_rel_path("G-Diffuser/shaders/", &rel, &reason) && rel == "shaders",
          "member path accepts a directory entry and drops the trailing slash");
}

void test_member_paths_refused() {
    std::string rel;
    std::string reason;

    check(!gdx_upd_member_rel_path("", &rel, &reason), "member path refuses an empty name");
    check(!gdx_upd_member_rel_path("/etc/passwd", &rel, &reason), "member path refuses an absolute path");
    check(!gdx_upd_member_rel_path("G-Diffuser/../evil.exe", &rel, &reason),
          "member path refuses a leading parent component");
    check(!gdx_upd_member_rel_path("G-Diffuser/fonts/../../evil.exe", &rel, &reason),
          "member path refuses a parent component inside the folder");
    check(!gdx_upd_member_rel_path("G-Diffuser/./G-Diffuser.exe", &rel, &reason),
          "member path refuses a dot component");
    check(!gdx_upd_member_rel_path("G-Diffuser//G-Diffuser.exe", &rel, &reason),
          "member path refuses an empty component");
    check(!gdx_upd_member_rel_path("G-Diffuser\\G-Diffuser.exe", &rel, &reason),
          "member path refuses a backslash");
    check(!gdx_upd_member_rel_path("G-Diffuser/..\\..\\evil.exe", &rel, &reason),
          "member path refuses a Windows style traversal");
    check(!gdx_upd_member_rel_path("C:/Windows/system32/evil.dll", &rel, &reason),
          "member path refuses a drive letter");
    check(!gdx_upd_member_rel_path("G-Diffuser/C:/evil.dll", &rel, &reason),
          "member path refuses a drive letter after the folder");
    check(!gdx_upd_member_rel_path("Other/G-Diffuser.exe", &rel, &reason),
          "member path refuses a different top folder");
    check(!gdx_upd_member_rel_path("G-Diffuser", &rel, &reason),
          "member path refuses the bare folder name without a separator");
}

void test_payload_membership() {
    check(gdx_upd_is_payload_item(gdx_upd_main_exe_name()), "payload accepts the main executable");
    check(gdx_upd_is_payload_item(gdx_upd_helper_exe_name()), "payload accepts the gdx-update helper");
    check(gdx_upd_is_payload_item("gdiffuser.o2r"), "payload accepts the asset container");
    check(gdx_upd_is_payload_item("gamecontrollerdb.txt"), "payload accepts the controller database");
    check(gdx_upd_is_payload_item("fonts"), "payload accepts fonts/");
    check(gdx_upd_is_payload_item("decomp-recipes"), "payload accepts decomp-recipes/");
    check(gdx_upd_is_payload_item("shaders"), "payload accepts shaders/");
    check(gdx_upd_is_payload_item("LICENSE"), "payload accepts LICENSE");
    check(gdx_upd_is_payload_item("THIRD_PARTY_NOTICES.md"), "payload accepts THIRD_PARTY_NOTICES.md");
    check(gdx_upd_is_payload_item("LICENSES"), "payload accepts LICENSES/");
    check(gdx_upd_is_payload_item("gdx-extract.exe") || gdx_upd_is_payload_item("gdx-extract"),
          "payload accepts the extractor on this platform");

    check(!gdx_upd_is_payload_item(""), "payload refuses an empty name");
    check(!gdx_upd_is_payload_item("saves"), "payload refuses user data in saves/");
    check(!gdx_upd_is_payload_item("generic.o2r"), "payload refuses the user's own extracted assets");
    check(!gdx_upd_is_payload_item("mods"), "payload refuses a mods folder");
    check(!gdx_upd_is_payload_item("gdx_enhancements.cfg"), "payload refuses a config file");
    check(!gdx_upd_is_payload_item("readme.txt"), "payload refuses an unknown file");
#ifdef _WIN32
    check(!gdx_upd_is_payload_item("G-Diffuser"), "payload refuses the POSIX executable name on Windows");
#else
    check(!gdx_upd_is_payload_item("G-Diffuser.exe"), "payload refuses the Windows executable name on POSIX");
#endif
}

void put_octal(unsigned char* block, size_t offset, size_t length, unsigned long long value) {
    std::snprintf(reinterpret_cast<char*>(block + offset), length, "%0*llo", static_cast<int>(length - 1), value);
}

void put_text(unsigned char* block, size_t offset, size_t length, const char* text) {
    std::memcpy(block + offset, text, std::strlen(text) < length ? std::strlen(text) : length);
}

// Fills the checksum field the way a real writer does: the field counts as spaces while summing.
void finish_tar_header(unsigned char* block) {
    unsigned long long sum = 0;

    std::memset(block + 148, ' ', 8);
    for (size_t i = 0; i < 512; i++) {
        sum += block[i];
    }
    std::snprintf(reinterpret_cast<char*>(block + 148), 8, "%06llo", sum);
    block[154] = 0;
    block[155] = ' ';
}

void build_tar_header(unsigned char* block, const char* name, const char* prefix, char type, unsigned mode,
                      unsigned long long size) {
    std::memset(block, 0, 512);
    put_text(block, 0, 100, name);
    put_octal(block, 100, 8, mode);
    put_octal(block, 108, 8, 0);
    put_octal(block, 116, 8, 0);
    put_octal(block, 124, 12, size);
    put_octal(block, 136, 12, 0);
    block[156] = static_cast<unsigned char>(type);
    put_text(block, 257, 6, "ustar");
    put_text(block, 263, 2, "00");
    put_text(block, 345, 155, prefix);
    finish_tar_header(block);
}

void test_tar_header() {
    unsigned char block[512];
    GdxUpdTarEntry entry;
    std::string reason;

    build_tar_header(block, "G-Diffuser/gdiffuser.o2r", "", '0', 0644, 4096);
    check(gdx_upd_tar_parse_header(block, &entry, &reason), "ustar header parses a regular file");
    check(entry.path == "G-Diffuser/gdiffuser.o2r", "ustar header reads the name");
    check(entry.size == 4096, "ustar header reads the size");
    check(entry.mode == 0644, "ustar header reads the mode");
    check(entry.type == '0', "ustar header reads the regular file type");

    build_tar_header(block, "fonts/", "", '5', 0755, 0);
    check(gdx_upd_tar_parse_header(block, &entry, &reason) && entry.type == '5',
          "ustar header reads a directory type");
    check(gdx_upd_tar_parse_header(block, &entry, &reason) && entry.size == 0, "ustar header reads an empty size");

    build_tar_header(block, "G-Diffuser/", "", 0, 0755, 0);
    check(gdx_upd_tar_parse_header(block, &entry, &reason) && entry.type == '0',
          "ustar header normalizes the NUL typeflag to a regular file");

    build_tar_header(block, "one.spv", "G-Diffuser/shaders", '0', 0644, 12);
    check(gdx_upd_tar_parse_header(block, &entry, &reason) && entry.path == "G-Diffuser/shaders/one.spv",
          "ustar header joins the prefix field to the name");

    build_tar_header(block, "G-Diffuser/G-Diffuser.exe", "", '0', 0755, 1);
    block[0] = 'X';
    check(!gdx_upd_tar_parse_header(block, &entry, &reason), "ustar header rejects a corrupted checksum");

    build_tar_header(block, "G-Diffuser/G-Diffuser.exe", "", '0', 0755, 1);
    block[257] = 'X';
    finish_tar_header(block);
    check(!gdx_upd_tar_parse_header(block, &entry, &reason), "ustar header rejects a foreign magic");

    build_tar_header(block, "G-Diffuser/G-Diffuser.exe", "", '0', 0755, 1);
    std::memset(block + 124, 'x', 12);
    finish_tar_header(block);
    check(!gdx_upd_tar_parse_header(block, &entry, &reason), "ustar header rejects a non octal size");

    check(!gdx_upd_tar_parse_header(nullptr, &entry, &reason), "ustar header refuses a null block");
}

void test_swap_and_rollback() {
    {
        const Layout layout = make_layout();
        std::vector<std::string> log;

        stage_old_install(layout);
        stage_payload(layout, "new main", "new helper");

        auto host = gdx_upd_make_host_fs();
        const GdxUpdSwapResult result =
            gdx_upd_swap_apply(host.get(), utf8(layout.install), utf8(layout.staging), utf8(layout.backup),
                               staged_items_of(layout), &log);

        check(result.code == GDX_UPD_OK, "swap of a good archive succeeds");
        check(result.rollbackComplete, "a successful swap reports no rollback");
        check(read_file(layout.install / gdx_upd_main_exe_name()) == "new main", "swap replaces the main executable");
        check(read_file(layout.install / gdx_upd_helper_exe_name()) == "new helper", "swap installs the new helper");
        check(fs::exists(layout.install / "fonts/new.ttf"), "swap replaces a payload folder");
        check(!fs::exists(layout.install / "fonts/old.ttf"), "the replaced folder keeps none of the old files");
        check(fs::exists(layout.install / "shaders/one.spv"), "swap installs a payload folder that did not exist");
        check(read_file(layout.install / "saves/save1.bin") == "user save", "swap never touches user saves");
        check(!fs::exists(layout.install / "notes.txt"), "swap ignores a staged item outside the payload");
        check(has_message(log, "ignored notes.txt"), "swap logs the ignored staged item");
        check(has_message(log, "installed fonts"), "swap logs each installed item");

        std::error_code ec;
        fs::remove_all(layout.root, ec);
    }
    {
        const Layout layout = make_layout();
        std::vector<std::string> log;

        stage_old_install(layout);
        stage_payload(layout, "new main", "new helper");

        auto host = gdx_upd_make_host_fs();
        // Failing on the fourth item means three moves already succeeded, including one that
        // replaced a folder and one that added a file the install never had.
        FailingFs failing(host.get(), "shaders");
        const GdxUpdSwapResult result =
            gdx_upd_swap_apply(&failing, utf8(layout.install), utf8(layout.staging), utf8(layout.backup),
                               staged_items_of(layout), &log);

        check(result.code == GDX_UPD_ERR_SWAP, "a failed swap reports the swap error");
        check(result.failedItem == "shaders", "a failed swap names the item that failed");
        check(result.rollbackComplete, "the rollback of a forced failure completes");
        check(read_file(layout.install / gdx_upd_main_exe_name()) == "old main",
              "rollback restores the previous main executable");
        check(read_file(layout.install / "fonts/old.ttf") == "old font", "rollback restores the previous folder");
        check(!fs::exists(layout.install / "fonts/new.ttf"), "rollback removes the new folder contents");
        check(!fs::exists(layout.install / gdx_upd_helper_exe_name()),
              "rollback removes a file the archive had just added");
        check(read_file(layout.install / "saves/save1.bin") == "user save", "rollback never touches user saves");
        check(has_message(log, "restored fonts"), "rollback logs each restored item");
        check(has_message(log, "ignored notes.txt"), "rollback still logs the ignored staged item");

        std::error_code ec;
        fs::remove_all(layout.root, ec);
    }
    {
        const Layout layout = make_layout();
        std::vector<std::string> log;

        // An archive that holds nothing the install owns is refused before any move happens.
        write_file(layout.staging / "notes.txt", "nothing useful");
        auto host = gdx_upd_make_host_fs();
        const GdxUpdSwapResult result = gdx_upd_swap_apply(host.get(), utf8(layout.install), utf8(layout.staging),
                                                          utf8(layout.backup), { "notes.txt" }, &log);

        check(result.code == GDX_UPD_ERR_ARCHIVE, "a swap with no payload item is refused");
        check(has_message(log, "no payload item"), "a swap with no payload item says so");

        std::error_code ec;
        fs::remove_all(layout.root, ec);
    }
}

} // namespace

int main() {
    test_member_paths_accepted();
    test_member_paths_refused();
    test_payload_membership();
    test_tar_header();
    test_swap_and_rollback();

    if (sFailures == 0) {
        std::printf("\nall gdx-update core tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d gdx-update core test(s) failed\n", sFailures);
    return 1;
}
