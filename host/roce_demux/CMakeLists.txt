# M3 / T1a: host-side RoCEv2 receiver / demux + the hml_host_roce CLI.
file(GLOB HML_ROCE_DEMUX_GLOB CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
foreach(_src ${HML_ROCE_DEMUX_GLOB})
  if(NOT _src MATCHES "hml_host_roce\\.c$")
    list(APPEND HML_ROCE_DEMUX_SRCS ${_src})
  endif()
endforeach()

add_library(hml_roce_demux STATIC ${HML_ROCE_DEMUX_SRCS})
target_include_directories(hml_roce_demux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(hml_roce_demux PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(hml_roce_demux PUBLIC hml_core hml_protocol hml_port_posix)

add_executable(hml_host_roce hml_host_roce.c)
target_include_directories(hml_host_roce PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(hml_host_roce PRIVATE hml_roce_demux hml_rocev2 hml_core hml_port_posix)
