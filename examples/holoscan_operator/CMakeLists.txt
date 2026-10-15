# Track C: Holoscan Operator Library
add_library(hml_receiver_op STATIC hml_receiver_op.cpp)

target_include_directories(hml_receiver_op PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/host)

target_link_libraries(hml_receiver_op PUBLIC
  hml_core
  hml_protocol
  hml_port_posix
  hml_verbs_rx)

find_path(CUDA_INCLUDE_DIR cuda_runtime.h PATHS /usr/local/cuda/include /usr/include)
if(CUDA_INCLUDE_DIR)
  target_include_directories(hml_receiver_op PUBLIC ${CUDA_INCLUDE_DIR})
endif()

find_library(CUDA_CUDART_LIBRARY NAMES cudart cudart_static PATHS /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)
if(CUDA_CUDART_LIBRARY)
  target_link_libraries(hml_receiver_op PUBLIC ${CUDA_CUDART_LIBRARY})
endif()
