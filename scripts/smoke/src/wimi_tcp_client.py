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
PROTO_DIR = ROOT_DIR / "server" / "public" / "proto"
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

from tcp_message_pb2 import Packet  # noqa: E402


ID_LOGIN_INIT_REQ = 1013
ID_USER_QUIT_REQ = 1015
ID_ACK = 1033

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
    1001: "pull_friend_list_request",
    1003: "pull_friend_apply_list_request",
    1005: "pull_conversation_messages_request",
    1009: "pull_group_members_request",
    1011: "ping_request",
    1013: "login_request",
    1015: "quit_request",
    1017: "update_user_info_request",
    1019: "search_user_request",
    1021: "add_friend_request",
    1023: "reply_friend_request",
    1025: "remove_friend_request",
    1027: "send_text_request",
    1029: "send_file_request",
    1031: "file_upload_request",
    1033: "receipt",
    1035: "create_group_request",
    1037: "join_group_request",
    1039: "reply_join_group_request",
    1041: "quit_group_request",
    1043: "send_group_text_request",
    1047: "file_upload_chunk_request",
    1049: "file_upload_complete_request",
    1051: "file_download_request",
    1053: "kick_group_member_request",
    1055: "send_group_file_request",
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


def encode_packet(service_id, body):
    packet = Packet()
    packet.request.request_id = body.get("requestId", "")
    payload_name = REQUEST_PAYLOADS.get(service_id)
    if payload_name is None:
        raise ValueError(f"unsupported request service id: {service_id}")
    payload = getattr(packet, payload_name)
    payload.SetInParent()
    for key, value in body.items():
        if key not in ("requestId", "requestTimeoutMs"):
            _set_scalar(payload, key, value)
    return packet.SerializeToString()


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
    def __init__(self, uid, host, port, timeout=5, async_ack_ids=None,
                 auto_ack=False, auth_token=None, device_id=None):
        self.uid = uid
        self.device_id = device_id or str(uuid.uuid4())
        self.auth_token = auth_token
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.pending = []
        self.async_ack_ids = set(async_ack_ids or [])
        self.auto_ack = auto_ack
        self.next_request_id = 1

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_packet(self, service_id, body):
        body = dict(body)
        body.setdefault("requestId", self._allocate_request_id())
        data = encode_packet(service_id, body)
        self.sock.sendall(struct.pack("!II", service_id, len(data)) + data)

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
            header = self.recv_exact(8)
            service_id, size = struct.unpack("!II", header)
            body = self.recv_exact(size)
            payload = decode_packet(body) if body else {}
            return service_id, payload
        finally:
            self.sock.settimeout(old_timeout)

    def ack(self, seq, receipt_type=1, conversation_id=None,
            conversation_seq=None):
        body = {"seq": seq, "receiptType": receipt_type}
        if conversation_id is not None:
            body["conversationId"] = conversation_id
        if conversation_seq is not None:
            body["conversationSeq"] = conversation_seq
        self.send_packet(ID_ACK, body)

    def ack_async(self, service_id, payload):
        reliable_id = payload.get("seq", payload.get("messageId"))
        if service_id in self.async_ack_ids and reliable_id is not None:
            self.ack(reliable_id)
            return True
        return False

    def request(self, service_id, body, expected_id=None, timeout=5, auto_ack=None):
        if expected_id is None:
            expected_id = service_id + 1
        if auto_ack is None:
            auto_ack = self.auto_ack

        body = dict(body)
        body.setdefault("requestId", self._allocate_request_id())
        self.send_packet(service_id, body)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                service, payload = self.recv_packet(remaining)
            except socket.timeout:
                continue
            if service == expected_id:
                return payload
            if auto_ack and self.ack_async(service, payload):
                continue
            self.pending.append((service, payload))

        raise TimeoutError(f"uid {self.uid} did not receive service {expected_id}")

    def request_with_retry(self, service_id, body, attempts=3, timeout=5,
                           base_delay=0.2, expected_id=None):
        """Retry one logical command without changing its idempotency fields."""
        body = dict(body)
        body.setdefault("requestId", self._allocate_request_id())
        last_error = None
        for attempt in range(attempts):
            try:
                response = self.request(service_id, body, expected_id, timeout)
                if not is_retryable_error(response.get("error", 0)):
                    return response
                last_error = RuntimeError(f"retryable response: {response}")
            except TimeoutError as error:
                last_error = error
            if attempt + 1 < attempts:
                time.sleep(base_delay * (2 ** attempt))
        raise last_error

    def expect_async(self, service_id, predicate=lambda payload: True, timeout=5):
        for idx, (service, payload) in enumerate(self.pending):
            if service == service_id and predicate(payload):
                self.pending.pop(idx)
                return payload

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                service, payload = self.recv_packet(remaining)
            except socket.timeout:
                continue
            if service == service_id and predicate(payload):
                return payload
            self.pending.append((service, payload))

        raise TimeoutError(f"uid {self.uid} did not receive async service {service_id}")

    def login(self, init=None):
        body = {
            "uid": self.uid,
            "authToken": self.auth_token or "",
            "deviceId": self.device_id,
        }
        if init is not None:
            body["init"] = init
        rsp = self.request(ID_LOGIN_INIT_REQ, body)
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
                self.request(ID_USER_QUIT_REQ, {"uid": self.uid}, timeout=2)
            else:
                self.send_packet(ID_USER_QUIT_REQ, {"uid": self.uid})
        except Exception:
            pass
        self.close()
