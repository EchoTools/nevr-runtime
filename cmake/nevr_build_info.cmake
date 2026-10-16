# nevr_build_info.cmake — generated/nevr_build_info.h for the Quest sentinel.
#
# The version comes from git (set_project_version_from_git.cmake). The build carries no feature list: what the
# sentinel does follows from the embedded service endpoints and keys and from config.yaml (docs/reference/example-config.yaml).

include("${CMAKE_CURRENT_LIST_DIR}/set_project_version_from_git.cmake")

function(nevr_generate_build_info root)
  set_project_version_from_git()
  set(NEVR_BUILD_VERSION "${PROJECT_VERSION}")
  set(NEVR_BUILD_COMMIT "${GIT_COMMIT_HASH}")
  configure_file("${root}/cmake/nevr_build_info.h.in"
                 "${CMAKE_BINARY_DIR}/generated/nevr_build_info.h" @ONLY)
  message(STATUS "nevr build info: version ${NEVR_BUILD_VERSION}")
endfunction()
