find_package(OpenSSL REQUIRED)

add_library(imFile options.cc protocol.cc storage.cc service.cc)
target_include_directories(imFile PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(imFile PUBLIC imReactorIo OpenSSL::Crypto)

add_executable(file main.cc)
target_link_libraries(file PRIVATE imFile)
