"""The built BugSplat64.dll carries exactly one identity literal, equal to the stamped version and commit.

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
DLL = BUILD / "bin" / "BugSplat64.dll"
NINJA = BUILD / "build.ninja"
IDENTITY = re.compile(rb"(?<=\x00)NEVR-BUILD ([0-9A-Za-z.+-]+) ([0-9a-f]{40})(?=\x00)")


def definition(name: str) -> str:
    """The value the build itself passed to the compiler: -DNAME=\"value\" in build.ninja."""
    match = re.search(rf'-D{name}=\\"([^"\\]+)\\"', NINJA.read_text(errors="replace"))
    if not match:
        raise AssertionError(f"build.ninja has no -D{name}")
    return match.group(1)


@unittest.skipUnless(DLL.exists() and NINJA.exists(), "no built BugSplat64.dll in build/mingw-release")
class BuiltIdentityLiteralTest(unittest.TestCase):
    def test_the_dll_has_exactly_one_identity_literal_matching_its_definitions(self):
        blob = DLL.read_bytes()
        found = IDENTITY.findall(blob)
        self.assertEqual(len(found), 1, f"expected exactly one NEVR-BUILD literal, found {len(found)}")
        version, commit = found[0][0].decode(), found[0][1].decode()
        self.assertEqual(version, definition("NEVR_PROJECT_VERSION"))
        self.assertEqual(commit, definition("NEVR_GIT_COMMIT_FULL"))
        # A branch build is a development version; a release stamp only ever comes from a CI tag build.
        self.assertRegex(version, r"^\d+\.\d+\.\d+(-dev\.\d+\+[0-9a-f]{7,40})?$")

    def test_the_commit_is_the_checked_out_commit_when_the_build_is_current(self):
        head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        built = definition("NEVR_GIT_COMMIT_FULL")
        if built != head:
            self.skipTest(f"the build tree is of {built[:12]}, HEAD is {head[:12]}: stale, not asserted")
        self.assertEqual(IDENTITY.findall(DLL.read_bytes())[0][1].decode(), head)

    def test_the_literal_is_not_in_the_dll_twice_as_a_substring_of_something_else(self):
        # The signer's regex requires NUL on both sides; a plain substring search must also find one place.
        self.assertEqual(DLL.read_bytes().count(b"NEVR-BUILD "), 1)


if __name__ == "__main__":
    unittest.main()
