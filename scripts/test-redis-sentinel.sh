#!/usr/bin/env bash
set -euo pipefail

# This test resets this workspace's local Redis topology, stops its original
# master, and verifies that Sentinel promotes one of the two replicas.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/wimi-server}"
PASSWORD=root
MASTER_NAME=wimi-master
KEY="wimi:sentinel:test:$$"
CLIENT_TEST="$BUILD_DIR/public/redisSentinelFailoverTest"

if [[ ! -x "$CLIENT_TEST" ]]; then
  echo "Missing build output: $CLIENT_TEST" >&2
  echo "Run ./scripts/build-server.sh first." >&2
  exit 1
fi

redis_cli() {
  local port="$1"
  shift
  redis-cli --no-auth-warning -h 127.0.0.1 -p "$port" -a "$PASSWORD" "$@"
}

sentinel_master_port() {
  redis_cli 26389 --raw SENTINEL get-master-addr-by-name "$MASTER_NAME" | tail -n 1
}

wait_for_value() {
  local port="$1"
  local expected="$2"
  for _ in {1..50}; do
    if [[ "$(redis_cli "$port" GET "$KEY")" == "$expected" ]]; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

"$ROOT_DIR/scripts/start-redis.sh"
trap '"$ROOT_DIR/scripts/stop-redis.sh" >/dev/null 2>&1 || true' EXIT INT TERM

redis_cli 6380 SET "$KEY" before-failover >/dev/null
wait_for_value 6381 before-failover
wait_for_value 6382 before-failover

"$CLIENT_TEST" "$KEY"

new_master_port=""
for _ in {1..100}; do
  candidate="$(sentinel_master_port)"
  if [[ "$candidate" == "6381" || "$candidate" == "6382" ]]; then
    new_master_port="$candidate"
    break
  fi
  sleep 0.2
done

if [[ -z "$new_master_port" ]]; then
  echo "Sentinel did not promote a replica." >&2
  exit 1
fi

if [[ "$new_master_port" == "6381" ]]; then
  remaining_replica_port=6382
else
  remaining_replica_port=6381
fi
wait_for_value "$remaining_replica_port" after-failover

echo "Redis Sentinel failover passed: new master=$new_master_port"
