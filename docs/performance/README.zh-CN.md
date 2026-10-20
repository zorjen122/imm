# 性能测试

主要入口为 [Gateway 性能测试](../../server/bench/gateway/README.md)，
可直接连接已有服务，测量以下指标：

| 指标 | 口径 |
| --- | --- |
| RTT | PING 请求到响应的应用层往返时间 |
| 直接处理 TPS | Gateway 本地处理 PING 的成功请求数/秒 |
| 转发 TPS | 好友列表查询经过 Gateway、Message 和存储的成功请求数/秒 |
| 网络 I/O 吞吐 | Gateway 协议实际收发的应用字节数/秒，包含长度前缀 |
| 链路带宽（可选） | iperf3 测得的主机间 TCP 带宽，不经过 Gateway |

## 运行

需要 Python 3.11+、Python Protobuf、`protoc` 和运行中的 Gateway。
先通过 Gate 登录获取测试用户的 `chatToken`，再执行：

```bash
read -rs IMM_BENCH_TOKEN; export IMM_BENCH_TOKEN
python3 server/bench/gateway/benchmark.py \
  --host 127.0.0.1 --port 8090 --uid 1001 \
  --seconds 30 --warmup 3 --connections 16 --pipeline 8 --workers 2 \
  --output build/gateway-bench/result.json
```

压测会创建设备记录并占用连接，应使用测试账号及数据库。需要测量不限流的转发能力时，
在相应 Gateway 节点下配置 `commandRateLimit: 0`；否则保留限流并记录该条件。
完整参数、iperf3 使用方式和指标计算规则见上述 Gateway 指南。

结果应同时记录机器、服务 shard 数、并发、数据规模和失败数。同机 loopback 测量只反映
本机条件，不作为生产容量；增加负载时也要检查压测端是否先达到瓶颈。

## 可选的整栈采集

`scripts/perf/run-local.py` 可启动独立拓扑，采集 Gate、Gateway、State、File 的业务延迟
和进程资源；它需要启用 `IMM_BUILD_BENCHMARKS` 构建 `immPerf`。
详细参数可运行 `python3 scripts/perf/run-local.py --help` 查看。
该工具用于需要整栈对照时的进一步测量，不是日常使用的必要步骤。
