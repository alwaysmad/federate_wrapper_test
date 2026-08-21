#include "PythonSubprocess.hpp"
#include "JsonUdpChannel.hpp"

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

static std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_shutdown_requested.store(true);
    }
}

int main() {
    // 1. Hook signals for clean Ctrl+C shutdown
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    constexpr uint16_t kCppBindPort = 5006;
    constexpr uint16_t kPythonBindPort = 5005;

    try {
        // 2. Launch Python subprogram targeting the port
        PythonSubprocess worker("worker.py", {
            "--listen-port", std::to_string(kPythonBindPort),
            "--send-port", std::to_string(kCppBindPort)
        });

        // Give Python a moment to bind its socket
        std::this_thread::sleep_for(200ms);

        // 3. Initialize the UDP channel
        JsonUdpChannel channel(kCppBindPort, kPythonBindPort);

        // 4. Dispatch test JSON packets every 500ms
        int iteration = 0;
        while (!g_shutdown_requested.load()) {
            if (!worker.is_running()) {
                std::cerr << "[C++] Error: Worker exited unexpectedly.\n";
                break;
            }

            nlohmann::json test_payload = {
                {"command", "telemetry_update"},
                {"step", iteration},
                {"position", {1.2 * iteration, 0.5 * iteration, 10.0}},
                {"active", true}
            };

            std::cout << "[C++] Sending iteration " << iteration << "...\n";
            channel.send(test_payload);

            iteration++;
            std::this_thread::sleep_for(500ms);
        }

        std::cout << "\n[C++] Stopping run loop...\n";

    } catch (const std::exception& e) {
        std::cerr << "[C++] Fatal Error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[C++] Exit clean.\n";
    return 0;
}