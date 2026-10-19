cmake_minimum_required(VERSION 3.16)
project(gate LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(gRPC MODULE REQUIRED)
set(Seastar_LTTNG ON)
find_package(Seastar REQUIRED)

set(API_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/api")
add_custom_command(
  OUTPUT "${API_PROTO_DIR}/gate_api.pb.cc" "${API_PROTO_DIR}/gate_api.pb.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${API_PROTO_DIR}"
  COMMAND ${Protobuf_PROTOC_EXECUTABLE} --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
          --cpp_out=${API_PROTO_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/proto/gate_api.proto
  DEPENDS proto/gate_api.proto
  VERBATIM)
add_library(imGateApi "${API_PROTO_DIR}/gate_api.pb.cc")
target_include_directories(imGateApi PUBLIC "${API_PROTO_DIR}")
target_link_libraries(imGateApi PUBLIC protobuf::libprotobuf)

add_executable(gate main.cc)

target_compile_options(gate PRIVATE -Wall)

file(GLOB IO_SOURCE CONFIGURE_DEPENDS io/*.cc api/*.cc transaction/*.cc)
add_library(imGateIo ${IO_SOURCE})
target_include_directories(imGateIo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" ../common/include)
target_link_libraries(imGateIo PUBLIC imReactorIo imGateApi Seastar::seastar)
target_link_libraries(gate PRIVATE imGateIo)

target_compile_options(imGateIo PRIVATE -Wall -g)
