#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 PID OUTPUT_DIR [SECONDS]" >&2
  exit 2
fi

pid="$1"
output_dir="$2"
seconds="${3:-30}"
if [[ ! "$pid" =~ ^[1-9][0-9]*$ || ! "$seconds" =~ ^[1-9][0-9]*$ || "$seconds" -gt 60 ]]; then
  echo "PID and SECONDS must be positive integers; SECONDS is capped at 60." >&2
  exit 2
fi
if ! kill -0 "$pid" 2>/dev/null; then
  echo "Process $pid is not running." >&2
  exit 1
fi
if ! command -v perf >/dev/null 2>&1; then
  echo "perf is required." >&2
  exit 127
fi

mkdir -p "$output_dir"
perf stat -p "$pid" \
  -e task-clock,cycles,instructions,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
  -o "$output_dir/perf-stat.txt" -- sleep "$seconds"
perf record -q -g -p "$pid" -o "$output_dir/perf.data" -- sleep "$seconds"
perf report --stdio -i "$output_dir/perf.data" > "$output_dir/perf-report.txt"
