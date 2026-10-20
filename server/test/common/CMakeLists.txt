cmake_minimum_required(VERSION 3.10)

project(common_test)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


include(CTest)

if(BUILD_TESTING)
  add_executable(configuration_test configuration_test.cc)
  target_link_libraries(configuration_test PRIVATE imCommon)
  target_compile_definitions(configuration_test PRIVATE IMM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/..")
  add_test(NAME common.configuration COMMAND configuration_test)

  add_executable(gateway_security_test gateway_security_test.cc)
  target_link_libraries(gateway_security_test PRIVATE imCommon imProto)
  add_test(NAME common.gateway_security COMMAND gateway_security_test)

  add_executable(tcp_packet_protocol_test tcp_packet_protocol_test.cc)
  target_link_libraries(tcp_packet_protocol_test PRIVATE imCommon imProto)
  add_test(NAME common.tcp_packet_protocol COMMAND tcp_packet_protocol_test)
endif()
