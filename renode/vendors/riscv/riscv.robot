*** settings ***
# riscv.robot -- the RISC-V RV32IMAC smoke test under Renode.
#
# WHAT THIS TEST PROVES, PRECISELY
#   IF IT EVER PASSES: the riscv demo image booted on an emulated RV32IMAC core
#   with the memory map in riscv.resc, ran the shipping holoscan_mcu_lite
#   codec, CRC and reassembly code, and printed a DONE line with result=PASS.
#   That would be the entire claim.
#
# WHAT IT DOES NOT PROVE -- do not quote this test for any of these
#   * Not FreeRTOS. The image's task API is a single-task cooperative stub
#     (renode/firmware/freertos_demo/stub_freertos/FreeRTOS.h says so, at the
#     top, in capitals). Nothing here exercises preemption, priorities,
#     context switches, the tick interrupt, a port layer or a real-time kernel.
#   * Not lwIP and not networking. No datagram is sent or received. There is no
#     network peripheral in the platform description, so a datagram could not
#     be sent even in principle. The beacon and control codecs are exercised
#     by handing bytes to the library parsers inside the image.
#   * Not a sensor, a DMA engine, a cache, an MPU, a clock tree or an
#     interrupt controller. None exist in the platform: no NVIC and no PLIC
#     are wired, no interrupt is ever taken, reset is firmware text (mtvec),
#     and the image's tick is the machine timer inside the CLINT, not an ARM
#     SysTick. The uart-irq of 0 the image prints is fictional, on purpose.
#   * Not silicon. An emulator is a model. A green run is evidence about this C
#     code and about the emulator's fidelity, never about a GigaDevice part.
#   * Not a performance, latency or power measurement. Nothing is timed.
#
# STATUS
#   HAS NEVER BEEN EXECUTED. The machine this was written on has no Renode, no
#   mono, no dotnet and no riscv32 toolchain, so this file has never been run
#   and its correctness is unverified. The CTest wrapper therefore exits 77
#   (skipped) when the tooling is absent, and when the tooling IS present a
#   failure here is a failure, not a skip. Do not let a skip read as a pass.
#
# THE MARKERS, ALL DEFINED IN renode/firmware/freertos_demo/app/hml_mcu_demo.h
#   HML-MCU-DEMO-READY  printed once the banner and the non-claims are out.
#   HML-MCU-DEMO-PHASE  one per check group, all six asserted below.
#   HML-MCU-DEMO-TASK   the task's own rc, asserted below.
#   HML-MCU-DEMO-DONE   the verdict line, result=PASS, asserted below.
#   HML-MCU-DEMO-FAIL   one per failed check. The DONE result=PASS assertion
#                       below is what makes it fatal: a FAIL can only coexist
#                       with PASS if the image lies about its own tally, which
#                       the result=PASS-failures=0-checks=[0-9]+ pattern is there to catch.
#   HML-MCU-DEMO-NOTE   the non-claims, asserted below.
#   HML-MCU-DEMO-WARN   a non-fatal problem. Not asserted: a WARN is allowed on
#                       a part too small for the default 1 MiB frame cap, and it
#                       always shows up in the DONE tally as a failure.
#   selfcheck_vendors.sh asserts that every one of these strings is defined in
#   hml_mcu_demo.h AND named in this file, so the two cannot drift apart.

*** variables ***
# Overridden by renode/scripts/run_vendor_smoke.sh with
#   --variable ELF:<abs path>  --variable MACHINE:riscv
# The defaults are the same relative ELF and the same .resc this file belongs to,
# so running it by hand from a directory that holds the image does something
# useful.
${ELF}      riscv_hml_mcu_demo.elf
${MACHINE}  riscv
${UART}     sysbus.uart

*** keywords ***
Prepare Hml Mcu
    Execute Command    $elf=@${ELF}
    Execute Script    ${CURDIR}/${MACHINE}.resc
    Create Terminal Tester    ${UART}

*** Test Cases ***
Emulated RISC-V RV32IMAC Runs The hml Codec And Reassembly Checks
    Prepare Hml Mcu
    Start Emulation
    # THE ASSERTION ORDER IS WIRE ORDER, and it has to be. This board's
    # identity lines are printed by main() before it calls the shared demo
    # body, so they reach the UART before the banner does. Renode's terminal
    # tester consumes lines as it matches them, so a file that asserted the
    # banner first and the vendor lines second would wait for a line that had
    # already gone past.
    #
    # This board's own identity line, from firmware/platform/
    # startup_riscv.c. It is what proves the image that booted was built for
    # THIS vendor and not the base platform's.
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE vendor=riscv arch=rv32imac.*    timeout=30    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE image=riscv_hml_mcu_demo.elf.*    timeout=30    treatAsRegex=true
    # The shared banner and the non-claims. Asserting these means a run that
    # stopped early, or an image that is not the one this repository builds,
    # cannot pass.
    Wait For Line On Uart    .*HML-MCU-DEMO.*holoscan_mcu_lite.*    timeout=30    treatAsRegex=true
    Wait For Line On Uart    .*not a FreeRTOS port.*    timeout=30    treatAsRegex=true
    Wait For Line On Uart    .*no datagram leaves this image.*    timeout=30    treatAsRegex=true
    # The boot banner is complete: the task is alive and past its own preamble.
    Wait For Line On Uart    .*HML-MCU-DEMO-READY banner complete.*    timeout=30    treatAsRegex=true
    # Every phase must have been reached before the verdict is accepted, so a
    # truncated run cannot report PASS.
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE crc.*    timeout=60    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE beacon.*    timeout=60    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE control.*    timeout=60    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE frame-codec.*    timeout=60    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE reasm.*    timeout=60    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE reasm-cap.*    timeout=60    treatAsRegex=true
    # The verdict. DONE result=PASS is the demo's own final line; the startup
    # prints TASK rc=0 only AFTER the demo returns, so DONE arrives first on the
    # wire. Waiting for rc=0 first would consume the DONE line before it is read.
    Wait For Line On Uart    .*HML-MCU-DEMO-DONE result=PASS failures=0 checks=[0-9]+.*    timeout=1300    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-TASK rc=0.*    timeout=1300    treatAsRegex=true

# No Test Teardown: renode-test owns the emulator lifecycle and quits it when
# the file finishes. run_vendor_smoke.sh additionally wraps the whole run in
# `timeout`, so even a wedged emulator cannot hang the test suite.
