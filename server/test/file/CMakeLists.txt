cmake_minimum_required(VERSION 3.10)

project(file_test)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)

find_package(GTest REQUIRED)
include(CTest)

function(add_file_test target)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PUBLIC include)
  target_link_libraries(${target} PRIVATE GTest::GTest GTest::Main imFile)
  target_compile_options(${target} PRIVATE -Wall -g)
  add_test(NAME ${CMAKE_PROJECT_NAME}.${target} COMMAND ${target})
endfunction()

add_file_test(integration_file_client_test integration/file_client_test.cc)
