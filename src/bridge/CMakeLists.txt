# Track E: M4 bridge mode (I2C/GPIO/UART-shaped windows + control glue).
file(GLOB HML_BRIDGE_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(REMOVE_ITEM HML_BRIDGE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/hml_bridge_demo.c)

find_package(Threads REQUIRED)

add_library(hml_bridge STATIC ${HML_BRIDGE_SRCS})
target_include_directories(hml_bridge PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/control)
target_link_libraries(hml_bridge PUBLIC hml_control hml_protocol hml_core hml_port_posix)

add_executable(hml_bridge_demo hml_bridge_demo.c)
target_link_libraries(hml_bridge_demo PRIVATE hml_bridge hml_control hml_core Threads::Threads)
