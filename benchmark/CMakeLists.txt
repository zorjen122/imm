cmake_minimum_required(VERSION 4.1)

project(imm-benchmark)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Seastar REQUIRED)
find_package(LTTngUST REQUIRED)

find_package(Protobuf CONFIG REQUIRED)

function(imm_generate_proto out_var proto_file)
  get_filename_component(proto_abs "${proto_file}" ABSOLUTE)
  get_filename_component(proto_dir "${proto_abs}" DIRECTORY)
  get_filename_component(proto_name "${proto_abs}" NAME_WE)

  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${proto_name}")
  set(gen_srcs "${gen_dir}/${proto_name}.pb.cc")
  set(gen_hdrs "${gen_dir}/${proto_name}.pb.h")

  add_custom_command(
    OUTPUT ${gen_srcs} ${gen_hdrs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
    COMMAND
      $<TARGET_FILE:protobuf::protoc> --proto_path=${proto_dir}
      --cpp_out=${gen_dir} ${proto_abs}
    DEPENDS "${proto_abs}"
    COMMENT "Generating Protobuf sources for ${proto_file}"
    VERBATIM)

  set(${out_var}
      ${gen_srcs}
      PARENT_SCOPE)
  set(${out_var}_INCLUDE_DIR
      "${gen_dir}"
      PARENT_SCOPE)
endfunction()


add_executable(bm_gateway ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_benchmark.cc)
target_link_libraries(bm_gateway Seastar::seastar)


set(proto_dir "${CMAKE_CURRENT_SOURCE_DIR}/../server/common/proto")
set(proto_files gateway_message tcp_message)

foreach(proto_file IN LISTS proto_files)
    imm_generate_proto(generated_proto "${proto_dir}/${proto_file}.proto")
    target_include_directories(bm_gateway PRIVATE "${generated_proto_INCLUDE_DIR}")
    target_sources(bm_gateway PRIVATE ${generated_proto})
endforeach()

target_link_libraries(bm_gateway protobuf::libprotobuf)

# 基准客户端只依赖安装好的 gRPC 和协议定义，不链接任何服务端实现。
find_package(gRPC CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
set(rpc_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/rpc")
set(rpc_sources)
foreach(proto_name IN ITEMS state file gateway_message tcp_message)
    set(proto_file "${proto_dir}/${proto_name}.proto")
    set(outputs "${rpc_gen_dir}/${proto_name}.pb.cc" "${rpc_gen_dir}/${proto_name}.pb.h")
    set(grpc_arguments)
    if(NOT proto_name STREQUAL "tcp_message")
        list(APPEND outputs "${rpc_gen_dir}/${proto_name}.grpc.pb.cc" "${rpc_gen_dir}/${proto_name}.grpc.pb.h")
        list(APPEND grpc_arguments --grpc_out=${rpc_gen_dir} --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>)
    endif()
    add_custom_command(
        OUTPUT ${outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${rpc_gen_dir}"
        COMMAND protobuf::protoc --proto_path=${proto_dir} --cpp_out=${rpc_gen_dir} ${grpc_arguments} "${proto_file}"
        DEPENDS "${proto_file}" "${proto_dir}/tcp_message.proto" protobuf::protoc gRPC::grpc_cpp_plugin
        VERBATIM)
    list(APPEND rpc_sources ${outputs})
endforeach()
add_library(benchmark_rpc STATIC ${rpc_sources})
target_include_directories(benchmark_rpc PUBLIC "${rpc_gen_dir}")
target_link_libraries(benchmark_rpc PUBLIC gRPC::grpc++ protobuf::libprotobuf)
foreach(service IN ITEMS state message file)
    add_executable(bm_${service} "src/${service}_benchmark.cc")
    target_link_libraries(bm_${service} PRIVATE benchmark_rpc Threads::Threads)
endforeach()
target_link_libraries(bm_file PRIVATE OpenSSL::Crypto)
