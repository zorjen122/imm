# IMM

一个基于 C++23、Seastar 和 gRPC 的实验性即时通讯服务端。

中文 · [English](README.md)

IMM 提供注册登录、单聊与群聊、多设备会话、消息确认、增量同步和分块文件传输。

## 背景

一个 IM 服务需要同时维护大量连接，还要完成鉴权、消息存储、路由和在线投递。
这些工作既涉及复杂的业务关系，也会花很多时间等待网络和数据库。IMM 希望在清楚表达这些业务的同时，
让多核机器把更多时间用在处理请求上。

常见的多线程设计会让工作线程共享连接池、任务队列和在线状态。共享方便了统一管理，
但也带来了协调成本：访问数据需要同步，缓存行可能在 CPU 核之间来回移动，任务还需要跨线程交接。
即使使用原子操作或无锁结构，跨核共享本身的成本仍然存在。并发越高，这些成本就越值得关注。

IMM 的 Gate、Gateway 和 Message 基于 Seastar，采用尽量不共享资源的方式：每个 shard
在自己的 reactor 上管理连接、协议状态和在途请求，跨 shard 协作通过显式消息传递完成。
常用数据和执行任务尽量留在所属核上，减少对共享锁、共享队列和跨核协调的依赖。

每个 reactor 同时推进多个请求。请求发起 Redis、MySQL、SMTP 或 gRPC 操作后，协程挂起，
reactor 继续处理其他就绪任务；操作完成后，原 shard 恢复业务判断并生成响应。
C++ 协程让接收、校验、存储和响应仍能按业务顺序书写，同时避免为每个等待中的请求占用一个工作线程。

这种执行方式也要求清晰的资源与职责边界：

- 协议层统一解析、校验和分派，事务层表达业务步骤，I/O driver 驱动依赖并交付结果。
  各层分别进入 `protocol`、`transaction`、`io` 调度组，按类别分配 CPU 调度份额。
- 在途请求、排队操作和待发送数据受数量及字节额度约束，超额时明确拒绝。
  工作量在进入系统时就受到限制，避免积压持续放大内存占用和等待时间。
- 连接由所属 shard 创建和关闭。每个 Message shard 的 MySQL 连接按完整事务独占，
  避免事务语句交错；跨 shard 投递则显式承担通信成本。

减少共享，是为了降低并发处理中的协调开销；明确归属和限制，是为了让系统在繁忙时仍然可控。
实际吞吐和延迟还取决于数据库、shard 数和负载，可通过
[Gateway 性能测试](server/bench/gateway/README.md)在具体环境中测量。

## 本地运行

先安装[服务端依赖](docs/requirements.zh-CN.md)，包括 Seastar 和支持 C++23 的编译器，
然后在仓库根目录执行：

```bash
./scripts/build-server.sh
./scripts/init-mysql.sh
./scripts/run-local-services.sh
```

`init-mysql.sh` 会重置本地测试表和种子数据。启动脚本会按需启动本地 Redis/Sentinel，
MySQL 需要提前运行。

本地配置统一放在 `loopback/`：`single.yaml` 定义共享依赖和各服务的首个节点，
`cluster.yaml` 追加第二个 Gateway 和 Message。运行
`./scripts/run-local-services.sh --cluster` 即可启动双节点拓扑。
默认客户端入口为 Gate `127.0.0.1:18080` 和 Gateway `127.0.0.1:8090`。

## 验证

```bash
ctest --test-dir build/imm-server --output-on-failure
./scripts/smoke/smoke-base.sh
```

smoke 需要先启动上述本地服务。双节点拓扑启动后，可运行
`./scripts/smoke/smoke-multi-chat.sh` 验证跨节点消息链路。

## 使用文档

- [服务端依赖与启动](docs/requirements.zh-CN.md)
- [配置说明](docs/configuration.zh-CN.md)
- [Gateway 性能测试](server/bench/gateway/README.md)：带宽、RTT、网络 I/O 吞吐及直接处理/转发 TPS
- [服务端能力与验证状态](docs/server-feature-verification.zh-CN.md)
