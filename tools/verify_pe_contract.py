#!/usr/bin/env python3
"""
PE import/export contract for the shipped BugSplat64.dll host and nevr-runtime.dll.

The game statically imports BugSplat64.dll, and plugins and the launcher resolve
NEVR_* and module exports from it by name. This check pins that surface so a
change of which target owns the code cannot silently drop, rename or renumber an
export.

Checks (all fail-closed; an unreadable or empty table is a failure, never a pass):

  1. EXPORTS   - the export table equals EXPECTED_EXPORTS exactly, name AND
                 ordinal (28 entries). A missing export breaks a caller; an extra
                 export is surface nobody reviewed; a renumbered export is a
                 change of the ordinal table nobody asked for.
  2. ORDINAL 1 - DetoursExportPlaceholder is ordinal 1 (exports.def `@1`); a
                 suspended-process loader needs it.
  3. NO EAGER RUNTIME IMPORT - the host does not statically import nevr-runtime.dll. An
                 eager import would stop the host loading when the runtime is
                 absent.
  4. GAME IMPORTS (only with --game-exe) - every symbol echovr.exe imports from
                 BugSplat64.dll is imported BY NAME, equals the pinned
                 GAME_IMPORTS set, and is exported by the DLL.
  5. THIN HOST - the host carries no runtime code: its imports are a subset of
                 HOST_ALLOWED_IMPORTS, none of RUNTIME_ONLY_MARKERS is in the file, it is linked
                 at HOST_IMAGE_BASE (N146), its SizeOfImage stays under HOST_MAX_SIZE_OF_IMAGE, and it carries its own
                 NEVR-BUILD literal.
  6. RUNTIME     - nevr-runtime.dll exports NEVR_GetRuntimeApi (the rendezvous entry), none
                 of the names the game imports from the host, carries exactly one NEVR-BUILD
                 literal, imports nothing from the host, and is linked at RUNTIME_IMAGE_BASE.
  7. PAIR        - the two files state one version and one commit, and their image ranges
                 do not overlap.

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
    "NEVR_DeregisterAssets": 12,
    "NEVR_DeregisterResourceOverrides": 13,
    "NEVR_GetUPnPConfig": 14,
    "NEVR_RegisterAsset": 15,
    "NEVR_RegisterResourceOverride": 16,
    "NEVR_ResetResourceOverrides": 17,
    "NEVR_ScheduleReturnToLobby": 18,
    "NEVR_SetGameModule": 19,
    "TokenAuth_GetDiscordId": 20,
    "TokenAuth_GetToken": 21,
    "TokenAuth_GetUsername": 22,
    "platform_compat_ApiVersion": 23,
    "platform_compat_Init": 24,
    "platform_compat_Shutdown": 25,
    "token_auth_ApiVersion": 26,
    "token_auth_Init": 27,
    "token_auth_Shutdown": 28,
}

ORDINAL_ONE = "DetoursExportPlaceholder"
RUNTIME_DLL = "nevr-runtime.dll"
HOST_DLL = "BugSplat64.dll"
RUNTIME_ENTRY = "NEVR_GetRuntimeApi"
# Everything nevr-runtime.dll may export: the rendezvous entry and the two static modules' entry points
# (NEVR_MODULE_API in src/extension/module_interface.h). The host's sixteen thunks reach the runtime through the
# descriptor's table (core/runtime_abi.h), so the runtime exports no NEVR_* or TokenAuth_* name of its own.
RUNTIME_EXPORTS = frozenset({
    RUNTIME_ENTRY,
    "TokenAuth_GetDiscordId", "TokenAuth_GetToken", "TokenAuth_GetUsername",
    "platform_compat_ApiVersion", "platform_compat_Init", "platform_compat_Shutdown",
    "token_auth_ApiVersion", "token_auth_Init", "token_auth_Shutdown",
})

# N146: the host keeps the stock BugSplat64.dll's image base (the game's graphics DLLs fail to load otherwise).
# The runtime has a base of its own, inside the range the one-file DLL used to occupy, so the loader never
# has to choose between the two and nothing new is asked of the address space.
HOST_IMAGE_BASE = 0x180000000
RUNTIME_IMAGE_BASE = 0x181000000

# The identity both files carry (src/core/build_identity.cpp, src/runtime/host/host_identity.cpp).
IDENTITY = re.compile(rb"(?<=\x00)NEVR-BUILD ([0-9A-Za-z.+-]+) ([0-9a-f]{40})(?=\x00)")

# What the thin host may import: kernel32 and the UCRT. Anything else is code the host has no business running
# inside a crash path (sockets, TLS, COM, the shell, the registry).
HOST_ALLOWED_IMPORTS = ("kernel32.dll",)
HOST_ALLOWED_IMPORT_PREFIXES = ("api-ms-win-crt-",)
# Text that only the runtime carries: a plugin trust literal, a log tag of the bridge, a library's name.
RUNTIME_ONLY_MARKERS = (b"NEVR-PLUGIN-TRUST ", b"[NEVR.PLUGIN]", b"[NEVR.WS]", b"ixwebsocket")
# The unstripped host (with its debug sections) measures 0x276000; the runtime's image is 0x1c30000.
HOST_MAX_SIZE_OF_IMAGE = 0x400000

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
_IMAGE_BASE = re.compile(r"^ImageBase\s+([0-9a-fA-F]+)\s*$", re.MULTILINE)
_SIZE_OF_IMAGE = re.compile(r"^SizeOfImage\s+([0-9a-fA-F]+)\s*$", re.MULTILINE)


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


def parse_image(objdump_text):
    """Return {"base": ImageBase, "size": SizeOfImage} from the optional header."""
    base, size = _IMAGE_BASE.search(objdump_text), _SIZE_OF_IMAGE.search(objdump_text)
    if base is None or size is None:
        raise ContractError("no ImageBase / SizeOfImage in objdump output (objdump format changed?)")
    return {"base": int(base.group(1), 16), "size": int(size.group(1), 16)}


def identity_in(blob, label):
    """The (version, commit) of the one NEVR-BUILD literal in `blob`."""
    found = IDENTITY.findall(blob)
    if len(found) != 1:
        raise ContractError(f"{label}: expected exactly one NEVR-BUILD literal, found {len(found)}")
    return found[0][0].decode("ascii"), found[0][1].decode("ascii")


def check_host_is_thin(objdump_text, blob):
    """Check 5: the host carries the exports and no runtime code."""
    imports = parse_import_dlls(objdump_text)
    for dll in sorted(imports):
        allowed = dll in HOST_ALLOWED_IMPORTS or dll.startswith(HOST_ALLOWED_IMPORT_PREFIXES)
        if not allowed:
            raise ContractError(f"host imports {dll}; the host may import only {', '.join(HOST_ALLOWED_IMPORTS)} "
                                f"and {', '.join(p + '*' for p in HOST_ALLOWED_IMPORT_PREFIXES)} "
                                "(runtime code does not belong in the host)")
    present = [marker.decode("ascii") for marker in RUNTIME_ONLY_MARKERS if marker in blob]
    if present:
        raise ContractError(f"host carries runtime-only text: {present}")
    image = parse_image(objdump_text)
    if image["base"] != HOST_IMAGE_BASE:
        raise ContractError(f"host image base is {image['base']:#x}, expected {HOST_IMAGE_BASE:#x} "
                            "(N146: the DLL the game imports keeps the stock image base)")
    if image["size"] > HOST_MAX_SIZE_OF_IMAGE:
        raise ContractError(f"host SizeOfImage is {image['size']:#x}, over the {HOST_MAX_SIZE_OF_IMAGE:#x} cap "
                            "(runtime code does not belong in the host)")
    identity_in(blob, HOST_DLL)
    return image


def check_runtime(objdump_text, blob):
    """Check 6: nevr-runtime.dll has the rendezvous entry, the identity and an image base of its own."""
    exports = parse_exports(objdump_text)
    if RUNTIME_ENTRY not in exports:
        raise ContractError(f"{RUNTIME_DLL} does not export {RUNTIME_ENTRY}; the host cannot rendezvous with it")
    shadowing = sorted(set(exports) & GAME_IMPORTS)
    if shadowing:
        raise ContractError(f"{RUNTIME_DLL} exports names the game imports from the host: {shadowing}")
    unexpected = sorted(set(exports) - RUNTIME_EXPORTS)
    if unexpected:
        raise ContractError(f"{RUNTIME_DLL} exports names it may not: {unexpected}; the host owns the plugin-facing "
                            "names and reaches the runtime through the descriptor's table")
    imports = parse_import_dlls(objdump_text)
    if HOST_DLL.lower() in imports:
        raise ContractError(f"{RUNTIME_DLL} imports {HOST_DLL}; the dependency runs from the host to the runtime "
                            "by the rendezvous only")
    image = parse_image(objdump_text)
    if image["base"] != RUNTIME_IMAGE_BASE:
        raise ContractError(f"{RUNTIME_DLL} image base is {image['base']:#x}, expected {RUNTIME_IMAGE_BASE:#x} "
                            f"(the host's is {HOST_IMAGE_BASE:#x})")
    identity_in(blob, RUNTIME_DLL)
    return image


def check_ranges(host_image, runtime_image):
    """Check 7a: the two images do not overlap."""
    host_end = host_image["base"] + host_image["size"]
    runtime_end = runtime_image["base"] + runtime_image["size"]
    if host_image["base"] < runtime_end and runtime_image["base"] < host_end:
        raise ContractError(f"the host [{host_image['base']:#x}, {host_end:#x}) and the runtime "
                            f"[{runtime_image['base']:#x}, {runtime_end:#x}) overlap")


def check_pair(host_blob, runtime_blob):
    """Check 7b: one version and one commit across the two files."""
    host_version, host_commit = identity_in(host_blob, HOST_DLL)
    runtime_version, runtime_commit = identity_in(runtime_blob, RUNTIME_DLL)
    if host_version != runtime_version:
        raise ContractError(f"the pair states a different version: host {host_version}, runtime {runtime_version}")
    if host_commit != runtime_commit:
        raise ContractError(f"the pair states a different commit: host {host_commit}, runtime {runtime_commit}")
    return host_version, host_commit


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
    parser.add_argument("--runtime-dll", required=True, type=pathlib.Path,
                        help="built nevr-runtime.dll to check")
    parser.add_argument("--game-exe", type=pathlib.Path,
                        help="echovr.exe; when given, its BugSplat64.dll imports are checked")
    args = parser.parse_args(argv)
    try:
        for path in (args.dll, args.runtime_dll):
            if not path.is_file():
                raise ContractError(f"DLL not found: {path}")
        host_text = run_objdump(args.dll)
        exports = check_host(host_text)
        host_image = check_host_is_thin(host_text, args.dll.read_bytes())
        runtime_text = run_objdump(args.runtime_dll)
        runtime_image = check_runtime(runtime_text, args.runtime_dll.read_bytes())
        check_ranges(host_image, runtime_image)
        version, commit = check_pair(args.dll.read_bytes(), args.runtime_dll.read_bytes())
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
          f"no-{RUNTIME_DLL}-import host-thin runtime-entry={RUNTIME_ENTRY} "
          f"runtime-base={runtime_image['base']:#x} identity={version}+{commit[:8]} {game_note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
