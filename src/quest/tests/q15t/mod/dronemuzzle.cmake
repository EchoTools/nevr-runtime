# host test of the drone muzzle module (plain functions, install, frame tick; the arm64 thunks are stood in for on the host), then the
# arm64 cross-compile of the thunks and the check that the thunks are in the object
q15t_host_test(dronemuzzle_test TEST dronemuzzle_test.cpp SRC dronemuzzle dronekill import_hook got_swap safe_read plugin_log LIBS dl pthread)
q15t_arm64_object(dronemuzzle)
if(Q15T_ARM64)
  # the two naked thunks are in the object: instruction words and relocations, at least 38 lines
  add_test(NAME dronemuzzle.arm64_thunks
           COMMAND "${Q15T_TESTS_DIR}/check/dronemuzzle_thunks.sh" "${Q15T_NDK_BIN}/llvm-objdump" "$<TARGET_OBJECTS:nevr_q15t_arm64_dronemuzzle>")
  set_tests_properties(dronemuzzle.arm64_thunks PROPERTIES LABELS dronemuzzle)
endif()
