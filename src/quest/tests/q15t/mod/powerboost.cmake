q15t_host_test(powerboost_test TEST powerboost_test.cpp SRC powerboost gamespace import_hook got_swap safe_read plugin_log LIBS dl pthread)
# the arm64 build and the naked thunk (the only asm of the plugin; this x86 host cannot run it natively)
q15t_arm64_object(powerboost)
if(Q15T_ARM64)
  # the thunk as compiled into the plugin: exactly these instructions, in this order (no other register, no other branch)
  add_test(NAME powerboost.arm64_thunk_disassembly
           COMMAND "${Q15T_TESTS_DIR}/check/powerboost_thunk.sh" "${Q15T_NDK_BIN}/llvm-objdump" "$<TARGET_OBJECTS:nevr_q15t_arm64_powerboost>"
                   "${Q15T_TESTS_DIR}/check/powerboost_thunk.want")
  # the thunk run under qemu-aarch64: freestanding (no libc, no plugin code), the same text src/powerboost.cpp includes
  add_executable(nevr_q15t_pb_thunk_qemu "${Q15T_TESTS_DIR}/pb_thunk_qemu.cpp")
  target_compile_options(nevr_q15t_pb_thunk_qemu PRIVATE -O1 -fno-exceptions -fno-rtti -fno-stack-protector -Wall -Wextra -Werror)
  target_include_directories(nevr_q15t_pb_thunk_qemu PRIVATE "${Q15T_PLUGIN_SRC}")
  set_target_properties(nevr_q15t_pb_thunk_qemu PROPERTIES OUTPUT_NAME pb_thunk_qemu)
  target_link_options(nevr_q15t_pb_thunk_qemu PRIVATE -static -nostdlib -fuse-ld=lld)
  add_dependencies(mod_powerboost nevr_q15t_pb_thunk_qemu)
  add_test(NAME powerboost.pb_thunk_qemu COMMAND "${Q15T_TESTS_DIR}/check/run_qemu.sh" "$<TARGET_FILE:nevr_q15t_pb_thunk_qemu>")
  set_tests_properties(powerboost.arm64_thunk_disassembly powerboost.pb_thunk_qemu PROPERTIES LABELS powerboost)
endif()
