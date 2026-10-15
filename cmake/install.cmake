include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# hml is a partition: several independently linkable components, each shipped
# as a static archive AND (unless -DHML_BUILD_SHARED=OFF) as a shared object.
# This file is the packaging side of that; the component table lives in
# cmake/hml_component.cmake and the arm builders in cmake/hml_{static,shared}.cmake.
#
# Installed layout:
#   <prefix>/include/hml/*.h            the public API (now 15 headers, was 13)
#   <prefix>/include/hml/detail/*.h     the control-plane component's headers
#   <prefix>/lib/libhml_<c>.a           static arm
#   <prefix>/lib/libhml_<c>.so          shared arm
#   <prefix>/lib/cmake/hml/             hmlConfig.cmake + two export sets
#   <prefix>/lib/pkgconfig/hml*.pc      hml (static), hml_shared, hml_firmware
# ---------------------------------------------------------------------------

# The public API surface for 0.2.x is every header in include/hml/. The shipped
# libraries are the components: hml_protocol + hml_core (the portable arms) and
# hml_port_posix + hml_control (the arms that need a platform). The interface
# include dirs and link libraries were already fixed up in
# cmake/hml_static.cmake / cmake/hml_shared.cmake, using $<INSTALL_INTERFACE:>
# so they stay relocatable; a bare ${CMAKE_SOURCE_DIR}/include is rejected by
# CMake's export machinery.

set(HML_INSTALL_TARGETS "")
if(HML_BUILD_STATIC)
  list(APPEND HML_INSTALL_TARGETS ${HML_EXPORT_TARGETS} ${HML_EXPORT_CONTROL_TARGETS})
  if(HML_EXPORT_TRANSPORT_TARGETS)
    list(APPEND HML_INSTALL_TARGETS ${HML_EXPORT_TRANSPORT_TARGETS})
  endif()
endif()

set(HML_INSTALL_SHARED_TARGETS "")
if(HML_BUILD_SHARED)
  list(APPEND HML_INSTALL_SHARED_TARGETS ${HML_SHARED_EXPORT_TARGETS})
endif()

install(TARGETS ${HML_INSTALL_TARGETS}
  EXPORT hmlTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(HML_INSTALL_SHARED_TARGETS)
  install(TARGETS ${HML_INSTALL_SHARED_TARGETS}
    EXPORT hmlTargetsShared
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# The export sets must be written before hmlConfig.cmake, which includes them.
# Each is guarded: install(EXPORT) on an export set with no targets is a hard
# error, so -DHML_BUILD_STATIC=OFF would otherwise fail at generate time.
if(HML_INSTALL_TARGETS)
  install(EXPORT hmlTargets
    NAMESPACE hml::
    FILE hmlTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hml)
endif()

if(HML_INSTALL_SHARED_TARGETS)
  # The file must NOT be named hmlTargets-shared*.cmake. An export set globs
  # "<stem>-*.cmake" for its per-configuration files, so the static
  # hmlTargets.cmake would pick up the shared per-config file and try to set
  # properties on hml::*_shared targets that the static file has not declared
  # yet. A distinct stem keeps the two globs disjoint.
  install(EXPORT hmlTargetsShared
    NAMESPACE hml::
    FILE hmlSharedTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hml)
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/hml
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")

# hml_control's own server/client headers are not part of the frozen 13-header
# public API, but the control component is installed, so its headers ship with
# it under include/hml/detail/. Only the codec entry points (hml_ctrl_fill,
# hml_ctrl_parse, hml_beacon_fill, hml_beacon_parse) are HML_API-exported from
# the .so; the server internals are in the .a. See docs/final_partitioning.md.
install(FILES
  ${CMAKE_SOURCE_DIR}/src/control/hml_control.h
  ${CMAKE_SOURCE_DIR}/src/control/hml_client.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hml/detail)

# Capability flags for downstream.
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/hmlConfigVersion.cmake
  VERSION ${HML_VERSION}
  COMPATIBILITY SameMinorVersion)
configure_package_config_file(
  ${CMAKE_SOURCE_DIR}/cmake/hmlConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/hmlConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hml)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/hmlConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/hmlConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hml)

# pkg-config needs the *actual* configured prefix, not a literal @CMAKE_INSTALL_PREFIX@,
# so the .pc stays correct when installed with --prefix or via DESTDIR.
set(HML_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  # absolute LIBDIR: assume the prefix is its parent
  string(REGEX REPLACE "/lib.*$" "" HML_PC_PREFIX "${CMAKE_INSTALL_LIBDIR}")
endif()

# Libs: line per variant. The static variant keeps its 0.2.0 spelling exactly,
# so an existing `pkg-config --libs hml` consumer does not change behaviour.
if(HML_NO_POSIX)
  set(HML_PC_LIBS_STATIC "-L\${libdir} -lhml_core -lhml_protocol")
else()
  set(HML_PC_LIBS_STATIC "-L\${libdir} -lhml_core -lhml_protocol -lhml_port_posix")
endif()
# The shared variant must name the .so files by full path. `-lhml_core` alone
# would NOT work: this install ships both libhml_core.a and libhml_core.so, and
# the linker prefers the archive, so a `-l` spelling would silently hand the
# consumer static linkage from a package named "shared". Spelling the filenames
# is also what makes the loader dependency real, so the binary actually fails at
# startup without LD_LIBRARY_PATH instead of quietly working.
if(HML_NO_POSIX)
  set(HML_PC_LIBS_SHARED
    "\${libdir}/libhml_core.so \${libdir}/libhml_protocol.so")
else()
  set(HML_PC_LIBS_SHARED
    "\${libdir}/libhml_core.so \${libdir}/libhml_protocol.so \${libdir}/libhml_port_posix.so")
endif()
# The bare-metal variant: the two portable arms and nothing that can open a
# socket. Your port supplies hml_clock_us and the hml_sock_* family.
set(HML_PC_LIBS_FIRMWARE "-L\${libdir} -lhml_core -lhml_protocol")

set(HML_PC_NAME hml)
set(HML_PC_LIBS "${HML_PC_LIBS_STATIC}")
configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/hml.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/hml.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hml.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(HML_BUILD_SHARED)
  set(HML_PC_LIBS "${HML_PC_LIBS_SHARED}")
  set(HML_PC_NAME hml_shared)
  configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/hml.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/hml_shared.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hml_shared.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

if(HML_BUILD_STATIC)
  set(HML_PC_LIBS "${HML_PC_LIBS_FIRMWARE}")
  set(HML_PC_NAME hml_firmware)
  configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/hml_firmware.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/hml_firmware.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hml_firmware.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

# Record what this install actually contains, for the docs and for anyone
# reading the staged tree. Harmless, one line, and it makes the partition
# auditable without re-running CMake with --trace.
set(HML_MANIFEST_LINES "")
foreach(_t ${HML_INSTALL_TARGETS} ${HML_INSTALL_SHARED_TARGETS})
  get_target_property(_tn ${_t} OUTPUT_NAME)
  if(NOT _tn)
    set(_tn ${_t})
  endif()
  get_target_property(_tt ${_t} TYPE)
  string(APPEND HML_MANIFEST_LINES "  ${_tn} (${_t}, ${_tt})\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hml-partition-manifest.txt
  "hml ${HML_VERSION} installed partition\n"
  "  HML_BUILD_STATIC=${HML_BUILD_STATIC}\n"
  "  HML_BUILD_SHARED=${HML_BUILD_SHARED}\n"
  "  HML_NO_POSIX=${HML_NO_POSIX}\n"
  "  HML_INSTALL_TRANSPORTS=${HML_INSTALL_TRANSPORTS}\n"
  "${HML_MANIFEST_LINES}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hml-partition-manifest.txt
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hml)
