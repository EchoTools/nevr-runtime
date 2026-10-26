"""#749: the Quest OBB skip chooses its data root before it hooks the game.

Install() reaches libr15's GOT, which no host test can; the host test (obb_skip_hook_test) covers the choice
and what the hook serves. What it cannot see is that Install() makes the choice at all, and before the path
hook can answer, so that is pinned here by source shape.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]


class InstallChoosesTheRootFirst(unittest.TestCase):
    def test_install_resolves_the_root_with_the_real_filesystem_before_the_first_thunk(self):
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(source, "bool Install() noexcept {")
        self.assertIn("ResolveRoot(&DirectoryExists)", body)
        self.assertLess(body.index("ResolveRoot(&DirectoryExists)"), body.index("PathThunk::Arm"))

    def test_install_hooks_the_engines_data_root_question_after_the_root_is_chosen(self):
        """#866: with no OBB file neither OBB hook ever runs, so the data-root slot is what redirects the engine."""
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(source, "bool Install() noexcept {")
        self.assertIn("InstallThunk<DataRootThunk>(g_dataRootHook, sentinel::pinned::LibR15GetDataRootDir())", body)
        self.assertLess(body.index("ResolveRoot(&DirectoryExists)"), body.index("DataRootThunk::Arm"))
        self.assertLess(body.index("DataRootThunk::Arm"), body.index("InstallThunk<DataRootThunk>"))
        self.assertIn("g_dataRootHookLive.store(dataRoot == sentinel::GotStatus::kOk", body)
        # The install result includes it: a refused slot is an error line and a false return, not a quiet pass.
        self.assertRegex(body, r"const bool ok = [^;]*dataRoot == sentinel::GotStatus::kOk")

    def test_the_data_step_asks_the_hook_for_the_engines_root_when_no_mount_was_skipped(self):
        source = (ROOT / "src/quest/sentinel/data_wiring.cpp").read_text()
        body = extract_braced_function(source, "EngineRootChoice EngineRoot() {")
        self.assertIn("obb_skip::DataRootRedirect()", body)
        self.assertIn('"data_root_hook"', body)
        self.assertLess(body.index("mountSkipped"), body.index("DataRootRedirect"))




class TheGamesCallPathDoesNoWork(unittest.TestCase):
    """#757: the handlers the game calls touch neither the filesystem nor the log.

    ResolveRoot does that work once, before the game runs; a handler that stats a path or writes a line runs on the
    game's own thread inside its OBB wait. The host test counts the log lines but cannot see a stat.
    """

    FORBIDDEN = re.compile(
        r"\b(DirectoryExists|HoldsData|Resolve|ResolveRoot|stat|lstat|fstat|access|open|fopen|opendir|readdir|"
        r"LogFields|Log|Emit|printf|fprintf|snprintf|syslog|__android_log_print|malloc|new)\b"
    )

    def handler(self, signature, must_contain):
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(source, signature)
        self.assertIn(must_contain, body, f"{signature}: the body read is not the handler (a sensor over nothing)")
        return body

    def test_the_mount_handler_does_no_filesystem_or_log_work(self):
        body = self.handler("void MountHandler(", "g_skipped.store")
        self.assertIsNone(self.FORBIDDEN.search(body), self.FORBIDDEN.search(body))

    def test_the_path_handler_does_no_filesystem_or_log_work(self):
        body = self.handler("const char* PathHandler(", "g_root.load")
        self.assertIsNone(self.FORBIDDEN.search(body), self.FORBIDDEN.search(body))

    def test_the_data_root_handler_does_no_filesystem_or_log_work(self):
        body = self.handler("const char* DataRootHandler(", "g_dataRootRedirect.load")
        self.assertIsNone(self.FORBIDDEN.search(body), self.FORBIDDEN.search(body))


class InstallCallsResolveRootUnconditionally(unittest.TestCase):
    """#757: `if (false) ResolveRoot(...)` and `#if 0` around it must not pass the sensor above."""

    def install_body(self):
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        return extract_braced_function(source, "bool Install() noexcept {")

    def test_resolve_root_is_the_first_statement_of_install(self):
        body = self.install_body()
        self.assertRegex(body, r"^\{\s*ResolveRoot\(&DirectoryExists\);")

    def test_install_has_no_preprocessor_conditional(self):
        self.assertIsNone(re.search(r"^\s*#\s*(if|ifdef|ifndef|elif|else)\b", self.install_body(), re.M))


if __name__ == "__main__":
    unittest.main()
