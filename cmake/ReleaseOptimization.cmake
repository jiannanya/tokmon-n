include(CheckIPOSupported)

option(TOKMON_OPTIMIZE_RELEASE_SIZE
  "Remove unused/duplicate code from Release binaries without size-first optimization" ON)

option(TOKMON_RELEASE_REMOVE_UNUSED_RTTI
  "Remove RTTI metadata from non-test Release targets that do not use it" ON)
if(BUILD_TESTING AND TOKMON_BUILD_TESTS)
  # Contract tests deliberately use dynamic_cast to distinguish native Lens
  # implementations. Production targets have no dynamic_cast/typeid sites.
  set(TOKMON_RELEASE_REMOVE_UNUSED_RTTI FALSE)
endif()

set(TOKMON_RELEASE_IPO_SUPPORTED FALSE)
if(TOKMON_OPTIMIZE_RELEASE_SIZE)
  check_ipo_supported(
    RESULT TOKMON_RELEASE_IPO_SUPPORTED
    OUTPUT TOKMON_RELEASE_IPO_ERROR
    LANGUAGES C CXX)
  if(NOT TOKMON_RELEASE_IPO_SUPPORTED)
    message(WARNING
      "Release IPO/LTO is unavailable; continuing with section garbage collection: "
      "${TOKMON_RELEASE_IPO_ERROR}")
  endif()
endif()

function(tokmon_optimize_release_target target)
  if(NOT TOKMON_OPTIMIZE_RELEASE_SIZE OR NOT TARGET "${target}")
    return()
  endif()

  if(TOKMON_RELEASE_IPO_SUPPORTED)
    set_property(TARGET "${target}" PROPERTY
      INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()

  # Keep the compiler's speed-oriented Release optimization level.  These
  # options only give the linker finer-grained units and remove unreachable or
  # byte-identical code/data; they do not trade runtime speed for file size.
  if(MSVC)
    target_compile_options("${target}" PRIVATE
      "$<$<CONFIG:Release>:/Gy>"
      "$<$<CONFIG:Release>:/Gw>"
      "$<$<CONFIG:Release>:/GF>"
      "$<$<CONFIG:Release>:/Zc:inline>"
      "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/Zc:throwingNew>"
      "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>,$<BOOL:${TOKMON_RELEASE_REMOVE_UNUSED_RTTI}>>:/GR->")
    get_target_property(target_type "${target}" TYPE)
    if(NOT target_type STREQUAL "STATIC_LIBRARY")
      target_link_options("${target}" PRIVATE
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF=10>")
    endif()
  else()
    target_compile_options("${target}" PRIVATE
      "$<$<CONFIG:Release>:-ffunction-sections>"
      "$<$<CONFIG:Release>:-fdata-sections>"
      "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>,$<BOOL:${TOKMON_RELEASE_REMOVE_UNUSED_RTTI}>>:-fno-rtti>")
    get_target_property(target_type "${target}" TYPE)
    if(NOT target_type STREQUAL "STATIC_LIBRARY")
      if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_options("${target}" PRIVATE
          "$<$<CONFIG:Release>:LINKER:/OPT:REF,/OPT:ICF>")
      elseif(APPLE)
        target_link_options("${target}" PRIVATE
          "$<$<CONFIG:Release>:LINKER:-dead_strip>")
      else()
        target_link_options("${target}" PRIVATE
          "$<$<CONFIG:Release>:LINKER:--gc-sections>")
      endif()
    endif()
  endif()
endfunction()
