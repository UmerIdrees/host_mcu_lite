# Path A CPU receiver library + the hml_host tool.
# NOTE: hml_host links hml_synth on top of the base hml_core/hml_protocol/hml_port_posix
# set, because hml_fnv1a32() is defined in adapters/synthetic and the receiver uses
# it to hash every reassembled frame. hml_synth only depends on hml_core, so this
# introduces no cycle.
#
# The library target is `hml_host`; the CLI target has to be called hml_host_cli
# (CMake target names are unique) but OUTPUT_NAME makes the binary `hml_host`.
#
# main.c lives in this directory, so it is filtered back out of the library sources.
file(GLOB HML_HOST_LIB_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(FILTER HML_HOST_LIB_SRCS EXCLUDE REGEX "/main\\.c$")
add_library(hml_host STATIC ${HML_HOST_LIB_SRCS})
target_include_directories(hml_host PUBLIC ${CMAKE_SOURCE_DIR}/include
                                         ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_host PUBLIC hml_core hml_protocol hml_port_posix hml_synth)

add_executable(hml_host_cli main.c)
set_target_properties(hml_host_cli PROPERTIES OUTPUT_NAME hml_host)
target_link_libraries(hml_host_cli PRIVATE hml_host hml_core hml_protocol hml_port_posix
                                         hml_synth)
