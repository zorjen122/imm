#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   启动本地联调所需的 state/file/message/gateway/gate 服务。
#   若本地 Redis 主从和 Sentinel 未运行，会调用 start-redis.sh 启动；脚本退出时清理本次启动的进程。
# 常用方式：
#   ./scripts/run-local-services.sh
# 停止当前用户从本仓库启动的旧 WIMI 服务，然后启动新服务：
#   ./scripts/run-local-services.sh --stop-existing
# G=2、N=2 方式：
#   WIMI_STATE_CONFIG="$PWD/server/conf/state-multi.yaml" \
#   WIMI_MESSAGE_CONFIGS="$PWD/server/conf/message-hunan-im.yaml $PWD/server/conf/message-beijing-im.yaml" \
#   WIMI_GATEWAY_CONFIGS="$PWD/server/conf/gateway-hunan.yaml $PWD/server/conf/gateway-beijing.yaml" \
#     ./scripts/run-local-services.sh
# 前置条件：
#   先执行 ./scripts/build-server.sh；需要 MySQL 已可连接，通常也会先执行 ./scripts/init-mysql.sh。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/wimi-server}"
: "${WIMI_STATE_CONFIG:=$ROOT_DIR/server/conf/state-multi.yaml}"
: "${WIMI_GATE_CONFIG:=$ROOT_DIR/server/conf/gate.yaml}"
: "${WIMI_FILE_CONFIG:=$ROOT_DIR/server/conf/file.yaml}"
: "${WIMI_MESSAGE_CONFIGS:=$ROOT_DIR/server/conf/message-hunan-im.yaml $ROOT_DIR/server/conf/message-beijing-im.yaml}"
: "${WIMI_GATEWAY_CONFIGS:=$ROOT_DIR/server/conf/gateway-hunan.yaml $ROOT_DIR/server/conf/gateway-beijing.yaml}"
: "${WIMI_MESSAGE_LOG_LEVEL:=--info}"

usage() {
  cat <<EOF
Usage: ./scripts/run-local-services.sh [--stop-existing]

Options:
  --stop-existing  Stop WIMI services started by the current user from this
                   repository before starting the requested local topology.
                   MySQL and Redis are not stopped.
  -h, --help       Show this help message.
EOF
}

stop_existing=0
while [[ "$#" -gt 0 ]]; do
  case "$1" in
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

for exe in "$BUILD_DIR/state/state" "$BUILD_DIR/file/file" "$BUILD_DIR/message/message" "$BUILD_DIR/gateway/gateway" "$BUILD_DIR/gate/gate" ; do
  if [[ ! -x "$exe" ]]; then
    echo "Missing build output: $exe"
    echo "Run ./scripts/build-server.sh first."
    exit 1
  fi
done

if [[ "$ROOT_DIR/server/public/proto/tcp_message.proto" -nt "$BUILD_DIR/gateway/gateway" ||
      "$ROOT_DIR/server/public/proto/tcp_message.proto" -nt "$BUILD_DIR/message/message" ]]; then
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

  pid_is_wimi_service() {
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
      pid_is_wimi_service "$pid"; then
      service_pids+=("$pid")
      seen["$pid"]=1
    fi
  done < <(pgrep -u "$(id -u)" -f -- "$ROOT_DIR/build/" || true)

  if [[ "${#service_pids[@]}" -eq 0 ]]; then
    echo "No existing WIMI services found for the current user."
    return 0
  fi

  echo "Stopping existing WIMI services: ${service_pids[*]}"
  kill -TERM "${service_pids[@]}" 2>/dev/null || true

  local deadline=$((SECONDS + 8))
  local -a remaining=()
  while ((SECONDS < deadline)); do
    remaining=()
    for pid in "${service_pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null && pid_is_wimi_service "$pid"; then
        remaining+=("$pid")
      fi
    done
    if [[ "${#remaining[@]}" -eq 0 ]]; then
      echo "Existing WIMI services stopped."
      return 0
    fi
    sleep 0.2
  done

  local -a force_stop=()
  for pid in "${remaining[@]}"; do
    if kill -0 "$pid" 2>/dev/null && pid_is_wimi_service "$pid"; then
      force_stop+=("$pid")
    fi
  done
  if [[ "${#force_stop[@]}" -eq 0 ]]; then
    echo "Existing WIMI services stopped."
    return 0
  fi

  echo "Forcing unresponsive WIMI services to stop: ${force_stop[*]}" >&2
  kill -KILL "${force_stop[@]}" 2>/dev/null || true
  sleep 0.2
  for pid in "${force_stop[@]}"; do
    if kill -0 "$pid" 2>/dev/null && pid_is_wimi_service "$pid"; then
      echo "Unable to stop WIMI service process $pid." >&2
      return 1
    fi
  done
  echo "Existing WIMI services stopped."
}

if [[ "$stop_existing" == "1" ]]; then
  stop_existing_services
fi

redis_topology_ready() {
  [[ "$(timeout 1 redis-cli --no-auth-warning --raw -h 127.0.0.1 -p 26389 -a root \
    SENTINEL get-master-addr-by-name wimi-master 2>/dev/null | tail -n 1)" =~ ^638[0-2]$ ]]
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

start_service state "$ROOT_DIR/server/state" env WIMI_CONFIG="$WIMI_STATE_CONFIG" "$BUILD_DIR/state/state"
start_service file "$ROOT_DIR/server/file" env WIMI_CONFIG="$WIMI_FILE_CONFIG" "$BUILD_DIR/file/file"
message_index=1
for message_config in $WIMI_MESSAGE_CONFIGS; do
  start_service "message-$message_index" "$ROOT_DIR/server/message" "$BUILD_DIR/message/message" "$message_config" "$WIMI_MESSAGE_LOG_LEVEL"
  message_index=$((message_index + 1))
done
gateway_index=1
for gateway_config in $WIMI_GATEWAY_CONFIGS; do
  start_service "gateway-$gateway_index" "$ROOT_DIR/server/gateway" env WIMI_CONFIG="$gateway_config" "$BUILD_DIR/gateway/gateway"
  gateway_index=$((gateway_index + 1))
done
start_service gate "$ROOT_DIR/server/gate" env WIMI_CONFIG="$WIMI_GATE_CONFIG" "$BUILD_DIR/gate/gate"

echo "Local services are running. Gate: http://127.0.0.1:18080"
wait
