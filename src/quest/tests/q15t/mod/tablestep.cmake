q15t_host_test(tablestep_test TEST tablestep_test.cpp SRC tablestep import_hook got_swap safe_read plugin_log LIBS dl)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(tablestep)
