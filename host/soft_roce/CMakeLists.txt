# M3b -- optional Soft-RoCE (rdma_rxe) verbs lab.
#
# SCRIPTS AND DOCUMENTATION ONLY. This directory builds no C target and links
# no libibverbs: the lab is shell scripts under host/soft_roce/ plus the note
# in docs/soft_roce_lab.md. The library itself must keep building and testing
# green on a machine with no RDMA hardware, no root and no rdma_rxe module,
# so nothing here is part of a default build.
#
# Tests registered here:
#
#   roce_lab_skip_path  always registered, always expected to PASS. It runs
#                       skip_selfcheck.sh, which invokes every lab script with
#                       HML_RXE_SKIP=1 and requires exit 77 (CTest "skipped").
#                       This works everywhere: no root, no RDMA, no network.
#
#   roce_lab            registered ONLY when the user opts in, because it
#                       creates network namespaces and RDMA devices:
#                         cmake -S . -B build -DHML_ENABLE_ROCE_LAB=ON
#                       or  HML_ENABLE_ROCE_LAB=1 cmake -S . -B build
#                       It exits 77 (SKIP_RETURN_CODE) when the environment
#                       cannot run the lab, so it can never fail CI by
#                       accident. Set HML_LAB_KEEP=1 to inspect the lab after
#                       the run; it is torn down via an EXIT trap otherwise.

if(NOT HML_BUILD_TESTS)
  return()
endif()

# The top-level CMakeLists.txt calls enable_testing() *after* it adds this
# subdirectory, so add_test() below would be a no-op without a local call.
# enable_testing() is idempotent and directory-scoped: it only generates this
# directory's CTestTestfile.cmake, and the root CTestTestfile.cmake already
# carries subdirs("host/soft_roce") because of the guarded add_subdirectory().
enable_testing()

# ---- always on: the fail-closed self-check --------------------------------
add_test(NAME roce_lab_skip_path
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/skip_selfcheck.sh)
set_tests_properties(roce_lab_skip_path PROPERTIES
  ENVIRONMENT "HML_RXE_SKIP=1"
  TIMEOUT 120)

# ---- always on: the mock/simulated pipeline verification -------------------
add_test(NAME roce_lab_mock
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_lab.sh --mock)
set_tests_properties(roce_lab_mock PROPERTIES
  ENVIRONMENT "HML_LAB_MOCK=1"
  TIMEOUT 120)

# ---- opt-in only: the real lab --------------------------------------------
if(DEFINED ENV{HML_ENABLE_ROCE_LAB} OR HML_ENABLE_ROCE_LAB)
  add_test(NAME roce_lab
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_lab.sh)
  set_tests_properties(roce_lab PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "HML_ENABLE_ROCE_LAB=1"
    TIMEOUT 600)
endif()
