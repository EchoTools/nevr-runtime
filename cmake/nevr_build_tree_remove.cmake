# nevr_build_tree_remove(<path> <allowed_parent>)
#
# Removes the build tree <path> recursively, and only when its real path is strictly inside the real path of
# <allowed_parent> and no component between the two is a symbolic link. Otherwise it stops with FATAL_ERROR and removes
# nothing. A <path> that does not exist is not an error. This is the one checked recursive removal a CMake step may make
# (ORDER.md, "The manifest rule"): a build tree, a dist staging tree, a vcpkg package directory.
#
# Run as a script, it does one removal:
#   cmake -DNEVR_BT_PATH=<path> -DNEVR_BT_ALLOWED=<allowed parent> -P cmake/nevr_build_tree_remove.cmake
# or include this file and call the function at configure time.

function(nevr_build_tree_remove path allowed)
  get_filename_component(abs_allowed "${allowed}" ABSOLUTE)
  get_filename_component(abs_path "${path}" ABSOLUTE)
  if(IS_SYMLINK "${abs_allowed}")
    message(FATAL_ERROR "nevr_build_tree_remove: refused: ${abs_allowed} is a symbolic link")
  endif()
  if(NOT EXISTS "${abs_path}" AND NOT IS_SYMLINK "${abs_path}")
    return()
  endif()
  file(RELATIVE_PATH rel "${abs_allowed}" "${abs_path}")
  if(rel STREQUAL "" OR rel STREQUAL "." OR rel MATCHES "^\\.\\." OR IS_ABSOLUTE "${rel}")
    message(FATAL_ERROR "nevr_build_tree_remove: refused: ${abs_path} is not inside ${abs_allowed}")
  endif()
  set(current "${abs_allowed}")
  string(REPLACE "/" ";" parts "${rel}")
  foreach(part IN LISTS parts)
    set(current "${current}/${part}")
    if(IS_SYMLINK "${current}")
      message(FATAL_ERROR "nevr_build_tree_remove: refused: ${current} is a symbolic link")
    endif()
    if(NOT EXISTS "${current}")
      return()
    endif()
  endforeach()
  get_filename_component(real_allowed "${abs_allowed}" REALPATH)
  get_filename_component(real_path "${abs_path}" REALPATH)
  string(FIND "${real_path}" "${real_allowed}/" found)
  if(NOT found EQUAL 0)
    message(FATAL_ERROR "nevr_build_tree_remove: refused: real path ${real_path} is not strictly inside ${real_allowed}")
  endif()
  file(REMOVE_RECURSE "${abs_path}")
endfunction()

if(DEFINED NEVR_BT_PATH)
  if(NOT DEFINED NEVR_BT_ALLOWED)
    message(FATAL_ERROR "nevr_build_tree_remove.cmake: NEVR_BT_ALLOWED is required with NEVR_BT_PATH")
  endif()
  nevr_build_tree_remove("${NEVR_BT_PATH}" "${NEVR_BT_ALLOWED}")
endif()
