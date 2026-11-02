"""The work directory contract (AGENTS.md, "Working practices for every agent"): every tool's default
scratch location is the work root, $NEVR_WORK_ROOT else src/nevr-runtime-work under the home
directory, from one definition per language, and a directory a tool makes there carries a README.

Nothing here starts the game: the shell tools run against the fake install and fake `wine` of
test_launch_client.py, the Python tools are imported in a child process that is given the environment.
"""
from __future__ import annotations

import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

import test_launch_client as fixtures  # noqa: E402

# The directory name this contract retired, split so that this file does not contain it.
RETIRED = "work-" + "nevr-runtime"
DEFAULT_RELATIVE = pathlib.Path("src") / "nevr-runtime-work"


def child_env(**extra):
    """The environment of a child: no inherited work-root or per-tool override, then `extra`."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("NEVR_WORK_ROOT", "NEVR_RUN_SCRATCH_ROOT", "NEVR_TEST_LOG_DIR", "NEVR_LAUNCH_LOCK",
                        "NEVR_SMOKE_STATUS_DIR")}
    env.update(extra)
    return env


class NoRetiredPathTest(unittest.TestCase):
    def test_no_tracked_file_names_the_retired_directory(self):
        listing = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, capture_output=True, check=True).stdout
        hits = []
        for name in listing.split(b"\0"):
            if not name:
                continue
            path = REPO / name.decode()
            if not path.is_file() or path.is_symlink():
                continue
            for number, line in enumerate(path.read_bytes().splitlines(), 1):
                if RETIRED.encode() in line:
                    hits.append(f"{name.decode()}:{number}")
        self.assertEqual(hits, [])


class PythonHelperTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="work-root-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        import work_root
        self.work_root = work_root
        saved = {k: os.environ.get(k) for k in ("NEVR_WORK_ROOT", "HOME")}
        self.addCleanup(lambda: [os.environ.pop(k, None) if v is None else os.environ.__setitem__(k, v)
                                 for k, v in saved.items()])

    def test_the_variable_names_the_root(self):
        os.environ["NEVR_WORK_ROOT"] = str(self.tmp / "named")
        self.assertEqual(self.work_root.work_root(), self.tmp / "named")

    def test_without_the_variable_the_root_is_under_the_home_directory(self):
        os.environ.pop("NEVR_WORK_ROOT", None)
        os.environ["HOME"] = str(self.tmp / "home")
        self.assertEqual(self.work_root.work_root(), self.tmp / "home" / DEFAULT_RELATIVE)

    def test_an_empty_variable_is_the_default(self):
        os.environ["NEVR_WORK_ROOT"] = ""
        os.environ["HOME"] = str(self.tmp / "home")
        self.assertEqual(self.work_root.work_root(), self.tmp / "home" / DEFAULT_RELATIVE)

    def test_a_directory_under_the_root_gets_readmes_on_its_top_level_directory_and_the_root(self):
        os.environ["NEVR_WORK_ROOT"] = str(self.tmp / "root")
        made = self.work_root.ensure_dir(self.tmp / "root/runs/one/deeper", "Run output of the tool.", "when the PR merges.")
        self.assertTrue(made.is_dir())
        readme = (self.tmp / "root/runs/README.md").read_text()
        self.assertIn("Run output of the tool.", readme)
        self.assertIn("when the PR merges.", readme)
        self.assertIn("Owner: ", readme)
        self.assertTrue((self.tmp / "root/README.md").is_file())
        self.assertFalse((self.tmp / "root/runs/one/README.md").exists())

    def test_a_readme_that_exists_is_kept(self):
        os.environ["NEVR_WORK_ROOT"] = str(self.tmp / "root")
        (self.tmp / "root/runs").mkdir(parents=True)
        (self.tmp / "root/runs/README.md").write_text("mine\n")
        self.work_root.ensure_dir(self.tmp / "root/runs/one", "Other.", "never.")
        self.assertEqual((self.tmp / "root/runs/README.md").read_text(), "mine\n")

    def test_the_test_scratch_directory_is_under_the_root_with_a_readme(self):
        os.environ["NEVR_WORK_ROOT"] = str(self.tmp / "root")
        scratch = self.work_root.test_scratch()
        self.assertEqual(scratch, self.tmp / "root" / "test-scratch")
        self.assertIn("removes its own", (scratch / "README.md").read_text())
        with self.work_root.make_test_dir("probe-") as made:
            self.assertEqual(pathlib.Path(made).parent, scratch)

    def test_a_directory_outside_the_root_is_made_without_a_readme(self):
        os.environ["NEVR_WORK_ROOT"] = str(self.tmp / "root")
        self.work_root.ensure_dir(self.tmp / "elsewhere/logs", "Logs.", "never.")
        self.assertTrue((self.tmp / "elsewhere/logs").is_dir())
        self.assertFalse((self.tmp / "elsewhere/logs/README.md").exists())
        self.assertFalse((self.tmp / "elsewhere/README.md").exists())


class ShellHelperTest(unittest.TestCase):
    LIB = TOOLS / "lib/work_root.sh"

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="work-root-sh-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)

    def sh(self, script, **env):
        return subprocess.run(["bash", "-c", f"set -euo pipefail; source {self.LIB}; {script}"],
                              env=child_env(**env), capture_output=True, text=True, timeout=30)

    def test_the_variable_names_the_root(self):
        result = self.sh('printf %s "$NEVR_WORK_ROOT"', NEVR_WORK_ROOT=str(self.tmp / "named"), HOME=str(self.tmp))
        self.assertEqual(result.stdout, str(self.tmp / "named"), result.stderr)

    def test_without_the_variable_the_root_is_under_the_home_directory(self):
        result = self.sh('printf %s "$NEVR_WORK_ROOT"', HOME=str(self.tmp))
        self.assertEqual(result.stdout, str(self.tmp / DEFAULT_RELATIVE), result.stderr)

    def test_work_dir_writes_the_readmes_the_python_helper_writes(self):
        root = self.tmp / "root"
        result = self.sh('work_dir "$NEVR_WORK_ROOT/runs/one" "Run output." "when the PR merges."',
                         NEVR_WORK_ROOT=str(root))
        self.assertEqual(result.returncode, 0, result.stderr)
        readme = (root / "runs/README.md").read_text()
        self.assertIn("Run output.", readme)
        self.assertIn("when the PR merges.", readme)
        self.assertIn("Owner: ", readme)
        self.assertTrue((root / "README.md").is_file())
        self.assertTrue((root / "runs/one").is_dir())

    def test_work_dir_keeps_a_readme_and_leaves_an_outside_directory_bare(self):
        root = self.tmp / "root"
        (root / "runs").mkdir(parents=True)
        (root / "runs/README.md").write_text("mine\n")
        result = self.sh(f'work_dir "$NEVR_WORK_ROOT/runs/one" a b; work_dir {self.tmp}/out/x a b',
                         NEVR_WORK_ROOT=str(root))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((root / "runs/README.md").read_text(), "mine\n")
        self.assertTrue((self.tmp / "out/x").is_dir())
        self.assertFalse((self.tmp / "out/x/README.md").exists())


class PythonToolDefaultsTest(unittest.TestCase):
    """Each Python tool's default location, read in a child process."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="work-root-defaults-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)

    def printed(self, code, **env):
        result = subprocess.run([sys.executable, "-I", "-c", code], env=child_env(**env), capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return pathlib.Path(result.stdout.strip())

    def cases(self):
        run_tool_tests = (f"import importlib.util as u; s = u.spec_from_file_location('r', {str(TOOLS / 'run_tool_tests.py')!r}); "
                          "m = u.module_from_spec(s); s.loader.exec_module(m); print(m.DEFAULT_LOG_DIR)")
        run_scenario = (f"import sys; sys.path.insert(0, {str(TOOLS / 'scenario')!r}); import run_scenario; "
                        "print(run_scenario.SCRATCH)")
        control = (f"import sys; sys.path.insert(0, {str(TOOLS / 'scenario')!r}); import control; print(control.LOG)")
        systest = (f"import sys; sys.path.insert(0, {str(TOOLS / 'winvm')!r}); import systest; "
                   "print(systest.default_out_dir())")
        return [
            (run_tool_tests, pathlib.Path("test-runs/tools-tests")),
            (run_scenario, pathlib.Path("scenario-runs")),
            (control, pathlib.Path("control-log.jsonl")),
            (systest, None),  # winvm-<timestamp>: only its parent is fixed
        ]

    def check(self, expected_root, **env):
        for code, relative in self.cases():
            found = self.printed(code, **env)
            if relative is None:
                self.assertEqual(found.parent, expected_root, code)
                self.assertTrue(found.name.startswith("winvm-"), found)
            else:
                self.assertEqual(found, expected_root / relative, code)

    @unittest.skipIf(importlib.util.find_spec("pytest") is None, "pytest is not installed")
    def test_run_tool_tests_keeps_its_records_under_the_root_with_a_readme(self):
        probe = self.tmp / "probe" / "test_probe.py"
        probe.parent.mkdir()
        probe.write_text("def test_probe():\n    pass\n")
        root = self.tmp / "named"
        result = subprocess.run([sys.executable, str(TOOLS / "run_tool_tests.py"), "-q", str(probe)],
                                env=child_env(NEVR_WORK_ROOT=str(root), HOME=str(self.tmp / "home")), cwd=self.tmp,
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((root / "test-runs/tools-tests/runs.jsonl").is_file())
        self.assertIn("run_tool_tests.py", (root / "test-runs/README.md").read_text())
        self.assertTrue((root / "README.md").is_file())

    def test_the_variable_moves_every_default(self):
        self.check(self.tmp / "named", NEVR_WORK_ROOT=str(self.tmp / "named"), HOME=str(self.tmp / "home"))

    def test_without_the_variable_every_default_is_under_the_home_directory(self):
        self.check(self.tmp / "home" / DEFAULT_RELATIVE, HOME=str(self.tmp / "home"))


class JustfileTest(unittest.TestCase):
    """Source shape (evidence rank 5): test-auth-unit takes its scratch from the shell definition and hands
    test_nevr_config a directory inside the one it removes."""

    def recipe(self):
        recipe = just_recipe.recipe((REPO / "justfile").read_text(), "test-auth-unit")
        self.assertIsNotNone(recipe, "justfile no longer has a test-auth-unit recipe")
        return recipe

    def test_the_recipe_sources_the_shell_definition_and_makes_its_scratch_with_a_readme(self):
        recipe = self.recipe()
        self.assertIn("source tools/lib/work_root.sh", recipe)
        self.assertIn('work_dir "$NEVR_WORK_ROOT/test-scratch"', recipe)
        self.assertIn('mktemp -d "$NEVR_WORK_ROOT/test-scratch/', recipe)

    def test_the_recipe_gives_test_nevr_config_a_directory_that_is_removed_with_the_fixtures(self):
        recipe = self.recipe()
        self.assertIn('export NEVR_CONFIG_TEST_DIR="$sigfix/nevr-config-tests"', recipe)
        self.assertLess(recipe.index("trap 'rm -rf \"$sigfix\"' EXIT"), recipe.index("NEVR_CONFIG_TEST_DIR"))


class ToolsMakeTheirDirectoriesWithAReadmeTest(unittest.TestCase):
    """Source shape (evidence rank 5), for the entry points that only run with a game, a VM or a socket:
    each makes its output directory through work_root.ensure_dir, not a bare mkdir."""

    CASES = {
        "tools/scenario/run_scenario.py": "work_root.ensure_dir(out, SCRATCH_PURPOSE, SCRATCH_REMOVED)",
        "tools/scenario/run_all.py": "run_scenario.work_root.ensure_dir(out, run_scenario.SCRATCH_PURPOSE",
        "tools/scenario/control.py": "work_root.ensure_dir(LOG.parent,",
        "tools/winvm/systest.py": "work_root.ensure_dir(out,",
    }

    def test_each_entry_point_uses_ensure_dir(self):
        for name, call in self.CASES.items():
            text = (REPO / name).read_text()
            self.assertIn(call, text, name)
            self.assertNotIn("out.mkdir(", text, name)
            self.assertNotIn("LOG.parent.mkdir(", text, name)


class ShellToolDefaultsTest(unittest.TestCase):
    """launch-client.sh, verify-server.sh and the game-run lock, run against the fake install with no
    per-tool override: their output lands under the work root."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="work-root-shell-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.game_root = fixtures.make_game_root(self.tmp / "main")
        self.fake_bin = self.tmp / "bin"
        fixtures.make_fake_bin(self.fake_bin)
        self.checkout = self.tmp / "checkout"
        self.checkout.mkdir()
        fixtures.install_scripts(self.checkout)
        self.dll = self.tmp / "test.dll"
        self.dll.write_bytes(fixtures.TEST_DLL)
        (self.tmp / "nevr-runtime.dll").write_bytes(fixtures.TEST_RUNTIME)
        bin_dir = self.checkout / "build/mingw-release/bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "BugSplat64.dll").write_bytes(fixtures.TEST_DLL)
        (bin_dir / "nevr-runtime.dll").write_bytes(fixtures.TEST_RUNTIME)
        self.env = child_env(
            PATH=f"{self.fake_bin}:{os.environ['PATH']}",
            NEVR_GAME_ROOT=str(self.game_root),
            NEVR_EVIDENCE_DELAY="0",
            NEVR_VERIFY_MIN_SECONDS="1",
            NEVR_VERIFY_POLL_SECONDS="1",
            FAKE_EXPECT_DLL=str(self.dll),
        )
        self.env.pop("FAKE_ECHOVR_PIDS", None)

    def launch_client(self, **env):
        return subprocess.run([str(self.checkout / "launch-client.sh"), "--dll", str(self.dll), "--device-code"],
                              env=dict(self.env, **env), cwd=self.checkout, capture_output=True, text=True, timeout=60)

    def verify_server(self, **env):
        return subprocess.run([str(self.checkout / "verify-server.sh"), "run1", "default", "1"],
                              env=dict(self.env, FAKE_EXPECT_DLL=str(self.checkout / "build/mingw-release/bin/BugSplat64.dll"), **env),
                              cwd=self.checkout, capture_output=True, text=True, timeout=60)

    def test_launch_client_writes_its_run_and_its_lock_under_the_named_root(self):
        root = self.tmp / "named"
        result = self.launch_client(NEVR_WORK_ROOT=str(root), HOME=str(self.tmp / "home"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        runs = sorted(root.glob("client-run-*"))
        self.assertEqual(len(runs), 1, sorted(p.name for p in root.iterdir()))
        self.assertTrue((root / "launch-client.lock").exists())
        self.assertTrue((root / "README.md").is_file())
        self.assertTrue((runs[0] / "README.md").is_file())
        self.assertFalse((self.tmp / "home").exists())

    def test_launch_client_without_the_variable_uses_the_home_directory(self):
        home = self.tmp / "home"
        home.mkdir()
        result = self.launch_client(HOME=str(home))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        root = home / DEFAULT_RELATIVE
        self.assertEqual(len(sorted(root.glob("client-run-*"))), 1, result.stderr)
        self.assertTrue((root / "launch-client.lock").exists())

    def test_every_client_run_directory_carries_a_readme(self):
        root = self.tmp / "named"
        self.assertEqual(self.launch_client(NEVR_WORK_ROOT=str(root)).returncode, 0)
        run = next(root.glob("client-run-*"))
        readme = (run / "README.md").read_text()
        self.assertIn("launch-client.sh", readme)
        self.assertIn("Owner: ", readme)
        self.assertIn("Removed: ", readme)

    def test_verify_server_writes_server_runs_under_the_named_root(self):
        root = self.tmp / "named"
        result = self.verify_server(NEVR_WORK_ROOT=str(root))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((root / "server-runs/run1/server.log").is_file())
        readme = (root / "server-runs/README.md").read_text()
        self.assertIn("verify-server.sh", readme)

    def test_taking_the_game_run_lock_makes_the_root_with_its_readme(self):
        root = self.tmp / "fresh"
        game_root = fixtures.make_game_root(self.tmp / "game-root")  # the lock first checks the root for sign-in files (#1198)
        result = subprocess.run(["bash", "-c", f"set -eu; source {fixtures.LIB}; GAME_ROOT={game_root}; acquire_game_run_lock; echo $GAME_RUN_LOCK"],
                                env=dict(self.env, NEVR_WORK_ROOT=str(root)), capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), f"{root}/launch-client.lock")
        self.assertIn("Owner: ", (root / "README.md").read_text())

    def test_the_per_tool_overrides_still_win(self):
        root = self.tmp / "named"
        scratch = self.tmp / "scratch"
        lock = self.tmp / "my.lock"
        result = self.launch_client(NEVR_WORK_ROOT=str(root), NEVR_RUN_SCRATCH_ROOT=str(scratch),
                                    NEVR_LAUNCH_LOCK=str(lock))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(sorted(scratch.glob("client-run-*"))), 1)
        self.assertTrue(lock.exists())
        self.assertFalse((root / "launch-client.lock").exists())
        self.assertEqual(sorted(root.glob("client-run-*")), [])


if __name__ == "__main__":
    unittest.main()
