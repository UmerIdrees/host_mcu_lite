# FreeRTOS + lwIP port. This target is ALWAYS configured and built, even on a
# plain Linux host, because hml_port_freertos.c compiles to a portable stub
# there: the socket API answers HML_ERR_NOTSUP and the clock stays monotonic.
# That is what lets tests/m5_ports/test_ports_api.c link every port into one
# binary and assert the API surface. The real lwIP/FreeRTOS path is behind
# HML_PORT_HAVE_FREERTOS, which a cross build defines for this target only.
file(GLOB HML_PORT_FREERTOS_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(hml_port_freertos STATIC ${HML_PORT_FREERTOS_SRCS})
target_link_libraries(hml_port_freertos PUBLIC hml_protocol hml_port_posix)
