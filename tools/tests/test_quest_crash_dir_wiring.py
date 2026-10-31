"""#1797: the Quest crash writer and the crash sender read one resolved folder.

sentinel.cpp (breakpad) and production_steps.cpp are the device's wiring and no host recipe compiles them, so the
shape that makes the writer and the sender agree is pinned here; the choice itself (order, creation, fallback,
errno lines) is tested on the host in src/quest/tests/sentinel_host_test.cpp against the shipped crash_dir.h.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"


def code(rel: str) -> str:
    text = (QUEST / rel).read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class WriterAndSenderShareOneFolder(unittest.TestCase):
    def test_the_folder_is_chosen_once_by_the_shared_chooser(self):
        text = code("sentinel/sentinel.cpp")
        arm = extract_braced_function(text, "void Arm() {")
        self.assertIn("DumpDirStorage() = ResolveDumpDir();", arm)
        resolve = extract_braced_function(text, "std::string ResolveDumpDir() {")
        self.assertIn("crash_dir::Choose(", resolve)
        self.assertIn('#include "quest/sentinel/crash_dir.h"', text)

    def test_the_minidump_is_written_where_dumpdir_says(self):
        arm = extract_braced_function(code("sentinel/sentinel.cpp"), "void Arm() {")
        self.assertIn("google_breakpad::MinidumpDescriptor descriptor(DumpDirStorage());", arm)
        dump_dir = extract_braced_function(code("sentinel/sentinel.cpp"), "const char* DumpDir() {")
        self.assertIn("DumpDirStorage()", dump_dir)

    def test_the_readable_report_is_written_where_dumpdir_says(self):
        self.assertIn("const char* dir = DumpDir();", code("sentinel/crash_report_quest.cpp"))

    def test_the_sender_is_given_dumpdir_after_arm(self):
        step = extract_braced_function(code("integration/production_steps.cpp"), "void ArmCrashReporter() override {")
        self.assertIn("sentinel::Arm();", step)
        self.assertIn("StartCrashSend(sentinel::DumpDir(), NEVR_QUEST_PROJECT_VERSION);", step)
        self.assertLess(step.index("sentinel::Arm();"), step.index("StartCrashSend("))

    def test_the_sender_looks_nowhere_else(self):
        text = code("integration/crash_send_step.cpp")
        start = extract_braced_function(text, "void StartCrashSend(const char* dir, const char* build) noexcept {")
        self.assertIn('job->config.dir = dir != nullptr ? dir : "";', start)

    def test_no_other_source_names_a_crash_folder(self):
        # The folder names live in crash_dir.h alone; a second spelling would be a second place that could disagree.
        offenders = []
        for path in sorted(QUEST.rglob("*.cpp")) + sorted(QUEST.rglob("*.h")):
            if "tests" in path.parts or path.name == "crash_dir.h":
                continue
            if "nevr-crashes" in code(str(path.relative_to(QUEST))):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
