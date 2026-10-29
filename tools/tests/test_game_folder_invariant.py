"""The snapshot library of the game-folder invariant (tools/game_folder_invariant.py): each way a step can leave something
behind in the install is seen, and the one allowed difference, the content of the host, is not."""
from __future__ import annotations

import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import game_folder_invariant as inv  # noqa: E402


def make_install(root: pathlib.Path) -> None:
    (root / "bin/win10").mkdir(parents=True)
    (root / "bin/win10/echovr.exe").write_bytes(b"exe")
    (root / "bin/win10/BugSplat64.dll").write_bytes(b"the game's own")
    (root / "_local").mkdir()
    (root / "_local/old.log").write_text("log")


class SnapshotTest(unittest.TestCase):
    def fresh(self):
        tmp = tempfile.TemporaryDirectory(prefix="invariant-")
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        make_install(root)
        return root, inv.snapshot(root)

    def test_an_untouched_tree_has_no_violations(self):
        root, before = self.fresh()
        self.assertEqual(inv.violations(before, inv.snapshot(root)), [])

    def test_the_host_changing_content_is_the_one_allowed_difference(self):
        root, before = self.fresh()
        (root / "bin/win10/BugSplat64.dll").write_bytes(b"ours, longer")
        self.assertEqual(inv.violations(before, inv.snapshot(root)), [])

    def test_every_other_way_of_leaving_something_behind_is_named(self):
        def new_folder_with_file(r):
            (r / "_temp").mkdir()
            (r / "_temp/a").write_text("a")

        cases = {
            "an added file": (lambda r: (r / "bin/win10/nevr-install.txt").write_text("x"), "bin/win10/nevr-install.txt: added"),
            "an added backup": (lambda r: (r / "bin/win10/BugSplat64.dll.original-1").write_bytes(b"y"), "BugSplat64.dll.original-1: added"),
            "an added folder": (lambda r: (r / "plugins").mkdir(), "plugins: added"),
            "a file in a new folder": (new_folder_with_file, "_temp/a: added"),
            "a removed file": (lambda r: (r / "bin/win10/echovr.exe").unlink(), "bin/win10/echovr.exe: removed"),
            "a changed file": (lambda r: (r / "_local/old.log").write_text("log2"), "_local/old.log: changed"),
            "a changed mode": (lambda r: os.chmod(r / "_local/old.log", 0o600), "_local/old.log: changed"),
            "a link": (lambda r: (r / "bin/win10/link").symlink_to("echovr.exe"), "bin/win10/link: added"),
            "the host removed": (lambda r: (r / "bin/win10/BugSplat64.dll").unlink(), "BugSplat64.dll: removed"),
            "the host's mode": (lambda r: os.chmod(r / "bin/win10/BugSplat64.dll", 0o600), "the mode changed"),
        }
        for name, (act, expected) in cases.items():
            with self.subTest(name):
                root, before = self.fresh()
                act(root)
                found = inv.violations(before, inv.snapshot(root))
                self.assertEqual(len(found), 1 + (name == "a file in a new folder"), found)
                self.assertTrue(any(expected in v for v in found), found)

    def test_the_host_replaced_by_a_folder_is_a_violation(self):
        root, before = self.fresh()
        (root / "bin/win10/BugSplat64.dll").unlink()
        (root / "bin/win10/BugSplat64.dll").mkdir()
        self.assertTrue(any("BugSplat64.dll" in v for v in inv.violations(before, inv.snapshot(root))))


class StepsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="invariant-steps-")
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "bin/win10").mkdir(parents=True)
        (self.root / "bin/win10/BugSplat64.dll").write_bytes(b"orig")

    def test_a_step_that_leaves_a_file_is_named_and_a_clean_one_after_it_is_still_judged_against_the_start(self):
        def leaves(root): (root / "nevr-install.txt").write_text("x")
        def host(root): (root / "bin/win10/BugSplat64.dll").write_bytes(b"new")
        results = inv.run_steps(self.root, [("host", host), ("leaves", leaves), ("noop", lambda root: None)])
        self.assertEqual([r.ok for r in results], [True, False, False])
        self.assertIn("nevr-install.txt: added", results[1].violations[0])
        self.assertIn("nevr-install.txt: added", results[2].violations[0])

    def test_a_step_that_is_none_is_not_run_and_never_ok(self):
        results = inv.run_steps(self.root, [("client run", None)])
        self.assertEqual((results[0].ran, results[0].ok), (False, False))

    def test_a_step_that_raises_ends_the_run(self):
        def boom(root): raise RuntimeError("no")
        results = inv.run_steps(self.root, [("a", boom), ("b", lambda root: None)])
        self.assertEqual([r.name for r in results], ["a"])
        self.assertIn("RuntimeError: no", results[0].error)
        self.assertFalse(results[0].ok)

    def test_a_command_step_gets_the_root_as_its_last_argument_and_fails_on_a_nonzero_exit(self):
        out = self.root / "arg.txt"
        script = pathlib.Path(self.tmp.name + "-cmd.sh")
        script.write_text(f'#!/bin/bash\nprintf "%s|%s" "$1" "$2" > "{out}"\nexit ${{FAKE_EXIT:-0}}\n')
        script.chmod(0o755)
        self.addCleanup(script.unlink)
        with mock.patch.dict(os.environ, {inv.CLIENT_RUN_ENV: f"{script} first"}):
            step = inv.command_step(inv.CLIENT_RUN_ENV)
            step(self.root)
            self.assertEqual(out.read_text(), f"first|{self.root}")
        out.unlink()
        with mock.patch.dict(os.environ, {inv.CLIENT_RUN_ENV: f"{script} first", "FAKE_EXIT": "3"}):
            with self.assertRaises(RuntimeError) as caught:
                inv.command_step(inv.CLIENT_RUN_ENV)(self.root)
            self.assertIn("exited 3", str(caught.exception))

    def test_an_unset_or_empty_variable_gives_no_step(self):
        with mock.patch.dict(os.environ, {inv.SERVER_RUN_ENV: "  "}):
            self.assertIsNone(inv.command_step(inv.SERVER_RUN_ENV))
        with mock.patch.dict(os.environ):
            os.environ.pop(inv.SERVER_RUN_ENV, None)
            self.assertIsNone(inv.command_step(inv.SERVER_RUN_ENV))


if __name__ == "__main__":
    unittest.main()
