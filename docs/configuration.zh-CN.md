# IMM 服务配置

宿主机本地拓扑使用两个配置文件：

| 文件 | 内容 |
| --- | --- |
| `loopback/single.yaml` | 共享 MySQL/Redis/TLS 配置，以及 Gate、File、State、首个 Gateway 和 Message。 |
| `loopback/cluster.yaml` | 只追加第二个 Gateway 和 Message。 |

```bash
./scripts/run-local-services.sh                  # 单节点
./scripts/run-local-services.sh --cluster        # 双 Gateway、双 Message
```

独立启动进程时，通过 `IMM_CONFIG` 指定基础文件，`IMM_NODE` 选择节点：

```bash
IMM_CONFIG="$PWD/loopback/single.yaml" IMM_NODE=gateway-1 build/imm-server/gateway/gateway --smp 2 --memory 256M --overprovisioned
```

双节点拓扑的每个进程还须设置 `IMM_EXTRA_CONFIG="$PWD/loopback/cluster.yaml"`。额外文件只允许追加节点；重复节点名或覆盖共享配置会被拒绝。

## 配置结构

`shared` 定义依赖连接及默认传输凭据，`nodes` 按节点名定义 `service`、`host` 和 `port`。节点可单独设置传输凭据、调度额度等参数。`host` 为对其他服务发布的地址；需要分别设置监听地址时使用 `listenHost`。State 的 Gateway/Message 列表直接来自节点定义，不另写数量和地址。

节点名是服务身份的一部分。同一拓扑中的节点名必须唯一，Gate、File、State 各只能定义一个。
例如在 `nodes.gateway-1` 下设置 `commandRateLimit: 0`，表示关闭该 Gateway 的单连接命令限流。

## 监听与存储

| 默认端口 | 服务 |
| --- | --- |
| `18080` | Gate HTTP |
| `8090`、`8091` | 两个 Gateway 的客户端 TCP 入口 |
| `50052` | State gRPC |
| `50055`、`50056` | 两个 Message 节点，各一个 shard |
| `51100` | File gRPC |
| `3306` | 本地 MySQL |
| `6380`、`6381`、`6382` | Redis 初始主节点及副本 |
| `26389`、`26390`、`26391` | Redis Sentinel |

Message 的 `shards` 决定默认 SMP 数及 `[port, port + shards - 1]` 监听范围；显式 `--smp` 必须与它一致。Message 的 `machineId` 在节点上配置，用于生成通知等 ID。增大 shard 数时，还需调整其他节点的
基准端口，避免监听范围重叠。

File 的相对 `storageRoot` 以可执行文件所在目录为基准。固定存储位置请使用绝对路径。

## 本地依赖与传输

`loopback/redis/` 保存本地 Redis/Sentinel 模板；`loopback/compose/` 保存基础容器部署配置。容器配置不参与本地配置追加。

默认 Redis 和 Sentinel 密码均为 `root`；MySQL 默认账号及库名见
[本地运行说明](requirements.zh-CN.md)。这些配置用于本机测试。

开发环境可以使用 `transportSecurity.mode: insecure`；生产环境须使用 mTLS，Gateway 证书身份须与节点名一致。
