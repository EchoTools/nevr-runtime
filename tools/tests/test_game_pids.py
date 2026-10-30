"""game_pids (tools/lib/game_install.sh) and its Python twin (tools/scenario/run_scenario.py): a process is the game when
its argv[0] is echovr.exe, and not when its command line only names the file (#1490). The processes are real: a decoy
that names echovr.exe as an argument, and a stand-in whose argv[0] is ./echovr.exe, the way the game's command line
starts."""
import os
import pathlib
import signal
import subprocess
import sys
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
LIB = REPO / "tools/lib/game_install.sh"
sys.path.insert(0, str(REPO / "tools/scenario"))
import run_scenario  # noqa: E402


def start(command: list[str], marker: bytes) -> subprocess.Popen:
    """Start command in its own session and wait until its command line starts with marker (exec -a changes it)."""
    proc = subprocess.Popen(command, start_new_session=True)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        with open(f"/proc/{proc.pid}/cmdline", "rb") as f:
            if f.read().startswith(marker):
                return proc
        time.sleep(0.02)
    raise AssertionError(f"pid {proc.pid} never showed {marker!r}")


class GamePidsTest(unittest.TestCase):
    def setUp(self):
        self.decoy = start(["bash", "-c", "sleep 600; true", "/x/bin/win10/echovr.exe"], b"bash\0")
        self.standin = start(["bash", "-c", "exec -a ./echovr.exe sleep 600"], b"./echovr.exe\0")
        self.addCleanup(self.stop)

    def stop(self):
        for proc in (self.decoy, self.standin):
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()

    def shell_game_pids(self) -> list[str]:
        result = subprocess.run(["bash", "-c", 'source "$1"; game_pids', "bash", str(LIB)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.split()

    def test_shell_finds_the_stand_in_and_not_the_decoy(self):
        pids = self.shell_game_pids()
        self.assertIn(str(self.standin.pid), pids)
        self.assertNotIn(str(self.decoy.pid), pids)

    def test_python_finds_the_stand_in_and_not_the_decoy(self):
        self.assertTrue(run_scenario.is_game_process(str(self.standin.pid)))
        self.assertFalse(run_scenario.is_game_process(str(self.decoy.pid)))
        self.assertIn(str(self.standin.pid), run_scenario.game_pids())
        self.assertNotIn(str(self.decoy.pid), run_scenario.game_pids())

    def test_a_pid_that_is_gone_is_not_the_game(self):
        self.assertFalse(run_scenario.is_game_process("999999999"))


if __name__ == "__main__":
    unittest.main()
