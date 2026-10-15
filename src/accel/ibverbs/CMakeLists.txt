# Backend selection for hml_accel. Exactly one of
#   hml_accel_ibverbs.c       (real: device + PD + ibv_reg_mr)
#   hml_accel_backend_stub.c  (default: CPU passthrough only)
# is added to the hml_accel target. They live in this subdirectory so the
# non-recursive file(GLOB *.c) one level up cannot pick both up, which is what
# keeps src/ free of #ifdef.
#
# HML_HAVE_IBVERBS (note the spelling: no trailing H) is defined only when BOTH
# <infiniband/verbs.h> and libibverbs are found. Note also that finding the
# headers is not the same as having a device: hml_accel_probe() checks for an
# enumerable device at runtime, so a build host with the headers but no NIC
# still reports the CPU fallback.
find_path(HML_ACCEL_IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)
find_library(HML_ACCEL_IBVERBS_LIBRARY NAMES ibverbs)

if(HML_ACCEL_IBVERBS_INCLUDE_DIR AND HML_ACCEL_IBVERBS_LIBRARY)
  set(HML_HAVE_IBVERBS 1)
  target_sources(hml_accel PRIVATE hml_accel_ibverbs.c)
  target_compile_definitions(hml_accel PRIVATE HML_HAVE_IBVERBS=1)
  target_include_directories(hml_accel PRIVATE ${HML_ACCEL_IBVERBS_INCLUDE_DIR})
  target_link_libraries(hml_accel PRIVATE ${HML_ACCEL_IBVERBS_LIBRARY})
  set(HML_ACCEL_HAVE_IBVERBS 1 PARENT_SCOPE)
else()
  target_sources(hml_accel PRIVATE hml_accel_backend_stub.c)
  set(HML_ACCEL_HAVE_IBVERBS 0 PARENT_SCOPE)
endif()
