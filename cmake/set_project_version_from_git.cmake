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
    COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=4 --long --match "v[0-9]*" --exclude "v*-*"
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

  # Tags are the single source of the version, and a version is plain semver: vX.Y.Z.
  # `git describe --tags --long --match "v[0-9]*" --exclude "v*-*"` reports the nearest reachable release
  # tag by commit distance (git-describe(1): "the tag which has the fewest commits different from the
  # input commit-ish will be selected"), always in the long form `<tag>-<distance>-g<sha>` (--long:
  # "Always output the long format ... even when it matches a tag"). --exclude drops every tag with a
  # pre-release part (git-describe(1): "a tag will be considered when it matches at least one --match
  # pattern and does not match any of the --exclude patterns"), so every pre-release tag (a history tag with a
  # "-<part>") is never a base. When several release tags point at one commit, annotated tags are preferred over
  # lightweight ones and newer tag dates over older ones (same page). No reachable release tag is an
  # error, not a guess: a build that cannot say what it is must not invent a version (fetch the tags;
  # CI checks out with fetch-depth: 0).
  if(NOT GIT_PROJECT_VERSION_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "set_project_version_from_git: no v<X>.<Y>.<Z> tag is reachable from HEAD "
        "(git describe: ${GIT_PROJECT_VERSION_ERROR}). Fetch the tags (git fetch --tags; in CI use "
        "fetch-depth: 0).")
  endif()

  # Parse `v<X>.<Y>.<Z>-<distance>-g<sha>`. With if(MATCHES) the groups land in CMAKE_MATCH_<n> (CMake:
  # "All regular expression-related commands, including e.g. if(MATCHES), save subgroup matches in the
  # variables CMAKE_MATCH_<n>"); a string that does not match is an error here, never a half-parsed
  # version.
  if(NOT GIT_PROJECT_VERSION_STRING MATCHES "^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)-([0-9]+)-g([0-9a-f]+)$")
    message(
      FATAL_ERROR
        "set_project_version_from_git: cannot parse `git describe` output '${GIT_PROJECT_VERSION_STRING}' "
        "(expected v<X>.<Y>.<Z>, then -<distance>-g<sha>)")
  endif()
  set(PROJECT_VERSION_MAJOR_LOCAL "${CMAKE_MATCH_1}")
  set(PROJECT_VERSION_MINOR_LOCAL "${CMAKE_MATCH_2}")
  set(PROJECT_VERSION_PATCH_LOCAL "${CMAKE_MATCH_3}")
  set(PROJECT_VERSION_TWEAK_LOCAL "${CMAKE_MATCH_4}")
  set(_nevr_tag "v${PROJECT_VERSION_MAJOR_LOCAL}.${PROJECT_VERSION_MINOR_LOCAL}.${PROJECT_VERSION_PATCH_LOCAL}")

  # PROJECT_VERSION_{MAJOR,MINOR,PATCH} are the nearest tag's own numbers; TWEAK is the distance.
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

  # The version string. A release is a CI build exactly on a release tag: GitHub Actions
  # (GITHUB_ACTIONS=true), a tag ref (GITHUB_REF_TYPE=tag) whose name is the tag git describe found
  # (GITHUB_REF_NAME), at distance 0. Its version is exactly X.Y.Z (v5.0.0 -> 5.0.0). Every other build
  # is a development build, X.Y.(Z+1)-dev.<distance>+<sha>: the next patch with a pre-release part, so it
  # sorts after the tag it descends from and before the next release (semver.org item 11.3: "a
  # pre-release version has lower precedence than the associated normal version"; item 10: build
  # metadata is ignored for precedence).
  set(_nevr_is_release FALSE)
  if("$ENV{GITHUB_ACTIONS}" STREQUAL "true" AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag"
     AND "$ENV{GITHUB_REF_NAME}" STREQUAL "${_nevr_tag}" AND "${PROJECT_VERSION_TWEAK_LOCAL}" STREQUAL "0")
    set(_nevr_is_release TRUE)
  endif()
  if(_nevr_is_release)
    set(_nevr_version
        "${PROJECT_VERSION_MAJOR_LOCAL}.${PROJECT_VERSION_MINOR_LOCAL}.${PROJECT_VERSION_PATCH_LOCAL}")
  else()
    math(EXPR _nevr_next_patch "${PROJECT_VERSION_PATCH_LOCAL} + 1")
    set(_nevr_version
        "${PROJECT_VERSION_MAJOR_LOCAL}.${PROJECT_VERSION_MINOR_LOCAL}.${_nevr_next_patch}-dev.${PROJECT_VERSION_TWEAK_LOCAL}+${GIT_COMMIT_HASH}")
  endif()
  set(PROJECT_VERSION
      "${_nevr_version}"
      PARENT_SCOPE)
  set(NEVR_GIT_DISTANCE
      "${PROJECT_VERSION_TWEAK_LOCAL}"
      PARENT_SCOPE)

  set(GIT_COMMIT_HASH
      "${GIT_COMMIT_HASH}"
      PARENT_SCOPE)
  # The full 40-hex commit: with PROJECT_VERSION it forms the one identity literal embedded in the
  # binary (src/core/build_identity.cpp, "NEVR-BUILD <version> <commit40>").
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _nevr_commit_full
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(GIT_COMMIT_HASH_FULL
      "${_nevr_commit_full}"
      PARENT_SCOPE)

  if(_nevr_is_release)
    message(STATUS "Release ${_nevr_tag}: version ${_nevr_version}")
  else()
    message(STATUS "Development build: version ${_nevr_version} (not a release; the nearest tag is ${_nevr_tag})")
  endif()
endfunction()
