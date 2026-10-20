add_library(imMessageIo io/mysql_store.cc io/driver.cc io/options.cc)
target_include_directories(imMessageIo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(imMessageIo PUBLIC imReactorIo)

add_library(imMessageTransport transport/server.cc)
target_include_directories(imMessageTransport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(imMessageTransport PUBLIC imMessageIo)

add_library(imMessageTransaction transaction/messages.cc transaction/friends.cc transaction/groups.cc
    transaction/files.cc transaction/service.cc api/protocol.cc)
target_include_directories(imMessageTransaction PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(imMessageTransaction PUBLIC imMessageTransport)

add_library(imMessage service.cc)
target_include_directories(imMessage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(imMessage PUBLIC imMessageTransaction)

add_executable(message main.cc)
target_link_libraries(message PRIVATE imMessage)
