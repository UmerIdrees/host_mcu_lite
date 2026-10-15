# Track D: 6-Way Verification Matrix Test
add_test(
  NAME test_6way_matrix
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_6way_matrix.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_6way_matrix PROPERTIES LABELS "matrix;e2e;holoscan")

# Renode MCU to NVIDIA Holoscan Application Graph End-to-End Test
add_test(
  NAME test_renode_holoscan_e2e
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_renode_holoscan_e2e.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_renode_holoscan_e2e PROPERTIES
  SKIP_RETURN_CODE 77
  TIMEOUT 600
  LABELS "renode;holoscan;e2e;matrix"
)
