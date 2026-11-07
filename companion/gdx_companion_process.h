// Copyright (c) Zorkats
//
// A child process with piped stdin, stdout and stderr. The argument vector is passed as given.
// There is no shell and no PATH search: argv[0] must be an absolute path to the executable.
// Output is read without blocking, so the UI thread can poll it once per frame.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace gdx_companion {

class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();  // terminates the child if it still runs
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool Start(const std::vector<std::string>& argv, std::string& error);

    // Writes all bytes. Blocks only while the pipe is full.
    bool WriteStdin(const std::string& bytes, std::string& error);

    // Appends available bytes without blocking. Returns the number appended.
    size_t ReadStdout(std::string& out);
    size_t ReadStderr(std::string& out);

    // Closes the child's stdin, which the child sees as end of input.
    void CloseStdin();

    // True once the child has exited. Records the exit code.
    bool HasExited();

    // Waits up to timeout_ms for the child to exit. Returns true if it has.
    bool WaitForExit(int timeout_ms);

    // Kills the child if it still runs, then waits for it and closes the pipes.
    void Terminate();

    bool started() const;
    int exit_code() const;  // -1 until the child has exited

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gdx_companion
