// Copyright (c) Zorkats
//
// Companion worker protocol, version 1. JSON Lines in both directions over the worker's stdin and
// stdout. Requests carry "op", events carry "event". Every line carries "v": 1.
//
//   request  {"v":1,"op":"hello"}
//   event    {"v":1,"event":"hello","worker_version":"0.1.0","protocol":1,"ops":["hello","shutdown"]}
//   event    {"v":1,"event":"shutdown"}
//   event    {"v":1,"event":"error","code":"unknown_op","message":"..."}
//
// Jobs (C1, gdx_companion_jobs.h builds the requests): check, plan, build, init, names, inspect and
// unpack carry a "job" id, and every event of a job names it.
//
//   event    {"v":1,"event":"log","job":"j1","stream":"out","text":"..."}
//   event    {"v":1,"event":"result","job":"j1","op":"build","status":"ok","artifacts":[...],...}
//   event    {"v":1,"event":"result","job":"j2","op":"init","status":"ok","project":"C:/..."}
//   event    {"v":1,"event":"result","job":"j3","op":"names","status":"ok","names":["..."]}
//   event    {"v":1,"event":"result","job":"j4","op":"inspect","status":"ok","format":"o2r","title":"...",
//             "summary":["3 x Texture"],"entries":["path\tkind\tdetail"],"total":3}
//   event    {"v":1,"event":"result","job":"j5","op":"unpack",...,"folder":"C:/...","written":3,"failed":0}
//   event    {"v":1,"event":"job_error","job":"j1","code":"exists","message":"..."}
//
// This header has no SDL or ImGui dependency, so companion_protocol_tests links it alone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "gdx_companion_log.h"

namespace gdx_companion {

constexpr int64_t kProtocolVersion = 1;
constexpr size_t kMaxLineBytes = 64 * 1024;  // per line, the LF terminator not counted

// Splits a byte stream into LF-terminated lines. A CR before the LF is removed. A line longer than
// kMaxLineBytes is refused, logged once, and its bytes are discarded up to the next LF.
class FrameReader {
public:
    FrameReader(BoundedLog& log, std::string source);

    void Feed(const char* data, size_t size);

    // Pops the oldest complete line. Returns false when none is ready.
    bool Next(std::string& line);

private:
    BoundedLog& log_;
    std::string source_;
    std::string pending_;
    std::deque<std::string> lines_;
    bool discarding_ = false;
};

enum class EventType { Hello, Shutdown, Error, Log, Result, JobError, Unknown };

struct Artifact {
    std::string path;  // UTF-8, absolute
    int64_t bytes = 0;
    std::string sha256;
};

struct Event {
    EventType type = EventType::Unknown;
    std::string name;  // event name as received
    std::string worker_version;
    std::vector<std::string> ops;
    std::string code;
    std::string message;
    // Job events.
    std::string job;
    std::string stream;  // log: "out" or "err"
    std::string text;    // log line
    std::string op;      // result: check, plan, build, init, names, inspect or unpack
    std::string status;  // result: "ok", or "problems" for a check that found some
    bool has_exit = false;
    int64_t exit_code = 0;
    std::vector<Artifact> artifacts;  // build
    bool replaced = false;            // build: an older pack was moved to previous
    std::string previous;             // build and plan: the .previous path, or empty
    bool exists = false;              // plan
    bool needs_overwrite = false;     // plan
    std::string project;              // init: the new project folder, UTF-8 absolute
    std::vector<std::string> names;   // names: what init can start from, at most kMaxNames
    // inspect and unpack: a pack someone else made. Text fields come from its workshop.json.
    std::string format;                 // "o2r" or "gdxp"
    std::string title;
    std::string author;
    std::string pack_version;           // "version" on the wire
    std::string pack_id;
    std::string description;
    std::vector<std::string> summary;   // one line each, at most kMaxSummary
    std::vector<std::string> entries;   // "path\tkind\tdetail", the first kMaxEntries of total
    int64_t total = 0;                  // every entry in the pack, listed or not
    std::string folder;                 // unpack: the new folder, UTF-8 absolute
    int64_t written = 0;                // unpack: entries written
    int64_t failed = 0;                 // unpack: entries skipped or unreadable
};

constexpr size_t kMaxNames = 256;
constexpr size_t kMaxSummary = 64;
constexpr size_t kMaxEntries = 1000;

enum class ParseStatus { Ok, Malformed, VersionMismatch, UnknownEvent };

ParseStatus ParseEventLine(std::string_view line, Event& out, std::string& detail);

// The handshake phase the session is in. Decides which known events are expected.
enum class Phase { AwaitingHello, Ready, Stopping, Closed };

// Accept: expected in this phase. Stale: a known event that does not apply now.
enum class Disposition { Accept, Stale, Refuse };

Disposition ClassifyEvent(Phase phase, EventType type);

enum class Received { Accepted, Refused, VersionMismatch };

// Parses one event line, classifies it for the phase, and logs every refusal. On Accepted, out
// holds the event. On VersionMismatch the caller should fail the worker.
Received ReceiveEventLine(Phase phase, std::string_view line, Event& out, BoundedLog& log);

// Encoders. Each result ends with LF.
std::string EncodeRequest(std::string_view op);
std::string EncodeEvent(const Event& event);

}  // namespace gdx_companion
