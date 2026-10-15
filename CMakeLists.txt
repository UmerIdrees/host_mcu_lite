cmake_minimum_required(VERSION 3.16)
project(host_mcu_lite LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

project(host_mcu_lite VERSION 0.2.0 LANGUAGES C CXX)
set(HML_VERSION ${PROJECT_VERSION})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HML_BUILD_TESTS "Build tests" ON)
option(HML_BUILD_EXAMPLES "Build examples/tools" ON)
option(HML_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(HML_INSTALL "Generate install rules" ON)
# --- partitioning options (docs/final_partitioning.md) ---------------------
option(HML_BUILD_STATIC
  "Install/export the static archives (libhml_*.a). The in-tree static targets are always built because the CTest suite links them."
  ON)
option(HML_BUILD_SHARED
  "Build and install the shared arm (libhml_*.so) alongside the static archives"
  ON)
option(HML_NO_POSIX
  "Embedded-flavoured install: do not install or export the POSIX port, and stamp HML_NO_POSIX on the exported interfaces so a bare-metal consumer sees hml_port.h as 'you supply the clock and the sockets'. The in-tree host tools and tests still need POSIX and keep linking ports/posix; for firmware that must never compile socket code use firmware/hml_object"
  OFF)
option(HML_INSTALL_TRANSPORTS
  "Also install/export the UDP and RoCEv2 transports. Their public entry points live in private headers (hml_udp_priv.h / hml_roce_priv.h) and are not HML_API-decorated, so the .a arm is the useful one."
  OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(HML_WARNINGS_AS_ERRORS)
  add_compile_options(-Werror)
endif()

# Shared include dir for every target in this project.
add_library(hml_iface INTERFACE)
target_include_directories(hml_iface INTERFACE ${CMAKE_SOURCE_DIR}/include)

# strict C11 needs an explicit POSIX feature level for clock_gettime/socket APIs
# (clock_gettime, nanosleep, ...). Applied directory-wide rather than only on
# hml_iface, because install.cmake detaches the interface link to keep the
# export set self-contained.
add_compile_definitions(_POSIX_C_SOURCE=200809L)
target_compile_definitions(hml_iface INTERFACE _POSIX_C_SOURCE=200809L)

include_directories(${CMAKE_SOURCE_DIR}/include)

# Source root handed to the component table in cmake/hml_component.cmake and to
# the drop-in package in cmake/hml_firmware.cmake. Derived from this file's own
# location, so it survives being pulled in as a subdirectory of a firmware tree.
get_filename_component(HML_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

# enable_testing() must precede the subdirectory loop so add_test() in any
# subdirectory is registered (CTest is directory-scoped).
if(HML_BUILD_TESTS)
  enable_testing()
endif()

# ---- fixed directories -------------------------------------------------
add_subdirectory(src/protocol)
add_subdirectory(src/core)
# hml_port_posix is always built, because the in-tree host tools, adapters and
# tests are POSIX programs and link it. HML_NO_POSIX is an *install flavour*:
# it drops the port from the shared arm and from the export set, and stamps
# HML_NO_POSIX on the exported interfaces. Firmware that must not compile POSIX
# socket code at all does not use this project as a host -- it uses
# firmware/hml_object, which never adds ports/posix.
add_subdirectory(ports/posix)

# ---- parallel-track directories (guarded so partial trees still configure)
foreach(_d
    src/transports/udp
    src/transports/rocev2
    src/control
    src/bridge
    src/accel
    adapters/synthetic
    adapters/filestream
    host/hml_host
    host/roce_demux
    host/soft_roce
    host/verbs_rx
    ports/freertos_lwip
    ports/esp_idf
    renode
    examples/holoscan_ai_ingest
    examples/holoscan_operator
    examples/holoscan_ai_pipeline)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_d}/CMakeLists.txt)
    add_subdirectory(${_d})
  endif()
endforeach()

# ---- partition arms ----------------------------------------------------
# Included after every component target exists: hml_static.cmake adjusts the
# static targets in place and hml_shared.cmake mirrors them.
include(cmake/hml_static.cmake)
include(cmake/hml_shared.cmake)

if(HML_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- install / package -------------------------------------------------
if(HML_INSTALL)
  include(cmake/install.cmake)
endif()
