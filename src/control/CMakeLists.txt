# Track B: discovery beacon + control plane (hml_control library + CLIs).
file(GLOB HML_CONTROL_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(REMOVE_ITEM HML_CONTROL_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/hml_ctrl_device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/hml_ctrl_host.c
)

add_library(hml_control STATIC ${HML_CONTROL_SRCS})
target_include_directories(hml_control PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_control PUBLIC hml_protocol hml_port_posix)

add_executable(hml_ctrl_device hml_ctrl_device.c)
target_link_libraries(hml_ctrl_device PRIVATE hml_control hml_rocev2 hml_core)
target_include_directories(hml_ctrl_device PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(hml_ctrl_host hml_ctrl_host.c)
target_link_libraries(hml_ctrl_host PRIVATE hml_control)
