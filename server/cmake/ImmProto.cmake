find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

function(imm_generate_grpc_proto out_var proto_file)
  get_filename_component(proto_abs "${proto_file}" ABSOLUTE)
  get_filename_component(proto_dir "${proto_abs}" DIRECTORY)
  get_filename_component(proto_name "${proto_abs}" NAME_WE)

  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${proto_name}")
  set(gen_srcs
      "${gen_dir}/${proto_name}.pb.cc"
      "${gen_dir}/${proto_name}.grpc.pb.cc")
  set(gen_hdrs
      "${gen_dir}/${proto_name}.pb.h"
      "${gen_dir}/${proto_name}.grpc.pb.h")

  add_custom_command(
    OUTPUT ${gen_srcs} ${gen_hdrs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
    COMMAND
      ${Protobuf_PROTOC_EXECUTABLE} --proto_path=${proto_dir}
      --cpp_out=${gen_dir} --grpc_out=${gen_dir}
      --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN} ${proto_abs}
    DEPENDS "${proto_abs}"
    COMMENT "Generating gRPC sources for ${proto_file}"
    VERBATIM)

  set(${out_var}
      ${gen_srcs}
      PARENT_SCOPE)
  set(${out_var}_INCLUDE_DIR
      "${gen_dir}"
      PARENT_SCOPE)
endfunction()
