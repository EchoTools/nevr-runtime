q15t_host_test(impactsnd_test TEST impactsnd_test.cpp SRC impactsnd import_hook got_swap safe_read plugin_log
  OPTS -pthread LINK_OPTS -pthread LIBS dl)
# the arm64 build: compiles with the NDK clang, no static initializer, no emulated thread-local variable
q15t_arm64_object(impactsnd)
