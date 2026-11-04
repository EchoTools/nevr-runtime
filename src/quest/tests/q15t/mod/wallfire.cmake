q15t_host_test(wallfire_test TEST wallfire_test.cpp SRC wallfire bvh import_hook got_swap safe_read plugin_log
  INCLUDES "${Q15T_PLUGIN_SRC}" "${Q15T_TESTS_DIR}" "${Q15T_SDK_INCLUDE}" LIBS dl pthread TIMEOUT 60)
