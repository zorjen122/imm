#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   面向本地 Connection Gateway 拓扑的端到端 smoke 测试。
#   覆盖登录、state 路由、文本消息落库、文件上传和在线直发消息。
# 前置条件：
#   已执行 ./scripts/build-server.sh 和 ./scripts/init-mysql.sh，
#   并通过 ./scripts/run-local-services.sh --cluster 启动本地服务。
# 注意：
#   该脚本会使用测试用户 1001/1002，并创建临时上传文件。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/imm-server}"

: "${GATE_URL:=http://127.0.0.1:18080}"
: "${MYSQL_HOST:=127.0.0.1}"
: "${MYSQL_PORT:=3306}"
: "${IMM_DB:=chatServ}"
: "${IMM_DB_USER:=zorjen}"
: "${IMM_DB_PASSWORD:=root}"
: "${IMM_REDIS_HOST:=127.0.0.1}"
: "${IMM_REDIS_PORT:=6380}"
: "${IMM_REDIS_PASSWORD:=root}"

timeout 3 redis-cli -h "$IMM_REDIS_HOST" -p "$IMM_REDIS_PORT" -a "$IMM_REDIS_PASSWORD" \
  DEL im:user:1001 im:user:1002 >/dev/null 2>&1 || true

stamp="$(date +%s)"
text_payload="hello_from_smoke_$stamp"
upload_file="imm_upload_smoke_$stamp.txt"
upload_src="/tmp/$upload_file"
upload_file_id="smoke-file-$stamp"
upload_dst="$BUILD_DIR/file/imm-files/1001/$upload_file_id"
cleanup_files() {
  rm -f "$upload_src" "$upload_dst"
}
trap cleanup_files EXIT

require_error_zero() {
  local label="$1"
  local body="$2"
  if ! grep -q '"error" : 0' <<<"$body"; then
    echo "$label failed:"
    echo "$body"
    exit 1
  fi
  echo "$label ok"
}

signin_rsp="$(curl --max-time 10 -sS -H 'Content-Type: application/json' \
  -d '{"username":"zorjen","password":"123456"}' \
  "$GATE_URL/post-signIn")"
require_error_zero signin "$signin_rsp"
grep -q '"ip" : "127.0.0.1"' <<<"$signin_rsp"
read -r gateway_host gateway_port < <(
  python3 "$ROOT_DIR/scripts/smoke/src/smoke_support.py" \
    json-fields ip port <<<"$signin_rsp"
)
if [[ -z "$gateway_host" || ! "$gateway_port" =~ ^[0-9]+$ || "$gateway_port" == "0" ]]; then
  echo "state route did not return a usable Gateway endpoint"
  exit 1
fi
echo "state route ok: $gateway_host:$gateway_port"

printf 'file smoke %s\n' "$stamp" > "$upload_src"

GATEWAY_HOST="$gateway_host" \
GATEWAY_PORT="$gateway_port" \
GATE_URL="$GATE_URL" \
TEXT_PAYLOAD="$text_payload" \
ONLINE_PAYLOAD="hello_online_$stamp" \
UPLOAD_PATH="$upload_src" \
UPLOAD_FILE_ID="$upload_file_id" \
python3 "$ROOT_DIR/scripts/smoke/src/smoke_base.py"

for _ in {1..10}; do
  text_count="$(mysql --protocol=tcp -h"$MYSQL_HOST" -P"$MYSQL_PORT" \
    -u"$IMM_DB_USER" -p"$IMM_DB_PASSWORD" -N -B \
    -e "SELECT COUNT(*) FROM $IMM_DB.messages WHERE content='$text_payload';" 2>/dev/null)"
  if [[ "$text_count" == "1" ]]; then
    echo "chat text ok"
    break
  fi
  sleep 1
done
if [[ "${text_count:-0}" != "1" ]]; then
  echo "chat text message was not persisted"
  exit 1
fi

for _ in {1..10}; do
  if [[ -f "$upload_dst" ]] && cmp -s "$upload_src" "$upload_dst"; then
    echo "file upload ok"
    break
  fi
  sleep 1
done
if [[ ! -f "$upload_dst" ]] || ! cmp -s "$upload_src" "$upload_dst"; then
  echo "file upload did not create expected file: $upload_dst"
  exit 1
fi

echo "smoke ok"
