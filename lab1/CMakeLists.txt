cmake_minimum_required(VERSION 3.16)
project(NexusChat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(chat_server server.cpp)
add_executable(chat_client client.cpp)

target_link_libraries(chat_server PRIVATE ws2_32)
target_link_libraries(chat_client PRIVATE ws2_32)

if(MSVC)
    target_compile_options(chat_server PRIVATE /W4 /EHsc /utf-8)
    target_compile_options(chat_client PRIVATE /W4 /EHsc /utf-8)
else()
    target_compile_options(chat_server PRIVATE -Wall -Wextra)
    target_compile_options(chat_client PRIVATE -Wall -Wextra)
endif()
