// Copyright (c) Zorkats
//
// Headless tests for Companion C1 jobs on the host side: request encoding, job events, stale events,
// the owned staging folder, and whole jobs against the real worker and against small fake workers
// that crash, send stale events or send an oversized line. Every file lives in a fresh temporary
// folder; nothing reads a ROM, a dump or a game install.
//
// GDX_TEST_PYTHON and GDX_TEST_WORKER come from CMake: the build's Python and the worker script.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gdx_companion_jobs.h"
#include "gdx_companion_json.h"
#include "gdx_companion_paths.h"
#include "gdx_companion_protocol.h"
#include "gdx_companion_state.h"
#include "gdx_companion_worker.h"

using namespace gdx_companion;
namespace fs = std::filesystem;

namespace {

int gPassed = 0;
int gFailed = 0;

void Check(bool ok, const std::string& name) {
    if (ok) {
        ++gPassed;
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name.c_str());
    }
}

bool LogContains(const BoundedLog& log, const std::string& needle) {
    for (const LogEntry& entry : log.entries()) {
        if (entry.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

void WriteFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Scratch {
    fs::path root;
    Scratch() {
        std::random_device device;
        root = fs::temp_directory_path() / ("gdx_companion_job_tests_" + std::to_string(device()));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

std::string Sha(char c) {
    return std::string(64, c);
}

bool Parses(const std::string& line, Event& event) {
    std::string detail;
    std::string text = line;
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return ParseEventLine(text, event, detail) == ParseStatus::Ok;
}

void TestEncodeJobRequest() {
    JobRequest request;
    request.op = "build";
    request.job = "j7";
    request.packer = "htc";
    request.input = "C:/in \"quoted\"\\cache.htc";
    request.output = "C:/out/\xC3\xA9t\xC3\xA9.o2r";
    request.staging = "C:/out/.gdx-companion-staging-j7";
    request.overwrite = true;
    request.options.push_back({"name", JobOption::Kind::Text, "My \"HD\" pack", false, {}});
    request.options.push_back({"alt", JobOption::Kind::Flag, "", true, {}});
    request.options.push_back({"base", JobOption::Kind::Paths, "", false, {"C:/a.o2r", "C:/b.o2r"}});
    const std::string line = EncodeJobRequest(request);
    Check(!line.empty() && line.back() == '\n', "job request ends with LF");
    JsonValue root;
    std::string error;
    Check(JsonValue::Parse(line.substr(0, line.size() - 1), root, error), "job request is valid JSON");
    auto text = [&root](const char* key) {
        const JsonValue* v = root.Find(key);
        return v != nullptr && v->kind() == JsonValue::Kind::String ? v->AsString() : std::string("<missing>");
    };
    Check(text("op") == "build" && text("job") == "j7" && text("packer") == "htc", "job request names");
    Check(text("input") == request.input && text("output") == request.output, "job request paths survive escaping");
    Check(text("staging") == request.staging, "build carries its staging folder");
    const JsonValue* overwrite = root.Find("overwrite");
    Check(overwrite != nullptr && overwrite->kind() == JsonValue::Kind::Bool && overwrite->AsBool(), "overwrite");
    const JsonValue* options = root.Find("options");
    Check(options != nullptr && options->Keys().size() == 3, "three options");
    if (options != nullptr && options->Keys().size() == 3) {
        Check(options->Find("name")->AsString() == "My \"HD\" pack", "text option");
        Check(options->Find("alt")->AsBool(), "flag option");
        Check(options->Find("base")->AsArray().size() == 2 &&
                  options->Find("base")->AsArray()[1].AsString() == "C:/b.o2r",
              "paths option");
    }

    JobRequest check;
    check.op = "check";
    check.job = "c1";
    check.packer = "sequence";
    check.input = "C:/seq";
    check.output = "C:/ignored.o2r";
    check.staging = "C:/.gdx-companion-staging-c1";
    JsonValue check_root;
    const std::string check_line = EncodeJobRequest(check);
    Check(JsonValue::Parse(check_line.substr(0, check_line.size() - 1), check_root, error), "check is JSON");
    Check(check_root.Find("output") == nullptr && check_root.Find("staging") == nullptr &&
              check_root.Find("overwrite") == nullptr,
          "check sends no output, staging or overwrite");

    JobRequest init;
    init.op = "init";
    init.job = "i1";
    init.packer = "model";
    init.output = "C:/projects/falcon";
    init.catalog = "C:/dump/models";
    init.staging = "C:/projects/.gdx-companion-staging-i1";
    init.options.push_back({"machine", JobOption::Kind::Text, "blue_falcon", false, {}});
    JsonValue init_root;
    const std::string init_line = EncodeJobRequest(init);
    Check(JsonValue::Parse(init_line.substr(0, init_line.size() - 1), init_root, error), "init is JSON");
    Check(init_root.Find("input") == nullptr && init_root.Find("overwrite") == nullptr &&
              init_root.Find("output")->AsString() == init.output &&
              init_root.Find("staging")->AsString() == init.staging &&
              init_root.Find("catalog")->AsString() == init.catalog &&
              init_root.Find("options")->Find("machine")->AsString() == "blue_falcon",
          "init sends output, staging, catalog and options, and no input");

    JobRequest names;
    names.op = "names";
    names.job = "n1";
    names.packer = "soundfont";
    names.input = "C:/ignored";
    names.options.push_back({"font", JobOption::Kind::Text, "x", false, {}});
    JsonValue names_root;
    const std::string names_line = EncodeJobRequest(names);
    Check(JsonValue::Parse(names_line.substr(0, names_line.size() - 1), names_root, error), "names is JSON");
    Check(names_root.Keys().size() == 4 && names_root.Find("packer")->AsString() == "soundfont",
          "names sends only v, op, job and packer");
    Check(OwnsStaging("build") && OwnsStaging("init") && OwnsStaging("unpack") && !OwnsStaging("check") &&
              !OwnsStaging("plan") && !OwnsStaging("names") && !OwnsStaging("inspect"),
          "only build, init and unpack own a staging folder");
    Check(ReadsPack("inspect") && ReadsPack("unpack") && !ReadsPack("build") && !ReadsPack("check"),
          "inspect and unpack read a pack");

    JobRequest inspect;
    inspect.op = "inspect";
    inspect.job = "r1";
    inspect.packer = "unpack";
    inspect.input = "C:/packs/theirs \xC3\xA9.o2r";
    inspect.catalog = "C:/game/dump";
    inspect.output = "C:/ignored";
    inspect.options.push_back({"name", JobOption::Kind::Text, "ignored", false, {}});
    JsonValue inspect_root;
    const std::string inspect_line = EncodeJobRequest(inspect);
    Check(JsonValue::Parse(inspect_line.substr(0, inspect_line.size() - 1), inspect_root, error), "inspect is JSON");
    Check(inspect_root.Keys().size() == 6 && inspect_root.Find("input")->AsString() == inspect.input &&
              inspect_root.Find("catalog")->AsString() == inspect.catalog && inspect_root.Find("options") == nullptr &&
              inspect_root.Find("output") == nullptr,
          "inspect sends input and catalog, and no output or options");

    JobRequest unpack = inspect;
    unpack.op = "unpack";
    unpack.job = "u1";
    unpack.catalog.clear();
    unpack.output = "C:/packs/theirs-unpacked";
    unpack.staging = "C:/packs/.gdx-companion-staging-u1";
    JsonValue unpack_root;
    const std::string unpack_line = EncodeJobRequest(unpack);
    Check(JsonValue::Parse(unpack_line.substr(0, unpack_line.size() - 1), unpack_root, error), "unpack is JSON");
    Check(unpack_root.Find("output")->AsString() == unpack.output &&
              unpack_root.Find("staging")->AsString() == unpack.staging && unpack_root.Find("catalog") == nullptr &&
              unpack_root.Find("options") == nullptr && unpack_root.Find("overwrite") == nullptr,
          "unpack sends output and staging, no empty catalog and no options");
}

void TestJobEvents() {
    Event log;
    log.type = EventType::Log;
    log.job = "j1";
    log.stream = "err";
    log.text = "line with \"quotes\" and \xE2\x98\x83";
    Event parsed;
    Check(Parses(EncodeEvent(log), parsed) && parsed.type == EventType::Log && parsed.text == log.text &&
              parsed.stream == "err" && parsed.job == "j1",
          "log round trip");

    Event result;
    result.type = EventType::Result;
    result.job = "j1";
    result.op = "build";
    result.status = "ok";
    result.artifacts.push_back({"C:/out/pack.o2r", 1234, Sha('a')});
    result.replaced = true;
    result.previous = "C:/out/pack.o2r.previous";
    Check(Parses(EncodeEvent(result), parsed) && parsed.type == EventType::Result && parsed.artifacts.size() == 1 &&
              parsed.artifacts[0].bytes == 1234 && parsed.replaced && parsed.previous == result.previous,
          "result round trip");

    Event init;
    init.type = EventType::Result;
    init.job = "i1";
    init.op = "init";
    init.status = "ok";
    init.project = "C:/projects/falcon \xC3\xA9";
    Check(Parses(EncodeEvent(init), parsed) && parsed.op == "init" && parsed.project == init.project &&
              parsed.artifacts.empty(),
          "init result round trip");

    Event names;
    names.type = EventType::Result;
    names.job = "n1";
    names.op = "names";
    names.status = "ok";
    names.names = {"FONT_GUITAR", "FONT_\"Q\""};
    Check(Parses(EncodeEvent(names), parsed) && parsed.op == "names" && parsed.names == names.names,
          "names result round trip");

    Event unpacked;
    unpacked.type = EventType::Result;
    unpacked.job = "u1";
    unpacked.op = "unpack";
    unpacked.status = "ok";
    unpacked.format = "o2r";
    unpacked.title = "Theirs \"HD\"";
    unpacked.author = "someone";
    unpacked.pack_version = "1.2";
    unpacked.pack_id = "someone.theirs";
    unpacked.description = "line one\nline two";
    unpacked.summary = {"3 x Texture", "Needs: base.pack"};
    unpacked.entries = {"textures/pack/seg/a\tTexture\tRGBA16 4x4", "workshop.json\tPack metadata\t45 bytes"};
    unpacked.total = 7;
    unpacked.folder = "C:/packs/theirs-unpacked \xC3\xA9";
    unpacked.written = 6;
    unpacked.failed = 1;
    Check(Parses(EncodeEvent(unpacked), parsed) && parsed.op == "unpack" && parsed.format == "o2r" &&
              parsed.title == unpacked.title && parsed.author == "someone" && parsed.pack_version == "1.2" &&
              parsed.pack_id == unpacked.pack_id && parsed.description == unpacked.description &&
              parsed.summary == unpacked.summary && parsed.entries == unpacked.entries && parsed.total == 7 &&
              parsed.folder == unpacked.folder && parsed.written == 6 && parsed.failed == 1,
          "unpack result round trip");
    Event inspected = unpacked;
    inspected.op = "inspect";
    inspected.folder.clear();
    Check(Parses(EncodeEvent(inspected), parsed) && parsed.op == "inspect" && parsed.total == 7 &&
              parsed.folder.empty() && parsed.written == 0,
          "inspect result round trip");

    Event error;
    error.type = EventType::JobError;
    error.job = "j1";
    error.code = "packer_failed";
    error.message = "the packer stopped";
    error.has_exit = true;
    error.exit_code = 2;
    Check(Parses(EncodeEvent(error), parsed) && parsed.type == EventType::JobError && parsed.code == "packer_failed" &&
              parsed.has_exit && parsed.exit_code == 2,
          "job_error round trip");
    Check(Parses("{\"v\":1,\"event\":\"job_error\",\"job\":\"\",\"code\":\"bad_job\"}", parsed),
          "job_error may have an empty job id");

    const char* malformed[] = {
        "{\"v\":1,\"event\":\"log\",\"stream\":\"out\",\"text\":\"x\"}",
        "{\"v\":1,\"event\":\"log\",\"job\":\"\",\"stream\":\"out\",\"text\":\"x\"}",
        "{\"v\":1,\"event\":\"log\",\"job\":\"has space\",\"stream\":\"out\",\"text\":\"x\"}",
        "{\"v\":1,\"event\":\"log\",\"job\":\"j1\",\"stream\":\"both\",\"text\":\"x\"}",
        "{\"v\":1,\"event\":\"log\",\"job\":\"j1\",\"stream\":\"out\",\"text\":5}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"delete\",\"status\":\"ok\"}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"check\",\"status\":\"maybe\"}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"build\",\"status\":\"ok\"}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"build\",\"status\":\"ok\",\"artifacts\":[{\"path\":"
        "\"C:/a\",\"bytes\":1,\"sha256\":\"xyz\"}]}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"build\",\"status\":\"ok\",\"artifacts\":[{\"path\":"
        "\"C:/a\",\"bytes\":-1,\"sha256\":\"" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" "\"}]}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"check\",\"status\":\"ok\",\"exit\":1.5}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"plan\",\"status\":\"ok\",\"exists\":\"yes\"}",
        "{\"v\":1,\"event\":\"job_error\",\"job\":\"j1\",\"message\":\"no code\"}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"i1\",\"op\":\"init\",\"status\":\"ok\"}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"i1\",\"op\":\"init\",\"status\":\"ok\",\"project\":7}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"n1\",\"op\":\"names\",\"status\":\"ok\",\"names\":\"a\"}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"n1\",\"op\":\"names\",\"status\":\"ok\",\"names\":[\"a\",3]}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"n1\",\"op\":\"names\",\"status\":\"ok\",\"names\":[\"\"]}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"u1\",\"op\":\"unpack\",\"status\":\"ok\",\"total\":0}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"r1\",\"op\":\"inspect\",\"status\":\"ok\",\"entries\":[\"a\"],"
        "\"total\":0}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"r1\",\"op\":\"inspect\",\"status\":\"ok\",\"total\":-1}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"r1\",\"op\":\"inspect\",\"status\":\"ok\",\"summary\":[3],"
        "\"total\":0}",
        "{\"v\":1,\"event\":\"result\",\"job\":\"r1\",\"op\":\"inspect\",\"status\":\"ok\",\"title\":5,"
        "\"total\":0}",
    };
    for (const char* line : malformed) {
        std::string detail;
        Check(ParseEventLine(line, parsed, detail) == ParseStatus::Malformed, std::string("refused: ") + line);
    }
    std::string many = "{\"v\":1,\"event\":\"result\",\"job\":\"j1\",\"op\":\"build\",\"status\":\"ok\",\"artifacts\":[";
    for (int i = 0; i < 17; ++i) {
        many += std::string(i ? "," : "") + "{\"path\":\"C:/a\",\"bytes\":1,\"sha256\":\"" + Sha('b') + "\"}";
    }
    many += "]}";
    std::string detail;
    Check(ParseEventLine(many, parsed, detail) == ParseStatus::Malformed, "more than 16 artifacts refused");
    std::string too_many = "{\"v\":1,\"event\":\"result\",\"job\":\"n1\",\"op\":\"names\",\"status\":\"ok\",\"names\":[";
    for (size_t i = 0; i <= kMaxNames; ++i) too_many += std::string(i ? "," : "") + "\"n\"";
    too_many += "]}";
    Check(ParseEventLine(too_many, parsed, detail) == ParseStatus::Malformed, "more than kMaxNames names refused");

    Check(ClassifyEvent(Phase::Ready, EventType::Result) == Disposition::Accept, "results accepted when ready");
    Check(ClassifyEvent(Phase::AwaitingHello, EventType::Log) == Disposition::Stale, "job log before hello is stale");
    Check(ClassifyEvent(Phase::Stopping, EventType::Result) == Disposition::Stale, "result while stopping is stale");
}

void TestClassifyJobEvent() {
    JobStatus status;
    status.state = JobState::Running;
    status.job = "j2";
    status.op = "build";
    Event event;
    event.type = EventType::Result;
    event.job = "j2";
    event.op = "build";
    Check(ClassifyJobEvent(status, event) == JobEventFit::Current, "own result is current");
    event.job = "j1";
    Check(ClassifyJobEvent(status, event) == JobEventFit::Stale, "older job's result is stale");
    event.job = "j2";
    event.op = "check";
    Check(ClassifyJobEvent(status, event) == JobEventFit::Stale, "result for another op is stale");
    event.type = EventType::JobError;
    event.job.clear();
    Check(ClassifyJobEvent(status, event) == JobEventFit::Current, "refusal without id answers the running job");
    event.type = EventType::Log;
    Check(ClassifyJobEvent(status, event) == JobEventFit::Stale, "log without id is stale");
    status.state = JobState::Succeeded;
    event.type = EventType::Result;
    event.job = "j2";
    event.op = "build";
    Check(ClassifyJobEvent(status, event) == JobEventFit::Stale, "result after the job ended is stale");
}

void TestStaging() {
    Scratch scratch;
    const fs::path output = scratch.root / "pack.o2r";
    const fs::path staging = StagingPathFor(output, "j3");
    Check(staging == scratch.root / ".gdx-companion-staging-j3", "staging sits beside the output");
    fs::create_directories(staging / "inner");
    WriteFile(staging / "inner" / "half.o2r", "partial");
    std::string error;
    Check(RemoveOwnedStaging(staging, output, error) && !fs::exists(staging), "owned staging removed");
    Check(RemoveOwnedStaging(staging, output, error), "missing staging counts as removed");

    const fs::path keep = scratch.root / "keep";
    fs::create_directories(keep);
    WriteFile(keep / "mine.txt", "user data");
    Check(!RemoveOwnedStaging(keep, output, error) && fs::exists(keep / "mine.txt"), "foreign folder refused");
    Check(!RemoveOwnedStaging(scratch.root / ".gdx-companion-staging-", output, error), "bare prefix refused");
    const fs::path elsewhere = scratch.root / "sub" / ".gdx-companion-staging-j3";
    fs::create_directories(elsewhere);
    Check(!RemoveOwnedStaging(elsewhere, output, error) && fs::exists(elsewhere), "staging in another folder refused");
    Check(!RemoveOwnedStaging(scratch.root, output, error) && fs::exists(scratch.root), "output folder refused");
    Check(!RemoveOwnedStaging(fs::path(), output, error), "empty path refused");

    // Interrupted publish: output gone, previous present.
    fs::path previous = output;
    previous += ".previous";
    WriteFile(previous, "old pack");
    Check(RestoreInterruptedPublish(output, error) && ReadFile(output) == "old pack" && !fs::exists(previous),
          "interrupted publish restored");
    WriteFile(previous, "older pack");
    Check(!RestoreInterruptedPublish(output, error) && ReadFile(output) == "old pack",
          "existing output is never replaced by previous");
}

// Session tests against a worker process.

#ifdef GDX_TEST_PYTHON

bool WaitFor(AppState& app, WorkerSession& session, bool (*done)(const AppState&), int timeout_ms = 60000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done(app)) {
        session.Poll();
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool NotStarting(const AppState& app) {
    return app.worker.state != WorkerState::Starting;
}

bool JobEnded(const AppState& app) {
    return app.job.state != JobState::Running;
}

std::vector<std::string> RealWorker() {
    return {GDX_TEST_PYTHON, "-I", GDX_TEST_WORKER};
}

std::vector<std::string> FakeWorker(const fs::path& script) {
    return {GDX_TEST_PYTHON, "-I", PathToUtf8(script)};
}

JobRequest SequenceBuild(const fs::path& source, const fs::path& output, bool overwrite, const std::string& name) {
    JobRequest request;
    request.op = "build";
    request.packer = "sequence";
    request.input = PathToUtf8(source);
    request.output = PathToUtf8(output);
    request.overwrite = overwrite;
    request.options.push_back({"name", JobOption::Kind::Text, name, false, {}});
    return request;
}

void TestRealWorker() {
    Scratch scratch;
    const fs::path source = scratch.root / "seq";
    fs::create_directories(source);
    WriteFile(source / "ddbgm_mute_city.seq", std::string("\xD3\x80\xD7\xFF\xFF\x00", 6) + std::string(26, '\0'));
    const fs::path out_dir = scratch.root / "out \xC3\xA9";  // a space and a non-ASCII letter
    fs::create_directories(out_dir);
    const fs::path output = out_dir / "music.o2r";

    AppState app;
    WorkerSession session(app);
    Check(session.Start(RealWorker(), "test worker"), "real worker starts");
    Check(WaitFor(app, session, NotStarting) && app.worker.state == WorkerState::Ready, "real worker ready");
    std::string error;

    Check(session.StartJob(SequenceBuild(source, output, false, "One"), error), "first build starts");
    Check(!session.StartJob(SequenceBuild(source, output, false, "Two"), error), "second job refused while running");
    Check(WaitFor(app, session, JobEnded) && app.job.state == JobState::Succeeded, "first build succeeds");
    Check(fs::is_regular_file(output) && app.job.result.artifacts.size() == 1 &&
              app.job.result.artifacts[0].bytes == static_cast<int64_t>(fs::file_size(output)),
          "artifact matches the file");
    Check(!fs::exists(StagingPathFor(output, app.job.job)), "staging gone after the build");
    Check(LogContains(app.log, "job " + app.job.job + ": wrote"), "packer output reached the log");
    const std::string first = ReadFile(output);

    Check(session.StartJob(SequenceBuild(source, output, false, "Two"), error), "build without overwrite starts");
    Check(WaitFor(app, session, JobEnded) && app.job.state == JobState::Failed && app.job.code == "exists",
          "existing output needs overwrite");
    Check(ReadFile(output) == first, "refused build left the pack alone");

    JobRequest plan = SequenceBuild(source, output, false, "Two");
    plan.op = "plan";
    Check(session.StartJob(plan, error) && WaitFor(app, session, JobEnded) && app.job.state == JobState::Succeeded &&
              app.job.result.needs_overwrite,
          "plan says the output needs overwrite");

    Check(session.StartJob(SequenceBuild(source, output, true, "Two"), error), "overwrite build starts");
    Check(WaitFor(app, session, JobEnded) && app.job.state == JobState::Succeeded && app.job.result.replaced,
          "overwrite build succeeds");
    fs::path previous = output;
    previous += ".previous";
    Check(ReadFile(previous) == first && ReadFile(output) != first, "previous pack kept");

    JobRequest unsupported = SequenceBuild(source, output, true, "Three");
    unsupported.packer = "nope";
    Check(session.StartJob(unsupported, error) && WaitFor(app, session, JobEnded) &&
              app.job.state == JobState::Failed && app.job.code == "unsupported_packer",
          "unknown packer refused");

    // Cancel right away: whatever the worker reached, the staging folder goes and a pack stays.
    const std::string before = ReadFile(output);
    Check(session.StartJob(SequenceBuild(source, output, true, "Four"), error), "cancelled build starts");
    const std::string staging = app.job.staging;
    session.CancelJob();
    Check(app.job.state == JobState::Cancelled && app.worker.state == WorkerState::Stopped, "cancel ends the job");
    Check(!fs::exists(PathFromUtf8(staging)), "cancel removed the staging folder");
    Check(fs::is_regular_file(output), "a pack is still installed after cancel");
    const std::string after = ReadFile(output);
    Check(after == before || ReadFile(previous) == before, "the last good pack survives the cancel");

    Check(session.Start(RealWorker(), "test worker again"), "worker restarts after cancel");
    Check(WaitFor(app, session, NotStarting) && app.worker.state == WorkerState::Ready, "restarted worker ready");

    // Someone else's pack: the sequence pack built above. Inspect writes nothing; unpack makes a new folder.
    JobRequest inspect;
    inspect.op = "inspect";
    inspect.packer = "unpack";
    inspect.input = PathToUtf8(output);
    Check(session.StartJob(inspect, error) && WaitFor(app, session, JobEnded) &&
              app.job.state == JobState::Succeeded && app.job.result.op == "inspect" &&
              app.job.result.format == "o2r" && !app.job.result.title.empty() && app.job.result.total >= 2 &&
              static_cast<int64_t>(app.job.result.entries.size()) == app.job.result.total,
          "inspect lists the pack");
    const fs::path unpacked = out_dir / "music unpacked";
    JobRequest unpack = inspect;
    unpack.op = "unpack";
    unpack.output = PathToUtf8(unpacked);
    Check(session.StartJob(unpack, error) && WaitFor(app, session, JobEnded) &&
              app.job.state == JobState::Succeeded && app.job.result.folder == unpack.output &&
              app.job.result.written == app.job.result.total && app.job.result.failed == 0,
          "unpack writes every entry");
    Check(fs::is_regular_file(unpacked / "contents.tsv") && fs::is_regular_file(unpacked / "workshop.json") &&
              !fs::exists(PathFromUtf8(app.job.staging)),
          "unpacked folder in place and its staging gone");
    Check(LogContains(app.log, "unpacked " + std::to_string(app.job.result.written)), "unpack progress logged");
    Check(session.StartJob(unpack, error) && WaitFor(app, session, JobEnded) && app.job.state == JobState::Failed &&
              app.job.code == "exists",
          "unpack refuses an existing folder");
    const fs::path cancelled = out_dir / "cancelled unpack";
    unpack.output = PathToUtf8(cancelled);
    Check(session.StartJob(unpack, error), "cancelled unpack starts");
    const std::string unpack_staging = app.job.staging;
    session.CancelJob();
    Check(app.job.state == JobState::Cancelled && !fs::exists(PathFromUtf8(unpack_staging)),
          "cancelled unpack leaves no staging folder");
    Check(app.job.detail.find("No folder was written") != std::string::npos, "cancel says no folder was written");
    std::error_code ec;
    fs::remove_all(cancelled, ec);  // the worker may have finished before the cancel landed
    Check(session.Start(RealWorker(), "test worker after unpack"), "worker restarts after the unpack cancel");
    Check(WaitFor(app, session, NotStarting) && app.worker.state == WorkerState::Ready, "worker ready again");

    // A model project from a minimal dump: names first, then init through a staging folder.
    const fs::path dump = scratch.root / "dump";
    fs::create_directories(dump);
    WriteFile(dump / "machine_models_D_9001210.obj",
              "mtllib machine_models_D_9001210.mtl\nv 0 0 0 1 1 1\nv 10 0 0 1 1 1\nv 0 10 0 1 1 1\n"
              "usemtl body--flat\nf 1 2 3\n");
    WriteFile(dump / "machine_models_D_9001210.mtl", "newmtl body--flat\nKd 1 0 0\n");
    JobRequest names;
    names.op = "names";
    names.packer = "model";
    Check(session.StartJob(names, error) && WaitFor(app, session, JobEnded) &&
              app.job.state == JobState::Succeeded &&
              std::find(app.job.result.names.begin(), app.job.result.names.end(), "blue_falcon") !=
                  app.job.result.names.end(),
          "names lists blue_falcon");
    const fs::path project = out_dir / "falcon";
    JobRequest init;
    init.op = "init";
    init.packer = "model";
    init.output = PathToUtf8(project);
    init.catalog = PathToUtf8(dump);
    init.options.push_back({"machine", JobOption::Kind::Text, "blue_falcon", false, {}});
    Check(session.StartJob(init, error) && WaitFor(app, session, JobEnded) &&
              app.job.state == JobState::Succeeded && app.job.result.project == init.output,
          "init creates the project");
    Check(fs::is_directory(project) && !fs::is_empty(project) && !fs::exists(PathFromUtf8(app.job.staging)),
          "project in place and its staging gone");
    Check(session.StartJob(init, error) && WaitFor(app, session, JobEnded) && app.job.state == JobState::Failed &&
              app.job.code == "exists",
          "init refuses an existing project");
    session.Shutdown();
    Check(app.worker.state == WorkerState::Stopped, "worker shuts down");
}

const char kFakeHead[] =
    "import json, sys\n"
    "out = sys.stdout\n"
    "def send(e):\n"
    "    e['v'] = 1\n"
    "    out.write(json.dumps(e) + '\\n'); out.flush()\n"
    "for raw in sys.stdin:\n"
    "    req = json.loads(raw)\n"
    "    op = req['op']\n"
    "    if op == 'hello':\n"
    "        send({'event': 'hello', 'worker_version': 'fake', 'protocol': 1, 'ops': ['hello', 'shutdown', "
    "'check', 'plan', 'build']})\n"
    "    elif op == 'shutdown':\n"
    "        send({'event': 'shutdown'}); break\n"
    "    else:\n";

void TestFakeWorkers() {
    Scratch scratch;
    const fs::path source = scratch.root / "seq";
    fs::create_directories(source);
    const fs::path output = scratch.root / "pack.o2r";
    std::string error;

    {
        // Crash in the middle of a build.
        const fs::path script = scratch.root / "crash.py";
        WriteFile(script, std::string(kFakeHead) + "        sys.exit(3)\n");
        AppState app;
        WorkerSession session(app);
        session.Start(FakeWorker(script), "crashing worker");
        Check(WaitFor(app, session, NotStarting) && app.worker.state == WorkerState::Ready, "crash worker ready");
        Check(session.StartJob(SequenceBuild(source, output, true, "X"), error), "crash job starts");
        fs::create_directories(PathFromUtf8(app.job.staging));  // as if the worker had begun
        WriteFile(PathFromUtf8(app.job.staging) / "pack.o2r", "half");
        Check(WaitFor(app, session, JobEnded) && app.job.state == JobState::Failed &&
                  app.job.code == "worker_stopped",
              "crash fails the job");
        Check(app.worker.state == WorkerState::Failed, "crash fails the worker");
        Check(!fs::exists(PathFromUtf8(app.job.staging)), "crash staging removed");
        session.Shutdown();
    }
    {
        // Stale events and an oversized line before the real answer.
        const fs::path script = scratch.root / "stale.py";
        WriteFile(script, std::string(kFakeHead) +
                              "        send({'event': 'log', 'job': 'old', 'stream': 'out', 'text': 'from an older job'})\n"
                              "        send({'event': 'result', 'job': 'old', 'op': req['op'], 'status': 'ok'})\n"
                              "        out.write('{\"v\":1,\"event\":\"log\",\"job\":\"' + req['job'] + "
                              "'\",\"stream\":\"out\",\"text\":\"' + 'x' * 70000 + '\"}\\n')\n"
                              "        send({'event': 'log', 'job': req['job'], 'stream': 'out', 'text': 'real line'})\n"
                              "        send({'event': 'result', 'job': req['job'], 'op': req['op'], 'status': 'problems', "
                              "'exit': 1})\n");
        AppState app;
        WorkerSession session(app);
        session.Start(FakeWorker(script), "stale worker");
        Check(WaitFor(app, session, NotStarting) && app.worker.state == WorkerState::Ready, "stale worker ready");
        JobRequest check;
        check.op = "check";
        check.packer = "sequence";
        check.input = PathToUtf8(source);
        Check(session.StartJob(check, error), "stale check starts");
        Check(WaitFor(app, session, JobEnded) && app.job.state == JobState::Problems, "real result wins");
        Check(LogContains(app.log, "stale worker event 'result' for job old ignored"), "stale result logged");
        Check(!LogContains(app.log, "from an older job"), "stale log line not shown");
        Check(LogContains(app.log, ": real line"), "real log line shown");
        Check(LogContains(app.log, "65536"), "oversized line refused");
        session.Shutdown();
        Check(app.worker.state == WorkerState::Stopped, "stale worker shuts down");
    }
}

#endif  // GDX_TEST_PYTHON

}  // namespace

int main() {
    TestEncodeJobRequest();
    TestJobEvents();
    TestClassifyJobEvent();
    TestStaging();
#ifdef GDX_TEST_PYTHON
    TestRealWorker();
    TestFakeWorkers();
#else
    std::printf("SKIP: session tests (no Python at configure time)\n");
#endif
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
