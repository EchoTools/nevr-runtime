# nevr_build_stamp.cmake: the build's version and commit, written into ONE generated translation unit (#1935).
#
# Run as a script, at configure time and at every build (the root CMakeLists.txt's nevr_build_stamp_refresh target):
#
#   cmake -DNEVR_STAMP_SOURCE_DIR=<repo> -DNEVR_STAMP_OUTPUT=<file.cpp> [-DNEVR_STAMP_GIT=<git>]
#         [-DNEVR_STAMP_PYTHON=<python3> -DNEVR_STAMP_APK_TOOL=<tools/quest_apk_version.py>] -P nevr_build_stamp.cmake
#
# The version is the committed VERSION file at the repo root: exactly X.Y.Z (a newline at the end is allowed).
# The commit is read when the script runs, so it is current at every build:
#   describe      git describe --always --dirty --broken --abbrev=12 (the source string; "-dirty" when the tree has
#                 uncommitted changes)
#   commit        git rev-parse HEAD (40 hex) and git rev-parse --short HEAD
#   distance      commits since the nearest reachable release tag: git describe --tags --long --abbrev=4
#                 --match "v[0-9]*" --exclude "v*-*" (the tag, git-describe(1); a pre-release tag is never a base)
#
# A release is a CI build exactly on the tag vX.Y.Z of VERSION: GITHUB_ACTIONS=true, GITHUB_REF_TYPE=tag,
# GITHUB_REF_NAME=vX.Y.Z, at distance 0 from that tag. It stamps exactly X.Y.Z, and it refuses to build when its
# describe string ends in -dirty. Every other build stamps X.Y.Z-dev.<distance>+<short commit>, which sorts after the
# release before it and before X.Y.Z (semver.org item 11.3). tools/build_version.py is the Python mirror of this rule
# (the packaging tools read it); tools/tests/test_build_stamp.py checks the two agree.
#
# The output is the one unit that carries the version and the commit (src/core/build_stamp.h declares its accessors).
# It is written to a temporary file and copied over the output only when the content differs, so the unit recompiles
# only when the version or the commit changes. With NEVR_STAMP_APK_TOOL it also carries the Quest versionCode of this
# build (tools/quest_apk_version.py code), the one the packaging writes into the APK's manifest; otherwise it is 0.

if(NOT DEFINED NEVR_STAMP_SOURCE_DIR OR NOT DEFINED NEVR_STAMP_OUTPUT)
  message(FATAL_ERROR "nevr_build_stamp: NEVR_STAMP_SOURCE_DIR and NEVR_STAMP_OUTPUT are required")
endif()
if(NOT NEVR_STAMP_GIT)
  set(NEVR_STAMP_GIT git)
endif()
set(_src "${NEVR_STAMP_SOURCE_DIR}")

# --- the version: VERSION, exactly X.Y.Z ---------------------------------------------------------------------------
set(_version_file "${_src}/VERSION")
if(NOT EXISTS "${_version_file}")
  message(FATAL_ERROR "nevr_build_stamp: ${_version_file} is missing: the committed version, exactly X.Y.Z")
endif()
file(READ "${_version_file}" _version_raw)
string(REGEX REPLACE "\n$" "" _version "${_version_raw}")
if(NOT _version MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
  message(FATAL_ERROR "nevr_build_stamp: ${_version_file} must hold exactly X.Y.Z (for example 6.15.1), found '${_version}'")
endif()
set(_version_major "${CMAKE_MATCH_1}")
set(_version_minor "${CMAKE_MATCH_2}")
set(_version_patch "${CMAKE_MATCH_3}")

# --- the commit and the nearest release tag -----------------------------------------------------------------------
execute_process(
  COMMAND "${NEVR_STAMP_GIT}" describe --tags --long --abbrev=4 --match "v[0-9]*" --exclude "v*-*"
  WORKING_DIRECTORY "${_src}"
  OUTPUT_VARIABLE _near
  ERROR_VARIABLE _near_err
  RESULT_VARIABLE _near_rc
  OUTPUT_STRIP_TRAILING_WHITESPACE)
# No reachable release tag is an error, not a guess: a build that cannot say what it is must not invent a version.
if(NOT _near_rc EQUAL 0)
  message(FATAL_ERROR
    "nevr_build_stamp: no v<X>.<Y>.<Z> tag is reachable from HEAD (git describe: ${_near_err}). Fetch the tags "
    "(git fetch --tags; in CI use fetch-depth: 0).")
endif()
# `<tag>-<distance>-g<sha>` (--long), with the tag in plain semver: a tag that is not (leading zeros, build metadata)
# fails here, never as a half-parsed version.
if(NOT _near MATCHES "^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)-([0-9]+)-g([0-9a-f]+)$")
  message(FATAL_ERROR
    "nevr_build_stamp: cannot parse `git describe` output '${_near}' (expected v<X>.<Y>.<Z>, then -<distance>-g<sha>)")
endif()
set(_tag "v${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
set(_distance "${CMAKE_MATCH_4}")

execute_process(
  COMMAND "${NEVR_STAMP_GIT}" describe --always --dirty --broken --abbrev=12
  WORKING_DIRECTORY "${_src}"
  OUTPUT_VARIABLE _describe
  RESULT_VARIABLE _describe_rc
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _describe_rc EQUAL 0 OR _describe STREQUAL "")
  message(FATAL_ERROR "nevr_build_stamp: git describe --always --dirty --broken failed in ${_src}")
endif()
execute_process(
  COMMAND "${NEVR_STAMP_GIT}" rev-parse HEAD
  WORKING_DIRECTORY "${_src}"
  OUTPUT_VARIABLE _commit_full
  RESULT_VARIABLE _commit_rc
  OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
  COMMAND "${NEVR_STAMP_GIT}" rev-parse --short HEAD
  WORKING_DIRECTORY "${_src}"
  OUTPUT_VARIABLE _commit_short
  OUTPUT_STRIP_TRAILING_WHITESPACE)
string(LENGTH "${_commit_full}" _commit_full_len)
if(NOT _commit_rc EQUAL 0 OR NOT _commit_full_len EQUAL 40 OR NOT _commit_full MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "nevr_build_stamp: git rev-parse HEAD did not print a 40-hex commit in ${_src}")
endif()
if(NOT _commit_short MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "nevr_build_stamp: git rev-parse --short HEAD printed '${_commit_short}' in ${_src}")
endif()

if(_describe MATCHES "-dirty$")
  set(_dirty TRUE)
else()
  set(_dirty FALSE)
endif()

# --- the stamp: a release or a development build -------------------------------------------------------------------
set(_release FALSE)
if("$ENV{GITHUB_ACTIONS}" STREQUAL "true" AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag"
   AND "$ENV{GITHUB_REF_NAME}" STREQUAL "v${_version}" AND "${_tag}" STREQUAL "v${_version}" AND _distance STREQUAL "0")
  set(_release TRUE)
endif()
if(_release)
  if(_dirty)
    message(FATAL_ERROR
      "nevr_build_stamp: a release build of v${_version} refuses to build: its describe string is '${_describe}', "
      "which ends in -dirty. Commit or stash the uncommitted changes, then build the tag again.")
  endif()
  set(_stamp "${_version}")
else()
  set(_stamp "${_version}-dev.${_distance}+${_commit_short}")
endif()

# --- the Quest versionCode of this build (optional) ----------------------------------------------------------------
set(_apk_code 0)
if(DEFINED NEVR_STAMP_APK_TOOL AND NOT "${NEVR_STAMP_APK_TOOL}" STREQUAL "")
  if(NOT DEFINED NEVR_STAMP_PYTHON OR "${NEVR_STAMP_PYTHON}" STREQUAL "")
    set(NEVR_STAMP_PYTHON python3)
  endif()
  execute_process(
    COMMAND "${NEVR_STAMP_PYTHON}" -I "${NEVR_STAMP_APK_TOOL}" code "${_stamp}"
    OUTPUT_VARIABLE _apk_out
    ERROR_VARIABLE _apk_err
    RESULT_VARIABLE _apk_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _apk_rc EQUAL 0 OR NOT _apk_out MATCHES "^[0-9]+$")
    message(FATAL_ERROR "nevr_build_stamp: the Quest versionCode of '${_stamp}' is not a number "
                        "(${NEVR_STAMP_APK_TOOL} code: ${_apk_err})")
  endif()
  set(_apk_code "${_apk_out}")
endif()

# --- the unit ------------------------------------------------------------------------------------------------------
# The identity literal: a leading NUL so the string is NUL-bounded in the file whatever the linker places before it,
# and the same shape the identity has always had: NEVR-BUILD <version> <commit40>.
set(_text "// GENERATED by cmake/nevr_build_stamp.cmake at build time: do not edit (the next build rewrites it).\n")
string(APPEND _text "// The one translation unit that carries this build's version and commit (src/core/build_stamp.h).\n")
string(APPEND _text "#include \"core/build_stamp.h\"\n\n")
string(APPEND _text "namespace nevr_build_stamp {\nnamespace {\n")
string(APPEND _text "const char kIdentityArray[] = \"\\0NEVR-BUILD ${_stamp} ${_commit_full}\";\n")
string(APPEND _text "const char kVersion[] = \"${_version}\";\n")
string(APPEND _text "const char kProjectVersion[] = \"${_stamp}\";\n")
string(APPEND _text "const char kCommitShort[] = \"${_commit_short}\";\n")
string(APPEND _text "const char kCommitFull[] = \"${_commit_full}\";\n")
string(APPEND _text "const char kDescribe[] = \"${_describe}\";\n")
if(_dirty)
  string(APPEND _text "const bool kDirty = true;\n")
else()
  string(APPEND _text "const bool kDirty = false;\n")
endif()
if(_release)
  string(APPEND _text "const bool kRelease = true;\n")
else()
  string(APPEND _text "const bool kRelease = false;\n")
endif()
string(APPEND _text "const int kApkVersionCode = ${_apk_code};\n")
string(APPEND _text "}  // namespace\n\n")
string(APPEND _text "const char* Version() noexcept { return kVersion; }\n")
string(APPEND _text "const char* ProjectVersion() noexcept { return kProjectVersion; }\n")
string(APPEND _text "const char* CommitShort() noexcept { return kCommitShort; }\n")
string(APPEND _text "const char* CommitFull() noexcept { return kCommitFull; }\n")
string(APPEND _text "const char* Describe() noexcept { return kDescribe; }\n")
string(APPEND _text "bool IsDirty() noexcept { return kDirty; }\n")
string(APPEND _text "bool IsRelease() noexcept { return kRelease; }\n")
string(APPEND _text "int ApkVersionCode() noexcept { return kApkVersionCode; }\n")
string(APPEND _text "const char* IdentityLiteral() noexcept { return kIdentityArray + 1; }\n")
string(APPEND _text "}  // namespace nevr_build_stamp\n")

get_filename_component(_out_dir "${NEVR_STAMP_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
set(_tmp "${NEVR_STAMP_OUTPUT}.tmp")
file(WRITE "${_tmp}" "${_text}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tmp}" "${NEVR_STAMP_OUTPUT}"
                RESULT_VARIABLE _copy_rc)
file(REMOVE "${_tmp}")
if(NOT _copy_rc EQUAL 0)
  message(FATAL_ERROR "nevr_build_stamp: cannot write ${NEVR_STAMP_OUTPUT}")
endif()
