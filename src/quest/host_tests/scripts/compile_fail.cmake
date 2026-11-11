# ctest wrapper: build one object target that must fail to compile, and require that every `error:` line of the compiler's
# output matches the rule's own pattern (a second, unrelated error fails the check), with at least one such line.
#   cmake -DBUILD_DIR=<tree> -DTARGET=<object target> -DPATTERN=<regex> -DSOURCE=<snippet> -P compile_fail.cmake
# The patterns are GCC's wording, which is why the quest-host presets pin g++.
foreach(var BUILD_DIR TARGET PATTERN SOURCE)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "compile_fail.cmake: ${var} is not set")
  endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
if(rc EQUAL 0)
  message(FATAL_ERROR "test-quest-hooks: ${SOURCE} compiled, but must not")
endif()
# Fail closed: no diagnostics at all means the compiler never ran.
if(all STREQUAL "")
  message(FATAL_ERROR "test-quest-hooks: ${SOURCE} left no diagnostics; the gate did not run")
endif()
string(REGEX MATCHALL "[^\n]*error:[^\n]*" error_lines "${all}")
list(LENGTH error_lines total)
set(matched 0)
foreach(line IN LISTS error_lines)
  if(line MATCHES "${PATTERN}")
    math(EXPR matched "${matched} + 1")
  endif()
endforeach()
if(total LESS 1 OR NOT total EQUAL matched)
  message("${all}")
  message(FATAL_ERROR "test-quest-hooks: ${SOURCE}: ${total} error line(s), ${matched} match '${PATTERN}'; every error must be the rule's own")
endif()
message("${SOURCE}: ${total} error line(s), all match '${PATTERN}'")
