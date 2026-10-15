# hml_static.cmake -- the static arm of the hml partition.
#
# The static targets themselves (hml_protocol, hml_core, hml_port_posix,
# hml_control, hml_udp, hml_rocev2) are created by the subdirectory CMakeLists
# files under src/ and ports/, because the in-tree test suite and tools link
# them by those exact names. This file does not recreate them; it
#
#   1. marks them POSITION_INDEPENDENT_CODE so the *same* compilation settings
#      are valid for both arms (this is the cost control for the shared arm:
#      the sources are small and the only difference is -fvisibility=hidden),
#   2. fixes up the PUBLIC include path and interface link libraries from the
#      component table in cmake/hml_component.cmake,
#   3. declares HML_STATIC on their interface, which is what makes the exported
#      targets correct on MSVC/PE, and
#   4. records which targets cmake/install.cmake must install and export.
#
# HML_BUILD_STATIC=OFF skips step 4 (nothing static is installed or exported).
# The in-tree static targets are still built in that case, because the CTest
# suite and the CLI tools link them directly; this option is about the
# *packaging* surface, not about the build graph. See docs/final_partitioning.md.

include(GNUInstallDirs)
include(cmake/hml_component.cmake)

hml_component_list(HML_ALL_COMPONENTS)

# Which components reach the export set. Only those need their include
# interface rewritten to a relocatable form; the rest (the transports) keep the
# absolute build-tree include dirs their own in-tree consumers (hml_roce_gen,
# hml_host_roce) already rely on, and are never installed.
if(HML_BUILD_STATIC)
  set(HML_EXPORTED_COMPONENTS protocol core control udp rocev2)
  if(NOT HML_NO_POSIX)
    list(APPEND HML_EXPORTED_COMPONENTS port_posix)
  endif()
  if(NOT HML_INSTALL_TRANSPORTS)
    list(REMOVE_ITEM HML_EXPORTED_COMPONENTS udp rocev2)
  endif()
else()
  set(HML_EXPORTED_COMPONENTS "")
endif()

set(HML_STATIC_EXPORT_TARGETS "")
set(HML_STATIC_INSTALL_LIBS "")

# The bare-metal surface: protocol + core, and nothing that touches a socket.
# Deliberately NOT hml::hml_core alone -- hmlConfig.cmake appends the POSIX port
# to hml::hml_core for host convenience, and a firmware must not inherit that.
set(HML_FIRMWARE_COMPONENTS protocol core)

foreach(_c IN LISTS HML_ALL_COMPONENTS)
  if(TARGET hml_${_c})
    hml_component_spec(${_c} _srcs _links _priv)
    hml_component_is_portless(${_c} _portless)

    set_target_properties(hml_${_c} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)

    # hml_port_posix used to carry an INTERFACE link to the artifact-less
    # hml_iface; the export set cannot contain an INTERFACE target that has no
    # artifact, so the dependency is dropped and the include dir / POSIX
    # feature macro it provided are already in this target's own interface.
    set(_iface_libs "")
    foreach(_d IN LISTS _links)
      list(APPEND _iface_libs hml_${_d})
    endforeach()
    set_target_properties(hml_${_c} PROPERTIES INTERFACE_LINK_LIBRARIES "${_iface_libs}")

    if(NOT _c IN_LIST HML_EXPORTED_COMPONENTS)
      continue()
    endif()

    # Public: the installed include dir only. Private: the component's own
    # directory for its private header (hml_udp_priv.h, hml_roce_priv.h).
    # Existing include dirs are kept but wrapped in $<BUILD_INTERFACE:...>: an
    # absolute build-tree path in an exported INTERFACE_INCLUDE_DIRECTORIES is
    # rejected by CMake at install time, and in-tree consumers (tests/fuzz
    # includes "hml_control.h" through hml_control's interface) still need it.
    get_target_property(_inc hml_${_c} INTERFACE_INCLUDE_DIRECTORIES)
    set(_relocatable "")
    foreach(_d IN LISTS _inc)
      if(_d MATCHES "^\\$<")
        list(APPEND _relocatable ${_d})
      elseif(IS_ABSOLUTE "${_d}")
        list(APPEND _relocatable "$<BUILD_INTERFACE:${_d}>")
      else()
        list(APPEND _relocatable ${_d})
      endif()
    endforeach()
    list(APPEND _relocatable
      "$<BUILD_INTERFACE:${HML_ROOT}/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    if(_c STREQUAL "control")
      # hml_control.h / hml_client.h ship under include/hml/detail so the
      # control component is installable without widening the frozen 13-header
      # public API.
      list(APPEND _relocatable
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/hml/detail>")
    endif()
    set_target_properties(hml_${_c} PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_relocatable}")
    if(_priv)
      target_include_directories(hml_${_c} PRIVATE ${_priv})
    endif()

    # HML_STATIC: on MSVC/PE this is what turns __declspec(dllimport) off for a
    # consumer linking the archive. Inert on ELF.
    #
    # Both definitions go through $<INSTALL_INTERFACE:> on purpose. As a plain
    # INTERFACE definition they would also fire for in-tree consumers, and
    # HML_NO_POSIX in particular is actively harmful there: ports/freertos_lwip
    # links hml_protocol PUBLIC and guards its own port definitions with
    # `#if !defined(HML_PORT_POSIX)`, so inheriting HML_NO_POSIX makes it emit a
    # full host port implementation and collide with hml_port_posix at link time
    # in tests/m5_ports. These two are properties of the *installed package*,
    # not of this build tree.
    target_compile_definitions(hml_${_c} INTERFACE "$<INSTALL_INTERFACE:HML_STATIC>")
    if(HML_NO_POSIX)
      target_compile_definitions(hml_${_c} INTERFACE "$<INSTALL_INTERFACE:HML_NO_POSIX>")
    endif()

    list(APPEND HML_STATIC_EXPORT_TARGETS hml_${_c})
  endif()
endforeach()

# hml_iface is an INTERFACE target with no artifact: it exists so ports/posix can
# be configured before the fixed directories are added, and it must never reach
# an export set.
if(TARGET hml_iface)
  set_target_properties(hml_iface PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ---- packaging inputs, consumed by cmake/install.cmake ---------------------
# HML_NO_POSIX: an embedded-flavoured install. The POSIX port is neither built
# into the shared arm nor installed nor exported, and HML_NO_POSIX is stamped on
# the exported interfaces so a consumer's translation units cannot auto-tag as
# POSIX. Bare-metal consumers then *must* supply hml_clock_us/hml_sock_*.
if(HML_NO_POSIX)
  set(HML_EXPORT_TARGETS hml_protocol hml_core)
  set(HML_EXPORT_TRANSPORT_TARGETS "")
  # hml_control is excluded here, not because it is unwanted, but because it
  # *links* hml_port_posix (it speaks the control protocol over UDP). Exporting
  # it without the port produces an export set that cannot resolve its own
  # INTERFACE_LINK_LIBRARIES, and CMake rejects the generate step. Firmware that
  # wants the control plane builds it through firmware/hml_object, which
  # supplies the port symbols itself.
  set(HML_EXPORT_CONTROL_TARGETS "")
else()
  set(HML_EXPORT_TARGETS hml_protocol hml_core)
  if(TARGET hml_port_posix)
    list(APPEND HML_EXPORT_TARGETS hml_port_posix)
  endif()
  set(HML_EXPORT_CONTROL_TARGETS "")
  if(TARGET hml_control)
    list(APPEND HML_EXPORT_CONTROL_TARGETS hml_control)
  endif()
  set(HML_EXPORT_TRANSPORT_TARGETS "")
  if(HML_INSTALL_TRANSPORTS)
    foreach(_t hml_udp hml_rocev2)
      if(TARGET ${_t})
        list(APPEND HML_EXPORT_TRANSPORT_TARGETS ${_t})
      endif()
    endforeach()
  endif()
endif()

message(STATUS "hml: static arm  = static=${HML_BUILD_STATIC} shared=${HML_BUILD_SHARED} posix_port=${HML_NO_POSIX}")
