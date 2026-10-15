# Track A M0+M1 tests. Every test_*.c here becomes one test binary and one ctest entry.
# The sources are globbed so an integrator can drop a new test_foo.c in without
# touching this file; the object lists below only say which libraries it links.
file(GLOB HML_M0M1_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

find_package(Threads REQUIRED)

# name -> extra libraries. hml_core/hml_protocol come from the shared source lists.
set(_HML_M0M1_EXTRA_libs "")
set(_HML_M0M1_EXTRA_libs_test_crc "")
set(_HML_M0M1_EXTRA_libs_test_frame_codec "")
set(_HML_M0M1_EXTRA_libs_test_reasm "hml_port_posix")  # reasm now taps hml_clock_us()
set(_HML_M0M1_EXTRA_libs_test_stream "")
set(_HML_M0M1_EXTRA_libs_test_latency "hml_port_posix")
set(_HML_M0M1_EXTRA_libs_test_loopback "hml_udp;hml_synth;hml_host;hml_port_posix;Threads::Threads")

foreach(_src ${HML_M0M1_TEST_SRCS})
  get_filename_component(_name ${_src} NAME_WE)
  add_executable(${_name} ${_src})
  target_include_directories(${_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${_name} PRIVATE hml_core hml_protocol
                        ${_HML_M0M1_EXTRA_libs_${_name}})
  add_test(NAME ${_name} COMMAND ${_name})
  set_tests_properties(${_name} PROPERTIES TIMEOUT 300)
endforeach()
