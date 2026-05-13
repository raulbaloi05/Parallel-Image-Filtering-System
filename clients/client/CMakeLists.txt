find_library(GSOAP_LIB gsoap REQUIRED)
find_path(GSOAP_INCLUDE stdsoap2.h REQUIRED)

find_library(LIBCONFIG_LIB config REQUIRED)
find_path(LIBCONFIG_INCLUDE libconfig.h REQUIRED)

add_executable(pif-client
    main.c
    client.c
    config.c
    soapClient.c
    soapC.c
)

target_include_directories(pif-client PRIVATE
    ${GSOAP_INCLUDE}
    ${LIBCONFIG_INCLUDE}
)

target_link_libraries(pif-client PRIVATE
    ${GSOAP_LIB}
    ${LIBCONFIG_LIB}
)
