# set_project_version_from_git(): PROJECT_VERSION and its parts, from the committed VERSION file (#1935).
#
# VERSION (at the repo root) holds exactly X.Y.Z. A release is a commit that bumps VERSION, and its tag is made from
# that commit (RELEASING.md). PROJECT_VERSION is X.Y.Z; the development and release stamp that a build carries
# (X.Y.Z-dev.<distance>+<sha>, or X.Y.Z for a CI build on the tag) is written at build time by
# cmake/nevr_build_stamp.cmake, not here, so this function never reads git.
#
# Sets, in the caller's scope:
#   PROJECT_VERSION, PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH  from VERSION
#   PROJECT_VERSION_TWEAK  "0": the Windows file version (cmake/nevr_version_info.cmake) is X.Y.Z.0
# and makes VERSION a configure dependency, so a version bump re-runs the configure.

# The optional argument is the repository root that holds VERSION. A sub-project whose own source directory is not the
# root (the Quest sub-project at src/quest) passes it; without one the root is CMAKE_SOURCE_DIR (#1935).
function(set_project_version_from_git)
  if(ARGC GREATER 0)
    set(_version_root "${ARGV0}")
  else()
    set(_version_root "${CMAKE_SOURCE_DIR}")
  endif()
  set(_version_file "${_version_root}/VERSION")
  if(NOT EXISTS "${_version_file}")
    message(FATAL_ERROR "set_project_version_from_git: ${_version_file} is missing: the committed version, exactly X.Y.Z")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_version_file}")

  file(READ "${_version_file}" _version_raw)
  string(REGEX REPLACE "\n$" "" _version "${_version_raw}")
  if(NOT _version MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
      "set_project_version_from_git: ${_version_file} must hold exactly X.Y.Z (for example 6.15.1), found '${_version}'")
  endif()

  set(PROJECT_VERSION "${_version}" PARENT_SCOPE)
  set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(PROJECT_VERSION_TWEAK "0" PARENT_SCOPE)
  message(STATUS "Project version: ${_version} (VERSION)")
endfunction()
