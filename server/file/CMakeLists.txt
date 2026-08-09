cmake_minimum_required(VERSION 3.16)
project(file LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(gRPC REQUIRED)
find_package(OpenSSL REQUIRED)

file(GLOB SOURCE src/**/*.cc)
add_library(imFile ${SOURCE})
add_executable(file src/main.cc)

target_include_directories(imFile PUBLIC ./include ../public/include)
target_link_directories(imFile PUBLIC ../public/build)

target_link_libraries(
  imFile
  PUBLIC imPublic
         Threads::Threads
         protobuf::libprotobuf
         imPublic
         gRPC::grpc++
         gRPC::grpc++_reflection
         OpenSSL::Crypto
         fmt::fmt
         spdlog
         yaml-cpp
         Threads::Threads)

target_compile_options(imFile PRIVATE -Wall -g)

target_link_libraries(file PRIVATE imFile)

target_compile_options(file PRIVATE -Wall)

if(WIMI_BUILD_UNIT_TESTS)
  add_subdirectory(test)
endif()
