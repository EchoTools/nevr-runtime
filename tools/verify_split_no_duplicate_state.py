#!/usr/bin/env python3
"""BugSplat64/nevr split (step 5): no duplicate static state across the pair.

The design's explicit risk is that the host and the runtime each resolve the
other's static state — globals, MinHook, singletons — through their own copy,
which breaks hook ownership or double-initializes. The property is a LINK-level
one (which static archives each DLL pulls in), so it is verified from the built
artifacts with `nm`:

  - the runtime-owned globals (g_isServer, g_pGame, g_isHeadless) must be
    DEFINED in nevr.dll and ABSENT from BugSplat64.dll — i.e. nevr_core / the
    runtime sources are linked only into the runtime, never duplicated into the
    host;
  - MinHook (MH_Initialize) must be present in BOTH, as two SEPARATE copies —
    the host owns the PreprocessCommandLine detour and the runtime owns its
    disjoint hooks (the rendezvous harness proves the disjoint targets).

Exit 0 = the split holds. Exit 1 = a violation or an unreadable artifact.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

NM_CANDIDATES = ("x86_64-w64-mingw32-nm", "nm")

# Runtime-owned state that must NOT be duplicated into the host. g_isServer lives
# in src/runtime/lifecycle/cli.cpp, g_pGame in src/runtime/lifecycle/config.cpp,
# g_isHeadless in src/core/globals.cpp — so together they cover both the runtime
# sources and the nevr_core archive.
RUNTIME_ONLY_SYMBOLS = ("g_isServer", "g_pGame", "g_isHeadless")

# MinHook's own state (trampoline pool, frozen-thread list) is per-DLL. Both DLLs
# must carry a copy — separate, so hook ownership is not shared — but the host
# must never link nevr_core/nevr_abi's state.
SHARED_MINHOOK_SYMBOL = "MH_Initialize"


class SplitError(Exception):
    """The no-duplicate-state property does not hold, or input is unreadable."""


def run_nm(path: Path) -> str:
    nm = next((tool for tool in NM_CANDIDATES if shutil.which(tool)), None)
    if nm is None:
        raise SplitError(f"no nm found (tried {', '.join(NM_CANDIDATES)})")
    result = subprocess.run([nm, str(path)], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SplitError(f"{nm} {path} exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def defined_symbols(nm_text: str) -> set[str]:
    # nm rows look like: `00000002f93f722c B g_isServer`. The symbol name is the
    # last whitespace-separated token; `.refptr.` and `.rdata$` rows carry the
    # name as a suffix, so match on the exact name token rather than the line.
    symbols = set()
    for line in nm_text.splitlines():
        tokens = line.split()
        if not tokens:
            continue
        name = tokens[-1]
        # strip the MinGW `.refptr.X` / relocation-artifact suffixes
        if name.startswith(".refptr."):
            name = name[len(".refptr."):]
        if name.startswith(".rdata$."):
            name = name[len(".rdata$."):]
        if "." in name:
            name = name.rsplit(".", 1)[-1]
        symbols.add(name)
    return symbols


def check(host: Path, runtime: Path) -> str:
    host_symbols = defined_symbols(run_nm(host))
    runtime_symbols = defined_symbols(run_nm(runtime))

    duplicated = [s for s in RUNTIME_ONLY_SYMBOLS if s in host_symbols]
    if duplicated:
        raise SplitError(f"runtime-only state duplicated into the host: {duplicated}")

    missing = [s for s in RUNTIME_ONLY_SYMBOLS if s not in runtime_symbols]
    if missing:
        raise SplitError(f"runtime-owned state missing from nevr.dll: {missing}")

    if SHARED_MINHOOK_SYMBOL not in host_symbols:
        raise SplitError(f"{SHARED_MINHOOK_SYMBOL} missing from the host; it must own its MinHook copy")
    if SHARED_MINHOOK_SYMBOL not in runtime_symbols:
        raise SplitError(f"{SHARED_MINHOOK_SYMBOL} missing from the runtime; it must own its MinHook copy")

    return (f"no-duplicate-state: OK host-has-no-{','.join(RUNTIME_ONLY_SYMBOLS)} "
            f"runtime-defines-{len(RUNTIME_ONLY_SYMBOLS)} minhook-in-both")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    parser.add_argument("--host", required=True, type=Path, help="built BugSplat64.dll")
    parser.add_argument("--runtime", required=True, type=Path, help="built nevr.dll")
    args = parser.parse_args(argv)
    try:
        if not args.host.is_file():
            raise SplitError(f"host DLL not found: {args.host}")
        if not args.runtime.is_file():
            raise SplitError(f"runtime DLL not found: {args.runtime}")
        print(check(args.host, args.runtime))
        return 0
    except SplitError as error:
        print(f"no-duplicate-state: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
