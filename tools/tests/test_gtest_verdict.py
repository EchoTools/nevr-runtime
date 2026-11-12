"""tools/gtest_verdict.py: a GTest binary passes on GTest's own result, never on its exit code alone (#2152)."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "gtest_verdict.py"


def result(body: str) -> str:
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + body + "\n"


class GTestVerdictTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = pathlib.Path(self._tmp.name)

    def judge(self, text=None):
        path = self.dir / "result.xml"
        if text is not None:
            path.write_text(text, encoding="utf-8")
        proc = subprocess.run([sys.executable, str(TOOL), str(path)], capture_output=True, text=True, timeout=60)
        return proc.returncode, proc.stdout.strip()

    def test_a_passing_run_is_ok(self):
        code, out = self.judge(result('<testsuites tests="3" failures="0" disabled="0" errors="0" name="AllTests"/>'))
        self.assertEqual((code, out), (0, "ok 3 tests"))

    def test_a_missing_file_is_not_a_pass(self):
        code, out = self.judge(None)
        self.assertEqual(code, 1)
        self.assertIn("result unreadable", out)

    def test_an_empty_file_is_not_a_pass(self):
        code, out = self.judge("")
        self.assertEqual(code, 1)
        self.assertIn("result unreadable", out)

    def test_a_run_with_no_tests_is_not_a_pass(self):
        code, out = self.judge(result('<testsuites tests="0" failures="0" errors="0" name="AllTests"/>'))
        self.assertEqual((code, out), (1, "no pass: no tests ran"))

    def test_a_failure_or_an_error_is_not_a_pass(self):
        code, out = self.judge(result('<testsuites tests="5" failures="2" errors="1" name="AllTests"/>'))
        self.assertEqual((code, out), (1, "no pass: 2 failed, 1 errors of 5 tests"))

    def test_an_error_alone_is_not_a_pass(self):
        code, out = self.judge(result('<testsuites tests="2" failures="0" errors="1" name="AllTests"/>'))
        self.assertEqual((code, out), (1, "no pass: 0 failed, 1 errors of 2 tests"))

    def test_another_xml_document_is_not_a_gtest_result(self):
        code, out = self.judge(result("<html/>"))
        self.assertEqual(code, 1)
        self.assertIn("not a GTest result", out)

    def test_counts_that_are_not_numbers_are_not_a_pass(self):
        code, out = self.judge(result('<testsuites tests="many" failures="0" errors="0"/>'))
        self.assertEqual(code, 1)
        self.assertIn("not numbers", out)

    def test_no_argument_is_a_usage_error(self):
        proc = subprocess.run([sys.executable, str(TOOL)], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
