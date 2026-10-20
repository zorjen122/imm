#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   启动本地联调所需的 state/file/message/gateway/gate 服务。
#   若本地 Redis 主从和 Sentinel 未运行，会调用 start-redis.sh 启动；脚本退出时清理本次启动的进程。
# 常用方式：
#   ./scripts/run-local-services.sh
# 停止当前用户从本仓库启动的旧 IMM 服务，然后启动新服务：
#   ./scripts/run-local-services.sh --stop-existing
# 双 Gateway/Message：./scripts/run-local-services.sh --cluster
# 前置条件：
#   先执行 ./scripts/build-server.sh；需要 MySQL 已可连接，通常也会先执行 ./scripts/init-mysql.sh。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/imm-server}"
: "${IMM_CONFIG:=$ROOT_DIR/loopback/single.yaml}"
: "${IMM_LOG_LEVEL:=info}"

usage() {
  cat <<EOF
Usage: ./scripts/run-local-services.sh [--stop-existing] [--cluster]

Options:
  --stop-existing  Stop IMM services started by the current user from this
                   repository before starting the requested local topology.
                   MySQL and Redis are not stopped.
  --cluster       Append loopback/cluster.yaml nodes.
  -h, --help       Show this help message.
EOF
}

cluster=0
stop_existing=0
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --cluster)
      cluster=1
      ;;
    --stop-existing)
      stop_existing=1
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ "$cluster" == "1" ]]; then
  export IMM_EXTRA_CONFIG="${IMM_EXTRA_CONFIG:-$ROOT_DIR/loopback/cluster.yaml}"
else
  unset IMM_EXTRA_CONFIG
fi
export IMM_CONFIG
node_listing="$(python3 - "$IMM_CONFIG" "${IMM_EXTRA_CONFIG:-}" <<'PYCONF'
import sys, yaml
with open(sys.argv[1]) as source:
    nodes = yaml.safe_load(source)["nodes"]
if sys.argv[2]:
    with open(sys.argv[2]) as source:
        additional = yaml.safe_load(source)["nodes"]
    if nodes.keys() & additional.keys():
        raise ValueError("duplicate node names")
    nodes.update(additional)
for kind in ("state", "file", "message", "gateway", "gate"):
    for name, node in nodes.items():
        if node["service"] == kind:
            print(kind, name)
PYCONF
)"
mapfile -t nodes <<< "$node_listing"

for exe in "$BUILD_DIR/state/state" "$BUILD_DIR/file/file" "$BUILD_DIR/message/message" "$BUILD_DIR/gateway/gateway" "$BUILD_DIR/gate/gate" ; do
  if [[ ! -x "$exe" ]]; then
    echo "Missing build output: $exe"
    echo "Run ./scripts/build-server.sh first."
    exit 1
  fi
done

if [[ "$ROOT_DIR/server/common/proto/tcp_message.proto" -nt "$BUILD_DIR/gateway/gateway" ||
      "$ROOT_DIR/server/common/proto/tcp_message.proto" -nt "$BUILD_DIR/message/message" ]]; then
  echo "Build output is older than tcp_message.proto: $BUILD_DIR" >&2
  echo "Run ./scripts/build-server.sh, then restart with --stop-existing." >&2
  exit 1
fi

stop_existing_services() {
  if ! command -v pgrep >/dev/null 2>&1; then
    echo "Cannot use --stop-existing: pgrep is not installed." >&2
    return 1
  fi

  local -a service_pids=()
  local -A seen=()
  local pid

  pid_is_imm_service() {
    local target_pid="$1"
    local argument
    [[ -r "/proc/$target_pid/cmdline" ]] || return 1
    while IFS= read -r -d '' argument; do
      case "$argument" in
        "$ROOT_DIR"/build/*/state/state | "$ROOT_DIR"/build/*/file/file | \
        "$ROOT_DIR"/build/*/message/message | "$ROOT_DIR"/build/*/gateway/gateway | \
        "$ROOT_DIR"/build/*/gate/gate)
          return 0
          ;;
      esac
    done < "/proc/$target_pid/cmdline"
    return 1
  }

  while IFS= read -r pid; do
    if [[ -n "$pid" && "$pid" != "$$" && -z "${seen[$pid]:-}" ]] &&
      pid_is_imm_service "$pid"; then
      service_pids+=("$pid")
      seen["$pid"]=1
    fi
  done < <(pgrep -u "$(id -u)" -f -- "$ROOT_DIR/build/" || true)

  if [[ "${#service_pids[@]}" -eq 0 ]]; then
    echo "No existing IMM services found for the current user."
    return 0
  fi

  echo "Stopping existing IMM services: ${service_pids[*]}"
  kill -TERM "${service_pids[@]}" 2>/dev/null || true

  local deadline=$((SECONDS + 8))
  local -a remaining=()
  while ((SECONDS < deadline)); do
    remaining=()
    for pid in "${service_pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null && pid_is_imm_service "$pid"; then
        remaining+=("$pid")
      fi
    done
    if [[ "${#remaining[@]}" -eq 0 ]]; then
      echo "Existing IMM services stopped."
      return 0
    fi
    sleep 0.2
  done

  local -a force_stop=()
  for pid in "${remaining[@]}"; do
    if kill -0 "$pid" 2>/dev/null && pid_is_imm_service "$pid"; then
      force_stop+=("$pid")
    fi
  done
  if [[ "${#force_stop[@]}" -eq 0 ]]; then
    echo "Existing IMM services stopped."
    return 0
  fi

  echo "Forcing unresponsive IMM services to stop: ${force_stop[*]}" >&2
  kill -KILL "${force_stop[@]}" 2>/dev/null || true
  sleep 0.2
  for pid in "${force_stop[@]}"; do
    if kill -0 "$pid" 2>/dev/null && pid_is_imm_service "$pid"; then
      echo "Unable to stop IMM service process $pid." >&2
      return 1
    fi
  done
  echo "Existing IMM services stopped."
}

if [[ "$stop_existing" == "1" ]]; then
  stop_existing_services
fi

redis_topology_ready() {
  [[ "$(timeout 1 redis-cli --no-auth-warning --raw -h 127.0.0.1 -p 26389 -a root \
    SENTINEL get-master-addr-by-name imm-master 2>/dev/null | tail -n 1)" =~ ^638[0-2]$ ]]
}

started_redis=0
if ! redis_topology_ready; then
  started_redis=1
  "$ROOT_DIR/scripts/start-redis.sh"
fi

pids=()
cleanup() {
  for pid in "${pids[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
  done
  if [[ "$started_redis" == "1" ]]; then
    "$ROOT_DIR/scripts/stop-redis.sh" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

start_service() {
  local name="$1"
  local cwd="$2"
  shift 2
  echo "Starting $name..."
  (cd "$cwd" && exec "$@") &
  pids+=("$!")
  sleep 1
  if ! kill -0 "${pids[-1]}" >/dev/null 2>&1; then
    echo "$name failed to start"
    exit 1
  fi
}

for entry in "${nodes[@]}"; do
  read -r service node <<< "$entry"
  start_service "$node" "$ROOT_DIR" env IMM_NODE="$node" "$BUILD_DIR/$service/$service" --log_level "$IMM_LOG_LEVEL"
done

echo "Local services are running. Gate: http://127.0.0.1:18080"
wait
