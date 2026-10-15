# M5 port tests + the M6 accel probe. Every test_*.c here becomes one binary and
# one ctest entry.
#
# test_ports_api      links all three port libraries into ONE binary on purpose:
#                     that is the only way to prove the API surface is identical
#                     across them and that the "one port API per binary" clock
#                     guard holds. hml_port_freertos and hml_port_esp contribute
#                     no public symbol on a host build (each guards its
#                     definitions with #if !defined(HML_PORT_POSIX)).
# test_core_no_ifdef  needs no library at all; it only reads the source tree, so
#                     it gets the tree root as a define instead.
# test_accel_probe    exercises the M6 layer on whatever hardware is present,
#                     which on a plain box is "none", and that is the case the
#                     plan cares about: report honestly, fall back fast.
file(GLOB HML_M5_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

set(_HML_M5_libs_test_ports_api "hml_port_posix;hml_port_freertos;hml_port_esp;hml_protocol")
set(_HML_M5_libs_test_accel_probe "hml_accel;hml_core;hml_protocol;hml_port_posix")
set(_HML_M5_libs_test_core_no_ifdef "")

foreach(_src ${HML_M5_TEST_SRCS})
  get_filename_component(_name ${_src} NAME_WE)
  add_executable(${_name} ${_src})
  target_include_directories(${_name} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/accel
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${_name} PRIVATE
    HML_M5_SRC_ROOT="${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${_name} PRIVATE ${_HML_M5_libs_${_name}})
  add_test(NAME ${_name} COMMAND ${_name})
  set_tests_properties(${_name} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endforeach()
