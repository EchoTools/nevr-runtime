#!/usr/bin/env python3
"""
Tier-0 hook invariants — static checks, no Wine, no game process, no execution.

These exist because every serious bug in the broadcaster thread came from the
same root: an address was given a NAME nobody re-derived, and every later
decision was reasoned from the name instead of the binary. `0x140f80ed0` was
labelled ENGINE_ENTITY_LOOKUP; it is CBroadcaster::Listen. Called by its real
name, "return -1 when the listener table looks unset" is obviously load-bearing.
Called ENGINE_ENTITY_LOOKUP, it reads as a harmless null-check on render junk.

Three checks, in descending order of how badly their absence hurt:

  1. SELF-COLLISION  — an address we call through a live function pointer must
     not also be an address we install a detour on. Otherwise our own call
     re-enters our own hook. This is what severed the ServerDB -> game message
     path: echovr_functions.cpp assigns BroadcasterReceiveLocalEvent = base +
     0xF87AA0, and mode_patches.cpp detours that same RVA, so all 15 injection
     sites in gameserver.cpp land in a hook that returns early on a server.

  2. IDENTITY PINNING — the bytes at each hooked address must still match what
     was there when the hook was written. Catches both binary drift and the
     original misidentification, and needs only a file read.

  3. DOUBLE DETOUR   — one address, two MinHook instances (gamepatches links
     extern/minhook, plugins link vcpkg minhook — separate static copies with
     separate private hook tables) means the second builds its trampoline out
     of the first's JMP stub.

KNOWN_* entries below are open bugs, recorded so they are visible and tracked.
They do NOT pass silently — each prints a warning naming its ledger ID. Anything
NOT on those lists is a hard failure. When a bug is fixed, delete its entry and
this checker enforces that it stays fixed.

Exit 0 = pass (possibly with known-bug warnings). Exit 1 = new violation.
"""

import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
IMAGE_BASE = 0x140000000
GAME_BINARY = REPO / "echovr" / "bin" / "win10" / "echovr.exe"
MANIFEST = REPO / "tools" / "hook_identity_manifest.json"

# --- Known-bug registers -----------------------------------------------------
# Each entry: normalized VA -> (ledger-id, one-line description).
# Presence here = "we know, it is filed, it is not fixed yet". Absence = hard fail.

KNOWN_SELF_COLLISIONS = {
    0x140F87AA0: ("N83", "CBroadcaster::ReceiveLocalEvent — called via "
                         "EchoVR::BroadcasterReceiveLocalEvent (echovr_functions.cpp:87) AND "
                         "detoured as ENGINE_ENTITY_PROP_DISPATCH (mode_patches.cpp). "
                         "ACCEPTED: since 2026-07-26 the hook is a pass-through null-guard, not "
                         "an early return, so a re-entering call is checked and then dispatched. "
                         "Our own injections get the same guard, which is arguably correct. "
                         "Re-evaluate if that hook ever regains an unconditional return path."),
    0x140F80ED0: ("N83", "CBroadcaster::Listen — called via EchoVR::BroadcasterListen "
                         "(echovr_functions.cpp:88) AND detoured as ENGINE_ENTITY_LOOKUP "
                         "(mode_patches.cpp:723)."),
}

KNOWN_DOUBLE_DETOURS = {
    0x140F87AA0: ("N84", "REGRESSION, knowingly accepted and time-boxed. gamepatches detours this "
                         "again as of 2026-07-26 (N83 null-guard), so broadcaster_bridge is no "
                         "longer sole owner and the two-MinHook-instance hazard is back. This is "
                         "a real cost of the N83 fix, not a benign entry. EXIT CONDITION: the "
                         "guard now logs when it trips. If it does not trip across representative "
                         "server runs, the AV it guards is not occurring — remove the gamepatches "
                         "detour entirely and this closes permanently. Runtime HookGuard reports "
                         "the collision at ERROR if the plugin's install actually overwrites ours."),
    # Previously empty. 0x140F87AA0 was here until 2026-07-26: gamepatches detoured it as
    # ENGINE_ENTITY_PROP_DISPATCH while broadcaster_bridge hooked it as
    # VA_BROADCASTER_RECEIVE_LOCAL. Removing the unjustified gamepatches detour
    # (N83) left the plugin as sole owner, which resolved this too. If a second
    # owner reappears, that is a NEW violation and fails hard.
}


def norm_va(value: int) -> int:
    """Accept either an RVA (patch_addresses.h) or a full VA (address_registry.h)."""
    return value if value >= IMAGE_BASE else value + IMAGE_BASE


def read(rel: str) -> str:
    p = REPO / rel
    return p.read_text(errors="replace") if p.exists() else ""


# --- Extraction --------------------------------------------------------------

def live_function_pointers() -> dict:
    """RVAs the runtime CALLS through, from src/common/echovr_functions.cpp."""
    src = read("src/common/echovr_functions.cpp")
    out = {}
    for m in re.finditer(
        r"^\s*(\w+)\s*=\s*\([^)]*\)\s*\(\s*g_GameBaseAddress\s*\+\s*(0x[0-9A-Fa-f]+)\s*\)",
        src, re.M,
    ):
        out[norm_va(int(m.group(2), 16))] = m.group(1)
    return out


def patch_address_constants() -> dict:
    """name -> normalized VA, from src/gamepatches/patch_addresses.h."""
    src = read("src/gamepatches/patch_addresses.h")
    return {
        m.group(1): norm_va(int(m.group(2), 16))
        for m in re.finditer(
            r"constexpr\s+uintptr_t\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", src
        )
    }


def registry_constants() -> dict:
    """name -> normalized VA, from plugins/common/include/address_registry.h."""
    src = read("plugins/common/include/address_registry.h")
    return {
        m.group(1): norm_va(int(m.group(2), 16))
        for m in re.finditer(
            r"constexpr\s+uint64_t\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", src
        )
    }


def gamepatches_detour_targets() -> dict:
    """
    VA -> constant name, for PatchAddresses:: constants that reach a detour
    installer. Deliberately coarse-but-bounded: a constant counts if it appears
    in the same file as PatchDetour/MH_CreateHook AND is assigned into a
    variable that one of those is called on. Over-inclusion is safe here (it can
    only add scrutiny); under-inclusion is what we cannot afford.
    """
    consts = patch_address_constants()
    found = {}
    for path in sorted((REPO / "src" / "gamepatches").rglob("*.cpp")):
        text = path.read_text(errors="replace")
        if "PatchDetour" not in text and "MH_CreateHook" not in text:
            continue
        for m in re.finditer(
            r"(\w+)\s*=\s*\([^;]*?PatchAddresses::(\w+)\s*\)\s*;", text, re.S
        ):
            var, const = m.group(1), m.group(2)
            if const not in consts:
                continue
            if re.search(rf"(PatchDetour|MH_CreateHook)\s*\(\s*&?\s*{re.escape(var)}\b", text):
                found[consts[const]] = const
    return found


def plugin_hooked_vas() -> dict:
    """VA -> (plugin, registry-constant) for address_registry constants used in plugins."""
    reg = registry_constants()
    found = {}
    for path in sorted((REPO / "plugins").rglob("*.cpp")):
        text = path.read_text(errors="replace")
        if "CreateAndEnable" not in text and "MH_CreateHook" not in text:
            continue
        plugin = path.relative_to(REPO / "plugins").parts[0]
        for name in re.findall(r"nevr::addresses::(VA_\w+)", text):
            if name in reg:
                found[reg[name]] = (plugin, name)
    return found


# --- Minimal PE reader (no dependencies) -------------------------------------

def pe_sections(data: bytes):
    e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
    n_sections = int.from_bytes(data[e_lfanew + 6: e_lfanew + 8], "little")
    opt_size = int.from_bytes(data[e_lfanew + 20: e_lfanew + 22], "little")
    tbl = e_lfanew + 24 + opt_size
    out = []
    for i in range(n_sections):
        e = tbl + i * 40
        out.append((
            int.from_bytes(data[e + 12: e + 16], "little"),  # VirtualAddress
            int.from_bytes(data[e + 8: e + 12], "little"),   # VirtualSize
            int.from_bytes(data[e + 20: e + 24], "little"),  # PointerToRawData
            int.from_bytes(data[e + 16: e + 20], "little"),  # SizeOfRawData
        ))
    return out


def read_at_va(data, sections, va: int, count: int):
    rva = va - IMAGE_BASE
    for vaddr, vsize, praw, rsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            off = praw + (rva - vaddr)
            if off + count <= len(data):
                return data[off: off + count]
    return None


# --- Checks ------------------------------------------------------------------

def check_self_collision(failures, warnings):
    called = live_function_pointers()
    detoured = gamepatches_detour_targets()
    for va in sorted(set(called) & set(detoured)):
        desc = (f"0x{va:X} is CALLED as EchoVR::{called[va]} "
                f"(src/common/echovr_functions.cpp) and DETOURED as "
                f"PatchAddresses::{detoured[va]} (src/gamepatches/). "
                f"Our own calls re-enter our own hook.")
        if va in KNOWN_SELF_COLLISIONS:
            lid, why = KNOWN_SELF_COLLISIONS[va]
            warnings.append(f"[{lid}] known self-collision 0x{va:X}: {why}")
        else:
            failures.append("SELF-COLLISION: " + desc)


def check_double_detour(failures, warnings):
    gp = gamepatches_detour_targets()
    pl = plugin_hooked_vas()
    for va in sorted(set(gp) & set(pl)):
        plugin, const = pl[va]
        desc = (f"0x{va:X} is detoured by gamepatches (PatchAddresses::{gp[va]}) "
                f"and by plugin '{plugin}' ({const}). Separate MinHook instances "
                f"do not share a hook table.")
        if va in KNOWN_DOUBLE_DETOURS:
            lid, why = KNOWN_DOUBLE_DETOURS[va]
            warnings.append(f"[{lid}] known double detour 0x{va:X}: {why}")
        else:
            failures.append("DOUBLE-DETOUR: " + desc)


def check_identity(failures, warnings):
    if not MANIFEST.exists():
        warnings.append(f"identity manifest absent ({MANIFEST.relative_to(REPO)}) — "
                        f"run: tools/verify_hook_invariants.py --write-manifest")
        return
    if not GAME_BINARY.exists():
        warnings.append(f"game binary absent ({GAME_BINARY.relative_to(REPO)}) — "
                        f"identity pinning skipped (source-level checks still ran)")
        return

    data = GAME_BINARY.read_bytes()
    sections = pe_sections(data)
    entries = json.loads(MANIFEST.read_text())["addresses"]
    for e in entries:
        va = int(e["va"], 16)
        want = bytes.fromhex(e["prologue"])
        got = read_at_va(data, sections, va, len(want))
        if got is None:
            failures.append(f"IDENTITY: 0x{va:X} ({e['name']}) not mapped in "
                            f"{GAME_BINARY.name} — address is outside every section.")
        elif got != want:
            failures.append(
                f"IDENTITY: 0x{va:X} ({e['name']}) prologue changed — "
                f"expected {want.hex()} got {got.hex()}. Either the game binary "
                f"changed or this address never was {e['name']}. Re-derive from "
                f"ReVault before touching the hook.")


def write_manifest():
    """Pin current bytes for every address we hook. Names come from ReVault."""
    if not GAME_BINARY.exists():
        print(f"cannot write manifest: {GAME_BINARY} not found", file=sys.stderr)
        return 1
    data = GAME_BINARY.read_bytes()
    sections = pe_sections(data)

    # ReVault-verified identities. Where a local constant name disagrees with
    # ReVault, ReVault wins and the disagreement is recorded in `note`.
    revault_names = {
        0x140F80ED0: ("BroadcasterListen (CBroadcaster::Listen)",
                      "patch_addresses.h calls this ENGINE_ENTITY_LOOKUP — wrong (N83)"),
        0x140F87AA0: ("CBroadcaster::ReceiveLocalEvent",
                      "patch_addresses.h calls this ENGINE_ENTITY_PROP_DISPATCH — wrong (N83)"),
    }

    targets = {}
    targets.update(gamepatches_detour_targets())
    for va, name in live_function_pointers().items():
        targets.setdefault(va, f"EchoVR::{name}")

    entries = []
    for va in sorted(targets):
        raw = read_at_va(data, sections, va, 8)
        if raw is None:
            continue
        name, note = revault_names.get(va, (targets[va], ""))
        entry = {"va": f"0x{va:X}", "name": name, "prologue": raw.hex()}
        if note:
            entry["note"] = note
        entries.append(entry)

    MANIFEST.write_text(json.dumps(
        {"_comment": "Pinned by tools/verify_hook_invariants.py --write-manifest. "
                     "prologue = first 8 bytes at va in echovr/bin/win10/echovr.exe. "
                     "A mismatch means the binary changed OR the address was never "
                     "what its constant claims. Re-derive from ReVault, do not re-pin.",
         "image_base": f"0x{IMAGE_BASE:X}",
         "addresses": entries}, indent=2) + "\n")
    print(f"wrote {MANIFEST.relative_to(REPO)} ({len(entries)} addresses)")
    return 0



# --- N82: one owner for the int3-skip decision -------------------------------

# The designated owner of EXCEPTION_BREAKPOINT. Every other vectored exception
# handler must return EXCEPTION_CONTINUE_SEARCH for it.
VEH_BREAKPOINT_OWNER = "src/gamepatches/crash_recovery.cpp"

# Not built: plugins/CMakeLists.txt add_subdirectory()s only log-filter,
# broadcaster-bridge, anim-debugger, example. This file ships nowhere, so it
# cannot be a second live owner. If it is ever added to the build it MUST be
# reworked first — hence it is excluded by path, not by pretending it is clean.
VEH_UNBUILT = {"plugins/crash-handler/src/plugin.cpp"}


def check_veh_ownership(failures, warnings):
    """
    Two vectored handlers that both act on EXCEPTION_BREAKPOINT will disagree,
    and the order that resolves the disagreement is registration timing, not
    design. AddVectoredExceptionHandler(1, ...) puts the newest handler FIRST,
    so 'priority 1' does not mean 'the one you meant'.

    The rule is narrow on purpose: any number of VEHs may coexist (safe_memory.h
    legitimately installs a scoped one), but exactly one may consume int3.
    """
    for path in sorted(REPO.rglob("*.cpp")) + sorted(REPO.rglob("*.h")):
        try:
            rel = path.relative_to(REPO).as_posix()
        except ValueError:
            continue
        if not (rel.startswith("src/") or rel.startswith("plugins/")):
            continue
        if "legacy" in rel or rel in VEH_UNBUILT or rel == VEH_BREAKPOINT_OWNER:
            continue
        text = path.read_text(errors="replace")
        if "AddVectoredExceptionHandler" not in text:
            continue
        if "EXCEPTION_BREAKPOINT" in text:
            failures.append(
                f"VEH-OWNERSHIP: {rel} registers a vectored exception handler AND "
                f"references EXCEPTION_BREAKPOINT. Only {VEH_BREAKPOINT_OWNER} may act "
                f"on int3 — a second handler makes the skip decision depend on plugin "
                f"load order (N82). Return EXCEPTION_CONTINUE_SEARCH for it.")

    # The owner must still exist and still handle it.
    owner = read(VEH_BREAKPOINT_OWNER)
    if "EXCEPTION_BREAKPOINT" not in owner:
        failures.append(
            f"VEH-OWNERSHIP: {VEH_BREAKPOINT_OWNER} no longer handles "
            f"EXCEPTION_BREAKPOINT — the int3 skip after a suppressed ExitProcess "
            f"has no owner at all (N82).")


def main() -> int:
    if "--write-manifest" in sys.argv:
        return write_manifest()

    failures, warnings = [], []
    check_self_collision(failures, warnings)
    check_double_detour(failures, warnings)
    check_identity(failures, warnings)
    check_veh_ownership(failures, warnings)

    for w in warnings:
        print(f"hook-invariants: WARN {w}")
    for f in failures:
        print(f"hook-invariants: FAIL {f}", file=sys.stderr)

    if failures:
        print(f"\nhook-invariants: {len(failures)} NEW violation(s). "
              f"These are not on the known-bug register — an address was hooked "
              f"or called without the collision being considered.", file=sys.stderr)
        return 1

    print(f"hook-invariants: OK ({len(warnings)} known bug(s) tracked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
