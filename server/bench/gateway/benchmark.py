#!/usr/bin/env python3
"""Measure Gateway application RTT, direct/forwarded TPS and TCP byte throughput."""
import argparse
import asyncio
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
import importlib
import json
import math
import multiprocessing
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

SERVER = Path(__file__).resolve().parents[2]
MAX_FRAME = 10 * 1024 * 1024
protocol = None
start_barrier = None


def load_protocol(directory):
    global protocol
    sys.path.insert(0, directory)
    protocol = importlib.import_module("tcp_message_pb2")


def initialize_worker(directory, barrier):
    global start_barrier
    load_protocol(directory)
    start_barrier = barrier


def frame(operation, request_id, field, values=None):
    envelope = protocol.ClientEnvelope()
    request = envelope.request
    request.operation = operation
    request.request_id = request_id
    payload = getattr(request.packet, field)
    payload.SetInParent()
    for key, value in (values or {}).items():
        setattr(payload, key, value)
    body = envelope.SerializeToString()
    return struct.pack("!I", len(body)) + body


async def receive(reader):
    header = await reader.readexactly(4)
    size = struct.unpack("!I", header)[0]
    if not 0 < size <= MAX_FRAME:
        raise ValueError(f"invalid LV frame length: {size}")
    envelope = protocol.ClientEnvelope()
    envelope.ParseFromString(await reader.readexactly(size))
    return envelope, size + 4


def validate_response(envelope, request_id, operation, field):
    if not envelope.HasField("response"):
        raise ValueError("expected response envelope")
    response = envelope.response
    if response.request_id != request_id or response.operation != operation:
        raise ValueError("response correlation/operation mismatch")
    error = response.packet.status.error
    if error == 0 and not response.packet.HasField(field):
        raise ValueError("incorrect successful response payload")
    return error


class Statistics:
    def __init__(self):
        self.sent = self.succeeded = self.tx_bytes = self.rx_bytes = self.pushes = 0
        self.errors = Counter()
        self.latencies = Counter()
        self.latency_sum = 0.0
        self.latency_min = math.inf
        self.latency_max = 0.0

    def complete(self, latency, error):
        if error:
            self.errors[f"server:{error}"] += 1
            return
        self.succeeded += 1
        # 对数直方图限制内存占用；分位数取桶上界，相对误差不超过约 1%。
        bucket = math.ceil(math.log(max(latency * 1e6, 1)) / math.log(1.01))
        self.latencies[bucket] += 1
        self.latency_sum += latency
        self.latency_min = min(self.latency_min, latency)
        self.latency_max = max(self.latency_max, latency)

    def export(self):
        return {**vars(self), "latency_min": self.latency_min if self.succeeded else None}


class Connection:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer

    @classmethod
    async def open(cls, config):
        reader, writer = await asyncio.open_connection(config["host"], config["port"])
        writer.get_extra_info("socket").setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection = cls(reader, writer)
        try:
            writer.write(frame(protocol.OP_LOGIN, "login", "login_request", {
                "uid": config["uid"], "auth_token": os.environ[config["token_env"]],
                "device_id": str(uuid.uuid4()), "init_profile": False}))
            await writer.drain()
            response, _ = await receive(reader)
            error = validate_response(response, "login", protocol.OP_LOGIN, "login_response")
            if error:
                raise ValueError(f"login failed: {error}")
            if response.response.packet.login_response.uid != config["uid"]:
                raise ValueError("login response UID mismatch")
            return connection
        except BaseException:
            writer.close()
            await writer.wait_closed()
            raise

    async def run(self, config, phase, duration, stats):
        operation = protocol.OP_PULL_FRIEND_LIST if phase == "forward_tps" else protocol.OP_PING
        field = "pull_friend_list" if phase == "forward_tps" else "ping"
        pipeline = 1 if phase == "rtt" else config["pipeline"]
        padding = "x" * config["payload_bytes"] if phase == "io_throughput" else ""
        pending = {}
        changed = asyncio.Event()
        slots = asyncio.Semaphore(pipeline)
        finished = False
        stop = time.perf_counter() + duration

        async def send():
            nonlocal finished
            sequence = 0
            try:
                while time.perf_counter() < stop:
                    await slots.acquire()
                    if time.perf_counter() >= stop:
                        slots.release()
                        break
                    request_id = f"{sequence}:{padding}"
                    sequence += 1
                    data = frame(operation, request_id, field + "_request")
                    pending[request_id] = time.perf_counter()
                    stats.sent += 1
                    self.writer.write(data)
                    # 统计交给流的 LV + protobuf 字节，不包含 TCP/IP/TLS 头。
                    stats.tx_bytes += len(data)
                    changed.set()
                    await asyncio.wait_for(self.writer.drain(), config["timeout"])
            finally:
                finished = True
                changed.set()

        async def read():
            while True:
                if not pending:
                    changed.clear()
                    if finished:
                        return
                    await changed.wait()
                    continue
                envelope, size = await asyncio.wait_for(receive(self.reader), config["timeout"])
                stats.rx_bytes += size
                if envelope.HasField("push"):
                    stats.pushes += 1
                    continue
                request_id = envelope.response.request_id
                if request_id not in pending:
                    raise ValueError("unexpected or duplicate response request_id")
                error = validate_response(envelope, request_id, operation, field + "_response")
                started = pending.pop(request_id)
                stats.complete(time.perf_counter() - started, error)
                slots.release()

        # 任一收发任务失败都取消另一个，避免 socket 断开后遗留等待槽位的发送者。
        try:
            async with asyncio.TaskGroup() as tasks:
                tasks.create_task(send())
                tasks.create_task(read())
        except ExceptionGroup as errors:
            for error in errors.exceptions:
                stats.errors[f"client:{type(error).__name__}"] += 1
            raise

    async def close(self):
        self.writer.close()
        try:
            await self.writer.wait_closed()
        except OSError:
            pass


async def run_worker(config, phase, count, barrier):
    connections = []
    stats = Statistics()
    try:
        # 登录和预热均在计时外；随机 device_id 避免不同进程互相替换连接。
        for _ in range(count):
            connections.append(await asyncio.wait_for(Connection.open(config), config["timeout"]))
        if config["warmup"]:
            await asyncio.gather(*(item.run(config, phase, config["warmup"], Statistics()) for item in connections))
        barrier.wait(timeout=config["timeout"] + 30)
        started, cpu_started = time.perf_counter(), time.process_time()
        results = await asyncio.gather(*(item.run(config, phase, config["seconds"], stats)
                                         for item in connections), return_exceptions=True)
        ended, cpu = time.perf_counter(), time.process_time() - cpu_started
        return {"started": started, "ended": ended, "cpu_seconds": cpu,
                "aborted_connections": sum(isinstance(value, BaseException) for value in results),
                "statistics": stats.export()}
    finally:
        await asyncio.gather(*(item.close() for item in connections))


def worker(config, phase, count):
    return asyncio.run(run_worker(config, phase, count, start_barrier))


def summarize(rows):
    elapsed = max(row["ended"] for row in rows) - min(row["started"] for row in rows)
    values = [row["statistics"] for row in rows]
    total = lambda key: sum(value[key] for value in values)
    succeeded, sent = total("succeeded"), total("sent")
    histogram, errors = Counter(), Counter()
    for value in values:
        histogram.update(value["latencies"])
        errors.update(value["errors"])

    def percentile(quantile):
        cumulative = 0
        for bucket, count in sorted(histogram.items()):
            cumulative += count
            if cumulative >= math.ceil(succeeded * quantile):
                return 1.01 ** bucket / 1000
        return None

    return {"elapsed_seconds": elapsed, "sent": sent, "succeeded": succeeded,
            "failed_or_unanswered": sent - succeeded, "errors": dict(errors),
            "aborted_connections": sum(row["aborted_connections"] for row in rows),
            "successful_tps": succeeded / elapsed,
            "completed_responses_per_second": (succeeded + sum(count for key, count in errors.items()
                                                    if key.startswith("server:"))) / elapsed,
            "tx_bytes": total("tx_bytes"), "rx_bytes": total("rx_bytes"),
            "tx_mbps": total("tx_bytes") * 8 / elapsed / 1e6,
            "rx_mbps": total("rx_bytes") * 8 / elapsed / 1e6,
            "io_bytes_per_second": (total("tx_bytes") + total("rx_bytes")) / elapsed,
            "unsolicited_pushes": total("pushes"),
            "latency_ms": {"mean": total("latency_sum") * 1000 / succeeded if succeeded else None,
                           "min": min(value["latency_min"] for value in values if value["succeeded"]) * 1000 if succeeded else None,
                           "p50": percentile(.50), "p95": percentile(.95), "p99": percentile(.99),
                           "max": max(value["latency_max"] for value in values) * 1000 if succeeded else None},
            "generator_cpu_seconds": sum(row["cpu_seconds"] for row in rows),
            "max_generator_cpu_utilization": max(row["cpu_seconds"] / (row["ended"] - row["started"])
                                                   for row in rows)}


def measure(config, phase, proto_directory):
    connections = 1 if phase == "rtt" else config["connections"]
    workers = min(config["workers"], connections)
    context = multiprocessing.get_context("spawn")
    # 使用进程规避 Python GIL；各进程登录和预热后在屏障处同时开始计时。
    barrier = context.Barrier(workers)
    with ProcessPoolExecutor(workers, mp_context=context, initializer=initialize_worker,
                             initargs=(proto_directory, barrier)) as pool:
        futures = [pool.submit(worker, config, phase, connections // workers + (index < connections % workers))
                   for index in range(workers)]
        result = summarize([future.result() for future in futures])
    result["warnings"] = []
    if result["max_generator_cpu_utilization"] >= .85:
        result["warnings"].append("generator process CPU >=85%; increase workers or use separate load hosts before inferring server capacity")
    if result["unsolicited_pushes"]:
        result["warnings"].append("unsolicited pushes received; use a dedicated idle test account")
    return {"connections": connections, "pipeline_per_connection": 1 if phase == "rtt" else config["pipeline"],
            "workers": workers, **result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--uid", type=int, required=True)
    parser.add_argument("--token-env", default="IMM_BENCH_TOKEN", help="environment variable containing chat token")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--warmup", type=float, default=2)
    parser.add_argument("--connections", type=int, default=16)
    parser.add_argument("--pipeline", type=int, default=8)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--payload-bytes", type=int, default=65536, help="echoed request_id padding for byte throughput")
    parser.add_argument("--phases", nargs="+", choices=("rtt", "direct_tps", "forward_tps", "io_throughput"),
                        default=["rtt", "direct_tps", "forward_tps", "io_throughput"])
    parser.add_argument("--iperf-host", help="optional separately started iperf3 server (never the Gateway port)")
    parser.add_argument("--iperf-port", type=int, default=5201)
    parser.add_argument("--output", type=Path, default=Path("build/gateway-bench/result.json"))
    args = parser.parse_args()
    if (not all(math.isfinite(value) for value in (args.seconds, args.warmup, args.timeout)) or
            min(args.seconds, args.timeout, args.connections, args.pipeline, args.workers, args.uid) <= 0 or
            args.warmup < 0 or not 0 <= args.payload_bytes <= 1024 * 1024 or
            not 0 < args.port < 65536 or not 0 < args.iperf_port < 65536):
        parser.error("positive durations/counts/UID and valid ports required; padding must be 0..1 MiB")
    if not os.environ.get(args.token_env):
        parser.error(f"set chat token in {args.token_env}")
    config = vars(args).copy()
    result = {"target": {"host": args.host, "port": args.port, "uid": args.uid},
              "config": {key: config[key] for key in ("seconds", "warmup", "connections", "pipeline", "workers", "timeout", "payload_bytes")},
              "phases": {}, "notes": ["closed-loop load; latency starts before client write, includes queueing",
                  "elapsed includes final response drain; login/warmup excluded",
                  "byte counters are client LV+protobuf bytes; not NIC or disk I/O",
                  "Python generator may limit measured capacity; check generator CPU and scale workers/hosts"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="imm-gateway-bench-") as directory:
            subprocess.run(["protoc", f"--proto_path={SERVER/'common/proto'}", f"--python_out={directory}",
                            str(SERVER / "common/proto/tcp_message.proto")], check=True)
            for phase in args.phases:
                result["phases"][phase] = measure(config, phase, directory)
                print(f"{phase}: {json.dumps(result['phases'][phase])}", flush=True)
        if args.iperf_host:
            result["network_bandwidth"] = {}
            for direction, extra in (("upload", []), ("download", ["--reverse"])):
                completed = subprocess.run(["iperf3", "--client", args.iperf_host, "--port", str(args.iperf_port),
                    "--time", str(max(1, math.ceil(args.seconds))), "--json", *extra],
                    capture_output=True, text=True, timeout=args.seconds + 30, check=True)
                report = json.loads(completed.stdout)
                if report.get("error"):
                    raise ValueError(report["error"])
                result["network_bandwidth"][direction] = {
                    "receiver_mbps": report["end"]["sum_received"]["bits_per_second"] / 1e6,
                    "sender_mbps": report["end"]["sum_sent"]["bits_per_second"] / 1e6,
                    "iperf3": report}
        result["passed"] = all(value["succeeded"] > 0 and value["failed_or_unanswered"] == 0
                               and not value["errors"] and value["aborted_connections"] == 0
                               for value in result["phases"].values())
    except Exception as error:
        # 输出诊断但不输出配置或环境，防止令牌进入测试报告。
        result["passed"] = False
        result["failure"] = f"{type(error).__name__}: {error}"
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    print(f"report: {args.output}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
