# The build identity (version, commit, git describe) is defined for the source files that read it, not for the
# tree (#930).
#
# Every compile command that carries a definition is out of date when its value changes, and the commit changes
# with every commit (the describe string with every uncommitted edit: `-dirty`). A definition on every target made
# each new commit rebuild every translation unit and every precompiled header, for three files that read it.
#
#   nevr_build_identity_define(TARGET <target> SOURCES <file>... READS <what>... [LEGACY])
#       Defines on each of <file>... (absolute paths), as built by <target> and by every other target of the same
#       directory that compiles it, what the file reads. <what> is any of:
#           VERSION       NEVR_PROJECT_VERSION   (PROJECT_VERSION with LEGACY)
#           COMMIT_HASH   NEVR_GIT_COMMIT_HASH   (GIT_COMMIT_HASH with LEGACY)
#           COMMIT_FULL   NEVR_GIT_COMMIT_FULL
#           DESCRIBE      NEVR_GIT_DESCRIBE
#       LEGACY gives the unprefixed spellings the frozen src/legacy sources read; it takes VERSION and COMMIT_HASH only.
#
# A file that reads a macro must be listed here, and a header must not read one: the definition is on the source
# file, so a header's use would compile into files that were given none. tools/tests/test_build_identity_scope.py
# reads build.ninja and the sources and fails on either.

function(nevr_build_identity_define)
  cmake_parse_arguments(PARSE_ARGV 0 _bi "LEGACY" "TARGET" "SOURCES;READS")
  if(NOT _bi_TARGET OR NOT TARGET "${_bi_TARGET}")
    message(FATAL_ERROR "nevr_build_identity_define: TARGET '${_bi_TARGET}' is not a target")
  endif()
  if(NOT _bi_SOURCES OR NOT _bi_READS)
    message(FATAL_ERROR "nevr_build_identity_define(${_bi_TARGET}): SOURCES and READS are required")
  endif()
  if(_bi_LEGACY)
    set(_prefix "")
  else()
    set(_prefix "NEVR_")
  endif()
  set(_definitions)
  foreach(_what IN LISTS _bi_READS)
    if(_what STREQUAL "VERSION")
      list(APPEND _definitions "${_prefix}PROJECT_VERSION=\"${PROJECT_VERSION}\"")
    elseif(_what STREQUAL "COMMIT_HASH")
      list(APPEND _definitions "${_prefix}GIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
    elseif(_what STREQUAL "COMMIT_FULL" AND NOT _bi_LEGACY)
      list(APPEND _definitions "NEVR_GIT_COMMIT_FULL=\"${GIT_COMMIT_HASH_FULL}\"")
    elseif(_what STREQUAL "DESCRIBE" AND NOT _bi_LEGACY)
      list(APPEND _definitions "NEVR_GIT_DESCRIBE=\"${GIT_DESCRIBE}\"")
    else()
      message(FATAL_ERROR "nevr_build_identity_define(${_bi_TARGET}): cannot read '${_what}' "
                          "(VERSION, COMMIT_HASH, COMMIT_FULL, DESCRIBE; LEGACY takes the first two)")
    endif()
  endforeach()
  foreach(_source IN LISTS _bi_SOURCES)
    if(NOT IS_ABSOLUTE "${_source}")
      message(FATAL_ERROR "nevr_build_identity_define(${_bi_TARGET}): '${_source}' is not an absolute path")
    endif()
    set_property(SOURCE "${_source}" TARGET_DIRECTORY "${_bi_TARGET}" APPEND PROPERTY COMPILE_DEFINITIONS ${_definitions})
  endforeach()
endfunction()
