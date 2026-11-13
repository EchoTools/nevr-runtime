#!/usr/bin/env bash
# The headset gates' arm64 checks (docs/adr/0046 rule 2; src/quest/combat/headset_gates.h). Run by `just test-quest-combat headset_gates` after the host
# tests. Needs ANDROID_NDK_HOME (NDK r26d) and qemu-aarch64; each stops with its own message when it is missing, and NEVR_SKIP_QEMU=1 skips the run (and says so).
#   1. the two entries (src/quest/combat/headset_gates_entry.inc) compiled for arm64 are exactly the expected instructions, in this order;
#   2. each continuation, compiled for arm64 (-O2, as the sentinel is), loads the word the manager publishes with ldar or ldapr (a relaxed load fails);
#   3. the freestanding program src/quest/tests/headset_gates_qemu.cpp, which includes that very text, runs under qemu-aarch64.
# This script reads instructions and relocations; it reads no CFI. The frame sensor (tests/quest, on the built sentinel) is what checks the entries' frame description.
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

# The continuations (headset_gates.cpp) read the published next word with __atomic_load_n(..., __ATOMIC_ACQUIRE), and rule 7 of callback_thunk.h
# requires an acquire load of it at every call. Each continuation is checked on its own: the register its first `blr` calls through must have
# been loaded by ldar (or ldapr), not by a plain ldr (the shape #2216 found in an entry that read its word plainly). A relaxed load in either one fails.
object="$scratch/headset_gates.o"
"$cxx" -std=c++17 -O2 -fno-exceptions -fvisibility=hidden -Wall -Wextra -Werror -I "$root/src" -I "$root/src/quest/sentinel" -I "$root/sdk/include" \
  -c "$root/src/quest/combat/headset_gates.cpp" -o "$object"
for fn in nevr_combat_party_dispatch nevr_combat_headset_dispatch; do
  "$objdump" -d --no-show-raw-insn --disassemble-symbols="$fn" "$object" | python3 -I -c '
import re, sys
fn = sys.argv[1]
lines = [l.split("\t", 1)[1].strip() for l in sys.stdin if re.match(r"^\s+[0-9a-f]+:\s", l)]
for n, ins in enumerate(lines):
    m = re.match(r"blr\s+(x\d+)$", ins)
    if not m:
        continue
    reg = m.group(1)
    for prev in reversed(lines[:n]):
        if re.match(r"(ldar|ldapr)\s+" + reg + r",", prev):
            print(fn + ": the next word reaches " + reg + " by " + prev.split()[0])
            sys.exit(0)
        if re.match(r"(?!cbn?z|tbn?z|cmp|cmn|tst|str|stp|stlr)[a-z0-9.]+\s+" + reg + r",", prev) or re.match(r"ldp\s+(\w+, )?" + reg + r",", prev):
            print("FAIL: " + fn + ": the register the first blr calls through (" + reg + ") was last set by `" + prev + "`, not by ldar/ldapr")
            sys.exit(1)
    print("FAIL: " + fn + ": no instruction sets " + reg + " before its blr")
    sys.exit(1)
print("FAIL: " + fn + " has no blr to follow")
sys.exit(1)
' "$fn" || exit 1
done

if command -v qemu-aarch64 >/dev/null 2>&1; then
  qemu-aarch64 "$program" || { echo "FAIL: headset_gates_qemu"; exit 1; }
elif [ "${NEVR_SKIP_QEMU:-0}" = 1 ]; then
  echo "SKIPPED (NEVR_SKIP_QEMU=1): qemu-aarch64 not found, the entries were not run"
else
  echo "FAIL: qemu-aarch64 not found (NEVR_SKIP_QEMU=1 to skip explicitly)"
  exit 1
fi
