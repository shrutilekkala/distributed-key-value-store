#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#include "protocol.hpp"

namespace {

bool send_all(int fd, const std::string& message) {
    size_t sent_total = 0;
    while (sent_total < message.size()) {
        const ssize_t sent = send(fd, message.data() + sent_total,
                                  message.size() - sent_total, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

}  // namespace

TCPServer::TCPServer(int port, KVStore& store, size_t num_worker_threads)
    : port_(port), store_(store), pool_(num_worker_threads) {}

TCPServer::~TCPServer() {
    stop();
}

void TCPServer::run() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
    }

    if (listen(listen_fd_, /*backlog=*/128) < 0) {
        throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));
    }

    running_.store(true);
    std::cout << "cppkv_server listening on port " << port_ << " with " << pool_.size()
              << " worker threads\n";

    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (!running_.load()) break;  // accept() interrupted by stop()
            continue;                      // transient error, keep serving
        }

        // Hand the whole connection lifecycle to the thread pool so the
        // accept loop never blocks on client I/O.
        pool_.submit([this, client_fd] { handle_connection(client_fd); });
    }
}

void TCPServer::stop() {
    bool was_running = running_.exchange(false);
    if (was_running && listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

void TCPServer::handle_connection(int client_fd) {
    // Nagle's algorithm off - this is a request/response protocol, and we
    // want responses flushed immediately rather than batched.
    int one = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::string buffer;
    char chunk[4096];

    while (true) {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;  // client closed or error

        buffer.append(chunk, static_cast<size_t>(n));

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            std::string response;
            try {
                response = protocol::handle_command(store_, line);
            } catch (const std::exception& error) {
                std::cerr << "request failed: " << error.what() << '\n';
                response = "ERROR storage failure";
            }
            response.push_back('\n');
            if (!send_all(client_fd, response)) {
                close(client_fd);
                return;
            }
        }
    }

    close(client_fd);
}
