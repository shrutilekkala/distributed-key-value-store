// cppkv_server - entry point.
//
// Usage:
//   cppkv_server [--port 6380] [--threads 8] [--aof cppkv.aof]
//
// Ctrl+C (SIGINT) triggers a clean shutdown.

#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

#include "kv_store.hpp"
#include "server.hpp"

namespace {
TCPServer* g_server = nullptr;

void handle_sigint(int) {
    if (g_server) g_server->stop();
}
}  // namespace

int main(int argc, char** argv) {
    int port = 6380;
    size_t threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;
    std::string aof_path = "cppkv.aof";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--port") {
            port = std::stoi(next("--port"));
        } else if (arg == "--threads") {
            threads = static_cast<size_t>(std::stoi(next("--threads")));
        } else if (arg == "--aof") {
            aof_path = next("--aof");
        } else if (arg == "--no-aof") {
            aof_path.clear();
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: cppkv_server [--port 6380] [--threads N] "
                         "[--aof path] [--no-aof]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    std::cout << "Loading store" << (aof_path.empty() ? " (persistence disabled)" : "")
              << (aof_path.empty() ? "" : (" from " + aof_path)) << "...\n";
    KVStore store(aof_path);
    std::cout << "Loaded " << store.size() << " keys.\n";

    TCPServer server(port, store, threads);
    g_server = &server;
    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    server.run();

    std::cout << "\nShutting down.\n";
    return 0;
}
