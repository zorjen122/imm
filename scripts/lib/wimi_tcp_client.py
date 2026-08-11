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


ROOT_DIR = pathlib.Path(__file__).resolve().parents[2]
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


FIELD_MAP = {
    "fileId": "file_id",
    "requestMessage": "request_message",
    "replyMessage": "reply_message",
    "headImageURL": "head_image_url",
    "fileName": "file_name",
    "fileType": "file_type",
    "fileSize": "file_size",
    "fileOffset": "file_offset",
    "fileComplete": "file_complete",
    "groupName": "group_name",
    "groupId": "group_id",
    "requestorUid": "requestor_uid",
    "replyorUid": "replyor_uid",
    "applyDateTime": "apply_date_time",
    "sendDateTime": "send_date_time",
    "readDateTime": "read_date_time",
    "joinTime": "join_time",
    "text": "data",
    "authToken": "auth_token",
    "messageId": "message_id",
    "messageState": "message_state",
    "receiptType": "receipt_type",
    "requestTimeoutMs": "request_timeout_ms",
    "requestId": "request_id",
    "conversationId": "conversation_id",
    "conversationSeq": "conversation_seq",
    "clientMessageId": "client_message_id",
    "afterSeq": "after_seq",
    "deviceId": "device_id",
}


def _set_scalar(packet, key, value):
    if key == "file":
        file = packet.file
        for file_key, file_value in value.items():
            if file_key == "sha256" and isinstance(file_value, str):
                file_value = bytes.fromhex(file_value)
            setattr(file, FIELD_MAP.get(file_key, file_key), file_value)
        return
    if key == "type" and isinstance(value, str):
        packet.file_type = value
        return
    field = FIELD_MAP.get(key, key)
    if field == "data" and isinstance(value, str):
        value = value.encode()
    setattr(packet, field, value)


def encode_packet(body):
    packet = Packet()
    for key, value in body.items():
        _set_scalar(packet, key, value)
    return packet.SerializeToString()


def _decode_bytes(value):
    try:
        return value.decode()
    except UnicodeDecodeError:
        return value.decode(errors="replace")


def decode_packet(data):
    packet = Packet()
    packet.ParseFromString(data)
    result = {}

    scalar_fields = [
        ("uid", "uid"),
        ("seq", "seq"),
        ("from", "from"),
        ("to", "to"),
        ("data", "data"),
        ("error", "error"),
        ("message", "message"),
        ("accept", "accept"),
        ("request_message", "requestMessage"),
        ("reply_message", "replyMessage"),
        ("username", "username"),
        ("name", "name"),
        ("age", "age"),
        ("sex", "sex"),
        ("head_image_url", "headImageURL"),
        ("init", "init"),
        ("file_name", "fileName"),
        ("file_type", "type"),
        ("type", "type"),
        ("limit", "limit"),
        ("group_id", "groupId"),
        ("group_name", "groupName"),
        ("requestor_uid", "requestorUid"),
        ("replyor_uid", "replyorUid"),
        ("content", "content"),
        ("create_time", "createTime"),
        ("apply_date_time", "applyDateTime"),
        ("send_date_time", "sendDateTime"),
        ("read_date_time", "readDateTime"),
        ("join_time", "joinTime"),
        ("auth_token", "authToken"),
        ("message_id", "messageId"),
        ("message_state", "messageState"),
        ("receipt_type", "receiptType"),
        ("retryable", "retryable"),
        ("request_id", "requestId"),
        ("conversation_id", "conversationId"),
        ("conversation_seq", "conversationSeq"),
        ("client_message_id", "clientMessageId"),
        ("after_seq", "afterSeq"),
        ("next_seq", "nextSeq"),
        ("has_more", "hasMore"),
        ("latest_seq", "latestSeq"),
        ("conversation_type", "conversationType"),
        ("device_id", "deviceId"),
        ("file_offset", "fileOffset"),
        ("file_next_offset", "fileNextOffset"),
        ("file_complete", "fileComplete"),
        ("group_change_kind", "groupChangeKind"),
    ]

    for proto_name, json_name in scalar_fields:
        if packet.HasField(proto_name):
            value = getattr(packet, proto_name)
            if proto_name == "data":
                value = _decode_bytes(value)
            result[json_name] = value

    result["friendList"] = [
        {
            "uid": item.uid,
            "name": item.name,
            "age": item.age,
            "sex": item.sex,
            "headImageURL": item.head_image_url,
        }
        for item in packet.friend_list
    ]
    result["applyList"] = [
        {
            "from": getattr(item, "from"),
            "to": item.to,
            "status": item.status,
            "content": item.content,
            "applyDateTime": item.apply_date_time,
            "createTime": item.create_time,
        }
        for item in packet.apply_list
    ]
    result["messageList"] = [
        {
            "messageId": item.message_id,
            "from": getattr(item, "from"),
            "to": item.to,
            "type": item.type,
            "content": item.content,
            "status": item.status,
            "sendDateTime": item.send_date_time,
            "readDateTime": item.read_date_time,
            "conversationId": item.conversation_id,
            "conversationSeq": item.conversation_seq,
            "clientMessageId": item.client_message_id,
        }
        for item in packet.message_list
    ]
    result["memberList"] = [
        {
            "uid": item.uid,
            "name": item.name,
            "joinTime": item.join_time,
        }
        for item in packet.member_list
    ]
    if packet.HasField("file"):
        result["file"] = {
            "fileId": packet.file.file_id,
            "fileName": packet.file.file_name,
            "type": packet.file.file_type,
            "fileSize": packet.file.file_size,
            "sha256": packet.file.sha256.hex(),
            "expired": packet.file.expired,
        }

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
        data = encode_packet(body)
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
        if service_id in self.async_ack_ids and "seq" in payload:
            self.ack(payload["seq"])
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
                if not response.get("retryable", False):
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
