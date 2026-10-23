add_library(imState options.cc service.cc)
target_include_directories(imState PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(imState PUBLIC imReactorIo)

add_executable(state main.cc)
target_link_libraries(state PRIVATE imState)
