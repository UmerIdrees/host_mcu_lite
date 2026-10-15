# hml_shared.cmake -- the shared arm of the hml partition.
#
# For every component in cmake/hml_component.cmake this file adds a parallel
# target
#
#     hml_<component>_shared      OUTPUT_NAME hml_<component>
#                                  -> libhml_<component>.so
#
# so the static and shared arms of a component install side by side
# (libhml_core.a and libhml_core.so) and can be linked independently.
#
# Two deliberate choices, both load-bearing:
#
#  * HML_BUILDING_DLL is defined PRIVATE. It is inert on ELF (see
#    include/hml/hml_export.h) and selects the __declspec(dllexport) side on
#    MSVC/PE. It is never on a consumer's interface, because on PE a consumer
#    wants dllimport, not dllexport.
#
#  * C_VISIBILITY_PRESET hidden. Without it a shared object exports every
#    non-static symbol in the object, including the private helpers of each
#    transport and the control server's internals, and the exported ABI stops
#    being the public headers. With it, exactly the HML_API-decorated
#    declarations in include/hml/*.h are exported -- that is the whole
#    definition of the shared ABI. include/hml/hml_export.h carries no
#    per-target macro on ELF precisely so that both arms can be produced from
#    one set of sources.
#
# Cost note: the shared arm recompiles the component sources rather than
# re-archiving the static objects, because the component targets are STATIC
# libraries (created under src/, owned by other tracks) and CMake cannot take
# $<TARGET_OBJECTS:...> from a STATIC library. The static targets are built with
# POSITION_INDEPENDENT_CODE ON (see cmake/hml_static.cmake), so the only real
# delta is -fvisibility=hidden on ~10 small files. Use -DHML_BUILD_SHARED=OFF
# for a pure-CI build that does not need the .so files at all.

include(cmake/hml_component.cmake)

set(HML_SHARED_EXPORT_TARGETS "")

if(HML_BUILD_SHARED)
  hml_component_list(HML_ALL_COMPONENTS)

  foreach(_c IN LISTS HML_ALL_COMPONENTS)
    # The port component is skipped entirely by an embedded-flavoured build,
    # and so is anything that links it. hml_udp / hml_rocev2 call the hml_sock_*
    # family, so exporting them without the port would produce a .so with
    # unresolved hml_sock_* symbols and an export set that references a target
    # which does not exist -- CMake rejects that at generate time. This matches
    # the static arm, which drops the same components under HML_NO_POSIX.
    # Embedded consumers that want a transport build one through
    # firmware/hml_object, which never adds ports/posix and therefore never hits
    # this problem.
    if(HML_NO_POSIX AND (_c STREQUAL "port_posix" OR _c STREQUAL "control" OR
                         _c STREQUAL "udp" OR _c STREQUAL "rocev2"))
      continue()
    endif()
    # The transports are not installed by default: their public entry points
    # (hml_udp_ops/hml_udp_register, hml_rocev2_ops/hml_rocev2_register,
    # hml_roce_build_datagram) live in private headers and are therefore not
    # HML_API-decorated, so -fvisibility=hidden would leave libhml_udp.so
    # exporting nothing a consumer can call. Build them only with
    # -DHML_INSTALL_TRANSPORTS=ON. They remain available in the build tree
    # (static arm) and through firmware/hml_object.
    if((_c STREQUAL "udp" OR _c STREQUAL "rocev2") AND NOT HML_INSTALL_TRANSPORTS)
      continue()
    endif()

    hml_component_spec(${_c} _srcs _links _priv)
    if(NOT _srcs)
      continue()
    endif()

    set(_t hml_${_c}_shared)
    if(NOT TARGET ${_t})
      add_library(${_t} SHARED ${_srcs})
      set_target_properties(${_t} PROPERTIES
        OUTPUT_NAME hml_${_c}
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        DEFINE_SYMBOL "")

      target_compile_definitions(${_t} PRIVATE HML_BUILDING_DLL)
      target_compile_definitions(${_t} INTERFACE HML_PARTITION_SHARED)
      if(HML_NO_POSIX)
        target_compile_definitions(${_t} PRIVATE HML_NO_POSIX)
      endif()

      set_target_properties(${_t} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${HML_ROOT}/include>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
      if(_priv)
        target_include_directories(${_t} PRIVATE ${_priv})
      endif()
    endif()

    # Same dependency shape as the static arm, expressed in shared targets.
    set(_shlibs "")
    foreach(_d IN LISTS _links)
      if(HML_NO_POSIX AND _d STREQUAL "port_posix")
        continue()
      endif()
      if(TARGET hml_${_d}_shared)
        list(APPEND _shlibs hml_${_d}_shared)
      endif()
    endforeach()
    set_target_properties(${_t} PROPERTIES INTERFACE_LINK_LIBRARIES "${_shlibs}")

    list(APPEND HML_SHARED_EXPORT_TARGETS ${_t})

    # Documentation aliases so a consumer can write the spelling it prefers.
    add_library(hml::hml_${_c}_shared ALIAS ${_t})
  endforeach()
endif()
