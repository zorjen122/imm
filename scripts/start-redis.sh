#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   启动本地开发/测试使用的临时 Redis 实例。
#   默认监听 127.0.0.1:6380，密码为 root，配置和 pid 文件写入 /tmp/wimi-redis。
#   如果目标 Redis 已经可 PING，则不会重复启动。
# 可选环境变量：
#   WIMI_REDIS_DIR/WIMI_REDIS_HOST/WIMI_REDIS_PORT/WIMI_REDIS_PASSWORD。

: "${WIMI_REDIS_DIR:=/tmp/wwimiredis}"
: "${WIMI_REDIS_HOST:=127.0.0.1}"
: "${WIMI_REDIS_PORT:=6380}"
: "${WIMI_REDIS_PASSWORD:=root}"

mkdir -p "$WIMI_REDIS_DIR"
cat > "$WIMI_REDIS_DIR/redis.conf" <<EOF
port $WIMI_REDIS_PORT
bind $WIMI_REDIS_HOST
requirepass $WIMI_REDIS_PASSWORD
daemonize yes
pidfile $WIMI_REDIS_DIR/redis.pid
dir $WIMI_REDIS_DIR
save ""
appendonly no
EOF

redis_ping() {
  timeout 3 redis-cli -h "$WIMI_REDIS_HOST" -p "$WIMI_REDIS_PORT" -a "$WIMI_REDIS_PASSWORD" PING >/dev/null 2>&1
}

if redis_ping; then
  echo "Redis already running on $WIMI_REDIS_HOST:$WIMI_REDIS_PORT"
else
  redis-server "$WIMI_REDIS_DIR/redis.conf"
fi

for _ in {1..10}; do
  if redis_ping; then
    echo "Redis ready on $WIMI_REDIS_HOST:$WIMI_REDIS_PORT"
    exit 0
  fi
  sleep 1
done

echo "Redis did not become ready on $WIMI_REDIS_HOST:$WIMI_REDIS_PORT" >&2
exit 1
