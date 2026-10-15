# Sample firmware image that links hml as a drop-in.
#
# This is the "copy this directory into your project" skeleton. It builds on a
# desktop Linux/gcc with no hml installed, which is deliberate: if it cannot
# build here, it cannot build in CI, and a firmware integration that is never
# compiled is a firmware integration that does not work.
#
#   cmake -S firmware/hml_object/sample -B /tmp/hml_fw_sample
#   cmake --build /tmp/hml_fw_sample
#   /tmp/hml_fw_sample/hml_fw_sample
#
# To use it from a real project, copy main.c + port_stub.c, point them at your
# board, and add_subdirectory() the parent instead:
#
#   add_subdirectory(third_party/hml/firmware/hml_object)
#   add_executable(my_firmware main.c port_my_board.c ${hml_firmware_contract_source})
#   target_link_libraries(my_firmware PRIVATE hml::hml_core)

cmake_minimum_required(VERSION 3.16)
project(hml_fw_sample C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror)

set(HML_FIRMWARE_NAME "hml_fw_sample" CACHE STRING "firmware image name")
set(HML_FIRMWARE_POSIX OFF CACHE BOOL "link the POSIX port (a real RTOS port sets its own tag)")

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." hml_object)

add_executable(${HML_FIRMWARE_NAME} main.c port_stub.c ${hml_firmware_contract_source})
target_link_libraries(${HML_FIRMWARE_NAME} PRIVATE hml::hml_core)
