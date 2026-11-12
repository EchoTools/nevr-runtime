# CDN_PC PC3: a map's file-backed resource set (patch/resource_sets.h, RegisterFileResourceSet; the registry's script
# waiver and the mount observer). Its tests run inside test_behavioral, which links resource_sets.cpp with
# NEVR_TEST_HOOKS, captures Log() and builds the fixture plugin whose set a map's set meets. Synthetic files only; no
# game file is read.
if(TARGET test_behavioral)
  get_filename_component(NEVR_PC3_TEST "${CMAKE_CURRENT_LIST_DIR}/../tests/test_resource_sets_file.cpp" ABSOLUTE)
  target_sources(test_behavioral PRIVATE "${NEVR_PC3_TEST}")
  set_source_files_properties("${NEVR_PC3_TEST}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
