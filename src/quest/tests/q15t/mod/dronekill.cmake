# host test of the drone kill / lock-on / gun / chase / shot module, then the arm64 cross-compile and the static-init check:
# the module compiles with the plugin's own flags and leaves no static constructor (the repack refuses a plugin with one).
q15t_host_test(dronekill_test TEST dronekill_test.cpp SRC dronekill import_hook got_swap safe_read plugin_log LIBS dl pthread)
q15t_arm64_object(dronekill)
