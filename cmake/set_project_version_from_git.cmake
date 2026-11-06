# The tool that names the base and the next release; the path is taken here, where the file is included from.
set(NEVR_NEXT_RELEASE_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/next_release_version.py")

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

  # The version of a build. Tags and commit messages are the only source: tools/next_release_version.py
  # (docs/standards/planning.md, "Release versions") names the base, the highest release tag reachable from
  # HEAD, and the release the commits since it lead to. Git describe's nearest tag is not the base: a release
  # tag on a branch that main does not contain (v6.16.5 beside main) is never the nearest, and a version
  # computed from the nearest tag falls below the release that is out.

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

  # A release is a CI build exactly on a release tag: GitHub Actions (GITHUB_ACTIONS=true), a tag ref
  # (GITHUB_REF_TYPE=tag) whose name is a plain vX.Y.Z (GITHUB_REF_NAME) and names the commit being built.
  # Its version is exactly X.Y.Z (v5.0.0 -> 5.0.0).
  set(_nevr_is_release FALSE)
  set(_nevr_ref "$ENV{GITHUB_REF_NAME}")
  if("$ENV{GITHUB_ACTIONS}" STREQUAL "true" AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag"
     AND _nevr_ref MATCHES "^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    set(_nevr_rel_major "${CMAKE_MATCH_1}")
    set(_nevr_rel_minor "${CMAKE_MATCH_2}")
    set(_nevr_rel_patch "${CMAKE_MATCH_3}")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --verify -q "${_nevr_ref}^{commit}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _nevr_ref_commit
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _nevr_commit_full
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nevr_ref_commit AND "${_nevr_ref_commit}" STREQUAL "${_nevr_commit_full}")
      set(_nevr_is_release TRUE)
    endif()
  endif()

  if(_nevr_is_release)
    set(_nevr_tag "${_nevr_ref}")
    set(_nevr_version "${_nevr_rel_major}.${_nevr_rel_minor}.${_nevr_rel_patch}")
    set(_nevr_distance 0)
    set(_nevr_num_major "${_nevr_rel_major}")
    set(_nevr_num_minor "${_nevr_rel_minor}")
    set(_nevr_num_patch "${_nevr_rel_patch}")
  else()
    # A development build is <next>-dev.<distance>+<sha>. <next> is the release the commits since the base lead
    # to (tools/next_release_version.py: a feat bumps the minor, a fix or perf the patch, a breaking change
    # the major), so the build sorts after the base and before that release (semver.org item 11.3: "a
    # pre-release version has lower precedence than the associated normal version"; item 10: build metadata is
    # ignored for precedence). <distance> counts the commits from the base to HEAD.
    # Exit 2 (no release tag is reachable, git cannot be read, or the computed tag already exists off this
    # branch) is an error, not a guess: a build that cannot say what it is must not invent a version (fetch the
    # tags; CI checks out with fetch-depth: 0). Exit 1 (no commit since the base is a feat, fix, perf or
    # breaking change) means no release is due, so the build is the base's next patch: the smallest version that
    # sorts above the base, and the release a fix would make.
    find_program(NEVR_PYTHON NAMES python3 python)
    if(NOT NEVR_PYTHON)
      message(FATAL_ERROR "set_project_version_from_git: python3 is needed to run ${NEVR_NEXT_RELEASE_TOOL}")
    endif()
    execute_process(
      COMMAND ${NEVR_PYTHON} -I "${NEVR_NEXT_RELEASE_TOOL}" --repo "${CMAKE_SOURCE_DIR}" --rev HEAD
      OUTPUT_VARIABLE _nevr_next_out
      ERROR_VARIABLE _nevr_next_err
      RESULT_VARIABLE _nevr_next_rc
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _nevr_next_rc EQUAL 0 AND NOT _nevr_next_rc EQUAL 1)
      string(REGEX MATCH "\"reason\": \"[^\"]*\"" _nevr_reason "${_nevr_next_err}")
      message(
        FATAL_ERROR
          "set_project_version_from_git: tools/next_release_version.py cannot name the next release "
          "(exit ${_nevr_next_rc}, ${_nevr_reason}). Fetch the tags (git fetch --tags; in CI use "
          "fetch-depth: 0).")
    endif()
    if(NOT _nevr_next_err MATCHES "\"base\": \"v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\"")
      message(FATAL_ERROR "set_project_version_from_git: no base tag in the output of next_release_version.py: ${_nevr_next_err}")
    endif()
    set(_nevr_base_major "${CMAKE_MATCH_1}")
    set(_nevr_base_minor "${CMAKE_MATCH_2}")
    set(_nevr_base_patch "${CMAKE_MATCH_3}")
    set(_nevr_tag "v${_nevr_base_major}.${_nevr_base_minor}.${_nevr_base_patch}")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-list --count "${_nevr_tag}..HEAD"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _nevr_distance
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nevr_next_rc EQUAL 0)
      string(REGEX MATCH "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _nevr_next_ok "${_nevr_next_out}")
      set(_nevr_next_major "${CMAKE_MATCH_1}")
      set(_nevr_next_minor "${CMAKE_MATCH_2}")
      set(_nevr_next_patch "${CMAKE_MATCH_3}")
    else()
      set(_nevr_next_major "${_nevr_base_major}")
      set(_nevr_next_minor "${_nevr_base_minor}")
      math(EXPR _nevr_next_patch "${_nevr_base_patch} + 1")
    endif()
    set(_nevr_version
        "${_nevr_next_major}.${_nevr_next_minor}.${_nevr_next_patch}-dev.${_nevr_distance}+${GIT_COMMIT_HASH}")
    # The Windows file version (PROJECT_VERSION_MAJOR/MINOR/PATCH, then the distance as TWEAK) is the
    # release just below <next> with the distance in the fourth part, so it sorts like the string:
    # tools/pe_version_info.py numeric_version holds the same rule.
    if(_nevr_next_patch GREATER 0)
      set(_nevr_num_major "${_nevr_next_major}")
      set(_nevr_num_minor "${_nevr_next_minor}")
      math(EXPR _nevr_num_patch "${_nevr_next_patch} - 1")
    elseif(_nevr_next_minor GREATER 0)
      set(_nevr_num_major "${_nevr_next_major}")
      math(EXPR _nevr_num_minor "${_nevr_next_minor} - 1")
      set(_nevr_num_patch 65535)
    else()
      math(EXPR _nevr_num_major "${_nevr_next_major} - 1")
      set(_nevr_num_minor 65535)
      set(_nevr_num_patch 65535)
    endif()
  endif()
  set(PROJECT_VERSION_MAJOR "${_nevr_num_major}" PARENT_SCOPE)
  set(PROJECT_VERSION_MINOR "${_nevr_num_minor}" PARENT_SCOPE)
  set(PROJECT_VERSION_PATCH "${_nevr_num_patch}" PARENT_SCOPE)
  set(PROJECT_VERSION_TWEAK "${_nevr_distance}" PARENT_SCOPE)
  set(PROJECT_VERSION "${_nevr_version}" PARENT_SCOPE)
  set(NEVR_GIT_DISTANCE "${_nevr_distance}" PARENT_SCOPE)

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
    message(STATUS "Development build: version ${_nevr_version} (not a release; the base is ${_nevr_tag}, ${_nevr_distance} commits back)")
  endif()
endfunction()
