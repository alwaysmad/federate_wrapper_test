#include "PythonSubprocess.hpp"
#include "JsonUdsChannel.hpp"

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
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const std::string cpp_sock = "/tmp/federate_cpp.sock";
    const std::string py_sock = "/tmp/federate_py.sock";

    try {
        // 1. Initialize C++ channel (binds /tmp/federate_cpp.sock immediately)
        JsonUdsChannel channel(cpp_sock, py_sock);

        // 2. Launch Python child passing explicit socket paths
        PythonSubprocess worker("worker.py", {
            "--listen-sock", py_sock,
            "--send-sock", cpp_sock
        });

        // Small delay for Python socket bind
        std::this_thread::sleep_for(150ms);

        // 3. Synchronous federate cycle
        int step = 0;
        while (!g_shutdown_requested.load() && step < 5) {
            if (!worker.is_running()) {
                std::cerr << "[C++] Worker exited prematurely.\n";
                break;
            }

            nlohmann::json tick = {
                {"command", "advance_time"},
                {"step", step},
                {"federate_time", step * 0.1}
            };

            std::cout << "[C++] Dispatching step " << step << "...\n";
            channel.send(tick);

            // Wait for worker ACK with 1-second timeout
            nlohmann::json ack = channel.receive(1000ms);
            std::cout << "[C++] Worker Response: " << ack.dump() << "\n";

            step++;
            std::this_thread::sleep_for(200ms);
        }

    } catch (const std::exception& e) {
        std::cerr << "[C++] Fatal Error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[C++] Clean exit. Sockets unlinked.\n";
    return 0;
}