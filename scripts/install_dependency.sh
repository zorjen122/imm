#!/usr/bin/env bash
set -euo pipefail

# 用途：在 Ubuntu 上安装服务端和 benchmark 的构建依赖。
#   缺少的系统开发包通过 apt 安装，普通用户会调用 sudo；源码依赖安装到指定前缀。
#   不安装或初始化 MySQL/Redis 服务。普通服务构建只链接安装结果。
# 使用方式（从仓库根目录执行）：
#   JOBS=8 ./scripts/install_dependency.sh
#   JOBS=8 ./scripts/install_dependency.sh --prefix /path/to/dependencies
# 默认安装位置：$HOME/.local/imm，可通过 IMM_DEPENDENCY_PREFIX 或 --prefix 修改。
# 可选环境变量：
#   JOBS：编译并行数，默认使用 nproc；CC/CXX：C/C++ 编译器，默认 gcc/g++。
#   IMM_DEPENDENCY_BUILD_DIR：安装构建缓存目录，默认 build/dependency。
#   IMM_DEPENDENCY_SOURCE_DIR：已有源码缓存目录，默认 build/deps。
# 安装后构建服务：
#   ./scripts/build-server.sh
# 自定义安装位置时，构建需使用相同位置：
#   IMM_DEPENDENCY_PREFIX=/path/to/dependencies ./scripts/build-server.sh
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${IMM_DEPENDENCY_PREFIX:-$HOME/.local/imm}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "--prefix requires a path" >&2
                exit 2
            fi
            prefix="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [--prefix PATH]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# 只安装缺少的开发包，不升级已有包，也不安装或初始化 MySQL/Redis 服务。
packages=(
    build-essential gcc-14 g++-14 cmake pkg-config ca-certificates curl openssl jq
    libboost-all-dev libfmt-dev libyaml-cpp-dev libspdlog-dev libjsoncpp-dev
    libssl-dev libc-ares-dev zlib1g-dev libhiredis-dev libgnutls28-dev
    libhwloc-dev liblttng-ust-dev liburing-dev libnuma-dev libsctp-dev
    liblz4-dev libpciaccess-dev libxml2-dev libtool ninja-build ragel
    python3-pyelftools python3-yaml
)
missing=()
for package in "${packages[@]}"; do
    if [[ "$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)" != "install ok installed" ]]; then
        missing+=("$package")
    fi
done
if ((${#missing[@]})); then
    privilege=()
    if [[ $(id -u) != 0 ]]; then
        privilege=(sudo)
    fi
    "${privilege[@]}" apt-get update
    "${privilege[@]}" apt-get install -y --no-install-recommends "${missing[@]}"
fi
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
dependency_build="${IMM_DEPENDENCY_BUILD_DIR:-$ROOT_DIR/build/dependency}"
source_cache="${IMM_DEPENDENCY_SOURCE_DIR:-$ROOT_DIR/build/deps}"

source_args=()
for dependency in imm_absl imm_protobuf imm_re2 imm_grpc imm_mariadb imm_seastar; do
    if [[ -d "$source_cache/$dependency" ]]; then
        source_args+=("-DFETCHCONTENT_SOURCE_DIR_${dependency^^}=$source_cache/$dependency")
    fi
done
if [[ -d "$source_cache/imm_grpc/third_party/grpc-proto/grpc" ]]; then
    source_args+=("-DFETCHCONTENT_SOURCE_DIR_IMM_GRPC_PROTO=$source_cache/imm_grpc/third_party/grpc-proto")
fi

for dependency in absl protobuf re2 grpc mariadb seastar; do
    cmake -S "$ROOT_DIR/scripts/dependency" -B "$dependency_build/$dependency" \
        "${source_args[@]}" \
        -DIMM_DEPENDENCY="$dependency" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}" \
        -DCMAKE_C_COMPILER="${CC:-gcc}" \
        -DCMAKE_CXX_COMPILER="${CXX:-g++}" \
        -DBUILD_SHARED_LIBS=ON
    cmake --build "$dependency_build/$dependency" -j"${JOBS:-$(nproc)}"
    cmake --install "$dependency_build/$dependency"
done
echo "Dependencies installed: $prefix"
