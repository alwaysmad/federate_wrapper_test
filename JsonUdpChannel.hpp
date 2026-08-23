#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

class JsonUdpChannel {
public:
    // Convenience type alias
    using json = nlohmann::json;

    /**
     * @brief Constructs a dual-port UDP communication channel.
     * @param bind_port Local port to bind and listen on.
     * @param target_port Remote port to send packets to.
     * @param bind_ip Local interface address to bind to.
     * @param target_ip Remote destination IP address.
     */
    JsonUdpChannel( uint16_t bind_port, 
                    uint16_t target_port,
                    const std::string& bind_ip = "127.0.0.1",
                    const std::string& target_ip = "127.0.0.1" )
        : bind_port_(bind_port), target_port_(target_port) {
        
        // 1. Create UDP socket
        sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_fd_ < 0)
            { throw std::runtime_error("Failed to create UDP socket: " + std::string(strerror(errno))); }

        // 2. Allow port reuse to prevent bind issues on fast restarts
        int optval = 1;
        if (setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
            close(sock_fd_);
            throw std::runtime_error("Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
        }

        // 3. Bind local address and port for receiving
        sockaddr_in local_addr{};
        std::memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(bind_port_);
        
        if (inet_pton(AF_INET, bind_ip.c_str(), &local_addr.sin_addr) <= 0) {
            close(sock_fd_);
            throw std::runtime_error("Invalid bind IP address supplied: " + bind_ip);
        }

        if (bind(sock_fd_, reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
            std::string err = strerror(errno);
            close(sock_fd_);
            throw std::runtime_error("Failed to bind UDP socket to port " + std::to_string(bind_port_) + ": " + err);
        }

        // 4. Setup target address structure for sending
        std::memset(&target_addr_, 0, sizeof(target_addr_));
        target_addr_.sin_family = AF_INET;
        target_addr_.sin_port = htons(target_port_);
        
        if (inet_pton(AF_INET, target_ip.c_str(), &target_addr_.sin_addr) <= 0) {
            close(sock_fd_);
            throw std::runtime_error("Invalid target IP address supplied: " + target_ip);
        }
    }

    ~JsonUdpChannel() noexcept {
        if (sock_fd_ >= 0) {
            close(sock_fd_);
        }
    }

    // Move-only semantics (RAII socket descriptor safety)
    JsonUdpChannel(const JsonUdpChannel&) = delete;
    JsonUdpChannel& operator=(const JsonUdpChannel&) = delete;

    JsonUdpChannel(JsonUdpChannel&& other) noexcept 
        : sock_fd_(other.sock_fd_), 
          bind_port_(other.bind_port_),
          target_port_(other.target_port_), 
          target_addr_(other.target_addr_)
          { other.sock_fd_ = -1; }

    JsonUdpChannel& operator=(JsonUdpChannel&& other) noexcept {
        if (this != &other) {
            if (sock_fd_ >= 0) close(sock_fd_);
            sock_fd_ = other.sock_fd_;
            bind_port_ = other.bind_port_;
            target_port_ = other.target_port_;
            target_addr_ = other.target_addr_;
            other.sock_fd_ = -1;
        }
        return *this;
    }

    /**
     * @brief Serializes and transmits a JSON payload to target_port.
     */
    void send(const nlohmann::json& payload) {
        std::string serialized = payload.dump();

        ssize_t bytes_sent = sendto(
            sock_fd_,
            serialized.data(),
            serialized.size(),
            0,
            reinterpret_cast<const struct sockaddr*>(&target_addr_),
            sizeof(target_addr_)
        );

        if (bytes_sent < 0) {
            throw std::runtime_error("UDP send error: " + std::string(strerror(errno)));
        }
    }

    /**
     * @brief Listens on bind_port for an incoming JSON payload.
     * @param timeout Optional duration. If nullopt, blocks indefinitely.
     */
    nlohmann::json receive(std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
        struct timeval tv{};
        if (timeout.has_value()) {
            auto ms = timeout->count();
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
        } else {
            tv.tv_sec = 0;
            tv.tv_usec = 0;
        }

        if (setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            throw std::runtime_error("Failed to configure SO_RCVTIMEO: " + std::string(strerror(errno)));
        }

        std::vector<char> buffer(65535);
        sockaddr_in sender_addr{};
        socklen_t sender_len = sizeof(sender_addr);

        ssize_t bytes_received = recvfrom(
            sock_fd_,
            buffer.data(),
            buffer.size() - 1,
            0,
            reinterpret_cast<struct sockaddr*>(&sender_addr),
            &sender_len
        );

        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw std::runtime_error("Timeout exceeded while waiting for JSON response.");
            }
            throw std::runtime_error("UDP receive error: " + std::string(strerror(errno)));
        }

        buffer[bytes_received] = '\0';

        try {
            return nlohmann::json::parse(buffer.data());
        } catch (const nlohmann::json::parse_error& e) {
            throw std::runtime_error(std::string("Malformed JSON received: ") + e.what());
        }
    }

    [[nodiscard]] uint16_t get_bind_port() const noexcept { return bind_port_; }
    [[nodiscard]] uint16_t get_target_port() const noexcept { return target_port_; }

private:
    int sock_fd_{-1};
    uint16_t bind_port_{0};
    uint16_t target_port_{0};
    sockaddr_in target_addr_{};
};