// Copyright (c) Zorkats
//
// Headless tests for the Companion protocol codec, the frame limits, the handshake phase rules
// and the 256 KiB log cap. Links only gdx_companion_codec: no SDL, ImGui or game code.

#include <cstdio>
#include <string>

#include "gdx_companion_json.h"
#include "gdx_companion_log.h"
#include "gdx_companion_protocol.h"

using namespace gdx_companion;

namespace {

int gPassed = 0;
int gFailed = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        ++gPassed;
        std::printf("PASS: %s\n", name);
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name);
    }
}

bool LogContains(const BoundedLog& log, const std::string& needle) {
    for (const LogEntry& entry : log.entries()) {
        if (entry.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

size_t CountLogContaining(const BoundedLog& log, const std::string& needle) {
    size_t count = 0;
    for (const LogEntry& entry : log.entries()) {
        if (entry.text.find(needle) != std::string::npos) ++count;
    }
    return count;
}

// A valid hello line whose length, LF excluded, is exactly `length` bytes.
std::string HelloOfLength(size_t length) {
    const std::string prefix =
        "{\"v\":1,\"event\":\"hello\",\"worker_version\":\"0.1.0\",\"protocol\":1,"
        "\"ops\":[\"hello\",\"shutdown\"],\"pad\":\"";
    const std::string suffix = "\"}";
    return prefix + std::string(length - prefix.size() - suffix.size(), 'x') + suffix;
}

const char kShutdownLine[] = "{\"v\":1,\"event\":\"shutdown\"}\n";

void TestRoundTrip() {
    Event hello;
    hello.type = EventType::Hello;
    hello.worker_version = "0.1.0";
    hello.ops = {"hello", "shutdown"};

    BoundedLog log;
    FrameReader reader(log, "test");
    const std::string wire = EncodeEvent(hello);
    for (char c : wire) reader.Feed(&c, 1);  // one byte at a time, to cover framing across reads

    std::string line;
    Check(reader.Next(line), "round trip: one frame after byte-by-byte feed");
    Check(!reader.Next(line), "round trip: no extra frame");

    Event parsed;
    std::string detail;
    Check(ParseEventLine(line, parsed, detail) == ParseStatus::Ok, "round trip: hello parses");
    Check(parsed.type == EventType::Hello && parsed.worker_version == "0.1.0" &&
              parsed.ops.size() == 2 && parsed.ops[1] == "shutdown",
          "round trip: hello fields survive");

    Event error;
    error.type = EventType::Error;
    error.code = "unknown_op";
    error.message = std::string("quote \" slash \\ tab \t bell \x07 e-acute \xC3\xA9");
    FrameReader error_reader(log, "test");
    const std::string error_wire = EncodeEvent(error);
    error_reader.Feed(error_wire.data(), error_wire.size());
    std::string error_line;
    Check(error_reader.Next(error_line), "round trip: error frame produced");
    Event error_parsed;
    Check(ParseEventLine(error_line, error_parsed, detail) == ParseStatus::Ok &&
              error_parsed.type == EventType::Error && error_parsed.code == "unknown_op" &&
              error_parsed.message == error.message,
          "round trip: escaped error message survives");

    Check(EncodeRequest("hello") == "{\"v\":1,\"op\":\"hello\"}\n", "encode: request line");
    Check(EncodeEvent(Event{EventType::Shutdown, "shutdown", {}, {}, {}, {}}) == kShutdownLine,
          "encode: shutdown event line");
    Check(log.entries().empty(), "round trip: no refusals logged");
}

void TestFrameLimits() {
    BoundedLog log;
    FrameReader reader(log, "worker stdout");
    std::string line;

    const std::string at_limit = HelloOfLength(kMaxLineBytes) + "\n";
    reader.Feed(at_limit.data(), at_limit.size());
    Check(reader.Next(line) && line.size() == kMaxLineBytes,
          "limits: a line of exactly 65536 bytes is accepted");
    Check(log.entries().empty(), "limits: exact-limit line logs nothing");

    const std::string over_limit = HelloOfLength(kMaxLineBytes + 1) + "\n";
    reader.Feed(over_limit.data(), over_limit.size());
    Check(!reader.Next(line), "limits: a line of 65537 bytes is refused");
    Check(LogContains(log, "refused") && LogContains(log, "65536"),
          "limits: refused oversized line leaves a log entry");

    const std::string after(kShutdownLine);
    reader.Feed(after.data(), after.size());
    Check(reader.Next(line) && line == "{\"v\":1,\"event\":\"shutdown\"}",
          "limits: the next line still arrives after a refusal");

    // An oversized line that arrives in pieces is still one refusal.
    BoundedLog split_log;
    FrameReader split_reader(split_log, "worker stdout");
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 80; ++i) split_reader.Feed(chunk.data(), chunk.size());
    split_reader.Feed("\n", 1);
    const std::string tail = std::string(kShutdownLine);
    split_reader.Feed(tail.data(), tail.size());
    Check(CountLogContaining(split_log, "refused") == 1,
          "limits: an oversized line split across reads is logged once");
    Check(split_reader.Next(line) && line == "{\"v\":1,\"event\":\"shutdown\"}",
          "limits: framing resumes after a split oversized line");
}

void TestMalformed() {
    const char* cases[] = {
        "",
        "{",
        "{\"v\":1,\"event\":\"shutdown\",}",
        "[1,2]",
        "\"shutdown\"",
        "{\"v\":1 \"event\":\"shutdown\"}",
        "{\"v\":1,\"v\":1,\"event\":\"shutdown\"}",
        "{\"v\":1,\"event\":\"sh\xFFutdown\"}",
        "{\"v\":1,\"event\":\"a\x01z\"}",
        "{\"v\":NaN,\"event\":\"shutdown\"}",
        "{\"v\":1,\"event\":\"shutdown\"} trailing",
        "{\"v\":1,\"event\":\"\\ud83d\"}",
        "{\"v\":1,\"event\":\"shutdown\",\"x\":[[[[[[[[[[1]]]]]]]]]]}",
        "{\"event\":\"shutdown\"}",
        "{\"v\":1,\"event\":7}",
        "{\"v\":1,\"event\":\"error\"}",
    };
    bool all_refused = true;
    bool none_mismatch = true;
    BoundedLog log;
    for (const char* text : cases) {
        Event event;
        const Received result = ReceiveEventLine(Phase::Ready, text, event, log);
        if (result != Received::Refused) all_refused = false;
        if (result == Received::VersionMismatch) none_mismatch = false;
    }
    Check(all_refused, "malformed: every bad line is refused");
    Check(none_mismatch, "malformed: bad lines are not reported as version mismatches");
    Check(log.entries().size() == sizeof(cases) / sizeof(cases[0]),
          "malformed: each refused line leaves one log entry");
    Check(LogContains(log, "worker event refused: "), "malformed: refusals are logged");
}

void TestJsonValues() {
    JsonValue value;
    std::string error;
    Check(JsonValue::Parse("{\"s\":\"\\u00e9\\ud83d\\ude00\",\"n\":-12.0,\"b\":true}", value, error),
          "json: accepts escapes and a surrogate pair");
    const JsonValue* s = value.Find("s");
    Check(s != nullptr && s->AsString() == "\xC3\xA9\xF0\x9F\x98\x80",
          "json: \\u escapes decode to UTF-8");
    int64_t n = 0;
    const JsonValue* number = value.Find("n");
    Check(number != nullptr && number->AsInt64(n) && n == -12, "json: integral number reads as int64");
    JsonValue fraction;
    Check(JsonValue::Parse("1.5", fraction, error) && !fraction.AsInt64(n),
          "json: fractional number is not an int64");
    Check(JsonValue::Parse("[[[1]]]", value, error), "json: nesting within the limit is accepted");
}

void TestLogCap() {
    BoundedLog log;
    const std::string body(100, 'y');
    for (int i = 0; i < 20000; ++i) {
        log.Append(LogLevel::Info, std::to_string(i) + " " + body);
    }
    Check(log.retained_bytes() <= BoundedLog::kCapBytes,
          "log cap: retained bytes stay within 256 KiB");
    Check(log.dropped_entries() > 0 && log.entries().size() + log.dropped_entries() == 20000,
          "log cap: dropped entries are counted");
    Check(log.entries().back().text.rfind("19999 ", 0) == 0,
          "log cap: the newest entry is kept");
    Check(log.entries().front().text.rfind("0 ", 0) != 0, "log cap: the oldest entry is dropped");

    BoundedLog long_log;
    long_log.Append(LogLevel::Warning, std::string(10000, 'z'));
    Check(long_log.entries().front().text.size() <= BoundedLog::kMaxEntryBytes + 3 &&
              long_log.entries().front().text.size() > 0 &&
              long_log.entries().front().text.compare(
                  long_log.entries().front().text.size() - 3, 3, "...") == 0,
          "log cap: a single long entry is cut at 4 KiB");

    BoundedLog clean_log;
    clean_log.Append(LogLevel::Info, "a\tb");
    Check(clean_log.entries().front().text == "a?b", "log: control characters become '?'");
}

void TestPhaseRules() {
    struct Row {
        Phase phase;
        EventType type;
        Disposition expected;
    };
    const Row rows[] = {
        {Phase::AwaitingHello, EventType::Hello, Disposition::Accept},
        {Phase::AwaitingHello, EventType::Shutdown, Disposition::Stale},
        {Phase::AwaitingHello, EventType::Error, Disposition::Accept},
        {Phase::Ready, EventType::Hello, Disposition::Stale},
        {Phase::Ready, EventType::Shutdown, Disposition::Stale},
        {Phase::Ready, EventType::Error, Disposition::Accept},
        {Phase::Stopping, EventType::Shutdown, Disposition::Accept},
        {Phase::Stopping, EventType::Hello, Disposition::Stale},
        {Phase::Closed, EventType::Hello, Disposition::Stale},
        {Phase::Closed, EventType::Unknown, Disposition::Refuse},
        {Phase::Ready, EventType::Unknown, Disposition::Refuse},
    };
    bool table_ok = true;
    for (const Row& row : rows) {
        if (ClassifyEvent(row.phase, row.type) != row.expected) table_ok = false;
    }
    Check(table_ok, "phase rules: every row of the classification table holds");
}

void TestVersionMismatch() {
    BoundedLog log;
    Event event;
    const char* mismatches[] = {
        "{\"v\":2,\"event\":\"hello\",\"worker_version\":\"9.0\",\"protocol\":2,\"ops\":[]}",
        "{\"v\":1,\"event\":\"hello\",\"worker_version\":\"9.0\",\"protocol\":2,\"ops\":[]}",
        "{\"v\":2,\"event\":\"frobnicate\"}",
        "{\"v\":0,\"event\":\"shutdown\"}",
    };
    bool all_mismatch = true;
    for (const char* text : mismatches) {
        if (ReceiveEventLine(Phase::AwaitingHello, text, event, log) != Received::VersionMismatch) {
            all_mismatch = false;
        }
    }
    Check(all_mismatch, "version: a different protocol number is a mismatch");
    Check(LogContains(log, "protocol"), "version: mismatch is logged with the protocol numbers");

    Event accepted;
    Check(ReceiveEventLine(Phase::AwaitingHello,
                           "{\"v\":1,\"event\":\"hello\",\"worker_version\":\"0.1.0\",\"protocol\":1,"
                           "\"ops\":[\"hello\"]}",
                           accepted, log) == Received::Accepted,
          "version: matching protocol 1 hello is accepted");
}

void TestUnknownAndStale() {
    BoundedLog log;
    Event event;
    Check(ReceiveEventLine(Phase::AwaitingHello, "{\"v\":1,\"event\":\"frobnicate\"}", event, log) ==
                  Received::Refused &&
              LogContains(log, "frobnicate"),
          "unknown: an unknown event type is refused and logged");

    Check(ReceiveEventLine(Phase::Ready,
                           "{\"v\":1,\"event\":\"hello\",\"worker_version\":\"0.1.0\",\"protocol\":1,"
                           "\"ops\":[]}",
                           event, log) == Received::Refused &&
              LogContains(log, "stale worker event 'hello'"),
          "stale: a second hello after the handshake is refused and logged");

    Check(ReceiveEventLine(Phase::AwaitingHello, kShutdownLine, event, log) == Received::Refused &&
              LogContains(log, "stale worker event 'shutdown'"),
          "stale: shutdown before a shutdown request is refused");

    Check(ReceiveEventLine(Phase::Ready, "{\"v\":1,\"event\":\"error\",\"code\":\"unknown_op\","
                                         "\"message\":\"no such op\"}",
                           event, log) == Received::Accepted &&
              LogContains(log, "worker reported error unknown_op"),
          "error event: accepted while ready and logged with its code");

    Check(ReceiveEventLine(Phase::Stopping, kShutdownLine, event, log) == Received::Accepted,
          "shutdown: the acknowledgement is accepted while stopping");
}

}  // namespace

int main() {
    TestRoundTrip();
    TestFrameLimits();
    TestMalformed();
    TestJsonValues();
    TestLogCap();
    TestPhaseRules();
    TestVersionMismatch();
    TestUnknownAndStale();

    std::printf("companion_protocol_tests: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
