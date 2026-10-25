"""tools/run_tool_tests.py: every tools/tests run leaves a record that names its failures (#913).

These tests run the real script, as `just test-tools` does, against a two-test suite with one failure.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "run_tool_tests.py"
JUSTFILE = REPO / "justfile"

SUITE = '''
def test_holds():
    assert True

def test_breaks():
    assert 1 == 2
'''


class RunToolTestsRecord(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="run-tool-tests-"))
        self.addCleanup(shutil.rmtree, self.root)
        self.suite = self.root / "suite"
        self.suite.mkdir()
        (self.suite / "test_sample.py").write_text(SUITE, encoding="utf-8")
        self.logs = self.root / "logs"

    def run_script(self, log_dir: Path) -> subprocess.CompletedProcess:
        env = dict(os.environ, NEVR_TEST_LOG_DIR=str(log_dir))
        return subprocess.run([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(self.suite)],
                              capture_output=True, text=True, cwd=REPO, env=env, timeout=120)

    def test_a_failing_run_keeps_its_output_names_the_failure_and_keeps_pytests_exit_code(self):
        result = self.run_script(self.logs)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

        records = [json.loads(line) for line in (self.logs / "runs.jsonl").read_text().splitlines()]
        self.assertEqual(len(records), 1)
        record = records[0]
        self.assertEqual(record["rc"], 1)
        self.assertEqual(record["failed"], ["test_sample::test_breaks"])
        self.assertIn("1 failed, 1 passed", record["summary"])
        self.assertIn(record["tree"], ("clean", "DIRTY"))
        self.assertRegex(record["head"], r"^[0-9a-f]{40}$")

        log = Path(record["log"]).read_text()
        self.assertEqual(Path(record["log"]).parent, self.logs)
        self.assertIn(f"test-tools: HEAD {record['head']}", log)
        self.assertIn("FAILED", log)
        self.assertIn("test_breaks", log)
        self.assertIn("rc=1", log)

        cases = {c.get("name"): c for c in ET.parse(record["junit"]).getroot().iter("testcase")}
        self.assertIsNotNone(cases["test_breaks"].find("failure"))
        self.assertIsNone(cases["test_holds"].find("failure"))
        self.assertIn(f"test-tools: log {record['log']}", result.stdout)

    def test_each_run_appends_its_own_record(self):
        self.run_script(self.logs)
        self.run_script(self.logs)
        records = [json.loads(line) for line in (self.logs / "runs.jsonl").read_text().splitlines()]
        self.assertEqual(len(records), 2)
        self.assertEqual(len({r["junit"] for r in records} | {r["log"] for r in records}), 4)

    def test_an_unwritable_log_directory_refuses_the_run(self):
        blocker = self.root / "a-file"
        blocker.write_text("")
        result = self.run_script(blocker / "logs")
        self.assertEqual(result.returncode, 2)
        self.assertIn("test-tools: FAIL cannot write the run record", result.stderr)
        self.assertNotIn("passed", result.stdout)

    def test_the_record_says_whether_the_tree_was_clean(self):
        repo = self.root / "repo"
        repo.mkdir()
        git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
        subprocess.run([*git, "init", "-q"], check=True)
        (repo / "f").write_text("x")
        subprocess.run([*git, "add", "f"], check=True)
        subprocess.run([*git, "commit", "-q", "-m", "c"], check=True)
        env = dict(os.environ, NEVR_TEST_LOG_DIR=str(self.logs))
        argv = [sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(self.suite)]
        subprocess.run(argv, capture_output=True, text=True, cwd=repo, env=env, timeout=120)
        (repo / "untracked").write_text("y")
        subprocess.run(argv, capture_output=True, text=True, cwd=repo, env=env, timeout=120)
        records = [json.loads(line) for line in (self.logs / "runs.jsonl").read_text().splitlines()]
        self.assertEqual([r["tree"] for r in records], ["clean", "DIRTY"])

    def test_the_recipe_runs_this_script(self):
        text = JUSTFILE.read_text()
        self.assertIn("\ntest-tools *ARGS:\n", text)
        self.assertIn("python3 tools/run_tool_tests.py {{ ARGS }}", text)


if __name__ == "__main__":
    unittest.main()
