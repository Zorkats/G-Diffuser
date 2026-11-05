// Staged save import test: compiles gdx_save_import_stage.cpp and gdx_save_import.c UNMODIFIED and
// drives them against files in a scratch folder under the system temp directory. Every save image
// is synthesized here; no real save is read or written.
#include "gdx_save_import_stage.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kImageSize = GDX_SAVE_IMPORT_SRAM_SIZE;
constexpr size_t kProfileStride = 0x19E0;
constexpr size_t kCourseBase = 0x20;
constexpr size_t kCourseStride = 0x110;
constexpr uint32_t kRecordValue = 0x11223344u;

int sFailures = 0;

void Check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        sFailures++;
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

void PutU16(std::vector<unsigned char>& image, size_t offset, uint16_t value, bool bigEndian) {
    image[offset + (bigEndian ? 0 : 1)] = static_cast<unsigned char>(value >> 8);
    image[offset + (bigEndian ? 1 : 0)] = static_cast<unsigned char>(value & 0xFF);
}

void PutU32(std::vector<unsigned char>& image, size_t offset, uint32_t value, bool bigEndian) {
    for (size_t i = 0; i < 4; i++) {
        const unsigned char byte = static_cast<unsigned char>(value >> (24 - 8 * i));
        image[offset + (bigEndian ? i : 3 - i)] = byte;
    }
}

uint16_t ByteSum(const std::vector<unsigned char>& image, size_t from, size_t to) {
    uint32_t sum = 0;
    for (size_t i = from; i < to; i++) {
        sum += image[i];
    }
    return static_cast<uint16_t>(sum & 0xFFFFu);
}

// A minimal save: every unit is zero except the two settings names and one course record. The
// settings checksum (0x022B over "F-ZERO X") reads differently in the two byte orders, so the
// converter can only accept the image in the order it was built in. All-zero units carry a zero
// checksum, which is valid in either order.
std::vector<unsigned char> BuildSave(bool bigEndian) {
    static const char kName[8] = { 'F', '-', 'Z', 'E', 'R', 'O', ' ', 'X' };
    std::vector<unsigned char> image(kImageSize, 0);
    for (size_t p = 0; p < 2; p++) {
        const size_t profile = p * kProfileStride;
        std::memcpy(image.data() + profile, kName, sizeof(kName));
        PutU16(image, profile + 0x0E, ByteSum(image, profile, profile + 0x0E), bigEndian);
    }
    const size_t course = kCourseBase + 3 * kCourseStride;
    PutU32(image, course + 4, kRecordValue, bigEndian);
    PutU16(image, course, ByteSum(image, course + 2, course + kCourseStride), bigEndian);
    return image;
}

std::vector<unsigned char> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& path, const std::vector<unsigned char>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<unsigned char> OldSave() {
    return std::vector<unsigned char>(kImageSize, 0xA5);
}

struct Scratch {
    fs::path root;
    fs::path live;
    fs::path source;

    Scratch() {
        std::error_code ec;
        root = fs::temp_directory_path(ec) / "gdx_save_import_stage_tests";
        fs::remove_all(root, ec);
        fs::create_directories(root / "saves", ec);
        live = root / "saves" / "fzerox.sav";
        source = root / "from-emulator.sra";
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void TestStageThenApply() {
    Scratch s;
    const std::vector<unsigned char> expected = BuildSave(false);
    WriteAll(s.source, BuildSave(true));
    WriteAll(s.live, OldSave());

    const gdx::SaveImportStageResult staged = gdx::StageSaveImport(s.source, s.live);
    CHECK(staged.ok);
    CHECK(staged.format == GDX_SAVE_IMPORT_FORMAT_SRA_BIG_ENDIAN);
    CHECK(gdx::SaveImportPending(s.live));
    CHECK(ReadAll(gdx::SaveImportStagedPath(s.live)) == expected);
    CHECK(ReadAll(s.live) == OldSave());

    const gdx::SaveImportApplyResult applied = gdx::ApplyStagedSaveImport(s.live, "20261104-120000");
    CHECK(applied.outcome == gdx::SaveImportApply::Applied);
    CHECK(applied.backup == s.root / "saves" / "fzerox.sav.before-import-20261104-120000");
    CHECK(ReadAll(applied.backup) == OldSave());
    CHECK(ReadAll(s.live) == expected);
    CHECK(!gdx::SaveImportPending(s.live));

    const gdx::SaveImportApplyResult again = gdx::ApplyStagedSaveImport(s.live, "20261104-120000");
    CHECK(again.outcome == gdx::SaveImportApply::NothingStaged);
    CHECK(ReadAll(s.live) == expected);
}

void TestBackupNameNeverOverwrites() {
    Scratch s;
    WriteAll(s.source, BuildSave(true));
    WriteAll(s.live, OldSave());
    const fs::path first = s.root / "saves" / "fzerox.sav.before-import-same";
    WriteAll(first, std::vector<unsigned char>(16, 0x11));

    CHECK(gdx::StageSaveImport(s.source, s.live).ok);
    const gdx::SaveImportApplyResult applied = gdx::ApplyStagedSaveImport(s.live, "same");
    CHECK(applied.outcome == gdx::SaveImportApply::Applied);
    CHECK(applied.backup == s.root / "saves" / "fzerox.sav.before-import-same-2");
    CHECK(ReadAll(applied.backup) == OldSave());
    CHECK(ReadAll(first) == std::vector<unsigned char>(16, 0x11));
}

void TestApplyWithoutLiveSave() {
    Scratch s;
    WriteAll(s.source, BuildSave(true));
    CHECK(gdx::StageSaveImport(s.source, s.live).ok);
    const gdx::SaveImportApplyResult applied = gdx::ApplyStagedSaveImport(s.live, "x");
    CHECK(applied.outcome == gdx::SaveImportApply::Applied);
    CHECK(applied.backup.empty());
    CHECK(ReadAll(s.live) == BuildSave(false));
}

void TestNativeSaveStagesAsIs() {
    Scratch s;
    WriteAll(s.source, BuildSave(false));
    const gdx::SaveImportStageResult staged = gdx::StageSaveImport(s.source, s.live);
    CHECK(staged.ok);
    CHECK(staged.format == GDX_SAVE_IMPORT_FORMAT_NATIVE);
    CHECK(ReadAll(gdx::SaveImportStagedPath(s.live)) == BuildSave(false));
}

void TestBadSourcesStageNothing() {
    Scratch s;
    WriteAll(s.live, OldSave());

    CHECK(!gdx::StageSaveImport(s.root / "missing.sra", s.live).ok);

    std::vector<unsigned char> noise(kImageSize);
    for (size_t i = 0; i < noise.size(); i++) {
        noise[i] = static_cast<unsigned char>((i * 131u + 7u) & 0xFF);
    }
    WriteAll(s.source, noise);
    CHECK(!gdx::StageSaveImport(s.source, s.live).ok);

    WriteAll(s.source, std::vector<unsigned char>(GDX_SAVE_IMPORT_SRM_SIZE + 1, 0));
    CHECK(!gdx::StageSaveImport(s.source, s.live).ok);

    WriteAll(s.source, {});
    CHECK(!gdx::StageSaveImport(s.source, s.live).ok);

    CHECK(!gdx::SaveImportPending(s.live));
    CHECK(ReadAll(s.live) == OldSave());
}

void TestDamagedStageIsRejected() {
    Scratch s;
    WriteAll(s.live, OldSave());
    std::vector<unsigned char> damaged = BuildSave(false);
    damaged[kCourseBase + 3 * kCourseStride + 4] ^= 0x01;
    WriteAll(gdx::SaveImportStagedPath(s.live), damaged);

    const gdx::SaveImportApplyResult applied = gdx::ApplyStagedSaveImport(s.live, "x");
    CHECK(applied.outcome == gdx::SaveImportApply::Rejected);
    CHECK(applied.backup.empty());
    CHECK(ReadAll(s.live) == OldSave());
    CHECK(!gdx::SaveImportPending(s.live));
    CHECK(ReadAll(gdx::SaveImportRejectedPath(s.live)) == damaged);
    CHECK(!fs::exists(s.root / "saves" / "fzerox.sav.before-import-x"));
}

// The staged file must already be native. A cartridge-order image dropped in by hand is refused
// rather than converted at launch, because only StageSaveImport writes that file.
void TestStagedFileMustBeNative() {
    Scratch s;
    WriteAll(s.live, OldSave());
    WriteAll(gdx::SaveImportStagedPath(s.live), BuildSave(true));
    CHECK(gdx::ApplyStagedSaveImport(s.live, "x").outcome == gdx::SaveImportApply::Rejected);
    CHECK(ReadAll(s.live) == OldSave());
}

void TestRestageAndCancel() {
    Scratch s;
    WriteAll(s.source, BuildSave(true));
    CHECK(gdx::StageSaveImport(s.source, s.live).ok);

    std::vector<unsigned char> second = BuildSave(false);
    WriteAll(s.source, second);
    CHECK(gdx::StageSaveImport(s.source, s.live).ok);
    CHECK(ReadAll(gdx::SaveImportStagedPath(s.live)) == second);
    CHECK(!fs::exists(fs::path(gdx::SaveImportStagedPath(s.live)) += ".tmp"));

    CHECK(gdx::CancelSaveImport(s.live));
    CHECK(!gdx::SaveImportPending(s.live));
    CHECK(gdx::CancelSaveImport(s.live));
    CHECK(gdx::ApplyStagedSaveImport(s.live, "x").outcome == gdx::SaveImportApply::NothingStaged);
    CHECK(!fs::exists(s.live));
}

void TestFormatNames() {
    CHECK(std::strcmp(gdx::SaveImportFormatName(GDX_SAVE_IMPORT_FORMAT_SRM_PARALLEL),
                      "RetroArch parallel-n64 save (.srm)") == 0);
    CHECK(std::strcmp(gdx::SaveImportFormatName(GDX_SAVE_IMPORT_FORMAT_NONE), "unknown format") == 0);
}

} // namespace

int main() {
    TestStageThenApply();
    TestBackupNameNeverOverwrites();
    TestApplyWithoutLiveSave();
    TestNativeSaveStagesAsIs();
    TestBadSourcesStageNothing();
    TestDamagedStageIsRejected();
    TestStagedFileMustBeNative();
    TestRestageAndCancel();
    TestFormatNames();

    if (sFailures != 0) {
        std::fprintf(stderr, "gdx_save_import_stage_tests: %d failure(s)\n", sFailures);
        return 1;
    }
    std::printf("gdx_save_import_stage_tests: all passed\n");
    return 0;
}
