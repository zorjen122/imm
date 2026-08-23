cmake_minimum_required(VERSION 3.16)
project(state LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

file(GLOB SOURCE CONFIGURE_DEPENDS src/*.cc)
list(REMOVE_ITEM SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc")
add_library(imState ${SOURCE})
add_executable(state src/main.cc)

target_include_directories(imState PUBLIC ./include ../common/include)
target_link_directories(imState PUBLIC ../common/build)

target_link_libraries(
  imState
  PUBLIC imCommon
         Threads::Threads
         protobuf::libprotobuf
         gRPC::grpc++
         gRPC::grpc++_reflection
         fmt::fmt
         yaml-cpp
         spdlog
         hiredis
         redis++
         mysqlcppconnx
         jsoncpp)

target_compile_options(imState PRIVATE -Wall -g)

target_link_libraries(state PRIVATE imState)

target_compile_options(state PRIVATE -Wall)
