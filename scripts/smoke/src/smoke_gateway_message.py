import json
import os
import subprocess
import hashlib
import uuid

from imm_tcp_client import Operation, PushType, ImmClient, is_retryable_error, request_chat_auth, require

OP_PULL_CONVERSATION_MESSAGES = Operation.OP_PULL_CONVERSATION_MESSAGES
OP_PULL_GROUP_MEMBERS = Operation.OP_PULL_GROUP_MEMBERS
OP_SEND_TEXT = Operation.OP_SEND_TEXT
OP_SEND_GROUP_TEXT = Operation.OP_SEND_GROUP_TEXT
OP_UPLOAD_FILE = Operation.OP_UPLOAD_FILE
OP_DOWNLOAD_FILE = Operation.OP_DOWNLOAD_FILE
OP_SEND_GROUP_FILE = Operation.OP_SEND_GROUP_FILE
OP_CREATE_GROUP = Operation.OP_CREATE_GROUP
OP_JOIN_GROUP = Operation.OP_JOIN_GROUP
OP_REPLY_JOIN_GROUP = Operation.OP_REPLY_JOIN_GROUP
OP_QUIT_GROUP = Operation.OP_QUIT_GROUP
OP_KICK_GROUP_MEMBER = Operation.OP_KICK_GROUP_MEMBER

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
            "redis-cli", "-h", os.environ["IMM_REDIS_HOST"],
            "-p", os.environ["IMM_REDIS_PORT"],
            "-a", os.environ["IMM_REDIS_PASSWORD"],
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
a = ImmClient(uid_a, os.environ["GATEWAY_A_HOST"],
              int(os.environ["GATEWAY_A_PORT"]), auth_token=auth_a["chatToken"])
b = ImmClient(uid_b, os.environ["GATEWAY_B_HOST"],
              int(os.environ["GATEWAY_B_PORT"]), auth_token=auth_b["chatToken"])
c = ImmClient(uid_c, os.environ["GATEWAY_B_HOST"],
              int(os.environ["GATEWAY_B_PORT"]), auth_token=auth_c["chatToken"])
a_secondary = ImmClient(
    uid_a, os.environ["GATEWAY_B_HOST"], int(os.environ["GATEWAY_B_PORT"]),
    auth_token=auth_a["chatToken"])
b_secondary = ImmClient(
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

    # A 发送到 B
    command = {
        "to": uid_b,
        "data": payload,
        "conversationId": conversation_id,
        "clientMessageId": client_message_id,
    }
    accepted = a.request(OP_SEND_TEXT, command)
    require(accepted.get("error") == 0, accepted)
    require(accepted.get("conversationId") == conversation_id, accepted)
    require(accepted.get("conversationSeq", 0) > 0, accepted)

    # 用户的多个设备会接收到该消息，同时回传 ACK
    # 发送者的其他设备也会收到这条发送的消息，因为消息是由发送者的其中一个设备发送的
    pushed = b.expect_push(
        PushType.PUSH_TYPE_TEXT_MESSAGE,
        lambda item: item.get("clientMessageId") == client_message_id,
    )
    require(pushed.get("messageId") == accepted.get("messageId"), pushed)
    require(pushed.get("conversationSeq") == accepted.get("conversationSeq"), pushed)
    pushed_secondary = b_secondary.expect_push(
        PushType.PUSH_TYPE_TEXT_MESSAGE,
        lambda item: item.get("clientMessageId") == client_message_id,
    )
    sender_secondary = a_secondary.expect_push(
        PushType.PUSH_TYPE_TEXT_MESSAGE,
        lambda item: item.get("clientMessageId") == client_message_id,
    )
    require(pushed_secondary.get("messageId") == accepted.get("messageId"),
            pushed_secondary)
    require(sender_secondary.get("messageId") == accepted.get("messageId"),
            sender_secondary)
    b.ack(
        pushed["messageId"], receipt_type=1,
        conversation_id=conversation_id,
        conversation_seq=pushed["conversationSeq"],
    )
    b_secondary.ack(
        pushed_secondary["messageId"], receipt_type=1,
        conversation_id=conversation_id,
        conversation_seq=pushed_secondary["conversationSeq"],
    )
    a_secondary.ack(
        sender_secondary["messageId"], receipt_type=1,
        conversation_id=conversation_id,
        conversation_seq=sender_secondary["conversationSeq"],
    )
    a_secondary.quit(wait_response=True)
    b_secondary.quit(wait_response=True)

    duplicate = a.request(OP_SEND_TEXT, command)
    require(duplicate.get("error") == 0, duplicate)
    require(duplicate.get("messageId") == accepted.get("messageId"), duplicate)
    require(duplicate.get("conversationSeq") == accepted.get("conversationSeq"), duplicate)

    # Reuse the same sender/client_message_id while changing the content. This
    # proves the Message node compares the persisted command hash instead of
    # treating every duplicate idempotency key as a successful replay. Such a
    # conflict is deterministic, so the Gateway/client must not retry it.
    conflict = dict(command)
    conflict["data"] = payload + "_conflict"
    conflict_rsp = a.request(OP_SEND_TEXT, conflict)
    require(conflict_rsp.get("error", 0) != 0, conflict_rsp)
    require(not is_retryable_error(conflict_rsp.get("error", 0)), conflict_rsp)

    sync = b.request(
        OP_PULL_CONVERSATION_MESSAGES,
        {"conversationId": conversation_id, "afterSeq": 0, "limit": 20},
    )
    require(sync.get("error") == 0, sync)
    matches = [item for item in sync["messageList"]
               if item["messageId"] == accepted["messageId"]]
    require(len(matches) == 1, sync)
    require(matches[0]["conversationSeq"] == accepted["conversationSeq"], sync)

    group_first = a.request(
        OP_SEND_GROUP_TEXT,
        {
            "groupId": group_id,
            "data": payload + "_group_a",
            "conversationId": group_conversation_id,
            "clientMessageId": client_message_id + "_group_a",
        },
    )
    require(group_first.get("error") == 0, group_first)
    require(group_first.get("conversationSeq") == 1, group_first)
    group_push_b = b.expect_push(
        PushType.PUSH_TYPE_TEXT_MESSAGE,
        lambda item: item.get("clientMessageId") == client_message_id + "_group_a",
    )
    b.ack(group_push_b["messageId"], conversation_id=group_conversation_id,
          conversation_seq=group_push_b["conversationSeq"])

    group_second = b.request(
        OP_SEND_GROUP_TEXT,
        {
            "groupId": group_id,
            "data": payload + "_group_b",
            "conversationId": group_conversation_id,
            "clientMessageId": client_message_id + "_group_b",
        },
    )
    require(group_second.get("error") == 0, group_second)
    require(group_second.get("conversationSeq") == 2, group_second)
    group_push_a = a.expect_push(
        PushType.PUSH_TYPE_TEXT_MESSAGE,
        lambda item: item.get("clientMessageId") == client_message_id + "_group_b",
    )
    a.ack(group_push_a["messageId"], conversation_id=group_conversation_id,
          conversation_seq=group_push_a["conversationSeq"])

    group_sync = a.request(
        OP_PULL_CONVERSATION_MESSAGES,
        {"conversationId": group_conversation_id, "afterSeq": 0, "limit": 20},
    )
    require(group_sync.get("error") == 0, group_sync)
    require([item["conversationSeq"] for item in group_sync["messageList"]]
            == [1, 2], group_sync)

    # An application is not membership. Only the owner may reject/approve it,
    # and a rejected user can apply again and receive a fresh join timestamp.
    require(c.request(OP_JOIN_GROUP, {
        "groupId": group_id, "requestMessage": "outsider join",
    }).get("error") == 0, "outsider application failed")
    pending_outsider = c.request(OP_PULL_GROUP_MEMBERS, {"groupId": group_id})
    require(pending_outsider.get("error") != 0, pending_outsider)
    unauthorized_approval = b.request(OP_REPLY_JOIN_GROUP, {
        "groupId": group_id, "requestorUid": uid_c, "accept": True,
    })
    require(unauthorized_approval.get("error") != 0, unauthorized_approval)
    unauthorized_rejection = b.request(OP_REPLY_JOIN_GROUP, {
        "groupId": group_id, "requestorUid": uid_c, "accept": False,
    })
    require(unauthorized_rejection.get("error") != 0, unauthorized_rejection)
    unauthorized_member_kick = b.request(OP_KICK_GROUP_MEMBER, {
        "groupId": group_id, "requestorUid": uid_a,
    })
    require(unauthorized_member_kick.get("error") != 0,
            unauthorized_member_kick)
    unauthorized_outsider_kick = c.request(OP_KICK_GROUP_MEMBER, {
        "groupId": group_id, "requestorUid": uid_b,
    })
    require(unauthorized_outsider_kick.get("error") != 0,
            unauthorized_outsider_kick)
    member_view = b.request(OP_PULL_GROUP_MEMBERS, {"groupId": group_id})
    require(member_view.get("error") == 0 and not member_view["applyList"],
            member_view)
    require(member_view.get("groupName") == "GatewayGroup", member_view)
    owner_view = a.request(OP_PULL_GROUP_MEMBERS, {"groupId": group_id})
    require(owner_view.get("error") == 0, owner_view)
    require(owner_view.get("groupName") == "GatewayGroup", owner_view)
    require(any(item["from"] == uid_c for item in owner_view["applyList"]),
            owner_view)
    require(a.request(OP_REPLY_JOIN_GROUP, {
        "groupId": group_id, "requestorUid": uid_c, "accept": False,
    }).get("error") == 0, "owner rejection failed")
    require(c.request(OP_JOIN_GROUP, {
        "groupId": group_id, "requestMessage": "outsider reapply",
    }).get("error") == 0, "outsider reapplication failed")
    require(a.request(OP_REPLY_JOIN_GROUP, {
        "groupId": group_id, "requestorUid": uid_c, "accept": True,
    }).get("error") == 0, "owner approval failed")
    for member_client in (a, b, c):
        joined_c = member_client.expect_push(
            PushType.PUSH_TYPE_GROUP_CHANGE,
            lambda item: item.get("requestorUid") == uid_c and
            item.get("groupChangeKind") == 1,
        )
        require(joined_c.get("accept"), joined_c)
        require(joined_c.get("groupName") == "GatewayGroup", joined_c)
    approved_view = a.request(OP_PULL_GROUP_MEMBERS, {"groupId": group_id})
    approved_c = next(item for item in approved_view["memberList"]
                      if item["uid"] == uid_c)
    require(approved_c["joinTime"] != "2026-07-16 00:00:00", approved_c)
    require(c.request(OP_QUIT_GROUP, {"groupId": group_id}).get("error") == 0,
            "approved outsider quit failed")
    for member_client in (a, b, c):
        left_c = member_client.expect_push(
            PushType.PUSH_TYPE_GROUP_CHANGE,
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
    upload = a.request(OP_UPLOAD_FILE, {"file": file_meta, "data": file_data, "fileOffset": 0, "fileComplete": True})
    require(upload.get("error") == 0 and upload.get("fileComplete"), upload)
    group_file = a.request(OP_SEND_GROUP_FILE, {
        "groupId": group_id, "file": file_meta, "clientMessageId": client_message_id + "_group_file",
        "conversationId": group_conversation_id,
    })
    require(group_file.get("error") == 0, group_file)
    file_push = b.expect_push(PushType.PUSH_TYPE_FILE_MESSAGE,
        lambda item: item.get("clientMessageId") == client_message_id + "_group_file")
    b.ack(file_push["messageId"], conversation_id=group_conversation_id,
          conversation_seq=file_push["conversationSeq"])
    downloaded = b.request(OP_DOWNLOAD_FILE, {"file": file_meta, "fileOffset": 0, "limit": 1024})
    require(downloaded.get("error") == 0 and downloaded.get("data") == file_data.decode(), downloaded)

    def upload_owned_file(client, label):
        data = (label + "-owned-group-file").encode()
        meta = {
            "fileId": str(uuid.uuid4()), "fileName": label + ".txt",
            "fileType": "text/plain", "fileSize": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        result = client.request(OP_UPLOAD_FILE, {
            "file": meta, "data": data, "fileOffset": 0, "fileComplete": True,
        })
        require(result.get("error") == 0 and result.get("fileComplete"), result)
        return meta

    file_meta_b = upload_owned_file(b, "member-b")
    file_meta_c = upload_owned_file(c, "outsider-c")

    def require_revoked(client, owned_file, label):
        denied = {
            "text": client.request(OP_SEND_GROUP_TEXT, {
                "groupId": group_id, "data": label + "-text",
                "clientMessageId": client_message_id + "_" + label + "_text",
            }),
            "file": client.request(OP_SEND_GROUP_FILE, {
                "groupId": group_id, "file": owned_file,
                "clientMessageId": client_message_id + "_" + label + "_file",
            }),
            "sync": client.request(OP_PULL_CONVERSATION_MESSAGES, {
                "conversationId": group_conversation_id, "afterSeq": 0,
                "limit": 20,
            }),
            "members": client.request(OP_PULL_GROUP_MEMBERS, {
                "groupId": group_id,
            }),
            "download": client.request(OP_DOWNLOAD_FILE, {
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
    require(b.request(OP_QUIT_GROUP, {"groupId": group_id}).get("error") == 0, "member quit failed")
    for member_client in (a, b):
        member_client.expect_push(
            PushType.PUSH_TYPE_GROUP_CHANGE,
            lambda item: item.get("requestorUid") == uid_b and
            item.get("groupChangeKind") == 2,
        )
    require_revoked(b, file_meta_b, "after_quit")
    require(b.request(OP_JOIN_GROUP, {"groupId": group_id, "requestMessage": "rejoin"}).get("error") == 0, "rejoin request failed")
    require(a.request(OP_REPLY_JOIN_GROUP, {"groupId": group_id, "requestorUid": uid_b, "accept": True}).get("error") == 0, "rejoin approval failed")
    rejoined_view = a.request(OP_PULL_GROUP_MEMBERS, {"groupId": group_id})
    rejoined_b = next(item for item in rejoined_view["memberList"]
                      if item["uid"] == uid_b)
    require(rejoined_b["joinTime"] != "2026-07-16 00:00:00", rejoined_b)
    require(a.request(OP_KICK_GROUP_MEMBER, {"groupId": group_id, "requestorUid": uid_b}).get("error") == 0, "kick failed")
    for member_client in (a, b):
        member_client.expect_push(
            PushType.PUSH_TYPE_GROUP_CHANGE,
            lambda item: item.get("requestorUid") == uid_b and
            item.get("groupChangeKind") == 3,
        )
    require_revoked(b, file_meta_b, "after_kick")

    # Messages created while B is absent remain outside B's next membership
    # window; only messages sent after reapproval are visible in sync.
    absent_message = a.request(OP_SEND_GROUP_TEXT, {
        "groupId": group_id, "data": "while-b-absent",
        "clientMessageId": client_message_id + "_while_b_absent",
    })
    require(absent_message.get("error") == 0, absent_message)
    require(b.request(OP_JOIN_GROUP, {
        "groupId": group_id, "requestMessage": "second rejoin",
    }).get("error") == 0, "second rejoin request failed")
    require(a.request(OP_REPLY_JOIN_GROUP, {
        "groupId": group_id, "requestorUid": uid_b, "accept": True,
    }).get("error") == 0, "second rejoin approval failed")
    visible_message = a.request(OP_SEND_GROUP_TEXT, {
        "groupId": group_id, "data": "after-b-rejoined",
        "clientMessageId": client_message_id + "_after_b_rejoined",
    })
    require(visible_message.get("error") == 0, visible_message)
    rejoined_sync = b.request(OP_PULL_CONVERSATION_MESSAGES, {
        "conversationId": group_conversation_id, "afterSeq": 0, "limit": 20,
    })
    require(rejoined_sync.get("error") == 0, rejoined_sync)
    rejoined_ids = {item["messageId"] for item in rejoined_sync["messageList"]}
    require(absent_message["messageId"] not in rejoined_ids, rejoined_sync)
    require(visible_message["messageId"] in rejoined_ids, rejoined_sync)

    # Owner exit dissolves the group and revokes both the former owner and all
    # remaining members.
    require(a.request(OP_QUIT_GROUP, {"groupId": group_id}).get("error") == 0, "owner dissolve failed")
    a.expect_push(PushType.PUSH_TYPE_GROUP_CHANGE,
                   lambda item: item.get("requestorUid") == uid_a and
                   item.get("groupChangeKind") == 4)
    b.expect_push(PushType.PUSH_TYPE_GROUP_CHANGE,
                   lambda item: item.get("requestorUid") == uid_a and
                   item.get("groupChangeKind") == 4)
    require_revoked(a, file_meta, "owner_after_dissolve")
    require_revoked(b, file_meta_b, "member_after_dissolve")

    # Exercise the real create/join path used by the UI. The second join must
    # carry the group name to an existing member, and a following group message
    # must fan out to both members.
    ui_group = a.request(OP_CREATE_GROUP, {"groupName": "UI-regression-group"})
    require(ui_group.get("error") == 0, ui_group)
    ui_group_id = ui_group["groupId"]
    require(b.request(OP_JOIN_GROUP, {
        "groupId": ui_group_id, "requestMessage": "first UI member",
    }).get("error") == 0, "first UI join request failed")
    require(a.request(OP_REPLY_JOIN_GROUP, {
        "groupId": ui_group_id, "requestorUid": uid_b, "accept": True,
    }).get("error") == 0, "first UI join approval failed")
    for member_client in (a, b):
        joined = member_client.expect_push(
            PushType.PUSH_TYPE_GROUP_CHANGE,
            lambda item: item.get("groupId") == ui_group_id and
            item.get("requestorUid") == uid_b and
            item.get("groupChangeKind") == 1,
        )
        require(joined.get("groupName") == "UI-regression-group", joined)
    require(c.request(OP_JOIN_GROUP, {
        "groupId": ui_group_id, "requestMessage": "second UI member",
    }).get("error") == 0, "second UI join request failed")
    require(a.request(OP_REPLY_JOIN_GROUP, {
        "groupId": ui_group_id, "requestorUid": uid_c, "accept": True,
    }).get("error") == 0, "second UI join approval failed")
    for member_client in (a, b, c):
        joined = member_client.expect_push(
            PushType.PUSH_TYPE_GROUP_CHANGE,
            lambda item: item.get("groupId") == ui_group_id and
            item.get("requestorUid") == uid_c and
            item.get("groupChangeKind") == 1,
        )
        require(joined.get("groupName") == "UI-regression-group", joined)
    ui_group_view = c.request(OP_PULL_GROUP_MEMBERS, {"groupId": ui_group_id})
    require(ui_group_view.get("groupName") == "UI-regression-group", ui_group_view)
    ui_message_id = client_message_id + "_ui_group"
    ui_message = a.request(OP_SEND_GROUP_TEXT, {
        "groupId": ui_group_id, "data": "UI group message",
        "clientMessageId": ui_message_id,
    })
    require(ui_message.get("error") == 0, ui_message)
    for member_client in (b, c):
        pushed = member_client.expect_push(
            PushType.PUSH_TYPE_TEXT_MESSAGE,
            lambda item: item.get("clientMessageId") == ui_message_id,
        )
        require(pushed.get("messageId") == ui_message.get("messageId"), pushed)
    require(a.request(OP_QUIT_GROUP, {"groupId": ui_group_id}).get("error") == 0,
            "UI regression group cleanup failed")

    with open(os.environ["RESULT_FILE"], "w", encoding="utf-8") as output:
        json.dump(accepted, output)
finally:
    a.quit()
    b.quit()
    c.quit()
    a_secondary.close()
    b_secondary.close()
