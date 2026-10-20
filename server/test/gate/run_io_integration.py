#!/usr/bin/env python3
"""Run the real Gate against isolated MySQL, Redis/Sentinel, gRPC and TLS SMTP."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time
import base64


class smtp_handler(socketserver.StreamRequestHandler):
    def setup(self):
        if self.server.implicit:
            self.request = self.server.tls.wrap_socket(self.request, server_side=True)
        super().setup()

    def reply(self, text):
        self.wfile.write(text.encode() + b"\r\n")
        self.wfile.flush()

    def handle(self):
        secure = isinstance(self.request, ssl.SSLSocket)
        try:
            self.reply("220 localhost SMTP")
            while line := self.rfile.readline():
                command = line.decode().strip().split(" ", 1)[0].upper()
                if command == "EHLO":
                    self.reply("250-localhost")
                    self.reply("250-STARTTLS")
                    self.reply("250 AUTH LOGIN")
                elif command == "STARTTLS":
                    self.reply("220 Begin TLS")
                    self.connection = self.server.tls.wrap_socket(self.connection, server_side=True)
                    self.request = self.connection
                    self.rfile = self.connection.makefile("rb")
                    self.wfile = self.connection.makefile("wb")
                    secure = True
                elif command == "AUTH":
                    self.reply("334 VXNlcm5hbWU6")
                    username = base64.b64decode(self.rfile.readline().strip())
                    self.reply("334 UGFzc3dvcmQ6")
                    password = base64.b64decode(self.rfile.readline().strip())
                    valid = username == b"probe@localhost.test" and password == b"probe-local"
                    self.reply("235 Accepted" if valid else "535 Authentication rejected")
                elif command == "MAIL":
                    self.reply("250 Accepted")
                elif command == "RCPT":
                    if self.server.reject and self.server.before_reject:
                        self.server.before_reject()
                    self.reply("550 Rejected" if self.server.reject else "250 Accepted")
                elif command == "DATA":
                    self.reply("354 End with dot")
                    body = bytearray()
                    while chunk := self.rfile.readline():
                        if chunk == b".\r\n":
                            break
                        body.extend(chunk)
                    with self.server.lock:
                        self.server.messages.append((body.decode(), secure))
                    self.server.seen.set()
                    self.server.release.wait(15)
                    self.reply("250 Accepted")
                    if self.server.close_after_data:
                        return
                elif command == "QUIT":
                    self.reply("221 Goodbye")
                    return
        except (OSError, ssl.SSLError):
            return


class smtp_server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/imm-server"))
    parser.add_argument("--output-dir", type=Path, default=Path("build/gate-io-integration"))
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    processes, handles, chosen = [], [], set()

    def port():
        while True:
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                value = listener.getsockname()[1]
            if value not in chosen:
                chosen.add(value)
                return value

    def spawn(command, label, env=None):
        log = (args.output_dir / f"{label}.log").open("w")
        handles.append(log)
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def signal_owned(process, value):
        process.send_signal(value)

    def stop(process, limit=10):
        if process.poll() is None:
            signal_owned(process, signal.SIGTERM)
            try:
                process.wait(timeout=limit)
            except subprocess.TimeoutExpired:
                signal_owned(process, signal.SIGKILL)
                process.wait(timeout=5)
                raise RuntimeError(f"process did not stop: pid={process.pid}")

    def ready(process, action, label):
        for _ in range(200):
            if process.poll() is not None:
                raise RuntimeError(f"{label} exited: inspect {args.output_dir}")
            try:
                if action():
                    return
            except (OSError, subprocess.SubprocessError, http.client.HTTPException):
                pass
            time.sleep(0.05)
        raise RuntimeError(f"{label} readiness timed out")

    with tempfile.TemporaryDirectory(prefix="imm-gate-io-", dir="/tmp") as directory:
        root = Path(directory)
        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        with smtp_server(("127.0.0.1", 0), smtp_handler) as smtp:
            smtp.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            smtp.tls.load_cert_chain(cert, key)
            smtp.implicit = False
            smtp.reject = False
            smtp.before_reject = None
            smtp.close_after_data = False
            smtp.lock, smtp.messages = threading.Lock(), []
            smtp.seen, smtp.release = threading.Event(), threading.Event()
            smtp.release.set()
            thread = threading.Thread(target=smtp.serve_forever)
            thread.start()
            try:
                classic_port, redis_port, sentinel_port, state_port, delayed_port = [port() for _ in range(5)]
                data = root / "mysql"
                data.mkdir()
                initialized = subprocess.run(["mysqld", "--no-defaults", "--initialize-insecure", f"--datadir={data}"],
                                             capture_output=True, text=True, timeout=60)
                (args.output_dir / "mysql-initialize.log").write_text(initialized.stdout + initialized.stderr)
                initialized.check_returncode()
                mysql = spawn(["mysqld", "--no-defaults", f"--datadir={data}", f"--socket={root/'mysql.sock'}",
                               f"--port={classic_port}", "--bind-address=127.0.0.1", "--mysqlx=OFF",
                               "--innodb-buffer-pool-size=32M", "--performance-schema=OFF",
                               f"--ssl-cert={cert}", f"--ssl-key={key}", f"--ssl-ca={cert}"], "mysql")
                admin = ["mysql", "--no-defaults", "--protocol=socket", f"--socket={root/'mysql.sock'}", "-uroot", "-N", "-B"]
                ready(mysql, lambda: subprocess.run(admin + ["-e", "SELECT 1"], capture_output=True).returncode == 0, "MySQL")
                schema = """CREATE DATABASE reactor_gate;
CREATE USER 'probe'@'127.0.0.1' IDENTIFIED WITH caching_sha2_password BY 'probe-local';
GRANT ALL ON reactor_gate.* TO 'probe'@'127.0.0.1';
USE reactor_gate;
CREATE TABLE users(uid BIGINT PRIMARY KEY, username VARCHAR(64) UNIQUE, password VARCHAR(128),
                   email VARCHAR(255) UNIQUE, createTime VARCHAR(32) DEFAULT '');
CREATE TABLE userInfo(uid BIGINT PRIMARY KEY);
INSERT INTO users(uid,username,password,email) VALUES(1001,'alice','password','alice@example.test'),(1002,'bob','password','bob@example.test');
INSERT INTO userInfo VALUES(1001);
"""
                subprocess.run(admin, input=schema, text=True, capture_output=True, check=True)
                redis = spawn(["redis-server", "--bind", "127.0.0.1", "--port", str(redis_port), "--save", "",
                               "--appendonly", "no", "--requirepass", "probe-local", "--dir", str(root)], "redis")
                redis_cli = ["redis-cli", "-h", "127.0.0.1", "-p", str(redis_port), "-a", "probe-local", "--no-auth-warning", "--raw"]
                ready(redis, lambda: subprocess.run(redis_cli + ["PING"], capture_output=True).stdout.strip() == b"PONG", "Redis")
                sentinel_config = root / "sentinel.conf"
                sentinel_config.write_text(f"port {sentinel_port}\nbind 127.0.0.1\nrequirepass probe-local\nsentinel monitor probe 127.0.0.1 {redis_port} 1\nsentinel auth-pass probe probe-local\ndir {root}\n")
                sentinel = spawn(["redis-server", str(sentinel_config), "--sentinel"], "sentinel")
                sentinel_cli = ["redis-cli", "-p", str(sentinel_port), "-a", "probe-local", "--no-auth-warning", "--raw"]
                ready(sentinel, lambda: str(redis_port).encode() in subprocess.run(sentinel_cli + ["SENTINEL", "get-master-addr-by-name", "probe"], capture_output=True).stdout, "Sentinel")
                state = spawn([str(args.build_dir / "test/gate/gate_io_state_fixture"), str(state_port), "0"], "state")
                delayed = spawn([str(args.build_dir / "test/gate/gate_io_state_fixture"), str(delayed_port), "10000"], "state-delayed")
                ready(state, lambda: "ready" in (args.output_dir / "state.log").read_text(), "State")
                ready(delayed, lambda: "ready" in (args.output_dir / "state-delayed.log").read_text(), "delayed State")

                def request_http(http_port, path, value=None):
                    connection = http.client.HTTPConnection("127.0.0.1", http_port, timeout=20)
                    try:
                        connection.request("GET" if value is None else "POST", path,
                                           None if value is None else json.dumps(value), {"Content-Type": "application/json"})
                        response = connection.getresponse()
                        body = response.read().decode()
                        return response.status, body if value is None else json.loads(body)
                    finally:
                        connection.close()

                def launch(label, rpc_port=state_port, max_pending=128, max_bytes=1048576, concurrency=4,
                           max_requests=128, max_request_bytes=16777216):
                    http_port = port()
                    config = root / f"{label}.yaml"
                    config.write_text(f"""shared:
  mysql: {{host: localhost, port: {classic_port}, user: probe, password: probe-local, schema: reactor_gate, caFile: {cert}}}
  redis:
    masterName: probe
    password: probe-local
    sentinelPassword: probe-local
    sentinels: [{{host: 127.0.0.1, port: {sentinel_port}}}]
nodes:
  state: {{service: state, host: 127.0.0.1, port: {rpc_port}}}
  gate:
    service: gate
    host: 127.0.0.1
    port: {http_port}
    transaction: {{maxRequests: {max_requests}, maxBytes: {max_request_bytes}}}
    io: {{maxPending: {max_pending}, maxBytes: {max_bytes}, concurrency: {concurrency}}}
    verification:
      exposeCodeInResponse: true
      codeTtlSeconds: 180
      resendCooldownSeconds: 60
      email: {{enabled: true, smtpUrl: {'smtps' if smtp.implicit else 'smtp'}://localhost:{smtp.server_address[1]}, username: probe@localhost.test, password: probe-local, caFile: {cert}}}
""")
                    env = {key: value for key, value in os.environ.items() if not key.startswith("IMM_VERIFY_")}
                    env["IMM_CONFIG"] = str(config)
                    env["IMM_NODE"] = "gate"
                    gate = spawn([str(args.build_dir / "gate/gate"), "--smp", "2", "--memory", "256M",
                                  "--overprovisioned", "--reactor-backend", "epoll"], label, env)
                    ready(gate, lambda: request_http(http_port, "/test")[0] == 200, label)
                    return gate, http_port

                def redis_get(key):
                    return subprocess.run(redis_cli + ["GET", key], text=True, capture_output=True, check=True).stdout.strip()

                gate, http_port = launch("gate-normal")
                status, issued = request_http(http_port, "/post-verifycode", {"email": "  Alice@Example.test "})
                assert status == 200 and issued["error"] == 0 and issued["issued"]
                assert re.fullmatch(r"\d{6}", issued["verificationCode"])
                assert redis_get("im:verify:{alice@example.test}:code") == issued["verificationCode"]
                with smtp.lock:
                    assert len(smtp.messages) == 1 and smtp.messages[0][1] and issued["verificationCode"] in smtp.messages[0][0]
                _, repeated = request_http(http_port, "/post-verifycode", {"email": "alice@example.test"})
                assert not repeated["issued"] and repeated["verificationCode"] == issued["verificationCode"]
                with smtp.lock:
                    assert len(smtp.messages) == 1
                smtp.reject = True
                _, rejected = request_http(http_port, "/post-verifycode", {"email": "reject@example.test"})
                assert rejected["error"] == 1029
                for suffix in ("code", "cooldown", "attempts"):
                    assert not redis_get("im:verify:{reject@example.test}:" + suffix)
                replacement = []
                def replace_code():
                    current = redis_get("im:verify:{race@example.test}:code")
                    changed = "000000" if current != "000000" else "111111"
                    subprocess.run(redis_cli + ["SET", "im:verify:{race@example.test}:code", changed], capture_output=True, check=True)
                    replacement.append(changed)
                smtp.before_reject = replace_code
                _, rejected = request_http(http_port, "/post-verifycode", {"email": "race@example.test"})
                assert rejected["error"] == 1029 and redis_get("im:verify:{race@example.test}:code") == replacement[0]
                smtp.before_reject = None
                smtp.reject = False
                smtp.close_after_data = True
                _, accepted = request_http(http_port, "/post-verifycode", {"email": "quit@example.test"})
                assert accepted["error"] == 0 and redis_get("im:verify:{quit@example.test}:code") == accepted["verificationCode"]
                smtp.close_after_data = False
                for username, initialized in (("alice", 0), ("bob", 1)):
                    status, result = request_http(http_port, "/post-signIn", {"username": username, "password": "password"})
                    assert status == 200 and result["error"] == 0 and result["init"] == initialized
                    assert result["gatewayId"] == "io-test-gateway" and result["port"] == 8090
                    assert redis_get(f'im:chatAuth:{result["uid"]}') == result["chatToken"]
                for username, password in (("alice", "wrong"), ("' OR 1=1 --", "password")):
                    _, result = request_http(http_port, "/post-signIn", {"username": username, "password": password})
                    assert result["error"] == -1
                _, result = request_http(http_port, "/post-arrhythmia", {"uid": 1001})
                assert result["error"] == 0 and result["host"] == "127.0.0.1"
                # All API payloads use ProtoJSON with explicit presence checks.
                for path, payload in (
                    ("/post-signIn", {"username": "alice"}),
                    ("/post-signIn", {"username": 4, "password": "password"}),
                    ("/post-arrhythmia", {}),
                    ("/post-arrhythmia", {"uid": 0}),
                    ("/post-arrhythmia", {"uid": 1.5}),
                    ("/post-verifycode", {"email": None}),
                    ("/post-verifycode", {"email": "a@example.test", "extra": 1}),
                    ("/post-signUp", {"username": "new"}),
                    ("/post-forget-password", {"username": "alice"}),
                ):
                    status, invalid = request_http(http_port, path, payload)
                    assert status == 200 and invalid["error"] != 0, (path, invalid)

                _, code = request_http(http_port, "/post-verifycode", {"email": "new@example.test"})
                registration = {"username": "new", "password": "new-password", "email": "NEW@example.test",
                                "verifycode": code["verificationCode"]}
                _, invalid = request_http(http_port, "/post-signUp", dict(registration, verifycode="000000" if code["verificationCode"] != "000000" else "111111"))
                assert invalid["error"] == 1004
                _, registered = request_http(http_port, "/post-signUp", registration)
                assert registered["error"] == 0 and registered["email"] == "new@example.test"
                assert isinstance(registered["uid"], str) and int(registered["uid"]) > 2**31
                assert not redis_get("im:verify:{new@example.test}:code")
                _, duplicate = request_http(http_port, "/post-signUp", registration)
                assert duplicate["error"] == 1005
                _, login = request_http(http_port, "/post-signIn", {"username": "new", "password": "new-password"})
                assert login["error"] == 0 and login["uid"] == registered["uid"]
                _, gateway = request_http(http_port, "/post-arrhythmia", {"uid": registered["uid"]})
                assert gateway["error"] == 0
                assert f'call {registered["uid"]}' in (args.output_dir / "state.log").read_text()
                _, reset_code = request_http(http_port, "/post-verifycode", {"email": "new@example.test"})
                reset = dict(registration, password="changed-password", verifycode=reset_code["verificationCode"])
                _, mismatch = request_http(http_port, "/post-forget-password", dict(reset, email="other@example.test"))
                assert mismatch["error"] == 1007
                _, changed = request_http(http_port, "/post-forget-password", reset)
                assert changed["error"] == 0 and changed["username"] == "new"
                _, replay = request_http(http_port, "/post-forget-password", reset)
                assert replay["error"] == 1004
                _, old_password = request_http(http_port, "/post-signIn", {"username": "new", "password": "new-password"})
                assert old_password["error"] == -1
                _, new_password = request_http(http_port, "/post-signIn", {"username": "new", "password": "changed-password"})
                assert new_password["error"] == 0
                print("PASS ProtoJSON validation, 64-bit UID, async signup and password reset", flush=True)
                with ThreadPoolExecutor(max_workers=12) as pool:
                    results = list(pool.map(lambda index: request_http(http_port, "/post-verifycode", {"email": f"parallel-{index}@example.test"}), range(12)))
                    assert all(status == 200 and result["error"] == 0 for status, result in results)
                stop(gate)
                assert gate.returncode == 0
                print("PASS TLS SMTP/Redis verification, conditional cleanup, MySQL/gRPC/Redis login, concurrent requests", flush=True)

                smtp.implicit = True
                gate, http_port = launch("gate-smtps")
                _, result = request_http(http_port, "/post-verifycode", {"email": "implicit@example.test"})
                assert result["error"] == 0
                stop(gate)
                assert gate.returncode == 0
                smtp.implicit = False

                gate, http_port = launch("gate-byte-limit", max_bytes=1)
                status, result = request_http(http_port, "/post-verifycode", {"email": "bytes@example.test"})
                assert status == 503 and result["error"] == 1028
                stop(gate)
                assert gate.returncode == 0
                print("PASS implicit TLS and immediate byte-capacity rejection", flush=True)

                smtp.release.clear()
                smtp.seen.clear()
                gate, http_port = launch("gate-stop-smtp")
                with ThreadPoolExecutor(max_workers=1) as pool:
                    request = pool.submit(request_http, http_port, "/post-verifycode", {"email": "stop@example.test"})
                    assert smtp.seen.wait(5)
                    started = time.monotonic()
                    stop(gate, 5)
                    assert gate.returncode == 0 and time.monotonic() - started < 5
                    try:
                        request.result(timeout=5)
                    except (OSError, http.client.HTTPException):
                        pass
                smtp.release.set()

                gate, http_port = launch("gate-stop-grpc", rpc_port=delayed_port)
                with ThreadPoolExecutor(max_workers=1) as pool:
                    request = pool.submit(request_http, http_port, "/post-signIn", {"username": "alice", "password": "password"})
                    until = time.monotonic() + 5
                    while "call " not in (args.output_dir / "state-delayed.log").read_text():
                        assert time.monotonic() < until, "State RPC did not begin"
                        time.sleep(0.01)
                    assert not request.done()
                    started = time.monotonic()
                    assert request_http(http_port, "/test")[0] == 200 and time.monotonic() - started < 1
                    stop(gate, 5)
                    assert gate.returncode == 0
                    try:
                        request.result(timeout=5)
                    except (OSError, http.client.HTTPException):
                        pass
                print("PASS reactor progress and shutdown during SMTP/gRPC I/O", flush=True)

                # Hold an actual MySQL table lock in a separate admin session.
                # The Gate request must suspend without preventing HTTP progress.
                gate, http_port = launch("gate-stop-mysql")
                locker = subprocess.Popen(admin + ["--unbuffered", "-e",
                    "SELECT CONNECTION_ID(); LOCK TABLES reactor_gate.users WRITE; SELECT 'locked'; SELECT SLEEP(30);"],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                processes.append(locker)
                assert select.select([locker.stdout], [], [], 5)[0]
                connection_id = int(locker.stdout.readline())
                assert locker.stdout.readline().strip() == "locked"
                try:
                    with ThreadPoolExecutor(max_workers=1) as pool:
                        request = pool.submit(request_http, http_port, "/post-signIn", {"username": "alice", "password": "password"})
                        until = time.monotonic() + 5
                        while True:
                            waiting = subprocess.run(admin + ["-e", "SELECT COUNT(*) FROM information_schema.processlist WHERE USER='probe' AND STATE LIKE 'Waiting%'"], text=True, capture_output=True, check=True).stdout.strip()
                            if int(waiting):
                                break
                            assert time.monotonic() < until, "MySQL query did not reach table lock"
                            time.sleep(0.01)
                        assert not request.done()
                        started = time.monotonic()
                        assert request_http(http_port, "/test")[0] == 200 and time.monotonic() - started < 1
                        stop(gate, 5)
                        assert gate.returncode == 0
                        try:
                            request.result(timeout=5)
                        except (OSError, http.client.HTTPException):
                            pass
                finally:
                    subprocess.run(admin + ["-e", f"KILL {connection_id}"], capture_output=True, check=True)
                    locker.wait(timeout=5)

                gate, http_port = launch("gate-stop-redis")
                subprocess.run(redis_cli + ["CLIENT", "PAUSE", "10000", "ALL"], capture_output=True, check=True)
                try:
                    with ThreadPoolExecutor(max_workers=1) as pool:
                        request = pool.submit(request_http, http_port, "/post-verifycode", {"email": "redis-stop@example.test"})
                        time.sleep(0.1)
                        assert not request.done()
                        stop(gate, 5)
                        assert gate.returncode == 0
                        try:
                            request.result(timeout=5)
                        except (OSError, http.client.HTTPException):
                            pass
                finally:
                    subprocess.run(redis_cli + ["CLIENT", "UNPAUSE"], capture_output=True, check=True)
                print("PASS shutdown during MySQL/Redis I/O", flush=True)

                gate, http_port = launch("gate-count-limit", max_pending=1, concurrency=1)
                smtp.release.clear()
                smtp.seen.clear()
                with ThreadPoolExecutor(max_workers=17) as pool:
                    first = pool.submit(request_http, http_port, "/post-verifycode", {"email": "held@example.test"})
                    assert smtp.seen.wait(5)
                    requests = [pool.submit(request_http, http_port, "/post-verifycode", {"email": f"overload-{index}@example.test"}) for index in range(16)]
                    try:
                        until = time.monotonic() + 3
                        while not any(request.done() and request.result()[0] == 503 for request in requests):
                            assert time.monotonic() < until, "queue overload did not reject promptly"
                            time.sleep(0.01)
                    finally:
                        smtp.release.set()
                    results = [request.result(timeout=10) for request in requests]
                    first.result(timeout=10)
                    assert any(status == 503 and result["error"] == 1028 for status, result in results)
                    assert all(status in (200, 503) for status, _ in results)
                stop(gate)
                assert gate.returncode == 0
                print("PASS HTTP count-capacity rejection with in-flight SMTP", flush=True)
                gate, http_port = launch("gate-business-bytes", max_request_bytes=1)
                status, rejected = request_http(http_port, "/post-signIn", {"username": "alice", "password": "password"})
                assert status == 503 and rejected["error"] == 1028
                assert request_http(http_port, "/test")[0] == 200
                stop(gate)
                assert gate.returncode == 0

                gate, http_port = launch("gate-business-count", max_requests=1)
                smtp.release.clear()
                smtp.seen.clear()
                # Capacity is per shard, so a new connection may reach the other
                # shard. Hold concurrent requests until at least one shard rejects.
                with ThreadPoolExecutor(max_workers=9) as pool:
                    first = pool.submit(request_http, http_port, "/post-verifycode", {"email": "business-held@example.test"})
                    assert smtp.seen.wait(5)
                    requests = [pool.submit(request_http, http_port, "/post-verifycode",
                                            {"email": f"business-pending-{index}@example.test"}) for index in range(8)]
                    try:
                        until = time.monotonic() + 3
                        while not any(request.done() and request.result()[0] == 503 for request in requests):
                            assert time.monotonic() < until, "business admission did not reject promptly"
                            time.sleep(0.01)
                        assert request_http(http_port, "/test")[0] == 200
                    finally:
                        smtp.release.set()
                    results = [request.result(timeout=10) for request in requests]
                    assert any(status == 503 and rejected["error"] == 1028 for status, rejected in results)
                    assert all(status in (200, 503) for status, _ in results)
                    assert first.result(timeout=10)[1]["error"] == 0
                # Completion returns the semaphore units, permitting another request.
                assert request_http(http_port, "/post-signIn", {"username": "alice", "password": "password"})[1]["error"] == 0
                stop(gate)
                assert gate.returncode == 0
                print("PASS Seastar business admission: immediate count/byte rejection and permit release", flush=True)
                (args.output_dir / "result.json").write_text(json.dumps({"passed": True, "messages": len(smtp.messages)}, indent=2))
            finally:
                smtp.release.set()
                for process in reversed(processes):
                    if process.poll() is None:
                        signal_owned(process, signal.SIGTERM)
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            signal_owned(process, signal.SIGKILL)
                            process.wait(timeout=5)
                smtp.shutdown()
                thread.join(timeout=5)
                for handle in handles:
                    handle.close()


if __name__ == "__main__":
    main()
