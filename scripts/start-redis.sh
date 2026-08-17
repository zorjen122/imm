#!/usr/bin/env bash
set -euo pipefail

# Start a disposable local topology with one master, two replicas, and three
# Sentinels. Every run replaces /tmp/wimi-redis.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMPLATE_DIR="$ROOT_DIR/server/conf/redis"
RUNTIME_ROOT=/tmp/wimi-redis
VAR_DIR="$RUNTIME_ROOT/var"
LOG_DIR="$RUNTIME_ROOT/log"
RUN_DIR="$RUNTIME_ROOT/run"
SENTINEL_PORT=26389

sentinel_topology_ready() {
  local -a info=()
  local port=""
  local replicas=""
  local sentinels=""
  local index

  mapfile -t info < <(
    timeout 1 redis-cli --no-auth-warning --raw -h 127.0.0.1 -p "$SENTINEL_PORT" -a root \
      SENTINEL master wimi-master 2>/dev/null
  )
  for ((index = 0; index + 1 < ${#info[@]}; index += 2)); do
    case "${info[index]}" in
      port) port="${info[index + 1]}" ;;
      num-slaves) replicas="${info[index + 1]}" ;;
      num-other-sentinels) sentinels="${info[index + 1]}" ;;
    esac
  done
  [[ "$port" == "6380" && "$replicas" == "2" && "$sentinels" == "2" ]]
}

"$ROOT_DIR/scripts/stop-redis.sh" >/dev/null 2>&1 || true
rm -rf -- /tmp/wimi-redis
mkdir -p "$VAR_DIR/master" "$VAR_DIR/replica-1" "$VAR_DIR/replica-2"
mkdir -p "$VAR_DIR/sentinel-1" "$VAR_DIR/sentinel-2" "$VAR_DIR/sentinel-3"
mkdir -p "$LOG_DIR" "$RUN_DIR"

render_redis_config() {
  local name="$1"
  local port="$2"
  local replica_of="$3"
  local config="$RUN_DIR/$name.conf"

  cp "$TEMPLATE_DIR/redis.conf" "$config"
  sed -i \
    -e "s|@PORT@|$port|g" \
    -e "s|@PID_FILE@|$RUN_DIR/$name.pid|g" \
    -e "s|@LOG_FILE@|$LOG_DIR/$name.log|g" \
    -e "s|@DATA_DIR@|$VAR_DIR/$name|g" \
    -e "s|@REPLICA_OF@|$replica_of|g" \
    "$config"
}

render_redis_config master 6380 ""
render_redis_config replica-1 6381 "replicaof 127.0.0.1 6380"
render_redis_config replica-2 6382 "replicaof 127.0.0.1 6380"

render_sentinel_config() {
  local name="$1"
  local port="$2"
  local config="$RUN_DIR/$name.conf"

  cp "$TEMPLATE_DIR/sentinel.conf" "$config"
  sed -i \
    -e "s|@PORT@|$port|g" \
    -e "s|@PID_FILE@|$RUN_DIR/$name.pid|g" \
    -e "s|@LOG_FILE@|$LOG_DIR/$name.log|g" \
    -e "s|@DATA_DIR@|$VAR_DIR/$name|g" \
    "$config"
}

render_sentinel_config sentinel-1 26389
render_sentinel_config sentinel-2 26390
render_sentinel_config sentinel-3 26391

cleanup_failed_start() {
  "$ROOT_DIR/scripts/stop-redis.sh" >/dev/null 2>&1 || true
}
trap cleanup_failed_start EXIT

redis-server "$RUN_DIR/master.conf"
redis-server "$RUN_DIR/replica-1.conf"
redis-server "$RUN_DIR/replica-2.conf"
redis-server "$RUN_DIR/sentinel-1.conf" --sentinel
redis-server "$RUN_DIR/sentinel-2.conf" --sentinel
redis-server "$RUN_DIR/sentinel-3.conf" --sentinel

for _ in {1..100}; do
  if sentinel_topology_ready; then
    trap - EXIT
    echo "Local Redis topology ready: master=6380 replicas=6381,6382 sentinels=26389,26390,26391"
    exit 0
  fi
  sleep 0.2
done

echo "Redis Sentinel did not discover the complete local topology." >&2
exit 1
