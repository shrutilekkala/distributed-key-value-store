#include "kv_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

using Clock = std::chrono::system_clock;

KVStore::KVStore(std::string aof_path, size_t shard_count) : aof_path_(std::move(aof_path)) {
    if (shard_count == 0) shard_count = 1;
    shards_.reserve(shard_count);
    for (size_t i = 0; i < shard_count; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }

    if (!aof_path_.empty()) {
        aof_replay();
        aof_fd_ = ::open(aof_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (aof_fd_ < 0) {
            throw std::runtime_error("KVStore: failed to open AOF file: " + aof_path_ +
                                     ": " + std::strerror(errno));
        }
    }
}

KVStore::~KVStore() {
    if (aof_fd_ >= 0) {
        ::close(aof_fd_);
        aof_fd_ = -1;
    }
}

KVStore::Shard& KVStore::shard_for(const std::string& key) {
    size_t h = std::hash<std::string>{}(key);
    return *shards_[h % shards_.size()];
}

bool KVStore::is_expired(const Entry& e) {
    return e.expires_at.has_value() && Clock::now() >= *e.expires_at;
}

void KVStore::set(const std::string& key, const std::string& value,
                   std::optional<int64_t> ttl_seconds) {
    std::optional<Clock::time_point> expires_at;
    if (ttl_seconds.has_value()) {
        expires_at = Clock::now() + std::chrono::seconds(*ttl_seconds);
    }

    // Persist an absolute epoch-ms expiry (-1 == none) so replay is
    // unambiguous regardless of when it happens. The AOF write happens
    // before the in-memory mutation while the shard lock is held, preserving
    // per-key ordering and giving the log write-ahead semantics.
    int64_t abs_ms = -1;
    if (expires_at.has_value()) {
        abs_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     expires_at->time_since_epoch())
                     .count();
    }
    std::ostringstream oss;
    oss << "SET " << key.size() << ' ' << key << ' ' << value.size() << ' ' << value << ' '
        << abs_ms;

    Shard& s = shard_for(key);
    std::unique_lock lock(s.mutex);
    aof_append(oss.str());
    s.map[key] = Entry{value, expires_at};
}

std::optional<std::string> KVStore::get(const std::string& key) {
    Shard& s = shard_for(key);
    std::shared_lock lock(s.mutex);
    auto it = s.map.find(key);
    if (it == s.map.end()) return std::nullopt;
    if (is_expired(it->second)) return std::nullopt;  // lazy expiration on read
    return it->second.value;
}

bool KVStore::exists(const std::string& key) {
    return get(key).has_value();
}

bool KVStore::del(const std::string& key) {
    Shard& s = shard_for(key);
    bool existed = false;
    std::unique_lock lock(s.mutex);
    auto it = s.map.find(key);
    if (it != s.map.end() && !is_expired(it->second)) existed = true;
    if (existed) {
        std::ostringstream oss;
        oss << "DEL " << key.size() << ' ' << key;
        aof_append(oss.str());
    }
    s.map.erase(key);
    return existed;
}

bool KVStore::expire(const std::string& key, int64_t ttl_seconds) {
    Shard& s = shard_for(key);
    auto expires_at = Clock::now() + std::chrono::seconds(ttl_seconds);
    bool existed = false;
    std::unique_lock lock(s.mutex);
    auto it = s.map.find(key);
    if (it != s.map.end() && !is_expired(it->second)) existed = true;
    if (existed) {
        int64_t abs_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              expires_at.time_since_epoch())
                              .count();
        std::ostringstream oss;
        oss << "EXPIRE " << key.size() << ' ' << key << ' ' << abs_ms;
        aof_append(oss.str());
        it->second.expires_at = expires_at;
    }
    return existed;
}

size_t KVStore::size() const {
    size_t total = 0;
    for (auto& s : shards_) {
        std::shared_lock lock(s->mutex);
        for (const auto& [key, entry] : s->map) {
            (void)key;
            if (!is_expired(entry)) ++total;
        }
    }
    return total;
}

void KVStore::aof_append(const std::string& line) {
    if (aof_path_.empty()) return;
    std::lock_guard<std::mutex> lock(aof_mutex_);
    const std::string record = line + '\n';
    size_t written = 0;
    while (written < record.size()) {
        const ssize_t n = ::write(aof_fd_, record.data() + written, record.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            throw std::runtime_error("KVStore: AOF write failed: " +
                                     std::string(std::strerror(errno)));
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(aof_fd_) != 0) {
        throw std::runtime_error("KVStore: AOF fsync failed: " +
                                 std::string(std::strerror(errno)));
    }
}

// Minimal hand-rolled parser for the length-prefixed AOF format. Using
// explicit lengths (rather than delimiters) means keys/values can safely
// contain spaces or newlines.
void KVStore::aof_replay() {
    std::ifstream in(aof_path_);
    if (!in.is_open()) return;  // no existing log yet - fresh start

    constexpr size_t kMaxAofFieldBytes = 16 * 1024 * 1024;
    auto read_sized_field = [&](std::string& output) {
        size_t length = 0;
        if (!(in >> length) || length > kMaxAofFieldBytes) return false;
        if (in.get() != ' ') return false;
        output.resize(length);
        return length == 0 || static_cast<bool>(in.read(output.data(),
                                                       static_cast<std::streamsize>(length)));
    };

    std::string op;
    while (in >> op) {
        if (op == "SET") {
            std::string key;
            std::string value;
            int64_t abs_ms = -1;
            if (!read_sized_field(key) || !read_sized_field(value) || !(in >> abs_ms)) break;

            std::optional<Clock::time_point> expires_at;
            if (abs_ms >= 0) {
                expires_at = Clock::time_point(std::chrono::milliseconds(abs_ms));
            }
            Shard& s = shard_for(key);
            std::unique_lock lock(s.mutex);
            s.map[key] = Entry{value, expires_at};
        } else if (op == "DEL") {
            std::string key;
            if (!read_sized_field(key)) break;
            Shard& s = shard_for(key);
            std::unique_lock lock(s.mutex);
            s.map.erase(key);
        } else if (op == "EXPIRE") {
            std::string key;
            int64_t abs_ms = -1;
            if (!read_sized_field(key) || !(in >> abs_ms)) break;
            Shard& s = shard_for(key);
            std::unique_lock lock(s.mutex);
            auto it = s.map.find(key);
            if (it != s.map.end()) {
                it->second.expires_at = Clock::time_point(std::chrono::milliseconds(abs_ms));
            }
        } else {
            // Unknown/corrupt record - stop replay rather than risk
            // misinterpreting the rest of the file.
            break;
        }
    }
}
