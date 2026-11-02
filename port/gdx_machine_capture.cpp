#include "gdx_machine_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace gdx::machine {

size_t Mesh::RetainedBytes() const {
    return sizeof(*this) + vertices.capacity() * sizeof(Vertex) + faces.capacity() * sizeof(Face) +
           materials.capacity() * sizeof(Material) + pixels.capacity();
}

namespace {
uint16_t Word16(const uint8_t* p, Layout layout) {
    uint16_t value;
    std::memcpy(&value, p, 2);
    return layout == Layout::BigEndian ? uint16_t((p[0] << 8) | p[1]) : value;
}

uint32_t Word32(const uint8_t* p, Layout layout) {
    uint32_t value;
    std::memcpy(&value, p, 4);
    return layout == Layout::BigEndian ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | p[3] : value;
}

struct Tile {
    uint16_t tmem = 0, line = 0, width = 0, height = 0, uls = 0, ult = 0;
    uint8_t format = 0, size = 0, s = 0, t = 0, maskS = 0, maskT = 0, shiftS = 0, shiftT = 0;
};
struct Key {
    Tile tile;
    uint32_t generation, mux0, mux1, env, prim;
};
bool SameTile(const Tile& a, const Tile& b) {
    return a.tmem == b.tmem && a.line == b.line && a.width == b.width && a.height == b.height &&
           a.uls == b.uls && a.ult == b.ult && a.format == b.format && a.size == b.size &&
           a.s == b.s && a.t == b.t && a.maskS == b.maskS && a.maskT == b.maskT &&
           a.shiftS == b.shiftS && a.shiftT == b.shiftT;
}

struct Decoder {
    const Request& request;
    Mesh mesh;
    std::array<Vertex, 128> vertices{};
    std::array<bool, 128> loaded{};
    std::array<Tile, 8> tiles{};
    std::array<uint8_t, 4096> tmem{};
    std::array<bool, 4096> valid{};
    std::array<Key, MaxMaterials> keys{};
    std::array<uintptr_t, 64> stack{};
    uint32_t env = 0, prim = 0xFFFFFFFF, mux0 = 0, mux1 = 0, generation = 0;
    uintptr_t image = 0;
    uint32_t imageFormat = 0, imageSize = 0;
    uint32_t geometry = 0x200004;
    uint16_t scaleS = 65535, scaleT = 65535;
    uint8_t activeTile = 0;
    bool textureOn = false;

    explicit Decoder(const Request& r) : request(r) {
        for (uint8_t c : r.environment) {
            env = (env << 8) | c;
        }
    }

    Span ReadSpan(uintptr_t address, Resource kind, size_t bytes) {
        Span span;
        if (!request.read || !address || !request.read(request.user, address, kind, bytes, span) ||
            !span.data || span.size < bytes) {
            throw std::runtime_error("Referenced machine resource is unavailable or truncated");
        }
        return span;
    }

    int Combiner() const {
        const unsigned a = (mux0 >> 20) & 15, b = (mux1 >> 28) & 15;
        const unsigned c = (mux0 >> 15) & 31, d = (mux1 >> 15) & 7;
        if (((mux0 >> 5) & 15) != 0 || (mux0 & 31) != 4 || ((mux1 >> 24) & 15) != 15 ||
            ((mux1 >> 6) & 7) != 7 || ((mux1 >> 21) & 7) != 7 ||
            ((mux1 >> 18) & 7) != 7 || ((mux1 >> 3) & 7) != 7 || (mux1 & 7) != 0 ||
            ((mux0 >> 12) & 7) != 7 || ((mux0 >> 9) & 7) != 7 || ((mux1 >> 12) & 7) != 7) {
            throw std::runtime_error("Unsupported machine combiner; no incomplete export was cached");
        }
        const unsigned alpha = (mux1 >> 9) & 7;
        if (a == 15 && b == 15 && c == 31 && d == 5 && alpha == 5) {
            return 0;
        }
        if (a == 15 && b == 5 && c == 1 && d == 5 && alpha == 5) {
            return 1;
        }
        if (a == 6 && b == 15 && c == 1 && d == 7 && alpha == 6) {
            return 2;
        }
        if (a == 3 && b == 5 && c == 1 && d == 5 && alpha == 5) {
            return 3;
        }
        if (a == 1 && b == 5 && c == 8 && d == 5 && alpha == 5) {
            return 4;
        }
        throw std::runtime_error("Unsupported machine color equation");
    }

    std::array<uint8_t, 4> Texel(const Tile& tile, unsigned x, unsigned y) const {
        const unsigned bits = 4u << tile.size;
        const size_t row = tile.line * 8u;
        if (row < (tile.width * bits + 7) / 8) {
            throw std::runtime_error("Machine texture row is shorter than its tile");
        }
        const size_t offset = tile.tmem * 8u + y * row + x * bits / 8;
        const size_t bytes = std::max(1u, bits / 8);
        if (offset > tmem.size() || bytes > tmem.size() - offset) {
            throw std::runtime_error("Texture tile exceeds machine TMEM");
        }
        for (size_t i = 0; i < bytes; ++i) {
            if (!valid[offset + i]) {
                throw std::runtime_error("Texture tile references bytes that were not loaded");
            }
        }
        const uint8_t value = tmem[offset];
        const uint8_t nibble = (x & 1) ? (value & 15) : (value >> 4);
        if (tile.format == 0 && tile.size == 2) {
            const uint16_t rgba = uint16_t((value << 8) | tmem[offset + 1]);
            const auto expand = [](unsigned v) { return uint8_t((v << 3) | (v >> 2)); };
            return {expand(rgba >> 11), expand((rgba >> 6) & 31), expand((rgba >> 1) & 31),
                    uint8_t((rgba & 1) ? 255 : 0)};
        }
        if (tile.format == 4 && tile.size <= 1) {
            const uint8_t i = tile.size == 0 ? uint8_t(nibble * 17) : value;
            return {i, i, i, i};
        }
        if (tile.format == 3 && tile.size <= 2) {
            const uint8_t i = tile.size == 0 ? uint8_t((nibble >> 1) * 255 / 7) :
                              tile.size == 1 ? uint8_t((value >> 4) * 17) : value;
            const uint8_t a = tile.size == 0 ? uint8_t((nibble & 1) * 255) :
                              tile.size == 1 ? uint8_t((value & 15) * 17) : tmem[offset + 1];
            return {i, i, i, a};
        }
        throw std::runtime_error("Unsupported machine texture format (CI/YUV/RGBA32)");
    }

    uint16_t GetMaterial() {
        const int combine = Combiner();
        const bool textured = combine != 0;
        Tile tile = textured ? tiles[activeTile] : Tile{};
        if (textured && (!textureOn || !tile.width || !tile.height || tile.width > 256 || tile.height > 256)) {
            throw std::runtime_error("Missing or oversized active machine texture tile");
        }
        if (textured && ((tile.maskS && (1u << tile.maskS) != tile.width) ||
                         (tile.maskT && (1u << tile.maskT) != tile.height))) {
            throw std::runtime_error("Unsupported machine texture mask period");
        }
        const Key key{tile, generation, mux0, mux1, env, prim};
        for (size_t i = 0; i < mesh.materials.size(); ++i) {
            const auto& old = keys[i];
            if (SameTile(old.tile, key.tile) && old.generation == key.generation && old.mux0 == mux0 &&
                old.mux1 == mux1 && old.env == env && old.prim == prim) {
                return static_cast<uint16_t>(i);
            }
        }
        const uint16_t width = textured ? tile.width : 1, height = textured ? tile.height : 1;
        const size_t bytes = size_t(width) * height * 4;
        if (mesh.materials.size() >= MaxMaterials || bytes > MaxPixelBytes - mesh.pixels.size()) {
            throw std::runtime_error("Machine material/pixel budget exhausted");
        }
        const auto id = static_cast<uint16_t>(mesh.materials.size());
        keys[id] = key;
        mesh.materials.push_back({static_cast<uint32_t>(mesh.pixels.size()), width, height, tile.s, tile.t});
        const auto offset = mesh.pixels.size();
        mesh.pixels.resize(offset + bytes);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto tex = textured ? Texel(tile, x, y) : std::array<uint8_t, 4>{255, 255, 255, 255};
                auto* pixel = mesh.pixels.data() + offset + (y * width + x) * 4;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const int e = (env >> (24 - channel * 8)) & 255;
                    const int p = (prim >> (24 - channel * 8)) & 255;
                    const int v = combine == 0 ? e : combine == 1 ? e * (255 - tex[channel]) / 255 :
                                  combine == 2 ? tex[channel] : combine == 3 ?
                                  e + (p - e) * tex[channel] / 255 : e + (tex[channel] - e) * tex[3] / 255;
                    pixel[channel] = static_cast<uint8_t>(std::clamp(v, 0, 255));
                }
                pixel[3] = combine == 2 ? 255 : uint8_t(env & 255);
            }
        }
        return id;
    }

    static float Shift(float coordinate, unsigned shift) {
        return shift <= 10 ? std::ldexp(coordinate, -int(shift)) : std::ldexp(coordinate, 16 - int(shift));
    }

    void Triangle(uint32_t encoded) {
        if (geometry & 0xC0000) {
            throw std::runtime_error("Generated texture coordinates are not supported for machine export");
        }
        const unsigned indices[3]{(encoded >> 16) & 255, (encoded >> 8) & 255, encoded & 255};
        for (unsigned index : indices) {
            if ((index & 1) || index / 2 >= loaded.size() || !loaded[index / 2]) {
                throw std::runtime_error("Machine triangle references an unloaded vertex");
            }
        }
        if (mesh.faces.size() >= MaxFaces) {
            throw std::runtime_error("Machine triangle budget exhausted");
        }
        const uint16_t material = GetMaterial();
        const auto& tile = tiles[activeTile];
        const auto& a = vertices[indices[0] / 2];
        const auto& b = vertices[indices[1] / 2];
        const auto& c = vertices[indices[2] / 2];
        float normal[3]{
            (b.position[1] - a.position[1]) * (c.position[2] - a.position[2]) -
                (b.position[2] - a.position[2]) * (c.position[1] - a.position[1]),
            (b.position[2] - a.position[2]) * (c.position[0] - a.position[0]) -
                (b.position[0] - a.position[0]) * (c.position[2] - a.position[2]),
            (b.position[0] - a.position[0]) * (c.position[1] - a.position[1]) -
                (b.position[1] - a.position[1]) * (c.position[0] - a.position[0])};
        float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (length > 0) {
            for (auto& axis : normal) {
                axis /= length;
            }
        } else {
            normal[2] = 1;
        }
        mesh.faces.push_back({static_cast<uint32_t>(mesh.vertices.size()), material});
        const bool textured = Combiner() != 0;
        for (unsigned index : indices) {
            Vertex v = vertices[index / 2];
            v.shade[3] = 255;
            if (!(geometry & 0x20000)) {
                std::copy(std::begin(normal), std::end(normal), v.normal);
                if (!(geometry & 4)) {
                    std::fill(std::begin(v.shade), std::end(v.shade), uint8_t(255));
                } else if (!(geometry & 0x200000)) {
                    std::copy(std::begin(a.shade), std::end(a.shade), v.shade);
                }
            }
            if (textured) {
                v.uv[0] = (Shift(v.uv[0] * scaleS / 65536.0f, tile.shiftS) - tile.uls / 4.0f) / tile.width;
                v.uv[1] = (Shift(v.uv[1] * scaleT / 65536.0f, tile.shiftT) - tile.ult / 4.0f) / tile.height;
            } else {
                v.uv[0] = v.uv[1] = 0.5f;
            }
            mesh.vertices.push_back(v);
        }
    }

    void List(Root root, size_t depth = 0) {
        if (depth >= stack.size()) {
            throw std::runtime_error("Machine display-list nesting limit exceeded");
        }
        for (size_t i = 0; i < depth; ++i) {
            if (stack[i] == root.address) {
                throw std::runtime_error("Cyclic machine display list");
            }
        }
        stack[depth] = root.address;
        auto span = ReadSpan(root.address, Resource::Commands, 8);
        const size_t stride = span.layout == Layout::Wide ? 16 : 8;
        const size_t count = root.commands ? root.commands : std::min(span.size / stride, MaxCommands);
        if (count > span.size / stride || count > MaxCommands) {
            throw std::runtime_error("Invalid machine display-list extent");
        }
        for (size_t i = 0; i < count; ++i) {
            if (++mesh.commands > MaxCommands) {
                throw std::runtime_error("Machine command budget exhausted");
            }
            const auto* p = span.data + i * stride;
            const uint32_t w0 = Word32(p, span.layout);
            uintptr_t pointer = 0;
            if (stride == 16) {
                std::memcpy(&pointer, p + 8, sizeof(pointer));
            } else {
                pointer = Word32(p + 4, span.layout);
            }
            const uint32_t w1 = static_cast<uint32_t>(pointer);
            const unsigned op = w0 >> 24;
            switch (op) {
                case 0xDF: return;
                case 0xDE:
                    List({pointer, 0}, depth + 1);
                    if ((w0 >> 16) & 255) {
                        return;
                    }
                    break;
                case 0x01: {
                    const unsigned n = (w0 >> 12) & 255, end = (w0 >> 1) & 127;
                    if (!n || end < n || end > vertices.size()) {
                        throw std::runtime_error("Invalid machine vertex load");
                    }
                    const auto data = ReadSpan(pointer, Resource::Vertices, n * 16);
                    for (unsigned v = 0; v < n; ++v) {
                        const auto* src = data.data + v * 16;
                        auto& dst = vertices[end - n + v];
                        for (unsigned axis = 0; axis < 3; ++axis) {
                            dst.position[axis] = float(int16_t(Word16(src + axis * 2, data.layout)));
                        }
                        dst.uv[0] = int16_t(Word16(src + 8, data.layout)) / 32.0f;
                        dst.uv[1] = int16_t(Word16(src + 10, data.layout)) / 32.0f;
                        std::memcpy(dst.shade, src + 12, 4);
                        if (geometry & 0x20000) {
                            float norm = 0;
                            for (unsigned axis = 0; axis < 3; ++axis) {
                                dst.normal[axis] = float(int8_t(src[12 + axis]));
                                norm += dst.normal[axis] * dst.normal[axis];
                                dst.shade[axis] = 255;
                            }
                            norm = std::sqrt(norm);
                            if (norm > 0) {
                                for (auto& axis : dst.normal) {
                                    axis /= norm;
                                }
                            } else {
                                dst.normal[2] = 1;
                            }
                        }
                        loaded[end - n + v] = true;
                    }
                    break;
                }
                case 0x05: Triangle(w0); break;
                case 0x06: case 0x07: Triangle(w0); Triangle(w1); break;
                case 0xD7:
                    activeTile = (w0 >> 8) & 7;
                    textureOn = (w0 & 0xFE) != 0;
                    scaleS = uint16_t(w1 >> 16);
                    scaleT = uint16_t(w1);
                    break;
                case 0xFA: prim = w1; break;
                case 0xD9: geometry = (geometry & (w0 & 0xFFFFFF)) | w1; break;
                case 0xFB: env = w1; break;
                case 0xFC: mux0 = w0; mux1 = w1; break;
                case 0xFD: image = pointer; imageFormat = (w0 >> 21) & 7; imageSize = (w0 >> 19) & 3; break;
                case 0xF5: {
                    auto& tile = tiles[(w1 >> 24) & 7];
                    tile.tmem = w0 & 511;
                    tile.line = (w0 >> 9) & 511;
                    tile.format = (w0 >> 21) & 7;
                    tile.size = (w0 >> 19) & 3;
                    tile.s = (w1 >> 8) & 3;
                    tile.t = (w1 >> 18) & 3;
                    tile.maskS = (w1 >> 4) & 15;
                    tile.maskT = (w1 >> 14) & 15;
                    tile.shiftS = w1 & 15;
                    tile.shiftT = (w1 >> 10) & 15;
                    break;
                }
                case 0xF2: {
                    auto& tile = tiles[(w1 >> 24) & 7];
                    tile.uls = (w0 >> 12) & 4095;
                    tile.ult = w0 & 4095;
                    const unsigned endS = (w1 >> 12) & 4095, endT = w1 & 4095;
                    if (endS < tile.uls || endT < tile.ult) {
                        throw std::runtime_error("Invalid machine tile dimensions");
                    }
                    tile.width = uint16_t((endS - tile.uls) / 4 + 1);
                    tile.height = uint16_t((endT - tile.ult) / 4 + 1);
                    break;
                }
                case 0xF3: {
                    if (w0 & 0xFFFFFF) {
                        throw std::runtime_error("Unsupported nonzero texture load origin");
                    }
                    const size_t offset = tiles[(w1 >> 24) & 7].tmem * 8u;
                    const size_t bytes = (((w1 >> 12) & 4095) + 1) * (4u << imageSize) / 8;
                    if (!bytes || offset > tmem.size() || bytes > tmem.size() - offset) {
                        throw std::runtime_error("Machine texture load exceeds TMEM");
                    }
                    const auto src = ReadSpan(image, Resource::Texture, bytes);
                    std::memcpy(tmem.data() + offset, src.data, bytes);
                    if (imageFormat == 0 && imageSize == 2 && src.layout != Layout::BigEndian) {
                        for (size_t byte = 0; byte + 1 < bytes; byte += 2) {
                            const uint16_t color = Word16(src.data + byte, Layout::Native);
                            tmem[offset + byte] = uint8_t(color >> 8);
                            tmem[offset + byte + 1] = uint8_t(color);
                        }
                    }
                    std::fill(valid.begin() + offset, valid.begin() + offset + bytes, true);
                    ++generation;
                    break;
                }
                case 0xDB:
                    if (((w0 >> 16) & 255) == 6 || ((w0 >> 16) & 255) == 0) {
                        throw std::runtime_error("Machine list changes segments or matrix state");
                    }
                    break;
                // Lighting remains editable in the DCC; camera and raster state are not baked.
                case 0x00: case 0xDC: case 0xE2: case 0xE3:
                case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xEF: case 0xF9:
                    break;
                default: {
                    char message[96];
                    std::snprintf(message, sizeof(message), "Unsupported machine command 0x%02X at command %zu", op, mesh.commands);
                    throw std::runtime_error(message);
                }
            }
        }
        throw std::runtime_error("Machine display list has no end command");
    }
};
}

Result Capture(const Request& request) {
    try {
        Decoder decoder(request);
        for (size_t i = 0; i < request.roots.size(); ++i) {
            const auto& root = request.roots[i];
            if (i == 2) {
                // Racer draw sets body paint after shared setup and texture preparation.
                decoder.env = 0;
                for (uint8_t channel : request.environment) {
                    decoder.env = (decoder.env << 8) | channel;
                }
            }
            if (root.address) {
                decoder.List(root);
            }
        }
        if (decoder.mesh.faces.empty()) {
            return {nullptr, "No captured triangles"};
        }
        return {std::make_shared<Mesh>(std::move(decoder.mesh)), {}};
    } catch (const std::exception& e) {
        return {nullptr, e.what()};
    }
}

void Cache::Publish(int character, Result result) {
    if (character >= 0 && character < 30) {
        const std::lock_guard<std::mutex> lock(mutex);
        entries[character] = std::move(result);
    }
}

Result Cache::Get(int character) const {
    if (character < 0 || character >= 30) {
        return {nullptr, "Invalid machine identity"};
    }
    const std::lock_guard<std::mutex> lock(mutex);
    return entries[character];
}

}
