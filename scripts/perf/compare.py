#!/usr/bin/env python3
"""Compare two archived reports without turning a local qualification into a capacity claim."""
import argparse
import json
from pathlib import Path

import report


def load(path):
    return report.summarize(path) if path.is_dir() else json.loads(path.read_text())


def ratio(candidate, baseline):
    return candidate / baseline if baseline else None


def aggregate_resource(resource, successes):
    processes = resource.get('processes', [])
    network = resource.get('network') or {}
    disk = resource.get('disk') or {}
    cpu_seconds = sum(row['cpu_seconds'] for row in processes)
    return {
        'cpu_ms_per_success': cpu_seconds * 1000 / successes if successes else None,
        'rss_mib': sum(row['rss_mib'] for row in processes),
        'network_packets_per_s': network.get('rx_packets_per_s', 0) + network.get('tx_packets_per_s', 0),
        'network_bytes_per_s': network.get('rx_bytes_per_s', 0) + network.get('tx_bytes_per_s', 0),
        'disk_iops': disk.get('read_ops_per_s', 0) + disk.get('write_ops_per_s', 0),
        'read_syscalls_per_s': sum(row['read_syscalls_per_s'] for row in processes),
        'write_syscalls_per_s': sum(row['write_syscalls_per_s'] for row in processes),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    baseline = load(args.baseline)
    candidate = load(args.candidate)
    comparable_keys = ('rate', 'seconds', 'workers')
    comparable = all(baseline['manifest'].get(key) == candidate['manifest'].get(key)
                     for key in comparable_keys)

    baseline_scenarios = {row['scenario']: row for row in baseline['scenarios']}
    candidate_scenarios = {row['scenario']: row for row in candidate['scenarios']}
    scenarios = []
    for name in sorted(baseline_scenarios.keys() & candidate_scenarios.keys()):
        before = baseline_scenarios[name]
        after = candidate_scenarios[name]
        before_resource = aggregate_resource(baseline['resources'].get(name, {}), before['success'])
        after_resource = aggregate_resource(candidate['resources'].get(name, {}), after['success'])
        scenarios.append({
            'scenario': name,
            'baseline': {
                'success_tps': before['success_tps_in_window'],
                'failure_fraction': before['failure_fraction'],
                'p99_ms': before['request_latency_ms']['0.99'],
                **before_resource,
            },
            'candidate': {
                'success_tps': after['success_tps_in_window'],
                'failure_fraction': after['failure_fraction'],
                'p99_ms': after['request_latency_ms']['0.99'],
                **after_resource,
            },
            'ratios': {
                'success_tps': ratio(after['success_tps_in_window'], before['success_tps_in_window']),
                'p99': ratio(after['request_latency_ms']['0.99'], before['request_latency_ms']['0.99']),
                'cpu_per_success': ratio(after_resource['cpu_ms_per_success'],
                                         before_resource['cpu_ms_per_success']),
                'rss': ratio(after_resource['rss_mib'], before_resource['rss_mib']),
            },
        })

    result = {
        'scope': 'comparison helper; dedicated repeated capacity runs are required for an adoption decision',
        'comparable_load_settings': comparable,
        'load_keys': {key: {'baseline': baseline['manifest'].get(key),
                            'candidate': candidate['manifest'].get(key)} for key in comparable_keys},
        'scenarios': scenarios,
        'connections': {'baseline': baseline.get('connections'), 'candidate': candidate.get('connections')},
    }
    encoded = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(encoded + '\n')
    else:
        print(encoded)


if __name__ == '__main__':
    main()
