#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   验证两个 message 节点同时运行时的跨节点核心流程。
#   用户 A 登录 gateway-1，用户 B 登录 gateway-2，脚本检查 Redis 会话路由、
#   跨节点文本 ACCEPTED/投递/ACK 落库、跨节点好友申请/回复和好友拉取。
# 前置条件：
#   使用 server/conf/state-multi.yaml 和两个 message config 启动服务。
#   典型启动方式见 --help 输出。
# 注意：
#   该脚本会向测试库写入临时用户、好友申请、好友关系和消息数据。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<EOF
Usage: ./scripts/smoke_multi_chat.sh

Requires two gateway services, two message services, and shared dependencies to be running, for example:
  ./scripts/init_mysql.sh
  WIMI_STATE_CONFIG="\$PWD/server/conf/state-multi.yaml" \\
  WIMI_MESSAGE_CONFIGS="\$PWD/server/conf/message-hunan-im.yaml \$PWD/server/conf/message-beijing-im.yaml" \\
  WIMI_GATEWAY_CONFIGS="\$PWD/server/conf/gateway-hunan.yaml \$PWD/server/conf/gateway-beijing.yaml" \\
    ./scripts/run_local_services.sh

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
: "${WIMI_DB:=chatServ}"
: "${WIMI_DB_USER:=zorjen}"
: "${WIMI_DB_PASSWORD:=root}"
: "${WIMI_REDIS_HOST:=127.0.0.1}"
: "${WIMI_REDIS_PORT:=6380}"
: "${WIMI_REDIS_PASSWORD:=root}"
: "${CHAT_A_HOST:=127.0.0.1}"
: "${CHAT_A_PORT:=8090}"
: "${CHAT_B_HOST:=127.0.0.1}"
: "${CHAT_B_PORT:=8091}"
: "${GATEWAY_A_ID:=hunan-gateway}"
: "${GATEWAY_B_ID:=beijing-gateway}"
: "${GATE_URL:=http://127.0.0.1:18080}"

check_chat_port() {
  local label="$1"
  local host="$2"
  local port="$3"

  python3 - "$label" "$host" "$port" <<'PY'
import socket
import sys

label = sys.argv[1]
host = sys.argv[2]
port = int(sys.argv[3])

try:
    with socket.create_connection((host, port), timeout=2):
        pass
except OSError as exc:
    print(f"{label} is not reachable at {host}:{port}: {exc}", file=sys.stderr)
    sys.exit(1)
PY
}

if ! check_chat_port "gateway node A" "$CHAT_A_HOST" "$CHAT_A_PORT" ||
  ! check_chat_port "gateway node B" "$CHAT_B_HOST" "$CHAT_B_PORT"; then
  cat >&2 <<EOF
smoke_multi_chat.sh requires two gateway nodes before it writes test data.

Start the local stack in multi-chat mode:
  WIMI_STATE_CONFIG="\$PWD/server/conf/state-multi.yaml" \\
  WIMI_MESSAGE_CONFIGS="\$PWD/server/conf/message-hunan-im.yaml \$PWD/server/conf/message-beijing-im.yaml" \\
  WIMI_GATEWAY_CONFIGS="\$PWD/server/conf/gateway-hunan.yaml \$PWD/server/conf/gateway-beijing.yaml" \\
    ./scripts/run_local_services.sh

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
result_file="$(mktemp /tmp/wimi-multi-chat.XXXXXX.json)"
trap 'rm -f "$result_file"' EXIT

mysql_exec() {
  mysql --protocol=tcp -h"$MYSQL_HOST" -P"$MYSQL_PORT" \
    -u"$WIMI_DB_USER" -p"$WIMI_DB_PASSWORD" "$WIMI_DB" "$@"
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
WIMI_REDIS_HOST="$WIMI_REDIS_HOST" \
WIMI_REDIS_PORT="$WIMI_REDIS_PORT" \
WIMI_REDIS_PASSWORD="$WIMI_REDIS_PASSWORD" \
RESULT_FILE="$result_file" \
PYTHONPATH="$ROOT_DIR/scripts/lib${PYTHONPATH:+:$PYTHONPATH}" \
python3 - <<'PY'
import json
import os
import subprocess
import time

from wimi_tcp_client import WimClient, request_chat_auth, require

UID_A = int(os.environ["UID_A"])
UID_B = int(os.environ["UID_B"])
USER_A = os.environ["USER_A"]
USER_B = os.environ["USER_B"]
FRIEND_MSG = os.environ["FRIEND_MSG"]
FRIEND_REPLY = os.environ["FRIEND_REPLY"]
TEXT_PAYLOAD = os.environ["TEXT_PAYLOAD"]
CHAT_A_HOST = os.environ["CHAT_A_HOST"]
CHAT_A_PORT = int(os.environ["CHAT_A_PORT"])
CHAT_B_HOST = os.environ["CHAT_B_HOST"]
CHAT_B_PORT = int(os.environ["CHAT_B_PORT"])
GATEWAY_A_ID = os.environ["GATEWAY_A_ID"]
GATEWAY_B_ID = os.environ["GATEWAY_B_ID"]
REDIS_HOST = os.environ["WIMI_REDIS_HOST"]
REDIS_PORT = os.environ["WIMI_REDIS_PORT"]
REDIS_PASSWORD = os.environ["WIMI_REDIS_PASSWORD"]
RESULT_FILE = os.environ["RESULT_FILE"]
GATE_URL = os.environ["GATE_URL"]

ID_PULL_FRIEND_LIST_REQ = 1001
ID_PULL_FRIEND_APPLY_LIST_REQ = 1003
ID_NOTIFY_ADD_FRIEND_REQ = 1021
ID_REPLY_ADD_FRIEND_REQ = 1023
ID_TEXT_SEND_REQ = 1027
ID_TEXT_READ_RECEIPT_NOTIFY = 1045

NOTIFY_MESSAGE_READ = 2 
MESSAGE_READ = 3

def redis_session_lease(uid, device_id):
    output = subprocess.check_output(
        [
            "redis-cli",
            "-h",
            REDIS_HOST,
            "-p",
            REDIS_PORT,
            "-a",
            REDIS_PASSWORD,
            "GET",
            f"im:session:{uid}:{device_id}",
        ],
        stderr=subprocess.DEVNULL,
        text=True,
    ).strip()
    require(output, f"redis session lease missing for uid {uid}")
    lease = json.loads(output)
    require(lease.get("gatewayId"), f"redis session lease has no gatewayId: {lease}")
    require(lease.get("instanceId"), f"redis session lease has no instanceId: {lease}")
    require(lease.get("connectionId"), f"redis session lease has no connectionId: {lease}")
    require(lease.get("generation", 0) > 0,
            f"redis session lease has invalid generation: {lease}")
    return lease

auth_a = request_chat_auth(USER_A, "123456", GATE_URL)
auth_b = request_chat_auth(USER_B, "123456", GATE_URL)
a = WimClient(UID_A, CHAT_A_HOST, CHAT_A_PORT,
              auth_token=auth_a["chatToken"])
b = WimClient(UID_B, CHAT_B_HOST, CHAT_B_PORT,
              auth_token=auth_b["chatToken"])
server_seq = 0
delayed_server_seq = 0
try:
    a.login(init=False)
    b.login(init=False)

    lease_a = redis_session_lease(UID_A, a.device_id)
    lease_b = redis_session_lease(UID_B, b.device_id)
    require(lease_a["gatewayId"] == GATEWAY_A_ID,
            f"uid {UID_A} is on {lease_a['gatewayId']}, expected {GATEWAY_A_ID}")
    require(lease_b["gatewayId"] == GATEWAY_B_ID,
            f"uid {UID_B} is on {lease_b['gatewayId']}, expected {GATEWAY_B_ID}")
    print("redis gateway session routes ok")

    apply_rsp = a.request(
        ID_NOTIFY_ADD_FRIEND_REQ,
        {"from": UID_A, "to": UID_B, "requestMessage": FRIEND_MSG},
    )
    require(apply_rsp.get("error") == 0, f"cross-node friend apply failed: {apply_rsp}")
    b.expect_async(
        ID_NOTIFY_ADD_FRIEND_REQ,
        lambda payload: payload.get("from") == UID_A
        and payload.get("to") == UID_B
        and payload.get("requestMessage") == FRIEND_MSG,
    )
    print("cross-node friend apply push ok")

    apply_pull = b.request(ID_PULL_FRIEND_APPLY_LIST_REQ, {"uid": UID_B})
    require(apply_pull.get("error") == 0, f"pull friend apply failed: {apply_pull}")
    require(
        any(item.get("content") == FRIEND_MSG for item in apply_pull.get("applyList", [])),
        f"pulled friend applies missing {FRIEND_MSG}: {apply_pull}",
    )

    reply_rsp = b.request(
        ID_REPLY_ADD_FRIEND_REQ,
        {"from": UID_B, "to": UID_A, "accept": True, "replyMessage": FRIEND_REPLY},
    )
    require(reply_rsp.get("error") == 0, f"cross-node friend reply failed: {reply_rsp}")
    a.expect_async(
        ID_REPLY_ADD_FRIEND_REQ,
        lambda payload: payload.get("from") == UID_B
        and payload.get("to") == UID_A
        and payload.get("accept") is True,
    )
    print("cross-node friend reply push ok")

    friends_a = a.request(ID_PULL_FRIEND_LIST_REQ, {"uid": UID_A})
    friends_b = b.request(ID_PULL_FRIEND_LIST_REQ, {"uid": UID_B})
    require(friends_a.get("error") == 0, f"pull friend list A failed: {friends_a}")
    require(friends_b.get("error") == 0, f"pull friend list B failed: {friends_b}")
    require(
        any(item.get("uid") == UID_B for item in friends_a.get("friendList", [])),
        f"A friend list missing {UID_B}: {friends_a}",
    )
    require(
        any(item.get("uid") == UID_A for item in friends_b.get("friendList", [])),
        f"B friend list missing {UID_A}: {friends_b}",
    )
    print("cross-node friend pull ok")

    client_message_id = f"multi_text_{time.time_ns()}"
    text_rsp = a.request(
        ID_TEXT_SEND_REQ,
        {
            "from": UID_A,
            "to": UID_B,
            "clientMessageId": client_message_id,
            "data": TEXT_PAYLOAD,
        },
    )
    require(text_rsp.get("error") == 0, f"cross-node text response failed: {text_rsp}")
    require(text_rsp.get("clientMessageId") == client_message_id,
            f"cross-node client message id changed: {text_rsp}")
    require(text_rsp.get("messageState") == 1, f"cross-node text was not accepted: {text_rsp}")
    require(text_rsp.get("messageId", 0) > 0, f"cross-node message id missing: {text_rsp}")

    text_push = b.expect_async(
        ID_TEXT_SEND_REQ,
        lambda payload: payload.get("from") == UID_A
        and payload.get("to") == UID_B
        and payload.get("data") == TEXT_PAYLOAD,
    )
    server_seq = int(text_push["seq"])
    require(text_rsp["messageId"] == server_seq,
            f"response/push message id mismatch: {text_rsp}, {text_push}")
    b.ack(server_seq, conversation_id=text_push["conversationId"],
          conversation_seq=text_push["conversationSeq"])

    # read notify sender when the recvier look the message 
    b.ack(server_seq, receipt_type=NOTIFY_MESSAGE_READ, 
          conversation_id=text_push["conversationId"],
          conversation_seq=text_push["conversationSeq"])
    read_notify = a.expect_async(
      ID_TEXT_READ_RECEIPT_NOTIFY,
      lambda payload: payload.get("conversationId") == text_push["conversationId"]
      and payload.get("conversationSeq") == text_push["conversationSeq"]
      and payload.get("messageState") == MESSAGE_READ    # state 3 = MESSAGE_READ
    )

    print("cross-node text push ok")

    if os.environ.get("WIMI_TEST_RPC_DELAY") == "1":
        delayed_body = {
            "from": UID_A,
            "to": UID_B,
            "clientMessageId": f"multi_delay_{time.time_ns()}",
            "data": "rpc_delay_after_accept",
            "requestTimeoutMs": 100,
            "requestId": f"multi_delay_request_{time.time_ns()}",
        }
        delayed_first = a.request(ID_TEXT_SEND_REQ, delayed_body)
        require(delayed_first.get("error", 0) != 0 and
                delayed_first.get("retryable") is True,
                f"delayed RPC did not time out retryably: {delayed_first}")
        delayed_push = b.expect_async(
            ID_TEXT_SEND_REQ,
            lambda payload: payload.get("data") == "rpc_delay_after_accept",
        )
        delayed_server_seq = int(delayed_push["seq"])

        delayed_body["requestTimeoutMs"] = 2000
        delayed_retry = a.request(ID_TEXT_SEND_REQ, delayed_body)
        require(delayed_retry.get("error", 0) != 0 and
                delayed_retry.get("retryable") is False,
                f"delayed RPC retry was not rejected as duplicate: {delayed_retry}")
        b.ack(delayed_server_seq,
              conversation_id=delayed_push["conversationId"],
              conversation_seq=delayed_push["conversationSeq"])
        print("RPC deadline duplicate suppression ok")

    print("cross-node message delivery ok")
finally:
    try:
        a.quit(wait_response=True)
    finally:
        b.quit(wait_response=True)

with open(RESULT_FILE, "w", encoding="utf-8") as fp:
    json.dump({"server_seq": server_seq,
               "delayed_server_seq": delayed_server_seq}, fp)
PY

server_seq="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["server_seq"])' "$result_file")"
delayed_server_seq="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["delayed_server_seq"])' "$result_file")"

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
