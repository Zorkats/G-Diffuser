// Copyright (c) Zorkats
//
// Plain state owned by the UI thread. The worker session writes the worker fields and the log, and
// the UI reads them. Nothing here depends on SDL or ImGui.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gdx_companion_dialog.h"
#include "gdx_companion_install.h"
#include "gdx_companion_jobs.h"
#include "gdx_companion_log.h"
#include "gdx_companion_theme.h"

namespace gdx_companion {

enum class WorkerState { NotStarted, Starting, Ready, Failed, Stopped };

enum class Section { Music, Textures, Models, Tracks, Inspect, Advanced, Settings };

struct WorkerStatus {
    WorkerState state = WorkerState::NotStarted;
    std::string detail;  // the worker in use, or the reason it failed
    std::string version;  // worker_version from the hello event
    std::vector<std::string> ops;  // operations from the hello event
};

// A job a page asked for. owner names the page, so only that page reads the result.
struct PageJobRequest {
    std::string owner;
    JobRequest request;
};

// What the page wants the audio preview to do this frame.
struct PreviewRequest {
    std::string path;          // UTF-8; empty with stop set to stop playback
    uint64_t start_frame = 0;
    uint64_t loop_start = 0;   // repeat [loop_start, loop_end), source frames
    uint64_t loop_end = 0;
    bool stop = false;
};

struct PreviewStatus {
    bool available = false;  // SDL audio opened at startup
    bool playing = false;
    std::string path;        // UTF-8, what plays
    uint64_t position = 0;   // source frames
    uint32_t rate = 0;
    std::string error;       // the last failure, for the page
};

struct AppState {
    InstallState installation = InstallState::Unknown;
    std::string installation_detail;
    std::string installation_dir;      // UTF-8, the folder that holds the game; empty when Unavailable
    bool installation_chosen = false;  // a folder is saved in companion-settings.json
    std::string installation_error;    // the last rejected choice, shown under the path field
    bool install_browse = false;       // set by the UI: open the system file picker
    std::string install_choice;        // set by the UI: a typed path to use
    bool install_reset = false;        // set by the UI: forget the chosen folder
    WorkerStatus worker;
    JobStatus job;  // the last job, or the one running
    Section section = Section::Settings;
    int ui_scale_percent = 100;  // one of 100, 125, 150, 200
    std::vector<ThemeTokens> themes;  // Circuit first, then the user themes
    size_t theme_index = 0;           // the active theme
    int theme_request = -1;           // set by the UI: the theme to switch to
    bool theme_rescan = false;        // set by the UI: read the themes folder again
    bool theme_open_folder = false;   // set by the UI: show the themes folder
    std::string themes_dir;           // UTF-8, shown so users can copy it
    BoundedLog log;

    std::string exe_dir;  // UTF-8, the Companion's folder (music_targets.json lives there)

    // Workspace pages ask the main loop for dialogs, jobs and audio through these.
    bool browse_available = false;          // a system file dialog exists on this platform
    std::optional<BrowseRequest> browse;    // set by a page, cleared by the main loop
    std::map<std::string, std::string> browse_results;  // target to UTF-8 path; the page erases it
    std::optional<PageJobRequest> job_request;  // set by a page, cleared by the main loop
    std::string job_request_error;              // why the last request was not started
    std::string job_request_error_owner;        // the page whose request that was
    std::string job_owner;                      // the page that started the current job
    bool job_cancel = false;                    // set by a page: stop the running job
    bool worker_restart = false;                // set by a page: start the worker again after a cancel
    std::string open_folder;                    // set by a page: UTF-8 folder to show in the file manager
    std::optional<PreviewRequest> preview_request;
    PreviewStatus preview;
};

}  // namespace gdx_companion
