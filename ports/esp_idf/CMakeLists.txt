# ESP-IDF port. Always configured and built; on a host it is the portable stub
# (HML_ERR_NOTSUP from every socket call, monotonic clock). An ESP-IDF firmware
# build compiles the same .c with HML_PORT_HAVE_ESP_IDF defined and the IDF
# component requirements listed at the top of hml_port_esp.c; that build does
# not use this CMakeLists (idf.py owns the target).
file(GLOB HML_PORT_ESP_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(hml_port_esp STATIC ${HML_PORT_ESP_SRCS})
target_link_libraries(hml_port_esp PUBLIC hml_protocol hml_port_posix)
