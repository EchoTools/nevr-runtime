# ctest fixture setup: make DIR, which must not exist yet. Tests do not delete trees (ORDER.md item 2): DIR lives under the
# run root the recipe made (NEVR_TEST_TMP), which is new for every run, and the recipe removes the root once at the end.
# An existing DIR is refused here, never cleared.
#   cmake -DDIR=<directory> -P fresh_dir.cmake
if(NOT DEFINED DIR OR "${DIR}" STREQUAL "")
  message(FATAL_ERROR "fresh_dir.cmake: DIR is not set")
endif()
if(EXISTS "${DIR}")
  message(FATAL_ERROR "fresh_dir.cmake: ${DIR} already exists. A fixture makes its folder new and never removes one. Re-run the configure step (cmake --preset quest-host-<group>), or run the group through just test-quest-<group>, which makes a new run root.")
endif()
file(MAKE_DIRECTORY "${DIR}")
