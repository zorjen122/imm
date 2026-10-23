cmake_minimum_required(VERSION 3.24)
project(imm_dependency LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard shared by dependencies and services" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_INSTALL_LIBDIR lib)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
include(FetchContent)

if(IMM_DEPENDENCY STREQUAL "absl")
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(imm_absl
        URL https://codeload.github.com/abseil/abseil-cpp/tar.gz/refs/tags/20250512.1
        URL_HASH SHA256=9b7a064305e9fd94d124ffa6cc358592eb42b5da588fb4e07d09254aa40086db
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imm_absl)
elseif(IMM_DEPENDENCY STREQUAL "protobuf")
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(protobuf_LOCAL_DEPENDENCIES_ONLY ON CACHE BOOL "" FORCE)
    # 使用 gRPC 官方固定的 Protobuf 提交，其 Abseil 依赖同为 20250512.1。
    FetchContent_Declare(imm_protobuf
        URL https://codeload.github.com/protocolbuffers/protobuf/tar.gz/35cd01f9fe9afbeea38cc7b979a3b6bfcde82c03
        URL_HASH SHA256=f8264c6e56b1fe59d06aa1b99ee81f76246ed2b46cf2c86b8a50f8482fa05665
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imm_protobuf)
elseif(IMM_DEPENDENCY STREQUAL "re2")
    set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(imm_re2
        URL https://codeload.github.com/google/re2/tar.gz/refs/tags/2022-04-01
        URL_HASH SHA256=1ae8ccfdb1066a731bba6ee0881baad5efd2cd661acd9569b689f2586e1a50e9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imm_re2)
elseif(IMM_DEPENDENCY STREQUAL "grpc")
    set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(gRPC_INSTALL ON CACHE BOOL "" FORCE)
    set(gRPC_BUILD_CODEGEN ON CACHE BOOL "" FORCE)
    foreach(plugin CSHARP NODE OBJECTIVE_C PHP PYTHON RUBY)
        set(gRPC_BUILD_GRPC_${plugin}_PLUGIN OFF CACHE BOOL "" FORCE)
    endforeach()
    foreach(dependency ABSL CARES PROTOBUF RE2 SSL ZLIB)
        set(gRPC_${dependency}_PROVIDER package CACHE STRING "" FORCE)
    endforeach()
    FetchContent_Declare(imm_grpc
        URL https://codeload.github.com/grpc/grpc/tar.gz/refs/tags/v1.84.0
        URL_HASH SHA256=17a4d25717f4871eb71cfdf191f7491f66a59c8ebc782ab53d52fcbfc478959f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR imm-populate-only)
    FetchContent_Declare(imm_grpc_proto
        URL https://codeload.github.com/grpc/grpc-proto/tar.gz/ec30f589e2519d595688b9a42f88a91bdd6b733f
        URL_HASH SHA256=5e9b520b22afbd53a662cc29017064be253c1dfa6df8958738594e7fec6ade33
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imm_grpc_proto imm_grpc)
    file(COPY "${imm_grpc_proto_SOURCE_DIR}/grpc" DESTINATION "${imm_grpc_SOURCE_DIR}/third_party/grpc-proto")
    add_subdirectory("${imm_grpc_SOURCE_DIR}" "${imm_grpc_BINARY_DIR}")
    # 此扩展没有公开安装规则，但 reactor 的 DNS 适配依赖它的接口。
    install(FILES "${imm_grpc_SOURCE_DIR}/src/core/lib/event_engine/extensions/blocking_dns.h"
        DESTINATION include/src/core/lib/event_engine/extensions)
elseif(IMM_DEPENDENCY STREQUAL "mariadb")
    add_subdirectory(mariadb)
elseif(IMM_DEPENDENCY STREQUAL "seastar")
    set(Seastar_API_LEVEL 10 CACHE STRING "" FORCE)
    set(Seastar_APPS OFF CACHE BOOL "" FORCE)
    set(Seastar_DEMOS OFF CACHE BOOL "" FORCE)
    set(Seastar_DOCS OFF CACHE BOOL "" FORCE)
    set(Seastar_TESTING OFF CACHE BOOL "" FORCE)
    set(Seastar_INSTALL ON CACHE BOOL "" FORCE)
    set(Seastar_DPDK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(imm_seastar
        URL https://codeload.github.com/scylladb/seastar/tar.gz/00da3e90c65de411df25760e0a40dfb63a65adaf
        URL_HASH SHA256=9f53b6fb3b4539f82aa4705a7f314cfb664029afe612e182f0d2c3b37452ba57
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # metrics2.pb.cc 必须由同一前缀的 protoc 生成并链接同版 runtime。
    find_package(Protobuf CONFIG REQUIRED)
    FetchContent_MakeAvailable(imm_seastar)
else()
    message(FATAL_ERROR "IMM_DEPENDENCY must be absl, protobuf, re2, grpc, mariadb or seastar")
endif()
