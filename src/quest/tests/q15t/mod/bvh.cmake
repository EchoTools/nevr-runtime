q15t_host_test(bvh_core_test TEST bvh_core_test.cpp
  INCLUDES "${Q15T_PLUGIN_SRC}" "${Q15T_TESTS_DIR}" TIMEOUT 30)
q15t_host_test(bvh_test TEST bvh_test.cpp SRC bvh import_hook got_swap safe_read plugin_log
  INCLUDES "${Q15T_PLUGIN_SRC}" "${Q15T_TESTS_DIR}" "${Q15T_SDK_INCLUDE}" LIBS dl pthread TIMEOUT 60)
# the arm64 build: compiles with the NDK clang, no static initializer, no emulated thread-local variable
q15t_arm64_object(bvh)
