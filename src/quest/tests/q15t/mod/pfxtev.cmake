q15t_host_test(pfxtev_test TEST pfxtev_test.cpp SRC pfxtev plugin_log LIBS dl TIMEOUT 30)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(pfxtev)
