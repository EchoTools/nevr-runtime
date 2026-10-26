#!/usr/bin/env python3
"""The thin host carries no writable state of the runtime's core or ABI libraries (#853, ADR 0012 decision 4).

BugSplat64.dll and nevr-runtime.dll are separate images. A global from libnevr_core.a or libnevr_abi.a linked into
both would be two variables with one name: `g_isServer` set by the runtime's command-line parse and read as false by
code in the host. The host links only minhook and nlohmann_json, so none of those libraries' objects should be in it;
this check says so from the symbols of the UNSTRIPPED host (the build's own bin/BugSplat64.dll, before dist strips it).

  usage: verify_host_no_core_state.py --host-dll <BugSplat64.dll> --lib <libnevr_core.a> [--lib <libnevr_abi.a> ...]
                                      [--nm <x86_64-w64-mingw32-nm>]

A writable data symbol is an nm type B, b, D, d or C (zero-initialised, initialised and common data, global or file
local); function-local statics and their guard variables are among them. The names are compared as nm prints them,
mangled. Exit 0: no name is defined as writable data by both the host and a library. Exit 1: one is (named), or the
check could not look: nm failed, a library or the host defines no writable data (an empty comparison passes
nothing: a stripped host, a wrong path, an archive nm cannot read).
"""

import argparse
import subprocess
import sys

DATA_TYPES = frozenset("BbDdCc")


class SensorError(Exception):
    pass


def data_symbols(nm_output):
    """The names of the writable data symbols in raw `nm --defined-only` output. Section symbols (.data) are not names."""
    names = set()
    for line in nm_output.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3 or len(parts[1]) != 1 or parts[1] not in DATA_TYPES:
            continue
        if parts[2].startswith("."):
            continue
        names.add(parts[2].strip())
    return names


def run_nm(nm, path):
    result = subprocess.run([nm, "--defined-only", str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        raise SensorError(f"nm exited {result.returncode} on {path}: {result.stderr.strip()}")
    return result.stdout


def shared_state(host_symbols, library_symbols_by_name):
    """{library: sorted names} that the host also defines as writable data."""
    return {lib: sorted(host_symbols & names) for lib, names in library_symbols_by_name.items() if host_symbols & names}


def check(host_listing, library_listings):
    """`library_listings` is {label: nm output}. Raises SensorError when the host defines a writable symbol one of the libraries also defines,
    or when a comparison saw nothing."""
    host = data_symbols(host_listing)
    if not host:
        raise SensorError("the host defines no writable data symbols: it is stripped, or this is not the host "
                          "(run it on the unstripped build/<preset>/bin/BugSplat64.dll)")
    libraries = {}
    for label, listing in library_listings.items():
        names = data_symbols(listing)
        if not names:
            raise SensorError(f"{label} defines no writable data symbols: nothing to compare against")
        libraries[label] = names
    shared = shared_state(host, libraries)
    if shared:
        lines = [f"  {lib}: {', '.join(names)}" for lib, names in sorted(shared.items())]
        raise SensorError("the host defines writable data that the runtime's core or ABI library also defines "
                          "(two variables with one name across two images):\n" + "\n".join(lines))
    return len(host), {label: len(names) for label, names in libraries.items()}


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host-dll", required=True)
    parser.add_argument("--lib", action="append", required=True, dest="libs")
    parser.add_argument("--nm", default="x86_64-w64-mingw32-nm")
    args = parser.parse_args(argv)
    try:
        host_listing = run_nm(args.nm, args.host_dll)
        library_listings = {lib: run_nm(args.nm, lib) for lib in args.libs}
        host_count, library_counts = check(host_listing, library_listings)
    except (SensorError, OSError) as error:
        print(f"verify_host_no_core_state: FAIL: {error}")
        return 1
    counts = ", ".join(f"{lib}={n}" for lib, n in sorted(library_counts.items()))
    print(f"verify_host_no_core_state: OK host-data-symbols={host_count} none shared with: {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
