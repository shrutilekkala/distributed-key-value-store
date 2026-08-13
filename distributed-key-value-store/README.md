# distributed-key-value-store

A multithreaded, persistent key-value store server written in modern C++17 —
a tiny slice of Redis, built from raw POSIX sockets up, to demonstrate
concurrency, networking, and systems-design fundamentals.

> **Naming note:** despite the repo name, this is currently a **single-node**
> store — "distributed" here refers to the sharded, lock-partitioned
> in-memory keyspace (16 independent shards), not multi-node replication or
> consensus. True multi-node distribution (replication, leader election,
> partition tolerance) is listed under [Future Work](#future-work) as the
> natural next step.

```
SET foo bar         -> OK
GET foo             -> VALUE bar
SET session1 tok EX 60   (expires in 60s)
DEL foo              -> DELETED
```

## Why this project

Most new-grad projects are either a CRUD app or a LeetCode-style algorithm
exercise. This one is neither — it's a small, real systems project that
touches the fundamentals interviewers actually probe for backend/infra
roles: thread safety, memory ownership, network I/O, and durability
tradeoffs, all with no external dependencies beyond the C++ standard
library and POSIX sockets.

## Architecture

```
┌─────────────┐      TCP       ┌──────────────────────────────┐
│ cppkv_client │ ─────────────▶│         TCPServer             │
│ (CLI)        │                │  accept() loop (single thread)│
└─────────────┘                │            │                   │
                                │            ▼                   │
                                │      ThreadPool (N workers)     │
                                │   each owns one connection's     │
                                │   full read→parse→exec→write     │
                                │   lifecycle until disconnect      │
                                └──────────────┬───────────────────┘
                                               ▼
                                     ┌──────────────────┐
                                     │      KVStore       │
                                     │  16 shards, each    │
                                     │  guarded by a        │
                                     │  std::shared_mutex    │
                                     └─────────┬─────────────┘
                                               ▼
                                     ┌──────────────────┐
                                     │   AOF log file     │
                                     │ (fsync'd on every    │
                                     │  mutation, replayed   │
                                     │  on startup)            │
                                     └──────────────────┘
```

**Concurrency model.** The keyspace is split into 16 shards, each with its
own `std::shared_mutex`. `GET`/`EXISTS` take a shared (read) lock, so reads
on the same shard never block each other; `SET`/`DEL`/`EXPIRE` take an
exclusive lock scoped to just that shard. Unrelated keys almost never
contend at all, since they typically land in different shards. This is the
same core idea behind `ConcurrentHashMap` (Java) and Redis Cluster's
hash-slot partitioning.

**Networking model.** The accept loop lives on its own thread and never
touches client I/O directly — each accepted connection is handed to a
fixed-size `ThreadPool`, and one worker owns that connection end-to-end
(parse newline-delimited commands, execute, write response, repeat until
close). This is simpler to reason about than an epoll/reactor design and
scales fine to thousands of connections for a project like this; a
single-threaded epoll event loop is a natural "v2" extension (see
[Future Work](#future-work)).

**Persistence model (Append-Only File).** Every mutating command is
serialized with explicit length prefixes (so keys/values can safely
contain spaces) and appended to a log file, fsync'd immediately for
durability. On startup, the log is replayed from the top to rebuild the
in-memory state — the same idea as Redis's AOF persistence. The tradeoff:
the log grows unbounded and fsync-per-write caps throughput in exchange
for "no data loss on crash." Both are called out explicitly below.

## Protocol

A deliberately simple newline-delimited text protocol — no framing
library needed, and you can talk to the server by hand with `nc`:

| Command | Response |
|---|---|
| `SET <key> <value>` | `OK` |
| `SET <key> <value> EX <seconds>` | `OK` (with TTL) |
| `GET <key>` | `VALUE <value>` or `NOT_FOUND` |
| `DEL <key>` | `DELETED` or `NOT_FOUND` |
| `EXISTS <key>` | `TRUE` or `FALSE` |
| `EXPIRE <key> <seconds>` | `OK` or `NOT_FOUND` |
| `PING` | `PONG` |

## Build & run

Requires a C++17 compiler and CMake ≥ 3.16. No third-party dependencies.

```bash
git clone https://github.com/<you>/cppkv.git
cd cppkv
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Start the server (default port 6380, AOF at ./cppkv.aof)
./build/cppkv_server

# In another terminal — interactive CLI client
./build/cppkv_client
> SET foo bar
OK
> GET foo
VALUE bar

# Run the test suite
./build/cppkv_tests

# Run the load-test benchmark
./build/cppkv_bench --clients 16 --ops 3000
```

Server flags: `--port <n>` `--threads <n>` `--aof <path>` `--no-aof`

## Benchmark results

Measured locally (16 concurrent clients, 3,000 `SET`+`GET` pairs each,
96,000 total ops, durable mode with `fsync` on every write):

```
Completed 96000 ops (0 errors) in 1.47s
Throughput: ~65,200 ops/sec
```

This is with full durability on (every mutation fsync'd before
acknowledging) — the honest, non-cherry-picked number. Disabling AOF
(`--no-aof`) or batching fsyncs would push this significantly higher; see
[Future Work](#future-work).

## Testing

`tests/test_kv_store.cpp` is a small, dependency-free test harness (no
GTest/Catch2 needed to build) covering:
- basic get/set/overwrite/delete/exists
- TTL expiry (lazy, on read)
- 8 threads × 2,000 concurrent writes → asserts final size is exact (no
  lost updates, validating the sharded locking)
- full AOF persistence round-trip: write, close, reopen, verify state and
  deletions survived

```
21/21 checks passed
```

CI (`.github/workflows/ci.yml`) builds and runs the full suite plus an
end-to-end server/client smoke test on every push.

## Design decisions & tradeoffs

- **Thread-pool-per-connection vs. epoll reactor.** Chose the simpler
  model for correctness and readability within project scope. Noted as
  the clearest next step for anyone extending this toward
  production-grade connection scaling (10K+ concurrent idle connections).
- **fsync on every write.** Maximizes durability, costs throughput.
  Redis itself defaults to `fsync` once per second for exactly this
  reason — a configurable flush policy is a natural extension.
- **Length-prefixed AOF encoding** instead of delimiter-based, so keys/
  values can contain arbitrary bytes (spaces, newlines) without escaping
  logic or corrupting the log.
- **Lazy TTL expiration** (checked on read) rather than a background
  reaper thread — simpler, no risk of a sweep thread contending with hot
  shards. A background sweeper is a reasonable addition for
  memory-bounded workloads with many expired-but-unread keys.

## Future work

- Epoll/reactor-based event loop instead of thread-per-connection
- AOF compaction (periodic snapshot + log truncation, like Redis `BGREWRITEAOF`)
- Binary protocol (RESP-style) instead of text, for lower parsing overhead
- Replication (leader/follower) for availability
- Range queries / simple secondary indexing

## Project layout

```
cppkv/
├── include/           # public headers (kv_store, threadpool, server, protocol)
├── src/                # implementations + server main
├── client/             # interactive CLI client
├── benchmark/          # multithreaded load generator
├── tests/               # dependency-free unit tests
├── .github/workflows/    # CI: build + test on every push
└── CMakeLists.txt
```

## License

MIT — see [LICENSE](LICENSE).
