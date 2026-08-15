#!/usr/bin/env bash
set -euo pipefail

# Verifies the first-phase Connection Gateway topology with G=2 and N=2.
# The test uses the three default users created by init-mysql.sh and writes
# temporary group, conversation, file and message rows to chatServ.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<EOF
Usage: ./scripts/smoke/smoke-gateway-message.sh

Start the multi-node stack first:
  ./scripts/init-mysql.sh
  WIMI_STATE_CONFIG="\$PWD/server/conf/state-multi.yaml" \
  WIMI_MESSAGE_CONFIGS="\$PWD/server/conf/message-hunan-im.yaml \$PWD/server/conf/message-beijing-im.yaml" \
  WIMI_GATEWAY_CONFIGS="\$PWD/server/conf/gateway-hunan.yaml \$PWD/server/conf/gateway-beijing.yaml" \
    ./scripts/run-local-services.sh

Checks:
  - default users zorjen(1001), alice(1002) and bob(1003) exist
  - four established Gateway -> Message connections (2 x 2)
  - users on different Gateways have generation-fenced Redis leases
  - cross-Gateway direct text is accepted, sequenced and delivered
  - an identical retry returns the same message and sequence
  - a conflicting idempotency retry is rejected
  - conversation_seq sync repairs delivery state
  - group owner/member/outsider permissions, group files and membership changes
EOF
  exit 0
fi

if [[ "$#" -gt 0 ]]; then
  echo "Unknown argument: $1" >&2
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
: "${GATEWAY_A_HOST:=127.0.0.1}"
: "${GATEWAY_A_PORT:=8090}"
: "${GATEWAY_B_HOST:=127.0.0.1}"
: "${GATEWAY_B_PORT:=8091}"
: "${GATEWAY_A_ID:=hunan-gateway}"
: "${GATEWAY_B_ID:=beijing-gateway}"
: "${GATE_URL:=http://127.0.0.1:18080}"
: "${MESSAGE_A_PORT:=50055}"
: "${MESSAGE_B_PORT:=50056}"
: "${EXPECTED_GATEWAY_MESSAGE_LINKS:=4}"

python3 "$ROOT_DIR/scripts/smoke/src/smoke_support.py" check-tcp \
  "gateway A" "$GATEWAY_A_HOST" "$GATEWAY_A_PORT" \
  "gateway B" "$GATEWAY_B_HOST" "$GATEWAY_B_PORT"

if command -v ss >/dev/null 2>&1; then
  established_links="$(ss -Htn state established 2>/dev/null | awk -v a=":$MESSAGE_A_PORT" -v b=":$MESSAGE_B_PORT" '$NF ~ (a "$") || $NF ~ (b "$") { count++ } END { print count + 0 }')"
  if [[ "$established_links" -lt "$EXPECTED_GATEWAY_MESSAGE_LINKS" ]]; then
    echo "expected at least $EXPECTED_GATEWAY_MESSAGE_LINKS Gateway -> Message TCP connections, found $established_links" >&2
    exit 1
  fi
  echo "G x N topology ok: $established_links Gateway -> Message connections"
fi

stamp="$(date +%s%N)"
uid_a=1001
uid_b=1002
uid_c=1003
conversation_id=92001
group_id=$((700000 + stamp % 100000))
group_conversation_id=$((970000000 + stamp % 10000000))
user_a="zorjen"
user_b="alice"
user_c="bob"
payload="gateway_text_${stamp}"
client_message_id="client_${stamp}"
result_file="$(mktemp /tmp/wimi-gateway-message.XXXXXX.json)"
trap 'rm -f "$result_file"' EXIT

mysql_exec() {
  mysql --protocol=tcp -h"$MYSQL_HOST" -P"$MYSQL_PORT" \
    -u"$WIMI_DB_USER" -p"$WIMI_DB_PASSWORD" "$WIMI_DB" "$@"
}

default_user_count="$(
  mysql_exec -N -B -e "
SELECT COUNT(*)
FROM users
WHERE (uid, username) IN (
  (1001, 'zorjen'),
  (1002, 'alice'),
  (1003, 'bob')
);" 2>/dev/null
)"
if [[ "$default_user_count" != "3" ]]; then
  echo "expected default users zorjen(1001), alice(1002), bob(1003); run ./scripts/init-mysql.sh first" >&2
  exit 1
fi

mysql_exec <<SQL
INSERT INTO conversations (conversationId, type, businessId, latestSeq, createTime) VALUES
  ($group_conversation_id, 2, $group_id, 0, '2026-07-16 00:00:00');
INSERT INTO groupInfo (groupId, masterUid, name, conversationId, createTime) VALUES
  ($group_id, $uid_a, 'GatewayGroup', $group_conversation_id, '2026-07-16 00:00:00');
INSERT INTO conversationUserStates (conversationId, uid, joinedSeq) VALUES
  ($group_conversation_id, $uid_a, 1),
  ($group_conversation_id, $uid_b, 1);
INSERT INTO groupMembers (groupId, uid, joinTime) VALUES
  ($group_id, $uid_a, '2026-07-16 00:00:00'),
  ($group_id, $uid_b, '2026-07-16 00:00:00');
SQL

UID_A="$uid_a" UID_B="$uid_b" UID_C="$uid_c" USER_A="$user_a" \
USER_B="$user_b" USER_C="$user_c" \
CONVERSATION_ID="$conversation_id" PAYLOAD="$payload" \
GROUP_ID="$group_id" GROUP_CONVERSATION_ID="$group_conversation_id" \
CLIENT_MESSAGE_ID="$client_message_id" GATEWAY_A_HOST="$GATEWAY_A_HOST" \
GATEWAY_A_PORT="$GATEWAY_A_PORT" GATEWAY_B_HOST="$GATEWAY_B_HOST" \
GATEWAY_B_PORT="$GATEWAY_B_PORT" GATEWAY_A_ID="$GATEWAY_A_ID" \
GATEWAY_B_ID="$GATEWAY_B_ID" GATE_URL="$GATE_URL" \
WIMI_REDIS_HOST="$WIMI_REDIS_HOST" WIMI_REDIS_PORT="$WIMI_REDIS_PORT" \
WIMI_REDIS_PASSWORD="$WIMI_REDIS_PASSWORD" RESULT_FILE="$result_file" \
python3 "$ROOT_DIR/scripts/smoke/src/smoke_gateway_message.py"

read -r message_id conversation_seq < <(
  python3 "$ROOT_DIR/scripts/smoke/src/smoke_support.py" json-fields \
    --file "$result_file" messageId conversationSeq
)

row_count="$(mysql_exec -N -B -e "SELECT COUNT(*) FROM messages WHERE senderId=$uid_a AND clientMessageId='$client_message_id' AND messageId=$message_id AND conversationSeq=$conversation_seq;" 2>/dev/null)"
if [[ "$row_count" != "1" ]]; then
  echo "persistent idempotency failed: expected one row, got $row_count" >&2
  exit 1
fi

echo "cross-Gateway persistent message closure ok: message=$message_id seq=$conversation_seq"
echo "group authorization matrix ok: outsider, quit, kick, rejoin window, dissolve"
echo "group UI path ok: name sync, existing-member notice, message fan-out"
echo "gateway message smoke ok"
