// cppkv_bench - simple multithreaded load generator.
//
// Spawns N client threads, each performing M SET+GET pairs against the
// server, and reports completed operations, errors, elapsed time, and throughput.
//
// Usage:
//   cppkv_bench [--host 127.0.0.1] [--port 6380] [--clients 8] [--ops 5000]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

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
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool roundtrip(int fd, const std::string& cmd) {
    std::string msg = cmd + "\n";
    if (!send_all(fd, msg)) return false;
    char ch;
    while (true) {
        const ssize_t n = recv(fd, &ch, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        if (ch == '\n') return true;
    }
}

void worker(const std::string& host, int port, int ops_per_client, int client_id,
            std::atomic<long long>& completed, std::atomic<long long>& errors) {
    int fd = connect_to(host, port);
    if (fd < 0) {
        errors += ops_per_client;
        return;
    }

    for (int i = 0; i < ops_per_client; ++i) {
        std::string key = "bench:" + std::to_string(client_id) + ":" + std::to_string(i);
        bool ok = roundtrip(fd, "SET " + key + " value" + std::to_string(i));
        ok = ok && roundtrip(fd, "GET " + key);
        if (ok) {
            completed += 2;
        } else {
            errors += 2;
        }
    }

    close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6380;
    int clients = 8;
    int ops = 5000;  // total SET+GET pairs per client

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--clients" && i + 1 < argc) {
            clients = std::stoi(argv[++i]);
        } else if (arg == "--ops" && i + 1 < argc) {
            ops = std::stoi(argv[++i]);
        }
    }

    std::cout << "Benchmarking " << clients << " clients x " << ops
              << " SET+GET pairs each...\n";

    std::atomic<long long> completed{0};
    std::atomic<long long> errors{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(clients));
    for (int c = 0; c < clients; ++c) {
        threads.emplace_back(worker, host, port, ops, c, std::ref(completed), std::ref(errors));
    }
    for (auto& t : threads) t.join();

    auto end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();

    std::cout << "Completed " << completed.load() << " ops (" << errors.load()
              << " errors) in " << secs << "s\n";
    std::cout << "Throughput: " << static_cast<double>(completed.load()) / secs << " ops/sec\n";

    return 0;
}
