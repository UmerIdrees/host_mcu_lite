# hml_firmware.cmake -- build hml as a drop-in subdirectory for a firmware.
#
# This is the "drop your own firmware here" packaging path. A firmware project
# add_subdirectory()s firmware/hml_object/ and gets:
#
#   hml_protocol   codecs + CRCs                 portable
#   hml_core       framer, reassembly, latency   portable, needs a PORT
#   hml_control    beacon + control codec/server needs a PORT
#   hml_udp        T0 UDP data plane             needs a PORT
#   hml_rocev2     experimental RoCEv2           needs a PORT
#   (hml_<c>_shared, with SHARED)                same, as .so
#
# and, unless asked for, NO hml_port_posix and nothing that includes
# <sys/socket.h>. "Needs a PORT" is not a warning: the firmware must define
# hml_clock_us() and the five hml_sock_*() calls. hml_firmware_contract_source
# points at a generated translation unit that references all six, so a missing
# one is a link error naming the symbol rather than a runtime surprise.
#
# hml_firmware_add(
#   [COMPONENTS protocol;core;control;udp;rocev2]   subset to build (default: all)
#   [SHARED]                                       also build the _shared arm
#   [POSIX]                                        build hml_port_posix and do NOT
#                                                  define HML_NO_POSIX on consumers
# )
#
# Options honoured from the parent scope (cache or normal):
#   HML_FIRMWARE_POSIX   default for [POSIX]
#   HML_FIRMWARE_SHARED  default for [SHARED]
#
# Note there is no `tests`, no `host/`, no `adapters/`, no install rule and no
# pkg-config file here: this package exists to be linked into a firmware image.

include("${CMAKE_CURRENT_LIST_DIR}/hml_component.cmake")

# INTERNAL cache so hml_firmware_add() can find the template no matter which
# scope included this file.
set(HML_FIRMWARE_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "hml firmware template dir")

# hml_firmware_add(...) -- see the block comment above.
function(hml_firmware_add)
  cmake_parse_arguments(FW "" "ROOT" "COMPONENTS;SHARED;POSIX" ${ARGN})

  if(NOT FW_ROOT)
    set(FW_ROOT "${HML_ROOT}")
  endif()
  if(NOT FW_ROOT)
    message(FATAL_ERROR "hml_firmware_add: pass -DROOT=<hml source dir> or set HML_ROOT")
  endif()
  if(NOT IS_DIRECTORY "${FW_ROOT}/include/hml")
    message(FATAL_ERROR "hml_firmware_add: '${FW_ROOT}' does not look like an hml source root "
                        "(no include/hml)")
  endif()

  if(NOT FW_COMPONENTS)
    set(FW_COMPONENTS protocol core control udp rocev2)
  endif()
  if(NOT DEFINED FW_SHARED)
    if(HML_FIRMWARE_SHARED)
      set(FW_SHARED ${HML_FIRMWARE_SHARED})
    else()
      set(FW_SHARED OFF)
    endif()
  endif()
  if(NOT DEFINED FW_POSIX)
    if(HML_FIRMWARE_POSIX)
      set(FW_POSIX ${HML_FIRMWARE_POSIX})
    else()
      set(FW_POSIX OFF)
    endif()
  endif()

  set(_inc "${FW_ROOT}/include")
  set(_built "")

  # 1. The portable arms first, so everything else can link them.
  foreach(_c IN LISTS FW_COMPONENTS)
    if(TARGET hml_${_c})
      list(APPEND _built ${_c}) # component name, not target name
      continue()                # already provided by an enclosing hml build
    endif()
    if(_c STREQUAL "port_posix" AND NOT FW_POSIX)
      continue()
    endif()

    hml_component_spec(${_c} _srcs _links _priv)
    if(NOT _srcs)
      continue()
    endif()

    add_library(hml_${_c} STATIC ${_srcs})
    set_target_properties(hml_${_c} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)
    # PUBLIC, not INTERFACE: the component's own sources include "hml/*.h" too.
    target_include_directories(hml_${_c} PUBLIC "${_inc}")
    if(_priv)
      target_include_directories(hml_${_c} PRIVATE ${_priv})
    endif()
    # No HML_NO_POSIX unless the consumer really is a POSIX platform, and never
    # the build machine's platform: a cross-compiled firmware must not inherit
    # the host's __linux__.
    if(NOT FW_POSIX)
      target_compile_definitions(hml_${_c} INTERFACE HML_NO_POSIX)
    endif()
    add_library(hml::hml_${_c} ALIAS hml_${_c})
    list(APPEND _built ${_c})
  endforeach()

  # 2. Wire the interface chain. port_posix is only ever linked when it exists.
  foreach(_c IN LISTS _built)
    if(_c STREQUAL "port_posix")
      set(_links "")
    else()
      hml_component_spec(${_c} _unused _links _unused2)
    endif()
    set(_libs "")
    foreach(_d IN LISTS _links)
      if(TARGET hml_${_d})
        list(APPEND _libs hml_${_d})
      endif()
    endforeach()
    set_target_properties(hml_${_c} PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
  endforeach()

  # 3. Optional shared arm, same names, .so artifacts.
  if(FW_SHARED)
    foreach(_c IN LISTS _built)
      if(TARGET hml_${_c}_shared)
        continue()
      endif()
      hml_component_spec(${_c} _srcs _links _priv)
      if(NOT _srcs)
        continue()
      endif()
      add_library(hml_${_c}_shared SHARED ${_srcs})
      set_target_properties(hml_${_c}_shared PROPERTIES
        OUTPUT_NAME hml_${_c}
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
      target_include_directories(hml_${_c}_shared PUBLIC "${_inc}")
      # PRIVATE for the library's own sources, INTERFACE for its consumers, so a
      # program linked against the shared arm reports HML_LINKAGE_NAME="shared"
      # instead of the "static" it would otherwise see.
      target_compile_definitions(hml_${_c}_shared PRIVATE HML_BUILDING_DLL)
      target_compile_definitions(hml_${_c}_shared INTERFACE HML_PARTITION_SHARED)
      if(NOT FW_POSIX)
        target_compile_definitions(hml_${_c}_shared PRIVATE HML_NO_POSIX)
      endif()
      if(_priv)
        target_include_directories(hml_${_c}_shared PRIVATE ${_priv})
      endif()
      add_library(hml::hml_${_c}_shared ALIAS hml_${_c}_shared)
    endforeach()
    foreach(_c IN LISTS _built)
      if(NOT TARGET hml_${_c}_shared)
        continue()
      endif()
      if(_c STREQUAL "port_posix")
        set(_links "")
      else()
        hml_component_spec(${_c} _unused _links _unused2)
      endif()
      set(_libs "")
      foreach(_d IN LISTS _links)
        if(TARGET hml_${_d}_shared)
          list(APPEND _libs hml_${_d}_shared)
        endif()
      endforeach()
      set_target_properties(hml_${_c}_shared PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
    endforeach()
  endif()

  # 4. The link-time port contract. Compile this into the firmware's own port
  #    layer; every entry is a real function address, so a missing symbol is a
  #    link error that names it.
  set(_contract_dir "${CMAKE_CURRENT_BINARY_DIR}/hml_generated")
  file(MAKE_DIRECTORY "${_contract_dir}")
  set(hml_firmware_contract_source "${_contract_dir}/hml_port_contract.c"
      CACHE INTERNAL "generated hml port-contract source" FORCE)
  configure_file("${HML_FIRMWARE_TEMPLATE_DIR}/hml_port_contract.c.in"
                 "${hml_firmware_contract_source}" @ONLY)

  message(STATUS "hml: firmware drop-in built [components: ${_built}] "
                 "shared=${FW_SHARED} posix_port=${FW_POSIX}")
endfunction()
