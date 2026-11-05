q15t_host_test(equiprefire_test TEST equiprefire_test.cpp SRC equiprefire import_hook got_swap safe_read plugin_log LIBS dl pthread TIMEOUT 60)
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors); the shared Debouncer must be constant-initialised
q15t_arm64_object(equiprefire)
