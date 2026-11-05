# the `decals` module: the table parser, the per-system state machine and the wrappers over a stand-in engine (a fake bullet system,
# allocator, node, mesh and slot page), then the arm64 build with the NDK clang (no static initializer)
q15t_host_test(decals_test TEST decals_test.cpp SRC decals import_hook got_swap safe_read plugin_log LIBS dl pthread TIMEOUT 60)
q15t_arm64_object(decals)
