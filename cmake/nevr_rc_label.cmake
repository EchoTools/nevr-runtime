# nevr_rc_label.cmake — release-candidate label for the version string of a build.
#
# set_project_version_from_git() reads the nearest v* tag (the single source of X.Y.Z) and leaves
# PROJECT_VERSION as a development version, "X.Y.Z-dev+<distance>.<sha>", except exactly on a plain
# vX.Y.Z tag. A build configured with -DNEVR_RC_LABEL=rc.<N> gets "X.Y.Z-rc.<N>+0.<sha>" ONLY when it
#   - runs in GitHub Actions (GITHUB_ACTIONS=true),
#   - on a tag ref (GITHUB_REF_TYPE=tag) named v<x.y.z>-rc.<N> (GITHUB_REF_NAME), the same N as the label,
#   - and is exactly on that tag: the nearest tag is the rc tag with the same N, at distance 0.
# Any other build — a local tree, a branch, another tag, a different N, a child of the tag — keeps the
# development version and says so, so a running client or a packaged file can never present a local build
# as a release candidate. This macro is the one place a label becomes a version: the Windows DLL
# (CMakeLists.txt) and the Quest sentinel (nevr_build_info.cmake) both call it.
# NEVR_RC_LABEL=dev states the development version explicitly (what `just package-dev` passes).
# An unset label leaves PROJECT_VERSION exactly as set_project_version_from_git computed it.

set(NEVR_RC_LABEL "" CACHE STRING "Release-candidate label (rc.<N>); honoured only on a CI build of the matching v<x.y.z>-rc.<N> tag, else a dev version is stamped")

# True when this process is a GitHub Actions build of the tag v<x.y.z>-rc.<number>, checked out exactly
# (reads NEVR_GIT_TAG_RC / NEVR_GIT_DISTANCE from set_project_version_from_git).
function(_nevr_rc_tag_build number result)
  set(${result} FALSE PARENT_SCOPE)
  if(NOT "$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    return()
  endif()
  if(NOT "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    return()
  endif()
  if(NOT "$ENV{GITHUB_REF_NAME}" MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+-rc\\.([0-9]+)$")
    return()
  endif()
  if(NOT "${CMAKE_MATCH_1}" STREQUAL "${number}")
    return()
  endif()
  if(NOT "${NEVR_GIT_TAG_RC}" STREQUAL "${number}" OR NOT "${NEVR_GIT_DISTANCE}" STREQUAL "0")
    return()
  endif()
  set(${result} TRUE PARENT_SCOPE)
endfunction()

# Call after set_project_version_from_git(). Sets PROJECT_VERSION in the caller's scope.
macro(nevr_apply_rc_label)
  if(NOT NEVR_RC_LABEL STREQUAL "")
    set(_nevr_rc_requested TRUE)
    if(NEVR_RC_LABEL STREQUAL "dev")
      set(_nevr_rc_requested FALSE)
      set(_nevr_rc_number "")
      set(_nevr_rc_is_tag_build FALSE)
    elseif(NEVR_RC_LABEL MATCHES "^rc\\.([0-9]+)$")
      set(_nevr_rc_number "${CMAKE_MATCH_1}")
      _nevr_rc_tag_build("${_nevr_rc_number}" _nevr_rc_is_tag_build)
    else()
      message(FATAL_ERROR "NEVR_RC_LABEL must be dev or look like rc.<N>, got '${NEVR_RC_LABEL}'")
    endif()
    set(_nevr_rc_base "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
    if(_nevr_rc_is_tag_build)
      set(PROJECT_VERSION "${_nevr_rc_base}-${NEVR_RC_LABEL}+0.${GIT_COMMIT_HASH}")
      message(STATUS "Release candidate ${NEVR_RC_LABEL}: version ${PROJECT_VERSION}")
    else()
      if("${NEVR_GIT_DISTANCE}" STREQUAL "")
        set(_nevr_rc_distance "0")
      else()
        set(_nevr_rc_distance "${NEVR_GIT_DISTANCE}")
      endif()
      set(PROJECT_VERSION "${_nevr_rc_base}-dev+${_nevr_rc_distance}.${GIT_COMMIT_HASH}")
      if(_nevr_rc_requested)
        message(STATUS "NEVR_RC_LABEL ${NEVR_RC_LABEL} ignored: not a CI build exactly on the v<x.y.z>-rc.${_nevr_rc_number} tag; "
                       "stamping a development version: ${PROJECT_VERSION}")
      else()
        message(STATUS "Development build: version ${PROJECT_VERSION} (not a release candidate)")
      endif()
    endif()
  endif()
endmacro()
