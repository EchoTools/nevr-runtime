"""Each built DLL (the host BugSplat64.dll and the runtime nevr-runtime.dll) carries exactly one identity literal, equal
to the stamped version and commit, and the two literals are the same.

    NEVR-BUILD <version> <commit40>      (NUL-bounded; src/core/build_identity.cpp)

`just verify` builds first and runs the Python tests afterwards, so the DLL here is the one just built.
Run on its own with no build tree, the test skips: it reads a binary, it does not make one."""
from __future__ import annotations

import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD = REPO / "build" / "mingw-release"
HOST = BUILD / "bin" / "BugSplat64.dll"
RUNTIME = BUILD / "bin" / "nevr-runtime.dll"
DLLS = (HOST, RUNTIME)
NINJA = BUILD / "build.ninja"
IDENTITY = re.compile(rb"(?<=\x00)NEVR-BUILD ([0-9A-Za-z.+-]+) ([0-9a-f]{40})(?=\x00)")


STAMP_UNIT = BUILD / "generated" / "nevr_build_stamp.cpp"
STAMP_VALUE = re.compile(r'const char (k\w+)\[\] = "([^"\\]*)";')


def definition(name: str) -> str:
    """The value the build's one stamp unit carries (src/core/build_stamp.h), written at build time by
    cmake/nevr_build_stamp.cmake. No compile command carries it (#1935)."""
    if not STAMP_UNIT.exists():
        raise AssertionError(f"{STAMP_UNIT} is missing: the build has not written its stamp unit")
    found = dict(STAMP_VALUE.findall(STAMP_UNIT.read_text(errors="replace")))
    if name not in found:
        raise AssertionError(f"{STAMP_UNIT.name} has no {name}")
    return found[name]


@unittest.skipUnless(HOST.exists() and RUNTIME.exists() and NINJA.exists(),
                     "no built BugSplat64.dll and nevr-runtime.dll in build/mingw-release")
class BuiltIdentityLiteralTest(unittest.TestCase):
    def test_each_dll_has_exactly_one_identity_literal_matching_its_definitions(self):
        for dll in DLLS:
            with self.subTest(dll=dll.name):
                found = IDENTITY.findall(dll.read_bytes())
                self.assertEqual(len(found), 1, f"expected exactly one NEVR-BUILD literal, found {len(found)}")
                version, commit = found[0][0].decode(), found[0][1].decode()
                self.assertEqual(version, definition("kProjectVersion"))
                self.assertEqual(commit, definition("kCommitFull"))
                # A branch build is a development version; a release stamp only ever comes from a CI tag build.
                self.assertRegex(version, r"^\d+\.\d+\.\d+(-dev\.\d+\+[0-9a-f]{7,40})?$")

    def test_the_host_and_the_runtime_are_one_build(self):
        self.assertEqual(IDENTITY.findall(HOST.read_bytes()), IDENTITY.findall(RUNTIME.read_bytes()))

    def test_the_commit_is_the_checked_out_commit_when_the_build_is_current(self):
        head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        built = definition("kCommitFull")
        if built != head:
            self.skipTest(f"the build tree is of {built[:12]}, HEAD is {head[:12]}: stale, not asserted")
        for dll in DLLS:
            self.assertEqual(IDENTITY.findall(dll.read_bytes())[0][1].decode(), head, dll.name)

    def test_the_literal_is_not_in_the_dll_twice_as_a_substring_of_something_else(self):
        # The signer's regex requires NUL on both sides; a plain substring search must also find one place.
        for dll in DLLS:
            self.assertEqual(dll.read_bytes().count(b"NEVR-BUILD "), 1, dll.name)


PLUGIN_TRUST = re.compile(rb"(?<=\x00)NEVR-PLUGIN-TRUST ([a-z-]+)(?=\x00)")


@unittest.skipUnless(HOST.exists() and RUNTIME.exists() and NINJA.exists(),
                     "no built BugSplat64.dll and nevr-runtime.dll in build/mingw-release")
class BuiltPluginTrustLiteralTest(unittest.TestCase):
    """ADR 0010 (#741): the runtime, which loads plugins, says which kind of build it is, in a literal of its own.
    The host loads no plugin and carries none."""

    def test_the_runtime_has_exactly_one_plugin_trust_literal_and_it_says_release(self):
        found = PLUGIN_TRUST.findall(RUNTIME.read_bytes())
        self.assertEqual(found, [b"release"], "the mingw-release preset must not be the plugin developer build")
        self.assertEqual(RUNTIME.read_bytes().count(b"NEVR-PLUGIN-TRUST "), 1)

    def test_the_host_carries_no_plugin_trust_literal(self):
        self.assertEqual(HOST.read_bytes().count(b"NEVR-PLUGIN-TRUST "), 0)

    def test_the_option_is_off_in_the_tree_that_built_it(self):
        self.assertNotIn("NEVR_ALLOW_UNSIGNED_PLUGINS=1", NINJA.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
