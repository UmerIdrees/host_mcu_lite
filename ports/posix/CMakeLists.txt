file(GLOB_RECURSE HML_PORT_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(hml_port_posix STATIC ${HML_PORT_SRCS})
target_include_directories(hml_port_posix PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(hml_port_posix PUBLIC hml_iface)
