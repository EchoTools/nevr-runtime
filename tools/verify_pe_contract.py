#!/usr/bin/env python3
"""
PE import/export contract for the shipped BugSplat64.dll.

The game statically imports BugSplat64.dll, and plugins and the launcher resolve
NEVR_* and module exports from it by name. This check pins that surface so a
change of which target owns the code cannot silently drop, rename or renumber an
export.

Checks (all fail-closed; an unreadable or empty table is a failure, never a pass):

  1. EXPORTS   - the export table equals EXPECTED_EXPORTS exactly, name AND
                 ordinal (26 entries). A missing export breaks a caller; an extra
                 export is surface nobody reviewed; a renumbered export is a
                 change of the ordinal table nobody asked for.
  2. ORDINAL 1 - DetoursExportPlaceholder is ordinal 1 (exports.def `@1`); a
                 suspended-process loader needs it.
  3. NO EAGER RUNTIME IMPORT - the host does not statically import nevr.dll. An
                 eager import would stop the host loading when the runtime is
                 absent.
  4. GAME IMPORTS (only with --game-exe) - every symbol echovr.exe imports from
                 BugSplat64.dll is imported BY NAME, equals the pinned
                 GAME_IMPORTS set, and is exported by the DLL.

The table parser reads `objdump -p` output. A PE export table carries no types,
so signatures and calling conventions of the undecorated C exports are NOT
covered; this checker pins names and ordinals only.

Exit 0 = contract holds. Exit 1 = violation or unreadable input.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

# The export table of build/mingw-release/bin/BugSplat64.dll, from
# `x86_64-w64-mingw32-objdump -p`. Ordinals other than DetoursExportPlaceholder@1
# are assigned by the linker in name order, so adding or removing any export
# renumbers the ones after it; the game imports by name, and this pin makes such
# a renumbering a reviewed change instead of a surprise.
EXPECTED_EXPORTS = {
    "DetoursExportPlaceholder": 1,
    "??0MiniDmpSender@@QEAA@PEB_W000K@Z": 2,
    "??1MiniDmpSender@@UEAA@XZ": 3,
    "?createReport@MiniDmpSender@@QEAAXPEAU_EXCEPTION_POINTERS@@@Z": 4,
    "?getFlags@MiniDmpSender@@QEBAKXZ": 5,
    "?resetAppIdentifier@MiniDmpSender@@QEAAXPEB_W@Z": 6,
    "?sendAdditionalFile@MiniDmpSender@@QEAAXPEB_W@Z": 7,
    "?setDefaultUserDescription@MiniDmpSender@@QEAAXPEB_W@Z": 8,
    "?setDefaultUserName@MiniDmpSender@@QEAAXPEB_W@Z": 9,
    "?setFlags@MiniDmpSender@@QEAA_NK@Z": 10,
    "MiniDumpWriteDump": 11,
    "NEVR_DeregisterResourceOverrides": 12,
    "NEVR_GetUPnPConfig": 13,
    "NEVR_RegisterResourceOverride": 14,
    "NEVR_ResetResourceOverrides": 15,
    "NEVR_ScheduleReturnToLobby": 16,
    "NEVR_SetGameModule": 17,
    "TokenAuth_GetDiscordId": 18,
    "TokenAuth_GetToken": 19,
    "TokenAuth_GetUsername": 20,
    "platform_compat_ApiVersion": 21,
    "platform_compat_Init": 22,
    "platform_compat_Shutdown": 23,
    "token_auth_ApiVersion": 24,
    "token_auth_Init": 25,
    "token_auth_Shutdown": 26,
}

ORDINAL_ONE = "DetoursExportPlaceholder"
RUNTIME_DLL = "nevr.dll"
HOST_DLL = "BugSplat64.dll"

# echovr.exe sha256 b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043
# (PE timestamp 0x6452dff6): nine named imports from BugSplat64.dll, no ordinal imports.
GAME_IMPORTS = frozenset({
    "?setDefaultUserDescription@MiniDmpSender@@QEAAXPEB_W@Z",
    "?setDefaultUserName@MiniDmpSender@@QEAAXPEB_W@Z",
    "?setFlags@MiniDmpSender@@QEAA_NK@Z",
    "?getFlags@MiniDmpSender@@QEBAKXZ",
    "?createReport@MiniDmpSender@@QEAAXPEAU_EXCEPTION_POINTERS@@@Z",
    "??1MiniDmpSender@@UEAA@XZ",
    "??0MiniDmpSender@@QEAA@PEB_W000K@Z",
    "?resetAppIdentifier@MiniDmpSender@@QEAAXPEB_W@Z",
    "?sendAdditionalFile@MiniDmpSender@@QEAAXPEB_W@Z",
})

OBJDUMP_CANDIDATES = ("x86_64-w64-mingw32-objdump", "objdump")

# `\t[   0] +base[   1]  0009 DetoursExportPlaceholder`
_EXPORT_ROW = re.compile(r"^\s*\[\s*\d+\]\s+\+base\[\s*(\d+)\]\s+[0-9a-fA-F]+\s+(\S+)\s*$")
# By-name import row: `\t016c3080  <none>  001f  ?setFlags@MiniDmpSender@@QEAA_NK@Z`
_NAMED_IMPORT_ROW = re.compile(r"^\s*[0-9a-fA-F]+\s+<none>\s+[0-9a-fA-F]+\s+(\S+)")


class ContractError(Exception):
    """The PE contract does not hold, or its input could not be read."""


def parse_exports(objdump_text):
    """Return {name: ordinal} from the `[Ordinal/Name Pointer] Table` section."""
    lines = objdump_text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines)
                     if line.strip().startswith("[Ordinal/Name Pointer] Table"))
    except StopIteration as missing:
        raise ContractError("no [Ordinal/Name Pointer] Table in objdump output "
                            "(no export table, or objdump format changed)") from missing
    exports = {}
    for line in lines[start + 1:]:
        if not line.strip():
            if exports:
                break
            continue
        row = _EXPORT_ROW.match(line)
        if row is None:
            if "Ordinal" in line and "Name" in line:
                continue  # column header
            raise ContractError(f"unparseable export row: {line!r}")
        ordinal, name = int(row.group(1)), row.group(2)
        if name in exports:
            raise ContractError(f"duplicate export name {name}")
        exports[name] = ordinal
    if not exports:
        raise ContractError("export table parsed to zero rows")
    return exports


def parse_import_dlls(objdump_text):
    """Return {lower-case DLL name: [(raw-row-or-None, imported-name-or-None), ...]}.

    By-name rows are (None, name); any other row is (raw text, None)."""
    imports = {}
    current = None
    for line in objdump_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("DLL Name:"):
            current = stripped.split(":", 1)[1].strip().lower()
            imports.setdefault(current, [])
            continue
        if current is None:
            continue
        if not stripped:
            current = None
            continue
        if stripped.startswith("vma:"):
            continue
        row = _NAMED_IMPORT_ROW.match(line)
        if row is None:
            # Anything that is not a by-name row (an ordinal import, or a format
            # this parser does not know) is kept verbatim so check_game rejects it
            # instead of the row silently vanishing.
            imports[current].append((stripped, None))
        else:
            imports[current].append((None, row.group(1)))
    return imports


def check_host(objdump_text):
    """Checks 1-3 against the host DLL. Returns the export map on success."""
    exports = parse_exports(objdump_text)
    names = set(exports)
    missing = sorted(set(EXPECTED_EXPORTS) - names)
    extra = sorted(names - set(EXPECTED_EXPORTS))
    if missing or extra:
        raise ContractError(f"export set drifted: missing={missing} extra={extra}")
    if exports[ORDINAL_ONE] != 1:
        raise ContractError(f"{ORDINAL_ONE} is ordinal {exports[ORDINAL_ONE]}, expected 1")
    renumbered = sorted(f"{name}: expected {EXPECTED_EXPORTS[name]}, found {exports[name]}"
                        for name in EXPECTED_EXPORTS if exports[name] != EXPECTED_EXPORTS[name])
    if renumbered:
        raise ContractError(f"export ordinals drifted: {renumbered}")
    imports = parse_import_dlls(objdump_text)
    if not imports:
        raise ContractError("import table parsed to zero DLLs")
    if RUNTIME_DLL in imports:
        raise ContractError(f"host statically imports {RUNTIME_DLL}; the runtime must be "
                            "loaded lazily so the host survives its absence")
    return exports


def check_game(objdump_text, exports):
    """Check 4: the game's BugSplat64.dll imports are by name, pinned and exported."""
    imports = parse_import_dlls(objdump_text)
    rows = imports.get(HOST_DLL.lower())
    if not rows:
        raise ContractError(f"game executable imports nothing from {HOST_DLL}")
    not_by_name = [raw for raw, name in rows if name is None]
    if not_by_name:
        raise ContractError(f"game has non-by-name {HOST_DLL} import rows "
                            f"(ordinal import or unknown format): {not_by_name}")
    names = {row[1] for row in rows}
    if names != GAME_IMPORTS:
        raise ContractError("game import set drifted from the pinned set: "
                            f"missing={sorted(GAME_IMPORTS - names)} "
                            f"extra={sorted(names - GAME_IMPORTS)}")
    unresolved = sorted(names - set(exports))
    if unresolved:
        raise ContractError(f"game imports not exported by host: {unresolved}")
    return names


def run_objdump(path):
    objdump = next((tool for tool in OBJDUMP_CANDIDATES if shutil.which(tool)), None)
    if objdump is None:
        raise ContractError(f"no objdump found (tried {', '.join(OBJDUMP_CANDIDATES)})")
    result = subprocess.run([objdump, "-p", str(path)], capture_output=True, text=True,
                            check=False)
    if result.returncode != 0:
        raise ContractError(f"{objdump} -p {path} exited {result.returncode}: "
                            f"{result.stderr.strip()}")
    return result.stdout


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    parser.add_argument("--dll", required=True, type=pathlib.Path,
                        help="built BugSplat64.dll (host) to check")
    parser.add_argument("--game-exe", type=pathlib.Path,
                        help="echovr.exe; when given, its BugSplat64.dll imports are checked")
    args = parser.parse_args(argv)
    try:
        if not args.dll.is_file():
            raise ContractError(f"DLL not found: {args.dll}")
        exports = check_host(run_objdump(args.dll))
        game_note = "game-imports=not-checked (no --game-exe)"
        if args.game_exe is not None:
            if not args.game_exe.is_file():
                raise ContractError(f"game executable not found: {args.game_exe}")
            names = check_game(run_objdump(args.game_exe), exports)
            game_note = f"game-imports={len(names)} all-exported by-name"
    except ContractError as error:
        print(f"pe-contract: FAIL - {error}", file=sys.stderr)
        return 1
    print(f"pe-contract: OK host-exports={len(exports)} {ORDINAL_ONE}=ordinal-1 "
          f"no-{RUNTIME_DLL}-import {game_note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
