# nevr_rc_label.cmake — release-candidate label for the version string of a build.
#
# `just package-rc <N>` configures with -DNEVR_RC_LABEL=rc.<N>. The label becomes a semver
# pre-release of PROJECT_VERSION ("4.0.0-rc.3+1172.3a35e0b9") and the commit hash is always in the
# string, so a running client or a packaged file says which candidate and which commit it is.
# An unset label leaves PROJECT_VERSION exactly as set_project_version_from_git computed it.

set(NEVR_RC_LABEL "" CACHE STRING "Release-candidate label (rc.<N>) added to the version string; empty for a normal build")

# Call after set_project_version_from_git(). Sets PROJECT_VERSION in the caller's scope.
macro(nevr_apply_rc_label)
  if(NOT NEVR_RC_LABEL STREQUAL "")
    if(NOT NEVR_RC_LABEL MATCHES "^rc\\.[0-9]+$")
      message(FATAL_ERROR "NEVR_RC_LABEL must look like rc.<N>, got '${NEVR_RC_LABEL}'")
    endif()
    string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+)" "\\1-${NEVR_RC_LABEL}" PROJECT_VERSION "${PROJECT_VERSION}")
    if(NOT PROJECT_VERSION MATCHES "[+]")
      set(PROJECT_VERSION "${PROJECT_VERSION}+${GIT_COMMIT_HASH}")
    endif()
    message(STATUS "Release candidate ${NEVR_RC_LABEL}: version ${PROJECT_VERSION}")
  endif()
endmacro()
