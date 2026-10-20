cmake_minimum_required(VERSION 3.10)

project(common_test)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


include(CTest)

if(BUILD_TESTING)
  add_executable(redisSentinelFailoverTest redisSentinelFailoverTest.cc)
  target_link_libraries(redisSentinelFailoverTest PRIVATE imCommon imProto)

  add_executable(gatewaySecurityTest gatewaySecurityTest.cc)
  target_link_libraries(gatewaySecurityTest PRIVATE imCommon imProto)
  add_test(NAME common.gateway_security COMMAND gatewaySecurityTest)

  add_executable(tcpPacketProtocolTest tcpPacketProtocolTest.cc)
  target_link_libraries(tcpPacketProtocolTest PRIVATE imCommon imProto)
  add_test(NAME common.tcp_packet_protocol COMMAND tcpPacketProtocolTest)
endif()

if(WIMI_BUILD_UNIT_TESTS)
  find_package(GTest REQUIRED)

  function(add_common_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE GTest::GTest GTest::Main imCommon)

    add_test(NAME ${CMAKE_PROJECT_NAME}.${target} COMMAND ${target})
  endfunction()

  add_common_test(test_mysql_dao mysqlDaoTest.cc)
  add_common_test(test_redis_dao redisDaoTest.cc)
endif()
