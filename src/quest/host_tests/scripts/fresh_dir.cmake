# ctest fixture setup: make DIR exist and be empty.
#   cmake -DDIR=<directory> -P fresh_dir.cmake
if(NOT DEFINED DIR OR "${DIR}" STREQUAL "")
  message(FATAL_ERROR "fresh_dir.cmake: DIR is not set")
endif()
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
