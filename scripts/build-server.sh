#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   配置并构建整个 server 工程，默认输出到 build/imm-server。
#   同时开启测试目标、关闭 demo 目标，并导出 compile_commands.json 供 clangd 使用。
# 常用方式：
#   ./scripts/build-server.sh
# 可选环境变量：
#   BUILD_DIR 指定构建目录，CC/CXX 指定编译器，JOBS 指定并行构建数量。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/imm-server}"

: "${CC:=gcc}"
: "${CXX:=g++}"
export CC CXX

# 清理构建时可保留依赖源码；有缓存时直接使用，避免重复下载。
source_args=()
for dependency in imm_absl imm_grpc imm_mariadb; do
  source_dir="$ROOT_DIR/build/deps/$dependency"
  if [[ -d "$source_dir" ]]; then
    source_args+=("-DFETCHCONTENT_SOURCE_DIR_${dependency^^}=$source_dir")
  fi
done
if [[ -d "$ROOT_DIR/build/deps/imm_grpc/third_party/grpc-proto/grpc" ]]; then
  source_args+=("-DFETCHCONTENT_SOURCE_DIR_IMM_GRPC_PROTO=$ROOT_DIR/build/deps/imm_grpc/third_party/grpc-proto")
fi

if [[ -f "$ROOT_DIR/build/deps/protoc/usr/lib/x86_64-linux-gnu/libprotoc.a" ]]; then
  source_args+=("-DProtobuf_PROTOC_LIBRARY=$ROOT_DIR/build/deps/protoc/usr/lib/x86_64-linux-gnu/libprotoc.a")
fi

cmake -S "$ROOT_DIR/server" -B "$BUILD_DIR" \
  "${source_args[@]}" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DBUILD_TESTING=ON \
  -DBUILD_DEMOS=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$BUILD_DIR" -j"${JOBS:-$(nproc)}"

if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
  ln -sfn "$BUILD_DIR/compile_commands.json" "$ROOT_DIR/compile_commands.json"
fi

echo "Build complete: $BUILD_DIR"
