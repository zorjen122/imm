# Gateway 性能测试

测试已有 Gateway，不更改服务配置或清空数据。依赖 Python 3.11+、Python `protobuf` 和 `protoc`；可选链路带宽测试还需要客户端及服务端安装 `iperf3`。压测使用专用用户，对应 Gateway 节点配置 `commandRateLimit: 0`（如 `nodes.gateway-1.commandRateLimit`），否则转发 TPS 会受单连接限流影响。测试期间没有自动重试。

## 运行

先通过 Gate 登录取得该用户的 `chatToken`，放到环境变量中，令牌不会写入命令行或结果文件：

```bash
read -rs IMM_BENCH_TOKEN; export IMM_BENCH_TOKEN
python3 server/bench/gateway/benchmark.py \
    --host 127.0.0.1 --port 8090 --uid 1001 \
    --seconds 30 --warmup 3 --connections 16 --pipeline 8 --workers 2 \
    --payload-bytes 65536 --output build/gateway-bench/result.json
```

四个阶段按顺序运行，每个阶段有自己的登录、预热和测量周期。连接使用随机 device_id，完成后关闭，避免压测连接相互替换。登录会创建设备记录，反复压测应使用测试数据库；脚本不会删除业务记录。通过 `--phases rtt direct_tps` 可以只运行指定阶段。

| 阶段 / 指标 | 测量口径 |
|---|---|
| `rtt` | 单连接、单在途 PING 的应用层往返时间，报告 mean/min/p50/p95/p99/max，单位 ms；包含客户端、TCP、Gateway 解析和响应处理，不是纯网络 RTT |
| `direct_tps` | 多连接、每连接最多 pipeline 个在途 PING；成功响应数 / 实际测量秒数，仅 Gateway 直接处理 |
| `forward_tps` | 相同并发下的好友列表查询；成功响应数 / 实际测量秒数，包含 Gateway → Message → Redis/MySQL → Gateway 链路及结果编码 |
| `io_throughput` | 使用带可调长度 request_id 的 PING，服务端原样返回 ID；测量实际 Gateway LV/protobuf 的收发字节吞吐，不增加服务端专用测试接口 |
| 各阶段 `tx_mbps / rx_mbps` | 客户端发送 / 接收的应用字节率，十进制 Mbit/s，包含 4 字节 LV 头，不含 TCP/IP 头及重传 |
| 各阶段 `io_bytes_per_second` | 上述两个方向的字节率之和，单位 B/s；这是网络 I/O，不是磁盘 IOPS |

登录和预热不计入统计。计时从测量请求发送前开始，到最后一个响应收完结束，包含最终排空时间。分位数用相对宽度约 1% 的直方图，最低桶为 1 µs；成功延迟与错误计数分开。`failed_or_unanswered` 包含服务端拒绝、协议错误、断连和超时，错误不能计入成功 TPS；失败轮次退出码为 1，并保留结果 JSON。`--timeout` 只约束压测客户端等待，不修改服务端 deadline。

测试采用固定并发闭环负载。它报告给定连接数、pipeline 和账户数据下的实测吞吐，不自动推断最大容量；好友数量也影响转发成本。Python 压测端可能成为瓶颈，因此使用独立进程，记录客户端 CPU 利用率，进程达到 85% 时给出提示。正式实验应在独立负载机运行，逐步提高 workers/连接数并重复测试；负载机与服务器同机的结果只用于验证工具和比较同条件轮次。

## 可选网络链路带宽

在 Gateway 所在主机另开端口启动 `iperf3 -s -p 5201`，然后为上面的命令增加：

```bash
--iperf-host SERVER_IP --iperf-port 5201
```

脚本在应用测试结束后分别执行正向和反向 TCP 测试，保存 `network_bandwidth.upload/download` 的 sender/receiver Mbps 及完整 iperf3 JSON。这是两台主机之间的链路基线，不经过 Gateway；不能将其标为 Gateway 业务吞吐，也不能使用 Gateway 的监听端口运行 iperf3。未指定时不运行，不虚构链路带宽数值。

## 验证工具

```bash
python3 server/bench/gateway/test_benchmark.py
python3 server/test/gateway/run_integration.py --mtls --benchmark-seconds 1 \
    --output-dir build/gateway-bench-qualification
```

前者使用 TCP fixture 对账请求数、成功 TPS、双向字节、分帧以及拒绝/断连/超时/错误关联/错误 payload，iperf3 分支使用替身检查参数与 JSON 整合。后者使用临时 MySQL、Redis/Sentinel 和真实双 Gateway/双 Message 拓扑试跑四项指标；不触碰开发数据库。启用 `IMM_BUILD_BENCHMARKS` 与 `BUILD_TESTING` 时，前者还会注册为 `perf.gateway_accounting` CTest。
