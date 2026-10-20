#!/usr/bin/env python3
"""Summarize raw samples without hiding generator misses or late completions."""
import json
import math
from pathlib import Path
import sys


def key_values(source):
    result = {}
    for line in source.splitlines():
        if ':' not in line:
            continue
        key, value = line.split(':', 1)
        result[key] = value.strip().split()[0] if value.strip() else ''
    return result


def network_counters(source, interface):
    for line in source.splitlines():
        if ':' not in line:
            continue
        name, values = line.split(':', 1)
        if name.strip() != interface:
            continue
        fields = [int(value) for value in values.split()]
        return {'rx_bytes': fields[0], 'rx_packets': fields[1],
                'tx_bytes': fields[8], 'tx_packets': fields[9]}
    return None


def disk_counters(source, device):
    if not device:
        return None
    for line in source.splitlines():
        fields = line.split()
        if len(fields) >= 14 and fields[2] == device:
            return {'read_ops': int(fields[3]), 'read_sectors': int(fields[5]),
                    'write_ops': int(fields[7]), 'write_sectors': int(fields[9])}
    return None


def process_counters(snapshot):
    stat = snapshot['stat']
    fields = stat[stat.rfind(')') + 2:].split()
    status = key_values(snapshot['status'])
    io = key_values(snapshot['io'])
    return {
        'name': snapshot.get('name', 'unknown'),
        'cpu_ticks': int(fields[11]) + int(fields[12]),
        'rss_kib': int(status.get('VmRSS', 0)),
        'read_syscalls': int(io.get('syscr', 0)),
        'write_syscalls': int(io.get('syscw', 0)),
        'read_bytes': int(io.get('read_bytes', 0)),
        'write_bytes': int(io.get('write_bytes', 0)),
    }


def rate_delta(first, last, elapsed):
    if first is None or last is None or elapsed <= 0:
        return None
    return {key + '_per_s': (last[key] - first[key]) / elapsed for key in first}


def summarize_resources(directory, manifest, successes):
    path = directory / 'resources.jsonl'
    if not path.exists():
        return {}
    grouped = {}
    for line in path.read_text().splitlines():
        row = json.loads(line)
        grouped.setdefault(row['scenario'], []).append(row)

    clock_ticks = __import__('os').sysconf('SC_CLK_TCK')
    reports = {}
    for scenario, rows in grouped.items():
        if len(rows) < 2:
            continue
        first, last = rows[0], rows[-1]
        elapsed = last['monotonic_s'] - first['monotonic_s']
        report = {
            'observed_s': elapsed,
            'network': rate_delta(network_counters(first['net_dev'], manifest.get('network_interface', 'lo')),
                                  network_counters(last['net_dev'], manifest.get('network_interface', 'lo')), elapsed),
            'disk': rate_delta(disk_counters(first['diskstats'], manifest.get('block_device')),
                               disk_counters(last['diskstats'], manifest.get('block_device')), elapsed),
            'processes': [],
        }
        for pid in sorted(set(first['processes']) & set(last['processes'])):
            before = process_counters(first['processes'][pid])
            after = process_counters(last['processes'][pid])
            cpu_seconds = (after['cpu_ticks'] - before['cpu_ticks']) / clock_ticks
            success = successes.get(scenario, 0)
            report['processes'].append({
                'name': after['name'], 'pid': int(pid), 'cpu_seconds': cpu_seconds,
                'cpu_ms_per_success': cpu_seconds * 1000 / success if success else None,
                'rss_mib': after['rss_kib'] / 1024,
                'read_syscalls_per_s': (after['read_syscalls'] - before['read_syscalls']) / elapsed,
                'write_syscalls_per_s': (after['write_syscalls'] - before['write_syscalls']) / elapsed,
                'storage_read_bytes_per_s': (after['read_bytes'] - before['read_bytes']) / elapsed,
                'storage_write_bytes_per_s': (after['write_bytes'] - before['write_bytes']) / elapsed,
            })
        reports[scenario] = report
    return reports


def percentile(values, quantile):
    return sorted(values)[max(0, math.ceil(len(values) * quantile) - 1)] if values else None


def summarize(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    reports = []
    for path in sorted(directory.glob('*.summary.json')):
        summary = json.loads(path.read_text())
        samples = [json.loads(line) for line in path.with_name(path.name.replace('.summary.json', '.jsonl')).read_text().splitlines()]
        assert len(samples) == summary['planned']
        assert len({row['sequence'] for row in samples}) == len(samples)
        success = [row for row in samples if row['success']]
        assert len(success) == summary['success']
        errors = {}
        for row in samples:
            if not row['success']:
                errors[row['error']] = errors.get(row['error'], 0) + 1
        summary['errors'] = errors
        summary['failure_fraction'] = 1 - len(success) / len(samples)
        for metric, start in [('scheduled_latency_ms', 'scheduled_s'), ('request_latency_ms', 'sent_s')]:
            values = [(row['completed_s'] - row[start]) * 1000 for row in success]
            summary[metric] = {str(q): percentile(values, q) for q in [.5, .95, .99, .999, 1]}
        summary['generator_delay_ms_p99'] = percentile([(row['sent_s'] - row['scheduled_s']) * 1000 for row in samples], .99)
        reports.append(summary)
    resources = summarize_resources(directory, manifest, {row['scenario']: row['success'] for row in reports})
    connection_path = directory / 'connections.json'
    connections = json.loads(connection_path.read_text()) if connection_path.exists() else None
    return {'scope': 'local qualification; not a sustainable capacity measurement',
            'manifest': manifest, 'scenarios': reports, 'resources': resources,
            'connections': connections}


if __name__ == '__main__':
    print(json.dumps(summarize(Path(sys.argv[1])), indent=2))
