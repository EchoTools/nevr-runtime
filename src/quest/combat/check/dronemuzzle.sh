#!/usr/bin/env bash
# dronemuzzle's arm64 checks (docs/adr/0046 rule 2; src/quest/combat/dronemuzzle.h). Run by `just test-quest-combat dronemuzzle` after the host tests.
# Needs ANDROID_NDK_HOME (NDK r26d) and qemu-aarch64; each stops with its own message when it is missing, and Q15T_SKIP_QEMU=1 skips the run (and says so).
#   1. the two entries (src/quest/combat/dronemuzzle_entry.inc) in dronemuzzle.cpp compiled for arm64 (-O2, as the sentinel is) are exactly the expected instructions, in this order: both words
#      each entry reads are loaded with ldar (#2216), and the pass-through is a br through x16;
#   2. each continuation, compiled for arm64 (-O2, as the sentinel is), calls (blr) or jumps (br) through registers that only an ldar or ldapr of the
#      next word sets (a move from such a register counts; so does the frame restore of a path that returns), so a plain or relaxed load fails;
#   3. the freestanding program src/quest/tests/dronemuzzle_qemu.cpp, which includes that very text, runs under qemu-aarch64.
# This script reads instructions and relocations; it reads no CFI. The frame sensor (tests/quest, on the built sentinel) is what checks the entries' frame description.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
: "${ANDROID_NDK_HOME:?dronemuzzle.sh: ANDROID_NDK_HOME must name the NDK r26d directory}"
bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
cxx="$bin/aarch64-linux-android26-clang++"
objdump="$bin/llvm-objdump"
[ -x "$cxx" ] || { echo "dronemuzzle.sh: no NDK compiler at $cxx" >&2; exit 1; }
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
program="$scratch/dronemuzzle_qemu"
"$cxx" -O1 -fno-exceptions -fno-rtti -fno-stack-protector -Wall -Wextra -Werror -I "$root/src" -static -nostdlib -fuse-ld=lld \
  -o "$program" "$root/src/quest/tests/dronemuzzle_qemu.cpp"

object="$scratch/dronemuzzle.o"
"$cxx" -std=c++17 -O2 -fno-exceptions -fvisibility=hidden -Wall -Wextra -Werror -I "$root/src" -I "$root/src/quest/sentinel" -I "$root/sdk/include" \
  -c "$root/src/quest/combat/dronemuzzle.cpp" -o "$object"
# The entries as the sentinel's object holds them (the module's own translation unit, not the qemu program, whose static link relaxes adrp+add).
got="$scratch/got"
for entry in nevr_combat_dm_tp_entry nevr_combat_dm_fire_entry; do
  echo "$entry:" >> "$got"
  "$objdump" -d --disassemble-symbols="$entry" "$object" | sed -n -E 's/^ *[0-9a-f]+: +[0-9a-f]{8} +\t//p' | tr -s ' \t' ' ' >> "$got"
done
# an address is an address: keep the mnemonic and the operand's kind
sed -i -E 's/^(adrp x16), 0x[0-9a-f]+.*$/\1, 0x/; s/^(add x16, x16), #0x[0-9a-f]+.*$/\1, #lo12/; s/^(b\.eq|bl) 0x[0-9a-f]+.*$/\1 0x/' "$got"
want="$scratch/want"
for entry in nevr_combat_dm_tp_entry nevr_combat_dm_fire_entry; do
  cat >> "$want" <<WANT
$entry:
bti c
adrp x16, 0x
add x16, x16, #lo12
ldar x16, [x16]
cmp x30, x16
b.eq 0x
adrp x16, 0x
add x16, x16, #lo12
ldar x16, [x16]
br x16
stp x29, x30, [sp, #-0x10]!
mov x29, sp
bl 0x
ldp x29, x30, [sp], #0x10
ret
WANT
done
if ! diff -u "$want" "$got"; then
  echo "FAIL: the compiled entries differ from the expected instruction sequence"
  exit 1
fi
echo "dronemuzzle: the two entries are exactly the expected instructions (each word read by ldar)"

for fn in nevr_combat_dm_teleport nevr_combat_dm_fire; do
  "$objdump" -d --no-show-raw-insn --disassemble-symbols="$fn" "$object" | python3 -I -c '
import re, sys
fn = sys.argv[1]
lines = [l.split("\t", 1)[1].strip() for l in sys.stdin if re.match(r"^\s+[0-9a-f]+:\s", l)]
SKIP = r"(?!cbn?z|tbn?z|cmp|cmn|tst|str|stp|stlr|stur|blr|br|ret)"
def writes(ins, reg):
    return re.match(SKIP + r"[a-z0-9.]+\s+" + reg + r"(,|$)", ins) or re.match(r"ldp\s+\w+, " + reg + r",", ins)
def epilogue(n, reg):
    # a restore of a callee-saved register from the frame, followed by the return of that path before any use of it
    if not re.match(r"(ldp|ldr)\s+.*\[sp", lines[n]):
        return False
    for ins in lines[n + 1:n + 13]:
        if re.match(r"(blr|br)\s+" + reg + "$", ins):
            return False
        if ins == "ret" or ins.startswith("b\t") or re.match(r"b\s", ins):
            return True
    return False
def ok(at, reg, depth=0):
    # back from the instruction at `at`: the nearest write of `reg` on this path is an acquire load (or a move from one); a frame restore that
    # belongs to a path that returns before `at` is passed over
    for n in range(at - 1, -1, -1):
        ins = lines[n]
        if not writes(ins, reg):
            continue
        if re.match(r"(ldar|ldapr)\s+" + reg + r",", ins):
            return True, ""
        mv = re.match(r"mov\s+" + reg + r", (x\d+)$", ins)
        if mv and depth < 4:
            return ok(n, mv.group(1), depth + 1)
        if epilogue(n, reg):
            continue
        return False, ins
    return False, "nothing (no ldar or ldapr sets it)"
calls = 0
for at, ins in enumerate(lines):
    m = re.match(r"(blr|br)\s+(x\d+)$", ins)
    if not m:
        continue
    calls += 1
    good, why = ok(at, m.group(2))
    if not good:
        print("FAIL: " + fn + ": `" + ins + "` goes through " + m.group(2) + ", last set on its path by `" + why + "`, not by ldar/ldapr")
        sys.exit(1)
if calls == 0:
    print("FAIL: " + fn + " has no blr or br to follow")
    sys.exit(1)
print(fn + ": each of its " + str(calls) + " indirect calls goes through the next word loaded by ldar")
' "$fn" || exit 1
done

if command -v qemu-aarch64 >/dev/null 2>&1; then
  qemu-aarch64 "$program" || { echo "FAIL: dronemuzzle_qemu"; exit 1; }
elif [ "${Q15T_SKIP_QEMU:-0}" = 1 ]; then
  echo "SKIPPED (Q15T_SKIP_QEMU=1): qemu-aarch64 not found, the entries were not run"
else
  echo "FAIL: qemu-aarch64 not found (Q15T_SKIP_QEMU=1 to skip explicitly)"
  exit 1
fi
