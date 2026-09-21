cmake_minimum_required(VERSION 3.16)
project(gate LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(gRPC REQUIRED)
find_package(CURL REQUIRED)
set(Seastar_LTTNG ON)
find_package(Seastar REQUIRED)

file(GLOB SOURCE CONFIGURE_DEPENDS src/*.cc)
list(REMOVE_ITEM SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc")
add_library(imGate ${SOURCE})
add_executable(gate src/main.cc)

target_include_directories(imGate PUBLIC ./include ../common/include)
target_link_directories(imGate PUBLIC ../common/build)

target_link_libraries(
  imGate
  PUBLIC imCommon
         Threads::Threads
         protobuf::libprotobuf
         Boost::filesystem
         gRPC::grpc++
         gRPC::grpc++_reflection
         CURL::libcurl
         fmt::fmt
         yaml-cpp
         spdlog
         hiredis
         redis++
         mysqlcppconnx
         jsoncpp)

target_compile_options(imGate PRIVATE -Wall -g)

target_link_libraries(gate PRIVATE imGate Seastar::seastar)

target_compile_options(gate PRIVATE -Wall)
