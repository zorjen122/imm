#!/usr/bin/env python3
"""Run smoke/perf against an isolated schema, Redis/Sentinel and service ports."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid
import yaml

ROOT = Path(__file__).resolve().parents[2]


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_port(port, process):
    for _ in range(150):
        if process.poll() is not None:
            raise RuntimeError(f'process exited before listening on {port}')
        try:
            with socket.create_connection(('127.0.0.1', port), .2):
                return
        except OSError:
            time.sleep(.1)
    raise RuntimeError(f'port {port} did not become ready')


def capture(command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def value_after_prefix(path, prefix):
    try:
        for line in path.read_text().splitlines():
            if line.startswith(prefix):
                return line.split(':', 1)[1].strip()
    except OSError:
        pass
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--rate', type=float, default=40)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--seastar-smp', type=int, default=1)
    parser.add_argument('--network-interface', default='lo')
    parser.add_argument('--block-device', default='')
    parser.add_argument('--idle-connections', type=int, default=100)
    parser.add_argument('--scenarios', default='',
                        help='comma-separated subset; defaults to all qualification scenarios')
    parser.add_argument('--network-runtime', choices=('seastar', 'legacy'), default='seastar')
    args = parser.parse_args()
    if (not 1 <= args.workers <= 256 or not 1 <= args.seastar_smp <= 64 or
            not 0 <= args.idle_connections <= 10000 or
            not 0 < args.seconds <= 86400 or not 0 < args.rate <= 1000000):
        parser.error("invalid load bounds")
    args.build = args.build.resolve()
    all_scenarios = ['http-test', 'http-test-keepalive', 'http-login', 'http-login-keepalive',
                     'tcp-ping', 'tcp-pull', 'state-pick', 'state-topology', 'file-roundtrip']
    scenarios = args.scenarios.split(',') if args.scenarios else all_scenarios
    if not scenarios or len(scenarios) != len(set(scenarios)) or any(item not in all_scenarios for item in scenarios):
        parser.error('invalid or duplicate scenario')
    for executable in ["bench/wimiPerf", "gate/gate", "gateway/gateway", "message/message", "file/file", "state/state"]:
        if not (args.build / executable).is_file():
            parser.error(f"missing executable: {executable}")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    schema = 'wimi_perf_' + uuid.uuid4().hex[:12]
    original_ports = [18080, 8090, 8091, 50052, 50055, 50056, 51100, 6380, 26389]
    ports = {}
    for original in original_ports:
        port = free_port()
        while port in ports.values():
            port = free_port()
        ports[original] = port
    processes = []
    process_names = {}
    handles = []
    held_sockets = []
    idle_sockets = []
    created_schema = False

    def start(name, command, env=None, cwd=ROOT):
        handle = (args.output / f'{name}.log').open('w')
        handles.append(handle)
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=handle, stderr=subprocess.STDOUT)
        processes.append(process)
        process_names[process.pid] = name
        return process

    def mysql(sql):
        return subprocess.run(['sudo', '-n', 'mysql', '--batch', '--skip-column-names'],
                              input=sql, text=True, check=True, capture_output=True).stdout

    def rewrite(value):
        if isinstance(value, dict):
            return {key: rewrite(item) for key, item in value.items()}
        if isinstance(value, list):
            return [rewrite(item) for item in value]
        if isinstance(value, int) and value in ports:
            return ports[value]
        return value

    cache = args.build / 'CMakeCache.txt'
    cache_text = cache.read_text() if cache.exists() else ''
    build_type = next((line.split('=', 1)[1] for line in cache_text.splitlines()
                       if line.startswith('CMAKE_BUILD_TYPE:')), None)
    mtu_path = Path(f'/sys/class/net/{args.network_interface}/mtu')
    interface_mtu = int(mtu_path.read_text()) if mtu_path.exists() else None
    manifest = {'schema': schema, 'ports': ports, 'build': str(args.build),
                'commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'measurement': 'local qualification only; shared host, loopback, no capacity claim',
                'rate': args.rate, 'seconds': args.seconds, 'workers': args.workers,
                'seastar_smp': args.seastar_smp, 'network_interface': args.network_interface,
                'block_device': args.block_device or None, 'idle_connections': args.idle_connections,
                'scenarios': scenarios,
                'network_runtime': args.network_runtime,
                'reactor_backend': 'io_uring' if args.network_runtime == 'seastar' else None,
                'host': {'kernel': capture(['uname', '-srvmo']), 'logical_cpus': os.cpu_count(),
                         'cpu_model': value_after_prefix(Path('/proc/cpuinfo'), 'model name'),
                         'memory': value_after_prefix(Path('/proc/meminfo'), 'MemTotal')},
                'toolchain': {'cxx': capture(['/usr/bin/g++', '--version']), 'standard': 'C++23',
                              'cmake_build_type': build_type,
                              'seastar_pkg_config': capture(['pkg-config', '--modversion', 'seastar'])},
                'network': {'interface': args.network_interface, 'mtu': interface_mtu},
                'service_budget': {'gate_memory': '256M', 'gateway_memory': '256M',
                                   'gate_blocking_workers': 4, 'gateway_blocking_workers': 4,
                                   'gateway_message_io_workers': 2},
                'redis_persistence': {'save': False, 'appendonly': False}, 'log_level': 'info'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    (args.output / 'workspace.diff').write_bytes(subprocess.check_output(['git', 'diff'], cwd=ROOT))
    try:
        mysql(f'CREATE DATABASE {schema}; GRANT ALL ON {schema}.* TO zorjen@localhost; '
              f"GRANT ALL ON {schema}.* TO 'zorjen'@'%';")
        created_schema = True
        mysql(f'USE {schema};\n' + (ROOT / 'scripts/sql/init_message_mysql_test.sql').read_text())
        for worker in range(args.workers):
            mysql(f"USE {schema}; INSERT INTO users(uid,username,password,email) VALUES "
                  f"({100000+worker},'perf{worker}','perf-test-only','perf{worker}@example.test');"
                  f"INSERT INTO userInfo(uid,name) VALUES ({100000+worker},'Perf');")

        redis = start('redis', ['redis-server', '--bind', '127.0.0.1', '--port', str(ports[6380]),
                                '--save', '', '--appendonly', 'no', '--requirepass', 'root'])
        wait_port(ports[6380], redis)
        sentinel_config = args.output / 'sentinel.conf'
        sentinel_config.write_text(f'bind 127.0.0.1\nport {ports[26389]}\nrequirepass root\n'
                                   f'sentinel monitor wimi-master 127.0.0.1 {ports[6380]} 1\n'
                                   'sentinel auth-pass wimi-master root\n')
        sentinel = start('sentinel', ['redis-server', str(sentinel_config), '--sentinel'])
        wait_port(ports[26389], sentinel)

        services = [('state', 'state-multi.yaml', 50052), ('file', 'file.yaml', 51100),
                    ('message', 'message-hunan-im.yaml', 50055),
                    ('message', 'message-beijing-im.yaml', 50056),
                    ('gateway', 'gateway-hunan.yaml', 8090),
                    ('gateway', 'gateway-beijing.yaml', 8091), ('gate', 'gate.yaml', 18080)]
        for service, filename, original_port in services:
            config = rewrite(yaml.safe_load((ROOT / 'conf' / filename).read_text()))
            server = config['server']
            if 'mysql' in server:
                server['mysql']['schema'] = schema
            if 'redis' in server:
                server['redis']['sentinels'] = [{'host': '127.0.0.1', 'port': ports[26389]}]
            if service == 'file':
                server['file']['storageRoot'] = str(args.output / 'files')
            path = args.output / filename
            path.write_text(yaml.safe_dump(config))
            env = os.environ | {'WIMI_CONFIG': str(path)}
            command = [str(args.build / service / service)]
            if service == 'message':
                command.append(str(path))
            if service in {'gate', 'gateway'} and args.network_runtime == 'seastar':
                command.extend(['--smp', str(args.seastar_smp), '--memory', '256M',
                                '--overprovisioned', '--reactor-backend', 'io_uring'])
            process = start(filename, command, env)
            wait_port(ports[original_port], process)
        time.sleep(2)
        env = os.environ | {'WIMI_DB': schema, 'WIMI_REDIS_PORT': str(ports[6380]),
                            'CHAT_A_PORT': str(ports[8090]), 'CHAT_B_PORT': str(ports[8091]),
                            'GATE_URL': f'http://127.0.0.1:{ports[18080]}',
                            'PERF_GATE_PORT': str(ports[18080]), 'PERF_GATEWAY_PORT': str(ports[8090]),
                            'PERF_STATE': f'127.0.0.1:{ports[50052]}',
                            'PERF_FILE': f'127.0.0.1:{ports[51100]}'}
        with (args.output / 'lv-tcp.log').open('w') as log:
            subprocess.run([sys.executable, str(ROOT / 'scripts/perf/check-lv-tcp.py'), str(ports[8090])],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        slow_client = socket.create_connection(('127.0.0.1', ports[8090]), timeout=2)
        slow_client.sendall((1024 * 1024).to_bytes(4, 'big') + b'partial')
        held_sockets.append(slow_client)
        with (args.output / 'smoke.log').open('w') as log:
            subprocess.run([str(ROOT / 'scripts/smoke/smoke-multi-chat.sh')], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)

        gateway_pids = [pid for pid, name in process_names.items() if name.startswith('gateway-')]

        def total_rss_kib(pids):
            total = 0
            for pid in pids:
                status = Path(f'/proc/{pid}/status').read_text()
                for line in status.splitlines():
                    if line.startswith('VmRSS:'):
                        total += int(line.split()[1])
                        break
            return total

        rss_before = total_rss_kib(gateway_pids)
        for _ in range(args.idle_connections):
            idle_sockets.append(socket.create_connection(('127.0.0.1', ports[8090]), timeout=2))
        time.sleep(1)
        rss_with_idle = total_rss_kib(gateway_pids)
        for connection in idle_sockets:
            connection.close()
        idle_sockets.clear()
        time.sleep(1)
        rss_after_close = total_rss_kib(gateway_pids)
        (args.output / 'connections.json').write_text(json.dumps({
            'kind': 'idle unauthenticated LV TCP', 'connections': args.idle_connections,
            'gateway_processes': len(gateway_pids), 'rss_before_mib': rss_before / 1024,
            'rss_with_connections_mib': rss_with_idle / 1024,
            'rss_after_close_mib': rss_after_close / 1024,
            'rss_bytes_per_connection': ((rss_with_idle - rss_before) * 1024 / args.idle_connections
                                         if args.idle_connections else None),
        }, indent=2))

        # All counters retain their observation boundary; loopback is not physical NIC PPS.
        snapshots = []
        for scenario in scenarios:
            with (args.output / f'{scenario}.summary.json').open('w') as summary:
                command = [str(args.build / 'bench/wimiPerf'), scenario, str(args.rate), str(args.seconds),
                           str(args.workers), str(args.output / f'{scenario}.jsonl')]
                perf = subprocess.Popen(command, env=env, stdout=summary, stderr=subprocess.PIPE, text=True)
                processes.append(perf)
                process_names[perf.pid] = f'perf-{scenario}'
                deadline = time.monotonic() + args.seconds + 30
                while perf.poll() is None:
                    if time.monotonic() > deadline:
                        raise RuntimeError(f"{scenario}: driver exceeded settlement deadline")
                    row = {'scenario': scenario, 'monotonic_s': time.monotonic(),
                           'net_dev': Path('/proc/net/dev').read_text(),
                           'diskstats': Path('/proc/diskstats').read_text(), 'processes': {}}
                    for process in processes:
                        proc = Path(f'/proc/{process.pid}')
                        with contextlib.suppress(FileNotFoundError, ProcessLookupError):
                            row['processes'][str(process.pid)] = {
                                'name': process_names[process.pid],
                                'stat': (proc / 'stat').read_text(), 'status': (proc / 'status').read_text(),
                                'io': (proc / 'io').read_text()}
                    snapshots.append(row)
                    time.sleep(.5)
                error = perf.stderr.read()
                perf.stderr.close()
                (args.output / f'{scenario}.stderr').write_text(error)
                if perf.returncode:
                    raise RuntimeError(f'{scenario} failed ({perf.returncode}): {error}')
        (args.output / 'qualification.json').write_text(
            json.dumps({'lv_tcp': 'passed', 'smoke': 'passed', 'scenarios': len(scenarios)}))
    finally:
        forced = []
        if 'snapshots' in locals():
            (args.output / 'resources.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in snapshots))
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
        for process in reversed(processes):
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                forced.append({'pid': process.pid, 'name': process_names.get(process.pid, 'unknown')})
                process.kill()
                process.wait()
        for sock in held_sockets:
            sock.close()
        for sock in idle_sockets:
            sock.close()
        (args.output / 'shutdown.json').write_text(json.dumps({'forced_kills': forced}, indent=2))
        for handle in handles:
            handle.close()
        if created_schema:
            mysql(f'DROP DATABASE {schema};')
        if forced and sys.exc_info()[0] is None:
            raise RuntimeError(f'services did not stop within deadline: {forced}')


if __name__ == '__main__':
    main()
