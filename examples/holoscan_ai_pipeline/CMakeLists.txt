# Track C: Holoscan Multi-Tier AI Pipeline Application
add_executable(holoscan_ai_pipeline main.cpp)

target_include_directories(holoscan_ai_pipeline PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/host
  ${CMAKE_SOURCE_DIR}/examples/holoscan_operator)

target_link_libraries(holoscan_ai_pipeline PRIVATE
  hml_receiver_op
  hml_core
  hml_protocol
  hml_port_posix
  hml_verbs_rx)

find_path(CUDA_INCLUDE_DIR cuda_runtime.h PATHS /usr/local/cuda/include /usr/include)
if(CUDA_INCLUDE_DIR)
  target_include_directories(holoscan_ai_pipeline PRIVATE ${CUDA_INCLUDE_DIR})
endif()

find_library(CUDA_CUDART_LIBRARY NAMES cudart cudart_static PATHS /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)
if(CUDA_CUDART_LIBRARY)
  target_link_libraries(holoscan_ai_pipeline PRIVATE ${CUDA_CUDART_LIBRARY})
endif()
