cmake_minimum_required(VERSION 3.16)
project(connection_gateway LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(gRPC REQUIRED)
set(Seastar_LTTNG ON)
find_package(Seastar REQUIRED)

file(GLOB_RECURSE SOURCE CONFIGURE_DEPENDS src/*.cc)
list(REMOVE_ITEM SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc")
add_library(imConnectionGateway ${SOURCE})
add_executable(gateway src/main.cc)

target_include_directories(imConnectionGateway PUBLIC ./include ../common/include)
target_link_libraries(
  imConnectionGateway
  PUBLIC imCommon
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
         mysqlcppconnx
         jsoncpp
         Seastar::seastar)
target_compile_options(imConnectionGateway PRIVATE -Wall -g)
target_compile_definitions(imConnectionGateway PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)

target_link_libraries(gateway PRIVATE imConnectionGateway)
target_compile_options(gateway PRIVATE -Wall)
