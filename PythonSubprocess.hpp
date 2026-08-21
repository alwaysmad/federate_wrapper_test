#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

class PythonSubprocess {
public:
    PythonSubprocess(const std::string& script_path, const std::vector<std::string>& args = {})
    {
        pid_ = fork();

        if (pid_ < 0)
            { throw std::runtime_error("Failed to fork Python process"); }

        if (pid_ == 0)
        {
            // Child process: assemble arguments for execvp
            std::vector<const char*> exec_args;
            exec_args.push_back("python3");
            exec_args.push_back(script_path.c_str());

            for (const auto& arg : args)
                { exec_args.push_back(arg.c_str()); }
            exec_args.push_back(nullptr);

            execvp("python3", const_cast<char* const*>(exec_args.data()));

            // If execvp fails, exit child immediately
            std::cerr << "[C++ Child] Failed to exec python3 on script: " << script_path << "\n";
            _exit(127);
        }

        std::cout << "[C++] Spawned Python worker [PID: " << pid_ << "]\n";
    }

    ~PythonSubprocess() { stop(); }

    void stop()
    {
        if (pid_ <= 0) { return; }

        std::cout << "[C++] Terminating Python worker [PID: " << pid_ << "]...\n";
        
        // 1. Send graceful termination signal
        kill(pid_, SIGTERM);

        // 2. Wait up to 500ms for exit
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 5; ++i)
        {
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) { exited = true; break; }
            usleep(100000); // 100ms
        }

        // 3. Force kill if child did not shut down
        if (!exited)
        {
            std::cerr << "[C++] Child unresponsive, sending SIGKILL...\n";
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0); // Reap zombie
        }

        pid_ = -1;
    }

    [[nodiscard]] bool is_running() const
    {
        if (pid_ <= 0) return false;
        int status = 0;
        pid_t result = waitpid(pid_, &status, WNOHANG);
        return result == 0;
    }

    [[nodiscard]] pid_t get_pid() const { return pid_; }

    // Delete copy operations (prevent duplicate process handling)
    PythonSubprocess(const PythonSubprocess&) = delete;
    PythonSubprocess& operator=(const PythonSubprocess&) = delete;

    // Allow move operations
    PythonSubprocess(PythonSubprocess&& other) noexcept : pid_(other.pid_) { other.pid_ = -1; }

    PythonSubprocess& operator=(PythonSubprocess&& other) noexcept
    {
        if (this != &other)
        {
            stop();
            pid_ = other.pid_;
            other.pid_ = -1;
        }
        return *this;
    }

private:
    pid_t pid_{-1};
};
