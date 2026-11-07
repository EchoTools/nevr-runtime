#!/usr/bin/env bash
# POWER BOOST's arm64 checks (docs/adr/0046 rule 2; src/quest/combat/powerboost.h). Run by `just test-quest-combat powerboost` after the host
# tests. Needs ANDROID_NDK_HOME (NDK r26d) and qemu-aarch64; each stops with its own message when it is missing, and Q15T_SKIP_QEMU=1 skips the
# run (and says so).
#   1. the entry (src/quest/combat/powerboost_thunk.inc) compiled for arm64 is exactly the instructions and relocations in powerboost_thunk.want;
#   2. every word the entry reads reaches the register it compares or branches through by ldar or ldapr (#2216: a plain ldr fails), and the
#      continuation, compiled for arm64 (-O2, as the sentinel is), loads the next word it calls through with ldar or ldapr;
#   3. the freestanding program src/quest/tests/pb_thunk_qemu.cpp, which includes that very text, runs under qemu-aarch64.
# PB_THUNK_WANT names another expected listing (a test of this script).
# This script reads instructions and relocations; it reads no CFI. The frame sensor (tests/quest, on the built sentinel) is what checks the entry's frame description.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
: "${ANDROID_NDK_HOME:?powerboost_thunk.sh: ANDROID_NDK_HOME must name the NDK r26d directory}"
bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
cxx="$bin/aarch64-linux-android26-clang++"
objdump="$bin/llvm-objdump"
[ -x "$cxx" ] || { echo "powerboost_thunk.sh: no NDK compiler at $cxx" >&2; exit 1; }
want="${PB_THUNK_WANT:-$root/src/quest/combat/check/powerboost_thunk.want}"
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT

# 1. the entry as an object (relocations unresolved, so the listing names the words it reads)
"$cxx" -O1 -fno-exceptions -fno-rtti -fno-stack-protector -Wall -Wextra -Werror -I "$root/src" -c -o "$scratch/entry.o" "$root/src/quest/tests/pb_thunk_qemu.cpp"
got="$scratch/got"
"$objdump" -dr --disassemble-symbols=nevr_combat_pb_resolve_entry "$scratch/entry.o" \
  | sed -n -E -e 's/^ *[0-9a-f]+: +[0-9a-f]{8} +\t//p' -e 's/^\t+[0-9a-f]+: +(R_AARCH64_[A-Z0-9_]+)\t(.*)$/    reloc \1 \2/p' | tr -s ' \t' ' ' > "$got"
if ! diff -u "$want" "$got"; then
  echo "FAIL: the compiled entry differs from the expected instruction sequence ($want)"
  exit 1
fi
echo "powerboost: the entry is exactly the expected $(grep -vc reloc "$want") instructions and $(grep -c reloc "$want") relocations"

# 2. the acquire loads. In the entry: the register of each `cmp x30, <r>` and `br <r>` was last set by ldar/ldapr. In the continuation: the
# register its first blr calls through was last set by ldar/ldapr.
follow='
import re, sys
fn, mode = sys.argv[1], sys.argv[2]
lines = [l.split("\t", 1)[1].strip() for l in sys.stdin if re.match(r"^\s+[0-9a-f]+:\s", l)]
def origin(n, reg):
    for prev in reversed(lines[:n]):
        if re.match(r"(ldar|ldapr)\s+" + reg + r",", prev):
            return None
        if re.match(r"(?!cbn?z|tbn?z|cmp|cmn|tst|str|stp|stlr|b\.)[a-z0-9.]+\s+" + reg + r"\b", prev) or re.match(r"ldp\s+(\w+, )?" + reg + r",", prev):
            return prev
    return "nothing"
checked = 0
for n, ins in enumerate(lines):
    m = re.match(r"cmp\s+x30, (x\d+)$", ins) or re.match(r"br\s+(x\d+)$", ins) if mode == "entry" else re.match(r"blr\s+(x\d+)$", ins)
    if not m:
        continue
    bad = origin(n, m.group(1))
    if bad is not None:
        print("FAIL: " + fn + ": `" + ins + "` uses " + m.group(1) + ", last set by `" + bad + "`, not by ldar/ldapr")
        sys.exit(1)
    checked += 1
    if mode != "entry":
        break
if checked == 0:
    print("FAIL: " + fn + " has nothing to follow")
    sys.exit(1)
print(fn + ": " + str(checked) + " use(s) of a published word, each reached by an acquire load")
'
"$objdump" -d --no-show-raw-insn --disassemble-symbols=nevr_combat_pb_resolve_entry "$scratch/entry.o" | python3 -I -c "$follow" nevr_combat_pb_resolve_entry entry
object="$scratch/powerboost.o"
"$cxx" -std=c++17 -O2 -fno-exceptions -fvisibility=hidden -Wall -Wextra -Werror -I "$root/src" -I "$root/src/quest/sentinel" -I "$root/sdk/include" \
  -c "$root/src/quest/combat/powerboost.cpp" -o "$object"
"$objdump" -d --no-show-raw-insn --disassemble-symbols=_ZN10nevr_quest6combat10powerboost6ConeAtEPKNS1_8PhTargetEPKhm "$object" \
  | python3 -I -c "$follow" ConeAt continuation
# the entry in the sentinel's own object is the same text
"$objdump" -dr --disassemble-symbols=nevr_combat_pb_resolve_entry "$object" \
  | sed -n -E -e 's/^ *[0-9a-f]+: +[0-9a-f]{8} +\t//p' -e 's/^\t+[0-9a-f]+: +(R_AARCH64_[A-Z0-9_]+)\t(.*)$/    reloc \1 \2/p' | tr -s ' \t' ' ' > "$scratch/got_sentinel"
diff -u "$want" "$scratch/got_sentinel" || { echo "FAIL: powerboost.cpp's entry is not the text the qemu program runs"; exit 1; }

# 3. run it
program="$scratch/pb_thunk_qemu"
"$cxx" -O1 -fno-exceptions -fno-rtti -fno-stack-protector -Wall -Wextra -Werror -I "$root/src" -static -nostdlib -fuse-ld=lld \
  -o "$program" "$root/src/quest/tests/pb_thunk_qemu.cpp"
if command -v qemu-aarch64 >/dev/null 2>&1; then
  qemu-aarch64 "$program" || { echo "FAIL: pb_thunk_qemu"; exit 1; }
elif [ "${Q15T_SKIP_QEMU:-0}" = 1 ]; then
  echo "SKIPPED (Q15T_SKIP_QEMU=1): qemu-aarch64 not found, the entry was not run"
else
  echo "FAIL: qemu-aarch64 not found (Q15T_SKIP_QEMU=1 to skip explicitly)"
  exit 1
fi
