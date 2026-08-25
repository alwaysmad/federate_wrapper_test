#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <utility>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

class PythonSubprocess
{
private:
    mutable pid_t pid_{-1};
    static constexpr const char* kDefaultPython = "python3";

    void cleanup() noexcept
    {
        // 0. If no child process is running, nothing to do
        if (pid_ <= 0) return;

        // 1. Send graceful shutdown signal
        kill(pid_, SIGTERM);

        // 2. Wait up to 500ms for exit
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 5; ++i)
        {
            if (waitpid(pid_, &status, WNOHANG) == pid_)
                { exited = true; break; }
            usleep(100000); // 100ms
        }

        // 3. Force kill if child did not shut down
        if (!exited)
        {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0); // Reap zombie
        }

        pid_ = -1;
    }

public:
    explicit PythonSubprocess( const std::string& script_path, 
                               const std::vector<std::string>& args = {},
                               const char* python_bin = kDefaultPython )
    {
        // 1. Create a child process
        pid_ = fork();

        if (pid_ < 0)
            { throw std::runtime_error("[C++] Failed to fork Python process"); }

        if (pid_ == 0)
        {
            // Child process: assemble arguments for execvp
            std::vector<const char*> exec_args;
            exec_args.reserve(args.size() + 3);
            exec_args.push_back(python_bin);
            exec_args.push_back(script_path.c_str());

            for (const auto& arg : args)
                { exec_args.push_back(arg.c_str()); }
            exec_args.push_back(nullptr); // execvp() strictly requires a null-terminated array of pointers

            execvp(python_bin, const_cast<char* const*>(exec_args.data()));

            // If execvp fails, exit child immediately
            _exit(127); // Exit code 127 is the POSIX standard shell convention for "Command not found".
        }
    }

    ~PythonSubprocess() { cleanup(); }

    // Delete copy operations (prevent duplicate process handling)
    PythonSubprocess(const PythonSubprocess&) = delete;
    PythonSubprocess& operator=(const PythonSubprocess&) = delete;

    // Move operations transfer ownership of the running PID
    PythonSubprocess(PythonSubprocess&& other) noexcept : pid_(other.pid_) { other.pid_ = -1; }
    PythonSubprocess& operator=(PythonSubprocess&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            pid_ = std::exchange(other.pid_, -1);
        }
        return *this;
    }

    /**
     * @brief Checks if the child process is still alive.
     * Reaps the process if it has exited and returns false.
     */
    [[nodiscard]] bool is_running() const
    {
        if (pid_ <= 0) return false;

        int status = 0;
        pid_t result = waitpid(pid_, &status, WNOHANG);

        if (result == 0) // Child is still actively running
            { return true; }

        if (result == pid_) // Child exited on its own and has been reaped
            { pid_ = -1; return false; }

        // waitpid error (e.g., ECHILD)
        pid_ = -1;
        return false;
    }

    [[nodiscard]] inline pid_t get_pid() const noexcept { return pid_; }
};