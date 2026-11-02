"""The commit and the version enter the build in exactly one translation unit (#930, #1935).

A new commit, or an uncommitted edit (git describe says `-dirty`), changes the build identity. Defined as compile
definitions, it changed every compile command that carried it: 375 of 480 steps of the full tree, and every
precompiled header, for a change nobody reads. The identity is now written at build time into ONE generated unit,
build/<preset>/generated/nevr_build_stamp.cpp (cmake/nevr_build_stamp.cmake), which defines the accessors of
src/core/build_stamp.h. No compile command carries a version or a commit, so a new commit recompiles that unit and
relinks.

The tests read the `build.ninja` of the configured tree (they skip with no tree) and the sources:

  - no compile command defines a retired identity macro (NEVR_PROJECT_VERSION*, NEVR_GIT_*);
  - the commit, its short form and the describe string appear on no build edge (no include path, link line or
    generated header can carry them past this check);
  - the one generated unit is compiled once, from generated/nevr_build_stamp.cpp;
  - no source or header names a retired macro outside its comments.
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD = REPO / "build" / "mingw-release"
NINJA = BUILD / "build.ninja"
STAMP_UNIT = BUILD / "generated" / "nevr_build_stamp.cpp"

RETIRED_MACROS = re.compile(
    r"\bNEVR_(PROJECT_VERSION(_MAJOR|_MINOR|_PATCH|_TWEAK)?|GIT_COMMIT_HASH|GIT_COMMIT_FULL|GIT_DESCRIBE"
    r"|QUEST_PROJECT_VERSION|QUEST_GIT_COMMIT|QUEST_GIT_DESCRIBE)\b")
COMPILE_DEFINED = re.compile(r"-D(NEVR_PROJECT_VERSION|NEVR_GIT_|NEVR_QUEST_PROJECT_VERSION|NEVR_QUEST_GIT_)")
COMMENTS = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
EDGE = re.compile(r"^build (?P<outputs>[^:]+?): (?P<rule>\S+) (?P<inputs>[^|\n]*)", re.M)
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")
STAMP_VALUE = re.compile(r'const char (k\w+)\[\] = "([^"\\]*)";')


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
    """The retired identity macros a source file names outside its comments."""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return set()
    return set(m.group(0) for m in RETIRED_MACROS.finditer(COMMENTS.sub("", text)))


def compile_edges():
    for outputs, rule, first, block in edges():
        if "COMPILER" in rule and "RC_COMPILER" not in rule and not rule.startswith("RC_"):
            yield outputs, first, block


def stamp_values() -> dict:
    return dict(STAMP_VALUE.findall(STAMP_UNIT.read_text(errors="replace")))


@unittest.skipUnless(NINJA.exists() and STAMP_UNIT.exists(), "no configured tree with a stamp unit in build/mingw-release")
class IdentityScope(unittest.TestCase):
    def test_no_compile_command_defines_a_retired_identity_macro(self):
        carrying = [outputs for outputs, _, block in compile_edges() if COMPILE_DEFINED.search(block)]
        self.assertGreater(len(list(compile_edges())), 100, "the parser found too few compile edges to mean anything")
        self.assertEqual(carrying[:5], [], f"{len(carrying)} compile edges still define the identity")

    def test_the_stamp_unit_is_compiled_once_from_the_generated_file(self):
        units = [outputs for outputs, first, _ in compile_edges() if first.endswith("generated/nevr_build_stamp.cpp")]
        self.assertEqual(len(units), 1, f"expected one compile edge for the stamp unit, found {units}")

    def test_the_commit_and_the_describe_string_are_on_no_build_edge(self):
        values = stamp_values()
        needles = [values[name] for name in ("kCommitFull", "kCommitShort", "kDescribe") if values.get(name)]
        self.assertTrue(needles, "the stamp unit carries no commit: nothing to check")
        stray = [outputs for outputs, _, _, block in ((o, r, f, b) for o, r, f, b in edges())
                 if any(needle in block for needle in needles)]
        self.assertEqual(stray[:5], [], f"{len(stray)} build edges carry the commit or its describe string")

    def test_the_stamp_unit_is_the_one_writer_of_the_commit(self):
        values = stamp_values()
        head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        self.assertRegex(values["kCommitFull"], r"^[0-9a-f]{40}$")
        self.assertTrue(values["kCommitFull"].startswith(values["kCommitShort"]))
        if values["kCommitFull"] != head:
            self.skipTest(f"the stamp unit is of {values['kCommitFull'][:12]}, HEAD is {head[:12]}: stale, not asserted")
        self.assertEqual(values["kCommitFull"], head)


class SourcesDoNotNameRetiredMacros(unittest.TestCase):
    """The version and the commit are read through src/core/build_stamp.h, never through a macro."""

    def test_no_source_or_header_names_a_retired_identity_macro_outside_a_comment(self):
        offenders = {}
        for suffix in ("*.h", "*.hpp", "*.cpp", "*.cc"):
            for path in sorted((REPO / "src").rglob(suffix)) + sorted((REPO / "sdk").rglob(suffix)):
                names = named_in(path)
                if names:
                    offenders[path.relative_to(REPO).as_posix()] = sorted(names)
        self.assertEqual(offenders, {}, "these read the identity through a retired macro: use src/core/build_stamp.h")


if __name__ == "__main__":
    unittest.main()
