#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   合并 client 与 server 各自生成的 compile_commands.json 为单一编译数据库，
#   默认输出到仓库 build/compile_commands.json，供 clangd 同时索引两个工程的源码。
# 背景：
#   clangd 只读取一个 compile_commands.json（默认查找编译单元所在目录或其父目录，
#   即仓库根目录）。client 与 server 是彼此独立的 CMake 工程，各自在 build/ 下生成
#   编译数据库，因此需要合并。合并会保留每条记录的 directory 字段，command 中的
#   相对路径仍按原构建目录解析，因此安全。
# 常用方式：
#   ./scripts/merge-compile-commands.sh
# 指定输入（默认 server=build/wimi，client=build/client-clang）：
#   WIMI_SERVER_DB=... WIMI_CLIENT_DB=... ./scripts/merge-compile-commands.sh
#   或直接追加位置参数，可传任意多个数据库：
#   ./scripts/merge-compile-commands.sh build/wimi/compile_commands.json \
#       build/client-clang/compile_commands.json
# 指定输出文件（默认仓库根 compile_commands.json）：
#   ./scripts/merge-compile-commands.sh -o /tmp/cc.json
# 前置条件：
#   已分别执行 ./scripts/build.sh 与 client 的 CMake 构建，使得
#   build/wimi/compile_commands.json 与 build/client-clang/compile_commands.json 存在。
# 注意：
#   之后若重新执行 ./scripts/build.sh，根目录 compile_commands.json 会被重新指向
#   仅含 server 的符号链接，需再次运行本脚本合并。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUTPUT="${OUTPUT:-$ROOT_DIR/build/compile_commands.json}"
: "${WIMI_SERVER_DB:=$ROOT_DIR/build/wimi/compile_commands.json}"
: "${WIMI_CLIENT_DB:=$ROOT_DIR/build/client-clang/compile_commands.json}"

usage() {
  cat <<EOF
Usage: ./scripts/merge-compile-commands.sh [-o OUTPUT] [INPUT ...]

Merge multiple compile_commands.json into one database for clangd.

Options:
  -o, --output FILE  Write merged database to FILE (default: <repo>/compile_commands.json).
  -h, --help         Show this help message.

Positional INPUT files are merged in order. When none are given, the defaults are used:
  server: $WIMI_SERVER_DB
  client: $WIMI_CLIENT_DB

Environment:
  WIMI_SERVER_DB, WIMI_CLIENT_DB  Override default inputs.
  OUTPUT                           Override default output.
EOF
}

output="$OUTPUT"
inputs=()

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    -o | --output)
      [[ "$#" -ge 2 ]] || { echo "Option $1 requires an argument." >&2; exit 2; }
      output="$2"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    -*)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      inputs+=("$1")
      shift
      ;;
  esac
done

if [[ "${#inputs[@]}" -eq 0 ]]; then
  inputs=("$WIMI_SERVER_DB" "$WIMI_CLIENT_DB")
fi

for db in "${inputs[@]}"; do
  if [[ ! -f "$db" ]]; then
    echo "Missing compile database: $db" >&2
    echo "Build it first (e.g. ./scripts/build.sh and the client CMake build)." >&2
    exit 1
  fi
done

python3 - "${inputs[@]}" "$output" <<'PY'
import json
import os
import sys


def load(path: str) -> list:
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, list):
        raise SystemExit(f"{path}: 不是 compile_commands.json（顶层应为数组）")
    return [e for e in data if isinstance(e, dict) and e.get("file")]


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: merge.py INPUT... OUTPUT", file=sys.stderr)
        return 2
    *db_paths, output = sys.argv[1:]

    entries: list = []
    for path in db_paths:
        entries.extend(load(path))

    # 按 file 去重，保留最先出现的记录（同一文件可能出现在多个构建目录）。
    seen: set = set()
    merged: list = []
    for entry in entries:
        file_key = entry["file"]
        if file_key in seen:
            continue
        seen.add(file_key)
        merged.append(entry)

    merged.sort(key=lambda e: e["file"])

    # 输出文件可能是仓库根目录指向 server 数据库的符号链接，先解除再写入，
    # 避免覆盖原构建目录下的数据库。
    if os.path.islink(output):
        os.unlink(output)

    with open(output, "w", encoding="utf-8") as fh:
        json.dump(merged, fh, indent=2, ensure_ascii=False)
        fh.write("\n")

    print(f"merged {len(entries)} entries -> {len(merged)} unique -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
PY
