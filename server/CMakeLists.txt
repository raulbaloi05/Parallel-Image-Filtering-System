cmake_minimum_required(VERSION 3.10)
project(pif)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRAPHICSMAGICK REQUIRED GraphicsMagick)

find_library(GSOAP_LIB gsoap REQUIRED)
find_path(GSOAP_INCLUDE stdsoap2.h REQUIRED)

add_executable(pif_server
    server.c
    soapC.c
    soapServer.c
    processing.c
    unix_server.c
    tcp_server.c
)

target_include_directories(pif_server PRIVATE
    ${GSOAP_INCLUDE}
    ${GRAPHICSMAGICK_INCLUDE_DIRS}
)

target_link_libraries(pif_server PRIVATE
    ${GSOAP_LIB}
    ${GRAPHICSMAGICK_LIBRARIES}
    pthread
    m
)

find_package(Curses REQUIRED)

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})

add_executable(admin ./admin.c)
target_link_libraries(admin ${CURSES_LIBRARIES})