#pragma once
// TCPServer: accepts connections on a listening socket and dispatches each
// connection's full lifecycle (read -> parse -> execute -> write, looped
// until the client disconnects) to a worker in a bounded ThreadPool.
//
// This is "thread-pool-per-connection", not epoll/io_uring based
// multiplexing. It's simpler to reason about and correct, and is more than
// sufficient for a KV store handling thousands of concurrent connections
// on modern hardware; an epoll-based reactor is noted as a natural next
// step in the README for anyone who wants to push this further.

#include <atomic>
#include <memory>
#include <string>

#include "kv_store.hpp"
#include "threadpool.hpp"

class TCPServer {
public:
    TCPServer(int port, KVStore& store, size_t num_worker_threads);
    ~TCPServer();

    // Blocks, accepting connections, until stop() is called from another
    // thread (e.g. a signal handler).
    void run();

    void stop();

private:
    void handle_connection(int client_fd);

    int port_;
    KVStore& store_;
    ThreadPool pool_;

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
};
