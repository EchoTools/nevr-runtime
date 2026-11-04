# the `ecfx` module: the NEUTRON queue rules and the attach / tick / gate against a stand-in wall lib (soname libac4e9374a146d3a4.so)
# loaded for real, in two runs: a complete lib and one that lacks the burst entry.
if(NOT Q15T_ARM64)
  foreach(variant full missing)
    add_library(ecfx_stub_${variant} SHARED "${Q15T_TESTS_DIR}/ecfx_wall_stub.cpp")
    set_target_properties(ecfx_stub_${variant} PROPERTIES OUTPUT_NAME ac4e9374a146d3a4 POSITION_INDEPENDENT_CODE ON
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ecfx_${variant}")
    target_compile_options(ecfx_stub_${variant} PRIVATE -g -O1)
    target_link_options(ecfx_stub_${variant} PRIVATE -Wl,-soname,libac4e9374a146d3a4.so)
  endforeach()
  target_compile_definitions(ecfx_stub_missing PRIVATE STUB_MISSING_BURST)
endif()
set(_ecfx_src ecfx ec import_hook got_swap safe_read plugin_log)
q15t_host_test(ecfx_test TEST ecfx_test.cpp SRC ${_ecfx_src} LINK_OPTS -rdynamic LIBS dl pthread
  DEPENDS ecfx_stub_full ecfx_stub_missing ARGS full "$<TARGET_FILE:ecfx_stub_full>")
q15t_extra_run(ecfx_test_missing ecfx_test missing "$<TARGET_FILE:ecfx_stub_missing>")

# the default-on build (CMake's default): no neutronfx.on needed
q15t_host_test(ecfx_test_defaulton TEST ecfx_test.cpp SRC ${_ecfx_src} LINK_OPTS -rdynamic LIBS dl pthread
  DEFS Q15T_ECFX_DEFAULT_ON=1 DEPENDS ecfx_stub_full ARGS full "$<TARGET_FILE:ecfx_stub_full>")
