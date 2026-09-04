import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid


ROOT_DIR = pathlib.Path(__file__).resolve().parents[3]
PROTO_DIR = ROOT_DIR / "server" / "common" / "proto"
GEN_DIR = pathlib.Path(tempfile.gettempdir()) / "wimi_tcp_message_py"


def _ensure_proto_module():
    GEN_DIR.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            "protoc",
            f"--proto_path={PROTO_DIR}",
            f"--python_out={GEN_DIR}",
            str(PROTO_DIR / "tcp_message.proto"),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    if str(GEN_DIR) not in sys.path:
        sys.path.insert(0, str(GEN_DIR))


_ensure_proto_module()

from tcp_message_pb2 import ClientEnvelope, Operation, Packet, PushType  # noqa: E402

RETRYABLE_ERRORS = {1002, 1018, 1020, 1027, 1028, 1029}


def is_retryable_error(error):
    return error in RETRYABLE_ERRORS


FIELD_MAP = {
    "fileId": "file_id",
    "requestMessage": "request_message",
    "replyMessage": "reply_message",
    "headImageURL": "head_image_url",
    "fileName": "file_name",
    "fileType": "file_type",
    "fileSize": "file_size",
    "fileOffset": "offset",
    "fileComplete": "complete",
    "groupName": "group_name",
    "groupId": "group_id",
    "requestorUid": "requestor_uid",
    "replyorUid": "replyor_uid",
    "sendDateTime": "send_date_time",
    "readDateTime": "read_date_time",
    "joinTime": "join_time",
    "text": "content",
    "authToken": "auth_token",
    "messageId": "message_id",
    "receiptType": "receipt_type",
    "requestId": "request_id",
    "conversationId": "conversation_id",
    "conversationSeq": "conversation_seq",
    "clientMessageId": "client_message_id",
    "afterSeq": "after_seq",
    "deviceId": "device_id",
    "init": "init_profile",
}


REQUEST_PAYLOADS = {
    Operation.OP_PULL_FRIEND_LIST: "pull_friend_list_request",
    Operation.OP_PULL_FRIEND_APPLICATIONS: "pull_friend_apply_list_request",
    Operation.OP_PULL_CONVERSATION_MESSAGES: "pull_conversation_messages_request",
    Operation.OP_PULL_GROUP_MEMBERS: "pull_group_members_request",
    Operation.OP_PING: "ping_request",
    Operation.OP_LOGIN: "login_request",
    Operation.OP_QUIT: "quit_request",
    Operation.OP_UPDATE_USER_INFO: "update_user_info_request",
    Operation.OP_SEARCH_USER: "search_user_request",
    Operation.OP_ADD_FRIEND: "add_friend_request",
    Operation.OP_REPLY_FRIEND: "reply_friend_request",
    Operation.OP_REMOVE_FRIEND: "remove_friend_request",
    Operation.OP_SEND_TEXT: "send_text_request",
    Operation.OP_SEND_FILE: "send_file_request",
    Operation.OP_UPLOAD_FILE: "file_upload_request",
    Operation.OP_ACKNOWLEDGE: "receipt",
    Operation.OP_CREATE_GROUP: "create_group_request",
    Operation.OP_JOIN_GROUP: "join_group_request",
    Operation.OP_REPLY_JOIN_GROUP: "reply_join_group_request",
    Operation.OP_QUIT_GROUP: "quit_group_request",
    Operation.OP_SEND_GROUP_TEXT: "send_group_text_request",
    Operation.OP_UPLOAD_FILE_CHUNK: "file_upload_chunk_request",
    Operation.OP_COMPLETE_FILE_UPLOAD: "file_upload_complete_request",
    Operation.OP_DOWNLOAD_FILE: "file_download_request",
    Operation.OP_KICK_GROUP_MEMBER: "kick_group_member_request",
    Operation.OP_SEND_GROUP_FILE: "send_group_file_request",
}


OUTPUT_FIELD_MAP = {
    "head_image_url": "headImageURL",
    "request_message": "requestMessage",
    "reply_message": "replyMessage",
    "create_time": "createTime",
    "send_date_time": "sendDateTime",
    "read_date_time": "readDateTime",
    "join_time": "joinTime",
    "message_id": "messageId",
    "receipt_type": "receiptType",
    "conversation_id": "conversationId",
    "conversation_seq": "conversationSeq",
    "client_message_id": "clientMessageId",
    "after_seq": "afterSeq",
    "next_seq": "nextSeq",
    "has_more": "hasMore",
    "latest_seq": "latestSeq",
    "conversation_type": "conversationType",
    "device_id": "deviceId",
    "file_id": "fileId",
    "file_name": "fileName",
    "file_type": "type",
    "file_size": "fileSize",
    "offset": "fileOffset",
    "next_offset": "fileNextOffset",
    "complete": "fileComplete",
    "group_id": "groupId",
    "group_name": "groupName",
    "requestor_uid": "requestorUid",
    "replyor_uid": "replyorUid",
    "master_uid": "replyorUid",
    "member_uid": "requestorUid",
    "change": "groupChangeKind",
    "friends": "friendList",
    "applications": "applyList",
    "messages": "messageList",
    "members": "memberList",
}


def _set_scalar(payload, key, value):
    if key == "file":
        file = payload.file
        for file_key, file_value in value.items():
            if file_key == "sha256" and isinstance(file_value, str):
                file_value = bytes.fromhex(file_value)
            setattr(file, FIELD_MAP.get(file_key, file_key), file_value)
        return
    field = FIELD_MAP.get(key, key)
    fields = payload.DESCRIPTOR.fields_by_name
    if field == "data" and "data" not in fields and "content" in fields:
        field = "content"
    if field == "requestor_uid" and "member_uid" in fields:
        field = "member_uid"
    if field not in fields:
        return
    if field in ("data", "content") and isinstance(value, str):
        value = value.encode()
    setattr(payload, field, value)


def encode_envelope(operation, request_id, body):
    packet = Packet()
    payload_name = REQUEST_PAYLOADS.get(operation)
    if payload_name is None:
        raise ValueError(f"unsupported request operation: {operation}")
    payload = getattr(packet, payload_name)
    payload.SetInParent()
    for key, value in body.items():
        if key not in ("requestId", "requestTimeoutMs"):
            _set_scalar(payload, key, value)
    envelope = ClientEnvelope()
    envelope.request.request_id = request_id
    envelope.request.operation = operation
    envelope.request.packet.CopyFrom(packet)
    return envelope.SerializeToString()


def _decode_bytes(value):
    try:
        return value.decode()
    except UnicodeDecodeError:
        return value.decode(errors="replace")


def _decode_message(message):
    result = {}
    for descriptor, value in message.ListFields():
        name = OUTPUT_FIELD_MAP.get(descriptor.name, descriptor.name)
        if descriptor.name == "content" and message.DESCRIPTOR.name == "TextMessagePush":
            name = "data"
        elif descriptor.name == "requestor_uid" and message.DESCRIPTOR.name == "GroupJoinApplication":
            name = "from"
        elif descriptor.name == "request_message" and message.DESCRIPTOR.name == "GroupJoinApplication":
            name = "content"
        if descriptor.label == descriptor.LABEL_REPEATED:
            if descriptor.type == descriptor.TYPE_MESSAGE:
                result[name] = [_decode_message(item) for item in value]
            else:
                result[name] = list(value)
        elif descriptor.type == descriptor.TYPE_MESSAGE:
            result[name] = _decode_message(value)
        elif descriptor.type == descriptor.TYPE_BYTES:
            result[name] = value.hex() if descriptor.name == "sha256" else _decode_bytes(value)
        else:
            result[name] = value
    return result


def decode_packet(data):
    packet = Packet()
    packet.ParseFromString(data)
    result = {}
    if packet.HasField("status"):
        result.update(_decode_message(packet.status))
    payload_name = packet.WhichOneof("payload")
    if payload_name:
        result.update(_decode_message(getattr(packet, payload_name)))
    for list_name in ("friendList", "applyList", "messageList", "memberList"):
        result.setdefault(list_name, [])
    return result


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def request_chat_auth(username, password, gate_url="http://127.0.0.1:18080"):
    body = json.dumps({"username": username, "password": password}).encode()
    request = urllib.request.Request(
        f"{gate_url.rstrip('/')}/post-signIn",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        payload = json.loads(response.read().decode())
    require(payload.get("error") == 0, f"gate sign-in failed: {payload}")
    require(payload.get("chatToken"), f"gate response has no chat token: {payload}")
    return payload


class WimClient:
    def __init__(self, uid, host, port, timeout=5, auth_token=None, device_id=None):
        self.uid = uid
        self.device_id = device_id or str(uuid.uuid4())
        self.auth_token = auth_token
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.pending = []
        self.next_request_id = 1

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_packet(self, operation, body):
        body = dict(body)
        request_id = body.setdefault("requestId", self._allocate_request_id())
        data = encode_envelope(operation, request_id, body)
        self.sock.sendall(struct.pack("!I", len(data)) + data)
        return request_id

    def _allocate_request_id(self):
        request_id = f"smoke-{self.uid}-{self.next_request_id}"
        self.next_request_id += 1
        return request_id

    def recv_exact(self, size):
        chunks = []
        remaining = size
        while remaining:
            chunk = self.sock.recv(remaining)
            if not chunk:
                raise EOFError("chat connection closed")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def recv_packet(self, timeout=5):
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(timeout)
        try:
            header = self.recv_exact(4)
            (size,) = struct.unpack("!I", header)
            body = self.recv_exact(size)
            envelope = ClientEnvelope()
            envelope.ParseFromString(body)
            if envelope.HasField("response"):
                response = envelope.response
                return "response", response.operation, response.request_id, decode_packet(response.packet.SerializeToString())
            if envelope.HasField("push"):
                push = envelope.push
                return "push", push.type, "", decode_packet(push.packet.SerializeToString())
            raise ValueError("Gateway sent a ClientEnvelope without response or push")
        finally:
            self.sock.settimeout(old_timeout)

    def ack(self, seq, receipt_type=1, conversation_id=None,
            conversation_seq=None):
        body = {"seq": seq, "receiptType": receipt_type}
        if conversation_id is not None:
            body["conversationId"] = conversation_id
        if conversation_seq is not None:
            body["conversationSeq"] = conversation_seq
        self.send_packet(Operation.OP_ACKNOWLEDGE, body)

    def request(self, operation, body, timeout=5):
        body = dict(body)
        request_id = body.setdefault("requestId", self._allocate_request_id())
        self.send_packet(operation, body)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                kind, received_type, received_request_id, payload = self.recv_packet(remaining)
            except socket.timeout:
                continue
            if kind == "response" and received_request_id == request_id:
                if received_type != operation:
                    raise ValueError(f"response operation mismatch: expected {operation}, got {received_type}")
                return payload
            self.pending.append((kind, received_type, received_request_id, payload))

        raise TimeoutError(f"uid {self.uid} did not receive response for operation {operation}")

    def request_with_retry(self, operation, body, attempts=3, timeout=5, base_delay=0.2):
        """Retry one logical command without changing its idempotency fields."""
        body = dict(body)
        body.setdefault("requestId", self._allocate_request_id())
        last_error = None
        for attempt in range(attempts):
            try:
                response = self.request(operation, body, timeout)
                if not is_retryable_error(response.get("error", 0)):
                    return response
                last_error = RuntimeError(f"retryable response: {response}")
            except TimeoutError as error:
                last_error = error
            if attempt + 1 < attempts:
                time.sleep(base_delay * (2 ** attempt))
        raise last_error

    def expect_push(self, push_type, predicate=lambda payload: True, timeout=5):
        for idx, (kind, received_type, request_id, payload) in enumerate(self.pending):
            if kind == "push" and received_type == push_type and predicate(payload):
                self.pending.pop(idx)
                return payload

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                kind, received_type, request_id, payload = self.recv_packet(remaining)
            except socket.timeout:
                continue
            if kind == "push" and received_type == push_type and predicate(payload):
                return payload
            self.pending.append((kind, received_type, request_id, payload))

        raise TimeoutError(f"uid {self.uid} did not receive push type {push_type}")

    def login(self, init=None):
        body = {
            "uid": self.uid,
            "authToken": self.auth_token or "",
            "deviceId": self.device_id,
        }
        if init is not None:
            body["init"] = init
        rsp = self.request(Operation.OP_LOGIN, body)
        require(
            "error" in rsp,
            "login response has no ResponseStatus.error; rebuild with "
            "./scripts/build-server.sh and restart services with "
            "./scripts/run-local-services.sh --stop-existing "
            f"(possible stale TCP protocol): {rsp}",
        )
        require(rsp.get("error") == 0, f"login failed for {self.uid}: {rsp}")
        return rsp

    def quit(self, wait_response=False):
        try:
            if wait_response:
                self.request(Operation.OP_QUIT, {"uid": self.uid}, timeout=2)
            else:
                self.send_packet(Operation.OP_QUIT, {"uid": self.uid})
        except Exception:
            pass
        self.close()
