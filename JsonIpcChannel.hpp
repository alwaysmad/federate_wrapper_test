#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <cstring>
#include <cerrno>
#include <cstddef>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <nlohmann/json.hpp>

class JsonIpcChannel
{
private:
    std::string ipc_name_;
    int server_fd_{-1};
    int client_fd_{-1};

    // Reusable buffers to avoid heap fragmentation during execution loops
    mutable std::vector<char> recv_buffer_;
    mutable std::string send_buffer_;

    void cleanup() noexcept
    {
        if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
        if (server_fd_ >= 0) { close(server_fd_); server_fd_ = -1; }
    }

    static std::pair<sockaddr_un, socklen_t> make_abstract_addr(const std::string& name) noexcept
    {
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        // addr.sun_path[0] remains '\0' due to memset (Abstract Namespace)
        // ensuring addr.sun_path[0] == '\0', the magic byte that makes it an Abstract socket
        std::memcpy(addr.sun_path + 1, name.data(), name.size());

        // Linux requires the exact length of the abstract name to bind properly
        socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + name.size();
        return {addr, len};
    }

    /**
     * @brief Polls a file descriptor for incoming data with timeout support.
     * @throws std::runtime_error on timeout or poll system failure.
     */
    static void poll_fd_ready(int fd, std::chrono::milliseconds timeout, const char* error_context)
    {
        const int timeout_ms = (timeout == infinite_timeout()) ? -1 : static_cast<int>(timeout.count());
        
        struct pollfd pfd{};
        {
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
        }

        const int poll_res = poll(&pfd, 1, timeout_ms);
        if (poll_res == 0)
            { throw std::runtime_error(std::string("Timeout exceeded: ") + error_context); }
        else if (poll_res < 0)
            { throw std::runtime_error(std::string("Poll failure (") + error_context + "): " + strerror(errno)); }
    }

public:
    using json = nlohmann::json;

    // Timeout constants
    static constexpr std::chrono::milliseconds default_timeout() noexcept
        { return std::chrono::minutes(10); }
    static constexpr std::chrono::milliseconds infinite_timeout() noexcept
        { return std::chrono::milliseconds(0); }
    static constexpr size_t max_packet_size() noexcept
        { return 65536; } // 64KB, maximum for SOCK_SEQPACKET

    [[nodiscard]] inline const std::string& get_ipc_name() const noexcept { return ipc_name_; }
    [[nodiscard]] inline bool is_connected() const noexcept { return client_fd_ >= 0; }

    /**
     * @brief Creates a SOCK_SEQPACKET channel in the Linux Abstract Namespace.
     * @param ipc_name The unique name for this connection (e.g., "rti_worker_1234")
     */
    explicit JsonIpcChannel(const std::string& ipc_name) : ipc_name_(ipc_name), recv_buffer_(max_packet_size())
    {
        // 0. Validate socket name length
        if (ipc_name_.empty() || ipc_name_.length() >= sizeof(sockaddr_un::sun_path) - 1)
            { throw std::invalid_argument("Invalid abstract IPC socket name length"); }


        // 1. Create SEQPACKET socket with CLOEXEC flag
        server_fd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (server_fd_ < 0)
            { throw std::runtime_error("Failed to create SEQPACKET socket: " + std::string(strerror(errno))); }

        // 2. Configure Abstract Namespace address (starts with null byte)
        const std::pair<sockaddr_un, socklen_t> addr_pair = make_abstract_addr(ipc_name_);
        const sockaddr_un& addr = addr_pair.first;
        const socklen_t addr_len = addr_pair.second;

        // 3. Bind
        if (bind(server_fd_, reinterpret_cast<const struct sockaddr*>(&addr), addr_len) < 0)
        {
            int err = errno;
            close(server_fd_);
            server_fd_ = -1;
            throw std::runtime_error("Failed to bind abstract UDS: " + std::string(strerror(err)));
        }

        // 4. Listen
        if (listen(server_fd_, 1) < 0)
        {
            int err = errno;
            close(server_fd_);
            server_fd_ = -1;
            throw std::runtime_error("Failed to listen on UDS: " + std::string(strerror(err)));
        }

        // 5. Reserve send buffer to avoid repeated allocations during execution
        send_buffer_.reserve(max_packet_size());
    }

    ~JsonIpcChannel() noexcept { cleanup(); }

    // No copy
    JsonIpcChannel(const JsonIpcChannel&) = delete;
    JsonIpcChannel& operator=(const JsonIpcChannel&) = delete;

    // Move only
    JsonIpcChannel(JsonIpcChannel&& other) noexcept
        : ipc_name_(std::move(other.ipc_name_)),
          server_fd_(other.server_fd_),
          client_fd_(other.client_fd_),
          recv_buffer_(std::move(other.recv_buffer_)),
          send_buffer_(std::move(other.send_buffer_))
        { other.server_fd_ = -1; other.client_fd_ = -1; }
    JsonIpcChannel& operator=(JsonIpcChannel&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            ipc_name_ = std::move(other.ipc_name_);
            server_fd_ = other.server_fd_;
            client_fd_ = other.client_fd_;
            recv_buffer_ = std::move(other.recv_buffer_);
            send_buffer_ = std::move(other.send_buffer_);
            other.server_fd_ = -1;
            other.client_fd_ = -1;
        }
        return *this;
    }

    /**
    * @brief Configures OS-level socket timeout on the established connection descriptor.
    * @param timeout Duration. Passing 0ms (kInfiniteTimeout) disables the timeout.
    */
    void set_socket_timeout(std::chrono::milliseconds timeout)
    {
        // 0. Check socket status
        if (client_fd_ < 0)
            { throw std::runtime_error("Cannot set timeout: No IPC client connected."); }

        // 1. Convert timeout to timeval structure
        struct timeval tv{};
        // setting tv_sec = 0 and tv_usec = 0 explicitly disables the timeout (blocking indefinitely).
        if (timeout != infinite_timeout()) 
        {
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
            const auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(timeout - secs);
            tv.tv_sec = static_cast<time_t>(secs.count());
            tv.tv_usec = static_cast<suseconds_t>(usecs.count());
        }

        if (setsockopt(client_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            { throw std::runtime_error("Failed to configure SO_RCVTIMEO on IPC socket: " + std::string(strerror(errno))); }
    }

    /**
    * @brief Waits for the child Python worker to connect.
    */
    void accept_client(std::chrono::milliseconds timeout = default_timeout())
    {
        // 0. Check socket status
        if (server_fd_ < 0)
            { throw std::runtime_error("Server socket is closed."); }

        // 1. Wait for incoming connection with timeout via poll()
        poll_fd_ready(server_fd_, timeout, "waiting for Python worker to connect via IPC");

        // 2. Accept the connection
        client_fd_ = accept(server_fd_, nullptr, nullptr);
        if (client_fd_ < 0)
            { throw std::runtime_error("Failed to accept IPC client: " + std::string(strerror(errno))); }

        // 3. Configure receive timeout on the accepted socket
        set_socket_timeout(default_timeout());
    }

    /**
     * @brief Serializes and transmits a JSON payload. Const operation with reusable buffer.
     */
    void send(const json& payload) const
    {
        // 0. Check socket status
        if (client_fd_ < 0)
            { throw std::runtime_error("Cannot send: No IPC client connected."); }

        // 1. Serialize into pre-allocated mutable buffer
        send_buffer_.clear();
        send_buffer_ = payload.dump();
        
        // 2. Send the serialized string over the socket
        // MSG_NOSIGNAL prevents C++ from crashing with SIGPIPE if Python disconnects abruptly
        const ssize_t bytes = ::send(client_fd_, send_buffer_.data(), send_buffer_.size(), MSG_NOSIGNAL);
        if (bytes < 0) // Broken pipe
            { throw std::runtime_error("IPC send error: " + std::string(strerror(errno))); }
    }

    /**
     * @brief Receives a JSON payload.
     * @param timeout Wait duration. Passing std::chrono::milliseconds(0) blocks indefinitely.
     */
    [[nodiscard]] json receive(std::chrono::milliseconds timeout = default_timeout()) const
    {
        // 0. Check socket status
        if (client_fd_ < 0)
            { throw std::runtime_error("Cannot receive: No IPC client connected."); }

        // 1. Configure receive timeout
        poll_fd_ready(client_fd_, timeout, "waiting for IPC JSON payload");

        // 2. Receive data into buffer
        const ssize_t bytes = ::recv(client_fd_, recv_buffer_.data(), recv_buffer_.size() - 1, 0);
        if (bytes < 0)
            { throw std::runtime_error("IPC receive error: " + std::string(strerror(errno))); }
        else if (bytes == 0)
            { throw std::runtime_error("IPC connection closed by worker peer."); }

        // 3. Null-terminate and parse JSON
        recv_buffer_[bytes] = '\0';
        try
            { return json::parse(recv_buffer_.data()); } 
        catch (const json::parse_error& e)
            { throw std::runtime_error(std::string("Malformed JSON received: ") + e.what()); }
    }
};