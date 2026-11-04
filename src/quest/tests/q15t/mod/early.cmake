# The early plugin: the real entry points (src/plugin_early.cpp) against the heaps test's fake game library; then the arm64 build with
# the repack's static-init check (the plugin file must have no constructors).
q15t_host_test(early_test TEST early_test.cpp SRC plugin_early heaps memsys import_hook got_swap safe_read plugin_log LIBS dl)
q15t_arm64_object(plugin_early)
