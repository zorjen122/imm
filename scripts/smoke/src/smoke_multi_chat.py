import json
import os
import subprocess
import time

from wimi_tcp_client import Operation, PushType, WimClient, is_retryable_error, request_chat_auth, require

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

NOTIFY_MESSAGE_READ = 2

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
        Operation.OP_ADD_FRIEND,
        {"from": UID_A, "to": UID_B, "requestMessage": FRIEND_MSG},
    )
    require(apply_rsp.get("error") == 0, f"cross-node friend apply failed: {apply_rsp}")
    b.expect_push(
        PushType.PUSH_TYPE_FRIEND_REQUEST,
        lambda payload: payload.get("from") == UID_A
        and payload.get("to") == UID_B
        and payload.get("requestMessage") == FRIEND_MSG,
    )
    print("cross-node friend apply push ok")

    apply_pull = b.request(Operation.OP_PULL_FRIEND_APPLICATIONS, {"uid": UID_B})
    require(apply_pull.get("error") == 0, f"pull friend apply failed: {apply_pull}")
    require(
        any(item.get("content") == FRIEND_MSG for item in apply_pull.get("applyList", [])),
        f"pulled friend applies missing {FRIEND_MSG}: {apply_pull}",
    )

    reply_rsp = b.request(
        Operation.OP_REPLY_FRIEND,
        {"from": UID_B, "to": UID_A, "accept": True, "replyMessage": FRIEND_REPLY},
    )
    require(reply_rsp.get("error") == 0, f"cross-node friend reply failed: {reply_rsp}")
    a.expect_push(
        PushType.PUSH_TYPE_FRIEND_REPLY,
        lambda payload: payload.get("from") == UID_B
        and payload.get("to") == UID_A
        and payload.get("accept") is True,
    )
    print("cross-node friend reply push ok")

    friends_a = a.request(Operation.OP_PULL_FRIEND_LIST, {"uid": UID_A})
    friends_b = b.request(Operation.OP_PULL_FRIEND_LIST, {"uid": UID_B})
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
        Operation.OP_SEND_TEXT,
        {
            "from": UID_A,
            "to": UID_B,
            "clientMessageId": client_message_id,
            "data": TEXT_PAYLOAD,
        },
    )
    require(text_rsp.get("error") == 0, f"cross-node text response failed: {text_rsp}")
    require(text_rsp.get("messageId", 0) > 0, f"cross-node message id missing: {text_rsp}")

    text_push = b.expect_push(
        PushType.PUSH_TYPE_TEXT_MESSAGE,
        lambda payload: payload.get("from") == UID_A
        and payload.get("to") == UID_B
        and payload.get("data") == TEXT_PAYLOAD,
    )
    server_seq = int(text_push["messageId"])
    require(text_rsp["messageId"] == server_seq,
            f"response/push message id mismatch: {text_rsp}, {text_push}")
    b.ack(server_seq, conversation_id=text_push["conversationId"],
          conversation_seq=text_push["conversationSeq"])

    # Notify the sender when the receiver reads the message.
    b.ack(server_seq, receipt_type=NOTIFY_MESSAGE_READ,
          conversation_id=text_push["conversationId"],
          conversation_seq=text_push["conversationSeq"])
    read_notify = a.expect_push(
      PushType.PUSH_TYPE_READ_RECEIPT,
      lambda payload: payload.get("conversationId") == text_push["conversationId"]
      and payload.get("conversationSeq") == text_push["conversationSeq"]
      and payload.get("messageId") == server_seq
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
        delayed_first = a.request(Operation.OP_SEND_TEXT, delayed_body)
        require(delayed_first.get("error", 0) != 0 and
                is_retryable_error(delayed_first.get("error")),
                f"delayed RPC did not time out retryably: {delayed_first}")
        delayed_push = b.expect_push(
            PushType.PUSH_TYPE_TEXT_MESSAGE,
            lambda payload: payload.get("data") == "rpc_delay_after_accept",
        )
        delayed_server_seq = int(delayed_push["messageId"])

        delayed_body["requestTimeoutMs"] = 2000
        delayed_retry = a.request(Operation.OP_SEND_TEXT, delayed_body)
        require(delayed_retry.get("error", 0) != 0 and
                not is_retryable_error(delayed_retry.get("error")),
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
