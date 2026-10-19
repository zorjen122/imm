cmake_minimum_required(VERSION 3.10)

project(imCommon)

set(CMAKE_EXPORT_COMPILE_COMMANDS true)
# 仅用于clangd搜索
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(gRPC MODULE REQUIRED)

# 添加 src 目录下的所有源文件
file(GLOB SOURCES src/*.cc)

set(WIMI_COMMON_PROTO_FILES
    proto/file.proto
    proto/gateway_message.proto
    proto/state.proto
    proto/tcp_message.proto)

set(WIMI_COMMON_PROTO_SRCS)
set(WIMI_COMMON_PROTO_INCLUDE_DIRS)
foreach(proto_file IN LISTS WIMI_COMMON_PROTO_FILES)
  wimi_generate_grpc_proto(GENERATED_PROTO
                          "${CMAKE_CURRENT_SOURCE_DIR}/${proto_file}")
  list(APPEND WIMI_COMMON_PROTO_SRCS ${GENERATED_PROTO})
  list(APPEND WIMI_COMMON_PROTO_INCLUDE_DIRS ${GENERATED_PROTO_INCLUDE_DIR})
endforeach()
list(REMOVE_DUPLICATES WIMI_COMMON_PROTO_INCLUDE_DIRS)

add_library(imProto STATIC ${WIMI_COMMON_PROTO_SRCS})
set_target_properties(imProto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(imProto PUBLIC ${WIMI_COMMON_PROTO_INCLUDE_DIRS})
target_link_libraries(imProto PUBLIC protobuf::libprotobuf gRPC::grpc++)

add_library(imCommon SHARED ${SOURCES})

#add_library(imCommonStatic STATIC ${SOURCES})

target_include_directories(imCommon PUBLIC ./include)
target_link_libraries(
  imCommon
  PUBLIC imProto
         Threads::Threads
         protobuf::libprotobuf
         Boost::filesystem
         gRPC::grpc++
         fmt::fmt
         yaml-cpp
         spdlog
         hiredis
         redis++
         mysqlcppconnx)

set(Seastar_LTTNG ON)
find_package(Seastar REQUIRED)
include(ReactorMysql)
find_package(OpenSSL REQUIRED)
add_library(imReactorIo STATIC reactor/dns_resolver.cc reactor/grpc_engine.cc reactor/redis_connection.cc reactor/mysql_connection.cc)
target_include_directories(imReactorIo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" include)
target_link_libraries(imReactorIo PUBLIC imCommon Seastar::seastar mariadbclient OpenSSL::SSL)
