# the `ec` module: pure rules, install / wrappers / step / hide / selection against a fake libr15 image, the page as a shared-object
# export (tests/ec_page_test.cpp).
set(_ec_src ec import_hook got_swap safe_read plugin_log)
q15t_host_test(ec_test TEST ec_test.cpp SRC ${_ec_src} LINK_OPTS -rdynamic LIBS dl pthread)

if(NOT Q15T_ARM64)
  # the page as a symbol of a RTLD_LOCAL shared object, built with the plugin's flags (-fvisibility=hidden)
  set(_ec_sources "${Q15T_TESTS_DIR}/ec_page_shim.cpp")
  foreach(s IN LISTS _ec_src)
    list(APPEND _ec_sources "${Q15T_PLUGIN_SRC}/${s}.cpp")
  endforeach()
  add_library(nevr_q15t_ec_page_shim SHARED ${_ec_sources})
  set_target_properties(nevr_q15t_ec_page_shim PROPERTIES OUTPUT_NAME ec_page_test POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ec_page")
  target_compile_options(nevr_q15t_ec_page_shim PRIVATE ${Q15T_HOST_OPTIONS} -fvisibility=hidden)
  target_include_directories(nevr_q15t_ec_page_shim PRIVATE "${Q15T_PLUGIN_SRC}" "${Q15T_SDK_INCLUDE}")
  target_link_libraries(nevr_q15t_ec_page_shim PRIVATE dl pthread)
  add_test(NAME ec.ec_page_symbol
           COMMAND "${Q15T_TESTS_DIR}/check/dynamic_symbol.sh" "${CMAKE_NM}" "$<TARGET_FILE:nevr_q15t_ec_page_shim>" re33_ec_page)
  set_tests_properties(ec.ec_page_symbol PROPERTIES LABELS ec)
  add_dependencies(mod_ec nevr_q15t_ec_page_shim)
  add_library(nevr_q15t_ec_page_other SHARED "${Q15T_TESTS_DIR}/ec_page_other.cpp")
  set_target_properties(nevr_q15t_ec_page_other PROPERTIES OUTPUT_NAME ec_page_other POSITION_INDEPENDENT_CODE ON LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ec_page")
  target_compile_options(nevr_q15t_ec_page_other PRIVATE -g -O1 -Wall -Wextra -Werror)
  add_dependencies(mod_ec nevr_q15t_ec_page_other)
endif()
q15t_host_test(ec_page_test TEST ec_page_test.cpp NO_INCLUDES BASE_OPTS -g -O1 -Wall -Wextra -Werror LIBS dl
  DEPENDS nevr_q15t_ec_page_shim nevr_q15t_ec_page_other ARGS "$<TARGET_FILE:nevr_q15t_ec_page_shim>")
q15t_extra_run(ec_page_test_other ec_page_test "$<TARGET_FILE:nevr_q15t_ec_page_shim>" "$<TARGET_FILE:nevr_q15t_ec_page_other>")

# the build with default words (CMake's default): a fresh install with no switch file is on
q15t_host_test(ec_test_defaults TEST ec_test.cpp SRC ${_ec_src} LINK_OPTS -rdynamic LIBS dl pthread
  DEFS "Q15T_EC_DEFAULT_WORDS=\"types=VPTO equip hide\"")
