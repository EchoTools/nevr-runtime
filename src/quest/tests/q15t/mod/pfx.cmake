q15t_host_test(pfx_test TEST pfx_test.cpp SRC pfx plugin_log LIBS dl TIMEOUT 60)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(pfx)
