// Copyright (c) Zorkats

#include "gdx_companion_project.h"

#include <fstream>
#include <iterator>
#include <system_error>

#include "gdx_companion_json.h"
#include "gdx_companion_paths.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

constexpr uintmax_t kMaxProjectBytes = 256 * 1024;

std::string GenericUtf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path BaseOf(const fs::path& file) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    return (ec ? file : absolute).parent_path();
}

template <typename Map, typename Write>
void EncodeObject(std::string& out, const char* name, const Map& map, Write write) {
    out += ",\n  \"";
    out += name;
    out += "\": {";
    bool first = true;
    for (const auto& [key, value] : map) {
        out += first ? "\n    \"" : ",\n    \"";
        first = false;
        out += JsonEscape(key);
        out += "\": ";
        write(out, value);
    }
    out += first ? "}" : "\n  }";
}

}  // namespace

std::string RelativeProjectPath(const fs::path& base, const fs::path& path) {
    if (path.empty()) return std::string();
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec).lexically_normal();
    const fs::path root = fs::absolute(base, ec).lexically_normal();
    if (absolute.root_name() != root.root_name()) return GenericUtf8(absolute);
    const fs::path relative = absolute.lexically_relative(root);
    if (relative.empty()) return GenericUtf8(absolute);
    return GenericUtf8(relative);
}

fs::path ResolveProjectPath(const fs::path& base, const std::string& stored) {
    if (stored.empty()) return fs::path();
    const fs::path path = PathFromUtf8(stored);
    if (path.is_absolute()) return path.lexically_normal();
    return (base / path).lexically_normal();
}

std::string EncodeProject(const CompanionProject& project, const fs::path& file) {
    const fs::path base = BaseOf(file);
    std::string out = "{\n  \"format\": \"";
    out += kProjectFormat;
    out += "\",\n  \"version\": ";
    out += std::to_string(kProjectVersion);
    out += ",\n  \"workspace\": \"" + JsonEscape(project.workspace) + "\"";
    EncodeObject(out, "text", project.text, [](std::string& o, const std::string& value) {
        o += "\"" + JsonEscape(value) + "\"";
    });
    EncodeObject(out, "paths", project.paths, [&base](std::string& o, const fs::path& value) {
        o += "\"" + JsonEscape(RelativeProjectPath(base, value)) + "\"";
    });
    EncodeObject(out, "flags", project.flags, [](std::string& o, bool value) { o += value ? "true" : "false"; });
    out += "\n}\n";
    return out;
}

bool SaveProject(const fs::path& file, const CompanionProject& project, std::string& error) {
    if (file.empty() || file.filename().empty()) {
        error = "choose where to save the project";
        return false;
    }
    std::error_code ec;
    if (fs::is_directory(file, ec)) {
        error = PathToUtf8(file) + " is a folder";
        return false;
    }
    const fs::path parent = BaseOf(file);
    if (!fs::is_directory(parent, ec)) {
        error = "the folder " + PathToUtf8(parent) + " does not exist";
        return false;
    }
    const std::string text = EncodeProject(project, file);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            fs::remove(temp, ec);
            error = "cannot write " + PathToUtf8(temp);
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        error = "cannot replace " + PathToUtf8(file) + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool LoadProject(const fs::path& file, CompanionProject& project, std::vector<std::string>& missing,
                 std::string& error) {
    project = CompanionProject{};
    missing.clear();
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        error = "cannot read " + PathToUtf8(file) + ": " + ec.message();
        return false;
    }
    if (size > kMaxProjectBytes) {
        error = "the project file is larger than 256 KiB";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        error = "cannot read " + PathToUtf8(file);
        return false;
    }
    JsonValue root;
    if (!JsonValue::Parse(text, root, error)) {
        error = "the project file is not valid JSON: " + error;
        return false;
    }
    const JsonValue* format = root.Find("format");
    if (format == nullptr || format->kind() != JsonValue::Kind::String || format->AsString() != kProjectFormat) {
        error = "this is not a Companion project file";
        return false;
    }
    const JsonValue* version = root.Find("version");
    int64_t number = 0;
    if (version == nullptr || !version->AsInt64(number) || number < 1) {
        error = "the project file has no version";
        return false;
    }
    if (number > kProjectVersion) {
        error = "the project was saved by a newer Companion (version " + std::to_string(number) + ")";
        return false;
    }
    const JsonValue* workspace = root.Find("workspace");
    if (workspace == nullptr || workspace->kind() != JsonValue::Kind::String || workspace->AsString().empty()) {
        error = "the project file names no workspace";
        return false;
    }
    project.workspace = workspace->AsString();

    auto object = [&root, &error](const char* key, const JsonValue*& out) -> bool {
        out = root.Find(key);
        if (out == nullptr) return true;
        if (out->kind() != JsonValue::Kind::Object) {
            error = std::string("the project's ") + key + " entry is not an object";
            return false;
        }
        return true;
    };
    const JsonValue* texts = nullptr;
    const JsonValue* paths = nullptr;
    const JsonValue* flags = nullptr;
    if (!object("text", texts) || !object("paths", paths) || !object("flags", flags)) return false;
    if (texts != nullptr) {
        for (size_t i = 0; i < texts->Keys().size(); ++i) {
            const JsonValue& value = texts->AsArray()[i];
            if (value.kind() != JsonValue::Kind::String) {
                error = "the project's text entry " + texts->Keys()[i] + " is not text";
                return false;
            }
            project.text[texts->Keys()[i]] = value.AsString();
        }
    }
    const fs::path base = BaseOf(file);
    if (paths != nullptr) {
        for (size_t i = 0; i < paths->Keys().size(); ++i) {
            const JsonValue& value = paths->AsArray()[i];
            if (value.kind() != JsonValue::Kind::String) {
                error = "the project's path entry " + paths->Keys()[i] + " is not text";
                return false;
            }
            const fs::path resolved = ResolveProjectPath(base, value.AsString());
            project.paths[paths->Keys()[i]] = resolved;
            if (!resolved.empty() && !fs::exists(resolved, ec)) missing.push_back(paths->Keys()[i]);
        }
    }
    if (flags != nullptr) {
        for (size_t i = 0; i < flags->Keys().size(); ++i) {
            const JsonValue& value = flags->AsArray()[i];
            if (value.kind() != JsonValue::Kind::Bool) {
                error = "the project's flag entry " + flags->Keys()[i] + " is not true or false";
                return false;
            }
            project.flags[flags->Keys()[i]] = value.AsBool();
        }
    }
    return true;
}

}  // namespace gdx_companion
