set(Seastar_LTTNG ON)
find_package(Seastar REQUIRED)
add_executable(gate_io_queue_test io_queue_test.cc)
target_include_directories(gate_io_queue_test PRIVATE ../../common/include)
target_link_libraries(gate_io_queue_test PRIVATE Seastar::seastar)
add_test(NAME gate.io_queue COMMAND gate_io_queue_test --smp 2 --memory 256M --overprovisioned --reactor-backend epoll)
set_tests_properties(gate.io_queue PROPERTIES TIMEOUT 30)

add_executable(gate_io_state_fixture state_fixture.cc)
target_link_libraries(gate_io_state_fixture PRIVATE imProto)

add_executable(gate_api_protocol_test api_protocol_test.cc)
target_link_libraries(gate_api_protocol_test PRIVATE imGateIo)
add_test(NAME gate.api_protocol COMMAND gate_api_protocol_test)
