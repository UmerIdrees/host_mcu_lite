# M4 bridge mode tests: the window map and the stub peripherals, the
# configure-then-stream phase machine, and the full lifecycle over the real UDP
# control plane. Every test_*.c here becomes one binary and one ctest entry.
file(GLOB HML_M4_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

find_package(Threads REQUIRED)

foreach(_src ${HML_M4_TEST_SRCS})
  get_filename_component(_name ${_src} NAME_WE)
  add_executable(${_name} ${_src})
  target_include_directories(${_name} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/bridge
    ${CMAKE_SOURCE_DIR}/src/control
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${_name} PRIVATE hml_bridge hml_control hml_protocol
                        hml_port_posix Threads::Threads)
  add_test(NAME ${_name} COMMAND ${_name})
  # The lifecycle test binds the loopback control ports, so it must not run
  # beside another test that wants the same ones.
  set_tests_properties(${_name} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()
