# M6 host-accelerate layer. Always built, with or without hardware: the
# capability probe, the backend abstraction and the CPU fallback are useful on
# any box, and tests/m5_ports/test_accel_probe.c exercises them here.
# The single backend translation unit is chosen in ibverbs/ (below) so that no
# source file in src/ needs a preprocessor conditional.
file(GLOB HML_ACCEL_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(hml_accel STATIC ${HML_ACCEL_SRCS})
target_include_directories(hml_accel PUBLIC ${CMAKE_SOURCE_DIR}/include
                                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_accel PUBLIC hml_core hml_protocol hml_port_posix)

add_subdirectory(ibverbs)

# Report the detection result once, at configure time. Kept in a cache variable
# so the integrator can read the outcome without re-running the probe.
set(HML_ACCEL_HAVE_IBVERBS ${HML_ACCEL_HAVE_IBVERBS} CACHE INTERNAL
    "1 when hml_accel was built with libibverbs (HML_HAVE_IBVERBS)")
message(STATUS "hml_accel: libibverbs backend = ${HML_ACCEL_HAVE_IBVERBS}")
