# nevr_build_info.cmake — generated/nevr_build_info.h for the Quest sentinel.
#
# The version comes from git (set_project_version_from_git.cmake). The build carries no feature list: what the
# sentinel does follows from the embedded service endpoints and keys and from config.yaml (docs/reference/example-config.yaml).

include("${CMAKE_CURRENT_LIST_DIR}/set_project_version_from_git.cmake")

function(nevr_generate_build_info root)
  set_project_version_from_git()
  set(NEVR_BUILD_VERSION "${PROJECT_VERSION}")
  set(NEVR_BUILD_COMMIT "${GIT_COMMIT_HASH}")
  # A development build: the only kind that downloads game data and carries the maintainers' headset probe (#758).
  # A build is NOT a development build when either
  #   - its version has no "-dev." part (a CI build exactly on the tag `git describe` chose), or
  #   - it is a CI build of a release tag: GitHub Actions on a tag ref whose name is vX.Y.Z or vX.Y.Z-rc.N and that
  #     tag points at the commit being built. This holds whatever other tags the commit carries. `git describe` prefers
  #     one tag when several sit on a commit (and ignores every rc tag), so a build of the other tag's ref, or of an
  #     rc tag, gets a "-dev." version although it is a release; the tag the CI ref names settles it here.
  # Everything else, including a tag ref that does not point at HEAD, is a development build.
  string(FIND "${NEVR_BUILD_VERSION}" "-dev." _nevr_dev_at)
  set(_nevr_release_tag_build FALSE)
  find_package(Git QUIET)
  if(GIT_FOUND AND "$ENV{GITHUB_ACTIONS}" STREQUAL "true" AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag"
     AND "$ENV{GITHUB_REF_NAME}" MATCHES "^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-rc\\.[1-9][0-9]*)?$")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --verify --quiet "refs/tags/$ENV{GITHUB_REF_NAME}^{commit}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _nevr_ref_commit
      RESULT_VARIABLE _nevr_ref_result
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _nevr_head_commit
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nevr_ref_result EQUAL 0 AND _nevr_ref_commit STREQUAL _nevr_head_commit)
      set(_nevr_release_tag_build TRUE)
    endif()
  endif()
  if(_nevr_dev_at EQUAL -1 OR _nevr_release_tag_build)
    set(NEVR_BUILD_IS_DEV "false")
  else()
    set(NEVR_BUILD_IS_DEV "true")
  endif()
  configure_file("${root}/cmake/nevr_build_info.h.in"
                 "${CMAKE_BINARY_DIR}/generated/nevr_build_info.h" @ONLY)
  message(STATUS "nevr build info: version ${NEVR_BUILD_VERSION}")
endfunction()
