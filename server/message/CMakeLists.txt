cmake_minimum_required(VERSION 3.16)
project(message LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(gRPC REQUIRED)

file(GLOB SOURCE src/**/*.cc)
add_library(imMessage ${SOURCE})
add_executable(message src/main.cc)

target_include_directories(imMessage PUBLIC ./include ../public/include)
target_link_directories(imMessage PUBLIC ../public/build)

target_link_libraries(
  imMessage
  PUBLIC imPublic
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
         jsoncpp)

target_compile_options(imMessage PRIVATE -Wall -g)

target_link_libraries(message PRIVATE imMessage)

target_compile_options(message PRIVATE -Wall)

if(WIMI_BUILD_UNIT_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  add_subdirectory(test)
endif()
