// Copyright (c) Zorkats
//
// Finds files next to the Companion executable and decides how the worker is started. Nothing here
// searches PATH.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gdx_companion {

struct WorkerLaunchOptions {
    std::string python;         // dev mode: explicit interpreter, used with worker_script
    std::string worker_script;  // dev mode: explicit worker .py file
};

// Directory that holds the running executable. Returns an empty path and sets error on failure.
std::filesystem::path ExecutableDirectory(std::string& error);

// UTF-8 text for a path, for argv and log lines.
std::string PathToUtf8(const std::filesystem::path& path);

// The path for UTF-8 text, the inverse of PathToUtf8.
std::filesystem::path PathFromUtf8(const std::string& text);

// A path for C code that hands it to fopen, which reads it in the system code page on Windows: an ASCII
// path as is, otherwise its 8.3 short form when the volume keeps one. Returns false when neither exists.
// Elsewhere the UTF-8 form is used.
bool NarrowPathForC(const std::filesystem::path& path, std::string& out);

// The game executable whose presence beside the Companion makes the installation Validated.
std::filesystem::path GameExecutablePath(const std::filesystem::path& exe_dir);

// Builds the worker argv. Dev mode needs both options: {python, "-I", worker_script}. Otherwise the
// frozen worker is used: {exe_dir/worker/gdx_companion_worker[.exe]}. Returns false with error set
// when an option is missing, a file is absent, or the frozen worker was not copied. description is a
// short line for the log.
bool ResolveWorkerLaunch(const WorkerLaunchOptions& options, const std::filesystem::path& exe_dir,
                         std::vector<std::string>& argv, std::string& description, std::string& error);

}  // namespace gdx_companion
