#pragma once
#include "reactor/fd_watch.hh"
#include <cstdint>
#include <hiredis/async.h>
#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace imm::io
{

struct redis_reply {
    int type{};
    long long integer{};
    std::string text;
    std::vector<std::string> elements;
};

// Drives hiredis on the creating shard. Reply callbacks copy borrowed hiredis
// data into redis_reply; business promises and dispatch belong to operation_queue.
// close() must settle before destruction, including after failed connect/abort.
class redis_connection
{
    redisAsyncContext* _context{};
    std::shared_ptr<fd_watch> _watch;
    std::optional<seastar::future<>> _stopped;
    seastar::promise<> _connected;
    seastar::timer<> _timer;
    bool _connecting{};
    unsigned _owner;
    static void connected(const redisAsyncContext*, int);
    static void cleanup(void*);
    static void completed(redisAsyncContext*, void*, void*);

  public:
    redis_connection();
    ~redis_connection();
    redis_connection(const redis_connection&) = delete;
    redis_connection& operator=(const redis_connection&) = delete;
    seastar::future<> connect(std::string numeric_ip, uint16_t port);
    seastar::future<redis_reply> command(std::vector<std::string> arguments);
    void abort();
    seastar::future<> close();
};
} // namespace imm::io
