# 服务端性能测试入口

短测用于验证协议、业务对账、采集链路和停机，不代表可持续容量。运行器会创建独立 MySQL schema、Redis/Sentinel、双 Message 和双 Gateway 拓扑，并在结束时清理。

Gate 和 Gateway 固定使用 Seastar 的 `io_uring` reactor；运行器会显式传入 `--reactor-backend io_uring`，并在 `manifest.json` 中记录选择。运行日志中的 `Reactor backend: io_uring` 是实际生效的验收证据。

```bash
cmake -S server -B build/perf-cxx23 -DCMAKE_BUILD_TYPE=Release \
  -DWIMI_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON
cmake --build build/perf-cxx23 -j2
python3 scripts/perf/run-local.py \
  --build build/perf-cxx23 \
  --output build/perf-results/seastar-cxx23 \
  --seconds 10 --rate 40 --workers 4 --seastar-smp 1
python3 scripts/perf/report.py build/perf-results/seastar-cxx23 \
  > build/perf-results/seastar-cxx23/report.json
```

运行器分别覆盖 Gate 短连接/持久连接的固定响应和真实登录、Gateway 本地 ping、Gateway 到 Message 的好友列表事务、State 两个 RPC，以及 File 的 64 KiB 上传下载与摘要对账。`check-lv-tcp.py` 另外验证逐字节拆包、相邻帧、零长度帧、非法 Protobuf、超长声明和截断正文。`connections.json` 记录未认证空闲连接的 RSS 增量；这与认证后活跃连接的内存口径分开。

`resources.jsonl` 保留进程 CPU、RSS、读写系统调用、进程存储字节、指定网卡的包与字节计数，以及可选块设备计数。环回接口可能无法提供有意义的物理 PPS；正式实验应通过 `--network-interface` 指定独立数据接口，通过 `--block-device` 指定 File/MySQL/Redis 所在块设备。

对照组和候选必须使用相同的 `--rate`、`--seconds`、`--workers`、数据和资源预算：

```bash
python3 scripts/perf/compare.py BASELINE_DIR CANDIDATE_DIR --output comparison.json
```

热点分析另起诊断轮次，以免采样开销污染容量结果。以下命令分别采集硬件计数与调用栈，单轮最长 60 秒：

```bash
scripts/perf/diagnose-process.sh PID OUTPUT_DIR 30
```

正式容量实验仍按[方案](../server-network-performance-plan.zh-CN.md)执行预热、至少五次交错重复、1 小时初步持续测试和 8 小时最终稳定性测试，并记录硬件、网卡卸载、NUMA、持久化与数据库配置。共享开发机的结果只能作为资格验证。
