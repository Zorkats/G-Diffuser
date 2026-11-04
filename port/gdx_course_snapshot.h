#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gdx::snapshot {
using Bytes = std::vector<unsigned char>;
constexpr size_t BaseBytes = 0xC830;
constexpr size_t CourseBytes = 0x7E0;
constexpr size_t MaxBytes = 128 * 1024;
enum class Metadata : size_t { Extensions, Title, Forks, Settings, Count };
enum class Result { Ok, Missing, Invalid, Conflict, Io };

struct Generation {
    uint16_t version = 2;
    uint64_t disk = 0;
    uint16_t directory = 0;
    std::string courseId;
    uint8_t pointCount = 0;
    uint32_t nativeChecksum = 0;
    Bytes base;
    std::array<Bytes, size_t(Metadata::Count)> metadata;
};

bool encode(const Generation&, Bytes&);
bool decode(const Bytes&, Generation&);
bool seal(Generation&);
bool legacyImage(Generation&);
bool rebind(Generation&, uint64_t disk, uint16_t directory, const std::string& courseId);
bool portable(Generation&, bool stripGhosts);
bool packageEncode(const std::vector<Generation>&, uint32_t containerCrc, Bytes&);
bool packageDecode(const Bytes&, uint32_t containerCrc, std::vector<Generation>&);
std::string key(uint64_t disk, uint16_t directory, const std::string& courseId, const Bytes& base);

class Backend {
  public:
    virtual ~Backend() = default;
    virtual Result read(const std::string&, Bytes&) = 0;
    virtual bool publish(const std::string&, const Bytes&) = 0;
};

Result publish(Backend&, const Generation&);
Result resolve(Backend&, uint64_t disk, uint16_t directory, const std::string& courseId,
               const Bytes& base, Generation&);

class Files final : public Backend {
  public:
    explicit Files(std::filesystem::path root);
    Result read(const std::string&, Bytes&) override;
    bool publish(const std::string&, const Bytes&) override;
  private:
    std::filesystem::path root;
    bool ready = false;
};
}
