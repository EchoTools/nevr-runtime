#!/usr/bin/env python3
"""Ensure every ordinary byte rewrite in mode_patches.cpp has binary ground truth.

This intentionally inventories only byte-write primitives (``ApplyPatch`` and
``ProcessMemcpy`` used for an opcode rewrite).  It does not treat MinHook
detours, import hooks, or IAT hooks as prologue rewrites: their correctness has
different invariants and they must not be silently represented as byte patches.

The matching tests are the PE-backed assertions in
tests/system/mode_patches_test.go.  Their target tables exercise each listed
RVA through assertRVABytes, so an Echo VR binary update must re-derive the
original bytes before a new rewrite can ship.
"""

import argparse
from pathlib import Path
import re
import sys


REPO = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO / "src/runtime/patch/mode_patches.cpp"
DEFAULT_ADDRESSES = REPO / "src/runtime/hook/addresses.h"
DEFAULT_GROUND_TRUTH = REPO / "tests/system/mode_patches_test.go"


class InventoryError(Exception):
    """The source could not be unambiguously interpreted as a rewrite inventory."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--addresses", type=Path, default=DEFAULT_ADDRESSES)
    parser.add_argument("--ground-truth", type=Path, default=DEFAULT_GROUND_TRUTH)
    return parser.parse_args()


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise InventoryError(f"cannot read {path}: {error}") from error


def strip_cpp_comments(text: str) -> str:
    """Remove comments while preserving source newlines for simple inventories."""
    result = []
    index = 0
    state = "code"
    while index < len(text):
        current = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code" and current == "/" and following == "/":
            state = "line-comment"
            index += 2
            continue
        if state == "code" and current == "/" and following == "*":
            state = "block-comment"
            index += 2
            continue
        if state == "line-comment":
            if current == "\n":
                result.append(current)
                state = "code"
            index += 1
            continue
        if state == "block-comment":
            if current == "*" and following == "/":
                state = "code"
                index += 2
                continue
            if current == "\n":
                result.append(current)
            index += 1
            continue
        result.append(current)
        index += 1
    return "".join(result)


def integer_constants(text: str) -> dict[str, int]:
    constants: dict[str, int] = {}
    for name, value in re.findall(
        r"\bconstexpr\s+uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", text
    ):
        constants[name] = int(value, 16)
    return constants


def add_aliases(text: str, constants: dict[str, int]) -> None:
    aliases = re.findall(
        r"\bconstexpr\s+uintptr_t\s+(\w+)\s*=\s*"
        r"(?:PatchAddresses::)?(\w+)\s*;",
        text,
    )
    # Local aliases such as LOGIN_CAP_CHECK deliberately name an existing
    # patch target. Resolve them after the numeric entries have been collected.
    # Repeating supports a short chain without pretending an unknown expression
    # is safe.
    for _ in aliases:
        for name, target in aliases:
            if target in constants:
                constants[name] = constants[target]


def resolve(expression: str, constants: dict[str, int]) -> int:
    expression = expression.strip()
    expression = expression.removeprefix("PatchAddresses::")
    if expression.startswith("0x"):
        return int(expression, 16)
    if expression in constants:
        return constants[expression]
    raise InventoryError(f"unresolved byte-rewrite target expression: {expression}")


def table_gate_targets(addresses: str, constants: dict[str, int]) -> set[int]:
    table = re.search(
        r"HEADLESS_GATE_TABLE\[\]\s*=\s*\{(.*?)\};", addresses, re.DOTALL
    )
    if table is None:
        raise InventoryError("HEADLESS_GATE_TABLE not found")
    names = re.findall(r"\{\s*(\w+)\s*,", table.group(1))
    if not names:
        raise InventoryError("HEADLESS_GATE_TABLE has no entries")
    return {resolve(name, constants) for name in names}


def byte_rewrite_targets(source: str, addresses: str) -> set[int]:
    constants = integer_constants(addresses)
    constants.update(integer_constants(source))
    add_aliases(addresses, constants)
    add_aliases(source, constants)
    targets: set[int] = set()

    # ApplyPatch is the normal opcode/data rewrite abstraction.  The first
    # argument must be a resolvable named or literal RVA; otherwise fail closed
    # rather than letting a new form evade the inventory.
    for expression in re.findall(
        r"\bApplyPatch\s*\(\s*((?:PatchAddresses::)?\w+|0x[0-9a-fA-F]+)\s*,",
        source,
    ):
        targets.add(resolve(expression, constants))

    # ForceHeadlessSkip has one dynamic argument, whose concrete sites are the
    # authoritative HEADLESS_GATE_TABLE. Any additional direct invocation is
    # resolved normally and therefore also needs ground truth.
    for expression in re.findall(r"\bForceHeadlessSkip\s*\(\s*([^,]+),", source):
        expression = expression.strip()
        if expression == "uintptr_t offset":
            # The helper definition is not an installation call.
            continue
        if expression == "gate.rva":
            targets.update(table_gate_targets(addresses, constants))
        else:
            targets.add(resolve(expression, constants))

    # Direct ProcessMemcpy writes currently occur in the D3D12 gate and the
    # spectator-stream guard.  Detect both forms explicitly.  A different
    # target form fails closed below, forcing the verifier to be extended in
    # the same reviewed change as the rewrite.
    for expression in re.findall(
        r"ProcessMemcpy\s*\(\s*EchoVR::g_GameBaseAddress\s*\+\s*"
        r"((?:PatchAddresses::)?\w+|0x[0-9a-fA-F]+)",
        source,
    ):
        if expression == "offset":
            # ForceHeadlessSkip's helper body is represented by the gate table.
            continue
        targets.add(resolve(expression, constants))

    spectator_write = re.search(
        r"uintptr_t\s+addr\s*=.*?\+\s*PatchAddresses::(\w+).*?"
        r"ProcessMemcpy\s*\(\s*reinterpret_cast<VOID\*>\(addr\)",
        source,
        re.DOTALL,
    )
    if spectator_write is not None:
        targets.add(resolve(spectator_write.group(1), constants))

    process_memcpy_count = len(re.findall(r"\bProcessMemcpy\s*\(", source))
    recognized_process_memcpy = 1 + (1 if spectator_write is not None else 0)
    # The first ProcessMemcpy is ForceHeadlessSkip(offset), which is covered by
    # HEADLESS_GATE_TABLE. The second is the direct D3D12 write above.
    if process_memcpy_count != 3 or recognized_process_memcpy != 2:
        raise InventoryError(
            "unrecognized ProcessMemcpy byte-write form; update this verifier "
            "and its binary ground truth in the same change"
        )
    return targets


def ground_truth_targets(test: str) -> set[int]:
    # Target rows are consumed by assertRVABytes in the first four test
    # functions.  Exclude the overlap-only table, whose rows do not validate
    # original bytes.
    validated = test.split("func TestModePatchesTargetsDoNotOverlap", maxsplit=1)[0]
    values = re.findall(r'\{\s*"[^"]+"\s*,\s*(0x[0-9a-fA-F]+)\s*,', validated)
    values.extend(
        re.findall(r"assertRVABytes\s*\(\s*t\s*,\s*exe\s*,\s*(0x[0-9a-fA-F]+)\s*,", validated)
    )
    if not values:
        raise InventoryError("no binary ground-truth target rows found")
    return {int(value, 16) for value in values}


def verify(source_path: Path, addresses_path: Path, ground_truth_path: Path) -> tuple[set[int], set[int]]:
    source = strip_cpp_comments(read(source_path))
    addresses = strip_cpp_comments(read(addresses_path))
    tests = read(ground_truth_path)
    rewrites = byte_rewrite_targets(source, addresses)
    covered = ground_truth_targets(tests)
    missing = sorted(rewrites - covered)
    if missing:
        formatted = ", ".join(f"0x{value:X}" for value in missing)
        raise InventoryError(f"byte-rewrite target(s) lack assertRVABytes ground truth: {formatted}")
    return rewrites, covered


def main() -> int:
    args = parse_args()
    try:
        rewrites, _covered = verify(args.source, args.addresses, args.ground_truth)
    except InventoryError as error:
        print(f"mode-patch-ground-truth: FAIL {error}", file=sys.stderr)
        return 1
    names = ", ".join(f"0x{value:X}" for value in sorted(rewrites))
    print(
        "mode-patch-ground-truth: OK "
        f"byte-rewrites={len(rewrites)} matched={len(rewrites)} targets={names}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
