# non_rtos -- hml linked into a "generic firmware", bare-metal shaped.
#
# This is the drop-in / add_subdirectory path. It builds hml from source inside
# this project, links hml_core + hml_protocol and a hand-written port, and does
# NOT link hml_port_posix. Prove the last part: the link line below has no
# libhml_port_posix.a and no -lpthread, and the resulting binary contains no
# socket(2)/sendto(2) relocation.
#
#   cmake -S examples/firmware_dropin/non_rtos -B /tmp/hml_dropin_nonrtos
#   cmake --build /tmp/hml_dropin_non_rtos
#   /tmp/hml_dropin_nonrtos/hml_dropin_non_rtos
#
# The same tree with the shared arm instead (the demo reports linkage=shared):
#
#   cmake -S examples/firmware_dropin/non_rtos -B /tmp/hml_dropin_sh \
#         -DHML_FIRMWARE_SHARED=ON
#   cmake --build /tmp/hml_dropin_sh
#   /tmp/hml_dropin_sh/hml_dropin_non_rtos
#   ldd /tmp/hml_dropin_sh/hml_dropin_non_rtos | grep hml
#
# On a real board, replace non_rtos_port_stub.c with your port, point
# HML_FIRMWARE_POSIX=OFF at your cross-toolchain, and add
# ${hml_firmware_contract_source} to the port's sources so a missing port symbol
# is a link error rather than a first-datagram crash.

cmake_minimum_required(VERSION 3.16)
project(hml_dropin_non_rtos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror)

# HML_NO_POSIX must reach every translation unit that includes an hml header,
# including this one, or hml_port.h would auto-tag the *host* as POSIX and the
# demo would prove nothing about a cross build.
add_compile_definitions(HML_NO_POSIX)

# The whole drop-in package: hml_protocol, hml_core, hml_control, hml_udp,
# hml_rocev2 as static archives, and no hml_port_posix.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../firmware/hml_object hml_object)

add_executable(hml_dropin_non_rtos
  non_rtos_main.c
  non_rtos_port_stub.c
  ${hml_firmware_contract_source})

# hml::hml_core is the bare-metal surface: framer, reassembly, latency and the
# transport registry. It calls hml_clock_us() and (via the transports) the
# hml_sock_* family, all of which non_rtos_port_stub.c supplies.
#
# The same demo covers both linkages: -DHML_FIRMWARE_SHARED=ON switches to
# hml::hml_core_shared, which is what makes the binary need LD_LIBRARY_PATH and
# is what HML_LINKAGE_NAME reports at compile time.
if(HML_FIRMWARE_SHARED)
  target_link_libraries(hml_dropin_non_rtos PRIVATE hml::hml_core_shared)
  # CMake gives a build-tree binary an RPATH into hml_object/, so it runs in
  # place. Once the .so files are *installed* that RPATH is dropped, and the
  # loader has to be pointed at them, which is what the staged-install and
  # LD_LIBRARY_PATH checks in docs/install.md exercise.
else()
  target_link_libraries(hml_dropin_non_rtos PRIVATE hml::hml_core)
endif()

# The transports are separate targets, linkable independently:
#   target_link_libraries(fw PRIVATE hml::hml_core hml::hml_udp)
#   target_link_libraries(fw PRIVATE hml::hml_core hml::hml_rocev2)
