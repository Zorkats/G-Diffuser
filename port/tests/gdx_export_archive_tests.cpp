// port/tests/gdx_export_archive_tests.cpp -- "Export everything" on a made-up install (gdx_export_archive.h).

#include "../gdx_export_all.h"
#include "../gdx_export_archive.h"

#include <zip.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;

static int sFailures;

#define CHECK(cond, text)                         \
    do {                                          \
        if (cond) {                               \
            std::printf("PASS: %s\n", (text));    \
        } else {                                  \
            std::printf("FAIL: %s\n", (text));    \
            sFailures++;                          \
        }                                         \
    } while (0)

static void put(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

static std::set<std::string> zip_names(const fs::path& zipPath, std::string* firstContent, const char* firstName) {
    std::set<std::string> names;
    int err = 0;
    zip_t* archive = zip_open(zipPath.string().c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &err);

    if (archive == nullptr) {
        return names;
    }
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < count; i++) {
        const char* name = zip_get_name(archive, static_cast<zip_uint64_t>(i), ZIP_FL_ENC_UTF_8);
        if (name != nullptr) {
            names.insert(name);
            if (firstContent != nullptr && firstName != nullptr && std::string(name) == firstName) {
                zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(i), 0);
                char buffer[256];
                const zip_int64_t got = file != nullptr ? zip_fread(file, buffer, sizeof buffer) : -1;
                if (got > 0) {
                    firstContent->assign(buffer, static_cast<size_t>(got));
                }
                if (file != nullptr) {
                    zip_fclose(file);
                }
            }
        }
    }
    zip_close(archive);
    return names;
}

static void make_install(const fs::path& root) {
    // Tracks and snapshots.
    put(root / "course-meta.gms", "GMS1");
    put(root / "saves/disk.ndd.gdd", "journal");
    put(root / "saves/disk.ndd.gdd.recovery.snapshots/TRACK.snap", "snap");
    put(root / "exports/LKT.gdxc", "track");
    // Profiles, saves, records and ghosts.
    put(root / "profiles.gps", "profiles");
    put(root / "saves/fzerox.sav", "sram");
    put(root / "ghosts/ghost_1.gdg", "ghost");
    put(root / "gp_resume.bin", "resume");
    // Settings and mods.
    put(root / "gdiffuser.cfg.json", "{}");
    put(root / "mods/pack.o2r", "PK");
    put(root / "music/Big Blue.ogg", "OggS");
    // Signing keys.
    put(root / "profile-keys.gpk", "seeds");
    // Never exported.
    put(root / "baserom.us.rev0.z64", "ROM");
    put(root / "fzerox.o2r", "ROM-derived");
    put(root / "mods/~romhacks/Climax.o2r", "ROM-derived");
    put(root / "dump/manifest.tsv", "dump");
    put(root / "logs/run.log", "log");
    put(root / "saves/disk.ndd.gdd.bak", "old");
    put(root / "exports/gdiffuser-export-20260101-000000.zip", "PK");
    put(root / "exports/machines/machine_1/machine.obj", "captured from the game");
}

static void on_progress(double done, void* ctx) {
    double* last = static_cast<double*>(ctx);
    if (done >= *last) {
        *last = done;
    }
}

static void test_one_folder(const fs::path& base) {
    const fs::path root = base / "one";
    make_install(root);

    const gdx::ExportPlan plan = gdx::PlanExport(root, root);
    CHECK(plan.error.empty(), "a readable install lists without an error");
    CHECK(plan.count[0] == 4 && plan.count[1] == 4 && plan.count[2] == 3 && plan.count[3] == 1,
          "each group counts its own files");
    CHECK(gdx::ExportCount(plan, GDX_EXPORT_ALL_GROUPS) == 12 && gdx::ExportBytes(plan, GDX_EXPORT_KEYS) == 5,
          "the totals follow the picked groups");

    const fs::path zipPath = root / "exports" / "gdiffuser-export-20261106-010203.zip";
    std::string error;
    double last = -1.0;
    const unsigned noKeys = GDX_EXPORT_TRACKS | GDX_EXPORT_PLAYER | GDX_EXPORT_SETTINGS;
    CHECK(gdx::WriteExport(plan, noKeys, zipPath, on_progress, &last, error) && error.empty(),
          "the export writes its zip");
    CHECK(last >= 1.0, "progress reaches the end");

    std::string profiles;
    const std::set<std::string> names = zip_names(zipPath, &profiles, "profiles.gps");
    const std::set<std::string> expected = { "course-meta.gms",
                                             "saves/disk.ndd.gdd",
                                             "saves/disk.ndd.gdd.recovery.snapshots/TRACK.snap",
                                             "exports/LKT.gdxc",
                                             "profiles.gps",
                                             "saves/fzerox.sav",
                                             "ghosts/ghost_1.gdg",
                                             "gp_resume.bin",
                                             "gdiffuser.cfg.json",
                                             "mods/pack.o2r",
                                             "music/Big Blue.ogg" };
    CHECK(names == expected, "the zip holds the picked groups at their install paths, and no ROM data");
    CHECK(profiles == "profiles", "a file reads back unchanged");
    CHECK(names.count("profile-keys.gpk") == 0, "signing keys stay out unless picked");

    // A second export lists the first one as never exported.
    const gdx::ExportPlan again = gdx::PlanExport(root, root);
    CHECK(gdx::ExportCount(again, GDX_EXPORT_ALL_GROUPS) == 12, "an earlier export is not exported again");

    const fs::path keysZip = root / "exports" / "gdiffuser-export-20261106-010204.zip";
    CHECK(gdx::WriteExport(again, GDX_EXPORT_KEYS, keysZip, nullptr, nullptr, error) &&
              zip_names(keysZip, nullptr, nullptr) == std::set<std::string>{ "profile-keys.gpk" },
          "signing keys export when picked");

    const fs::path emptyZip = root / "exports" / "gdiffuser-export-20261106-010205.zip";
    CHECK(!gdx::WriteExport(again, 0, emptyZip, nullptr, nullptr, error) && !error.empty() && !fs::exists(emptyZip),
          "no group picked writes nothing and says why");

    const fs::path badZip = root / "profiles.gps" / "x.zip"; // a file where a folder should be
    CHECK(!gdx::WriteExport(again, noKeys, badZip, nullptr, nullptr, error) && !error.empty(),
          "a zip that cannot be created fails with a reason");
}

static void test_split_folders(const fs::path& base) {
    const fs::path data = base / "data";
    const fs::path exe = base / "exe";
    put(data / "profiles.gps", "profiles");
    put(data / "music/data-side.ogg", "OggS");
    put(exe / "music/Big Blue.ogg", "OggS");
    put(exe / "music/data-side.ogg", "other copy");
    put(exe / "exports/LKT.gdxc", "track");
    put(exe / "profiles.gps", "beside the exe, not read");
    put(exe / "saves/fzerox.sav", "beside the exe, not read");

    const gdx::ExportPlan plan = gdx::PlanExport(data, exe);
    std::set<std::string> members;
    for (const gdx::ExportFile& file : plan.files) {
        members.insert(file.member);
    }
    CHECK(members == std::set<std::string>({ "profiles.gps", "music/data-side.ogg", "music/Big Blue.ogg",
                                             "exports/LKT.gdxc" }),
          "music and exports come from beside the executable, the rest from the data folder, each once");

    const gdx::ExportPlan missing = gdx::PlanExport(base / "no such folder", base / "no such folder");
    CHECK(!missing.error.empty() && missing.files.empty(), "a missing install says so");
}

// --list <install>: prints what an export of that folder would take, by group, and writes nothing.
static int list_install(const char* folder) {
    const gdx::ExportPlan plan = gdx::PlanExport(fs::u8path(folder), fs::u8path(folder));
    for (const gdx::ExportFile& file : plan.files) {
        std::printf("%x %10llu %s\n", file.group, static_cast<unsigned long long>(file.size), file.member.c_str());
    }
    std::printf("tracks %zu, player %zu, settings %zu, keys %zu%s%s\n", plan.count[0], plan.count[1], plan.count[2],
                plan.count[3], plan.error.empty() ? "" : ", error: ", plan.error.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--list") {
        return list_install(argv[2]);
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("gdx_export_archive_tests_" + std::to_string(stamp));

    test_one_folder(base);
    test_split_folders(base);

    std::error_code ec;
    fs::remove_all(base, ec);
    if (sFailures != 0) {
        std::printf("%d check(s) failed\n", sFailures);
        return 1;
    }
    std::printf("all export archive checks passed\n");
    return 0;
}
