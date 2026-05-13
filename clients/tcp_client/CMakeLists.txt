add_executable(pif-tcp-client tcp_client.c)

target_include_directories(pif-tcp-client PRIVATE
    ${CMAKE_SOURCE_DIR}/server
)
