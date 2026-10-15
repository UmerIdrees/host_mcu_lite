# File-stream sensor adapter (library) + the hml_filestream tool (executable).
# The second hml_sensor_ops implementation beside hml_synth, so the demo path is
# not synthetic-only: it replays frames captured off the wire from an HMLF file,
# and it writes that same format, so a capture and a replay compose.
#
# The library target is `hml_filestream`; the CLI target has to be called
# hml_filestream_cli (CMake target names are unique) but OUTPUT_NAME makes the
# binary `hml_filestream`.
#
# The tool links hml_synth on top of the base set because hml_fnv1a32() is
# defined in adapters/synthetic and the tool hashes every frame it sends and
# receives with it, which is what makes its output diffable against hml_gen and
# hml_host. hml_synth only depends on hml_core, so this introduces no cycle.
#
# hml_filestream_cli.c lives in this directory, so it is filtered back out of
# the library: a static library must not carry a main().
file(GLOB HML_FILESTREAM_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(FILTER HML_FILESTREAM_SRCS EXCLUDE REGEX "/hml_filestream_cli\\.c$")
add_library(hml_filestream STATIC ${HML_FILESTREAM_SRCS})
target_include_directories(hml_filestream PUBLIC ${CMAKE_SOURCE_DIR}/include
                                            ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_filestream PUBLIC hml_core)

add_executable(hml_filestream_cli hml_filestream_cli.c)
set_target_properties(hml_filestream_cli PROPERTIES OUTPUT_NAME hml_filestream)
target_include_directories(hml_filestream_cli PRIVATE ${CMAKE_SOURCE_DIR}/src/transports/udp
                                                      ${CMAKE_SOURCE_DIR}/adapters/synthetic)
target_link_libraries(hml_filestream_cli PRIVATE hml_filestream hml_udp hml_synth hml_core
                                                     hml_protocol hml_port_posix)
