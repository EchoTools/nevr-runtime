# VERSIONINFO resource for the two Windows files this project ships, BugSplat64.dll and echovr_server.exe, so
# Explorer's Details tab names them (#850).
#
#   nevr_version_numeric(<out> <major> <minor> <patch> <distance>)
#       Sets <out> to "major,minor,patch,distance", the 4-part number a Windows version needs, after checking that
#       each part fits 16 bits. The parts are the nearest release tag's own numbers and the commit distance from it
#       (set_project_version_from_git.cmake: PROJECT_VERSION_MAJOR/MINOR/PATCH/TWEAK), so a release is X.Y.Z.0 and a
#       development build X.Y.Z.<distance>: the number sorts like the semver string, after the tag and before the
#       next release (a development build exactly on a tag, distance 0, has the tag's number).
#   nevr_add_version_info(<target> FILE_DESCRIPTION <text> ORIGINAL_FILENAME <file name> TYPE DLL|APP)
#       Fills cmake/nevr_version_info.rc.in from PROJECT_VERSION and adds it to <target>; the RC language is enabled
#       by the root CMakeLists.txt. tools/pe_version_info.py pins the result and `just verify` reads it back.

function(nevr_version_numeric out major minor patch distance)
  if("${distance}" STREQUAL "")
    set(distance 0)
  endif()
  foreach(_part IN ITEMS ${major} ${minor} ${patch} ${distance})
    if(NOT _part MATCHES "^(0|[1-9][0-9]*)$" OR _part GREATER 65535)
      message(FATAL_ERROR
        "nevr_version_numeric: '${_part}' is not a whole number up to 65535 (major=${major} minor=${minor} "
        "patch=${patch} distance=${distance}); a Windows file version holds four 16-bit parts")
    endif()
  endforeach()
  set(${out} "${major},${minor},${patch},${distance}" PARENT_SCOPE)
endfunction()

function(nevr_add_version_info target)
  if(NOT WIN32)
    return()
  endif()
  cmake_parse_arguments(PARSE_ARGV 1 _vi "" "FILE_DESCRIPTION;ORIGINAL_FILENAME;TYPE" "")
  foreach(_required FILE_DESCRIPTION ORIGINAL_FILENAME TYPE)
    if("${_vi_${_required}}" STREQUAL "")
      message(FATAL_ERROR "nevr_add_version_info(${target}): ${_required} is required")
    endif()
  endforeach()
  if(NOT CMAKE_RC_COMPILER)
    message(FATAL_ERROR "nevr_add_version_info(${target}): no resource compiler (CMAKE_RC_COMPILER); MinGW needs x86_64-w64-mingw32-windres")
  endif()
  if(_vi_TYPE STREQUAL "DLL")
    set(NEVR_VI_FILETYPE VFT_DLL)
  elseif(_vi_TYPE STREQUAL "APP")
    set(NEVR_VI_FILETYPE VFT_APP)
  else()
    message(FATAL_ERROR "nevr_add_version_info(${target}): TYPE is DLL or APP, not '${_vi_TYPE}'")
  endif()
  # A value is written between double quotes in the .rc: a quote or a backslash would change the resource.
  foreach(_text IN ITEMS "${_vi_FILE_DESCRIPTION}" "${_vi_ORIGINAL_FILENAME}" "${PROJECT_VERSION}")
    if(_text MATCHES "[\"\\\\]")
      message(FATAL_ERROR "nevr_add_version_info(${target}): '${_text}' contains a quote or a backslash")
    endif()
  endforeach()
  nevr_version_numeric(NEVR_VI_NUMERIC_VERSION "${PROJECT_VERSION_MAJOR}" "${PROJECT_VERSION_MINOR}"
                       "${PROJECT_VERSION_PATCH}" "${PROJECT_VERSION_TWEAK}")
  set(NEVR_VI_FILE_DESCRIPTION "${_vi_FILE_DESCRIPTION}")
  set(NEVR_VI_ORIGINAL_FILENAME "${_vi_ORIGINAL_FILENAME}")
  set(NEVR_VI_VERSION "${PROJECT_VERSION}")
  set(_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
  configure_file("${CMAKE_SOURCE_DIR}/cmake/nevr_version_info.rc.in" "${_rc}" @ONLY)
  target_sources(${target} PRIVATE "${_rc}")
endfunction()
