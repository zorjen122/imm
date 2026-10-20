#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   验证两个 message 节点同时运行时的跨节点核心流程。
#   用户 A 登录 gateway-1，用户 B 登录 gateway-2，脚本检查 Redis 会话路由、
#   跨节点文本 ACCEPTED/投递/ACK 落库、跨节点好友申请/回复和好友拉取。
# 前置条件：
#   使用 ./scripts/run-local-services.sh --cluster 启动服务。
#   典型启动方式见 --help 输出。
# 注意：
#   该脚本会向测试库写入临时用户、好友申请、好友关系和消息数据。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<EOF
Usage: ./scripts/smoke/smoke-multi-chat.sh

Requires two gateway services, two message services, and shared dependencies to be running, for example:
  ./scripts/init-mysql.sh
    ./scripts/run-local-services.sh --cluster

Verifies the currently supported multi-message-node path:
  - user A logs in to gateway node 1, user B logs in to gateway node 2
  - Redis records both users on different gateway session leases
  - cross-node text returns ACCEPTED with a message_id
  - delivery reaches the receiver and is ACKed as DELIVERED in MySQL
  - cross-node friend apply/reply succeeds and can be pulled
EOF
  exit 0
fi

if [[ "$#" -gt 0 ]]; then
  echo "Unknown argument: $1" >&2
  echo "Run with --help for usage." >&2
  exit 2
fi

: "${MYSQL_HOST:=127.0.0.1}"
: "${MYSQL_PORT:=3306}"
: "${IMM_DB:=chatServ}"
: "${IMM_DB_USER:=zorjen}"
: "${IMM_DB_PASSWORD:=root}"
: "${IMM_REDIS_HOST:=127.0.0.1}"
: "${IMM_REDIS_PORT:=6380}"
: "${IMM_REDIS_PASSWORD:=root}"
: "${CHAT_A_HOST:=127.0.0.1}"
: "${CHAT_A_PORT:=8090}"
: "${CHAT_B_HOST:=127.0.0.1}"
: "${CHAT_B_PORT:=8091}"
: "${GATEWAY_A_ID:=gateway-1}"
: "${GATEWAY_B_ID:=gateway-2}"
: "${GATE_URL:=http://127.0.0.1:18080}"

check_chat_port() {
  python3 "$ROOT_DIR/scripts/smoke/src/smoke_support.py" check-tcp "$@"
}

if ! check_chat_port "gateway node A" "$CHAT_A_HOST" "$CHAT_A_PORT" ||
  ! check_chat_port "gateway node B" "$CHAT_B_HOST" "$CHAT_B_PORT"; then
  cat >&2 <<EOF
smoke-multi-chat.sh requires two gateway nodes before it writes test data.

Start the local stack in multi-chat mode:
    ./scripts/run-local-services.sh --cluster

Current expected TCP endpoints:
  gateway node A: $CHAT_A_HOST:$CHAT_A_PORT
  gateway node B: $CHAT_B_HOST:$CHAT_B_PORT
EOF
  exit 1
fi

stamp="$(date +%s%N)"
uid_a=$((400000 + stamp % 100000))
uid_b=$((uid_a + 1))
user_a="multi_a_${stamp}"
user_b="multi_b_${stamp}"
friend_msg="multi_friend_apply_${stamp}"
friend_reply="multi_friend_reply_${stamp}"
text_payload="multi_text_${stamp}"
result_file="$(mktemp /tmp/imm-multi-chat.XXXXXX.json)"
trap 'rm -f "$result_file"' EXIT

mysql_exec() {
  mysql --protocol=tcp -h"$MYSQL_HOST" -P"$MYSQL_PORT" \
    -u"$IMM_DB_USER" -p"$IMM_DB_PASSWORD" "$IMM_DB" "$@"
}

mysql_scalar() {
  mysql_exec -N -B -e "$1" 2>/dev/null
}

expect_count() {
  local label="$1"
  local query="$2"
  local expected="$3"
  local actual
  actual="$(mysql_scalar "$query")"
  if [[ "$actual" != "$expected" ]]; then
    echo "$label failed: expected $expected, got ${actual:-<empty>}"
    echo "query: $query"
    exit 1
  fi
  echo "$label ok"
}

mysql_exec <<SQL
INSERT INTO users (uid, username, password, email, createTime) VALUES
  ($uid_a, '$user_a', '123456', '$user_a@example.com', '2026-07-11 00:00:00'),
  ($uid_b, '$user_b', '123456', '$user_b@example.com', '2026-07-11 00:00:00');

INSERT INTO userInfo (uid, name, age, sex, headImageURL) VALUES
  ($uid_a, 'MultiA', 21, 'test', '/images/multi-a.png'),
  ($uid_b, 'MultiB', 22, 'test', '/images/multi-b.png');
SQL

UID_A="$uid_a" \
UID_B="$uid_b" \
USER_A="$user_a" \
USER_B="$user_b" \
FRIEND_MSG="$friend_msg" \
FRIEND_REPLY="$friend_reply" \
TEXT_PAYLOAD="$text_payload" \
CHAT_A_HOST="$CHAT_A_HOST" \
CHAT_A_PORT="$CHAT_A_PORT" \
CHAT_B_HOST="$CHAT_B_HOST" \
CHAT_B_PORT="$CHAT_B_PORT" \
GATEWAY_A_ID="$GATEWAY_A_ID" \
GATEWAY_B_ID="$GATEWAY_B_ID" \
GATE_URL="$GATE_URL" \
IMM_REDIS_HOST="$IMM_REDIS_HOST" \
IMM_REDIS_PORT="$IMM_REDIS_PORT" \
IMM_REDIS_PASSWORD="$IMM_REDIS_PASSWORD" \
RESULT_FILE="$result_file" \
python3 "$ROOT_DIR/scripts/smoke/src/smoke_multi_chat.py"

read -r server_seq delayed_server_seq < <(
  python3 "$ROOT_DIR/scripts/smoke/src/smoke_support.py" json-fields \
    --file "$result_file" server_seq delayed_server_seq
)

for _ in {1..10}; do
  text_count="$(mysql_scalar "SELECT COUNT(*) FROM messages WHERE messageId = $server_seq AND senderId = $uid_a AND receiverId = $uid_b AND conversationId > 0 AND conversationSeq > 0 AND content = '$text_payload' AND status = 3 AND COALESCE(readDateTime, '') <> '';")"
  if [[ "$text_count" == "1" ]]; then
    echo "cross-node text mysql READ ack ok"
    break
  fi
  sleep 1
done

if [[ "${text_count:-0}" != "1" ]]; then
  echo "cross-node text row was not persisted as READ"
  mysql_exec -e "SELECT * FROM messages WHERE messageId = $server_seq;" || true
  exit 1
fi

if [[ "$delayed_server_seq" != "0" ]]; then
  expect_count \
    "RPC timeout retry single row" \
    "SELECT COUNT(*) FROM messages WHERE messageId = $delayed_server_seq AND senderId = $uid_a AND receiverId = $uid_b;" \
    "1"
fi

expect_count \
  "cross-node friend apply accepted rows" \
  "SELECT COUNT(*) FROM friendApplys WHERE status = 1 AND content = '$friend_reply' AND ((fromUid = $uid_a AND toUid = $uid_b) OR (fromUid = $uid_b AND toUid = $uid_a));" \
  "2"

expect_count \
  "cross-node friend relation rows" \
  "SELECT COUNT(*) FROM friends WHERE (uidA = $uid_a AND uidB = $uid_b) OR (uidA = $uid_b AND uidB = $uid_a);" \
  "1"

echo "multi chat smoke ok"
