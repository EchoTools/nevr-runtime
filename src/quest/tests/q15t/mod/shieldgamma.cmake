q15t_host_test(shieldgamma_test TEST shieldgamma_test.cpp SRC shieldgamma import_hook got_swap safe_read plugin_log LIBS dl)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(shieldgamma)
