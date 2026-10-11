# nevr_rc_label.cmake — release-candidate label for the version string of a build.
#
# A build configured with -DNEVR_RC_LABEL=rc.<N> gets "-rc.<N>" in its version
# ("4.0.0-rc.3+1172.3a35e0b9", the commit hash always in the string) ONLY when it runs in GitHub
# Actions on a tag named v<x.y.z>-rc.<N> with the same N (GITHUB_ACTIONS, GITHUB_REF_TYPE,
# GITHUB_REF_NAME). Any other build — a local tree, a branch, another tag, a different N — stamps
# "<x.y.z>-dev+<tweak>.<commit>" and says so, so a running client or a packaged file can never
# present a local build as a release candidate. This macro is the one place a label becomes a
# version: the Windows DLL (CMakeLists.txt) and the Quest sentinel (nevr_build_info.cmake) both call it.
# An unset label leaves PROJECT_VERSION exactly as set_project_version_from_git computed it.

set(NEVR_RC_LABEL "" CACHE STRING "Release-candidate label (rc.<N>); honoured only on a CI build of the matching v<x.y.z>-rc.<N> tag, else a dev version is stamped")

# True when this process is a GitHub Actions build of the tag v<x.y.z>-rc.<number>.
function(_nevr_rc_tag_build number result)
  set(${result} FALSE PARENT_SCOPE)
  if(NOT "$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    return()
  endif()
  if(NOT "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    return()
  endif()
  if("$ENV{GITHUB_REF_NAME}" MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+-rc\\.([0-9]+)$")
    if("${CMAKE_MATCH_1}" STREQUAL "${number}")
      set(${result} TRUE PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Call after set_project_version_from_git(). Sets PROJECT_VERSION in the caller's scope.
macro(nevr_apply_rc_label)
  if(NOT NEVR_RC_LABEL STREQUAL "")
    if(NOT NEVR_RC_LABEL MATCHES "^rc\\.([0-9]+)$")
      message(FATAL_ERROR "NEVR_RC_LABEL must look like rc.<N>, got '${NEVR_RC_LABEL}'")
    endif()
    set(_nevr_rc_number "${CMAKE_MATCH_1}")
    _nevr_rc_tag_build("${_nevr_rc_number}" _nevr_rc_is_tag_build)
    if(_nevr_rc_is_tag_build)
      set(_nevr_stamp "${NEVR_RC_LABEL}")
    else()
      set(_nevr_stamp "dev")
    endif()
    string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+)" "\\1-${_nevr_stamp}" PROJECT_VERSION "${PROJECT_VERSION}")
    if(NOT PROJECT_VERSION MATCHES "[+]")
      set(PROJECT_VERSION "${PROJECT_VERSION}+${GIT_COMMIT_HASH}")
    endif()
    if(_nevr_rc_is_tag_build)
      message(STATUS "Release candidate ${NEVR_RC_LABEL}: version ${PROJECT_VERSION}")
    else()
      message(STATUS "NEVR_RC_LABEL ${NEVR_RC_LABEL} ignored: not a CI build on a v<x.y.z>-rc.${_nevr_rc_number} tag; "
                     "stamping a development version: ${PROJECT_VERSION}")
    endif()
  endif()
endmacro()
