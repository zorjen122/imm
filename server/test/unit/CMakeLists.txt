find_package(Seastar REQUIRED)

add_executable(configuration_test configuration_test.cc)
target_link_libraries(configuration_test PRIVATE imCommon)
add_test(NAME unit.configuration COMMAND configuration_test)

add_executable(grpc_security_test grpc_security_test.cc)
target_link_libraries(grpc_security_test PRIVATE imCommon imProto)
add_test(NAME unit.grpc_security COMMAND grpc_security_test)

add_executable(tcp_protocol_test tcp_protocol_test.cc)
target_link_libraries(tcp_protocol_test PRIVATE imCommon imProto)
add_test(NAME unit.tcp_protocol COMMAND tcp_protocol_test)

add_executable(gate_protocol_test gate_protocol_test.cc)
target_link_libraries(gate_protocol_test PRIVATE imGateIo)
add_test(NAME unit.gate_protocol COMMAND gate_protocol_test)

add_executable(gateway_protocol_test gateway_protocol_test.cc)
target_link_libraries(gateway_protocol_test PRIVATE imGateway)
add_test(NAME unit.gateway_protocol COMMAND gateway_protocol_test)

add_executable(io_queue_test io_queue_test.cc)
target_include_directories(io_queue_test PRIVATE "${CMAKE_SOURCE_DIR}/common/include")
target_link_libraries(io_queue_test PRIVATE Seastar::seastar)
add_test(NAME unit.io_queue COMMAND io_queue_test --smp 2 --memory 256M --overprovisioned --reactor-backend epoll)
set_tests_properties(unit.io_queue PROPERTIES TIMEOUT 30)

add_executable(file_storage_test file_storage_test.cc)
target_link_libraries(file_storage_test PRIVATE imFile)
add_test(NAME unit.file_storage COMMAND file_storage_test --smp 1 --memory 128M --overprovisioned --reactor-backend epoll)
set_tests_properties(unit.file_storage PROPERTIES TIMEOUT 30)
