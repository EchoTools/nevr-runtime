foreach(required_var IN ITEMS ARCHIVE_TAR ARCHIVE_ZIP)
  if(NOT DEFINED ${required_var})
    message(FATAL_ERROR "VerifyDistribution requires ${required_var}")
  endif()
  if(NOT EXISTS "${${required_var}}")
    message(FATAL_ERROR "Required distribution archive is missing: ${${required_var}}")
  endif()
  file(SIZE "${${required_var}}" archive_size)
  if(archive_size EQUAL 0)
    message(FATAL_ERROR "Required distribution archive is empty: ${${required_var}}")
  endif()
endforeach()

execute_process(
  COMMAND tar --zstd -tf "${ARCHIVE_TAR}"
  RESULT_VARIABLE tar_result
  OUTPUT_VARIABLE tar_entries
  ERROR_VARIABLE tar_error
)
if(NOT tar_result EQUAL 0)
  message(FATAL_ERROR "Cannot list distribution tar.zst ${ARCHIVE_TAR}: ${tar_error}")
endif()
foreach(required_artifact IN ITEMS BugSplat64.dll echovr_server.exe)
  string(REPLACE "." "\\." escaped_artifact "${required_artifact}")
  if(NOT tar_entries MATCHES "(^|/)${escaped_artifact}([\r\n]|$)")
    message(FATAL_ERROR "Distribution tar.zst lacks ${required_artifact}: ${ARCHIVE_TAR}")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar tf "${ARCHIVE_ZIP}"
  RESULT_VARIABLE zip_result
  OUTPUT_VARIABLE zip_entries
  ERROR_VARIABLE zip_error
)
if(NOT zip_result EQUAL 0)
  message(FATAL_ERROR "Cannot list distribution zip ${ARCHIVE_ZIP}: ${zip_error}")
endif()
foreach(required_artifact IN ITEMS BugSplat64.dll echovr_server.exe)
  string(REPLACE "." "\\." escaped_artifact "${required_artifact}")
  if(NOT zip_entries MATCHES "(^|/)${escaped_artifact}([\r\n]|$)")
    message(FATAL_ERROR "Distribution zip lacks ${required_artifact}: ${ARCHIVE_ZIP}")
  endif()
endforeach()
