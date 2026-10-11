# nevr_build_info.cmake — generated/nevr_build_info.h for the Quest sentinel.
#
# The version comes from git (set_project_version_from_git.cmake) with the release-candidate
# label (nevr_rc_label.cmake). NEVR_QUEST_DEFAULT_FEATURES names the features the build turns on
# when no nevr-quest.json says otherwise: empty (the default) leaves every feature off, a release
# candidate sets the ones a tester needs so the APK logs in with no config file at all.

include("${CMAKE_CURRENT_LIST_DIR}/set_project_version_from_git.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/nevr_rc_label.cmake")

set(NEVR_QUEST_DEFAULT_FEATURES "" CACHE STRING
    "Comma-separated Quest features on by default with no config file (redirect,bridge,login,social); empty: all off")

# Fails the configure when `features` (a comma-separated list) names a feature the sentinel does not have.
# (`self_check` is not one: the self-checks are on in every build, #451.)
function(nevr_validate_default_features features_csv)
  set(known redirect bridge login social hwdump obb_skip)
  string(REPLACE "," ";" features "${features_csv}")
  foreach(feature IN LISTS features)
    if(NOT feature IN_LIST known)
      message(FATAL_ERROR "NEVR_QUEST_DEFAULT_FEATURES: unknown feature '${feature}' (known: ${known})")
    endif()
  endforeach()
endfunction()

function(nevr_generate_build_info root)
  set_project_version_from_git()
  nevr_apply_rc_label()
  nevr_validate_default_features("${NEVR_QUEST_DEFAULT_FEATURES}")
  set(NEVR_BUILD_VERSION "${PROJECT_VERSION}")
  set(NEVR_BUILD_COMMIT "${GIT_COMMIT_HASH}")
  set(NEVR_BUILD_DEFAULT_FEATURES "${NEVR_QUEST_DEFAULT_FEATURES}")
  configure_file("${root}/cmake/nevr_build_info.h.in"
                 "${CMAKE_BINARY_DIR}/generated/nevr_build_info.h" @ONLY)
  message(STATUS "nevr build info: version ${NEVR_BUILD_VERSION}, default features '${NEVR_BUILD_DEFAULT_FEATURES}'")
endfunction()
