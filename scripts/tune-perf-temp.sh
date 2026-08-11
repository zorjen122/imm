#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   为本地压测临时调整 Linux 运行时参数，并支持恢复原值。
#   该脚本只使用 sysctl -w，不会写入 /etc/sysctl.conf 或 systemd 配置。
#   文件描述符限制只能影响本脚本启动的子进程；可使用 run 子命令包装服务启动。
# 常用方式：
#   ./scripts/tune-perf-temp.sh show
#   ./scripts/tune-perf-temp.sh apply
#   ./scripts/tune-perf-temp.sh restore
#   ./scripts/tune-perf-temp.sh run ./scripts/run-local-services.sh

STATE_FILE="${WIMI_TUNE_STATE_FILE:-/tmp/wwimiperf-sysctl.state}"
NOFILE_LIMIT="${WIMI_TUNE_NOFILE:-1048576}"

PARAMS=(
  "fs.file-max=2097152"
  "fs.nr_open=2097152"
  "net.core.somaxconn=65535"
  "net.ipv4.tcp_max_syn_backlog=65535"
  "net.core.netdev_max_backlog=250000"
  "net.ipv4.tcp_keepalive_time=600"
  "net.ipv4.tcp_keepalive_intvl=30"
  "net.ipv4.tcp_keepalive_probes=5"
  "net.ipv4.ip_local_port_range=10000 65000"
  "net.ipv4.tcp_fin_timeout=15"
  "net.ipv4.tcp_tw_reuse=1"
  "vm.overcommit_memory=1"
  "vm.swappiness=1"
)

usage() {
  cat <<EOF
Usage: $0 <show|apply|restore|run -- command...>

Commands:
  show      Print current values and current shell nofile limit.
  apply     Save current sysctl values to $STATE_FILE, then apply temporary values.
  restore   Restore sysctl values saved by apply.
  run       Apply temporary values, raise nofile for the child process, then exec command.

Environment:
  WIMI_TUNE_STATE_FILE  Restore-state file. Default: /tmp/wwimiperf-sysctl.state
  WIMI_TUNE_NOFILE      Child-process nofile limit for run. Default: 1048576

Notes:
  - sysctl changes are runtime-only and vanish after reboot unless restored earlier.
  - Some parameters require root/CAP_SYS_ADMIN. Failed parameters are reported.
  - ulimit changes cannot modify an already running service.
EOF
}

param_name() {
  cut -d= -f1 <<<"$1"
}

param_value() {
  cut -d= -f2- <<<"$1"
}

get_value() {
  local name="$1"
  sysctl -n "$name" 2>/dev/null || true
}

show_values() {
  echo "nofile soft/hard: $(ulimit -Sn)/$(ulimit -Hn)"
  for entry in "${PARAMS[@]}"; do
    local name value
    name="$(param_name "$entry")"
    value="$(get_value "$name")"
    if [[ -n "$value" ]]; then
      printf '%s=%s\n' "$name" "$value"
    else
      printf '%s=<unavailable>\n' "$name"
    fi
  done
}

save_state() {
  : > "$STATE_FILE"
  for entry in "${PARAMS[@]}"; do
    local name value
    name="$(param_name "$entry")"
    value="$(get_value "$name")"
    if [[ -n "$value" ]]; then
      printf '%s=%s\n' "$name" "$value" >> "$STATE_FILE"
    fi
  done
  echo "Saved current sysctl state to $STATE_FILE"
}

apply_values() {
  local failed=0
  save_state
  for entry in "${PARAMS[@]}"; do
    local name value
    name="$(param_name "$entry")"
    value="$(param_value "$entry")"
    if sysctl -w "$name=$value" >/dev/null 2>&1; then
      printf 'applied %s=%s\n' "$name" "$value"
    else
      printf 'failed  %s=%s\n' "$name" "$value" >&2
      failed=1
    fi
  done
  return "$failed"
}

restore_values() {
  if [[ ! -f "$STATE_FILE" ]]; then
    echo "No state file found: $STATE_FILE" >&2
    exit 1
  fi

  local failed=0
  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    if sysctl -w "$entry" >/dev/null 2>&1; then
      echo "restored $entry"
    else
      echo "failed to restore $entry" >&2
      failed=1
    fi
  done < "$STATE_FILE"
  return "$failed"
}

raise_nofile() {
  local hard
  hard="$(ulimit -Hn)"
  if [[ "$hard" != "unlimited" && "$NOFILE_LIMIT" -gt "$hard" ]]; then
    NOFILE_LIMIT="$hard"
  fi
  if ulimit -n "$NOFILE_LIMIT" 2>/dev/null; then
    echo "nofile for child process: $(ulimit -Sn)/$(ulimit -Hn)"
  else
    echo "failed to raise nofile to $NOFILE_LIMIT" >&2
  fi
}

cmd="${1:-}"
case "$cmd" in
  show)
    show_values
    ;;
  apply)
    apply_values
    ;;
  restore)
    restore_values
    ;;
  run)
    shift
    if [[ "${1:-}" == "--" ]]; then
      shift
    fi
    if [[ "$#" -eq 0 ]]; then
      echo "run requires a command" >&2
      usage >&2
      exit 2
    fi
    apply_values || true
    raise_nofile
    exec "$@"
    ;;
  -h|--help|"")
    usage
    ;;
  *)
    echo "Unknown command: $cmd" >&2
    usage >&2
    exit 2
    ;;
esac
