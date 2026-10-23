#!/usr/bin/env bash
set -euo pipefail

# 依赖由 scripts/install_dependency.sh 安装。

# 用法：./benchmark/run_gateway.sh 用户名 密码 [连接数] [shard数] [每连接发送次数]
# 服务需提前启动；可用 GATE_URL 和 GATEWAY_ENDPOINT 指定地址。
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
username="${1:?用法：./benchmark/run_gateway.sh 用户名 密码 [连接数] [shard数] [每连接发送次数]}"
password="${2:?缺少密码参数}"
connections="${3:-1}"
smp_nr="${4:-$(nproc)}"
sent_nr="${5:-1}"

response="$(jq -n --arg username "$username" --arg password "$password" \
    '{username: $username, password: $password}' |
    curl -fsS "${GATE_URL:-http://127.0.0.1:18080}/post-signIn" \
        -H 'Content-Type: application/json' --data-binary @-)"
unset password

uid="$(jq -r '.uid // empty' <<< "$response")"
IMM_BENCH_TOKEN="$(jq -r '.chatToken // empty' <<< "$response")"
export IMM_BENCH_TOKEN
if [[ -z "$uid" || -z "$IMM_BENCH_TOKEN" ]]; then
    echo "登录响应缺少 uid 或 chatToken" >&2
    exit 1
fi

cmake -S "$root_dir/benchmark" -B "$root_dir/build/benchmark" \
    -DCMAKE_PREFIX_PATH="${IMM_DEPENDENCY_PREFIX:-$HOME/.local/imm}"
cmake --build "$root_dir/build/benchmark" -j 2
exec "$root_dir/build/benchmark/bm_gateway" \
    --endpoint "${GATEWAY_ENDPOINT:-127.0.0.1:8090}" \
    --uid "$uid" --connections "$connections" --sent "$sent_nr" \
    --smp "$smp_nr" --memory 128M --overprovisioned --reactor-backend io_uring
