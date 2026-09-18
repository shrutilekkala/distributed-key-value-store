// cppkv_client - minimal interactive CLI for talking to cppkv_server.
//
// Usage:
//   cppkv_client [--host 127.0.0.1] [--port 6380]
//
// Then type commands like:
//   SET foo bar
//   GET foo
//   DEL foo

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

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

int connect_to(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "invalid host: " << host << "\n";
        close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "connect() failed: " << std::strerror(errno) << "\n";
        close(fd);
        return -1;
    }
    return fd;
}

std::string send_command(int fd, const std::string& line) {
    std::string msg = line + "\n";
    if (!send_all(fd, msg)) return "ERROR send failed";

    std::string response;
    char chunk[1024];
    while (true) {
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return "ERROR connection closed";
        response.append(chunk, static_cast<size_t>(n));
        const size_t newline = response.find('\n');
        if (newline != std::string::npos) return response.substr(0, newline);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6380;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
    }

    int fd = connect_to(host, port);
    if (fd < 0) return 1;

    std::cout << "Connected to cppkv " << host << ":" << port << " - type 'quit' to exit.\n";

    std::string line;
    std::cout << "> ";
    while (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") break;
        if (!line.empty()) {
            std::cout << send_command(fd, line) << "\n";
        }
        std::cout << "> ";
    }

    close(fd);
    return 0;
}
