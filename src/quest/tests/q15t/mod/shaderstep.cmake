q15t_host_test(shaderstep_test TEST shaderstep_test.cpp SRC shaderstep import_hook got_swap safe_read plugin_log LIBS dl TIMEOUT 60)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(shaderstep)
