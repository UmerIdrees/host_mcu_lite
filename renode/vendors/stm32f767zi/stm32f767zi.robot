*** settings ***
# stm32f767zi.robot -- the STM32F767ZI (Cortex-M7) smoke test under Renode.
#
# WHAT THIS TEST PROVES, PRECISELY
#   IF IT EVER PASSES: the stm32f767zi demo image booted on an emulated
#   Cortex-M7 with the memory map in stm32f767zi.resc, brought up the FPU, left
#   the caches off on purpose, ran the shipping holoscan_mcu_lite codec, CRC and
#   reassembly code, and printed a DONE line with result=PASS. That is the
#   entire claim.
#
# WHAT IT DOES NOT PROVE -- do not quote this test for any of these
#   * NOT NETWORKING. This is the important one, because this is the only vendor
#     row whose platform HAS a modelled Ethernet MAC. Having the MAC present is
#     not the same as using it: this image drives the console UART and nothing
#     else. No frame is transmitted, nothing is received, and the shared demo
#     body prints "no datagram leaves this image" to say so. The network
#     dataplane is covered by a SEPARATE example and robot file, not by this
#     one; see examples/firmware_dropin and its README.
#   * Not FreeRTOS. The image's task API is a single-task cooperative stub
#     (renode/firmware/freertos_demo/stub_freertos/FreeRTOS.h says so, at the
#     top, in capitals).
#   * Not a sensor, a clock tree or an interrupt. None are present, none are
#     enabled, and no interrupt is ever taken: USART3_IRQn 39 and ETH_IRQn 61
#     are wired in the .resc for completeness and are never delivered.
#   * Not DMA coherency. cache_init() in startup_stm32f767zi.c leaves BOTH the M7
#     I-cache and D-cache OFF, and it prints that it does. That is a measured
#     choice, documented at length in cache_init: enabling the D-cache silently
#     strands peripheral writes, and enabling the I-cache wedges the image
#     outright. Cache-off means a plain volatile store reaches the MAC, so this
#     run says nothing about the clean-and-invalidate discipline a real MAC
#     descriptor ring needs on silicon, and nothing here would notice.
#   * Not silicon. An emulator is a model. A green run is evidence about this C
#     code and about the emulator's fidelity, never about an STM32F767.
#   * Not a performance, latency or power measurement. Nothing is timed.
#
# STATUS
#   This board is NEW. Whether any line below has been observed is a question for
#   a run, not for this comment: when the tooling is absent the CTest wrapper
#   exits 77 (skipped) and a skip is not a pass, and when the tooling is present a
#   failure here is a failure, never a skip.
#
# THE MARKERS, ALL DEFINED IN renode/firmware/freertos_demo/app/hml_mcu_demo.h
#   HML-MCU-DEMO-READY  printed once the banner and the non-claims are out.
#   HML-MCU-DEMO-PHASE  one per check group, all six asserted below.
#   HML-MCU-DEMO-TASK   the task's own rc, asserted below.
#   HML-MCU-DEMO-DONE   the verdict line, result=PASS, asserted below.
#   HML-MCU-DEMO-FAIL   one per failed check. The DONE result=PASS assertion
#                       below is what makes it fatal.
#   HML-MCU-DEMO-NOTE   the non-claims and this board's own identity lines.
#   HML-MCU-DEMO-WARN   a non-fatal problem. Not asserted: a WARN is allowed on
#                       a part too small for the default 1 MiB frame cap, and it
#                       always shows up in the DONE tally as a failure.

*** variables ***
# Overridden by renode/scripts/run_vendor_smoke.sh with
#   --variable ELF:<abs path>  --variable MACHINE:stm32f767zi
${ELF}      stm32f767zi_hml_mcu_demo.elf
${MACHINE}  stm32f767zi
${UART}     sysbus.uart

*** keywords ***
Prepare Hml Mcu
    Execute Command    $elf=@${ELF}
    Execute Script    ${CURDIR}/${MACHINE}.resc
    Create Terminal Tester    ${UART}

*** Test Cases ***
Emulated STM32F767ZI Cortex-M7 Runs The hml Codec And Reassembly Checks
    Prepare Hml Mcu
    Start Emulation
    # THE ASSERTION ORDER IS WIRE ORDER, and it has to be. This board's identity
    # lines are printed by main() before it calls the shared demo body, so they
    # reach the UART before the banner does. Renode's terminal tester consumes
    # lines as it matches them, so a file that asserted the banner first and the
    # vendor lines second would wait for a line that had already gone past.
    #
    # vendor= and arch= come from firmware/platform/startup_stm32f767zi.c, and
    # are what prove the image that booted was built for THIS vendor, on a
    # Cortex-M7, and not the base platform's image. The arch line is the
    # regression guard for this row specifically: a -mcpu=cortex-m4 build linked
    # against this platform would still run the codec and would otherwise be
    # indistinguishable here.
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE vendor=stm32f767zi arch=cortex-m7.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE image=stm32f767zi_hml_mcu_demo.elf.*    timeout=120    treatAsRegex=true
    # WIRE ORDER, AND THIS BLOCK IS ORDER-SENSITIVE IN A BRUTAL WAY.
    #
    # Renode's terminal tester consumes the UART as it matches, so a line that
    # arrives while the test is waiting for something else is DISCARDED, not
    # buffered. Therefore every line asserted below must be asserted in exactly
    # the order the image prints it, and a line asserted out of order is a line
    # that can never match again: the wait then burns its whole timeout and the
    # run still times out later on a line that HAD printed correctly.
    #
    # That is not hypothetical. This file used to assert the memory map before
    # heap-bytes and frame-bytes-default, but the image prints heap-bytes and
    # frame-bytes-default FIRST (see print_board_identity in
    # firmware/platform/startup_stm32f767zi.c). Every later wait therefore timed
    # out and the whole suite burned ~5360 s of waits to report a problem that
    # was a one-line reordering. The measured wire order of the asserted lines is
    # vendor, image, heap-bytes, frame-bytes-default, sram-base, sram-bytes,
    # flash-bytes, uart-irq, eth-base, eth-irq, banner, not-FreeRTOS, no-datagram,
    # READY -- and the waits below follow it. If you add a NOTE to the image,
    # add it HERE in the position it prints, or this test will hang, not fail.
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE heap-bytes=141312.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE frame-bytes-default=65536.*    timeout=120    treatAsRegex=true
    # The board's own memory map, as the image actually got it. Asserting these
    # means a link that silently landed somewhere else cannot pass.
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE sram-base=20000000.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE sram-bytes=524288.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE flash-bytes=2097152.*    timeout=120    treatAsRegex=true
    # The console and the MAC addresses this row pins, so a .resc and a
    # board_config.h that drifted apart cannot both be believed.
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE uart-irq=39.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE eth-base=40028000.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-NOTE eth-irq=61.*    timeout=120    treatAsRegex=true
    # The shared banner and the non-claims. The "no datagram" line is asserted
    # DELIBERATELY and is the load-bearing one for this board: it is the shared
    # body's own statement that this image does not use the MAC its platform
    # carries. If a future change makes this image transmit, that assertion is
    # what fails first, and it should fail until the network example's own robot
    # test takes over.
    Wait For Line On Uart    .*HML-MCU-DEMO.*holoscan_mcu_lite.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*not a FreeRTOS port.*    timeout=120    treatAsRegex=true
    Wait For Line On Uart    .*no datagram leaves this image.*    timeout=120    treatAsRegex=true
    # The boot banner is complete: the task is alive and past its own preamble.
    Wait For Line On Uart    .*HML-MCU-DEMO-READY banner complete.*    timeout=120    treatAsRegex=true
    # Every phase must have been reached before the verdict is accepted, so a
    # truncated run cannot report PASS.
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE crc.*    timeout=300    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE beacon.*    timeout=300    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE control.*    timeout=300    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE frame-codec.*    timeout=300    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE reasm.*    timeout=300    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-PHASE reasm-cap.*    timeout=300    treatAsRegex=true
    # The verdict. DONE result=PASS is the demo's own final line; the startup
    # prints TASK rc=0 only AFTER the demo returns, so DONE arrives first on the
    # wire. Waiting for rc=0 first would consume the DONE line before it is read.
    Wait For Line On Uart    .*HML-MCU-DEMO-DONE result=PASS failures=0 checks=[0-9]+.*    timeout=1300    treatAsRegex=true
    Wait For Line On Uart    .*HML-MCU-DEMO-TASK rc=0.*    timeout=1300    treatAsRegex=true

# No Test Teardown: renode-test owns the emulator lifecycle and quits it when
# the file finishes. run_vendor_smoke.sh additionally wraps the whole run in
# `timeout`, so even a wedged emulator cannot hang the test suite.
