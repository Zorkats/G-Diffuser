// Copyright (c) Zorkats

#include "gdx_companion_jobs.h"

#include <system_error>

#include "gdx_companion_json.h"
#include "gdx_companion_paths.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

std::string Quoted(const std::string& text) {
    return "\"" + JsonEscape(text) + "\"";
}

}  // namespace

bool OwnsStaging(const std::string& op) {
    return op == "build" || op == "init" || op == "unpack";
}

bool ReadsPack(const std::string& op) {
    return op == "inspect" || op == "unpack";
}

std::string EncodeJobRequest(const JobRequest& request) {
    std::string line = "{\"v\":" + std::to_string(kProtocolVersion) + ",\"op\":" + Quoted(request.op) +
                       ",\"job\":" + Quoted(request.job) + ",\"packer\":" + Quoted(request.packer);
    if (request.op == "names") return line + "}\n";
    if (!request.input.empty()) line += ",\"input\":" + Quoted(request.input);
    if (!request.catalog.empty()) line += ",\"catalog\":" + Quoted(request.catalog);
    if (request.op == "plan" || request.op == "build") {
        line += ",\"output\":" + Quoted(request.output);
        line += std::string(",\"overwrite\":") + (request.overwrite ? "true" : "false");
    }
    if (request.op == "init" || request.op == "unpack") line += ",\"output\":" + Quoted(request.output);
    if (OwnsStaging(request.op)) line += ",\"staging\":" + Quoted(request.staging);
    if (ReadsPack(request.op)) return line + "}\n";
    line += ",\"options\":{";
    for (size_t i = 0; i < request.options.size(); ++i) {
        const JobOption& option = request.options[i];
        if (i > 0) line += ',';
        line += Quoted(option.key) + ':';
        switch (option.kind) {
        case JobOption::Kind::Text:
            line += Quoted(option.text);
            break;
        case JobOption::Kind::Flag:
            line += option.flag ? "true" : "false";
            break;
        case JobOption::Kind::Paths:
            line += '[';
            for (size_t j = 0; j < option.paths.size(); ++j) {
                if (j > 0) line += ',';
                line += Quoted(option.paths[j]);
            }
            line += ']';
            break;
        }
    }
    return line + "}}\n";
}

fs::path StagingPathFor(const fs::path& output, const std::string& job) {
    return output.parent_path() / PathFromUtf8(std::string(kStagingPrefix) + job);
}

bool RemoveOwnedStaging(const fs::path& staging, const fs::path& output, std::string& error) {
    const std::string name = PathToUtf8(staging.filename());
    const std::string prefix = kStagingPrefix;
    if (staging.empty() || output.empty() || staging.parent_path() != output.parent_path() ||
        name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
        error = "not a staging folder of " + PathToUtf8(output) + ": " + PathToUtf8(staging);
        return false;
    }
    std::error_code ec;
    fs::remove_all(staging, ec);
    if (ec || fs::exists(staging, ec)) {
        error = "cannot delete the staging folder " + PathToUtf8(staging) + (ec ? ": " + ec.message() : "");
        return false;
    }
    return true;
}

bool RestoreInterruptedPublish(const fs::path& output, std::string& error) {
    if (output.empty()) return false;
    fs::path previous = output;
    previous += PathFromUtf8(kPreviousSuffix);
    std::error_code ec;
    if (fs::exists(output, ec) || !fs::is_regular_file(previous, ec)) return false;
    fs::rename(previous, output, ec);
    if (ec) {
        error = "cannot put " + PathToUtf8(previous) + " back: " + ec.message();
        return false;
    }
    return true;
}

JobEventFit ClassifyJobEvent(const JobStatus& status, const Event& event) {
    if (status.state != JobState::Running) return JobEventFit::Stale;
    // The worker runs one request at a time, so a refusal without a usable id answers the running job.
    if (event.type == EventType::JobError && event.job.empty()) return JobEventFit::Current;
    if (event.job != status.job) return JobEventFit::Stale;
    if (event.type == EventType::Result && event.op != status.op) return JobEventFit::Stale;
    return JobEventFit::Current;
}

}  // namespace gdx_companion
