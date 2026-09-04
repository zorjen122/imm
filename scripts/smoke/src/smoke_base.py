import hashlib
import os

from wimi_tcp_client import Operation, PushType, WimClient, request_chat_auth, require


gateway_host = os.environ["GATEWAY_HOST"]
gateway_port = int(os.environ["GATEWAY_PORT"])
gate_url = os.environ["GATE_URL"]
text_payload = os.environ["TEXT_PAYLOAD"]
online_payload = os.environ["ONLINE_PAYLOAD"]
upload_path = os.environ["UPLOAD_PATH"]
upload_file_id = os.environ["UPLOAD_FILE_ID"]

with open(upload_path, "rb") as source:
    upload_data = source.read()

auth_a = request_chat_auth("zorjen", "123456", gate_url)
auth_b = request_chat_auth("alice", "123456", gate_url)
a = WimClient(1001, gateway_host, gateway_port, auth_token=auth_a["chatToken"])
b = WimClient(1002, gateway_host, gateway_port, auth_token=auth_b["chatToken"])

try:
    a.login(init=False)
    b.login(init=False)

    accepted = a.request(Operation.OP_SEND_TEXT, {
        "to": 1002,
        "data": text_payload,
        "clientMessageId": f"smoke-text-{upload_file_id}",
    })
    require(accepted.get("error") == 0, accepted)
    pushed = b.expect_push(PushType.PUSH_TYPE_TEXT_MESSAGE,
                           lambda item: item.get("clientMessageId") == f"smoke-text-{upload_file_id}")
    b.ack(pushed["messageId"], conversation_id=pushed["conversationId"],
          conversation_seq=pushed["conversationSeq"])

    upload = a.request(Operation.OP_UPLOAD_FILE, {
        "file": {
            "fileId": upload_file_id,
            "fileName": os.path.basename(upload_path),
            "fileType": "text/plain",
            "fileSize": len(upload_data),
            "sha256": hashlib.sha256(upload_data).hexdigest(),
        },
        "data": upload_data,
        "fileOffset": 0,
        "fileComplete": True,
    })
    require(upload.get("error") == 0 and upload.get("fileComplete"), upload)

    online = a.request(Operation.OP_SEND_TEXT, {
        "to": 1002,
        "data": online_payload,
        "clientMessageId": f"smoke-online-{upload_file_id}",
    })
    require(online.get("error") == 0, online)
    b.expect_push(PushType.PUSH_TYPE_TEXT_MESSAGE,
                  lambda item: item.get("clientMessageId") == f"smoke-online-{upload_file_id}")
finally:
    a.quit(wait_response=True)
    b.quit(wait_response=True)
