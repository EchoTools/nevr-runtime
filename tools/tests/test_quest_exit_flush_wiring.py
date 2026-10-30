"""#1391: the login prompt's seventeen counters are written once at process exit.

The flush itself is host-tested (login_prompt_hook_test, TheExitFlushWritesEachCounterOnce). OnGameProcessEnd is
Android-only, so no host test reaches its call; this pins the call, its order and the seam by source shape.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


class TheExitFlushIsCalledAtTheProcessEnd(unittest.TestCase):
    def test_the_process_end_callback_flushes_before_the_self_check_report(self):
        body = extract_braced_function(
            read("src/quest/integration/production_steps.cpp"), "void OnGameProcessEnd() noexcept {"
        )
        self.assertIn("nevr_quest::integration::FlushLoginPromptCountersAtExit();", body)
        self.assertLess(
            body.index("nevr_quest::integration::FlushLoginPromptCountersAtExit();"),
            body.index("ReportAndroidMainReturned();"),
        )

    def test_the_seam_is_declared_in_the_integration_header(self):
        self.assertIn("void FlushLoginPromptCountersAtExit() noexcept;", read("src/quest/integration/entry_hooks.h"))

    def test_the_seam_is_defined_over_the_sentinel_flush(self):
        self.assertIn(
            "void FlushLoginPromptCountersAtExit() noexcept { nevr_quest::login_prompt::FlushCountersAtExit(); }",
            read("src/quest/sentinel/entry.cpp"),
        )

    def test_the_flush_writes_the_exit_reason_the_host_test_counts(self):
        body = extract_braced_function(
            read("src/quest/sentinel/login_prompt_hook.cpp"), "void FlushCountersAtExit() noexcept {"
        )
        self.assertIn('{"why", "exit"}', body)


if __name__ == "__main__":
    unittest.main()
