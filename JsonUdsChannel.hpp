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

#include <nlohmann/json.hpp>

class JsonUdsChannel {
public:
    using json = nlohmann::json;

    /**
     * @brief Creates a datagram Unix Domain Socket channel.
     * @param bind_path Filesystem path where this (C++) process listens.
     * @param target_path Filesystem path where the (Python) worker listens.
     */
    JsonUdsChannel(std::string bind_path, std::string target_path)
        : bind_path_(std::move(bind_path)), target_path_(std::move(target_path))
    {
        if (bind_path_.length() >= sizeof(sockaddr_un::sun_path))
        {
            throw std::runtime_error("bind_path exceeds max UNIX socket path length (" + 
                std::to_string(sizeof(sockaddr_un::sun_path)) + ")");
        }
        if (target_path_.length() >= sizeof(sockaddr_un::sun_path))
        {
            throw std::runtime_error("target_path exceeds max UNIX socket path length (" + 
                std::to_string(sizeof(sockaddr_un::sun_path)) + ")");
        }

        // 1. Create UNIX datagram socket
        sock_fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (sock_fd_ < 0)
        {
            throw std::runtime_error("Failed to create AF_UNIX socket: " +
                std::string(strerror(errno)));
        }

        // 2. Unlink any stale socket file before binding (enforce clean state)
        ::unlink(bind_path_.c_str());

        // 3. Bind local socket endpoint
        sockaddr_un local_addr{};
        local_addr.sun_family = AF_UNIX;
        std::strncpy(local_addr.sun_path, bind_path_.c_str(), sizeof(local_addr.sun_path) - 1);

        if (bind(sock_fd_, reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr)) < 0)
        {
            std::string err = strerror(errno);
            close(sock_fd_);
            sock_fd_ = -1;
            throw std::runtime_error("Failed to bind UDS socket at " + bind_path_ + ": " + err);
        }

        // 4. Configure remote target address structure
        std::memset(&target_addr_, 0, sizeof(target_addr_));
        target_addr_.sun_family = AF_UNIX;
        std::strncpy(target_addr_.sun_path, target_path_.c_str(), sizeof(target_addr_.sun_path) - 1);
    }

    // Strict RAII cleanup: close descriptor and remove filesystem socket node
    ~JsonUdsChannel() noexcept { cleanup(); }

    // Move-only semantics
    JsonUdsChannel(const JsonUdsChannel&) = delete;
    JsonUdsChannel& operator=(const JsonUdsChannel&) = delete;

    JsonUdsChannel(JsonUdsChannel&& other) noexcept
        : sock_fd_(other.sock_fd_),
          bind_path_(std::move(other.bind_path_)),
          target_path_(std::move(other.target_path_)),
          target_addr_(other.target_addr_)
          { other.sock_fd_ = -1; }

    JsonUdsChannel& operator=(JsonUdsChannel&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            sock_fd_ = other.sock_fd_;
            bind_path_ = std::move(other.bind_path_);
            target_path_ = std::move(other.target_path_);
            target_addr_ = other.target_addr_;
            other.sock_fd_ = -1;
        }
        return *this;
    }

    /**
     * @brief Transmits a JSON payload to the target UDS path.
     */
    void send(const nlohmann::json& payload)
    {
        std::string serialized = payload.dump();

        ssize_t bytes_sent = sendto(
            sock_fd_,
            serialized.data(),
            serialized.size(),
            0,
            reinterpret_cast<const struct sockaddr*>(&target_addr_),
            sizeof(target_addr_)
        );

        if (bytes_sent < 0)
        {
            throw std::runtime_error("UDS send error to " + target_path_ +
                ": " + std::string(strerror(errno)));
        }
    }

    /**
     * @brief Listens for an incoming JSON datagram on bind_path.
     */
    nlohmann::json receive(std::optional<std::chrono::milliseconds> timeout = std::nullopt)
    {
        struct timeval tv{};
        if (timeout.has_value())
        {
            auto ms = timeout->count();
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
        }
        else { tv.tv_sec = 0; tv.tv_usec = 0; }

        if (setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        {
            throw std::runtime_error("Failed to configure SO_RCVTIMEO on UDS: " + std::string(strerror(errno)));
        }

        std::vector<char> buffer(65535);
        sockaddr_un sender_addr{};
        socklen_t sender_len = sizeof(sender_addr);

        ssize_t bytes_received = recvfrom(
            sock_fd_,
            buffer.data(),
            buffer.size() - 1,
            0,
            reinterpret_cast<struct sockaddr*>(&sender_addr),
            &sender_len
        );

        if (bytes_received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                { throw std::runtime_error("Timeout exceeded while waiting for JSON on " + bind_path_); }
            throw std::runtime_error("UDS receive error on " + bind_path_ + ": " + std::string(strerror(errno)));
        }

        buffer[bytes_received] = '\0';

        try 
            { return nlohmann::json::parse(buffer.data()); }
        catch (const nlohmann::json::parse_error& e)
            { throw std::runtime_error(std::string("Malformed JSON received: ") + e.what()); }
    }

    [[nodiscard]] const std::string& get_bind_path() const noexcept { return bind_path_; }
    [[nodiscard]] const std::string& get_target_path() const noexcept { return target_path_; }

private:
    void cleanup() noexcept {
        if (sock_fd_ >= 0)
            { close(sock_fd_); sock_fd_ = -1; }
        if (!bind_path_.empty())
            { ::unlink(bind_path_.c_str()); }
    }

    int sock_fd_{-1};
    std::string bind_path_;
    std::string target_path_;
    sockaddr_un target_addr_{};
};