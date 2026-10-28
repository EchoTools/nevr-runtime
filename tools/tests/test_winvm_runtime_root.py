"""The Windows-VM rig puts nevr-runtime.dll in the nevr root, never beside the host (ADR 0012)."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "winvm"))
import systest  # noqa: E402


class RecordingGuest:
    def __init__(self):
        self.puts = []
        self.scripts = []

    def ps(self, script):
        self.scripts.append(script)
        return ""

    def put(self, local, remote_dir, name):
        self.puts.append((pathlib.Path(local).name, remote_dir, name))


class RigRuntimeRootTest(unittest.TestCase):
    def deploy(self):
        guest = RecordingGuest()
        with tempfile.TemporaryDirectory() as t:
            dll = pathlib.Path(t) / "BugSplat64.dll"
            dll.write_bytes(b"host")
            (pathlib.Path(t) / "nevr-runtime.dll").write_bytes(b"runtime")
            systest.deploy(guest, dll)
        return guest

    def test_the_runtime_goes_to_the_nevr_root_and_the_host_to_the_game_folder(self):
        puts = {name: remote for _, remote, name in self.deploy().puts}
        self.assertEqual(puts["nevr-runtime.dll"], "nevr-systest/localappdata/nevr/echovr")
        self.assertEqual(puts["BugSplat64.dll"], "nevr-systest/echovr/bin/win10")

    def test_the_game_is_started_with_the_rigs_own_local_app_data(self):
        guest = RecordingGuest()
        systest.launch(guest, "-headless")
        script = guest.scripts[0]
        self.assertIn(r"'set LOCALAPPDATA=C:\nevr-systest\localappdata'", script)
        self.assertLess(script.index("set LOCALAPPDATA="), script.index("echovr.exe -headless"))

    def test_the_rig_makes_the_root_and_clears_a_stale_runtime_beside_the_host(self):
        guest = RecordingGuest()
        with tempfile.TemporaryDirectory() as t:
            systest.setup_rig(guest, pathlib.Path(t))
        script = guest.scripts[0]
        self.assertIn(r'"$root\localappdata\nevr\echovr"', script)
        self.assertIn(r'$stale = "$root\echovr\bin\win10\nevr-runtime.dll"', script)
        self.assertIn("Remove-Item $stale -Force", script)
        self.assertIn('if (Test-Path $stale) { throw "could not remove stale $stale from the rig" }', script)
        self.assertIn("/XF dbgcore.dll BugSplat64.dll BugSplat64.dll.stock nevr-runtime.dll", script)


if __name__ == "__main__":
    unittest.main()
