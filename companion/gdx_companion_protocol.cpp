// Copyright (c) Zorkats

#include "gdx_companion_protocol.h"

#include <cstring>
#include <utility>

#include "gdx_companion_json.h"

namespace gdx_companion {

FrameReader::FrameReader(BoundedLog& log, std::string source)
    : log_(log), source_(std::move(source)) {}

void FrameReader::Feed(const char* data, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        const void* found = std::memchr(data + pos, '\n', size - pos);
        const bool has_lf = found != nullptr;
        const size_t end = has_lf ? static_cast<size_t>(static_cast<const char*>(found) - data) : size;
        const size_t take = end - pos;

        if (discarding_) {
            // Still inside an oversized line: drop bytes until its LF arrives.
            if (has_lf) {
                discarding_ = false;
                pos = end + 1;
            } else {
                pos = size;
            }
            continue;
        }

        if (pending_.size() + take > kMaxLineBytes) {
            log_.Append(LogLevel::Error,
                        source_ + " line refused: longer than " + std::to_string(kMaxLineBytes) +
                            " bytes, discarded");
            pending_.clear();
            if (has_lf) {
                pos = end + 1;
            } else {
                discarding_ = true;
                pos = size;
            }
            continue;
        }

        pending_.append(data + pos, take);
        if (!has_lf) {
            pos = size;
            break;
        }
        if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
        lines_.push_back(std::move(pending_));
        pending_.clear();
        pos = end + 1;
    }
}

bool FrameReader::Next(std::string& line) {
    if (lines_.empty()) return false;
    line = std::move(lines_.front());
    lines_.pop_front();
    return true;
}

namespace {

bool IsJobId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_';
        if (!ok) return false;
    }
    return true;
}

// Reads an optional string field. A present field of another kind is an error; null counts as absent.
bool OptionalString(const JsonValue& root, const char* key, std::string& out, std::string& detail) {
    const JsonValue* value = root.Find(key);
    if (value == nullptr || value->kind() == JsonValue::Kind::Null) return true;
    if (value->kind() != JsonValue::Kind::String) {
        detail = std::string("'") + key + "' is not a string";
        return false;
    }
    out = value->AsString();
    return true;
}

// Reads an optional list of non-empty strings of at most limit items.
bool OptionalStringList(const JsonValue& root, const char* key, size_t limit, std::vector<std::string>& out,
                        std::string& detail) {
    const JsonValue* value = root.Find(key);
    if (value == nullptr) return true;
    if (value->kind() != JsonValue::Kind::Array || value->AsArray().size() > limit) {
        detail = std::string("result '") + key + "' must be a list of at most " + std::to_string(limit) + " strings";
        return false;
    }
    for (const JsonValue& item : value->AsArray()) {
        if (item.kind() != JsonValue::Kind::String || item.AsString().empty()) {
            detail = std::string("result '") + key + "' holds an empty or non-string entry";
            return false;
        }
        out.push_back(item.AsString());
    }
    return true;
}

// Reads an optional count: an integer of zero or more.
bool OptionalCount(const JsonValue& root, const char* key, int64_t& out, std::string& detail) {
    const JsonValue* value = root.Find(key);
    if (value == nullptr) return true;
    if (!value->AsInt64(out) || out < 0) {
        detail = std::string("'") + key + "' is not a count";
        return false;
    }
    return true;
}

bool OptionalBool(const JsonValue& root, const char* key, bool& out, std::string& detail) {
    const JsonValue* value = root.Find(key);
    if (value == nullptr) return true;
    if (value->kind() != JsonValue::Kind::Bool) {
        detail = std::string("'") + key + "' is not true or false";
        return false;
    }
    out = value->AsBool();
    return true;
}

bool OptionalExit(const JsonValue& root, Event& out, std::string& detail) {
    const JsonValue* value = root.Find("exit");
    if (value == nullptr) return true;
    if (!value->AsInt64(out.exit_code)) {
        detail = "'exit' is not an integer";
        return false;
    }
    out.has_exit = true;
    return true;
}

bool IsSha256(const std::string& text) {
    if (text.size() != 64) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

ParseStatus ParseJobEvent(const JsonValue& root, Event& out, std::string& detail) {
    const JsonValue* job = root.Find("job");
    if (job == nullptr || job->kind() != JsonValue::Kind::String) {
        detail = out.name + " event without string 'job'";
        return ParseStatus::Malformed;
    }
    out.job = job->AsString();
    // A refused request whose own job id was unusable is answered with an empty id.
    if (!IsJobId(out.job) && !(out.name == "job_error" && out.job.empty())) {
        detail = out.name + " event with an invalid job id";
        return ParseStatus::Malformed;
    }

    if (out.name == "log") {
        out.type = EventType::Log;
        if (!OptionalString(root, "stream", out.stream, detail) || !OptionalString(root, "text", out.text, detail)) {
            return ParseStatus::Malformed;
        }
        if (out.stream != "out" && out.stream != "err") {
            detail = "log event 'stream' must be out or err";
            return ParseStatus::Malformed;
        }
        return ParseStatus::Ok;
    }

    if (out.name == "job_error") {
        out.type = EventType::JobError;
        if (!OptionalString(root, "code", out.code, detail) || !OptionalString(root, "message", out.message, detail) ||
            !OptionalExit(root, out, detail)) {
            return ParseStatus::Malformed;
        }
        if (out.code.empty()) {
            detail = "job_error event without 'code'";
            return ParseStatus::Malformed;
        }
        return ParseStatus::Ok;
    }

    out.type = EventType::Result;
    if (!OptionalString(root, "op", out.op, detail) || !OptionalString(root, "status", out.status, detail) ||
        !OptionalString(root, "previous", out.previous, detail) || !OptionalExit(root, out, detail) ||
        !OptionalBool(root, "replaced", out.replaced, detail) || !OptionalBool(root, "exists", out.exists, detail) ||
        !OptionalBool(root, "needs_overwrite", out.needs_overwrite, detail) ||
        !OptionalString(root, "project", out.project, detail) ||
        !OptionalString(root, "format", out.format, detail) || !OptionalString(root, "title", out.title, detail) ||
        !OptionalString(root, "author", out.author, detail) ||
        !OptionalString(root, "version", out.pack_version, detail) ||
        !OptionalString(root, "pack_id", out.pack_id, detail) ||
        !OptionalString(root, "description", out.description, detail) ||
        !OptionalString(root, "folder", out.folder, detail) ||
        !OptionalStringList(root, "summary", kMaxSummary, out.summary, detail) ||
        !OptionalStringList(root, "entries", kMaxEntries, out.entries, detail) ||
        !OptionalCount(root, "total", out.total, detail) || !OptionalCount(root, "written", out.written, detail) ||
        !OptionalCount(root, "failed", out.failed, detail)) {
        return ParseStatus::Malformed;
    }
    if (out.op != "check" && out.op != "plan" && out.op != "build" && out.op != "init" && out.op != "names" &&
        out.op != "inspect" && out.op != "unpack") {
        detail = "result event 'op' must be check, plan, build, init, names, inspect or unpack";
        return ParseStatus::Malformed;
    }
    if (out.op == "init" && out.status == "ok" && out.project.empty()) {
        detail = "a finished init names no project";
        return ParseStatus::Malformed;
    }
    if (out.op == "unpack" && out.status == "ok" && out.folder.empty()) {
        detail = "a finished unpack names no folder";
        return ParseStatus::Malformed;
    }
    if (static_cast<int64_t>(out.entries.size()) > out.total) {
        detail = "result lists more entries than its 'total'";
        return ParseStatus::Malformed;
    }
    const JsonValue* names = root.Find("names");
    if (names != nullptr) {
        if (names->kind() != JsonValue::Kind::Array || names->AsArray().size() > kMaxNames) {
            detail = "result 'names' must be a list of at most " + std::to_string(kMaxNames) + " strings";
            return ParseStatus::Malformed;
        }
        for (const JsonValue& item : names->AsArray()) {
            if (item.kind() != JsonValue::Kind::String || item.AsString().empty()) {
                detail = "result 'names' holds an empty or non-string entry";
                return ParseStatus::Malformed;
            }
            out.names.push_back(item.AsString());
        }
    }
    if (out.status != "ok" && out.status != "problems") {
        detail = "result event 'status' must be ok or problems";
        return ParseStatus::Malformed;
    }
    const JsonValue* artifacts = root.Find("artifacts");
    if (artifacts != nullptr) {
        if (artifacts->kind() != JsonValue::Kind::Array || artifacts->AsArray().size() > 16) {
            detail = "result 'artifacts' must be a list of at most 16 files";
            return ParseStatus::Malformed;
        }
        for (const JsonValue& item : artifacts->AsArray()) {
            Artifact artifact;
            const JsonValue* path = item.Find("path");
            const JsonValue* bytes = item.Find("bytes");
            const JsonValue* sha = item.Find("sha256");
            if (path == nullptr || path->kind() != JsonValue::Kind::String || path->AsString().empty() ||
                bytes == nullptr || !bytes->AsInt64(artifact.bytes) || artifact.bytes < 0 || sha == nullptr ||
                sha->kind() != JsonValue::Kind::String || !IsSha256(sha->AsString())) {
                detail = "result artifact needs a path, a byte count and a sha256";
                return ParseStatus::Malformed;
            }
            artifact.path = path->AsString();
            artifact.sha256 = sha->AsString();
            out.artifacts.push_back(std::move(artifact));
        }
    }
    if (out.op == "build" && out.status == "ok" && out.artifacts.empty()) {
        detail = "a finished build names no artifact";
        return ParseStatus::Malformed;
    }
    return ParseStatus::Ok;
}

}  // namespace

ParseStatus ParseEventLine(std::string_view line, Event& out, std::string& detail) {
    out = Event{};
    JsonValue root;
    std::string parse_error;
    if (!JsonValue::Parse(line, root, parse_error)) {
        detail = parse_error;
        return ParseStatus::Malformed;
    }
    if (root.kind() != JsonValue::Kind::Object) {
        detail = "event is not a JSON object";
        return ParseStatus::Malformed;
    }

    // The version is checked before anything else, so a newer worker is reported as a mismatch
    // even when its event names are unknown to us.
    int64_t version = 0;
    const JsonValue* version_value = root.Find("v");
    if (version_value == nullptr || !version_value->AsInt64(version)) {
        detail = "missing integer protocol version 'v'";
        return ParseStatus::Malformed;
    }
    if (version != kProtocolVersion) {
        detail = "worker speaks protocol " + std::to_string(version) + ", Companion speaks protocol " +
                 std::to_string(kProtocolVersion);
        return ParseStatus::VersionMismatch;
    }

    const JsonValue* event_value = root.Find("event");
    if (event_value == nullptr || event_value->kind() != JsonValue::Kind::String) {
        detail = "missing string 'event'";
        return ParseStatus::Malformed;
    }
    out.name = event_value->AsString();

    if (out.name == "hello") {
        out.type = EventType::Hello;
        const JsonValue* worker_version = root.Find("worker_version");
        const JsonValue* protocol = root.Find("protocol");
        const JsonValue* ops = root.Find("ops");
        if (worker_version == nullptr || worker_version->kind() != JsonValue::Kind::String) {
            detail = "hello without string 'worker_version'";
            return ParseStatus::Malformed;
        }
        int64_t hello_protocol = 0;
        if (protocol == nullptr || !protocol->AsInt64(hello_protocol)) {
            detail = "hello without integer 'protocol'";
            return ParseStatus::Malformed;
        }
        if (ops == nullptr || ops->kind() != JsonValue::Kind::Array) {
            detail = "hello without array 'ops'";
            return ParseStatus::Malformed;
        }
        if (hello_protocol != kProtocolVersion) {
            detail = "worker handshake offers protocol " + std::to_string(hello_protocol) +
                     ", Companion speaks protocol " + std::to_string(kProtocolVersion);
            return ParseStatus::VersionMismatch;
        }
        out.worker_version = worker_version->AsString();
        for (const JsonValue& item : ops->AsArray()) {
            if (item.kind() != JsonValue::Kind::String) {
                detail = "hello 'ops' holds a non-string entry";
                return ParseStatus::Malformed;
            }
            out.ops.push_back(item.AsString());
        }
        return ParseStatus::Ok;
    }

    if (out.name == "shutdown") {
        out.type = EventType::Shutdown;
        return ParseStatus::Ok;
    }

    if (out.name == "error") {
        out.type = EventType::Error;
        const JsonValue* code = root.Find("code");
        const JsonValue* message = root.Find("message");
        if (code == nullptr || code->kind() != JsonValue::Kind::String) {
            detail = "error event without string 'code'";
            return ParseStatus::Malformed;
        }
        out.code = code->AsString();
        if (message != nullptr) {
            if (message->kind() != JsonValue::Kind::String) {
                detail = "error event 'message' is not a string";
                return ParseStatus::Malformed;
            }
            out.message = message->AsString();
        }
        return ParseStatus::Ok;
    }

    if (out.name == "log" || out.name == "result" || out.name == "job_error") {
        return ParseJobEvent(root, out, detail);
    }

    out.type = EventType::Unknown;
    detail = "unknown event '" + out.name + "'";
    return ParseStatus::UnknownEvent;
}

Disposition ClassifyEvent(Phase phase, EventType type) {
    if (type == EventType::Unknown) return Disposition::Refuse;
    switch (phase) {
    case Phase::AwaitingHello:
        if (type == EventType::Hello || type == EventType::Error) return Disposition::Accept;
        return Disposition::Stale;
    case Phase::Ready:
        if (type == EventType::Error || type == EventType::Log || type == EventType::Result ||
            type == EventType::JobError) {
            return Disposition::Accept;  // the session then checks the job id
        }
        return Disposition::Stale;
    case Phase::Stopping:
        if (type == EventType::Shutdown || type == EventType::Error) return Disposition::Accept;
        return Disposition::Stale;
    case Phase::Closed:
        return Disposition::Stale;
    }
    return Disposition::Refuse;
}

Received ReceiveEventLine(Phase phase, std::string_view line, Event& out, BoundedLog& log) {
    std::string detail;
    switch (ParseEventLine(line, out, detail)) {
    case ParseStatus::Malformed:
        log.Append(LogLevel::Error, "worker event refused: " + detail);
        return Received::Refused;
    case ParseStatus::VersionMismatch:
        log.Append(LogLevel::Error, "worker event refused: " + detail);
        return Received::VersionMismatch;
    case ParseStatus::UnknownEvent:
        log.Append(LogLevel::Warning, "worker event refused: " + detail);
        return Received::Refused;
    case ParseStatus::Ok:
        break;
    }

    switch (ClassifyEvent(phase, out.type)) {
    case Disposition::Accept:
        break;
    case Disposition::Stale:
        log.Append(LogLevel::Warning,
                   "stale worker event '" + out.name + "' ignored in this handshake state");
        return Received::Refused;
    case Disposition::Refuse:
        log.Append(LogLevel::Warning, "worker event refused: '" + out.name + "'");
        return Received::Refused;
    }

    if (out.type == EventType::Error) {
        log.Append(LogLevel::Warning, "worker reported error " + out.code +
                                          (out.message.empty() ? std::string() : ": " + out.message));
    }
    return Received::Accepted;
}

std::string EncodeRequest(std::string_view op) {
    return "{\"v\":" + std::to_string(kProtocolVersion) + ",\"op\":\"" + JsonEscape(op) + "\"}\n";
}

std::string EncodeEvent(const Event& event) {
    const std::string version = std::to_string(kProtocolVersion);
    switch (event.type) {
    case EventType::Hello: {
        std::string ops;
        for (size_t i = 0; i < event.ops.size(); ++i) {
            if (i > 0) ops += ',';
            ops += "\"" + JsonEscape(event.ops[i]) + "\"";
        }
        return "{\"v\":" + version + ",\"event\":\"hello\",\"worker_version\":\"" +
               JsonEscape(event.worker_version) + "\",\"protocol\":" + version + ",\"ops\":[" + ops +
               "]}\n";
    }
    case EventType::Shutdown:
        return "{\"v\":" + version + ",\"event\":\"shutdown\"}\n";
    case EventType::Error:
        return "{\"v\":" + version + ",\"event\":\"error\",\"code\":\"" + JsonEscape(event.code) +
               "\",\"message\":\"" + JsonEscape(event.message) + "\"}\n";
    case EventType::Log:
        return "{\"v\":" + version + ",\"event\":\"log\",\"job\":\"" + JsonEscape(event.job) + "\",\"stream\":\"" +
               JsonEscape(event.stream) + "\",\"text\":\"" + JsonEscape(event.text) + "\"}\n";
    case EventType::JobError: {
        std::string line = "{\"v\":" + version + ",\"event\":\"job_error\",\"job\":\"" + JsonEscape(event.job) +
                           "\",\"code\":\"" + JsonEscape(event.code) + "\",\"message\":\"" +
                           JsonEscape(event.message) + "\"";
        if (event.has_exit) line += ",\"exit\":" + std::to_string(event.exit_code);
        return line + "}\n";
    }
    case EventType::Result: {
        std::string line = "{\"v\":" + version + ",\"event\":\"result\",\"job\":\"" + JsonEscape(event.job) +
                           "\",\"op\":\"" + JsonEscape(event.op) + "\",\"status\":\"" + JsonEscape(event.status) +
                           "\"";
        if (event.has_exit) line += ",\"exit\":" + std::to_string(event.exit_code);
        if (!event.artifacts.empty()) {
            line += ",\"artifacts\":[";
            for (size_t i = 0; i < event.artifacts.size(); ++i) {
                const Artifact& a = event.artifacts[i];
                if (i > 0) line += ',';
                line += "{\"path\":\"" + JsonEscape(a.path) + "\",\"bytes\":" + std::to_string(a.bytes) +
                        ",\"sha256\":\"" + JsonEscape(a.sha256) + "\"}";
            }
            line += ']';
        }
        line += std::string(",\"replaced\":") + (event.replaced ? "true" : "false");
        line += ",\"previous\":" +
                (event.previous.empty() ? std::string("null") : "\"" + JsonEscape(event.previous) + "\"");
        line += std::string(",\"exists\":") + (event.exists ? "true" : "false");
        line += std::string(",\"needs_overwrite\":") + (event.needs_overwrite ? "true" : "false");
        if (!event.project.empty()) line += ",\"project\":\"" + JsonEscape(event.project) + "\"";
        const auto list = [&line](const char* key, const std::vector<std::string>& items) {
            if (items.empty()) return;
            line += std::string(",\"") + key + "\":[";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i > 0) line += ',';
                line += "\"" + JsonEscape(items[i]) + "\"";
            }
            line += ']';
        };
        const auto text = [&line](const char* key, const std::string& value) {
            if (!value.empty()) line += std::string(",\"") + key + "\":\"" + JsonEscape(value) + "\"";
        };
        list("names", event.names);
        text("format", event.format);
        text("title", event.title);
        text("author", event.author);
        text("version", event.pack_version);
        text("pack_id", event.pack_id);
        text("description", event.description);
        list("summary", event.summary);
        list("entries", event.entries);
        if (event.op == "inspect" || event.op == "unpack") line += ",\"total\":" + std::to_string(event.total);
        if (event.op == "unpack") {
            text("folder", event.folder);
            line += ",\"written\":" + std::to_string(event.written) + ",\"failed\":" + std::to_string(event.failed);
        }
        return line + "}\n";
    }
    case EventType::Unknown:
        break;
    }
    return "{\"v\":" + version + ",\"event\":\"" + JsonEscape(event.name) + "\"}\n";
}

}  // namespace gdx_companion
