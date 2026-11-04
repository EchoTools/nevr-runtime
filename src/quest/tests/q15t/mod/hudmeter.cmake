q15t_host_test(hudmeter_test TEST hudmeter_test.cpp SRC hudmeter import_hook got_swap safe_read plugin_log LIBS dl)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(hudmeter)
