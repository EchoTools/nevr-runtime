# Function to set project PROJECT_VERSION from Git
function(set_project_version_from_git)
  # Set default return values
  set(PROJECT_VERSION
      "1.0.0"
      PARENT_SCOPE)
  set(PROJECT_VERSION_MAJOR
      "1"
      PARENT_SCOPE)
  set(PROJECT_VERSION_MINOR
      "0"
      PARENT_SCOPE)
  set(PROJECT_VERSION_PATCH
      "0"
      PARENT_SCOPE)
  set(PROJECT_VERSION_TWEAK
      ""
      PARENT_SCOPE)
  set(GIT_COMMIT_HASH
      "unknown"
      PARENT_SCOPE)
  set(GIT_DESCRIBE
      "unknown"
      PARENT_SCOPE)
  set(NEVR_GIT_TAG_RC
      ""
      PARENT_SCOPE)
  set(NEVR_GIT_DISTANCE
      ""
      PARENT_SCOPE)

  # Try to find Git
  find_package(Git QUIET)
  if(NOT GIT_FOUND)
    message(
      WARNING "Git not found - using default PROJECT_VERSION ${PROJECT_VERSION}"
    )
    return()
  endif()

  # The version is computed here, at configure time. Re-run the configure whenever the checked-out
  # commit changes (a branch switch, a commit, an amend, a reset), so `cmake --build` after any of
  # them cannot embed the previous commit's version. HEAD changes on a switch, the HEAD reflog on
  # every one of those; the branch's loose ref is added when it exists. Only existing files are
  # listed: a missing dependency would make Ninja rerun the configure on every build.
  # The dirty flag in GIT_DESCRIBE is not tracked: editing a file does not re-run the configure.
  foreach(_git_state_path HEAD logs/HEAD)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute --git-path ${_git_state_path}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _git_state_file
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_git_state_file AND EXISTS "${_git_state_file}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_state_file}")
    endif()
  endforeach()
  execute_process(
    COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _git_head_ref
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_git_head_ref)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute --git-path ${_git_head_ref}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _git_ref_file
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_git_ref_file AND EXISTS "${_git_ref_file}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_ref_file}")
    endif()
  endif()

  # Get PROJECT_VERSION from git describe
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=4 --long --match "v*"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_PROJECT_VERSION_STRING
    ERROR_VARIABLE GIT_PROJECT_VERSION_ERROR
    RESULT_VARIABLE GIT_PROJECT_VERSION_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  # Get raw git describe output (with dirty flag) for GIT_DESCRIBE
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_DESCRIBE_RAW
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(GIT_DESCRIBE
      "${GIT_DESCRIBE_RAW}"
      PARENT_SCOPE)

  # Get commit hash for GIT_COMMIT_HASH
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  # Tags are the single source of the version. `git describe --tags --long --match "v*"` reports the
  # nearest reachable v* tag by commit distance (git-describe(1): "the tag which has the fewest commits
  # different from the input commit-ish will be selected"), always in the long form
  # `<tag>-<distance>-g<sha>` (--long: "Always output the long format ... even when it matches a tag").
  # When several tags point at the checked-out commit, annotated tags are preferred over lightweight ones
  # and newer tag dates over older ones (same page); both vX.Y.Z and vX.Y.Z-rc.N parse below, so either
  # may win. No reachable version tag is an error, not a guess: a build that cannot say what it is must
  # not invent a version (fetch the tags; CI checks out with fetch-depth: 0).
  if(NOT GIT_PROJECT_VERSION_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "set_project_version_from_git: no v<X>.<Y>.<Z> or v<X>.<Y>.<Z>-rc.<N> tag is reachable from HEAD "
        "(git describe: ${GIT_PROJECT_VERSION_ERROR}). Fetch the tags (git fetch --tags; in CI use "
        "fetch-depth: 0).")
  endif()

  # Parse `v<X>.<Y>.<Z>[-rc.<N>]-<distance>-g<sha>`. With if(MATCHES) the groups land in CMAKE_MATCH_<n>
  # (CMake: "All regular expression-related commands, including e.g. if(MATCHES), save subgroup matches
  # in the variables CMAKE_MATCH_<n>"); a string that does not match is an error here, never a
  # half-parsed version.
  if(NOT GIT_PROJECT_VERSION_STRING MATCHES
     "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-rc\\.([0-9]+))?-([0-9]+)-g([0-9a-f]+)$")
    message(
      FATAL_ERROR
        "set_project_version_from_git: cannot parse `git describe` output '${GIT_PROJECT_VERSION_STRING}' "
        "(expected v<X>.<Y>.<Z> or v<X>.<Y>.<Z>-rc.<N>, then -<distance>-g<sha>)")
  endif()
  set(PROJECT_VERSION_MAJOR_LOCAL "${CMAKE_MATCH_1}")
  set(PROJECT_VERSION_MINOR_LOCAL "${CMAKE_MATCH_2}")
  set(PROJECT_VERSION_PATCH_LOCAL "${CMAKE_MATCH_3}")
  set(NEVR_GIT_TAG_RC_LOCAL "${CMAKE_MATCH_5}")
  set(PROJECT_VERSION_TWEAK_LOCAL "${CMAKE_MATCH_6}")

  set(PROJECT_VERSION_MAJOR
      "${PROJECT_VERSION_MAJOR_LOCAL}"
      PARENT_SCOPE)
  set(PROJECT_VERSION_MINOR
      "${PROJECT_VERSION_MINOR_LOCAL}"
      PARENT_SCOPE)
  set(PROJECT_VERSION_PATCH
      "${PROJECT_VERSION_PATCH_LOCAL}"
      PARENT_SCOPE)
  set(PROJECT_VERSION_TWEAK
      "${PROJECT_VERSION_TWEAK_LOCAL}"
      PARENT_SCOPE)
  # What nevr_apply_rc_label (nevr_rc_label.cmake) needs to decide a release-candidate stamp: the number
  # of the rc tag the nearest tag is ("" for a plain vX.Y.Z tag) and the commit distance from it.
  set(NEVR_GIT_TAG_RC
      "${NEVR_GIT_TAG_RC_LOCAL}"
      PARENT_SCOPE)
  set(NEVR_GIT_DISTANCE
      "${PROJECT_VERSION_TWEAK_LOCAL}"
      PARENT_SCOPE)

  # The version string. Exactly on a plain release tag (vX.Y.Z, distance 0) it is the release version.
  # Everything else is a development version, X.Y.Z-dev+<distance>.<sha>, including a build exactly on an
  # rc tag: only nevr_apply_rc_label, in a CI build of that tag, turns that into X.Y.Z-rc.<N>+0.<sha>.
  if("${NEVR_GIT_TAG_RC_LOCAL}" STREQUAL "" AND "${PROJECT_VERSION_TWEAK_LOCAL}" STREQUAL "0")
    set(_nevr_version
        "${PROJECT_VERSION_MAJOR_LOCAL}.${PROJECT_VERSION_MINOR_LOCAL}.${PROJECT_VERSION_PATCH_LOCAL}+0.${GIT_COMMIT_HASH}")
  else()
    set(_nevr_version
        "${PROJECT_VERSION_MAJOR_LOCAL}.${PROJECT_VERSION_MINOR_LOCAL}.${PROJECT_VERSION_PATCH_LOCAL}-dev+${PROJECT_VERSION_TWEAK_LOCAL}.${GIT_COMMIT_HASH}")
  endif()
  set(PROJECT_VERSION
      "${_nevr_version}"
      PARENT_SCOPE)

  set(GIT_COMMIT_HASH
      "${GIT_COMMIT_HASH}"
      PARENT_SCOPE)

  # Output for debugging
  message(STATUS "Project PROJECT_VERSION set to: ${_nevr_version}")
endfunction()
