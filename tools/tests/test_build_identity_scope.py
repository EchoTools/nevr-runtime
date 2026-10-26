"""The commit and the version reach only the translation units that read them (#930).

A new commit, or an uncommitted edit (git describe says `-dirty`), changes the build identity. Every compile
command that carries it is then out of date: when the definitions were global (`add_compile_definitions`) that was
every compile edge, 375 of 480 steps of the full tree (and every precompiled header), for a change nobody
reads. The identity is now defined per source file, on the files that read it, so a new commit recompiles those and
relinks.

The tests read the `build.ninja` of the configured tree (like test_build_identity_literal.py, they skip with no tree)
and the sources it compiles:

  - a compile edge carries exactly the identity macros its source file names, and no other;
  - the commit, the short commit and the describe string appear on no edge but those compile edges (no include
    path, no link line, no generated header can carry them past the check above);
  - the macros are read only in the .cpp/.c files, so a definition per source file is enough: a header that read one
    would be compiled into files that were given none (globals.h holds the fallbacks only)."""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD = REPO / "build" / "mingw-release"
NINJA = BUILD / "build.ninja"

IDENTITY_MACROS = (
    "NEVR_PROJECT_VERSION",
    "NEVR_PROJECT_VERSION_MAJOR",
    "NEVR_PROJECT_VERSION_MINOR",
    "NEVR_PROJECT_VERSION_PATCH",
    "NEVR_PROJECT_VERSION_TWEAK",
    "NEVR_GIT_COMMIT_HASH",
    "NEVR_GIT_COMMIT_FULL",
    "NEVR_GIT_DESCRIBE",
    # the unprefixed spellings the frozen src/legacy sources read
    "PROJECT_VERSION",
    "GIT_COMMIT_HASH",
)
MACRO = re.compile(r"\b(" + "|".join(IDENTITY_MACROS) + r")\b")
DEFINED_ON_COMMAND_LINE = re.compile(r"-D(" + "|".join(IDENTITY_MACROS) + r")=")
COMMENTS = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
EDGE = re.compile(r"^build (?P<outputs>[^:]+?): (?P<rule>\S+) (?P<inputs>[^|\n]*)", re.M)
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")


def edges():
    """(outputs, rule, first input, whole text) of every build statement of build.ninja."""
    text = NINJA.read_text(errors="replace")
    starts = [m.start() for m in re.finditer(r"^build ", text, re.M)] + [len(text)]
    for begin, end in zip(starts, starts[1:]):
        block = text[begin:end]
        m = EDGE.match(block)
        if m:
            inputs = m.group("inputs").split()
            yield m.group("outputs"), m.group("rule"), (inputs[0] if inputs else ""), block


def named_in(path: pathlib.Path) -> set[str]:
    """The identity macros a source file names outside its comments."""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return set()
    return set(MACRO.findall(COMMENTS.sub("", text)))


def source_path(first_input: str) -> pathlib.Path:
    p = pathlib.Path(first_input)
    return p if p.is_absolute() else BUILD / p


def compile_edges():
    for outputs, rule, first, block in edges():
        if "COMPILER" in rule and not rule.startswith("RC_") and "RC_COMPILER" not in rule:
            yield outputs, first, block


def is_dist(outputs: str, rule: str) -> bool:
    """The dist-* utility targets name their archives after the version (nevr-runtime-v<version>-windows-server.zip);
    they are not part of `all`, so a new commit does not run them."""
    return rule == "CUSTOM_COMMAND" and "CMakeFiles/dist" in outputs


def defined_on(block: str) -> set[str]:
    return set(DEFINED_ON_COMMAND_LINE.findall(block))


@unittest.skipUnless(NINJA.exists(), "no configured tree in build/mingw-release")
class IdentityScope(unittest.TestCase):
    def test_a_compile_edge_carries_exactly_the_identity_macros_its_source_names(self):
        wrong = []
        seen = 0
        for outputs, first, block in compile_edges():
            seen += 1
            want = named_in(source_path(first)) if first.endswith(SOURCE_SUFFIXES) else set()
            have = defined_on(block)
            if have != want:
                wrong.append((outputs, sorted(have - want), sorted(want - have)))
        self.assertGreater(seen, 100, "the parser found too few compile edges to mean anything")
        self.assertEqual(wrong[:5], [], f"{len(wrong)} compile edges carry other macros than their source names "
                                        "(outputs, carried but not named, named but not carried)")

    def test_the_known_readers_are_given_what_they_read(self):
        carried = {}
        for outputs, first, block in compile_edges():
            if first.endswith(SOURCE_SUFFIXES) and defined_on(block):
                carried.setdefault(pathlib.Path(first).name, set()).update(defined_on(block))
        self.assertEqual(carried.get("build_identity.cpp"),
                         {"NEVR_PROJECT_VERSION", "NEVR_GIT_COMMIT_HASH", "NEVR_GIT_COMMIT_FULL", "NEVR_GIT_DESCRIBE"})
        self.assertEqual(carried.get("ws_bridge.cpp"), {"NEVR_PROJECT_VERSION"})
        self.assertEqual(carried.get("initialize.cpp"), {"NEVR_PROJECT_VERSION"})
        self.assertEqual(carried.get("patches.cpp"), {"PROJECT_VERSION", "GIT_COMMIT_HASH"})
        self.assertEqual(carried.get("gameserver.cpp"), {"PROJECT_VERSION", "GIT_COMMIT_HASH"})
        self.assertEqual(set(carried), {"build_identity.cpp", "ws_bridge.cpp", "initialize.cpp", "patches.cpp", "gameserver.cpp"},
                         "another file now reads the identity: give it its definition in its CMakeLists.txt")

    def test_every_edge_that_defines_a_macro_gives_it_the_same_value_and_the_values_belong_together(self):
        values: dict[str, set[str]] = {}
        for _, first, block in compile_edges():
            for name in IDENTITY_MACROS:
                for m in re.finditer(rf'-D{name}=\\"([^"\\]+)\\"', block):
                    values.setdefault(name, set()).add(m.group(1))
        one = {name: next(iter(v)) for name, v in values.items() if len(v) == 1}
        self.assertEqual({n: sorted(v) for n, v in values.items() if len(v) != 1}, {}, "one macro, two values")
        full, short = one["NEVR_GIT_COMMIT_FULL"], one["NEVR_GIT_COMMIT_HASH"]
        self.assertRegex(full, r"^[0-9a-f]{40}$")
        self.assertRegex(short, r"^[0-9a-f]{7,39}$", "NEVR_GIT_COMMIT_HASH is the abbreviated commit")
        self.assertTrue(full.startswith(short), f"the short commit {short} is not the start of {full}")
        # the legacy spellings are the same values as the NEVR_ ones
        self.assertEqual(one["GIT_COMMIT_HASH"], short)
        self.assertEqual(one["PROJECT_VERSION"], one["NEVR_PROJECT_VERSION"])
        self.assertRegex(one["NEVR_PROJECT_VERSION"], r"^\d+\.\d+\.\d+(-dev\.\d+\+[0-9a-f]{7,40})?$")
        # `git describe --tags --always --dirty`: a tag, or <tag>-<N>-g<abbreviated commit>, or the abbreviated commit
        # alone; "-dirty" may follow. Never the full commit and never a version.
        described = one["NEVR_GIT_DESCRIBE"].removesuffix("-dirty")
        at = re.search(r"-g([0-9a-f]+)$", described)
        self.assertTrue(
            (at and full.startswith(at.group(1))) or re.fullmatch(r"v\d+\.\d+\.\d+", described)
            or (re.fullmatch(r"[0-9a-f]{7,12}", described) and full.startswith(described)),
            f"{described!r} is not what git describe --tags --always gives for {full}")

    def test_the_commit_and_the_describe_string_are_on_no_edge_but_those_compile_edges(self):
        values = set()
        for _, first, block in compile_edges():
            for name in ("NEVR_GIT_COMMIT_FULL", "NEVR_GIT_COMMIT_HASH", "NEVR_GIT_DESCRIBE"):
                m = re.search(rf'-D{name}=\\"([^"\\]+)\\"', block)
                if m:
                    values.add(m.group(1))
        self.assertTrue(values, "no edge carries the commit: the tree has no reader at all")
        stray = [outputs for outputs, rule, first, block in edges()
                 if any(v in block for v in values)
                 and not (("COMPILER" in rule) and first.endswith(SOURCE_SUFFIXES) and defined_on(block))
                 and not is_dist(outputs, rule)]
        self.assertEqual(stray[:5], [], f"{len(stray)} edges other than the readers' compile edges carry the commit")


class HeadersDoNotRead(unittest.TestCase):
    """A definition per source file reaches the files that name the macro. A header that read one would be
    compiled into files that got none."""

    def test_only_globals_h_names_an_identity_macro_outside_a_comment_and_only_to_default_it(self):
        offenders = {}
        for suffix in ("*.h", "*.hpp"):
            for path in sorted((REPO / "src").rglob(suffix)) + sorted((REPO / "plugins").rglob(suffix)):
                rel = path.relative_to(REPO).as_posix()
                if rel.startswith("src/legacy/") or rel.startswith("src/quest/"):
                    continue
                names = named_in(path)
                if names:
                    offenders[rel] = sorted(names)
        self.assertEqual(sorted(offenders), ["src/core/globals.h"], offenders)
        text = COMMENTS.sub("", (REPO / "src/core/globals.h").read_text())
        for line in text.splitlines():
            if MACRO.search(line):
                self.assertRegex(line, r"^\s*(#ifndef|#define)\s", f"globals.h may only default the macros: {line!r}")


if __name__ == "__main__":
    unittest.main()
