find_package(Threads REQUIRED)

file(GLOB M2_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
foreach(_src ${M2_TEST_SRCS})
  get_filename_component(_name ${_src} NAME_WE)
  add_executable(${_name} ${_src})
  target_include_directories(${_name} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/control)
  target_link_libraries(${_name} PRIVATE hml_control Threads::Threads)
  add_test(NAME ${_name} COMMAND ${_name})
  set_tests_properties(${_name} PROPERTIES TIMEOUT 60)
endforeach()
