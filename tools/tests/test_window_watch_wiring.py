"""#341: the window watch is wired into the per-frame dispatcher, on clients only, and is checked before it writes.

The GTests in src/runtime/tests/test_window_watch.cpp drive the pure decision (frame/window_watch.h). What they
cannot reach is the wiring: that DispatchPerFrameWork polls it, that a server never does, and that the write of the
game's quit flag happens only after the site's bytes were compared.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


class WindowWatchWiring(unittest.TestCase):
    def test_the_dispatcher_polls_the_watch(self):
        body = strip_comments(extract_braced_function(read("src/runtime/frame/tick.cpp"),
                                                      "void DispatchPerFrameWork(uint64_t nowUs)"))
        self.assertIn("nevr_window_watch::Poll(", body)

    def test_the_watch_is_in_the_runtime_sources(self):
        self.assertIn('"frame/window_watch.cpp"', read("src/runtime/CMakeLists.txt"))

    def test_a_server_never_polls(self):
        body = strip_comments(extract_braced_function(read("src/runtime/frame/window_watch.cpp"),
                                                      "void Poll(uint64_t nowMs)"))
        self.assertLess(body.index("g_isServer"), body.index("EnumWindows"))

    def test_the_quit_flag_is_written_only_after_the_site_bytes_match(self):
        body = strip_comments(extract_braced_function(read("src/runtime/frame/window_watch.cpp"),
                                                      "bool RequestGameQuit()"))
        self.assertLess(body.index("QuitSiteMatches("), body.index("kQuitFlagOffset"))

    def test_the_decision_asks_for_a_quit_once_through_the_flag_only(self):
        code = strip_comments(read("src/runtime/frame/window_watch.cpp"))
        for forbidden in ("PostQuitMessage", "TerminateProcess", "ExitProcess", "DestroyWindow", "SendMessage"):
            self.assertNotIn(forbidden, code)


if __name__ == "__main__":
    unittest.main()
