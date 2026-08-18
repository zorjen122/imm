#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   启动一个隔离的本地 Docker Compose 服务集群，并复用 smoke-multi-chat.sh
#   验证 2 个 Gateway、2 个 Message 之间的跨节点会话、好友和消息链路。
# 拓扑：
#   MySQL、Redis、Sentinel、State、File、Gate 各 1 个容器，Gateway、Message 各 2 个容器。
# 用法：
#   ./scripts/smoke/smoke-compose-service-cluster.sh
# 生命周期：
#   脚本会等待服务就绪后执行 smoke；退出前删除本次测试的容器、网络和数据卷，
#   但保留镜像与 Docker 构建缓存。失败时会在清理前输出容器状态和日志。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# 每次运行使用独立项目名，使容器、网络和数据卷与默认开发环境隔离。
PROJECT_NAME="wimi-service-cluster-smoke-${BASHPID}"
COMPOSE_FILES=(
  -f "$ROOT_DIR/compose.yaml"
  -f "$ROOT_DIR/compose.service-cluster.yaml"
)

# 统一 Compose 参数，避免启动、查询、日志和清理操作使用不同项目。
compose() {
  docker compose -p "$PROJECT_NAME" "${COMPOSE_FILES[@]}" "$@"
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM

  # 失败时先保留诊断信息，再删除本次测试创建的资源。
  if [[ "$status" -ne 0 ]]; then
    echo "service cluster smoke failed; container status:" >&2
    compose ps --all >&2 || true
    echo "service cluster smoke logs:" >&2
    compose logs --no-color >&2 || true
  fi

  # 镜像和构建缓存会保留，仅删除本次项目的容器、网络和测试数据卷。
  compose down --volumes --remove-orphans >/dev/null 2>&1 || true
  exit "$status"
}

# smoke 在宿主机运行，并直接使用 MySQL、Redis 命令行客户端检查数据。
for command_name in docker python3 mysql redis-cli; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "required command not found: $command_name" >&2
    exit 1
  fi
done

if ! docker compose version >/dev/null 2>&1; then
  echo "docker compose is not available" >&2
  exit 1
fi

trap cleanup EXIT INT TERM

echo "starting service cluster smoke project: $PROJECT_NAME"
# --wait 保证基础设施健康且所有服务已启动后才执行跨节点断言。
compose up -d --build --wait

# MySQL 和 Redis 使用 Docker 分配的空闲宿主机端口，避免与本地服务冲突。
mysql_port="$(compose port mysql 3306)"
mysql_port="${mysql_port##*:}"
redis_port="$(compose port redis 6379)"
redis_port="${redis_port##*:}"

# 复用现有 smoke：用户 A、B 分别固定连接两个 Gateway，验证跨节点链路。
MYSQL_HOST=127.0.0.1 \
MYSQL_PORT="$mysql_port" \
WIMI_REDIS_HOST=127.0.0.1 \
WIMI_REDIS_PORT="$redis_port" \
CHAT_A_HOST=127.0.0.1 \
CHAT_A_PORT=8090 \
CHAT_B_HOST=127.0.0.1 \
CHAT_B_PORT=8091 \
GATEWAY_A_ID=gateway-1 \
GATEWAY_B_ID=gateway-2 \
GATE_URL=http://127.0.0.1:18080 \
  "$ROOT_DIR/scripts/smoke/smoke-multi-chat.sh"

echo "service cluster smoke ok"
