# 服务端构建与本地运行

以下命令从仓库根目录执行，面向 Ubuntu 开发环境。服务端使用 C++23，需要 CMake 3.24+。

## 安装依赖

```bash
JOBS=8 ./scripts/install_dependency.sh
sudo apt-get install -y mysql-server redis-server protobuf-compiler python3-protobuf
```

依赖脚本补齐系统开发包，并将源码依赖安装到 `$HOME/.local/imm`；普通用户安装系统包时会调用 sudo。
MySQL、Redis 和 Python smoke 工具由第二条命令安装，数据库初始化单独执行。

## 构建与启动

```bash
JOBS=8 ./scripts/build-server.sh

# 确认 MySQL 已启动，再初始化测试数据
./scripts/init-mysql.sh
./scripts/run-local-services.sh
```

`init-mysql.sh` **会重置测试表和种子数据**。默认测试用户为 `zorjen`、`alice`、`bob`，密码均为 `123456`。
启动脚本负责启动本地 Redis；退出时清理本次启动的服务。

需要双 Gateway、双 Message 时，使用：

```bash
./scripts/run-local-services.sh --cluster
```

加 `--stop-existing` 可先停止当前用户从本仓库启动的旧服务。本地配置位于 `loopback/single.yaml` 和 `loopback/cluster.yaml`。

## 基本验证

```bash
ctest --test-dir build/imm-server --output-on-failure
```

服务启动后，另开终端执行：

```bash
./scripts/smoke/smoke-base.sh
# 双节点拓扑
./scripts/smoke/smoke-multi-chat.sh
```

smoke 会使用测试账号并写入数据。
