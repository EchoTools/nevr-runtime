#!/usr/bin/env python3
"""Print the object files the Quest sentinel's link line pulls in, from build.ninja.

usage: tools/quest_link_objects.py BUILD_DIR OUTPUT

BUILD_DIR is the CMake build directory that holds build.ninja (build/android-arm64), OUTPUT the shared
object's path relative to it (sentinel/libovrplatformloader.so). Each object that is an input of OUTPUT's
link statement is printed, followed by the members of every in-tree static archive it links (and of
the archives those link). The list is what the linker was given, not what `find` sees in the build
tree: an object left behind by a target that was removed is not on it, and an archive's members are.

Archives with no build statement (vcpkg's libcurl.a and the like) are named on stderr, not scanned.
Exits 1 when the link statement is absent, when it names no object, or when a listed object is missing.
"""

import os
import sys


def parse_statements(path):
    """Map each output of a `build` statement to its (explicit inputs, implicit inputs)."""
    statements = {}
    with open(path, encoding="utf-8") as handle:
        text = handle.read().replace("$\n", "")
    for line in text.splitlines():
        if not line.startswith("build "):
            continue
        head, sep, rest = line[len("build "):].partition(": ")
        if not sep:
            continue
        outputs = [tok for tok in head.split(" ") if tok and tok != "|"]
        tokens = rest.split(" ")[1:]  # drop the rule name
        explicit, implicit = [], []
        bucket = explicit
        for tok in tokens:
            if tok == "|":
                bucket = implicit
            elif tok == "||":
                break  # order-only
            elif tok:
                bucket.append(tok)
        for out in outputs:
            statements[out] = (explicit, implicit)
    return statements


def link_objects(build_dir, output):
    statements = parse_statements(os.path.join(build_dir, "build.ninja"))
    if output not in statements:
        raise SystemExit("quest_link_objects: no build statement for %s in %s/build.ninja" % (output, build_dir))
    objects, external, seen = [], [], set()

    def visit(name):
        if name in seen:
            return
        seen.add(name)
        explicit, implicit = statements[name]
        for tok in explicit:
            if tok.endswith(".o"):
                objects.append(tok)
        for tok in explicit + implicit:
            if not tok.endswith(".a"):
                continue
            if tok in statements:
                visit(tok)
            elif tok not in external:
                external.append(tok)

    visit(output)
    if not objects:
        raise SystemExit("quest_link_objects: %s links no object file" % output)
    missing = [o for o in objects if not os.path.exists(os.path.join(build_dir, o))]
    if missing:
        raise SystemExit("quest_link_objects: object file(s) missing from %s: %s" % (build_dir, " ".join(missing)))
    return sorted(set(objects)), external


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__.split("\n\n")[0])
    objects, external = link_objects(argv[1], argv[2])
    for tok in external:
        print("quest_link_objects: not scanned (no build statement): %s" % tok, file=sys.stderr)
    for obj in objects:
        print(os.path.join(argv[1], obj))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
