set(TOKMON_MISANS_ARCHIVE "" CACHE FILEPATH
  "Optional path to the official MiSans.zip archive (otherwise it is downloaded)")

set(TOKMON_MISANS_DOWNLOAD_URL
  "https://hyperos.mi.com/font-download/MiSans.zip"
  CACHE STRING "Official MiSans archive URL")

set(_TOKMON_MISANS_ARCHIVE_SHA256
  "b6aa1fc827035922612df8edf36e5609bca1c5441e25cd57572204569b7b81d9")
set(_TOKMON_MISANS_FONT_SHA256
  "0ddef90648998900175cfdca9a6f087a2544c182f130b0ad4f7e94a03a115e79")

function(tokmon_prepare_misans_font output_variable)
  set(_misans_root "${CMAKE_BINARY_DIR}/_deps/misans")
  file(MAKE_DIRECTORY "${_misans_root}")

  if(TOKMON_MISANS_ARCHIVE)
    get_filename_component(_misans_archive "${TOKMON_MISANS_ARCHIVE}"
      ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${_misans_archive}")
      message(FATAL_ERROR
        "TOKMON_MISANS_ARCHIVE does not exist: ${_misans_archive}")
    endif()
  else()
    set(_misans_archive "${_misans_root}/MiSans.zip")
    set(_download_archive TRUE)
    if(EXISTS "${_misans_archive}")
      file(SHA256 "${_misans_archive}" _cached_archive_sha256)
      if(_cached_archive_sha256 STREQUAL _TOKMON_MISANS_ARCHIVE_SHA256)
        set(_download_archive FALSE)
      endif()
    endif()
    if(_download_archive)
      message(STATUS "Downloading the official MiSans font archive")
      file(DOWNLOAD
        "${TOKMON_MISANS_DOWNLOAD_URL}"
        "${_misans_archive}"
        EXPECTED_HASH "SHA256=${_TOKMON_MISANS_ARCHIVE_SHA256}"
        TLS_VERIFY ON
        STATUS _misans_download_status)
      list(GET _misans_download_status 0 _misans_download_code)
      list(GET _misans_download_status 1 _misans_download_message)
      if(NOT _misans_download_code EQUAL 0)
        message(FATAL_ERROR
          "Unable to download official MiSans archive: ${_misans_download_message}")
      endif()
    endif()
  endif()

  file(SHA256 "${_misans_archive}" _misans_archive_sha256)
  if(NOT _misans_archive_sha256 STREQUAL _TOKMON_MISANS_ARCHIVE_SHA256)
    message(FATAL_ERROR
      "MiSans archive checksum mismatch: ${_misans_archive}")
  endif()

  set(_misans_font "${_misans_root}/MiSansVF.ttf")
  set(_extract_font TRUE)
  if(EXISTS "${_misans_font}")
    file(SHA256 "${_misans_font}" _cached_font_sha256)
    if(_cached_font_sha256 STREQUAL _TOKMON_MISANS_FONT_SHA256)
      set(_extract_font FALSE)
    endif()
  endif()

  if(_extract_font)
    set(_misans_extract_root "${_misans_root}/archive")
    file(ARCHIVE_EXTRACT
      INPUT "${_misans_archive}"
      DESTINATION "${_misans_extract_root}"
      PATTERNS "MiSans/*/MiSansVF.ttf")
    file(GLOB_RECURSE _misans_font_candidates
      LIST_DIRECTORIES FALSE "${_misans_extract_root}/MiSansVF.ttf")
    list(LENGTH _misans_font_candidates _misans_font_candidate_count)
    if(NOT _misans_font_candidate_count EQUAL 1)
      message(FATAL_ERROR
        "Official MiSans archive did not contain exactly one MiSansVF.ttf")
    endif()
    list(GET _misans_font_candidates 0 _misans_extracted_font)
    file(COPY_FILE "${_misans_extracted_font}" "${_misans_font}"
      ONLY_IF_DIFFERENT)
  endif()

  file(SHA256 "${_misans_font}" _misans_font_sha256)
  if(NOT _misans_font_sha256 STREQUAL _TOKMON_MISANS_FONT_SHA256)
    message(FATAL_ERROR "MiSansVF.ttf checksum mismatch after extraction")
  endif()

  set(${output_variable} "${_misans_font}" PARENT_SCOPE)
endfunction()

