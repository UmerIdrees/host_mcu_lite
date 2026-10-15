# Renode-based Cortex-M smoke harness.
#
# SCRIPTS AND ONE HOST SELF-TEST. This directory builds no MCU target into the
# host build: the Cortex-M image is cross-compiled on demand by
# scripts/build_mcu.sh, which needs arm-none-eabi-gcc, and run only on demand by
# scripts/run_smoke.sh, which needs Renode. Neither is a build dependency of
# this project, so a machine with no emulator and no cross compiler must still
# configure, build and test clean.
#
# Tests registered here (all three are CTest-registered, and the ordering below
# is the honesty ordering):
#
#   renode_smoke                 the real thing. Builds the Cortex-M image and
#                                runs renode/holoscan_mcu_lite.robot, which
#                                boots an emulated Cortex-M4 and asserts on the
#                                firmware's UART output. SKIP_RETURN_CODE 77,
#                                so a box without Renode or without the ARM
#                                toolchain reports ***Skipped***, never a
#                                failure. It also never reports a pass it did
#                                not observe: if the tooling is present and the
#                                run fails, the exit code is non-zero and CTest
#                                reports Failed.
#
#   renode_selfcheck             always runs, always expected to PASS. Proves
#                                the skip path is intact (both scripts exit 77
#                                when forced to skip, with no side effects), the
#                                scripts are valid bash, and the firmware's UART
#                                markers still match the robot test. A broken
#                                skip path is a bug, so this one fails loudly.
#
#   renode_demo_native_selftest  runs the demo body (app/hml_mcu_demo.c) on the
#                                HOST. Read the header of
#                                firmware/freertos_demo/platform/main_host_selftest.c
#                                before quoting it: no Cortex-M instruction
#                                executes, no RTOS executes, Renode is not
#                                involved. Its job is to keep that file
#                                compiling under -Werror and its marker strings
#                                in sync with the robot test, on a machine that
#                                has neither Renode nor a cross compiler. It is
#                                NOT evidence about any microcontroller and must
#                                never be reported as such.
#
# What a green renode_smoke does and does not establish, in one line: it runs
# this repository's codec/CRC/reassembly C on an emulated Cortex-M with a
# cooperative stub RTOS and no network stack. It is not a FreeRTOS validation,
# not a lwIP validation, and not a hardware result. docs/renode.md has the long
# version.

if(NOT HML_BUILD_TESTS)
  return()
endif()

# The top-level CMakeLists.txt calls enable_testing() before it adds this
# subdirectory, so add_test() below would be a no-op without a local call.
# enable_testing() is idempotent and directory-scoped: it only generates this
# directory's CTestTestfile.cmake, and the root one already carries
# subdirs("renode") because of the guarded add_subdirectory().
enable_testing()

# ---- the emulator smoke timeout ---------------------------------------------
# A Renode smoke is not a unit test: it cross-compiles an image, boots it, and
# then waits for a demo that does CRC, beacon, control, frame-codec and
# reassembly work. On a loaded or virtualised host the emulation runs well below
# real time -- the observed stm32f4 run needed roughly 3000 ms of guest time and
# about 20 minutes of wall clock, i.e. roughly 400x slower than real time.
#
# 420s was therefore not a generous bound but a wrong one: it killed Renode
# mid-run and reported a red smoke for an image that goes on to print
# result=PASS. A timeout here is indistinguishable from a hang, and the fix is
# not to shorten the test but to give it enough room to finish and answer.
#
# Override at configure time when a host is slower still:
#   cmake -DHML_RENODE_SMOKE_TIMEOUT=3600 ..
set(HML_RENODE_SMOKE_TIMEOUT 2400 CACHE STRING
    "Seconds CTest allows a Renode smoke test to run before killing it")
message(STATUS "Renode smoke test timeout: ${HML_RENODE_SMOKE_TIMEOUT}s")

# ---- the real smoke test ----------------------------------------------------
# run_smoke.sh cross-compiles the image and drives renode-test. It is
# self-contained: it locates its own scripts, so no RENODE_DIR or build-tree
# knowledge leaks in here.
add_test(NAME renode_smoke
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_smoke.sh)
set_tests_properties(renode_smoke PROPERTIES
  # CTest's convention: 77 means "skipped". A missing emulator is not a defect.
  SKIP_RETURN_CODE 77
  # Bounded twice over: the script's own `timeout` and this, with CTest holding
  # the outer and slightly larger bound so the script reports its own diagnosis
  # before CTest resorts to killing Renode.
  TIMEOUT ${HML_RENODE_SMOKE_TIMEOUT}
  LABELS "renode;optional")

# ---- the fail-closed self-check ---------------------------------------------
# HML_MCU_SMOKE_SKIP forces the scripts' skip path, so this test behaves
# identically on a box with Renode and on a box without it: it can never
# accidentally become a second, slow smoke run.
add_test(NAME renode_selfcheck
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/selfcheck.sh)
set_tests_properties(renode_selfcheck PROPERTIES
  ENVIRONMENT "HML_MCU_SKIP=1;HML_MCU_SMOKE_SKIP=1"
  TIMEOUT 120
  LABELS "renode")

# ---- the host self-test of the demo body -----------------------------------
# NOT an MCU test. See the comment at the top of this file.
add_executable(hml_mcu_demo_native
  firmware/freertos_demo/app/hml_mcu_demo.c
  firmware/freertos_demo/platform/main_host_selftest.c)
target_include_directories(hml_mcu_demo_native PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/firmware/freertos_demo/app)
# hml_core brings the reassembler, hml_control the beacon and control codecs,
# hml_protocol the CRCs and the frame codec. Exactly the set the Cortex-M image
# links, so this target cannot drift away from what the firmware compiles.
target_link_libraries(hml_mcu_demo_native PRIVATE hml_core hml_control hml_protocol)

add_test(NAME renode_demo_native_selftest COMMAND hml_mcu_demo_native)
set_tests_properties(renode_demo_native_selftest PROPERTIES
  TIMEOUT 60
  LABELS "renode;not-mcu")

# ---- per-vendor tests (five boards, three tests each) ----------------------
# Every vendor row in boards/index registers the same three tests. The honesty
# ordering of each triple is fixed: the native self-test always runs and only
# ever talks about the HOST build; the selfcheck always runs and only checks
# text-level consistency; the smoke test runs for real ONLY where Renode and the
# matching cross toolchain exist, and otherwise skips (exit 77 -> Skipped).
# See the comment block at the top of this file and docs/renode_multi_vendor.md.
set(HML_VENDORS
  stm32f4
  nxp_lpc
  ti_tm4c
  microchip_sam
  riscv)

foreach(v ${HML_VENDORS})
  # <v>_renode_smoke -- the real thing for this board, when tooling exists.
  add_test(NAME ${v}_renode_smoke
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_vendor_smoke.sh ${v})
  set_tests_properties(${v}_renode_smoke PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT ${HML_RENODE_SMOKE_TIMEOUT}
    LABELS "renode;optional;vendor")

  # <v>_renode_selfcheck -- always runs, always expected to PASS. Proves this
  # vendor's index row, vendor.env, board_config.h, linker script, .resc and
  # .robot still agree, and that the negative test catches a corrupted row.
  # Never evidence about an emulated boot.
  add_test(NAME ${v}_renode_selfcheck
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/selfcheck_vendors.sh
                   --vendor ${v} --no-negative)
  set_tests_properties(${v}_renode_selfcheck PROPERTIES
    TIMEOUT 120
    LABELS "renode;vendor")

  # <v>_renode_native_selftest -- HOST build of this vendor's board_config.h and
  # the shared demo body at this vendor's frame cap. NOT an MCU test: read the
  # header of renode/vendors/host_vendor_selftest.c before quoting it. Its job
  # is to keep every board_config.h compiling under -Werror and to prove the
  # frame cap the index declares is one the library's reassembly checks accept.
  add_executable(${v}_vendor_native
    vendors/host_vendor_selftest.c
    firmware/freertos_demo/app/hml_mcu_demo.c)
  target_include_directories(${v}_vendor_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendors/${v}/firmware/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/freertos_demo/app)
  target_link_libraries(${v}_vendor_native PRIVATE hml_core hml_control hml_protocol)

  add_test(NAME ${v}_renode_native_selftest COMMAND ${v}_vendor_native)
  set_tests_properties(${v}_renode_native_selftest PROPERTIES
    TIMEOUT 60
    LABELS "renode;not-mcu;vendor")
endforeach()
