#!/usr/bin/env bash
# The drone explosion's page fields, checked in the built drone script library (docs/quest/combat/drone_explosion.md). Run by
# `just test-quest-combat` after the host tests (the recipe runs every script in check/). Needs ANDROID_NDK_HOME (its llvm-objdump and
# llvm-nm) and the built library build/quest-combat-libs/lib221a0a3a2e56d247.so (`just build-quest-combat-libs`, which needs the
# reference build's libsrc and the store APK). It stops with its own message when either is missing.
# The offsets are read from src/quest/combat/ec_core.h (kDroneBlock, kDroneSteps, kDroneCtx, kDroneSpawn, kDroneSoundFn). It asserts:
#   1. the prelude computes the drone block from the page pointer in x0: `add xB, x0, #kDroneBlock`;
#   2. the counter at kDroneBlock + kDroneSteps is a ldaxr / stlxr pair on its address;
#   3. three 8-byte stores through the window write (the address in x1, `add x1, xB, #off`, then a bl, then `str xV, [x0]`), in address
#      order: the context (kDroneCtx; its value is loaded from the stack), the spawn entry (kDroneSpawn) and the sound entry (kDroneSoundFn);
#   4. the spawn and sound values are the addresses of the library's symbols re33_ecent_221a0a_13720 and re33_ecent_221a0a_13920, loaded by adrp + add;
#   5. those addresses are kDroneSpawnEntry and kDroneSoundEntry, and neither symbol is in the dynamic symbol table (dronekill checks an entry by
#      its library and offset, because dladdr cannot name a local symbol).
# It reads the disassembly and the symbol table; it runs nothing.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
header="$root/src/quest/combat/ec_core.h"
lib="$root/build/quest-combat-libs/lib221a0a3a2e56d247.so"
: "${ANDROID_NDK_HOME:?drone_explosion_page.sh: ANDROID_NDK_HOME must name the NDK r26d directory}"
bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
objdump="$bin/llvm-objdump"
nm="$bin/llvm-nm"
for tool in "$objdump" "$nm"; do
  [ -x "$tool" ] || { echo "drone_explosion_page.sh: no NDK tool at $tool" >&2; exit 1; }
done
[ -f "$lib" ] || { echo "drone_explosion_page.sh: $lib is not built; run: just build-quest-combat-libs (it needs NEVR_COMBAT_LIBSRC and NEVR_STORE_APK)" >&2; exit 1; }
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT

cat > "$scratch/check.py" <<'PY'
import re
import subprocess
import sys

header, lib, objdump, nm = sys.argv[1:5]


def die(message):
    print("FAIL: " + message)
    sys.exit(1)


text = open(header, encoding="utf-8").read()


def const(name):
    hits = re.findall(r"\b" + name + r" = (0x[0-9a-fA-F]+)", text)
    if len(hits) != 1:
        die("%s must be declared once as `%s = 0x..` in src/quest/combat/ec_core.h (found %d)" % (name, name, len(hits)))
    return int(hits[0], 16)


BLOCK = const("kDroneBlock")
STEPS = const("kDroneSteps")
CTX = const("kDroneCtx")
SPAWN = const("kDroneSpawn")
SOUND = const("kDroneSoundFn")

symbols = subprocess.run([nm, lib], capture_output=True, text=True)
if symbols.returncode != 0:
    die("llvm-nm could not read " + lib)
table = {}
for line in symbols.stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in ("t", "T"):
        table.setdefault(parts[2], int(parts[0], 16))


def symbol(suffix):
    found = [(name, addr) for name, addr in table.items() if name.endswith(suffix)]
    if len(found) != 1:
        die("%s: %d symbols in the library end with it (want 1)" % (suffix, len(found)))
    return found[0]


PRELUDE, _ = symbol("re33_ecpre_221a0aER16EXECUTION_THREAD")
_, SPAWN_ENTRY = symbol("re33_ecent_221a0a_13720EPv")
_, SOUND_ENTRY = symbol("re33_ecent_221a0a_13920EPv")

listing = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--disassemble-symbols=" + PRELUDE, lib],
                         capture_output=True, text=True)
if listing.returncode != 0:
    die("llvm-objdump could not disassemble " + PRELUDE)
ins = []
for line in listing.stdout.splitlines():
    m = re.match(r"^\s*([0-9a-f]+):\s+(\S.*)$", line)
    if m:
        # objdump puts a tab between the mnemonic and its operands; the patterns below expect one space
        ins.append((int(m.group(1), 16), re.sub(r"\s+", " ", m.group(2)).strip()))
if len(ins) < 100:
    die("the prelude disassembled to %d instructions (want the whole function)" % len(ins))

# 1. the drone block base
base = None
for i, (_, t) in enumerate(ins):
    m = re.fullmatch(r"add (x\d+), x0, #0x%x" % BLOCK, t)
    if m:
        base = (i, m.group(1))
        break
if base is None:
    die("no `add xN, x0, #0x%x` in the prelude: kDroneBlock is not where the library computes the drone block" % BLOCK)
bi, B = base

# 2. the counter
counter = None
for i in range(bi + 1, len(ins)):
    m = re.fullmatch(r"add (x\d+), x0, #0x%x" % (BLOCK + STEPS), ins[i][1])
    if m:
        counter = (i, m.group(1))
        break
if counter is None:
    die("no `add xN, x0, #0x%x` after the drone block base: kDroneBlock + kDroneSteps is not the counter" % (BLOCK + STEPS))
ci, A = counter
after = [t for _, t in ins[ci + 1:ci + 25]]
if not any(re.fullmatch(r"ldaxr w\d+, \[%s\]" % A, t) for t in after):
    die("the counter at kDroneBlock + kDroneSteps has no ldaxr on %s" % A)
if not any(re.fullmatch(r"stlxr w\d+, w\d+, \[%s\]" % A, t) for t in after):
    die("the counter at kDroneBlock + kDroneSteps has no stlxr on %s" % A)

WRITERS = ("adrp", "add", "sub", "ldr", "ldur", "ldp", "ldar", "ldaxr", "mov", "movz", "movk")


def last_writer(reg, before):
    for k in range(before - 1, -1, -1):
        m = re.match(r"([a-z]+)\s+([xw]\d+)(,|$)", ins[k][1])
        if m and m.group(1) in WRITERS and m.group(2)[1:] == reg[1:]:
            return k
    return None


def store(name, off, after_index):
    window = None
    for i in range(after_index + 1, len(ins)):
        m = re.fullmatch(r"add (x\d+), %s, #0x%x" % (B, off), ins[i][1])
        if m:
            window = (i, m.group(1))
            break
    if window is None:
        die("no `add xT, %s, #0x%x` (the %s field's window address) after the counter" % (B, off, name))
    wi, T = window
    bl = None
    for j in range(wi + 1, min(wi + 40, len(ins))):
        t = ins[j][1]
        if bl is None and t.startswith("bl "):
            bl = j
            continue
        if bl is not None and re.fullmatch(r"str (x\d+), \[x0\]", t):
            if last_writer(T, bl) is not None and last_writer(T, bl) > wi:
                die("the %s field's window address %s is rewritten between its add and its bl" % (name, T))
            return j, re.fullmatch(r"str (x\d+), \[x0\]", t).group(1)
    die("the %s field (+0x%x) is not stored through the window write (no bl then `str xV, [x0]`)" % (name, off))


# 3. the three stores, in address order
ctx_i, ctx_v = store("context", CTX, ci)
spawn_i, spawn_v = store("spawn", SPAWN, ctx_i)
sound_i, sound_v = store("sound", SOUND, spawn_i)

# the context: loaded from the stack
k = last_writer(ctx_v, ctx_i)
if k is None or not re.fullmatch(r"ldr x\d+, \[sp, #0x[0-9a-f]+\]", ins[k][1]):
    die("the context store (+0x%x) does not store a value loaded from the stack" % CTX)

# 4. the entries: adrp + add into the stored register
def entry_value(name, reg, store_index):
    k = last_writer(reg, store_index)
    if k is None:
        die("the %s store (+0x%x) has no writer of %s" % (name, store_index, reg))
    m = re.fullmatch(r"add (x\d+), (x\d+), #0x([0-9a-f]+)", ins[k][1])
    if not m or m.group(1) != reg or m.group(2) != reg:
        die("the %s value is not `add %s, %s, #imm` (found `%s`)" % (name, reg, reg, ins[k][1]))
    p = re.fullmatch(r"adrp (x\d+), 0x([0-9a-f]+)(?: .*)?", ins[k - 1][1])
    if not p or p.group(1) != reg:
        die("the %s value is not built by adrp %s before its add (found `%s`)" % (name, reg, ins[k - 1][1]))
    return int(p.group(2), 16) + int(m.group(3), 16)


spawn_target = entry_value("spawn", spawn_v, spawn_i)
sound_target = entry_value("sound", sound_v, sound_i)
if spawn_target != SPAWN_ENTRY:
    die("the spawn store (+0x%x) holds 0x%x, not re33_ecent_221a0a_13720 (0x%x)" % (SPAWN, spawn_target, SPAWN_ENTRY))
if sound_target != SOUND_ENTRY:
    die("the sound store (+0x%x) holds 0x%x, not re33_ecent_221a0a_13920 (0x%x)" % (SOUND, sound_target, SOUND_ENTRY))

# 5. the offsets dronekill checks, and why it checks offsets
if const("kDroneSpawnEntry") != SPAWN_ENTRY:
    die("kDroneSpawnEntry is 0x%x, re33_ecent_221a0a_13720 is at 0x%x" % (const("kDroneSpawnEntry"), SPAWN_ENTRY))
if const("kDroneSoundEntry") != SOUND_ENTRY:
    die("kDroneSoundEntry is 0x%x, re33_ecent_221a0a_13920 is at 0x%x" % (const("kDroneSoundEntry"), SOUND_ENTRY))
dynamic = subprocess.run([nm, "-D", "--defined-only", lib], capture_output=True, text=True)
if dynamic.returncode != 0:
    die("llvm-nm -D could not read " + lib)
for suffix in ("re33_ecent_221a0a_13720EPv", "re33_ecent_221a0a_13920EPv"):
    if any(line.split()[-1].endswith(suffix) for line in dynamic.stdout.splitlines() if line.split()):
        die("%s is in the dynamic symbol table: dladdr can name it, so check it by name" % suffix)

print("drone_explosion_page: the prelude stores the drone block (page+0x%x) as the header says: counter +0x%x, "
      "context +0x%x (stored from the stack), spawn +0x%x -> re33_ecent_221a0a_13720 (0x%x), "
      "sound +0x%x -> re33_ecent_221a0a_13920 (0x%x); both entries are local symbols at kDroneSpawnEntry and kDroneSoundEntry; library %s"
      % (BLOCK, STEPS, CTX, SPAWN, SPAWN_ENTRY, SOUND, SOUND_ENTRY, lib))
PY

python3 -I "$scratch/check.py" "$header" "$lib" "$objdump" "$nm"
