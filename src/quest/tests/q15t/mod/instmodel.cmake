q15t_host_test(instmodel_test TEST instmodel_test.cpp SRC instmodel import_hook got_swap safe_read plugin_log
  OPTS -pthread LINK_OPTS -pthread LIBS dl TIMEOUT 30)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
q15t_arm64_object(instmodel)
