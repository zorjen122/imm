# WIMI 服务端依赖与本地流程

本文档以 Ubuntu 26.04 与 clang 为基线，说明本地构建和联调服务端所需的依赖。

## 系统依赖

| 组件 | 用途 | 包或运行时 |
| --- | --- | --- |
| C++ 工具链 | 所有 C++ 服务 | `clang`、`cmake`、`build-essential`、`pkg-config` |
| Protobuf / gRPC | Gate、State、Message、File、Gateway 与协议 smoke | `protobuf-compiler`、`protobuf-compiler-grpc`、`libprotobuf-dev`、`libgrpc++-dev`、`python3-protobuf` |
| Qt 客户端 | Linux 客户端和 QtProtobuf 生成 | `qt6-base-dev`、`qt6-declarative-dev`、`qt6-svg-dev`、`qt6-grpc-dev` |
| Boost | Gate、Message、测试客户端 | `libboost-dev`、`libboost-filesystem-dev` |
| 配置、日志与 JSON | 所有 C++ 服务 | `libyaml-cpp-dev`、`libspdlog-dev`、`libfmt-dev`、`libjsoncpp-dev` |
| Redis | 验证码、登录交接、在线路由和 ID | `redis-server`、`libhiredis-dev` |
| MySQL | 用户、关系和消息 | `mysql-server`、MySQL Connector/C++ X DevAPI |
| SMTP | Gate 邮件验证码投递 | `libcurl4-openssl-dev` |

使用 Ubuntu 仓库安装基础依赖：

```bash
sudo apt-get install -y \
  clang cmake build-essential pkg-config \
  protobuf-compiler protobuf-compiler-grpc libprotobuf-dev python3-protobuf \
  libgrpc++-dev libgrpc-dev \
  qt6-base-dev qt6-declarative-dev qt6-svg-dev qt6-grpc-dev \
  libboost-dev libboost-filesystem-dev \
  libyaml-cpp-dev libspdlog-dev libfmt-dev libjsoncpp-dev \
  libhiredis-dev redis-server mysql-server libcurl4-openssl-dev \
  librdkafka-dev
```

项目使用 MySQL Connector/C++ 的 X DevAPI。通过 MySQL APT 仓库安装 Connector/C++：

```bash
curl -fsSL https://repo.mysql.com/RPM-GPG-KEY-mysql-2025 -o /tmp/RPM-GPG-KEY-mysql-2025
sudo sh -c 'gpg --dearmor < /tmp/RPM-GPG-KEY-mysql-2025 > /usr/share/keyrings/mysql-apt-config.gpg'
sudo apt-get update
sudo apt-get install -y libmysqlcppconn-dev libmysqlcppconn10 libmysqlcppconnx2
```

并确认 MySQL X Plugin 监听 `127.0.0.1:33060`。

## 本地端口

| 端口 | 服务 |
| --- | --- |
| `18080` | Gate HTTP API |
| `50052` | State gRPC |
| `50055`、`50056` | Message gRPC 节点 |
| `8090`、`8091` | Connection Gateway TCP 节点 |
| `51100` | File gRPC |
| `6380` | Redis 初始主节点，密码为 `root` |
| `6381`、`6382` | Redis 副本节点，密码为 `root` |
| `26389`、`26390`、`26391` | Redis Sentinel，密码为 `root` |
| `33060` | MySQL X Plugin |

## 构建、初始化与启动

在仓库根目录依次执行：

```bash
./scripts/build.sh
./scripts/init-mysql.sh
./scripts/start-redis.sh
./scripts/run-local-services.sh
```

构建会在 CMake 构建目录生成 gRPC/protobuf C++ 源码。`init-mysql.sh` 会创建本地
`zorjen/root` MySQL 账号，并导入 `scripts/sql/init_message_mysql_test.sql`。种子数据包含
三个聊天用户：`zorjen`（1001）、`alice`（1002）和 `bob`（1003），密码均为
`123456`。

本地 smoke 为方便验证，会通过 `gate.yaml` 中的 `exposeCodeInResponse` 返回验证码。
生产环境必须关闭该选项并配置 SMTP。

`start-redis.sh` 会启动一主两从和三个 Sentinel，并在每次启动时覆盖临时目录
`/tmp/wimi-redis`。可使用 `scripts/test-redis-sentinel.sh` 验证主节点停止后 Sentinel 能提升
一个副本；测试会重建并停止这套本地 Redis 拓扑。

## Docker Compose

Docker Compose 2.20.3 或更高版本可从仓库根目录启动单实例服务端：

```bash
docker compose up -d --build --wait
```

总入口 `compose.yaml` 通过 `include` 加载 `infra.yaml` 中的单节点 MySQL、Redis 和
Sentinel，并启动各一个 State、File、Message、Gateway 和 Gate。Gate 暴露在 `18080`，
Gateway 暴露在 `8090`；MySQL、Redis 和内部 RPC 端口不暴露到宿主机。该开发拓扑不使用
Compose watch，也不挂载源码目录；代码变化后重新执行带 `--build` 的启动命令。

```bash
docker compose ps
docker compose down
```

MySQL、Redis 和 File 使用命名卷，因此普通 `docker compose down` 不删除数据。

## 启动 G × N 拓扑

以下命令启动两个 Gateway、两个 Message 节点的本地拓扑：

```bash
WIMI_STATE_CONFIG="$PWD/conf/state-multi.yaml" \
WIMI_MESSAGE_CONFIGS="$PWD/conf/message-hunan-im.yaml $PWD/conf/message-beijing-im.yaml" \
WIMI_GATEWAY_CONFIGS="$PWD/conf/gateway-hunan.yaml $PWD/conf/gateway-beijing.yaml" \
./scripts/run-local-services.sh
```

## Smoke 测试

默认服务启动后：

```bash
./scripts/smoke/smoke-base.sh
```

该脚本覆盖注册、Gate 登录、state 路由、文本消息落库、文件上传和在线消息投递。

启动上面的 `G × N` 拓扑后，再执行：

```bash
./scripts/smoke/smoke-gateway-message.sh
./scripts/smoke/smoke-multi-chat.sh
```

这些脚本验证 Gateway-Message 服务流拓扑、跨 Gateway 文本投递、幂等、群会话序号分配、
同步修复和跨节点关系流程。
