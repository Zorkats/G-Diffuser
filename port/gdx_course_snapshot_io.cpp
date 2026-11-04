#include "gdx_course_snapshot_io.h"
#include "gdx_course_snapshot_state.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_state.h"
#include "gdx_course_registry.h"
#include "gdx_course_music.h"
#include "gdx_course_settings.h"
#include "gdx_course_branch_format.h"
#include "gdx_recovery_file.h"
#include "gdx_track_recovery_runtime.h"
#include "disk_savefile.h"
#include "port_log.h"

#include <cstring>
#include <algorithm>
#include <cctype>
#include <memory>

extern "C" void* gdx_snapshot_native_thread(void);

namespace {
using namespace gdx::snapshot;
void* owner = nullptr;
unsigned depth = 0;
std::vector<Generation> exported;
std::vector<Generation> imported;
std::vector<Generation> prepared;

bool identity(Generation& g, unsigned short directory, const char* name, const char* extension) {
    if (!name || !extension) {
        return false;
    }
    size_t length = 0;
    while (length < 21 && name[length]) {
        ++length;
    }
    if (!length || length > 20 || std::strlen(extension) != 4) {
        return false;
    }
    g.disk = gdx_disk_save_identity();
    g.directory = directory;
    g.courseId = std::string(name, length) + "." + extension;
    return !key(g.disk, directory, g.courseId, Bytes(BaseBytes)).empty();
}

std::filesystem::path sidecar(const char* container) {
    if (!container || !*container) {
        return {};
    }
    auto path = std::filesystem::u8path(container);
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".gdxc") {
        return {};
    }
    path.replace_extension(".gcs");
    return path;
}

bool readPackage(const char* container, unsigned int crc, std::vector<Generation>& members) {
    const auto path = sidecar(container);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    Bytes bytes;
    if (path.empty() || ec || size > 20 + 6 * (MaxBytes + 4) ||
        !gdx::recovery::readFile(path, bytes) || !packageDecode(bytes, crc, members)) {
        return false;
    }
    for (const auto& g : members) {
        if (!validateState(g)) {
            return false;
        }
    }
    return true;
}

const Generation* member(const std::vector<Generation>& members, const char* name, const char* extension,
                          const void* base) {
    if (!name || !extension || !base) {
        return nullptr;
    }
    const std::string id = std::string(name) + "." + extension;
    for (const auto& g : members) {
        if (g.courseId == id && !std::memcmp(g.base.data(), base, BaseBytes)) {
            return &g;
        }
    }
    return nullptr;
}

template<class F> int repository(F fn) {
    try {
        char path[1600]{};
        if (gdx_course_boot() < 0 || !gdx_course_state_recovery_ready() ||
            !gdx_disk_save_recovery_path(path, sizeof(path))) {
            return 0;
        }
        const auto root = std::filesystem::absolute(
            std::filesystem::u8path(std::string(path) + ".snapshots")).lexically_normal();
        gdx::recovery::FileLock lock(root.string() + ".lock");
        if (!root.is_absolute() || !lock.held()) {
            return 0;
        }
        Files files(root);
        return fn(files) ? 1 : 0;
    } catch (...) {
        gdx_port_logf("[snapshot] Complete generation operation refused after a host I/O failure.\n");
        return 0;
    }
}
}

extern "C" int gdx_snapshot_begin(int writing) {
    void* thread = gdx_snapshot_native_thread();
    if (!thread || (depth && owner != thread) || (writing && gdx_recovery_blocks_writes())) {
        return 0;
    }
    owner = thread;
    ++depth;
    return 1;
}

extern "C" void gdx_snapshot_end(void) {
    if (depth && owner == gdx_snapshot_native_thread() && !--depth) {
        owner = nullptr;
    }
}

extern "C" int gdx_snapshot_busy(void) {
    return depth != 0;
}

extern "C" int gdx_snapshot_blocks_writes(void) {
    return depth && owner != gdx_snapshot_native_thread();
}

extern "C" int gdx_snapshot_load(unsigned short directory, const char* name, const char* extension,
                                  const void* base, void* runtime) {
    if (!base || !runtime) {
        return 0;
    }
    return repository([&](Backend& files) {
        Generation g;
        return identity(g, directory, name, extension) &&
            loadState(files, g, Bytes(static_cast<const unsigned char*>(base),
                                     static_cast<const unsigned char*>(base) + BaseBytes), runtime) == Result::Ok;
    });
}

extern "C" int gdx_snapshot_prepare(unsigned short directory, const char* name, const char* extension,
                                     const void* runtime, const void* previous, void* base) {
    if (!runtime || !base) {
        return 0;
    }
    return repository([&](Backend& files) {
        Generation g;
        Bytes old;
        if (previous) {
            const auto* p = static_cast<const unsigned char*>(previous);
            old.assign(p, p + BaseBytes);
        }
        const auto status = identity(g, directory, name, extension)
            ? prepareSave(files, g, runtime, old) : Result::Invalid;
        if (status != Result::Ok) {
            const char* reason = "invalid course or snapshot data";
            switch (status) {
                case Result::Missing:
                    reason = "previous generation missing; verified load/backup required";
                    break;
                case Result::Conflict:
                    reason = "immutable snapshot key conflict; retained history was not overwritten";
                    break;
                case Result::Io:
                    reason = "snapshot storage read/write failed";
                    break;
                default:
                    break;
            }
            gdx_port_logf("[snapshot] %s.%s: save refused: %s.\n", name, extension, reason);
            return false;
        }
        std::memcpy(base, g.base.data(), BaseBytes);
        return true;
    });
}

extern "C" int gdx_snapshot_preview(unsigned short directory, const char* name, const void* base,
                                    float* x, float* z, int capacity, GdxBranchSet* forks) {
    if (!base || !x || !z || !forks || capacity < 0) {
        return 0;
    }
    int count = 0;
    repository([&](Backend& files) {
        Generation g;
        const auto* p = static_cast<const unsigned char*>(base);
        if (!identity(g, directory, name, "CRSD") ||
            readState(files, g, Bytes(p, p + BaseBytes)) != Result::Ok || g.pointCount > capacity) {
            return false;
        }
        GdxBranchSet stored{};
        const auto& bytes = g.metadata[size_t(Metadata::Forks)];
        const auto binding = gdx_course_registry_crc32(
            reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
        if (!bytes.empty() && gdx_branch_decode(bytes.data(), bytes.size(), binding, &stored) != 0) {
            return false;
        }
        for (int i = 0; i < g.pointCount; ++i) {
            if (i < 64) {
                std::memcpy(x + i, p + 0x20 + size_t(i) * 0x14, sizeof(float));
                std::memcpy(z + i, p + 0x28 + size_t(i) * 0x14, sizeof(float));
            } else {
                x[i] = stored.extended[i - 64].pos[0];
                z[i] = stored.extended[i - 64].pos[2];
            }
        }
        *forks = stored;
        count = g.pointCount;
        return true;
    });
    return count;
}

extern "C" int gdx_snapshot_validate(unsigned short directory, const char* name, const char* extension,
                                      const void* base) {
    if (!base) {
        return 0;
    }
    return repository([&](Backend& files) {
        Generation g;
        const auto* p = static_cast<const unsigned char*>(base);
        return identity(g, directory, name, extension) && readState(files, g, Bytes(p, p + BaseBytes)) == Result::Ok;
    });
}

extern "C" int gdx_snapshot_alias(unsigned short directory, const char* name, const char* extension,
                                   const void* base, unsigned short targetDirectory, const char* targetName,
                                   const char* targetExtension) {
    if (!base) {
        return 0;
    }
    return repository([&](Backend& files) {
        Generation g, target;
        const auto* p = static_cast<const unsigned char*>(base);
        return identity(g, directory, name, extension) &&
            identity(target, targetDirectory, targetName, targetExtension) &&
            readState(files, g, Bytes(p, p + BaseBytes)) == Result::Ok &&
            rebind(g, target.disk, target.directory, target.courseId) &&
            preflightState(g) && publish(files, g) == Result::Ok;
    });
}

extern "C" void gdx_snapshot_export_begin(void) {
    exported.clear();
}

extern "C" int gdx_snapshot_export_member(unsigned short directory, const char* name, const char* extension,
                                           void* base, int stripGhosts) {
    if (!base || exported.size() >= 6) {
        return 0;
    }
    return repository([&](Backend& files) {
        Generation g;
        const auto* p = static_cast<const unsigned char*>(base);
        if (!identity(g, directory, name, extension) || readState(files, g, Bytes(p, p + BaseBytes)) != Result::Ok ||
            !portable(g, stripGhosts != 0)) {
            return false;
        }
        for (const auto& prior : exported) {
            if (prior.courseId == g.courseId) {
                return false;
            }
        }
        std::memcpy(base, g.base.data(), BaseBytes);
        exported.push_back(std::move(g));
        return true;
    });
}

extern "C" int gdx_snapshot_export_write(const char* containerPath, unsigned int containerCrc) {
    try {
        Bytes bytes;
        const auto path = sidecar(containerPath);
        if (path.empty() || !packageEncode(exported, containerCrc, bytes) || !gdx::recovery::writeFile(path, bytes)) {
            return 0;
        }
        const auto directory = path.parent_path().string();
        for (const auto& g : exported) {
            const auto& metadata = g.metadata[size_t(Metadata::Extensions)];
            if (metadata.empty()) {
                continue;
            }
            GdxCourseExtBlock block{};
            const auto binding = gdx_course_registry_crc32(
                reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
            if (gdx_course_ext_decode(metadata.data(), metadata.size(), binding, &block) != 0) {
                return 0;
            }
            for (int i = 0; i < block.count; ++i) {
                if (block.records[i].kind == GDX_COURSE_KIND_BGM) {
                    gdx_course_music_export_key(block.records[i].key, directory.c_str());
                }
            }
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int gdx_snapshot_package_member(const char* containerPath, unsigned int containerCrc,
                                            const char* name, const char* extension, const void* base) {
    try {
        std::vector<Generation> members;
        return readPackage(containerPath, containerCrc, members) && member(members, name, extension, base);
    } catch (...) {
        return 0;
    }
}

extern "C" int gdx_snapshot_import_stage(const char* containerPath, unsigned int containerCrc) {
    imported.clear();
    prepared.clear();
    try {
        std::vector<Generation> members;
        if (!readPackage(containerPath, containerCrc, members)) {
            return 0;
        }
        auto adopted = members;
        for (auto& g : adopted) {
            auto& bytes = g.metadata[size_t(Metadata::Extensions)];
            if (bytes.empty()) {
                continue;
            }
            const auto binding = gdx_course_registry_crc32(
                reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
            GdxCourseExtBlock block{};
            if (gdx_course_ext_decode(bytes.data(), bytes.size(), binding, &block) != 0) {
                return 0;
            }
            for (int i = 0; i < block.count; ++i) {
                auto& rec = block.records[i];
                if (rec.kind == GDX_COURSE_KIND_BGM) {
                    char key[GDX_COURSE_KEY_MAX + 1]{};
                    const int status = gdx_course_music_adopt(containerPath, rec.key, key, sizeof(key));
                    if (status < 0) {
                        return 0;
                    }
                    if (status == 1) {
                        std::strcpy(rec.key, key);
                    }
                }
            }
            bytes.resize(GDX_COURSE_EXT_MAX_BYTES);
            const int length = gdx_course_ext_encode(block.records, block.count, binding, bytes.data(), bytes.size());
            if (length <= 0) {
                return 0;
            }
            bytes.resize(size_t(length));
            if (!seal(g)) {
                return 0;
            }
        }
        prepared = std::move(adopted);
        imported = std::move(members);
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int gdx_snapshot_import_publish(unsigned short directory, const char* name, const char* extension,
                                            void* base) {
    const auto* source = member(imported, name, extension, base);
    if (!source) {
        return 0;
    }
    return repository([&](Backend& files) {
        Generation g = prepared[size_t(source - imported.data())], target;
        if (!identity(target, directory, name, extension) || !rebind(g, target.disk, directory, target.courseId) ||
            !preflightState(g) || publish(files, g) != Result::Ok) {
            return false;
        }
        std::memcpy(base, g.base.data(), BaseBytes);
        return true;
    });
}

extern "C" int gdx_snapshot_import_legacy(unsigned short directory, const char* name, const char* extension,
                                           void* base, const void* records, int count, const char* title,
                                           const void* forks, const void* settings) {
    if (!base || (static_cast<const unsigned char*>(base)[8] & 0x80)) {
        return 0;
    }
    return repository([&](Backend& files) {
        Generation g;
        if (!identity(g, directory, name, extension)) {
            return false;
        }
        const auto* p = static_cast<const unsigned char*>(base);
        g.base.assign(p, p + BaseBytes);
        g.pointCount = p[1];
        std::memcpy(&g.nativeChecksum, p + 4, 4);
        const auto binding = gdx_course_registry_crc32(
            reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
        auto block = [&](Metadata kind, auto encode) {
            auto& bytes = g.metadata[size_t(kind)];
            bytes.resize(65536);
            const int length = encode(bytes);
            if (length <= 0) {
                return false;
            }
            bytes.resize(size_t(length));
            return true;
        };
        if (count && !block(Metadata::Extensions, [&](Bytes& b) {
                return gdx_course_ext_encode(static_cast<const GdxCourseExtRecord*>(records), count,
                                              binding, b.data(), b.size());
            })) {
            return false;
        }
        if (title && *title && !block(Metadata::Title, [&](Bytes& b) {
                return gdx_course_title_encode(title, binding, b.data(), b.size());
            })) {
            return false;
        }
        GdxBranchSet branches{};
        if (forks) {
            branches = *static_cast<const GdxBranchSet*>(forks);
            if (branches.ringCount != g.pointCount || branches.extendedCount) {
                return false;
            }
        }
        branches.ringCount = g.pointCount;
        branches.courseChecksum = g.nativeChecksum;
        branches.baseCrc = gdx_course_registry_crc32(p, CourseBytes);
        if (!block(Metadata::Forks, [&](Bytes& b) {
                return gdx_branch_encode(&branches, binding, b.data(), b.size());
            }) || (settings && !block(Metadata::Settings, [&](Bytes& b) {
                return gdx_settings_encode(static_cast<const GdxCourseSettings*>(settings),
                                             binding, b.data(), b.size());
            })) || !seal(g) || !preflightState(g) || publish(files, g) != Result::Ok) {
            return false;
        }
        std::memcpy(base, g.base.data(), BaseBytes);
        return true;
    });
}

extern "C" int gdx_snapshot_import_restore(unsigned short directory, const char* name, const char* extension,
                                            const void* base) {
    Bytes runtime(gdx_course_snapshot_runtime_bytes());
    return gdx_snapshot_load(directory, name, extension, base, runtime.data());
}

int gdx_snapshot_import_check(unsigned short directory, const char* const* names,
                               const void* const* bases, int count) {
    if (!names || !bases || count < 1 || count > 6) {
        return 0;
    }
    return repository([&](Backend& files) {
        std::vector<Generation> generations;
        generations.reserve(size_t(count));
        for (int i = 0; i < count; ++i) {
            Generation g;
            if (!bases[i] || !identity(g, directory, names[i], "CRSD") ||
                readState(files, g, Bytes(static_cast<const unsigned char*>(bases[i]),
                                         static_cast<const unsigned char*>(bases[i]) + BaseBytes)) != Result::Ok) {
                return false;
            }
            generations.push_back(std::move(g));
        }
        return preflightStates(generations);
    });
}
