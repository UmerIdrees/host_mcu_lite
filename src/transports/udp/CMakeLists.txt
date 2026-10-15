# T0 UDP datagram transport.
# hml_fnv1a32 / the synthetic pattern helpers live in hml_synth, which hml_host and
# hml_gen link for the shared hash function.
file(GLOB HML_UDP_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(hml_udp STATIC ${HML_UDP_SRCS})
target_include_directories(hml_udp PUBLIC ${CMAKE_SOURCE_DIR}/include
                                        ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_udp PUBLIC hml_core hml_protocol hml_port_posix)
