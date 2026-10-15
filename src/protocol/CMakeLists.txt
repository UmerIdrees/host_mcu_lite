file(GLOB_RECURSE HML_PROTOCOL_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(hml_protocol STATIC ${HML_PROTOCOL_SRCS})
target_include_directories(hml_protocol PUBLIC ${CMAKE_SOURCE_DIR}/include)
