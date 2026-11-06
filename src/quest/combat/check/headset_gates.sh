#!/usr/bin/env bash
# The headset gates' arm64 checks (docs/adr/0046 rule 2; src/quest/combat/headset_gates.h). Run by `just test-quest-combat headset_gates` after the host
# tests. Needs ANDROID_NDK_HOME (NDK r26d) and qemu-aarch64; each stops with its own message when it is missing, and Q15T_SKIP_QEMU=1 skips the run (and says so).
#   1. the two entries (src/quest/combat/headset_gates_entry.inc) compiled for arm64 are exactly the expected instructions, in this order;
#   2. the freestanding program src/quest/tests/headset_gates_qemu.cpp, which includes that very text, runs under qemu-aarch64.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
: "${ANDROID_NDK_HOME:?headset_gates.sh: ANDROID_NDK_HOME must name the NDK r26d directory}"
bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
cxx="$bin/aarch64-linux-android26-clang++"
objdump="$bin/llvm-objdump"
[ -x "$cxx" ] || { echo "headset_gates.sh: no NDK compiler at $cxx" >&2; exit 1; }
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
program="$scratch/headset_gates_qemu"
"$cxx" -O1 -fno-exceptions -fno-rtti -fno-stack-protector -Wall -Wextra -Werror -I "$root/src" -static -nostdlib -fuse-ld=lld \
  -o "$program" "$root/src/quest/tests/headset_gates_qemu.cpp"

got="$scratch/got"
for entry in nevr_combat_headset_tramp nevr_combat_party_tramp; do
  echo "$entry:" >> "$got"
  "$objdump" -d --disassemble-symbols="$entry" "$program" | sed -n -E 's/^ *[0-9a-f]+: +[0-9a-f]{8} +\t//p' | tr -s ' \t' ' ' >> "$got"
done
want="$scratch/want"
cat > "$want" <<'WANT'
nevr_combat_headset_tramp:
bti c
mov x0, sp
mov x1, x30
b 0x
nevr_combat_party_tramp:
bti c
mov x2, x30
b 0x
WANT
# a branch target is an address: keep the mnemonic and the destination's kind
sed -i -E 's/^(b) 0x[0-9a-f]+.*$/\1 0x/; s/^(b) 0x[0-9a-f]+ <(.*)>$/\1 0x/' "$got"
if ! diff -u "$want" "$got"; then
  echo "FAIL: the compiled entries differ from the expected instruction sequence"
  exit 1
fi
echo "headset_gates: the two entries are exactly the expected instructions"

if command -v qemu-aarch64 >/dev/null 2>&1; then
  qemu-aarch64 "$program" || { echo "FAIL: headset_gates_qemu"; exit 1; }
elif [ "${Q15T_SKIP_QEMU:-0}" = 1 ]; then
  echo "SKIPPED (Q15T_SKIP_QEMU=1): qemu-aarch64 not found, the entries were not run"
else
  echo "FAIL: qemu-aarch64 not found (Q15T_SKIP_QEMU=1 to skip explicitly)"
  exit 1
fi
