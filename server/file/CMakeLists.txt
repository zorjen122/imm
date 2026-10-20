cmake_minimum_required(VERSION 3.16)
project(file LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC MODULE REQUIRED)
find_package(OpenSSL REQUIRED)

file(GLOB SOURCE CONFIGURE_DEPENDS src/*.cc)
list(REMOVE_ITEM SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc")
add_library(imFile ${SOURCE})
add_executable(file src/main.cc)

target_include_directories(imFile PUBLIC ./include ../common/include)
target_link_directories(imFile PUBLIC ../common/build)

target_link_libraries(
  imFile
  PUBLIC imCommon
         Threads::Threads
         protobuf::libprotobuf
         imCommon
         gRPC::grpc++
         OpenSSL::Crypto
         fmt::fmt
         spdlog
         yaml-cpp
         Threads::Threads)

target_compile_options(imFile PRIVATE -Wall -g)

target_link_libraries(file PRIVATE imFile)

target_compile_options(file PRIVATE -Wall)
