# hml_object -- "drop your own firmware here".
#
# This is the add_subdirectory() packaging path for firmware. It builds the hml
# components from source as part of YOUR project, with no install step, no
# find_package, no tests, no host tools, and -- unless you ask -- no POSIX
# socket code anywhere in the link.
#
# From your firmware's CMakeLists.txt:
#
#     add_subdirectory(third_party/hml/firmware/hml_object)
#     add_executable(my_firmware src/main.c src/port_my_board.c)
#     target_link_libraries(my_firmware PRIVATE my_port hml_core)
#
#     # or, if you want the port contract enforced at link time:
#     add_library(my_port_check STATIC src/port_my_board.c
#                               ${hml_firmware_contract_source})
#     target_link_libraries(my_firmware PRIVATE my_port hml_core)
#     target_link_libraries(my_port_check PRIVATE hml::hml_core)
#
# Targets provided (see cmake/hml_firmware.cmake for the options):
#
#     hml_protocol   codecs + CRCs                    no platform symbols
#     hml_core       framer, reassembly, latency      needs YOUR port
#     hml_control    beacon + control codec/server    needs YOUR port
#     hml_udp        T0 UDP data plane                needs YOUR port
#     hml_rocev2     experimental RoCEv2              needs YOUR port
#     hml_<c>_shared the same components as .so       (COMPONENTS/SHARED)
#
#     hml::hml_core, hml::hml_protocol, ...           namespaced aliases
#     hml::hml_core_shared, ...                       (SHARED)
#
# Options (cache, or -D on the command line):
#     HML_FIRMWARE_COMPONENTS  semicolon list; default all five
#     HML_FIRMWARE_SHARED      ON to also build the _shared arm
#     HML_FIRMWARE_POSIX       ON to build ports/posix and keep the POSIX tag
#
# There is deliberately no `install()` here. If you want an installed hml, use
# the top-level project instead: docs/install.md.

cmake_minimum_required(VERSION 3.16)

get_filename_component(HML_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include("${HML_ROOT}/cmake/hml_firmware.cmake")

# Defaults, so the -D spellings below always pass an explicit value to
# hml_firmware_add() (a bare `SHARED` would otherwise mean ON).
if(NOT DEFINED HML_FIRMWARE_COMPONENTS)
  set(HML_FIRMWARE_COMPONENTS "protocol;core;control;udp;rocev2")
endif()
if(NOT DEFINED HML_FIRMWARE_SHARED)
  set(HML_FIRMWARE_SHARED OFF)
endif()
if(NOT DEFINED HML_FIRMWARE_POSIX)
  set(HML_FIRMWARE_POSIX OFF)
endif()

# If this is being added on top of a full hml build, the component targets
# already exist and hml_firmware_add() will reuse them unchanged.
hml_firmware_add(
  ROOT "${HML_ROOT}"
  COMPONENTS "${HML_FIRMWARE_COMPONENTS}"
  SHARED "${HML_FIRMWARE_SHARED}"
  POSIX "${HML_FIRMWARE_POSIX}"
)

# Convenience: the bare-metal surface as one target, protocol + core, and not
# one socket call. This is what a bare-metal or RTOS firmware links.
if(NOT TARGET hml::hml_firmware)
  add_library(hml::hml_firmware INTERFACE IMPORTED)
  set_target_properties(hml::hml_firmware PROPERTIES
    INTERFACE_LINK_LIBRARIES "hml::hml_protocol;hml::hml_core")
endif()
