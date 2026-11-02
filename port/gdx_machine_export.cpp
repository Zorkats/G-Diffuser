#include "gdx_machine_export.h"
#include "gdx_stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>

namespace gdx::machine {
namespace {
void CheckCancel(const std::atomic<bool>& cancel) {
    if (cancel.load(std::memory_order_relaxed)) {
        throw std::runtime_error("Export cancelled");
    }
}

std::ofstream Open(const std::filesystem::path& path, bool binary = false) {
    std::ofstream out(path, binary ? std::ios::binary : std::ios::out);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    return out;
}

unsigned Wrap(uint8_t mode) {
    return (mode & 2) ? 33071 : (mode & 1) ? 33648 : 10497;
}

void WritePng(const std::filesystem::path& path, const Material& material, const uint8_t* pixels) {
    struct Sink {
        std::ofstream stream;
        bool failed = false;
    } sink{Open(path, true)};
    const auto write = [](void* context, void* data, int size) {
        auto& sink = *static_cast<Sink*>(context);
        try {
            sink.stream.write(static_cast<const char*>(data), size);
        } catch (const std::exception&) {
            sink.failed = true;
        }
    };
    if (!stbi_write_png_to_func(write, &sink, material.width, material.height, 4, pixels, material.width * 4) ||
        sink.failed) {
        throw std::runtime_error("Could not write machine material PNG");
    }
    sink.stream.close();
}

void WriteGltf(const Mesh& mesh, const std::filesystem::path& directory) {
    static_assert(sizeof(float) == 4 && sizeof(Vertex) == 36, "glTF vertex layout");
    auto bin = Open(directory / "machine.bin", true);
    // glTF is little-endian even when the host is not.
    const auto word = [&](uint32_t value) {
        const char bytes[4]{char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
        bin.write(bytes, 4);
    };
    for (const auto& vertex : mesh.vertices) {
        const float fields[8]{vertex.position[0], vertex.position[1], vertex.position[2],
                              vertex.normal[0], vertex.normal[1], vertex.normal[2], vertex.uv[0], vertex.uv[1]};
        for (float value : fields) {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            word(bits);
        }
        bin.write(reinterpret_cast<const char*>(vertex.shade), 4);
    }
    for (size_t material = 0; material < mesh.materials.size(); ++material) {
        for (const auto& face : mesh.faces) {
            if (face.material == material) {
                word(face.first);
                word(face.first + 1);
                word(face.first + 2);
            }
        }
    }
    bin.close();
    auto out = Open(directory / "machine.gltf");
    const size_t vertexBytes = mesh.vertices.size() * sizeof(Vertex);
    out << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"G-Diffuser native machine capture\"},"
           "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
           "\"buffers\":[{\"uri\":\"machine.bin\",\"byteLength\":" << vertexBytes + mesh.faces.size() * 12
        << "}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":" << vertexBytes << ",\"byteStride\":36},"
           "{\"buffer\":0,\"byteOffset\":" << vertexBytes << ",\"byteLength\":" << mesh.faces.size() * 12
        << "}],\"accessors\":[";
    float minimum[3]{mesh.vertices[0].position[0], mesh.vertices[0].position[1], mesh.vertices[0].position[2]};
    float maximum[3]{minimum[0], minimum[1], minimum[2]};
    for (const auto& vertex : mesh.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
            maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
        }
    }
    for (int attribute = 0; attribute < 4; ++attribute) {
        if (attribute) {
            out << ',';
        }
        out << "{\"bufferView\":0,\"byteOffset\":" << (attribute == 3 ? 32 : attribute * 12)
            << ",\"componentType\":" << (attribute == 3 ? 5121 : 5126)
            << ",\"count\":" << mesh.vertices.size() << ",\"type\":\""
            << (attribute == 3 ? "VEC4" : attribute == 2 ? "VEC2" : "VEC3") << '"';
        if (attribute == 3) {
            out << ",\"normalized\":true";
        }
        if (attribute == 0) {
            out << ",\"min\":[" << minimum[0] << ',' << minimum[1] << ',' << minimum[2]
                << "],\"max\":[" << maximum[0] << ',' << maximum[1] << ',' << maximum[2] << ']';
        }
        out << '}';
    }
    size_t offset = 0;
    for (size_t material = 0; material < mesh.materials.size(); ++material) {
        size_t count = 0;
        for (const auto& face : mesh.faces) {
            count += face.material == material ? 3 : 0;
        }
        out << ",{\"bufferView\":1,\"byteOffset\":" << offset << ",\"componentType\":5125,\"count\":"
            << count << ",\"type\":\"SCALAR\"}";
        offset += count * 4;
    }
    out << "],\"meshes\":[{\"primitives\":[";
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"COLOR_0\":3},"
               "\"indices\":" << 4 + i << ",\"material\":" << i << '}';
    }
    out << "]}],\"materials\":[";
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << "{\"name\":\"native_" << i << "\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":"
            << i << "},\"metallicFactor\":0,\"roughnessFactor\":1},\"alphaMode\":\"BLEND\",\"doubleSided\":true}";
    }
    out << "],\"textures\":[";
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << "{\"sampler\":" << i << ",\"source\":" << i << '}';
    }
    out << "],\"images\":[";
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << "{\"uri\":\"material_" << i << ".png\"}";
    }
    out << "],\"samplers\":[";
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << "{\"magFilter\":9728,\"minFilter\":9728,\"wrapS\":" << Wrap(mesh.materials[i].wrapS)
            << ",\"wrapT\":" << Wrap(mesh.materials[i].wrapT) << '}';
    }
    out << "]}\n";
    out.close();
}
}

ExportResult Export(const Mesh& mesh, const std::filesystem::path& parent, int character,
                    const std::atomic<bool>& cancel, std::atomic<unsigned>& progress) {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> created;
    try {
        progress.store(0);
        CheckCancel(cancel);
        if (character < 0 || character >= 30 || mesh.vertices.empty() || mesh.faces.empty() ||
            mesh.faces.size() > MaxFaces || mesh.materials.empty() || mesh.materials.size() > MaxMaterials ||
            mesh.pixels.size() > MaxPixelBytes || mesh.vertices.size() > MaxFaces * 3) {
            throw std::runtime_error("Invalid machine snapshot");
        }
        for (const auto& face : mesh.faces) {
            if (face.first > mesh.vertices.size() || mesh.vertices.size() - face.first < 3 ||
                face.material >= mesh.materials.size()) {
                throw std::runtime_error("Invalid machine snapshot face");
            }
        }
        for (const auto& material : mesh.materials) {
            const size_t size = size_t(material.width) * material.height * 4;
            if (!material.width || !material.height || material.width > 256 || material.height > 256 ||
                material.pixels > mesh.pixels.size() || size > mesh.pixels.size() - material.pixels) {
                throw std::runtime_error("Invalid machine snapshot material");
            }
        }
        for (const auto& vertex : mesh.vertices) {
            for (const auto* field : {vertex.position, vertex.normal}) {
                for (int axis = 0; axis < 3; ++axis) {
                    if (!std::isfinite(field[axis])) {
                        throw std::runtime_error("Nonfinite machine snapshot vertex");
                    }
                }
            }
            if (!std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1])) {
                throw std::runtime_error("Nonfinite machine snapshot UV");
            }
        }
        std::filesystem::create_directories(parent);
        for (unsigned version = 1; version <= 100000; ++version) {
            const auto candidate = parent / ("machine_" + std::to_string(character + 1) + "_lod1_" + std::to_string(version));
            if (std::filesystem::create_directory(candidate)) {
                directory = candidate;
                break;
            }
        }
        if (directory.empty()) {
            throw std::runtime_error("No unused machine export directory available");
        }
        created.push_back(directory / "machine.obj");
        auto obj = Open(created.back());
        created.push_back(directory / "machine.mtl");
        auto mtl = Open(created.back());
        obj << "mtllib machine.mtl\no native_machine_lod1\n";
        for (const auto& vertex : mesh.vertices) {
            obj << "v " << vertex.position[0] << ' ' << vertex.position[1] << ' ' << vertex.position[2] << ' '
                << vertex.shade[0] / 255.0f << ' ' << vertex.shade[1] / 255.0f << ' ' << vertex.shade[2] / 255.0f << '\n';
        }
        for (const auto& vertex : mesh.vertices) {
            obj << "vt " << vertex.uv[0] << ' ' << 1.0f - vertex.uv[1] << '\n';
        }
        for (const auto& vertex : mesh.vertices) {
            obj << "vn " << vertex.normal[0] << ' ' << vertex.normal[1] << ' ' << vertex.normal[2] << '\n';
        }
        unsigned previous = unsigned(-1);
        for (const auto& face : mesh.faces) {
            CheckCancel(cancel);
            if (face.material != previous) {
                obj << "usemtl native_" << face.material << '\n';
                previous = face.material;
            }
            obj << 'f';
            for (unsigned corner = 0; corner < 3; ++corner) {
                const unsigned index = face.first + corner + 1;
                obj << ' ' << index << '/' << index << '/' << index;
            }
            obj << '\n';
        }
        obj.close();
        for (size_t i = 0; i < mesh.materials.size(); ++i) {
            CheckCancel(cancel);
            const auto& material = mesh.materials[i];
            const auto name = "material_" + std::to_string(i) + ".png";
            mtl << "newmtl native_" << i << "\nKd 1 1 1\nKa 0 0 0\nKs 0 0 0\nillum 1\nd 1\nmap_Kd " << name << "\n\n";
            created.push_back(directory / name);
            WritePng(created.back(), material, mesh.pixels.data() + material.pixels);
            progress.store(unsigned((i + 1) * 80 / mesh.materials.size()));
        }
        mtl.close();
        CheckCancel(cancel);
        created.push_back(directory / "machine.bin");
        created.push_back(directory / "machine.gltf");
        WriteGltf(mesh, directory);
        CheckCancel(cancel);
        created.push_back(directory / "capture.txt");
        auto notes = Open(created.back());
        notes << "Native primary machine, LOD 1, roster entry " << character + 1 << ".\n"
              << mesh.faces.size() << " triangles, " << mesh.materials.size() << " materials, "
              << mesh.commands << " commands.\n"
                 "Import machine.gltf for texture wrap, mirror, clamp, alpha and vertex shade attributes.\n"
                 "OBJ is a geometry companion; MTL cannot express independent N64 tile wrapping or vertex alpha.\n"
                 "Native model coordinates and winding. Game lighting/camera are not baked.\n"
                 "Paint/environment is variant 0 at preparation time; later palette edits are not live.\n"
                 "No alternate LODs, shadows, race effects, workshop replacements or HD texture overrides.\n"
                 "This is not a static-body replacement pack input.\n";
        notes.close();
        progress.store(100);
        return {true, directory, {}};
    } catch (const std::exception& e) {
        // Never recursively remove the export folder: unrelated files may have appeared in it.
        if (!directory.empty()) {
            std::error_code ec;
            std::error_code firstError;
            for (const auto& path : created) {
                std::filesystem::remove(path, ec);
                if (ec && !firstError) {
                    firstError = ec;
                }
            }
            std::filesystem::remove(directory, ec);
            if (firstError) {
                ec = firstError;
            }
            if (ec) {
                return {false, directory, std::string(e.what()) + "; partial export cleanup failed: " + ec.message()};
            }
        }
        return {false, {}, e.what()};
    }
}
}
