cmake_minimum_required(VERSION 3.16)
project(pif C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall)

find_package(PkgConfig REQUIRED)

add_subdirectory(server)
add_subdirectory(clients/client)
add_subdirectory(clients/tcp_client)
