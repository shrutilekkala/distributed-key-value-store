#include "kv_store.hpp"

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
        // Open in append mode for subsequent writes.
        aof_out_.open(aof_path_, std::ios::out | std::ios::app);
        if (!aof_out_.is_open()) {
            throw std::runtime_error("KVStore: failed to open AOF file: " + aof_path_);
        }
    }
}

KVStore::~KVStore() {
    if (aof_out_.is_open()) {
        aof_out_.flush();
        aof_out_.close();
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

    Shard& s = shard_for(key);
    {
        std::unique_lock lock(s.mutex);
        s.map[key] = Entry{value, expires_at};
    }

    // Persist as an absolute epoch-ms expiry (-1 == none) so replay is
    // unambiguous regardless of when it happens.
    int64_t abs_ms = -1;
    if (expires_at.has_value()) {
        abs_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     expires_at->time_since_epoch())
                     .count();
    }
    std::ostringstream oss;
    oss << "SET " << key.size() << ' ' << key << ' ' << value.size() << ' ' << value << ' '
        << abs_ms;
    aof_append(oss.str());
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
    {
        std::unique_lock lock(s.mutex);
        auto it = s.map.find(key);
        if (it != s.map.end() && !is_expired(it->second)) existed = true;
        s.map.erase(key);
    }
    if (existed) {
        std::ostringstream oss;
        oss << "DEL " << key.size() << ' ' << key;
        aof_append(oss.str());
    }
    return existed;
}

bool KVStore::expire(const std::string& key, int64_t ttl_seconds) {
    Shard& s = shard_for(key);
    auto expires_at = Clock::now() + std::chrono::seconds(ttl_seconds);
    bool existed = false;
    {
        std::unique_lock lock(s.mutex);
        auto it = s.map.find(key);
        if (it != s.map.end() && !is_expired(it->second)) {
            it->second.expires_at = expires_at;
            existed = true;
        }
    }
    if (existed) {
        int64_t abs_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              expires_at.time_since_epoch())
                              .count();
        std::ostringstream oss;
        oss << "EXPIRE " << key.size() << ' ' << key << ' ' << abs_ms;
        aof_append(oss.str());
    }
    return existed;
}

size_t KVStore::size() const {
    size_t total = 0;
    for (auto& s : shards_) {
        std::shared_lock lock(s->mutex);
        total += s->map.size();
    }
    return total;
}

void KVStore::aof_append(const std::string& line) {
    if (aof_path_.empty()) return;
    std::lock_guard<std::mutex> lock(aof_mutex_);
    aof_out_ << line << '\n';
    aof_out_.flush();  // durability over raw throughput; see README tradeoffs
}

// Minimal hand-rolled parser for the length-prefixed AOF format. Using
// explicit lengths (rather than delimiters) means keys/values can safely
// contain spaces or newlines.
void KVStore::aof_replay() {
    std::ifstream in(aof_path_);
    if (!in.is_open()) return;  // no existing log yet - fresh start

    std::string op;
    while (in >> op) {
        if (op == "SET") {
            size_t klen, vlen;
            int64_t abs_ms;
            in >> klen;
            in.get();  // consume single space
            std::string key(klen, '\0');
            in.read(&key[0], static_cast<std::streamsize>(klen));
            in >> vlen;
            in.get();
            std::string value(vlen, '\0');
            in.read(&value[0], static_cast<std::streamsize>(vlen));
            in >> abs_ms;

            std::optional<Clock::time_point> expires_at;
            if (abs_ms >= 0) {
                expires_at = Clock::time_point(std::chrono::milliseconds(abs_ms));
            }
            Shard& s = shard_for(key);
            std::unique_lock lock(s.mutex);
            s.map[key] = Entry{value, expires_at};
        } else if (op == "DEL") {
            size_t klen;
            in >> klen;
            in.get();
            std::string key(klen, '\0');
            in.read(&key[0], static_cast<std::streamsize>(klen));
            Shard& s = shard_for(key);
            std::unique_lock lock(s.mutex);
            s.map.erase(key);
        } else if (op == "EXPIRE") {
            size_t klen;
            int64_t abs_ms;
            in >> klen;
            in.get();
            std::string key(klen, '\0');
            in.read(&key[0], static_cast<std::streamsize>(klen));
            in >> abs_ms;
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
