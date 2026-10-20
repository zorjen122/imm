# 服务端构建与本地运行

以下命令从仓库根目录执行。

## 依赖

服务端需要支持 C++23 的编译器、CMake 3.24+ 和已安装的 Seastar（包含 CMake 配置）。
本机验证使用 GCC 15。Seastar 的构建依赖需按其源码中的说明安装。

Ubuntu 开发环境中的主要依赖可通过以下命令安装；这不包含 Seastar 本身：

```bash
sudo apt-get install -y \
  build-essential cmake pkg-config libboost-all-dev \
  protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libprotoc-dev libgrpc++-dev \
  libfmt-dev libyaml-cpp-dev libspdlog-dev libjsoncpp-dev \
  libssl-dev libc-ares-dev libre2-dev zlib1g-dev libhiredis-dev \
  mysql-server redis-server curl openssl python3-protobuf python3-yaml
```

CMake 会下载并构建 gRPC 1.84.0、匹配的 Abseil 和 MariaDB Connector/C。
系统 Protobuf 提供协议生成器和运行库。MySQL 使用经典协议端口 `3306`；
Redis 使用 hiredis 适配，SMTP 由 Seastar 网络流驱动。

## 构建与启动

```bash
JOBS=8 ./scripts/build-server.sh
./scripts/init-mysql.sh
./scripts/run-local-services.sh
```

先确保 MySQL 已启动。`init-mysql.sh` 默认通过 `sudo mysql` 创建测试库与账号，
**会重置测试表和种子数据**。默认应用账号为 `zorjen/root`，数据库为 `chatServ`。
聊天用户为 `zorjen`（1001）、`alice`（1002）、`bob`（1003），密码均为 `123456`。

启动脚本按需创建本地 Redis 主从和 Sentinel，并在退出时清理本次启动的服务及 Redis。
若 Redis 已在运行，退出时保留它。手动启动、停止 Redis 可使用
`./scripts/start-redis.sh` 和 `./scripts/stop-redis.sh`；这些脚本使用 `/tmp/imm-redis`。

双 Gateway、双 Message 拓扑使用：

```bash
./scripts/run-local-services.sh --cluster
```

重新启动本仓库的旧服务时，可加 `--stop-existing`。配置结构、端口和单独启动方式见
[配置说明](configuration.zh-CN.md)。本地配置会将验证码返回到响应，不发送外部邮件。

构建产物位于 `build/imm-server/`。`build/` 可删除后重建；保留 `build/deps/`
可复用本地依赖源码和开发库缓存，构建脚本会自动使用。

## 验证

基础测试无需启动完整服务：

```bash
ctest --test-dir build/imm-server --output-on-failure
```

本地服务启动后，另开终端执行：

```bash
./scripts/smoke/smoke-base.sh
```

双节点拓扑还可执行 `./scripts/smoke/smoke-multi-chat.sh`。smoke 会操作测试账号并写入数据。

需要独立端口和临时数据库时，使用以下集成测试；无需预先启动 IMM 服务。
当前测试通过本地 `mysqld --initialize-insecure` 启动隔离实例，应以普通开发用户运行：

```bash
python3 server/test/gate/run_io_integration.py
python3 server/test/gateway/run_integration.py --mtls --full-smoke --redis-failover
```

覆盖范围和验证边界见[服务端验证状态](server-feature-verification.zh-CN.md)。
性能测试见[性能测试入口](performance/README.zh-CN.md)。

## 可选的本地容器环境

基础 Compose 使用 `loopback/compose/` 配置，提供各一个服务实例及 MySQL、Redis/Sentinel。
容器配置已通过静态检查，当前未完成整套容器运行验证；本地集群使用上述宿主机流程。

```bash
docker compose config --quiet
IMM_SEASTAR_SOURCE=/path/to/seastar docker compose up -d --build --wait
docker compose down
```

Gate 和 Gateway 分别暴露在 `18080`、`8090`。MySQL、Redis 和 File 使用命名卷，
普通 `docker compose down` 不删除这些数据。构建需要 Compose 的 `include` 与
`additional_contexts` 支持，以及 `compose.yaml` 指定的 Seastar 源码上下文。
