# e2e_cpu -- the CPU-only vendor-matrix logic, run on the build machine.
#
# One host binary, five scenes, all on 127.0.0.1 with ports from the compiled-in
# private range (tests/e2e_cpu/e2e_cpu.h). This is NOT an MCU result: it runs
# the shipping transport/control/reassembly C natively. The run_matrix.sh script
# (registered below as e2e_cpu_matrix) is the primary entry point.
#
# Link set: every library a scene touches. hml_roce_gen/hml_roce_demux for the
# T1a scene, hml_host for the UDP receivers, hml_udp/hml_synth for the real
# transports and the synthetic pattern generator, hml_control for the control
# plane, hml_core/hml_protocol/hml_port_posix for the core, Threads for the
# receiver/server pollers. Include dirs mirror the per-library private headers
# exactly as tests/m3/CMakeLists.txt does.

find_package(Threads REQUIRED)

add_executable(e2e_cpu
  e2e_cpu.c
  scene_udp.c
  scene_roce.c
  scene_reasm.c
  scene_control.c
  scene_mcu.c
  ${CMAKE_SOURCE_DIR}/renode/firmware/freertos_demo/app/hml_mcu_demo.c)

target_include_directories(e2e_cpu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/transports/rocev2
  ${CMAKE_SOURCE_DIR}/src/transports/udp
  ${CMAKE_SOURCE_DIR}/adapters/synthetic
  ${CMAKE_SOURCE_DIR}/host/roce_demux
  ${CMAKE_SOURCE_DIR}/host/roce_gen
  ${CMAKE_SOURCE_DIR}/host/hml_host
  ${CMAKE_SOURCE_DIR}/src/control
  ${CMAKE_SOURCE_DIR}/renode/firmware/freertos_demo/app)

target_link_libraries(e2e_cpu PRIVATE
  hml_roce_demux hml_roce_gen hml_rocev2 hml_host hml_udp hml_synth hml_control
  hml_core hml_protocol hml_port_posix Threads::Threads)

add_test(NAME e2e_cpu_matrix
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_matrix.sh $<TARGET_FILE:e2e_cpu>)
set_tests_properties(e2e_cpu_matrix PROPERTIES
  TIMEOUT 300
  LABELS "e2e;optional")

foreach(_scene udp-loopback roce-e2e reasm-edge control-beacon)
  add_test(NAME e2e_cpu_${_scene}
           COMMAND e2e_cpu ${_scene} --frames 80 --seconds 60)
  set_tests_properties(e2e_cpu_${_scene} PROPERTIES
    TIMEOUT 120
    LABELS "e2e")
endforeach()