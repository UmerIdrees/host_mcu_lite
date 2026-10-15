# M3 / T1a: socket-crafted RoCEv2 (UC RDMA WRITE) transmitter.
file(GLOB HML_ROCEV2_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

add_library(hml_rocev2 STATIC ${HML_ROCEV2_SRCS})
target_include_directories(hml_rocev2 PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(hml_rocev2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hml_rocev2 PUBLIC hml_core hml_protocol hml_port_posix)

# hml_gen_roce (host/roce_gen) is the T1a sender CLI for this transport, so it is
# pulled in from here rather than from the top-level fixed-directory list.
add_subdirectory(${CMAKE_SOURCE_DIR}/host/roce_gen ${CMAKE_BINARY_DIR}/host/roce_gen)
