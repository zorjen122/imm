#!/usr/bin/env bash
set -euo pipefail

# Verifies the first-phase Connection Gateway topology with G=2 and N=2.
# The test uses the three default users created by init-mysql.sh and writes
# temporary group, conversation, file and message rows to chatServ.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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

python3 - "$GATEWAY_A_HOST" "$GATEWAY_A_PORT" "$GATEWAY_B_HOST" "$GATEWAY_B_PORT" <<'PY'
import socket
import sys

for label, host, port in (
    ("gateway A", sys.argv[1], int(sys.argv[2])),
    ("gateway B", sys.argv[3], int(sys.argv[4])),
):
    try:
        with socket.create_connection((host, port), timeout=2):
            pass
    except OSError as exc:
        raise SystemExit(f"{label} is not reachable at {host}:{port}: {exc}")
PY

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
PYTHONPATH="$ROOT_DIR/scripts/lib${PYTHONPATH:+:$PYTHONPATH}" \
python3 - <<'PY'
import json
import os
import subprocess
import hashlib
import uuid

from wimi_tcp_client import WimClient, request_chat_auth, require

ID_PULL_SESSION_MESSAGE_LIST_REQ = 1005
ID_GROUP_PULL_MEMBER_REQ = 1009
ID_TEXT_SEND_REQ = 1027
ID_GROUP_TEXT_SEND_REQ = 1043
ID_FILE_UPLOAD_REQ = 1031
ID_FILE_DOWNLOAD_REQ = 1051
ID_GROUP_FILE_SEND_REQ = 1055
ID_GROUP_CREATE_REQ = 1035
ID_GROUP_NOTIFY_JOIN_REQ = 1037
ID_GROUP_REPLY_JOIN_REQ = 1039
ID_GROUP_QUIT_REQ = 1041
ID_GROUP_KICK_MEMBER_REQ = 1053

uid_a = int(os.environ["UID_A"])
uid_b = int(os.environ["UID_B"])
uid_c = int(os.environ["UID_C"])
conversation_id = int(os.environ["CONVERSATION_ID"])
group_id = int(os.environ["GROUP_ID"])
group_conversation_id = int(os.environ["GROUP_CONVERSATION_ID"])
payload = os.environ["PAYLOAD"]
client_message_id = os.environ["CLIENT_MESSAGE_ID"]


def lease(uid, device_id):
    value = subprocess.check_output(
        [
            "redis-cli", "-h", os.environ["WIMI_REDIS_HOST"],
            "-p", os.environ["WIMI_REDIS_PORT"],
            "-a", os.environ["WIMI_REDIS_PASSWORD"],
            "GET", f"im:session:{uid}:{device_id}",
        ],
        stderr=subprocess.DEVNULL,
        text=True,
    ).strip()
    require(value, f"missing session lease for {uid}")
    return json.loads(value)


auth_a = request_chat_auth(os.environ["USER_A"], "123456", os.environ["GATE_URL"])
auth_b = request_chat_auth(os.environ["USER_B"], "123456", os.environ["GATE_URL"])
auth_c = request_chat_auth(os.environ["USER_C"], "123456", os.environ["GATE_URL"])
a = WimClient(uid_a, os.environ["GATEWAY_A_HOST"],
              int(os.environ["GATEWAY_A_PORT"]), auth_token=auth_a["chatToken"])
b = WimClient(uid_b, os.environ["GATEWAY_B_HOST"],
              int(os.environ["GATEWAY_B_PORT"]), auth_token=auth_b["chatToken"])
c = WimClient(uid_c, os.environ["GATEWAY_B_HOST"],
              int(os.environ["GATEWAY_B_PORT"]), auth_token=auth_c["chatToken"])
a_secondary = WimClient(
    uid_a, os.environ["GATEWAY_B_HOST"], int(os.environ["GATEWAY_B_PORT"]),
    auth_token=auth_a["chatToken"])
b_secondary = WimClient(
    uid_b, os.environ["GATEWAY_A_HOST"], int(os.environ["GATEWAY_A_PORT"]),
    auth_token=auth_b["chatToken"])

try:
    a.login(init=False)
    b.login(init=False)
    c.login(init=False)
    a_secondary.login(init=False)
    b_secondary.login(init=False)
    lease_a = lease(uid_a, a.device_id)
    lease_b = lease(uid_b, b.device_id)
    lease_a_secondary = lease(uid_a, a_secondary.device_id)
    lease_b_secondary = lease(uid_b, b_secondary.device_id)
    require(lease_a["gatewayId"] == os.environ["GATEWAY_A_ID"], lease_a)
    require(lease_b["gatewayId"] == os.environ["GATEWAY_B_ID"], lease_b)
    require(lease_a_secondary["gatewayId"] == os.environ["GATEWAY_B_ID"],
            lease_a_secondary)
    require(lease_b_secondary["gatewayId"] == os.environ["GATEWAY_A_ID"],
            lease_b_secondary)
    require(int(lease_a["generation"]) > 0 and int(lease_b["generation"]) > 0,
            "lease generation was not allocated")

    command = {
        "seq": int(client_message_id.split("_")[-1]) % 9_000_000_000 + 1,
        "to": uid_b,
        "data": payload,
        "conversationId": conversation_id,
        "clientMessageId": client_message_id,
    }
    accepted = a.request(ID_TEXT_SEND_REQ, command)
    require(accepted.get("error") == 0, accepted)
    require(accepted.get("messageState") == 1, accepted)
    require(accepted.get("conversationId") == conversation_id, accepted)
    require(accepted.get("conversationSeq", 0) > 0, accepted)

    pushed = b.expect_async(
        ID_TEXT_SEND_REQ,
        lambda item: item.get("clientMessageId") == client_message_id,
    )
    require(pushed.get("messageId") == accepted.get("messageId"), pushed)
    require(pushed.get("conversationSeq") == accepted.get("conversationSeq"), pushed)
    pushed_secondary = b_secondary.expect_async(
        ID_TEXT_SEND_REQ,
        lambda item: item.get("clientMessageId") == client_message_id,
    )
    sender_secondary = a_secondary.expect_async(
        ID_TEXT_SEND_REQ,
        lambda item: item.get("clientMessageId") == client_message_id,
    )
    require(pushed_secondary.get("messageId") == accepted.get("messageId"),
            pushed_secondary)
    require(sender_secondary.get("messageId") == accepted.get("messageId"),
            sender_secondary)
    b.ack(
        pushed["seq"], receipt_type=1,
        conversation_id=conversation_id,
        conversation_seq=pushed["conversationSeq"],
    )
    b_secondary.ack(
        pushed_secondary["seq"], receipt_type=1,
        conversation_id=conversation_id,
        conversation_seq=pushed_secondary["conversationSeq"],
    )
    a_secondary.ack(
        sender_secondary["seq"], receipt_type=1,
        conversation_id=conversation_id,
        conversation_seq=sender_secondary["conversationSeq"],
    )
    a_secondary.quit(wait_response=True)
    b_secondary.quit(wait_response=True)

    duplicate = a.request(ID_TEXT_SEND_REQ, command)
    require(duplicate.get("error") == 0, duplicate)
    require(duplicate.get("messageId") == accepted.get("messageId"), duplicate)
    require(duplicate.get("conversationSeq") == accepted.get("conversationSeq"), duplicate)

    # Reuse the same sender/client_message_id while changing the content. This
    # proves the Message node compares the persisted command hash instead of
    # treating every duplicate idempotency key as a successful replay. Such a
    # conflict is deterministic, so the Gateway/client must not retry it.
    conflict = dict(command)
    conflict["data"] = payload + "_conflict"
    conflict_rsp = a.request(ID_TEXT_SEND_REQ, conflict)
    require(conflict_rsp.get("error", 0) != 0, conflict_rsp)
    require(not conflict_rsp.get("retryable", True), conflict_rsp)

    sync = b.request(
        ID_PULL_SESSION_MESSAGE_LIST_REQ,
        {"conversationId": conversation_id, "afterSeq": 0, "limit": 20},
    )
    require(sync.get("error") == 0, sync)
    matches = [item for item in sync["messageList"]
               if item["messageId"] == accepted["messageId"]]
    require(len(matches) == 1, sync)
    require(matches[0]["conversationSeq"] == accepted["conversationSeq"], sync)

    group_first = a.request(
        ID_GROUP_TEXT_SEND_REQ,
        {
            "seq": command["seq"] + 1,
            "groupId": group_id,
            "data": payload + "_group_a",
            "conversationId": group_conversation_id,
            "clientMessageId": client_message_id + "_group_a",
        },
    )
    require(group_first.get("error") == 0, group_first)
    require(group_first.get("conversationSeq") == 1, group_first)
    group_push_b = b.expect_async(
        ID_GROUP_TEXT_SEND_REQ,
        lambda item: item.get("clientMessageId") == client_message_id + "_group_a",
    )
    b.ack(group_push_b["seq"], conversation_id=group_conversation_id,
          conversation_seq=group_push_b["conversationSeq"])

    group_second = b.request(
        ID_GROUP_TEXT_SEND_REQ,
        {
            "seq": command["seq"] + 2,
            "groupId": group_id,
            "data": payload + "_group_b",
            "conversationId": group_conversation_id,
            "clientMessageId": client_message_id + "_group_b",
        },
    )
    require(group_second.get("error") == 0, group_second)
    require(group_second.get("conversationSeq") == 2, group_second)
    group_push_a = a.expect_async(
        ID_GROUP_TEXT_SEND_REQ,
        lambda item: item.get("clientMessageId") == client_message_id + "_group_b",
    )
    a.ack(group_push_a["seq"], conversation_id=group_conversation_id,
          conversation_seq=group_push_a["conversationSeq"])

    group_sync = a.request(
        ID_PULL_SESSION_MESSAGE_LIST_REQ,
        {"conversationId": group_conversation_id, "afterSeq": 0, "limit": 20},
    )
    require(group_sync.get("error") == 0, group_sync)
    require([item["conversationSeq"] for item in group_sync["messageList"]]
            == [1, 2], group_sync)

    # An application is not membership. Only the owner may reject/approve it,
    # and a rejected user can apply again and receive a fresh join timestamp.
    require(c.request(ID_GROUP_NOTIFY_JOIN_REQ, {
        "groupId": group_id, "requestMessage": "outsider join",
    }).get("error") == 0, "outsider application failed")
    pending_outsider = c.request(ID_GROUP_PULL_MEMBER_REQ, {"groupId": group_id})
    require(pending_outsider.get("error") != 0, pending_outsider)
    unauthorized_approval = b.request(ID_GROUP_REPLY_JOIN_REQ, {
        "groupId": group_id, "requestorUid": uid_c, "accept": True,
    })
    require(unauthorized_approval.get("error") != 0, unauthorized_approval)
    unauthorized_rejection = b.request(ID_GROUP_REPLY_JOIN_REQ, {
        "groupId": group_id, "requestorUid": uid_c, "accept": False,
    })
    require(unauthorized_rejection.get("error") != 0, unauthorized_rejection)
    unauthorized_member_kick = b.request(ID_GROUP_KICK_MEMBER_REQ, {
        "groupId": group_id, "requestorUid": uid_a,
    })
    require(unauthorized_member_kick.get("error") != 0,
            unauthorized_member_kick)
    unauthorized_outsider_kick = c.request(ID_GROUP_KICK_MEMBER_REQ, {
        "groupId": group_id, "requestorUid": uid_b,
    })
    require(unauthorized_outsider_kick.get("error") != 0,
            unauthorized_outsider_kick)
    member_view = b.request(ID_GROUP_PULL_MEMBER_REQ, {"groupId": group_id})
    require(member_view.get("error") == 0 and not member_view["applyList"],
            member_view)
    require(member_view.get("groupName") == "GatewayGroup", member_view)
    owner_view = a.request(ID_GROUP_PULL_MEMBER_REQ, {"groupId": group_id})
    require(owner_view.get("error") == 0, owner_view)
    require(owner_view.get("groupName") == "GatewayGroup", owner_view)
    require(any(item["from"] == uid_c for item in owner_view["applyList"]),
            owner_view)
    require(a.request(ID_GROUP_REPLY_JOIN_REQ, {
        "groupId": group_id, "requestorUid": uid_c, "accept": False,
    }).get("error") == 0, "owner rejection failed")
    require(c.request(ID_GROUP_NOTIFY_JOIN_REQ, {
        "groupId": group_id, "requestMessage": "outsider reapply",
    }).get("error") == 0, "outsider reapplication failed")
    require(a.request(ID_GROUP_REPLY_JOIN_REQ, {
        "groupId": group_id, "requestorUid": uid_c, "accept": True,
    }).get("error") == 0, "owner approval failed")
    for member_client in (a, b, c):
        joined_c = member_client.expect_async(
            ID_GROUP_REPLY_JOIN_REQ,
            lambda item: item.get("requestorUid") == uid_c and
            item.get("groupChangeKind") == 1,
        )
        require(joined_c.get("accept"), joined_c)
        require(joined_c.get("groupName") == "GatewayGroup", joined_c)
    approved_view = a.request(ID_GROUP_PULL_MEMBER_REQ, {"groupId": group_id})
    approved_c = next(item for item in approved_view["memberList"]
                      if item["uid"] == uid_c)
    require(approved_c["joinTime"] != "2026-07-16 00:00:00", approved_c)
    require(c.request(ID_GROUP_QUIT_REQ, {"groupId": group_id}).get("error") == 0,
            "approved outsider quit failed")
    for member_client in (a, b, c):
        left_c = member_client.expect_async(
            ID_GROUP_REPLY_JOIN_REQ,
            lambda item: item.get("requestorUid") == uid_c and
            item.get("groupChangeKind") == 2,
        )
        require(left_c.get("groupName") == "GatewayGroup", left_c)

    # A completed owner file must be accepted as a group message and readable
    # by a current member through the same membership boundary.
    file_data = b"group-file-smoke"
    file_meta = {
        "fileId": str(uuid.uuid4()), "fileName": "group-smoke.txt",
        "fileType": "text/plain", "fileSize": len(file_data),
        "sha256": hashlib.sha256(file_data).hexdigest(),
    }
    upload = a.request(ID_FILE_UPLOAD_REQ, {"file": file_meta, "data": file_data, "fileOffset": 0, "fileComplete": True})
    require(upload.get("error") == 0 and upload.get("fileComplete"), upload)
    group_file = a.request(ID_GROUP_FILE_SEND_REQ, {
        "groupId": group_id, "file": file_meta, "clientMessageId": client_message_id + "_group_file",
        "conversationId": group_conversation_id,
    })
    require(group_file.get("error") == 0, group_file)
    file_push = b.expect_async(ID_GROUP_FILE_SEND_REQ,
        lambda item: item.get("clientMessageId") == client_message_id + "_group_file")
    b.ack(file_push["seq"], conversation_id=group_conversation_id, conversation_seq=file_push["conversationSeq"])
    downloaded = b.request(ID_FILE_DOWNLOAD_REQ, {"file": file_meta, "fileOffset": 0, "limit": 1024})
    require(downloaded.get("error") == 0 and downloaded.get("data") == file_data.decode(), downloaded)

    def upload_owned_file(client, label):
        data = (label + "-owned-group-file").encode()
        meta = {
            "fileId": str(uuid.uuid4()), "fileName": label + ".txt",
            "fileType": "text/plain", "fileSize": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        result = client.request(ID_FILE_UPLOAD_REQ, {
            "file": meta, "data": data, "fileOffset": 0, "fileComplete": True,
        })
        require(result.get("error") == 0 and result.get("fileComplete"), result)
        return meta

    file_meta_b = upload_owned_file(b, "member-b")
    file_meta_c = upload_owned_file(c, "outsider-c")

    def require_revoked(client, owned_file, label):
        denied = {
            "text": client.request(ID_GROUP_TEXT_SEND_REQ, {
                "groupId": group_id, "data": label + "-text",
                "clientMessageId": client_message_id + "_" + label + "_text",
            }),
            "file": client.request(ID_GROUP_FILE_SEND_REQ, {
                "groupId": group_id, "file": owned_file,
                "clientMessageId": client_message_id + "_" + label + "_file",
            }),
            "sync": client.request(ID_PULL_SESSION_MESSAGE_LIST_REQ, {
                "conversationId": group_conversation_id, "afterSeq": 0,
                "limit": 20,
            }),
            "members": client.request(ID_GROUP_PULL_MEMBER_REQ, {
                "groupId": group_id,
            }),
            "download": client.request(ID_FILE_DOWNLOAD_REQ, {
                "file": file_meta, "fileOffset": 0, "limit": 1024,
            }),
        }
        for operation, response in denied.items():
            require(response.get("error") != 0,
                    f"{label} {operation} unexpectedly succeeded: {response}")

    # C owns a completed file but is not a member, so every group boundary
    # still rejects it.
    require_revoked(c, file_meta_c, "outsider")

    # Leaving and kicking revoke all five protected operations immediately.
    require(b.request(ID_GROUP_QUIT_REQ, {"groupId": group_id}).get("error") == 0, "member quit failed")
    for member_client in (a, b):
        member_client.expect_async(
            ID_GROUP_REPLY_JOIN_REQ,
            lambda item: item.get("requestorUid") == uid_b and
            item.get("groupChangeKind") == 2,
        )
    require_revoked(b, file_meta_b, "after_quit")
    require(b.request(ID_GROUP_NOTIFY_JOIN_REQ, {"groupId": group_id, "requestMessage": "rejoin"}).get("error") == 0, "rejoin request failed")
    require(a.request(ID_GROUP_REPLY_JOIN_REQ, {"groupId": group_id, "requestorUid": uid_b, "accept": True}).get("error") == 0, "rejoin approval failed")
    rejoined_view = a.request(ID_GROUP_PULL_MEMBER_REQ, {"groupId": group_id})
    rejoined_b = next(item for item in rejoined_view["memberList"]
                      if item["uid"] == uid_b)
    require(rejoined_b["joinTime"] != "2026-07-16 00:00:00", rejoined_b)
    require(a.request(ID_GROUP_KICK_MEMBER_REQ, {"groupId": group_id, "requestorUid": uid_b}).get("error") == 0, "kick failed")
    for member_client in (a, b):
        member_client.expect_async(
            ID_GROUP_REPLY_JOIN_REQ,
            lambda item: item.get("requestorUid") == uid_b and
            item.get("groupChangeKind") == 3,
        )
    require_revoked(b, file_meta_b, "after_kick")

    # Messages created while B is absent remain outside B's next membership
    # window; only messages sent after reapproval are visible in sync.
    absent_message = a.request(ID_GROUP_TEXT_SEND_REQ, {
        "groupId": group_id, "data": "while-b-absent",
        "clientMessageId": client_message_id + "_while_b_absent",
    })
    require(absent_message.get("error") == 0, absent_message)
    require(b.request(ID_GROUP_NOTIFY_JOIN_REQ, {
        "groupId": group_id, "requestMessage": "second rejoin",
    }).get("error") == 0, "second rejoin request failed")
    require(a.request(ID_GROUP_REPLY_JOIN_REQ, {
        "groupId": group_id, "requestorUid": uid_b, "accept": True,
    }).get("error") == 0, "second rejoin approval failed")
    visible_message = a.request(ID_GROUP_TEXT_SEND_REQ, {
        "groupId": group_id, "data": "after-b-rejoined",
        "clientMessageId": client_message_id + "_after_b_rejoined",
    })
    require(visible_message.get("error") == 0, visible_message)
    rejoined_sync = b.request(ID_PULL_SESSION_MESSAGE_LIST_REQ, {
        "conversationId": group_conversation_id, "afterSeq": 0, "limit": 20,
    })
    require(rejoined_sync.get("error") == 0, rejoined_sync)
    rejoined_ids = {item["messageId"] for item in rejoined_sync["messageList"]}
    require(absent_message["messageId"] not in rejoined_ids, rejoined_sync)
    require(visible_message["messageId"] in rejoined_ids, rejoined_sync)

    # Owner exit dissolves the group and revokes both the former owner and all
    # remaining members.
    require(a.request(ID_GROUP_QUIT_REQ, {"groupId": group_id}).get("error") == 0, "owner dissolve failed")
    a.expect_async(ID_GROUP_REPLY_JOIN_REQ,
                   lambda item: item.get("requestorUid") == uid_a and
                   item.get("groupChangeKind") == 4)
    b.expect_async(ID_GROUP_REPLY_JOIN_REQ,
                   lambda item: item.get("requestorUid") == uid_a and
                   item.get("groupChangeKind") == 4)
    require_revoked(a, file_meta, "owner_after_dissolve")
    require_revoked(b, file_meta_b, "member_after_dissolve")

    # Exercise the real create/join path used by the UI. The second join must
    # carry the group name to an existing member, and a following group message
    # must fan out to both members.
    ui_group = a.request(ID_GROUP_CREATE_REQ, {"groupName": "UI-regression-group"})
    require(ui_group.get("error") == 0, ui_group)
    ui_group_id = ui_group["groupId"]
    require(b.request(ID_GROUP_NOTIFY_JOIN_REQ, {
        "groupId": ui_group_id, "requestMessage": "first UI member",
    }).get("error") == 0, "first UI join request failed")
    require(a.request(ID_GROUP_REPLY_JOIN_REQ, {
        "groupId": ui_group_id, "requestorUid": uid_b, "accept": True,
    }).get("error") == 0, "first UI join approval failed")
    for member_client in (a, b):
        joined = member_client.expect_async(
            ID_GROUP_REPLY_JOIN_REQ,
            lambda item: item.get("groupId") == ui_group_id and
            item.get("requestorUid") == uid_b and
            item.get("groupChangeKind") == 1,
        )
        require(joined.get("groupName") == "UI-regression-group", joined)
    require(c.request(ID_GROUP_NOTIFY_JOIN_REQ, {
        "groupId": ui_group_id, "requestMessage": "second UI member",
    }).get("error") == 0, "second UI join request failed")
    require(a.request(ID_GROUP_REPLY_JOIN_REQ, {
        "groupId": ui_group_id, "requestorUid": uid_c, "accept": True,
    }).get("error") == 0, "second UI join approval failed")
    for member_client in (a, b, c):
        joined = member_client.expect_async(
            ID_GROUP_REPLY_JOIN_REQ,
            lambda item: item.get("groupId") == ui_group_id and
            item.get("requestorUid") == uid_c and
            item.get("groupChangeKind") == 1,
        )
        require(joined.get("groupName") == "UI-regression-group", joined)
    ui_group_view = c.request(ID_GROUP_PULL_MEMBER_REQ, {"groupId": ui_group_id})
    require(ui_group_view.get("groupName") == "UI-regression-group", ui_group_view)
    ui_message_id = client_message_id + "_ui_group"
    ui_message = a.request(ID_GROUP_TEXT_SEND_REQ, {
        "groupId": ui_group_id, "data": "UI group message",
        "clientMessageId": ui_message_id,
    })
    require(ui_message.get("error") == 0, ui_message)
    for member_client in (b, c):
        pushed = member_client.expect_async(
            ID_GROUP_TEXT_SEND_REQ,
            lambda item: item.get("clientMessageId") == ui_message_id,
        )
        require(pushed.get("messageId") == ui_message.get("messageId"), pushed)
    require(a.request(ID_GROUP_QUIT_REQ, {"groupId": ui_group_id}).get("error") == 0,
            "UI regression group cleanup failed")

    with open(os.environ["RESULT_FILE"], "w", encoding="utf-8") as output:
        json.dump(accepted, output)
finally:
    a.quit()
    b.quit()
    c.quit()
    a_secondary.close()
    b_secondary.close()
PY

message_id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["messageId"])' "$result_file")"
conversation_seq="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["conversationSeq"])' "$result_file")"

row_count="$(mysql_exec -N -B -e "SELECT COUNT(*) FROM messages WHERE senderId=$uid_a AND clientMessageId='$client_message_id' AND messageId=$message_id AND conversationSeq=$conversation_seq;" 2>/dev/null)"
if [[ "$row_count" != "1" ]]; then
  echo "persistent idempotency failed: expected one row, got $row_count" >&2
  exit 1
fi

echo "cross-Gateway persistent message closure ok: message=$message_id seq=$conversation_seq"
echo "group authorization matrix ok: outsider, quit, kick, rejoin window, dissolve"
echo "group UI path ok: name sync, existing-member notice, message fan-out"
echo "gateway message smoke ok"
