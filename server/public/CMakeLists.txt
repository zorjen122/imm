cmake_minimum_required(VERSION 3.10)

project(imPublic)

set(CMAKE_EXPORT_COMPILE_COMMANDS true)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
# 仅用于clangd搜索
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(gRPC REQUIRED)

# 添加 src 目录下的所有源文件
file(GLOB SOURCES src/*.cc)

set(WIMI_PUBLIC_PROTO_FILES
    proto/file.proto
    proto/gateway_message.proto
    proto/state.proto
    proto/tcp_message.proto)

set(WIMI_PUBLIC_PROTO_SRCS)
set(WIMI_PUBLIC_PROTO_INCLUDE_DIRS)
foreach(proto_file IN LISTS WIMI_PUBLIC_PROTO_FILES)
  wimi_generate_grpc_proto(GENERATED_PROTO
                          "${CMAKE_CURRENT_SOURCE_DIR}/${proto_file}")
  list(APPEND WIMI_PUBLIC_PROTO_SRCS ${GENERATED_PROTO})
  list(APPEND WIMI_PUBLIC_PROTO_INCLUDE_DIRS ${GENERATED_PROTO_INCLUDE_DIR})
endforeach()
list(REMOVE_DUPLICATES WIMI_PUBLIC_PROTO_INCLUDE_DIRS)

add_library(imProto STATIC ${WIMI_PUBLIC_PROTO_SRCS})
set_target_properties(imProto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(imProto PUBLIC ${WIMI_PUBLIC_PROTO_INCLUDE_DIRS})
target_link_libraries(imProto PUBLIC protobuf::libprotobuf gRPC::grpc++
                                    gRPC::grpc++_reflection)

add_library(imPublic SHARED ${SOURCES})

#add_library(imPublicStatic STATIC ${SOURCES})

target_include_directories(imPublic PUBLIC ./include)
target_link_libraries(
  imPublic
  PUBLIC imProto
         Threads::Threads
         protobuf::libprotobuf
         Boost::filesystem
         gRPC::grpc++
         gRPC::grpc++_reflection
         fmt::fmt
         yaml-cpp
         spdlog
         hiredis
         redis++
         mysqlcppconnx)

if(BUILD_TESTING)
  add_executable(resourceDeadlineTest test/resourceDeadlineTest.cc)
  target_include_directories(resourceDeadlineTest PRIVATE ./include)
  target_link_libraries(resourceDeadlineTest PRIVATE imPublic imProto)
  add_test(NAME public.resource_deadline COMMAND resourceDeadlineTest)
  add_executable(redisSentinelFailoverTest test/redisSentinelFailoverTest.cc)
  target_include_directories(redisSentinelFailoverTest PRIVATE ./include)
  target_link_libraries(redisSentinelFailoverTest PRIVATE imPublic imProto)
  add_executable(gatewaySecurityTest test/gatewaySecurityTest.cc)
  target_include_directories(gatewaySecurityTest PRIVATE ./include)
  target_link_libraries(gatewaySecurityTest PRIVATE imPublic imProto)
  add_test(NAME public.gateway_security COMMAND gatewaySecurityTest)
  add_executable(tcpPacketProtocolTest test/tcpPacketProtocolTest.cc)
  target_include_directories(tcpPacketProtocolTest PRIVATE ./include)
  target_link_libraries(tcpPacketProtocolTest PRIVATE imPublic imProto)
  add_test(NAME public.tcp_packet_protocol COMMAND tcpPacketProtocolTest)
endif()

# 单元测试
if(WIMI_BUILD_UNIT_TESTS)
  add_subdirectory(test)
endif()
