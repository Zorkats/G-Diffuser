#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gdx::machine {

enum class Layout { Native, BigEndian, Wide };
enum class Resource { Commands, Vertices, Texture };
struct Span {
    const uint8_t* data = nullptr;
    size_t size = 0;
    Layout layout = Layout::Native;
};
using Read = bool (*)(void*, uintptr_t, Resource, size_t, Span&);
struct Root {
    uintptr_t address = 0;
    size_t commands = 0;
};
struct Request {
    std::array<Root, 3> roots{};
    std::array<uint8_t, 4> environment{255, 255, 255, 255};
    Read read = nullptr;
    void* user = nullptr;
};
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint8_t shade[4];
};
struct Face {
    uint32_t first;
    uint16_t material;
};
struct Material {
    uint32_t pixels;
    uint16_t width, height;
    uint8_t wrapS, wrapT;
};
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<Face> faces;
    std::vector<Material> materials;
    std::vector<uint8_t> pixels;
    size_t commands = 0;
    size_t RetainedBytes() const;
};
struct Result {
    std::shared_ptr<const Mesh> mesh;
    std::string error;
};
constexpr size_t MaxCommands = 16384;
constexpr size_t MaxFaces = 4096;
constexpr size_t MaxMaterials = 256;
constexpr size_t MaxPixelBytes = 4 * 1024 * 1024;

Result Capture(const Request& request);
class Cache {
  public:
    void Publish(int character, Result result);
    Result Get(int character) const;
  private:
    mutable std::mutex mutex;
    std::array<Result, 30> entries{};
};

}
