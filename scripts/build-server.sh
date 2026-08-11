#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   配置并构建整个 server 工程，默认输出到 build/wimi。
#   同时开启测试目标、关闭 demo 目标，并导出 compile_commands.json 供 clangd 使用。
# 常用方式：
#   ./scripts/build.sh
# 可选环境变量：
#   BUILD_DIR 指定构建目录，CC/CXX 指定编译器，JOBS 指定并行构建数量。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/wimi-server}"

: "${CC:=clang}"
: "${CXX:=clang++}"
export CC CXX

cmake -S "$ROOT_DIR/server" -B "$BUILD_DIR" \
  -DBUILD_TESTING=ON \
  -DBUILD_DEMOS=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$BUILD_DIR" -j"${JOBS:-$(nproc)}"

if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
  ln -sfn "$BUILD_DIR/compile_commands.json" "$ROOT_DIR/compile_commands.json"
fi

echo "Build complete: $BUILD_DIR"
