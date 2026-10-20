# All server targets use the same gRPC ABI. In particular the reactor client
# cannot coexist with the system 1.51 library in the Gate process.
if(TARGET gRPC::grpc++)
    set(gRPC_FOUND TRUE)
    return()
endif()
include(FetchContent)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CODEGEN OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
foreach(dependency ABSL CARES PROTOBUF RE2 SSL ZLIB)
    set(gRPC_${dependency}_PROVIDER package CACHE STRING "" FORCE)
endforeach()
# Build the Abseil revision used by this gRPC release. Distro gRPC may
# provide an older Abseil whose headers cannot compile the new transport.
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_Declare(wimi_absl
    URL https://codeload.github.com/abseil/abseil-cpp/tar.gz/refs/tags/20250512.1
    URL_HASH SHA256=9b7a064305e9fd94d124ffa6cc358592eb42b5da588fb4e07d09254aa40086db
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(wimi_absl)
set(Protobuf_DIR "${CMAKE_CURRENT_LIST_DIR}/grpc" CACHE PATH "" FORCE)
FetchContent_Declare(wimi_grpc
    URL https://codeload.github.com/grpc/grpc/tar.gz/refs/tags/v1.84.0
    URL_HASH SHA256=17a4d25717f4871eb71cfdf191f7491f66a59c8ebc782ab53d52fcbfc478959f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR wimi-populate-only)
FetchContent_Declare(wimi_grpc_proto
    URL https://codeload.github.com/grpc/grpc-proto/tar.gz/ec30f589e2519d595688b9a42f88a91bdd6b733f
    URL_HASH SHA256=5e9b520b22afbd53a662cc29017064be253c1dfa6df8958738594e7fec6ade33
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(wimi_grpc_proto)
# The release archive excludes this submodule; CMake's code-generation helpers
# expect the proto files below the gRPC source tree.
FetchContent_MakeAvailable(wimi_grpc)
file(COPY "${wimi_grpc_proto_SOURCE_DIR}/grpc" DESTINATION "${wimi_grpc_SOURCE_DIR}/third_party/grpc-proto")
add_subdirectory("${wimi_grpc_SOURCE_DIR}" "${wimi_grpc_BINARY_DIR}" EXCLUDE_FROM_ALL)
add_library(gRPC::grpc++ ALIAS grpc++)
set(gRPC_FOUND TRUE)
