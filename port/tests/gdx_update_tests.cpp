// port/tests/gdx_update_tests.cpp -- version compare, nightly parse, SUMS lines, and SHA-256.

#include "gdx_update.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int gdx_update_nightly_offer(const char* remoteDate, const char* remoteHash, const char* localDate,
                                        const char* localHash);

namespace {

int gFailures = 0;

void check(bool condition, const char* label) {
    if (condition) {
        std::printf("PASS: %s\n", label);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", label);
        gFailures++;
    }
}

std::string hex_of(const unsigned char digest[32]) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; i++) {
        out[(size_t)i * 2] = kHex[digest[i] >> 4];
        out[(size_t)i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

void check_sha256(const void* data, size_t len, const char* expect, const char* label) {
    unsigned char digest[32];
    gdx_update_sha256(data, len, digest);
    check(hex_of(digest) == expect, label);
}

void test_sha256() {
    check_sha256(nullptr, 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    check_sha256("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty string");
    check_sha256("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    const char* boundary = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check_sha256(boundary, std::strlen(boundary),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 56-byte boundary");
    std::vector<unsigned char> million(1000000, 'a');
    check_sha256(million.data(), million.size(),
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 one million a");
}

void test_compare() {
    check(gdx_update_compare_stable("1.1.0", "1.1.0") == 0, "compare equal");
    check(gdx_update_compare_stable("v1.2.0", "1.2.0") == 0, "compare strips v");
    check(gdx_update_compare_stable("1.2.0", "V1.2.0") == 0, "compare strips V");
    check(gdx_update_compare_stable("1.10.0", "1.9.0") == 1, "compare numeric not lexical");
    check(gdx_update_compare_stable("1.9.0", "1.10.0") == -1, "compare numeric less");
    check(gdx_update_compare_stable("10.0.0", "9.0.0") == 1, "compare longer component");
    check(gdx_update_compare_stable("1.2", "1.2.0") == 0, "compare missing component is zero");
    check(gdx_update_compare_stable("1.2.0", "1.2") == 0, "compare extra zero component");
    check(gdx_update_compare_stable("1.2.1", "1.2") == 1, "compare extra component newer");
    check(gdx_update_compare_stable("1.0.0", "1.0.1") == -1, "compare older patch");
    check(gdx_update_compare_stable("2.0.0", "1.9.9") == 1, "compare newer major");
    check(gdx_update_compare_stable("1.01", "1.1") == 0, "compare leading zeros");
    check(gdx_update_compare_stable("1.2.3-rc1", "1.2.3") == 0, "compare rejects suffix");
    check(gdx_update_compare_stable("", "1.0.0") == 0, "compare empty");
    check(gdx_update_compare_stable("1.0.0", nullptr) == 0, "compare null");
    check(gdx_update_compare_stable("v", "1.0.0") == 0, "compare bare v");
}

void test_nightly() {
    char date[9];
    char hash[41];
    check(gdx_update_parse_nightly("1.1.0-nightly.20261022.abc1234", date, hash) == 0, "parse version");
    check(std::strcmp(date, "20261022") == 0 && std::strcmp(hash, "abc1234") == 0, "parse version fields");
    check(gdx_update_parse_nightly("G-Diffuser-v1.1.0-nightly.20261022.abc1234-windows-x64.zip", date, hash) == 0,
          "parse asset name");
    check(std::strcmp(date, "20261022") == 0 && std::strcmp(hash, "abc1234") == 0, "parse asset fields");
    check(gdx_update_parse_nightly("1.1.0-nightly.20261022.ABC1234", date, hash) == 0, "parse upper hash");
    check(std::strcmp(hash, "abc1234") == 0, "parse hash lowercased");
    check(gdx_update_parse_nightly("1.1.0", date, hash) == -1, "parse missing marker");
    check(date[0] == '\0' && hash[0] == '\0', "parse failure clears outputs");
    check(gdx_update_parse_nightly("1.1.0-nightly.2026102.abc1234", date, hash) == -1, "parse short date");
    check(gdx_update_parse_nightly("1.1.0-nightly.20261022.ab", date, hash) == -1, "parse short hash");
    check(gdx_update_parse_nightly(nullptr, date, hash) == -1, "parse null");

    check(gdx_update_nightly_offer("20261022", "abc1234", "20261021", "abc1234") == 0, "offer equal hash newer date");
    check(gdx_update_nightly_offer("20261022", "abc1234", "20261022", "ABC1234") == 0, "offer equal hash ignore case");
    check(gdx_update_nightly_offer("20261023", "def5678", "20261022", "abc1234") == 1, "offer newer date different hash");
    check(gdx_update_nightly_offer("20261022", "def5678", "20261022", "abc1234") == 1, "offer same date different hash");
    check(gdx_update_nightly_offer("20261021", "def5678", "20261022", "abc1234") == 0, "offer older date");
    check(gdx_update_nightly_offer("2026102", "def5678", "20261022", "abc1234") == 0, "offer bad date");
    check(gdx_update_nightly_offer(nullptr, "def5678", "20261022", "abc1234") == 0, "offer null");
}

void test_assets() {
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-windows-x64.zip", "windows-x64") == 1, "asset windows");
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-nightly.20261022.abc1234-windows-x64.zip", "windows-x64") == 1,
          "asset nightly windows");
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-linux-x64.tar.gz", "linux-x64") == 1, "asset linux");
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-linux-arm64.tar.gz", "linux-arm64") == 1, "asset arm64");
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-macos-arm64.tar.gz", "macos-arm64") == 1, "asset macos");
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-linux-x64.zip", "linux-x64") == 0, "asset wrong extension");
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-linux-x64.tar.gz", "linux-arm64") == 0, "asset wrong platform");
    check(gdx_update_asset_matches("SHA256SUMS", "windows-x64") == 0, "asset sums name");
    check(gdx_update_asset_matches("../G-Diffuser-v1.1.0-windows-x64.zip", "windows-x64") == 0, "asset path prefix");
    check(gdx_update_asset_matches("G-Diffuser-v1.1.0-windows-x64.zip.exe", "windows-x64") == 0, "asset extra suffix");
    check(gdx_update_asset_matches("G-Diffuser-v-windows-x64.zip", "windows-x64") == 0, "asset empty version");
    check(gdx_update_asset_matches(nullptr, "windows-x64") == 0, "asset null");
}

void test_sums() {
    const char* sums =
        "\n"
        "  \n"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  other.zip\r\n"
        "not-a-hash  G-Diffuser-v1.1.0-windows-x64.zip\n"
        "\tBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB     "
        "G-Diffuser-v1.1.0-windows-x64.zip  \r\n"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc  G-Diffuser-v1.1.0-windows-x64.zip.part\n";
    char out[65];
    check(gdx_update_find_sha256(sums, "G-Diffuser-v1.1.0-windows-x64.zip", out) == 0, "sums found");
    check(std::strcmp(out, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0,
          "sums lowercased and extra whitespace");
    check(gdx_update_find_sha256(sums, "missing.zip", out) == -1, "sums missing");
    check(out[0] == '\0', "sums missing clears output");
    check(gdx_update_find_sha256(sums, "other.zip", out) == 0, "sums other entry");
    check(std::strcmp(out, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0,
          "sums exact two spaces");
    check(gdx_update_find_sha256(nullptr, "other.zip", out) == -1, "sums null text");
    check(gdx_update_find_sha256(sums, "other.zip", nullptr) == -1, "sums null out");
}

}  // namespace

int main() {
    test_sha256();
    test_compare();
    test_nightly();
    test_assets();
    test_sums();
    if (gFailures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILED\n", gFailures);
    return 1;
}
