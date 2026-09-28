"""VM scenario routing and environment-failure contracts."""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "winvm"))
import systest  # noqa: E402


class ScenarioDispatchTest(unittest.TestCase):
    def test_default_all_dispatches_gai_then_boot_then_login(self):
        calls = []

        def gai_runner(*args):
            calls.append("gai")
            return []

        def boot_runner(*args, **kwargs):
            calls.append("login" if kwargs.get("login_config") else "boot")
            return []

        self.assertEqual(systest.scenario_plan("all"), ("gai", "boot", "login"))
        systest.dispatch_scenarios("all", object(), pathlib.Path("out"), pathlib.Path("runtime.dll"),
                                  object(), "local-login-config", gai_runner, boot_runner)
        self.assertEqual(calls, ["gai", "boot", "login"])

    def test_explicit_boot_and_gai_do_not_load_or_dispatch_login(self):
        for scenario in ("boot", "gai"):
            with self.subTest(scenario=scenario):
                self.assertIsNone(systest.selected_login_config(scenario))
                calls = []

                def gai_runner(*args):
                    calls.append("gai")
                    return []

                def boot_runner(*args, **kwargs):
                    calls.append("boot")
                    return []

                systest.dispatch_scenarios(
                    scenario, object(), pathlib.Path("out"), pathlib.Path("runtime.dll"),
                    object(), None, gai_runner, boot_runner)
                self.assertEqual(calls, [scenario])

    def test_missing_login_configuration_is_environment_exit_two(self):
        scratch = pathlib.Path("/var/tmp/work-nevr-runtime")
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temp:
            root = pathlib.Path(temp)
            missing_nakama = root / "nakama"
            missing_nakama.mkdir()
            dll = root / "BugSplat64.dll"
            dll.write_bytes(b"fixture")
            output = io.StringIO()
            with mock.patch.dict("os.environ", {"WINVM_USER": "vm-user", "WINVM_PASS": "vm-pass"}), \
                    mock.patch.object(systest, "NAKAMA_DIR", missing_nakama), \
                    contextlib.redirect_stderr(output):
                result = systest.main([
                    "--scenario", "login", "--dll", str(dll), "--out", str(root / "out")])
            self.assertEqual(result, 2)
            self.assertIn("ENV:", output.getvalue())
            self.assertIn("no local nakama state", output.getvalue())


if __name__ == "__main__":
    unittest.main()
