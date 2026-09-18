// Minimal, dependency-free test harness for KVStore.
//
// Deliberately not using a third-party framework (Catch2/GTest) so the
// project builds with nothing but a C++17 compiler + CMake - one less
// thing that can go wrong when someone clones this fresh.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "kv_store.hpp"
#include "protocol.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                         \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "  FAIL: %s (line %d)\n", #cond, __LINE__); \
        }                                                                     \
    } while (0)

void run(const char* name, void (*fn)()) {
    std::fprintf(stderr, "[ RUN  ] %s\n", name);
    fn();
    std::fprintf(stderr, "[ DONE ] %s\n", name);
}

void test_basic_set_get() {
    KVStore store;  // no AOF - pure in-memory
    store.set("foo", "bar");
    auto v = store.get("foo");
    CHECK(v.has_value());
    CHECK(v.value_or("") == "bar");
    CHECK(store.get("missing") == std::nullopt);
}

void test_overwrite() {
    KVStore store;
    store.set("k", "v1");
    store.set("k", "v2");
    CHECK(store.get("k").value_or("") == "v2");
}

void test_delete() {
    KVStore store;
    store.set("k", "v");
    CHECK(store.del("k") == true);
    CHECK(store.get("k") == std::nullopt);
    CHECK(store.del("k") == false);  // already gone
}

void test_exists() {
    KVStore store;
    CHECK(store.exists("k") == false);
    store.set("k", "v");
    CHECK(store.exists("k") == true);
}

void test_ttl_expiry() {
    KVStore store;
    store.set("k", "v", /*ttl_seconds=*/1);
    CHECK(store.get("k").has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(store.get("k") == std::nullopt);  // lazily expired on read
    CHECK(store.size() == 0);
}

void test_expire_command() {
    KVStore store;
    CHECK(store.expire("nope", 10) == false);
    store.set("k", "v");
    CHECK(store.expire("k", 10) == true);
}

void test_size() {
    KVStore store;
    CHECK(store.size() == 0);
    store.set("a", "1");
    store.set("b", "2");
    CHECK(store.size() == 2);
    store.del("a");
    CHECK(store.size() == 1);
}

void test_concurrent_writes() {
    KVStore store;
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 2000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                store.set("t" + std::to_string(t) + ":" + std::to_string(i), "v");
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(store.size() == static_cast<size_t>(kThreads * kOpsPerThread));
}

void test_aof_persistence_roundtrip() {
    const std::string path = "test_cppkv.aof";
    std::filesystem::remove(path);

    {
        KVStore store(path);
        store.set("persisted", "value");
        store.set("with_ttl", "v", /*ttl_seconds=*/3600);
        store.set("to_delete", "temp");
        store.del("to_delete");
    }  // store destructs, AOF flushed + closed

    {
        KVStore reloaded(path);  // replays the AOF
        CHECK(reloaded.get("persisted").value_or("") == "value");
        CHECK(reloaded.get("with_ttl").value_or("") == "v");
        CHECK(reloaded.get("to_delete") == std::nullopt);
        CHECK(reloaded.size() == 2);
    }

    std::filesystem::remove(path);
}

void test_aof_binary_safe_roundtrip() {
    const std::string path = "test_cppkv_special.aof";
    std::filesystem::remove(path);

    {
        KVStore store(path);
        store.set("key with spaces", "value with spaces\nand a newline");
    }

    {
        KVStore reloaded(path);
        CHECK(reloaded.get("key with spaces").value_or("") ==
              "value with spaces\nand a newline");
    }

    std::filesystem::remove(path);
}

void test_protocol_validation() {
    KVStore store;
    CHECK(protocol::handle_command(store, "PING") == "PONG");
    CHECK(protocol::handle_command(store, "SET key value") == "OK");
    CHECK(protocol::handle_command(store, "GET key") == "VALUE value");
    CHECK(protocol::handle_command(store, "SET key value EX 0") ==
          "ERROR TTL must be positive");
    CHECK(protocol::handle_command(store, "EXPIRE key -1") ==
          "ERROR TTL must be positive");
    CHECK(protocol::handle_command(store, "UNKNOWN") ==
          "ERROR unknown command 'UNKNOWN'");
}

void test_truncated_aof_tail_is_ignored() {
    const std::string path = "test_cppkv_truncated.aof";
    std::filesystem::remove(path);

    {
        KVStore store(path);
        store.set("stable", "value");
    }
    {
        std::ofstream out(path, std::ios::app);
        out << "SET 8 partial";
    }
    {
        KVStore reloaded(path);
        CHECK(reloaded.get("stable").value_or("") == "value");
        CHECK(reloaded.get("partial") == std::nullopt);
    }

    std::filesystem::remove(path);
}

}  // namespace

int main() {
    run("basic_set_get", test_basic_set_get);
    run("overwrite", test_overwrite);
    run("delete", test_delete);
    run("exists", test_exists);
    run("ttl_expiry", test_ttl_expiry);
    run("expire_command", test_expire_command);
    run("size", test_size);
    run("concurrent_writes", test_concurrent_writes);
    run("aof_persistence_roundtrip", test_aof_persistence_roundtrip);
    run("aof_binary_safe_roundtrip", test_aof_binary_safe_roundtrip);
    run("protocol_validation", test_protocol_validation);
    run("truncated_aof_tail_is_ignored", test_truncated_aof_tail_is_ignored);

    std::fprintf(stderr, "\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
