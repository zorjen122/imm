#!/usr/bin/env bash
set -euo pipefail

# These ports are reserved for the disposable /tmp/wimi-redis topology.

RUN_DIR=/tmp/wimi-redis/run

for port in 26391 26390 26389 6382 6381 6380; do
  timeout 2 redis-cli --no-auth-warning -h 127.0.0.1 -p "$port" -a root \
    SHUTDOWN NOSAVE >/dev/null 2>&1 || true
done

rm -f "$RUN_DIR/sentinel-3.pid" "$RUN_DIR/sentinel-2.pid" "$RUN_DIR/sentinel-1.pid"
rm -f "$RUN_DIR/replica-2.pid" "$RUN_DIR/replica-1.pid" "$RUN_DIR/master.pid"

echo "Local Redis topology stopped."
ps -ef | grep -E ':(6380|6381|6382|26389|26390|26391)\b' | grep -v grep || true
