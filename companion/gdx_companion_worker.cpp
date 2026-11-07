// Copyright (c) Zorkats

#include "gdx_companion_worker.h"

#include <thread>

#include "gdx_companion_paths.h"

namespace gdx_companion {

namespace {

using Clock = std::chrono::steady_clock;

std::string JoinOps(const std::vector<std::string>& ops) {
    std::string out;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i > 0) out += ", ";
        out += ops[i];
    }
    return out;
}

bool Offers(const std::vector<std::string>& ops, const std::string& op) {
    for (const std::string& offered : ops) {
        if (offered == op) return true;
    }
    return false;
}

// What a stopped job left behind, for the cancel and worker-stopped messages.
std::string NothingLostText(const std::string& op) {
    if (op == "init") return "No project was made.";
    if (op == "unpack") return "No folder was written.";
    if (op == "inspect" || op == "names" || op == "plan" || op == "check") return "Nothing was written.";
    return "The last good pack was kept.";
}

}  // namespace

WorkerSession::WorkerSession(AppState& app, std::chrono::milliseconds handshake_timeout)
    : app_(app), handshake_timeout_(handshake_timeout) {}

bool WorkerSession::Start(const std::vector<std::string>& argv, const std::string& description) {
    if (phase_ != Phase::Closed) return false;

    // A fresh process and fresh line readers, so nothing from an earlier worker leaks into this one.
    child_ = std::make_unique<ChildProcess>();
    stdout_frames_.emplace(app_.log, "worker stdout");
    stderr_frames_.emplace(app_.log, "worker stderr");

    app_.worker = WorkerStatus{};
    app_.worker.state = WorkerState::Starting;
    app_.worker.detail = description;
    app_.log.Append(LogLevel::Info, "starting " + description);

    std::string error;
    if (!child_->Start(argv, error)) {
        Fail("could not start the worker process: " + error);
        return false;
    }

    phase_ = Phase::AwaitingHello;
    deadline_ = Clock::now() + handshake_timeout_;
    if (!child_->WriteStdin(EncodeRequest("hello"), error)) {
        Fail("could not send the hello request: " + error);
        return false;
    }
    return true;
}

bool WorkerSession::StartJob(JobRequest request, std::string& error) {
    if (phase_ != Phase::Ready) {
        error = "the worker is not ready";
        return false;
    }
    if (app_.job.state == JobState::Running) {
        error = "job " + app_.job.job + " is still running";
        return false;
    }
    if (!Offers(app_.worker.ops, request.op)) {
        error = "this worker does not offer '" + request.op + "'";
        return false;
    }
    if (request.job.empty()) request.job = "j" + std::to_string(next_job_++);
    if (OwnsStaging(request.op) && request.staging.empty()) {
        request.staging = PathToUtf8(StagingPathFor(PathFromUtf8(request.output), request.job));
    }
    if (!child_->WriteStdin(EncodeJobRequest(request), error)) {
        Fail("could not send job " + request.job + ": " + error);
        return false;
    }

    app_.job = JobStatus{};
    app_.job.state = JobState::Running;
    app_.job.job = request.job;
    app_.job.op = request.op;
    app_.job.packer = request.packer;
    app_.job.output = request.output;
    app_.job.staging = OwnsStaging(request.op) ? request.staging : std::string();
    app_.job.detail = request.op == "inspect"  ? std::string("Reading the pack.")
                      : request.op == "unpack" ? std::string("Unpacking the pack.")
                                               : "Running " + request.op + " with the " + request.packer + " packer.";
    app_.log.Append(LogLevel::Info, "job " + request.job + ": " + request.op + " " + request.packer);
    return true;
}

void WorkerSession::CancelJob() {
    if (app_.job.state != JobState::Running) return;
    const std::string job = app_.job.job;
    app_.log.Append(LogLevel::Info, "job " + job + ": cancelling, which ends the worker");
    DrainStderr();
    phase_ = Phase::Closed;
    child_->Terminate();  // the worker runs the packer in its own process, so this ends the whole job
    app_.worker.state = WorkerState::Stopped;
    app_.worker.detail = "stopped to cancel job " + job;
    CleanStaging();
    RestoreOutput();
    EndJob(JobState::Cancelled, "cancelled", "Cancelled. " + NothingLostText(app_.job.op));
}

void WorkerSession::Poll() {
    if (phase_ == Phase::Closed) return;

    // Sample the exit state before draining, so every byte written before exit is read below.
    const bool exited = child_->HasExited();

    scratch_.clear();
    child_->ReadStdout(scratch_);
    if (!scratch_.empty()) stdout_frames_->Feed(scratch_.data(), scratch_.size());
    std::string line;
    while (phase_ != Phase::Closed && stdout_frames_->Next(line)) HandleLine(line);
    DrainStderr();
    if (phase_ == Phase::Closed) return;

    const std::string code = std::to_string(child_->exit_code());
    if (exited) {
        if (phase_ == Phase::AwaitingHello) {
            Fail("the worker exited before answering hello (exit code " + code + ")");
        } else if (phase_ == Phase::Stopping) {
            Fail("the worker exited without confirming shutdown (exit code " + code + ")");
        } else {
            Fail("the worker exited unexpectedly (exit code " + code + ")");
        }
        return;
    }

    if (Clock::now() > deadline_) {
        if (phase_ == Phase::AwaitingHello) {
            Fail("no hello from the worker within " + std::to_string(handshake_timeout_.count()) + " ms");
        } else if (phase_ == Phase::Stopping) {
            Fail("the worker did not confirm shutdown within " + std::to_string(kShutdownTimeout.count()) +
                 " ms and was terminated");
        }
    }
}

void WorkerSession::Shutdown() {
    if (app_.job.state == JobState::Running) CancelJob();
    if (phase_ == Phase::Ready) {
        std::string error;
        app_.log.Append(LogLevel::Info, "asking the worker to shut down");
        if (!child_->WriteStdin(EncodeRequest("shutdown"), error)) {
            Fail("could not send the shutdown request: " + error);
        } else {
            phase_ = Phase::Stopping;
            deadline_ = Clock::now() + kShutdownTimeout;
            while (phase_ != Phase::Closed) {
                Poll();
                if (phase_ != Phase::Closed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    } else if (phase_ == Phase::AwaitingHello) {
        Finish(WorkerState::Stopped, "stopped before the handshake completed");
    }
    if (child_) {
        DrainStderr();
        child_->Terminate();
    }
}

void WorkerSession::HandleLine(const std::string& line) {
    Event event;
    switch (ReceiveEventLine(phase_, line, event, app_.log)) {
    case Received::Refused:
        return;
    case Received::VersionMismatch:
        Fail("the worker speaks a different protocol version than protocol " +
             std::to_string(kProtocolVersion));
        return;
    case Received::Accepted:
        break;
    }

    switch (event.type) {
    case EventType::Hello:
        app_.worker.state = WorkerState::Ready;
        app_.worker.version = event.worker_version;
        app_.worker.ops = event.ops;
        phase_ = Phase::Ready;
        app_.log.Append(LogLevel::Info, "worker ready: version " + event.worker_version + ", protocol " +
                                            std::to_string(kProtocolVersion) + ", operations " +
                                            JoinOps(event.ops));
        break;
    case EventType::Shutdown:
        child_->WaitForExit(static_cast<int>(kShutdownTimeout.count()));
        Finish(WorkerState::Stopped, "worker shut down");
        app_.log.Append(LogLevel::Info, "worker shut down");
        break;
    case EventType::Error:
        if (phase_ == Phase::AwaitingHello) Fail("the worker reported error " + event.code);
        break;
    case EventType::Log:
    case EventType::Result:
    case EventType::JobError:
        HandleJobEvent(event);
        break;
    case EventType::Unknown:
        break;
    }
}

void WorkerSession::HandleJobEvent(const Event& event) {
    if (ClassifyJobEvent(app_.job, event) == JobEventFit::Stale) {
        app_.log.Append(LogLevel::Warning, "stale worker event '" + event.name + "' for job " +
                                               (event.job.empty() ? std::string("(none)") : event.job) + " ignored");
        return;
    }
    const std::string prefix = "job " + app_.job.job + ": ";
    if (event.type == EventType::Log) {
        app_.log.Append(event.stream == "err" ? LogLevel::Warning : LogLevel::Info, prefix + event.text);
        return;
    }
    if (event.type == EventType::JobError) {
        // The worker already deleted its staging folder; this only covers a folder it could not delete.
        CleanStaging();
        EndJob(JobState::Failed, event.code, event.message.empty() ? event.code : event.message);
        return;
    }
    app_.job.result = event;
    if (event.status == "problems") {
        EndJob(JobState::Problems, std::string(),
               "The check found problems (status " + std::to_string(event.exit_code) + "). See the log.");
    } else if (event.op == "build") {
        EndJob(JobState::Succeeded, std::string(),
               event.replaced ? "Built and installed. The pack it replaced is kept as " + event.previous + "."
                              : "Built and installed.");
    } else if (event.op == "init") {
        EndJob(JobState::Succeeded, std::string(), "Project created in " + event.project + ".");
    } else if (event.op == "names") {
        EndJob(JobState::Succeeded, std::string(), std::to_string(event.names.size()) + " choices loaded.");
    } else if (event.op == "inspect") {
        EndJob(JobState::Succeeded, std::string(),
               "Read " + std::to_string(event.total) + (event.total == 1 ? " entry." : " entries."));
    } else if (event.op == "unpack") {
        std::string text = "Unpacked " + std::to_string(event.written) + " of " + std::to_string(event.total) +
                           " entries into " + event.folder + ".";
        if (event.failed > 0) {
            text += " " + std::to_string(event.failed) + " could not be written; contents.tsv says why.";
        }
        EndJob(JobState::Succeeded, std::string(), text);
    } else if (event.op == "plan") {
        EndJob(JobState::Succeeded, std::string(),
               event.needs_overwrite ? "The output exists. Building replaces it and keeps the old one as " +
                                           event.previous + "."
                                     : "Ready to build.");
    } else {
        EndJob(JobState::Succeeded, std::string(), "The check found no problems.");
    }
}

void WorkerSession::EndJob(JobState state, const std::string& code, const std::string& detail) {
    app_.job.state = state;
    app_.job.code = code;
    app_.job.detail = detail;
    const LogLevel level = state == JobState::Succeeded ? LogLevel::Info : LogLevel::Warning;
    app_.log.Append(level, "job " + app_.job.job + ": " + (code.empty() ? std::string() : code + ": ") + detail);
}

void WorkerSession::CleanStaging() {
    if (app_.job.staging.empty()) return;
    std::string error;
    if (!RemoveOwnedStaging(PathFromUtf8(app_.job.staging), PathFromUtf8(app_.job.output), error)) {
        app_.log.Append(LogLevel::Error, "job " + app_.job.job + ": " + error);
    }
}

void WorkerSession::RestoreOutput() {
    if (app_.job.op != "build") return;
    std::string error;
    if (RestoreInterruptedPublish(PathFromUtf8(app_.job.output), error)) {
        app_.log.Append(LogLevel::Warning, "job " + app_.job.job + ": the build stopped while installing; the "
                                           "pack it was replacing is back in place");
    } else if (!error.empty()) {
        app_.log.Append(LogLevel::Error, "job " + app_.job.job + ": " + error);
    }
}

void WorkerSession::DrainStderr() {
    if (!child_ || !stderr_frames_) return;
    scratch_.clear();
    child_->ReadStderr(scratch_);
    if (!scratch_.empty()) stderr_frames_->Feed(scratch_.data(), scratch_.size());
    std::string line;
    while (stderr_frames_->Next(line)) app_.log.Append(LogLevel::Info, "worker stderr: " + line);
}

void WorkerSession::Finish(WorkerState state, const std::string& detail) {
    DrainStderr();
    phase_ = Phase::Closed;
    app_.worker.state = state;
    app_.worker.detail = detail;
    if (child_) child_->Terminate();
    if (app_.job.state == JobState::Running) {
        CleanStaging();
        RestoreOutput();
        EndJob(JobState::Failed, "worker_stopped",
               "The worker stopped during the job. " + NothingLostText(app_.job.op));
    }
}

void WorkerSession::Fail(const std::string& reason) {
    Finish(WorkerState::Failed, reason);
    app_.log.Append(LogLevel::Error, "worker failed: " + reason);
}

}  // namespace gdx_companion
