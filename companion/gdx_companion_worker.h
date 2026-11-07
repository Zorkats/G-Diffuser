// Copyright (c) Zorkats
//
// Runs one worker process: the handshake, one job at a time, and shutdown. Writes the worker and job
// fields of AppState and the log. Poll() is called once per UI frame, or in a loop when there is no UI.
// Cancelling a job ends the worker; Start() may then be called again.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gdx_companion_process.h"
#include "gdx_companion_protocol.h"
#include "gdx_companion_state.h"

namespace gdx_companion {

class WorkerSession {
public:
    static constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{5000};
    static constexpr std::chrono::milliseconds kShutdownTimeout{2000};

    explicit WorkerSession(AppState& app, std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout);
    WorkerSession(const WorkerSession&) = delete;
    WorkerSession& operator=(const WorkerSession&) = delete;

    // Starts the worker and sends the hello request. Returns false when the process could not be
    // started or the request could not be written; the worker status is then Failed. Refused while a
    // worker runs; allowed again once it stopped or failed.
    bool Start(const std::vector<std::string>& argv, const std::string& description);

    // Sends a job to a ready worker. Fills in the job id and, for a build, the staging folder.
    // Returns false with error set when the worker is not ready or a job already runs.
    bool StartJob(JobRequest request, std::string& error);

    // Ends the worker to stop the running job, then deletes the job's staging folder. The job is
    // Cancelled and the worker Stopped. Does nothing when no job runs.
    void CancelJob();

    // Reads pending output, applies events, and enforces the handshake and shutdown deadlines.
    void Poll();

    // Sends shutdown to a ready worker and waits up to kShutdownTimeout. The process is terminated in
    // every case, so this is safe to call at exit.
    void Shutdown();

private:
    void HandleLine(const std::string& line);
    void DrainStderr();
    void Finish(WorkerState state, const std::string& detail);
    void Fail(const std::string& reason);
    void HandleJobEvent(const Event& event);
    void EndJob(JobState state, const std::string& code, const std::string& detail);
    void CleanStaging();
    void RestoreOutput();

    AppState& app_;
    std::chrono::milliseconds handshake_timeout_;
    std::unique_ptr<ChildProcess> child_;
    std::optional<FrameReader> stdout_frames_;
    std::optional<FrameReader> stderr_frames_;
    Phase phase_ = Phase::Closed;
    uint64_t next_job_ = 1;
    std::chrono::steady_clock::time_point deadline_{};
    std::string scratch_;
};

}  // namespace gdx_companion
