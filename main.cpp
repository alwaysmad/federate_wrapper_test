#include "PythonSubprocess.hpp"
#include "JsonIpcChannel.hpp"

#include <csignal>
#include <atomic>
#include <iostream>

using namespace std::chrono_literals;

static std::atomic<bool> g_shutdown_requested{false};

static inline void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        { g_shutdown_requested.store(true); }
}

int main()
{
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try {
        // 1. Generate mathematically unique abstract name for this specific instance
        const std::string unique_ipc_name = "rti_fed_" + std::to_string(getpid());

        // 2. Initialize the IPC Server
        JsonIpcChannel channel(unique_ipc_name);

        // 3. Spawn Python child, passing the unique name
        PythonSubprocess worker("worker.py", {"--ipc-name", unique_ipc_name});
        std::cout << "[C++] Spawned Python worker [PID: " << worker.get_pid() << "]\n";
        
        // 4. Accept the connection (throws if Python fails to launch/connect in 10 min)
        std::cout << "[C++] Waiting for Python worker to connect...\n";
        channel.accept_client();
        std::cout << "[C++] Connection established!\n";

        // 5. Synchronous federation loop
        int step = 0;
        while (!g_shutdown_requested.load() && step < 5)
        {
            if (!worker.is_running())
                { throw std::runtime_error("Python worker died prematurely during simulation step " + std::to_string(step)); }
            
            nlohmann::json cmd = {
                {"command", "federate_sync"},
                {"step", step}
            };

            channel.send(cmd);
            
            // receive() blocks until Python sends ACK or timeout occurs
            nlohmann::json ack = channel.receive();
            std::cout << "[C++] ACK: " << ack.dump() << "\n";

            step++;
            usleep(100000); // 100ms
            //std::this_thread::sleep_for(500ms);
        }
        std::cout << "[C++] Terminating Python worker [PID: " << worker.get_pid() << "]...\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "[C++] Fatal Error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[C++] Exit clean.\n";
    return 0;
}