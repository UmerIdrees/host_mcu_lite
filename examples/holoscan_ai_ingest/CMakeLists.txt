add_executable(hml_ai_ingest_demo hml_ai_ingest_demo.c)
target_include_directories(hml_ai_ingest_demo PRIVATE
  ${PROJECT_SOURCE_DIR}/host/hml_host
  ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(hml_ai_ingest_demo PRIVATE
  hml_host
  hml_core
  hml_port_posix
  m
)
