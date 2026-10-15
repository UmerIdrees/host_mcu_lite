# Track B: Server RDMA & Soft-RoCE Reference Engine (Verbs Receiver).

include(CheckIncludeFile)

check_include_file("infiniband/verbs.h" HML_HAVE_VERBS_H)
find_library(IBVERBS_LIB NAMES ibverbs libibverbs)

if(HML_HAVE_VERBS_H AND IBVERBS_LIB)
  message(STATUS "host/verbs_rx: libibverbs found (${IBVERBS_LIB})")
  set(HML_ENABLE_IBVERBS 1)
else()
  message(STATUS "host/verbs_rx: libibverbs headers/library not found; compiling with software emulation and fallback")
  set(HML_ENABLE_IBVERBS 0)
endif()

add_library(hml_verbs_rx STATIC
  hml_verbs_rx.c
)

target_include_directories(hml_verbs_rx PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(hml_verbs_rx PUBLIC
  HML_HAVE_IBVERBS=${HML_ENABLE_IBVERBS}
)

target_link_libraries(hml_verbs_rx PUBLIC
  hml_core
  hml_protocol
  hml_port_posix
)

if(HML_ENABLE_IBVERBS)
  target_link_libraries(hml_verbs_rx PUBLIC ${IBVERBS_LIB})
endif()

# Demonstration and validation test binary
add_executable(hml_verbs_rx_demo
  hml_verbs_rx_demo.c
)

target_include_directories(hml_verbs_rx_demo PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hml_verbs_rx_demo PRIVATE
  hml_verbs_rx
  hml_control
  hml_protocol
  hml_core
  hml_port_posix
  pthread
)

if(HML_BUILD_TESTS)
  add_test(NAME test_verbs_rx COMMAND hml_verbs_rx_demo)
endif()
