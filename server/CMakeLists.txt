cmake_minimum_required(VERSION 3.20)
project(wimi_system VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 为工具链提供编译数据库
add_compile_definitions(FMT_CONSTEVAL=constexpr)
find_package(fmt REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(WimiProto)

option(BUILD_TESTING "Build tests" ON)
option(BUILD_DEMOS "Build demo programs" OFF)
option(WIMI_BUILD_UNIT_TESTS "Build per-service unit tests" OFF)

if(BUILD_TESTING)
    enable_testing()
endif()

add_subdirectory(common)
add_subdirectory(file)
add_subdirectory(message)
add_subdirectory(gate)
add_subdirectory(gateway)
add_subdirectory(state)

if(BUILD_TESTING OR WIMI_BUILD_UNIT_TESTS)
    add_subdirectory(test)
endif()

option(WIMI_BUILD_BENCHMARKS "Build protocol-aware performance clients" OFF)
if(WIMI_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
