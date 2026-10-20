#!/usr/bin/env python3
"""Real two-shard Gateway, two Message nodes and isolated local storage."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import hashlib
import uuid
import yaml
import os
import re
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/smoke/src"))
from imm_tcp_client import ImmClient, Operation, PushType


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/imm-server")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/gateway-integration")
    parser.add_argument("--redis-failover", action="store_true")
    parser.add_argument("--mtls", action="store_true")
    parser.add_argument("--full-smoke", action="store_true")
    parser.add_argument("--single", action="store_true", help="verify one Gateway and one Message node")
    parser.add_argument("--benchmark-seconds", type=float, default=0,
                        help="also qualify the Gateway benchmark against this isolated topology")
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

    def message_port():
        while True:
            base = port()
            if base == 65535 or base + 1 in chosen:
                continue
            try:
                with socket.socket() as listener:
                    listener.bind(("127.0.0.1", base + 1))
                chosen.add(base + 1)
                return base
            except OSError:
                continue

    def spawn(command, label, env=None):
        log = (args.output_dir / f"{label}.log").open("w")
        handles.append(log)
        if env and "IMM_CONFIG" in env:
            env = dict(env)
            env.pop("IMM_EXTRA_CONFIG", None)
            if not args.single:
                env["IMM_EXTRA_CONFIG"] = str(root / "cluster.yaml")
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def stop(process, limit=10):
        if process.poll() is not None:
            return
        process.terminate()
        process.wait(timeout=limit)

    def ready(process, predicate, label, timeout=40):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if process.poll() is not None:
                raise RuntimeError(f"{label} exited {process.returncode}; inspect logs")
            try:
                if predicate():
                    return
            except (OSError, EOFError):
                pass
            time.sleep(0.05)
        raise TimeoutError(f"{label} readiness timed out")

    with tempfile.TemporaryDirectory(prefix="imm-gateway-io-") as directory:
        root = Path(directory)
        try:
            ca, ca_key = root / "ca.pem", root / "ca.key"
            if args.mtls:
                subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                                "-subj", "/CN=Gateway probe CA", "-keyout", str(ca_key), "-out", str(ca)],
                               check=True, capture_output=True)

            def security(identity):
                if not args.mtls:
                    return "{environment: development, mode: insecure}"
                cert, key, csr, extensions = [root / f"{identity}.{suffix}" for suffix in ("pem", "key", "csr", "ext")]
                extensions.write_text(f"subjectAltName=DNS:{identity}\nextendedKeyUsage=serverAuth,clientAuth\n")
                subprocess.run(["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={identity}",
                                "-keyout", str(key), "-out", str(csr)], check=True, capture_output=True)
                subprocess.run(["openssl", "x509", "-req", "-days", "1", "-in", str(csr), "-CA", str(ca),
                                "-CAkey", str(ca_key), "-CAcreateserial", "-extfile", str(extensions), "-out", str(cert)],
                               check=True, capture_output=True)
                return f"{{environment: production, mode: mtls, rootCertificates: {ca}, certificateChain: {cert}, privateKey: {key}}}"

            message_security = security("localhost")
            classic, redis_port, sentinel_port, state_port, file_port = [port() for _ in range(5)]
            message_ports = [message_port(), message_port()]
            gateway_ports = [port(), port()]
            data = root / "mysql"
            data.mkdir()
            initialized = subprocess.run(["mysqld", "--no-defaults", "--initialize-insecure", f"--datadir={data}"],
                                         capture_output=True, text=True, timeout=60)
            (args.output_dir / "mysql-initialize.log").write_text(initialized.stdout + initialized.stderr)
            initialized.check_returncode()
            mysql = spawn(["mysqld", "--no-defaults", f"--datadir={data}", f"--socket={root/'mysql.sock'}",
                           f"--port={classic}", "--bind-address=127.0.0.1", "--mysqlx=OFF",
                           "--innodb-buffer-pool-size=32M", "--performance-schema=OFF"], "mysql")
            admin = ["mysql", "--no-defaults", "--protocol=socket", f"--socket={root/'mysql.sock'}", "-uroot", "-N", "-B"]
            ready(mysql, lambda: subprocess.run(admin + ["-e", "SELECT 1"], capture_output=True).returncode == 0, "MySQL")
            sql = "CREATE DATABASE reactor_gateway; CREATE USER 'probe'@'127.0.0.1' IDENTIFIED BY 'probe-local'; GRANT ALL ON reactor_gateway.* TO 'probe'@'127.0.0.1'; USE reactor_gateway;\n"
            sql += (ROOT / "scripts/sql/init_message_mysql_test.sql").read_text()
            subprocess.run(admin, input=sql, text=True, capture_output=True, check=True)
            redis = spawn(["redis-server", "--bind", "127.0.0.1", "--port", str(redis_port), "--save", "",
                           "--appendonly", "no", "--requirepass", "probe-local", "--dir", str(root)], "redis")
            redis_cli = ["redis-cli", "-h", "127.0.0.1", "-p", str(redis_port), "-a", "probe-local", "--no-auth-warning", "--raw"]
            ready(redis, lambda: subprocess.run(redis_cli + ["PING"], capture_output=True).stdout.strip() == b"PONG", "Redis")
            replica = None
            replica_port = None
            if args.redis_failover:
                replica_port = port()
                replica = spawn(["redis-server", "--bind", "127.0.0.1", "--port", str(replica_port),
                    "--save", "", "--appendonly", "no", "--requirepass", "probe-local", "--masterauth", "probe-local",
                    "--replicaof", "127.0.0.1", str(redis_port), "--dir", str(root)], "redis-replica")
                replica_cli = ["redis-cli", "-p", str(replica_port), "-a", "probe-local", "--no-auth-warning", "--raw"]
                ready(replica, lambda: "master_link_status:up" in subprocess.run(replica_cli + ["INFO", "replication"],
                    capture_output=True, text=True).stdout, "Redis replication")
            sentinel_config = root / "sentinel.conf"
            sentinel_config.write_text(f"port {sentinel_port}\nbind 127.0.0.1\nrequirepass probe-local\nsentinel monitor probe 127.0.0.1 {redis_port} 1\nsentinel auth-pass probe probe-local\ndir {root}\n")
            sentinel = spawn(["redis-server", str(sentinel_config), "--sentinel"], "sentinel")
            sentinel_cli = ["redis-cli", "-p", str(sentinel_port), "-a", "probe-local", "--no-auth-warning", "--raw"]
            ready(sentinel, lambda: str(redis_port).encode() in subprocess.run(sentinel_cli + ["SENTINEL", "get-master-addr-by-name", "probe"], capture_output=True).stdout, "Sentinel")
            for uid in (1001, 1002, 1003):
                subprocess.run(redis_cli + ["SETEX", f"im:chatAuth:{uid}", "600", f"token-{uid}"], capture_output=True, check=True)

            def redis_get(key):
                return subprocess.run(redis_cli + ["GET", key], capture_output=True, text=True, check=True).stdout.strip()

            shared = {
                "mysql": {"host": "127.0.0.1", "port": classic, "user": "probe", "password": "probe-local", "schema": "reactor_gateway"},
                "redis": {"masterName": "probe", "password": "probe-local", "sentinelPassword": "probe-local",
                          "sentinels": [{"host": "127.0.0.1", "port": sentinel_port}]}}
            nodes = {
                "state": {"service": "state", "host": "127.0.0.1", "port": state_port},
                "file": {"service": "file", "host": "127.0.0.1", "port": file_port, "storageRoot": str(root / "files")},
                "message-a": {"service": "message", "host": "localhost", "port": message_ports[0], "shards": 2,
                              "transportSecurity": yaml.safe_load(message_security)},
                "message-b": {"service": "message", "host": "localhost", "port": message_ports[1], "shards": 2,
                              "machineId": 2, "transportSecurity": yaml.safe_load(message_security)},
                "gateway-a": {"service": "gateway", "host": "127.0.0.1", "port": gateway_ports[0],
                              "commandRateLimit": 0, "transportSecurity": yaml.safe_load(security("gateway-a"))},
                "gateway-b": {"service": "gateway", "host": "127.0.0.1", "port": gateway_ports[1],
                              "commandRateLimit": 0, "transportSecurity": yaml.safe_load(security("gateway-b"))}}

            def write_config(path):
                base_nodes = {name: node for name, node in nodes.items() if name not in ("message-b", "gateway-b")}
                path.write_text(yaml.safe_dump({"shared": shared, "nodes": base_nodes}, sort_keys=False))
                if not args.single:
                    extra_nodes = {name: nodes[name] for name in ("message-b", "gateway-b")}
                    (root / "cluster.yaml").write_text(yaml.safe_dump({"nodes": extra_nodes}, sort_keys=False))

            state_config = root / "state.yaml"
            write_config(state_config)
            state = spawn([str(args.build_dir / "state/state")], "state", {**os.environ, "IMM_CONFIG": str(state_config), "IMM_NODE": "state"})
            ready(state, lambda: "listening on" in (args.output_dir / "state.log").read_text(), "State")
            file_config = root / "file.yaml"
            write_config(file_config)
            file_server = spawn([str(args.build_dir / "file/file")], "file",
                                {**os.environ, "IMM_CONFIG": str(file_config), "IMM_NODE": "file"})
            message_processes = []
            for index, message_port in enumerate(message_ports[:1] if args.single else message_ports):
                config = root / f"message-{index}.yaml"
                write_config(config)
                message = spawn([str(args.build_dir / "message/message"), "--memory", "256M",
                                 "--overprovisioned", "--reactor-backend", "epoll"], f"message-{index}",
                                {**os.environ, "IMM_CONFIG": str(config), "IMM_NODE": f"message-{'ab'[index]}"})
                message_processes.append(message)
                ready(message, lambda index=index: "[message/startup]" in (args.output_dir / f"message-{index}.log").read_text(), f"Message {index}")

            def launch(label, index=0, extra=""):
                listen_port = gateway_ports[index] if label in ("gateway-a", "gateway-b") else port()
                config = root / f"{label}.yaml"
                nodes[label] = {"service": "gateway", "host": "127.0.0.1", "port": listen_port,
                                "commandRateLimit": 0, "transportSecurity": yaml.safe_load(security(label))}
                if extra:
                    nodes[label].update(yaml.safe_load(extra))
                write_config(config)
                gateway = spawn([str(args.build_dir / "gateway/gateway"), "--smp", "2", "--memory", "256M",
                                 "--overprovisioned", "--reactor-backend", "epoll"], label,
                                {**os.environ, "IMM_CONFIG": str(config), "IMM_NODE": label})
                ready(gateway, lambda: "[gateway/startup]" in (args.output_dir / f"{label}.log").read_text(), label)
                return gateway, listen_port

            a_server, a_port = launch("gateway-a")
            b_server, b_port = (a_server, a_port) if args.single else launch("gateway-b", 1)

            def client(uid, gateway_port, device=None):
                value = ImmClient(uid, "127.0.0.1", gateway_port, auth_token=redis_get(f"im:chatAuth:{uid}"), device_id=device)
                value.login(init=False)
                return value

            for index, process in enumerate(message_processes):
                log_path = args.output_dir / f"message-{index}.log"
                ready(process, lambda log_path=log_path: log_path.read_text().count("Gateway 流已注册") >= (2 if args.single else 4),
                      "Message owner registrations")
                routes = re.findall(r"gateway=(gateway-[ab]) gateway_shard=(\d+) message_shard=(\d+)", log_path.read_text())
                for gateway_id in (("gateway-a",) if args.single else ("gateway-a", "gateway-b")):
                    selected = [(gateway_shard, owner) for node, gateway_shard, owner in routes if node == gateway_id]
                    assert {shard for shard, _ in selected} == {"0", "1"}
                    assert len({owner for _, owner in selected}) == 1, "Gateway streams crossed Message owners"
                if not args.single:
                    assert len({owner for _, _, owner in routes}) == 2, "probe did not cover cross-owner delivery"
            print("PASS single topology ownership" if args.single else "PASS two Message shards per node, Gateway-node slicing and G×N stream count", flush=True)
            a = client(1001, a_port)
            b = client(1002, b_port)
            b_second = None
            try:
                first_route = json.loads(redis_get(f"im:session:1002:{b.device_id}"))
                for attempt in range(20):
                    candidate = client(1002, b_port)
                    route = json.loads(redis_get(f"im:session:1002:{candidate.device_id}"))
                    if route["shardId"] != first_route["shardId"]:
                        b_second = candidate
                        break
                    candidate.quit(wait_response=True)
                assert b_second is not None, "recipient devices did not reach both shards"
                assert a.request(Operation.OP_PING, {})["error"] == 0
                assert a.request(Operation.OP_PULL_FRIEND_LIST, {"uid": 1001})["error"] == 0
                text = a.request(Operation.OP_SEND_TEXT, {"from": 1001, "to": 1002,
                                  "conversationId": 92001, "clientMessageId": "gateway-probe-text", "data": "reactor-text"})
                assert text["error"] == 0, text
                push = b.expect_push(PushType.PUSH_TYPE_TEXT_MESSAGE, lambda value: value.get("data") == "reactor-text")
                b.ack(push["messageId"], conversation_id=push["conversationId"], conversation_seq=push["conversationSeq"])
                second_push = b_second.expect_push(PushType.PUSH_TYPE_TEXT_MESSAGE, lambda value: value.get("data") == "reactor-text")
                assert second_push["messageId"] == push["messageId"]
                b_second.ack(second_push["messageId"], conversation_id=second_push["conversationId"], conversation_seq=second_push["conversationSeq"])
                lease_a = json.loads(redis_get(f"im:session:1001:{a.device_id}"))
                lease_b = json.loads(redis_get(f"im:session:1002:{b.device_id}"))
                assert "shardId" in lease_a and "shardId" in lease_b
                print("PASS login, storage, shard lease and client delivery" if args.single else "PASS real login, MySQL device/profile, Redis shard lease, two Message nodes and cross-Gateway push", flush=True)
            finally:
                a.quit(wait_response=True)
                b.quit(wait_response=True)
                if b_second is not None:
                    b_second.quit(wait_response=True)
            if args.single:
                stop(a_server, 5)
                stop(message_processes[0], 5)
                assert a_server.returncode == 0 and message_processes[0].returncode == 0
                (args.output_dir / "result.json").write_text(json.dumps({"passed": True, "single": True}))
                print("PASS single-file single-node startup, login, storage, message delivery/ACK and stop", flush=True)
                return
            if args.full_smoke:
                # Use the existing broad business smoke against the isolated real
                # topology, including files, groups, receipts and multiple devices.
                subprocess.run(admin + ["reactor_gateway", "-e", "UPDATE groupInfo SET name='GatewayGroup' WHERE groupId=3001"],
                               capture_output=True, check=True)
                import http.client
                gate_port = port()
                gate_config = root / "gate.yaml"
                nodes["gate"] = {"service": "gate", "host": "127.0.0.1", "port": gate_port}
                write_config(gate_config)
                gate = spawn([str(args.build_dir / "gate/gate"), "--smp", "2", "--memory", "256M",
                              "--overprovisioned", "--reactor-backend", "epoll"], "auth-gate",
                             {**os.environ, "IMM_CONFIG": str(gate_config), "IMM_NODE": "gate"})
                ready(gate, lambda: "[gate/startup]" in (args.output_dir / "auth-gate.log").read_text(), "Auth Gate")
                smoke_env = {**os.environ, "UID_A": "1001", "UID_B": "1002", "UID_C": "1003",
                    "USER_A": "zorjen", "USER_B": "alice", "USER_C": "bob", "CONVERSATION_ID": "92001",
                    "GROUP_ID": "3001", "GROUP_CONVERSATION_ID": "93001", "PAYLOAD": "gateway-full-smoke",
                    "CLIENT_MESSAGE_ID": "gateway-full-message", "GATE_URL": f"http://127.0.0.1:{gate_port}",
                    "GATEWAY_A_HOST": "127.0.0.1", "GATEWAY_A_PORT": str(a_port), "GATEWAY_A_ID": "gateway-a",
                    "GATEWAY_B_HOST": "127.0.0.1", "GATEWAY_B_PORT": str(b_port), "GATEWAY_B_ID": "gateway-b",
                    "IMM_REDIS_HOST": "127.0.0.1", "IMM_REDIS_PORT": str(redis_port), "IMM_REDIS_PASSWORD": "probe-local",
                    "RESULT_FILE": str(args.output_dir / "business-smoke-result.json")}
                smoke = subprocess.run(["python3", str(ROOT / "scripts/smoke/src/smoke_gateway_message.py")],
                                       env=smoke_env, capture_output=True, text=True, timeout=120)
                (args.output_dir / "business-smoke.log").write_text(smoke.stdout + smoke.stderr)
                assert smoke.returncode == 0, "full business smoke failed; inspect business-smoke.log"
                stop(gate, 5)
                assert gate.returncode == 0
                print("PASS full Gateway/Message business smoke", flush=True)

            if args.benchmark_seconds:
                benchmark = subprocess.run(["python3", str(ROOT / "server/bench/gateway/benchmark.py"),
                    "--host", "127.0.0.1", "--port", str(a_port), "--uid", "1001",
                    "--seconds", str(args.benchmark_seconds), "--warmup", "0.2",
                    "--connections", "4", "--pipeline", "2", "--workers", "2",
                    "--output", str(args.output_dir / "benchmark.json")],
                    env={**os.environ, "IMM_BENCH_TOKEN": redis_get("im:chatAuth:1001")},
                    capture_output=True, text=True, timeout=60 + args.benchmark_seconds * 4)
                (args.output_dir / "benchmark.log").write_text(benchmark.stdout + benchmark.stderr)
                assert benchmark.returncode == 0, "Gateway benchmark qualification failed; inspect benchmark.log"
                print("PASS real Gateway RTT, direct/forward TPS and byte-throughput benchmark", flush=True)

            # Cover the notification handlers and the distinct chunk/complete payloads,
            # which are not exercised by the existing chat smoke's seeded friendship.
            requester = client(1003, a_port)
            recipient = client(1002, b_port)
            try:
                assert requester.request(Operation.OP_ADD_FRIEND, {"to": 1002, "requestMessage": "async-friend"})["error"] == 0
                notice = recipient.expect_push(PushType.PUSH_TYPE_FRIEND_REQUEST,
                    lambda value: value.get("requestMessage") == "async-friend")
                recipient.ack(notice["seq"])
                assert recipient.request(Operation.OP_PULL_FRIEND_APPLICATIONS, {})["error"] == 0
                assert recipient.request(Operation.OP_REPLY_FRIEND,
                    {"to": 1003, "accept": True, "replyMessage": "async-accepted"})["error"] == 0
                reply = requester.expect_push(PushType.PUSH_TYPE_FRIEND_REPLY,
                    lambda value: value.get("replyMessage") == "async-accepted")
                assert reply["accept"] is True
                requester.ack(reply["seq"])
                friends = requester.request(Operation.OP_PULL_FRIEND_LIST, {})
                assert friends["error"] == 0 and any(int(value["uid"]) == 1002 for value in friends["friendList"])
                content = b"async-chunk-completion" * 32
                split = len(content) // 2
                metadata = {"fileId": str(uuid.uuid4()), "fileName": "async-chunks.txt", "fileType": "text/plain",
                    "fileSize": len(content), "sha256": hashlib.sha256(content).hexdigest()}
                chunk = requester.request(Operation.OP_UPLOAD_FILE_CHUNK, {"file": metadata, "fileOffset": 0,
                    "data": content[:split], "fileComplete": False})
                assert chunk["error"] == 0, chunk
                completed = requester.request(Operation.OP_COMPLETE_FILE_UPLOAD, {"file": metadata, "fileOffset": split,
                    "data": content[split:], "fileComplete": True})
                assert completed["error"] == 0, completed
                # Download authorization follows a persisted conversation message,
                # including for the uploading sender; completing upload alone is insufficient.
                published = requester.request(Operation.OP_SEND_FILE, {"to": 1002, "file": metadata,
                    "clientMessageId": "explicit-file-complete"})
                assert published["error"] == 0, published
                file_push = recipient.expect_push(PushType.PUSH_TYPE_FILE_MESSAGE,
                    lambda value: value.get("clientMessageId") == "explicit-file-complete")
                recipient.ack(file_push["messageId"], conversation_id=file_push["conversationId"],
                    conversation_seq=file_push["conversationSeq"])
                downloaded = requester.request(Operation.OP_DOWNLOAD_FILE, {"file": metadata, "fileOffset": 0, "limit": len(content)})
                assert downloaded["error"] == 0 and downloaded["data"].encode() == content, downloaded
            finally:
                requester.quit(wait_response=True)
                recipient.quit(wait_response=True)
            print("PASS async friend application/reply pushes and explicit file chunk/complete/download APIs", flush=True)

            if args.redis_failover:
                # Keep the old primary running: its GET operations still succeed after
                # demotion, so clients must follow Sentinel instead of caching that endpoint.
                subprocess.run(sentinel_cli + ["SENTINEL", "FAILOVER", "probe"], check=True, capture_output=True)
                ready(sentinel, lambda: str(replica_port).encode() in subprocess.run(sentinel_cli +
                    ["SENTINEL", "get-master-addr-by-name", "probe"], capture_output=True).stdout,
                    "Sentinel primary switch", timeout=30)
                redis_cli[4] = str(replica_port)
                fresh = client(1001, a_port, "after-primary-switch")
                try:
                    assert fresh.request(Operation.OP_PULL_FRIEND_LIST, {})["error"] == 0
                    accepted = fresh.request(Operation.OP_SEND_TEXT, {"to": 1002, "conversationId": 92001,
                        "clientMessageId": "after-primary-switch", "data": "new-primary"})
                    assert accepted["error"] == 0, accepted
                finally:
                    fresh.quit(wait_response=True)
                print("PASS Sentinel primary switch with old Redis still online and Message connections reused", flush=True)
            # Restart one node without restarting Gateway: both shards of each
            # Gateway must recreate their independent authenticated streams.
            stop(message_processes[0], 10)
            assert message_processes[0].returncode == 0
            restarted = spawn([str(args.build_dir / "message/message"), "--memory", "256M",
                               "--overprovisioned", "--reactor-backend", "epoll"], "message-restarted",
                              {**os.environ, "IMM_CONFIG": str(root / "message-0.yaml"), "IMM_NODE": "message-a"})
            message_processes[0] = restarted
            ready(restarted, lambda: (args.output_dir / "message-restarted.log").read_text().count(
                "Gateway 流已注册") >= 4, "Message stream reconnection")
            reconnected = client(1001, a_port)
            try:
                assert reconnected.request(Operation.OP_PULL_FRIEND_LIST, {"uid": 1001})["error"] == 0
            finally:
                reconnected.quit(wait_response=True)
            print("PASS Message restart and four independent mTLS stream reconnections", flush=True)

            # Replace the same device across shard owners. The previous socket must
            # close even while its old lease cleanup races with the new login.
            current = client(1001, a_port, "same-device")
            old_route = json.loads(redis_get("im:session:1001:same-device"))
            different_shard = False
            try:
                for attempt in range(20):
                    replacement = client(1001, a_port, "same-device")
                    route = json.loads(redis_get("im:session:1001:same-device"))
                    assert route["generation"] > old_route["generation"]
                    different_shard |= route["shardId"] != old_route["shardId"]
                    current.sock.settimeout(2)
                    assert current.sock.recv(1) == b"", "old connection survived rebind"
                    current.close()
                    current, old_route = replacement, route
                    if different_shard:
                        break
                assert different_shard, "test did not reach different shard owners"
                time.sleep(0.1)
                assert json.loads(redis_get("im:session:1001:same-device"))["connectionId"] == old_route["connectionId"]
                assert current.request(Operation.OP_PING, {})["error"] == 0
            finally:
                current.quit(wait_response=True)
            print("PASS cross-shard same-device replacement and conditional lease cleanup", flush=True)

            # A partial/oversized LV frame must not cause an unbounded allocation.
            with socket.create_connection(("127.0.0.1", a_port), timeout=2) as invalid:
                invalid.sendall(b"\xff\xff\xff\xff")
                assert invalid.recv(1) == b""

            limited, limited_port = launch("gateway-pending", extra="  io: {concurrency: 1}\n  transport: {maxPending: 1}\n")
            pending_client = client(1001, limited_port)
            locker = subprocess.Popen(admin + ["--unbuffered", "-e",
                "SELECT CONNECTION_ID(); LOCK TABLES reactor_gateway.friends WRITE; SELECT 'locked'; SELECT SLEEP(30);"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(locker)
            lock_id = int(locker.stdout.readline())
            assert locker.stdout.readline().strip() == "locked"
            try:
                first_id = pending_client.send_packet(Operation.OP_PULL_FRIEND_LIST, {"uid": 1001})
                until = time.monotonic() + 5
                while True:
                    waiting = subprocess.run(admin + ["-e", "SELECT COUNT(*) FROM information_schema.processlist WHERE USER='probe' AND STATE LIKE 'Waiting%'"], capture_output=True, text=True, check=True).stdout.strip()
                    if int(waiting):
                        break
                    assert time.monotonic() < until, "first command did not reach storage"
                    time.sleep(0.01)
                # One I/O consumer has returned its ticket and can reject another
                # command despite the first request still waiting on Message.
                rejection = pending_client.request(Operation.OP_PULL_FRIEND_LIST, {"uid": 1001})
                assert rejection["error"] == 1028, rejection
                assert pending_client.request(Operation.OP_PING, {})["error"] == 0
                pending_client.ack(999)
            finally:
                subprocess.run(admin + ["-e", f"KILL {lock_id}"], capture_output=True, check=True)
                locker.wait(timeout=5)
            kind, operation, correlation, completed = pending_client.recv_packet(timeout=10)
            assert kind == "response" and correlation == first_id and completed["error"] == 0
            assert pending_client.request(Operation.OP_PULL_FRIEND_LIST, {"uid": 1001})["error"] == 0
            pending_client.quit(wait_response=True)
            stop(limited, 5)
            assert limited.returncode == 0
            print("PASS shard-wide pending capacity, released I/O consumer, PING/ACK progress and permit return", flush=True)

            byte_limited, byte_port = launch("gateway-bytes", extra="  transaction: {maxBytes: 1}\n")
            invalid_login = ImmClient(1001, "127.0.0.1", byte_port, auth_token="token-1001")
            try:
                rejected = invalid_login.request(Operation.OP_LOGIN, {"uid": 1001, "authToken": "token-1001", "deviceId": "byte-test"})
                assert rejected["error"] == 1028
            finally:
                invalid_login.close()
            stop(byte_limited, 5)
            assert byte_limited.returncode == 0
            print("PASS immediate business byte-capacity rejection", flush=True)
            held = client(1001, a_port)
            locker = subprocess.Popen(admin + ["--unbuffered", "-e",
                "SELECT CONNECTION_ID(); LOCK TABLES reactor_gateway.friends WRITE; SELECT 'locked'; SELECT SLEEP(30);"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(locker)
            lock_id = int(locker.stdout.readline())
            assert locker.stdout.readline().strip() == "locked"
            try:
                held.send_packet(Operation.OP_PULL_FRIEND_LIST, {"uid": 1001})
                until = time.monotonic() + 5
                while True:
                    waiting = subprocess.run(admin + ["-e", "SELECT COUNT(*) FROM information_schema.processlist WHERE USER='probe' AND STATE LIKE 'Waiting%'"], capture_output=True, text=True, check=True).stdout.strip()
                    if int(waiting):
                        break
                    assert time.monotonic() < until
                    time.sleep(0.01)
                stop(a_server, 5)
            finally:
                held.close()
                subprocess.run(admin + ["-e", f"KILL {lock_id}"], capture_output=True, check=True)
                locker.wait(timeout=5)
            survivor = client(1002, b_port)
            try:
                assert survivor.request(Operation.OP_PULL_FRIEND_LIST, {"uid": 1002})["error"] == 0
                assert all(process.poll() is None for process in message_processes)
            finally:
                survivor.quit(wait_response=True)
            print("PASS shutdown with in-flight Message work and safe late completion", flush=True)
            # Hold the database while stopping Message itself: cancellation must settle
            # both the active SQL operation and another request awaiting the same shard connection.
            held_message = client(1002, b_port)
            locker = subprocess.Popen(admin + ["--unbuffered", "-e",
                "SELECT CONNECTION_ID(); LOCK TABLES reactor_gateway.conversations WRITE; SELECT 'locked'; SELECT SLEEP(30);"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(locker)
            lock_id = int(locker.stdout.readline())
            assert locker.stdout.readline().strip() == "locked"
            try:
                held_ids = [held_message.send_packet(Operation.OP_PULL_CONVERSATION_MESSAGES,
                            {"conversationId": 92001, "afterSeq": 0, "limit": 5}) for _ in range(2)]
                until = time.monotonic() + 5
                while True:
                    waiting = subprocess.run(admin + ["-e", "SELECT COUNT(*) FROM information_schema.processlist WHERE USER='probe' AND STATE LIKE 'Waiting%'"], capture_output=True, text=True, check=True).stdout.strip()
                    if int(waiting):
                        break
                    assert time.monotonic() < until, "Message request did not wait for storage"
                    time.sleep(.01)
                for process in message_processes:
                    stop(process, 5)
                    assert process.returncode == 0, "Message shutdown failed"
                settled = set()
                while settled != set(held_ids):
                    kind, operation, correlation, response = held_message.recv_packet(timeout=5)
                    if kind == "response" and correlation in held_ids:
                        assert response["error"] == 1029, response
                        settled.add(correlation)
                assert held_message.request(Operation.OP_PING, {})["error"] == 0
            finally:
                held_message.close()
                subprocess.run(admin + ["-e", f"KILL {lock_id}"], capture_output=True, check=True)
                locker.wait(timeout=5)
            print("PASS Message shutdown cancels active storage and connection waiters, Gateway requests settle", flush=True)
            stop(b_server, 5)
            assert a_server.returncode == 0 and b_server.returncode == 0
            print("PASS graceful Gateway shutdown", flush=True)
            for process in message_processes:
                stop(process, 10)
                assert process.returncode == 0, "Message shutdown failed"
            print("PASS graceful Message shard shutdown", flush=True)
            (args.output_dir / "result.json").write_text(json.dumps({"passed": True}, indent=2))
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    try:
                        stop(process, 8)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
            for handle in handles:
                handle.close()

if __name__ == "__main__":
    main()
