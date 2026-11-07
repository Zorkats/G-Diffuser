// Copyright (c) Zorkats
//
// Companion C1 jobs on the host side: the request a workspace sends, the staging folder each build
// owns, and the status of the one job that may run at a time. The worker side is
// companion/worker/gdx_companion_jobs.py.
//
// A build writes into <output folder>/.gdx-companion-staging-<job>, which the worker refuses to reuse.
// When the Companion cancels a job it ends the worker and deletes that folder, and only that folder:
// RemoveOwnedStaging checks the name and the parent first. The worker publishes a pack only after it
// checked it, and keeps the pack it replaced as <output>.previous, so a cancel or a crash never loses
// the last good pack.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gdx_companion_protocol.h"

namespace gdx_companion {

inline constexpr char kStagingPrefix[] = ".gdx-companion-staging-";
inline constexpr char kPreviousSuffix[] = ".previous";

struct JobOption {
    enum class Kind { Text, Flag, Paths };
    std::string key;
    Kind kind = Kind::Text;
    std::string text;                // Text
    bool flag = false;               // Flag
    std::vector<std::string> paths;  // Paths, UTF-8 absolute
};

struct JobRequest {
    std::string op;      // check, plan, build, init, names, inspect or unpack
    std::string job;     // set by WorkerSession::StartJob when empty
    std::string packer;  // htc, model, sample, sequence, soundfont or texture; "unpack" for inspect and unpack
    std::string input;   // UTF-8 absolute; empty for init and names; inspect and unpack: the pack to read
    std::string output;  // plan and build: UTF-8 absolute .o2r; init and unpack: the new folder
    std::string catalog;  // sample: the dump audio folder; init: the dump folder it starts from;
                          // inspect and unpack: the dump whose palettes and manifest they use, or empty
    std::string staging;  // build, init and unpack: set by WorkerSession::StartJob when empty
    bool overwrite = false;
    std::vector<JobOption> options;
};

// True for the operations that write through a staging folder beside their output: build, init and
// unpack.
bool OwnsStaging(const std::string& op);

// True for inspect and unpack, which read someone else's pack and take no options.
bool ReadsPack(const std::string& op);

// One protocol line, LF included. names sends only the packer; an empty input or catalog is left out;
// inspect and unpack send no options.
std::string EncodeJobRequest(const JobRequest& request);

// The staging folder a build of output by job owns.
std::filesystem::path StagingPathFor(const std::filesystem::path& output, const std::string& job);

// Deletes staging when it is the staging folder of output: beside it and named kStagingPrefix plus a
// job id. Returns true when the folder is gone afterwards, and false with error set otherwise.
bool RemoveOwnedStaging(const std::filesystem::path& staging, const std::filesystem::path& output,
                        std::string& error);

// After a build was cancelled or its worker died: when output is missing but output.previous exists,
// the worker stopped between its two renames, so output.previous goes back to output. Returns true
// when it moved the file back.
bool RestoreInterruptedPublish(const std::filesystem::path& output, std::string& error);

enum class JobState { Idle, Running, Succeeded, Problems, Failed, Cancelled };

struct JobStatus {
    JobState state = JobState::Idle;
    std::string job;
    std::string op;
    std::string packer;
    std::string output;   // UTF-8
    std::string staging;  // UTF-8, build only
    std::string code;     // job_error code, or "worker_stopped" and "cancelled"
    std::string detail;   // one sentence for the page
    Event result;         // the result event when state is Succeeded or Problems
};

// Current: the running job's own event. Stale: any other job's event, or one after the job ended.
enum class JobEventFit { Current, Stale };

JobEventFit ClassifyJobEvent(const JobStatus& status, const Event& event);

}  // namespace gdx_companion
