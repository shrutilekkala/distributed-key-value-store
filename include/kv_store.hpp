#pragma once
// KVStore: an in-memory, thread-safe key-value store with optional TTL
// and crash-recoverable persistence via an append-only file (AOF).
//
// Concurrency model:
//  - The keyspace is split into N shards (default 16). Each shard has its
//    own std::shared_mutex, so unrelated keys almost never contend, and
//    reads within a shard (GET/EXISTS) can proceed concurrently.
//  - This is the same idea used by java.util.concurrent's
//    ConcurrentHashMap and by Redis Cluster's hash-slot sharding, just
//    single-process here.
//
// Persistence model (Append-Only File, same idea as Redis AOF):
//  - Every mutating command (SET/DEL/EXPIRE) is serialized and appended
//    to a log file *after* the in-memory mutation succeeds.
//  - On startup, the log is replayed from the beginning to rebuild state.
//  - This trades log size for simplicity; a production system would add
//    periodic log compaction (snapshot + truncate), noted as future work
//    in the README.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class KVStore {
public:
    // aof_path: if non-empty, enables persistence. Existing log (if any)
    // is replayed synchronously in the constructor.
    explicit KVStore(std::string aof_path = "", size_t shard_count = 16);
    ~KVStore();

    // Returns false only if the key existed but was expired (informational).
    void set(const std::string& key, const std::string& value,
              std::optional<int64_t> ttl_seconds = std::nullopt);

    std::optional<std::string> get(const std::string& key);

    bool exists(const std::string& key);

    // Returns true if the key existed (and was removed).
    bool del(const std::string& key);

    // Set/refresh a TTL on an existing key. Returns false if key absent.
    bool expire(const std::string& key, int64_t ttl_seconds);

    size_t size() const;

    // Number of shards - exposed for tests/metrics.
    size_t shard_count() const { return shards_.size(); }

private:
    struct Entry {
        std::string value;
        // nullopt == no expiration. Uses system_clock (wall time) rather
        // than steady_clock so that absolute expiry timestamps remain
        // meaningful when replayed from the AOF after a process restart.
        std::optional<std::chrono::system_clock::time_point> expires_at;
    };

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, Entry> map;
    };

    Shard& shard_for(const std::string& key);
    static bool is_expired(const Entry& e);

    // --- AOF ---
    void aof_append(const std::string& line);
    void aof_replay();

    std::vector<std::unique_ptr<Shard>> shards_;

    std::string aof_path_;
    int aof_fd_ = -1;
    std::mutex aof_mutex_;  // serializes appends across shards
};
