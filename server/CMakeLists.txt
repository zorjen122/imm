cmake_minimum_required(VERSION 3.24)
project(imm_system VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 为工具链提供编译数据库
add_compile_definitions(FMT_CONSTEVAL=constexpr)
find_package(fmt REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ImmProto)

option(BUILD_TESTING "Build tests" ON)
option(BUILD_DEMOS "Build demo programs" OFF)

if(BUILD_TESTING)
    enable_testing()
endif()

add_subdirectory(common)
add_subdirectory(file)
add_subdirectory(message)
add_subdirectory(gate)
add_subdirectory(gateway)
add_subdirectory(state)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
