"""Each built DLL (the host BugSplat64.dll and the runtime nevr-runtime.dll) carries exactly one identity literal, equal
to the stamped version and commit, and the two literals are the same.

    NEVR-BUILD <version> <commit40>      (NUL-bounded; src/core/build_identity.cpp)

`just verify` builds first and runs the Python tests afterwards, so the DLL here is the one just built.
Run on its own with no build tree, the test skips: it reads a binary, it does not make one."""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD = REPO / "build" / "mingw-release"
HOST = BUILD / "bin" / "BugSplat64.dll"
RUNTIME = BUILD / "bin" / "nevr-runtime.dll"
DLLS = (HOST, RUNTIME)
NINJA = BUILD / "build.ninja"
IDENTITY = re.compile(rb"(?<=\x00)NEVR-BUILD ([0-9A-Za-z.+-]+) ([0-9a-f]{40})(?=\x00)")


def definition(name: str) -> str:
    """The value the build itself passed to the compiler: -DNAME=\"value\" in build.ninja."""
    match = re.search(rf'-D{name}=\\"([^"\\]+)\\"', NINJA.read_text(errors="replace"))
    if not match:
        raise AssertionError(f"build.ninja has no -D{name}")
    return match.group(1)


def ninja_stale(target):
    """Ask ninja, as a dry run, whether `target` (relative to BUILD) needs rebuilding.

    Returns None when ninja reports `ninja: no work to do.`; otherwise the first command ninja would
    run, a `[n/m] ...` line. A failure of ninja (an unknown target, a broken build.ninja) raises
    AssertionError with ninja's own text. Without ninja on PATH the test skips, naming ninja."""
    if shutil.which("ninja") is None:
        raise unittest.SkipTest("ninja is not on PATH, so whether the DLLs are stale cannot be asked")
    proc = subprocess.run(["ninja", "-n", "-C", str(BUILD), target], capture_output=True, text=True)
    text = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise AssertionError(f"ninja -n {target} failed (rc={proc.returncode}): {text.strip()}")
    if "ninja: no work to do." in text:
        return None
    steps = [line for line in text.splitlines() if re.match(r"\[\d+/\d+\] ", line)]
    if not steps:
        raise AssertionError(f"ninja -n {target} reported neither work nor 'no work to do.': {text.strip()}")
    return steps[0]


def skip_if_stale(case, dlls=None):
    """A DLL that ninja would rebuild is stale: its literal is not asserted, and the skip names it."""
    for dll in (DLLS if dlls is None else dlls):
        target = dll.relative_to(BUILD).as_posix()
        step = ninja_stale(target)
        if step is not None:
            case.skipTest(f"{target} is stale: ninja -n would run `{step}`; its identity literal is not asserted")


class BuiltIdentityLiteralTest(unittest.TestCase):
    def setUp(self):
        missing = [p.relative_to(REPO).as_posix() for p in (HOST, RUNTIME, NINJA) if not p.exists()]
        if missing:
            self.skipTest("no built tree to read; missing: " + ", ".join(missing))

    def test_each_dll_has_exactly_one_identity_literal_matching_its_definitions(self):
        skip_if_stale(self)
        for dll in DLLS:
            with self.subTest(dll=dll.name):
                found = IDENTITY.findall(dll.read_bytes())
                self.assertEqual(len(found), 1, f"expected exactly one NEVR-BUILD literal, found {len(found)}")
                version, commit = found[0][0].decode(), found[0][1].decode()
                self.assertEqual(version, definition("NEVR_PROJECT_VERSION"))
                self.assertEqual(commit, definition("NEVR_GIT_COMMIT_FULL"))
                # A branch build is a development version; a release stamp only ever comes from a CI tag build.
                self.assertRegex(version, r"^\d+\.\d+\.\d+(-dev\.\d+\+[0-9a-f]{7,40})?$")

    def test_the_host_and_the_runtime_are_one_build(self):
        skip_if_stale(self)
        self.assertEqual(IDENTITY.findall(HOST.read_bytes()), IDENTITY.findall(RUNTIME.read_bytes()))

    def test_the_commit_is_the_checked_out_commit_when_the_build_is_current(self):
        head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        built = definition("NEVR_GIT_COMMIT_FULL")
        if built != head:
            self.skipTest(f"the build tree is of {built[:12]}, HEAD is {head[:12]}: stale, not asserted")
        skip_if_stale(self)
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


class StaleBuildTest(unittest.TestCase):
    """The staleness rule on a throwaway build tree: a real ninja builds two DLLs from two source files, the
    module's globals point at that tree, and the two methods above run on it. Skips, naming ninja, without ninja."""

    def setUp(self):
        if shutil.which("ninja") is None:
            self.skipTest("ninja is not on PATH, so the stale-tree fixture cannot be built")
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        git = ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run(git + ["init", "-q"], check=True)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "fixture"], check=True)
        self.head = subprocess.run(git + ["rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        build = root / "build" / "mingw-release"
        (build / "bin").mkdir(parents=True)
        (build / "src").mkdir()
        define = f'-DNEVR_PROJECT_VERSION=\\"6.0.0-dev.1+{self.head[:8]}\\" -DNEVR_GIT_COMMIT_FULL=\\"{self.head}\\"'
        (build / "build.ninja").write_text(
            "rule cp\n"
            f"  command = cp $in $out # {define}\n"
            "build bin/BugSplat64.dll: cp src/host.src\n"
            "build bin/nevr-runtime.dll: cp src/runtime.src\n")
        for name in ("host.src", "runtime.src"):
            (build / "src" / name).write_bytes(self.literal(self.head))
        subprocess.run(["ninja", "-C", str(build)], check=True, capture_output=True)
        self.build = build
        host, runtime = build / "bin" / "BugSplat64.dll", build / "bin" / "nevr-runtime.dll"
        patcher = mock.patch.multiple(sys.modules[__name__], REPO=root, BUILD=build, HOST=host, RUNTIME=runtime,
                                      DLLS=(host, runtime), NINJA=build / "build.ninja")
        patcher.start()
        self.addCleanup(patcher.stop)

    @staticmethod
    def literal(commit, version=None):
        version = version or f"6.0.0-dev.1+{commit[:8]}"
        return b"MZ" + b"\x00" * 32 + b"\x00NEVR-BUILD " + version.encode() + b" " + commit.encode() + b"\x00" + b"\x00" * 16

    def age_source(self, name):
        """Make a source newer than its DLL, so ninja reports the DLL as needing a rebuild."""
        future = time.time() + 60
        os.utime(self.build / "src" / name, (future, future))

    def plant(self, dll):
        """Rewrite a DLL's commit literal to another commit, leaving its version as the build's own."""
        (self.build / "bin" / dll).write_bytes(self.literal("a" * 40, version=f"6.0.0-dev.1+{self.head[:8]}"))

    def outcome(self, method):
        # A fresh, un-run case: its subTest raises, where a running case would record the failure and return.
        case = BuiltIdentityLiteralTest(method)
        try:
            getattr(BuiltIdentityLiteralTest, method)(case)
        except unittest.SkipTest as skip:
            return f"skip: {skip}"
        except AssertionError as err:
            return f"fail: {err}"
        return "pass"

    def both(self):
        return {m: self.outcome(m) for m in (
            "test_each_dll_has_exactly_one_identity_literal_matching_its_definitions",
            "test_the_commit_is_the_checked_out_commit_when_the_build_is_current",
            "test_the_host_and_the_runtime_are_one_build")}

    def test_a_current_tree_is_asserted_and_passes(self):
        for method, result in self.both().items():
            self.assertEqual(result, "pass", method)

    def test_a_tree_ninja_would_rebuild_skips_both_and_names_the_dll(self):
        self.age_source("host.src")
        for method, result in self.both().items():
            self.assertTrue(result.startswith("skip: bin/BugSplat64.dll is stale"), (method, result))
            self.assertIn("would run", result)

    def test_a_planted_literal_in_a_current_tree_fails_both(self):
        self.plant("nevr-runtime.dll")
        for method, result in self.both().items():
            self.assertTrue(result.startswith("fail:"), (method, result))

    def test_a_planted_literal_in_a_stale_tree_skips_and_does_not_fail(self):
        self.plant("nevr-runtime.dll")
        self.age_source("runtime.src")
        for method, result in self.both().items():
            self.assertTrue(result.startswith("skip: bin/nevr-runtime.dll is stale"), (method, result))

    def test_a_ninja_failure_is_an_error_not_a_skip(self):
        with self.assertRaises(AssertionError) as caught:
            ninja_stale("bin/nope.dll")
        self.assertIn("failed (rc=1)", str(caught.exception))
        self.assertIn("unknown target", str(caught.exception))

    def test_without_ninja_both_skip_and_name_ninja(self):
        with mock.patch("shutil.which", return_value=None):
            for method, result in self.both().items():
                self.assertTrue(result.startswith("skip:") and "ninja" in result, (method, result))

    def test_a_missing_build_file_skips_and_names_it(self):
        with mock.patch.multiple(sys.modules[__name__], NINJA=self.build / "absent.ninja"):
            with self.assertRaises(unittest.SkipTest) as caught:
                BuiltIdentityLiteralTest.setUp(self)
        self.assertIn("absent.ninja", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
