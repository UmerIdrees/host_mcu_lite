# M3 / T1a tests: RoCEv2 golden wire vectors, ICRC acceptance/rejection, and the
# in-process transmit/receive end-to-end run, plus the hml_gen_roce CLI driver and
# the T1a -> T0 fallback decision.
find_package(Threads REQUIRED)

file(GLOB HML_M3_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

# test_roce_cli drives two separate binaries and test_roce_fallback links the
# host/roce_gen helper and the host receiver library, so both need their own
# target setup below instead of the common in-process one.
list(FILTER HML_M3_TEST_SRCS EXCLUDE REGEX "/(test_roce_cli|test_roce_fallback)\\.c$")

foreach(_t ${HML_M3_TEST_SRCS})
  get_filename_component(_name ${_t} NAME_WE)
  add_executable(${_name} ${_t})
  target_include_directories(${_name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/transports/rocev2
    ${CMAKE_SOURCE_DIR}/host/roce_demux
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${_name} PRIVATE
    hml_roce_demux hml_rocev2 hml_core hml_protocol hml_port_posix Threads::Threads)
  add_test(NAME ${_name} COMMAND ${_name})
  set_tests_properties(${_name} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()

# ---- hml_gen_roce: the two-process CLI driver --------------------------------
# This one is deliberately not an in-process test: it execs the real binaries and
# asserts on what they printed, so the CLI's flags, output format and exit status
# are all covered. RUN_SERIAL because it binds fixed UDP ports.
add_executable(test_roce_cli test_roce_cli.c)
target_include_directories(test_roce_cli PRIVATE
  ${CMAKE_SOURCE_DIR}/src/transports/rocev2
  ${CMAKE_SOURCE_DIR}/host/roce_demux
  ${CMAKE_SOURCE_DIR}/host/roce_gen
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_roce_cli PRIVATE
  hml_roce_demux hml_rocev2 hml_core hml_protocol hml_port_posix Threads::Threads)
add_dependencies(test_roce_cli hml_gen_roce hml_host_roce hml_host_cli)

# argv[2] is the T1a receiver (hml_host_roce); the ctest entries below also reuse
# this binary against the plain T0 receiver via the 4th argument.
add_test(NAME test_roce_cli
  COMMAND test_roce_cli $<TARGET_FILE:hml_gen_roce> $<TARGET_FILE:hml_host_roce>)
set_tests_properties(test_roce_cli PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

add_test(NAME test_roce_cli_fallback
  COMMAND test_roce_cli $<TARGET_FILE:hml_gen_roce> $<TARGET_FILE:hml_host_cli>
          14893 fallback)
set_tests_properties(test_roce_cli_fallback PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

# ---- T1a -> T0 fallback decision (in process, no sockets beyond loopback) -----
add_executable(test_roce_fallback test_roce_fallback.c)
target_include_directories(test_roce_fallback PRIVATE
  ${CMAKE_SOURCE_DIR}/src/transports/rocev2
  ${CMAKE_SOURCE_DIR}/src/transports/udp
  ${CMAKE_SOURCE_DIR}/adapters/synthetic
  ${CMAKE_SOURCE_DIR}/host/roce_gen
  ${CMAKE_SOURCE_DIR}/host/hml_host
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_roce_fallback PRIVATE
  hml_roce_gen hml_host hml_rocev2 hml_udp hml_synth
  hml_core hml_protocol hml_port_posix Threads::Threads)
add_test(NAME test_roce_fallback COMMAND test_roce_fallback)
set_tests_properties(test_roce_fallback PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
