#include "Redis.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace
{

int ReadProcessId(const std::string& info)
{
    static const std::string prefix = "process_id:";
    const auto begin = info.find(prefix);
    if (begin == std::string::npos)
        return -1;
    const auto valueBegin = begin + prefix.size();
    const auto valueEnd = info.find_first_of("\r\n", valueBegin);
    return std::stoi(info.substr(valueBegin, valueEnd - valueBegin));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: redisSentinelFailoverTest <key>\n";
        return 2;
    }

    sw::redis::SentinelOptions sentinelOptions;
    sentinelOptions.nodes.emplace_back("127.0.0.1", 26389);
    sentinelOptions.nodes.emplace_back("127.0.0.1", 26390);
    sentinelOptions.nodes.emplace_back("127.0.0.1", 26391);
    sentinelOptions.password = "root";
    sentinelOptions.connect_timeout = 1s;
    sentinelOptions.socket_timeout = 1s;
    auto sentinel = std::make_shared<sw::redis::Sentinel>(sentinelOptions);

    wimi::db::RedisPool pool(std::move(sentinel), "wimi-master", "root", 1);
    auto redis = pool.GetConnectionUntil(wimi::RequestContext::Clock::now() + 1s);
    if (!redis) {
        std::cerr << "Cannot connect to the Redis master through Sentinel.\n";
        return 1;
    }

    const std::string key = argv[1];
    redis->set(key, "before-failover");
    const int masterPid = ReadProcessId(redis->info("server"));
    if (masterPid <= 0 || kill(masterPid, SIGTERM) != 0) {
        std::cerr << "Cannot stop the current Redis master.\n";
        return 1;
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        try {
            redis->set(key, "after-failover");
            if (redis->get(key).value_or("") == "after-failover") {
                std::cout << "redis++ reconnected to the promoted master.\n";
                return 0;
            }
        } catch (const sw::redis::Error&) {
        }
        std::this_thread::sleep_for(200ms);
    }

    std::cerr << "redis++ did not reconnect after Sentinel failover.\n";
    return 1;
}
