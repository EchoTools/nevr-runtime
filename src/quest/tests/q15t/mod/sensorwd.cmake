q15t_host_test(sensorwd_test TEST sensorwd_test.cpp SRC sensorwd import_hook got_swap safe_read plugin_log
  OPTS -pthread LINK_OPTS -pthread LIBS dl TIMEOUT 30)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(sensorwd)
