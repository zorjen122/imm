# IMM

An experimental instant-messaging server built with C++23, Seastar and gRPC.

English · [简体中文](README.zh-CN.md)

IMM provides account registration and sign-in, direct and group messaging,
per-device sessions, delivery acknowledgements, incremental synchronization,
and chunked file transfer.

## Background

An IM service maintains many connections while handling authentication, message
storage, routing and online delivery. These operations combine complex business
rules with time spent waiting for networks and databases. IMM aims to express
that work clearly while letting a multicore machine spend more time processing
requests.

A common multithreaded design shares connection pools, task queues and online
state between workers. Sharing simplifies central management, but introduces
coordination costs: data access needs synchronization, cache lines can move
between CPU cores, and tasks require handoffs. Atomic operations and lock-free
structures can still incur cross-core sharing costs. Those costs matter more
as concurrency grows.

Gate, Gateway and Message use Seastar to keep resources local to each shard.
Each shard manages its connections, protocol state and in-flight requests on
its own reactor, communicating explicitly when work crosses shard boundaries.
Keeping frequently accessed data and its execution on the owning core reduces
dependence on shared locks, shared queues and cross-core coordination.

Each reactor advances many requests. When a request starts a Redis, MySQL, SMTP
or gRPC operation, its coroutine suspends and the reactor runs other ready tasks.
Completion resumes business processing on the original shard. C++ coroutines
keep reception, validation, storage and response handling in business order,
without dedicating a worker thread to each waiting request.

This execution model also calls for clear ownership and responsibilities:

- The protocol layer parses, validates and dispatches; transactions express
  business steps; I/O drivers advance dependencies and deliver results.
  `protocol`, `transaction` and `io` scheduling groups assign CPU scheduling
  shares to these categories.
- Counts and byte budgets bound in-flight requests, queued operations and
  pending sends. Excess work is rejected explicitly, limiting admitted work
  before backlogs keep increasing memory use and waiting time.
- Shards create and close their own connections. Each Message shard holds its
  MySQL connection for a complete transaction to prevent statement interleaving;
  cross-shard delivery explicitly pays the cost of communication.

Reducing sharing lowers the coordination work involved in concurrent processing.
Clear ownership and limits make the system easier to control under load.
Throughput and latency still depend on the database, shard count and workload;
use the [Gateway performance tests](server/bench/gateway/README.md) to measure
those properties in a specific environment.

## Run locally

Install the [server dependencies](docs/requirements.zh-CN.md), including Seastar
and a C++23 compiler, then run from the repository root:

```bash
./scripts/build-server.sh
./scripts/init-mysql.sh
./scripts/run-local-services.sh
```

`init-mysql.sh` resets local test tables and seed data. The launcher starts local
Redis/Sentinel when needed; MySQL must already be running.

Local configuration lives in `loopback/`: `single.yaml` defines shared
dependencies and one instance of each service; `cluster.yaml` adds a second
Gateway and Message. Use `./scripts/run-local-services.sh --cluster` for that
topology. Default client entry points are Gate at `127.0.0.1:18080` and Gateway
at `127.0.0.1:8090`.

## Verify

```bash
ctest --test-dir build/imm-server --output-on-failure
./scripts/smoke/smoke-base.sh
```

The smoke command requires the local services above. With the cluster running,
use `./scripts/smoke/smoke-multi-chat.sh` to verify cross-node messaging.

## Guides

The following guides are currently in Chinese:

- [Server setup](docs/requirements.zh-CN.md)
- [Configuration](docs/configuration.zh-CN.md)
- [Gateway performance tests](server/bench/gateway/README.md): bandwidth, RTT, network I/O throughput and direct/forwarded TPS
- [Server capabilities and verification](docs/server-feature-verification.zh-CN.md)
