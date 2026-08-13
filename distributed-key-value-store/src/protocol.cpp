#include "protocol.hpp"

#include <sstream>
#include <vector>

namespace protocol {

namespace {

std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);
    return tokens;
}

// SET's value may legitimately contain no spaces (simplification of this
// protocol - use a quoted/binary-safe protocol for real payloads). We
// still support trailing `EX <secs>` for TTL.
std::string handle_set(KVStore& store, const std::vector<std::string>& t) {
    if (t.size() != 3 && t.size() != 5) {
        return "ERROR usage: SET <key> <value> [EX <seconds>]";
    }
    const std::string& key = t[1];
    const std::string& value = t[2];

    std::optional<int64_t> ttl;
    if (t.size() == 5) {
        if (t[3] != "EX") return "ERROR usage: SET <key> <value> [EX <seconds>]";
        try {
            ttl = std::stoll(t[4]);
        } catch (...) {
            return "ERROR invalid TTL";
        }
    }
    store.set(key, value, ttl);
    return "OK";
}

}  // namespace

std::string handle_command(KVStore& store, const std::string& line) {
    auto t = tokenize(line);
    if (t.empty()) return "ERROR empty command";

    const std::string& cmd = t[0];

    if (cmd == "PING") {
        return "PONG";
    } else if (cmd == "SET") {
        return handle_set(store, t);
    } else if (cmd == "GET") {
        if (t.size() != 2) return "ERROR usage: GET <key>";
        auto v = store.get(t[1]);
        return v.has_value() ? ("VALUE " + *v) : "NOT_FOUND";
    } else if (cmd == "DEL") {
        if (t.size() != 2) return "ERROR usage: DEL <key>";
        return store.del(t[1]) ? "DELETED" : "NOT_FOUND";
    } else if (cmd == "EXISTS") {
        if (t.size() != 2) return "ERROR usage: EXISTS <key>";
        return store.exists(t[1]) ? "TRUE" : "FALSE";
    } else if (cmd == "EXPIRE") {
        if (t.size() != 3) return "ERROR usage: EXPIRE <key> <seconds>";
        try {
            int64_t secs = std::stoll(t[2]);
            return store.expire(t[1], secs) ? "OK" : "NOT_FOUND";
        } catch (...) {
            return "ERROR invalid TTL";
        }
    }

    return "ERROR unknown command '" + cmd + "'";
}

}  // namespace protocol
