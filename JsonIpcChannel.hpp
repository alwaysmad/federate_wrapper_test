#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <nlohmann/json.hpp>

class JsonIpcChannel {
public:
    using json = nlohmann::json;

    /**
     * @brief Creates a SOCK_SEQPACKET channel in the Linux Abstract Namespace.
     * @param ipc_name The unique name for this connection (e.g., "rti_worker_1234")
     */
    explicit JsonIpcChannel(const std::string& ipc_name) : ipc_name_(ipc_name) {
        // 1. Create SEQPACKET socket
        server_fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (server_fd_ < 0) {
            throw std::runtime_error("Failed to create SEQPACKET socket: " + std::string(strerror(errno)));
        }

        // 2. Configure Abstract Namespace address (starts with null byte)
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0'; // The magic byte that makes it an Abstract socket
        std::strncpy(addr.sun_path + 1, ipc_name.c_str(), sizeof(addr.sun_path) - 2);

        // Linux requires the exact length of the abstract name to bind properly
        socklen_t addr_len = offsetof(sockaddr_un, sun_path) + 1 + ipc_name.length();

        // 3. Bind and Listen
        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), addr_len) < 0) {
            cleanup();
            throw std::runtime_error("Failed to bind abstract UDS: " + std::string(strerror(errno)));
        }

        if (listen(server_fd_, 1) < 0) {
            cleanup();
            throw std::runtime_error("Failed to listen on UDS: " + std::string(strerror(errno)));
        }
    }

    ~JsonIpcChannel() noexcept {
        cleanup();
    }

    // Move semantics only
    JsonIpcChannel(const JsonIpcChannel&) = delete;
    JsonIpcChannel& operator=(const JsonIpcChannel&) = delete;

    /**
     * @brief Blocks until the Python child process connects, or throws on timeout.
     */
    void accept_client(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        struct pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int poll_res = poll(&pfd, 1, timeout.count());
        if (poll_res == 0) {
            throw std::runtime_error("Timeout waiting for Python worker to connect via IPC.");
        } else if (poll_res < 0) {
            throw std::runtime_error("Poll failed: " + std::string(strerror(errno)));
        }

        client_fd_ = accept(server_fd_, nullptr, nullptr);
        if (client_fd_ < 0) {
            throw std::runtime_error("Failed to accept IPC client: " + std::string(strerror(errno)));
        }

        // Apply receive timeout directly to the connected client socket
        struct timeval tv{.tv_sec = 2, .tv_usec = 0}; // 2 second default timeout
        setsockopt(client_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    void send(const json& payload) {
        if (client_fd_ < 0) throw std::runtime_error("Cannot send: No IPC client connected.");

        std::string serialized = payload.dump();
        
        // MSG_NOSIGNAL prevents C++ from crashing with SIGPIPE if Python disconnects abruptly
        ssize_t bytes = ::send(client_fd_, serialized.data(), serialized.size(), MSG_NOSIGNAL);
        if (bytes < 0) {
            throw std::runtime_error("IPC send error: " + std::string(strerror(errno)));
        }
    }

    json receive() {
        if (client_fd_ < 0) throw std::runtime_error("Cannot receive: No IPC client connected.");

        std::vector<char> buffer(65535);
        ssize_t bytes = ::recv(client_fd_, buffer.data(), buffer.size() - 1, 0);

        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw std::runtime_error("Timeout exceeded while waiting for IPC JSON.");
            }
            throw std::runtime_error("IPC receive error: " + std::string(strerror(errno)));
        } else if (bytes == 0) {
            throw std::runtime_error("IPC connection gracefully closed by Python worker.");
        }

        buffer[bytes] = '\0';
        try {
            return json::parse(buffer.data());
        } catch (const json::parse_error& e) {
            throw std::runtime_error(std::string("Malformed JSON received: ") + e.what());
        }
    }

    [[nodiscard]] const std::string& get_ipc_name() const noexcept { return ipc_name_; }

private:
    void cleanup() noexcept {
        if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
        if (server_fd_ >= 0) { close(server_fd_); server_fd_ = -1; }
    }

    std::string ipc_name_;
    int server_fd_{-1};
    int client_fd_{-1};
};