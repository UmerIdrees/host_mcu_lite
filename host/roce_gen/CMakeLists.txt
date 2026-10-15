# M3 / T1a: the hml_gen_roce sender CLI, the RoCE counterpart of hml_gen.
#
# hml_gen_roce.c lives in this directory, so it is filtered back out of the
# library: a static library must not carry a main(). The library holds the
# open-with-fallback decision and the frame transmit loop so tests/m3 can drive
# exactly the code the binary runs.
file(GLOB HML_ROCE_GEN_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(FILTER HML_ROCE_GEN_SRCS EXCLUDE REGEX "/hml_gen_roce\\.c$")
add_library(hml_roce_gen STATIC ${HML_ROCE_GEN_SRCS})
target_include_directories(hml_roce_gen PUBLIC ${CMAKE_SOURCE_DIR}/include
                                             ${CMAKE_SOURCE_DIR}/src/transports/rocev2
                                             ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_roce_gen PUBLIC hml_rocev2 hml_udp hml_synth hml_core hml_protocol
                                           hml_port_posix)

add_executable(hml_gen_roce hml_gen_roce.c)
target_link_libraries(hml_gen_roce PRIVATE hml_roce_gen hml_rocev2 hml_core hml_protocol
                                            hml_port_posix)
