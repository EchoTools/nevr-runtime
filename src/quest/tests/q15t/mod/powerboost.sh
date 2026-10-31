g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/powerboost_test.cpp" "$here/src/powerboost.cpp" "$here/src/gamespace.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" \
  "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" -o "$out/powerboost_test" -ldl -lpthread
"$out/powerboost_test"
# the arm64 build and the naked thunk (the only asm of the plugin; this x86 host cannot run it natively)
. "$tests/mod/pbhud_ndk.sh"
pbhud_ndk_check powerboost || return 1 2>/dev/null || exit 1
_ndk="${Q15T_NDK:-}"
_cxx=""
[ -z "$_ndk" ] || _cxx="$(ls "$_ndk"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android26-clang++ 2>/dev/null | { read -r l; echo "$l"; })"
if [ -n "$_cxx" ]; then
  _objdump="$(dirname "$_cxx")/llvm-objdump"
  # the thunk as compiled into the plugin: exactly these instructions, in this order (no other register, no other branch)
  "$_objdump" -dr --disassemble-symbols=q15t_pb_resolve_thunk "$out/powerboost.arm64.o" \
    | sed -n -E -e 's/^ *[0-9a-f]+: +[0-9a-f]{8} +\t//p' -e 's/^\t+[0-9a-f]+: +(R_AARCH64_[A-Z0-9_]+)\t(.*)$/    reloc \1 \2/p' | tr -s ' \t' ' ' > "$out/thunk.dis"
  cat > "$out/thunk.want" <<'WANT'
bti c
adrp x16, 0x0 <q15t_pb_resolve_thunk>
 reloc R_AARCH64_ADR_PREL_PG_HI21 q15t_pb_cone_ret
ldr x16, [x16]
 reloc R_AARCH64_LDST64_ABS_LO12_NC q15t_pb_cone_ret
cmp x30, x16
b.eq 0x20 <q15t_pb_resolve_thunk+0x20>
adrp x16, 0x0 <q15t_pb_resolve_thunk>
 reloc R_AARCH64_ADR_PREL_PG_HI21 q15t_pb_resolve_real
ldr x16, [x16]
 reloc R_AARCH64_LDST64_ABS_LO12_NC q15t_pb_resolve_real
br x16
stp x29, x30, [sp, #-0x10]!
mov x29, sp
add x1, sp, #0x10
mov x2, x24
bl 0x30 <q15t_pb_resolve_thunk+0x30>
 reloc R_AARCH64_CALL26 q15t_pb_cone_at
ldp x29, x30, [sp], #0x10
ret
WANT
  if ! diff -u "$out/thunk.want" "$out/thunk.dis"; then echo "FAIL: the compiled thunk differs from the expected instruction sequence"; exit 1; fi
  echo "arm64 thunk disassembly: matches the expected 15 instructions and 5 relocations"
  if command -v qemu-aarch64 >/dev/null 2>&1; then
    "$_cxx" -std=c++17 -O1 -fno-exceptions -fno-rtti -fno-stack-protector -Wall -Wextra -Werror -static -nostdlib -fuse-ld=lld \
      -I"$here/src" "$tests/pb_thunk_qemu.cpp" -o "$out/pb_thunk_qemu" || { echo "FAIL: pb_thunk_qemu build"; exit 1; }
    qemu-aarch64 "$out/pb_thunk_qemu" || { echo "FAIL: pb_thunk_qemu"; exit 1; }
  elif [ "${Q15T_SKIP_QEMU:-0}" = 1 ]; then echo "SKIPPED (Q15T_SKIP_QEMU=1): qemu-aarch64 not found, the thunk was not run"
  else echo "FAIL: qemu-aarch64 not found (Q15T_SKIP_QEMU=1 to skip explicitly)"; exit 1; fi
fi
