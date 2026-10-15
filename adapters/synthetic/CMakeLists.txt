# Synthetic sensor adapter (library) + the hml_gen tool (executable).
# hml_fnv1a32 (shared with the host receiver) is defined in hml_synthetic.c.
#
# hml_gen.c is globbed in as well (it lives in this directory), so it is filtered back
# out of the library: a static library must not carry a main().
file(GLOB HML_SYNTH_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(FILTER HML_SYNTH_SRCS EXCLUDE REGEX "/hml_gen\\.c$")
add_library(hml_synth STATIC ${HML_SYNTH_SRCS})
target_include_directories(hml_synth PUBLIC ${CMAKE_SOURCE_DIR}/include
                                         ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_synth PUBLIC hml_core)

add_executable(hml_gen hml_gen.c)
# hml_gen reaches both tiers from one CLI: --xport udp is T0 and --xport rocev2 is
# T1a, so the tool links hml_rocev2 and includes its private header to register
# the transport and fill the RDMA window fields. The BTH/RETH/ICRC bytes
# themselves are still built only in src/transports/rocev2.
target_include_directories(hml_gen PRIVATE ${CMAKE_SOURCE_DIR}/src/transports/udp
                                            ${CMAKE_SOURCE_DIR}/src/transports/rocev2)
target_link_libraries(hml_gen PRIVATE hml_synth hml_udp hml_rocev2 hml_core hml_port_posix)
