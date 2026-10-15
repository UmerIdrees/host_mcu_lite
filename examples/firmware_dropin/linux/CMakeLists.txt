# linux -- the installed-package drop-in, over a real UDP socket.
#
# This is the "I want hml from a package manager" path, not the add_subdirectory
# path that ../non_rtos/ demonstrates. It consumes a *staged install* through
# find_package(hml), so it also proves the installed package is self-contained:
# the include dir, the export set and the .so files all come from the prefix,
# never from this source tree.
#
#   cmake --install /tmp/hml_build --prefix /tmp/hml_stage
#   cmake -S examples/firmware_dropin/linux -B /tmp/hml_linux \
#         -DCMAKE_PREFIX_PATH=/tmp/hml_stage
#   cmake --build /tmp/hml_linux
#   LD_LIBRARY_PATH=/tmp/hml_stage/lib /tmp/hml_linux/hml_dropin_linux
#
# The same, linked against the shared components:
#
#   cmake -S examples/firmware_dropin/linux -B /tmp/hml_linux_sh \
#         -DCMAKE_PREFIX_PATH=/tmp/hml_stage -DHML_DROPIN_SHARED=ON
#
# CMake adds a build-tree RUNPATH into the staged prefix automatically, so the
# shared binary runs as-is here. LD_LIBRARY_PATH is what an *installed* binary
# needs instead, and the hml.pc / hml_shared.pc path in docs/install.md is where
# that gets checked -- with the RPATH stripped, so the loader really is tested.
#
# On the port contract: this demo links hml::hml_core and gets hml_port_posix
# from the same package. It deliberately does NOT define HML_NO_POSIX, because
# on Linux the port is a real implementation rather than a stub. See
# docs/install.md for the hml_NO_DEFAULT_PORT variant that keeps the consumer
# free of the POSIX port.

cmake_minimum_required(VERSION 3.16)
project(hml_dropin_linux C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror)

# The whole point of this file is to consume an *installed* hml. Refusing to
# configure against the in-tree build keeps the check honest: if the export set
# is missing a target, this fails here instead of silently picking up the source
# tree's include path.
find_package(hml REQUIRED)
message(STATUS "hml_dropin_linux: hml ${hml_VERSION} from ${hml_CONFIG}")

# Same demo, both linkages, from the same staged install. The shared variant is
# the one that proves the .so files in the prefix are loadable and that the
# exported _shared targets resolve; the static one proves the archives and their
# HML_STATIC interface are self-contained.
option(HML_DROPIN_SHARED "link the demo against hml::hml_core_shared" OFF)

add_executable(hml_dropin_linux linux_main.c)
if(HML_DROPIN_SHARED)
  target_link_libraries(hml_dropin_linux PRIVATE hml::hml_core_shared)
else()
  target_link_libraries(hml_dropin_linux PRIVATE hml::hml_core)
endif()

# HML_HAS_POSIX_PORT must be 1 in this consumer. Note this cannot be checked in
# CMake: it is a C macro, not a variable, so `if(HML_HAS_POSIX_PORT)` would
# compare the *string* "HML_HAS_POSIX_PORT" and silently do nothing. The real
# check is the _Static_assert in linux_main.c, which fails at compile time.
