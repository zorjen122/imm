#!/usr/bin/env python3
"""Black-box qualification: framing, correlation, accounting and bounded failure."""
import asyncio
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import benchmark


async def main():
    with tempfile.TemporaryDirectory(prefix="imm-bench-test-") as directory:
        root = Path(directory)
        subprocess.run(["protoc", f"--proto_path={benchmark.SERVER/'common/proto'}", f"--python_out={root}",
                        str(benchmark.SERVER / "common/proto/tcp_message.proto")], check=True)
        benchmark.load_protocol(str(root))
        protocol = benchmark.protocol
        # 验证可选 iperf3 的参数和报告整合；这不是物理链路带宽测量。
        iperf = root / "iperf3"
        iperf.write_text(f"#!{sys.executable}\n" + """import json, sys
assert sys.argv[sys.argv.index('--client') + 1] == 'qualification.local'
assert sys.argv[sys.argv.index('--port') + 1] == '55201'
rate = 200000000 if '--reverse' in sys.argv else 100000000
print(json.dumps({'end': {'sum_received': {'bits_per_second': rate},
                         'sum_sent': {'bits_per_second': rate}}}))
""")
        iperf.chmod(0o700)
        mode = "success"
        tasks = set()
        wire = {"tx": 0, "rx": 0, "operations": 0}

        async def handle(reader, writer):
            task = asyncio.current_task()
            tasks.add(task)
            try:
                while True:
                    request, request_size = await benchmark.receive(reader)
                    request = request.request
                    response = protocol.ClientEnvelope()
                    result = response.response
                    result.request_id = request.request_id
                    result.operation = request.operation
                    result.packet.status.error = 0
                    if request.operation == protocol.OP_LOGIN:
                        assert request.packet.login_request.auth_token == "qualification-token"
                        result.packet.login_response.uid = request.packet.login_request.uid
                    else:
                        wire["tx"] += request_size
                        wire["operations"] += 1
                        if mode == "disconnect":
                            return
                        if mode == "timeout":
                            await asyncio.sleep(2)
                            return
                        if mode == "correlation":
                            result.request_id = "wrong"
                        if mode == "wrong_payload":
                            result.packet.quit_response.SetInParent()
                        elif request.operation == protocol.OP_PING:
                            result.packet.ping_response.SetInParent()
                        else:
                            assert request.operation == protocol.OP_PULL_FRIEND_LIST
                            result.packet.pull_friend_list_response.SetInParent()
                        if mode == "rejected":
                            result.packet.status.error = 1028
                    body = response.SerializeToString()
                    data = struct.pack("!I", len(body)) + body
                    if request.operation != protocol.OP_LOGIN:
                        wire["rx"] += len(data)
                    # 将帧拆成两段，客户端不能把单次 read 当作完整帧。
                    writer.write(data[:3])
                    await writer.drain()
                    writer.write(data[3:])
                    await writer.drain()
            except (EOFError, OSError, asyncio.IncompleteReadError):
                pass
            finally:
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass
                tasks.remove(task)

        server = await asyncio.start_server(handle, "127.0.0.1", 0)
        try:
            for mode in ("success", "rejected", "disconnect", "timeout", "correlation", "wrong_payload"):
                wire = {"tx": 0, "rx": 0, "operations": 0}
                output = root / (mode + ".json")
                command = [sys.executable, str(Path(benchmark.__file__)), "--port", str(server.sockets[0].getsockname()[1]),
                    "--uid", "1001", "--seconds", "0.15", "--warmup", "0", "--connections", "4",
                    "--workers", "2", "--pipeline", "3", "--timeout", "0.5", "--payload-bytes", "8192",
                    "--output", str(output)]
                if mode != "success":
                    command += ["--phases", "direct_tps"]
                else:
                    command += ["--iperf-host", "qualification.local", "--iperf-port", "55201"]
                process = await asyncio.create_subprocess_exec(*command,
                    env={**os.environ, "IMM_BENCH_TOKEN": "qualification-token", "PATH": f"{root}:{os.environ['PATH']}"},
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
                stdout, stderr = await asyncio.wait_for(process.communicate(), 30)
                result = json.loads(output.read_text())
                assert "qualification-token" not in output.read_text()
                if mode == "success":
                    assert process.returncode == 0 and result["passed"], (stdout, stderr, result)
                    phases = result["phases"]
                    assert len(phases) == 4
                    assert result["network_bandwidth"]["upload"]["receiver_mbps"] == 100
                    assert result["network_bandwidth"]["download"]["receiver_mbps"] == 200
                    assert sum(value["tx_bytes"] for value in phases.values()) == wire["tx"]
                    assert sum(value["rx_bytes"] for value in phases.values()) == wire["rx"]
                    assert sum(value["succeeded"] for value in phases.values()) == wire["operations"]
                    for value in phases.values():
                        assert value["succeeded"] > 0 and value["failed_or_unanswered"] == 0
                        assert math.isclose(value["successful_tps"] * value["elapsed_seconds"], value["succeeded"])
                        assert math.isclose(value["tx_mbps"] * value["elapsed_seconds"] * 1e6 / 8, value["tx_bytes"])
                        assert value["latency_ms"]["p99"] >= value["latency_ms"]["p50"] > 0
                    assert phases["io_throughput"]["tx_bytes"] / phases["io_throughput"]["sent"] > 8192
                else:
                    assert process.returncode == 1 and not result["passed"], (mode, stdout, stderr, result)
                    phase = result["phases"]["direct_tps"]
                    assert phase["succeeded"] == 0 and phase["failed_or_unanswered"] == phase["sent"]
                    assert phase["errors"], result
                    if mode == "rejected":
                        assert phase["errors"]["server:1028"] == phase["sent"]
                print(f"PASS {mode}", flush=True)
        finally:
            server.close()
            await server.wait_closed()
            remaining = list(tasks)
            for task in remaining:
                task.cancel()
            await asyncio.gather(*remaining, return_exceptions=True)


if __name__ == "__main__":
    asyncio.run(main())
