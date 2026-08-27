if(NOT DEFINED UI_DIRECTORY)
  message(FATAL_ERROR "UI_DIRECTORY is required")
endif()
if(NOT DEFINED FONT_PATH)
  message(FATAL_ERROR "FONT_PATH is required")
endif()

file(GLOB _slint_sources "${UI_DIRECTORY}/*.slint")
foreach(_source IN LISTS _slint_sources)
  file(READ "${_source}" _contents)
  string(REGEX MATCHALL "font-family:[^;\r\n]+" _font_declarations
    "${_contents}")
  foreach(_declaration IN LISTS _font_declarations)
    if(NOT _declaration MATCHES "font-family:[ \t]*\"MiSans VF\"[ \t]*$")
      message(FATAL_ERROR
        "Desktop font policy violation in ${_source}: ${_declaration}")
    endif()
  endforeach()
endforeach()

if(NOT EXISTS "${FONT_PATH}")
  message(FATAL_ERROR "Packaged MiSans font is missing: ${FONT_PATH}")
endif()
file(SHA256 "${FONT_PATH}" _font_sha256)
if(NOT _font_sha256 STREQUAL
   "0ddef90648998900175cfdca9a6f087a2544c182f130b0ad4f7e94a03a115e79")
  message(FATAL_ERROR "Packaged MiSans font checksum mismatch: ${FONT_PATH}")
endif()

