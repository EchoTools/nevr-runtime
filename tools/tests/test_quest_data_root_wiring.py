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
        body = extract_braced_function(source, "bool InstallWith(const InstallEnv& env) noexcept {")
        self.assertIn("ResolveRoot(env.dirExists, env.fileExists)", body)
        self.assertLess(body.index("ResolveRoot(env.dirExists, env.fileExists)"), body.index("PathThunk::Arm"))

    def test_install_is_install_with_over_the_real_filesystem_and_the_three_real_slots(self):
        """The host test drives InstallWith with fakes (#932); this pins that production Install() is that call, with the
        real directory check, the real file check and the real installers of the three libr15 slots."""
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(source, "bool Install() noexcept {")
        self.assertRegex(
            body,
            r"^\{\s*return InstallWith\(\{&DirectoryExists, &PathExists, &InstallPathSlot, &InstallMountSlot, "
            r"&InstallDataRootSlot\}\);\s*\}$",
        )
        slots = {
            "InstallPathSlot": "InstallThunk<PathThunk>(g_pathHook, sentinel::pinned::LibR15GetMountedObbPath())",
            "InstallMountSlot": "InstallThunk<MountThunk>(g_mountHook, sentinel::pinned::LibR15MountObb())",
            "InstallDataRootSlot": "InstallThunk<DataRootThunk>(g_dataRootHook, sentinel::pinned::LibR15GetDataRootDir())",
        }
        for name, call in slots.items():
            self.assertIn(call, extract_braced_function(source, f"sentinel::GotStatus {name}() noexcept {{"))

    def test_install_hooks_the_engines_data_root_question_after_the_root_is_chosen(self):
        """#866: with no OBB file neither OBB hook ever runs, so the data-root slot is what redirects the engine."""
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(source, "bool InstallWith(const InstallEnv& env) noexcept {")
        self.assertIn("env.installDataRoot()", body)
        self.assertLess(body.index("ResolveRoot(env.dirExists, env.fileExists)"), body.index("DataRootThunk::Arm"))
        self.assertLess(body.index("DataRootThunk::Arm"), body.index("env.installDataRoot()"))
        self.assertIn("g_dataRootHookLive.store(dataRoot == sentinel::GotStatus::kOk", body)
        # The install result includes it: a refused slot is an error line and a false return, not a quiet pass.
        self.assertRegex(body, r"const bool ok = [^;]*dataRoot == sentinel::GotStatus::kOk")

    def test_the_redirect_needs_the_manifest_the_data_step_checks_not_just_a_data_directory(self):
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(
            source, "void ResolveRoot(data_root::ExistsFn dirExists, data_root::ExistsFn fileExists) noexcept {"
        )
        self.assertIn("HoldsManifest(r.root, fileExists)", body)
        self.assertRegex(body, r"g_dataRootRedirect\.store\(answer")
        # C1: the answer is the legacy root only when it alone holds the data, the media root otherwise (any body).
        self.assertIn("legacyHoldsData ? data_root::kLegacyRoot : data_root::kMediaRoot", body)
        helper = extract_braced_function(source, "bool HoldsManifest(")
        self.assertIn("data::kManifestPath", helper)

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

    # The names each handler body may use: an allowlist, so a name not listed fails whatever it is called. The denylist
    # that stood here passed any call it did not name (#771).
    MOUNT_NAMES = frozenset({
        "if", "callback", "nullptr", "original", "manager", "filename", "key", "data", "return", "g_skipped", "store",
        "true", "std", "memory_order_release", "g_mountSkipped", "fetch_add", "memory_order_relaxed", "sentinel",
        "pinned", "kObbStateMounted",
    })
    PATH_NAMES = frozenset({
        "if", "g_skipped", "load", "std", "memory_order_acquire", "return", "original", "manager", "filename",
        "g_pathServed", "fetch_add", "memory_order_relaxed", "g_root",
    })
    DATAROOT_NAMES = frozenset({
        "const", "char", "redirect", "g_dataRootRedirect", "load", "std", "memory_order_acquire", "nullptr", "if",
        "return", "original", "g_dataRootServed", "fetch_add", "memory_order_relaxed",
    })

    def handler(self, signature, must_contain):
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(source, signature)
        self.assertIn(must_contain, body, f"{signature}: the body read is not the handler (a sensor over nothing)")
        return body

    @staticmethod
    def unlisted(body, allowed):
        return sorted(set(re.findall(r"[A-Za-z_]\w*", body)) - allowed)

    def test_the_mount_handler_does_no_filesystem_or_log_work(self):
        body = self.handler("void MountHandler(", "g_skipped.store")
        self.assertEqual(self.unlisted(body, self.MOUNT_NAMES), [])

    def test_the_path_handler_does_no_filesystem_or_log_work(self):
        body = self.handler("const char* PathHandler(", "g_root.load")
        self.assertEqual(self.unlisted(body, self.PATH_NAMES), [])

    def test_the_data_root_handler_does_no_filesystem_or_log_work(self):
        body = self.handler("const char* DataRootHandler(", "g_dataRootRedirect.load")
        self.assertEqual(self.unlisted(body, self.DATAROOT_NAMES), [])


class AMutantCallIsCaught(unittest.TestCase):
    """#771: a call the handler does not need, under a name no denylist lists, must fail the sensor. Each mutant here
    inserts one such call into a real handler body and asserts the sensor reports it."""

    MUTANT_CALL = "  Helper();\n"

    def mutant_of(self, signature, must_contain):
        body = TheGamesCallPathDoesNoWork().handler(signature, must_contain)
        return body.replace("{\n", "{\n" + self.MUTANT_CALL, 1)

    def test_the_mount_handler_sensor_catches_an_unlisted_call(self):
        mutant = self.mutant_of("void MountHandler(", "g_skipped.store")
        self.assertEqual(TheGamesCallPathDoesNoWork.unlisted(mutant, TheGamesCallPathDoesNoWork.MOUNT_NAMES), ["Helper"])

    def test_the_path_handler_sensor_catches_an_unlisted_call(self):
        mutant = self.mutant_of("const char* PathHandler(", "g_root.load")
        self.assertEqual(TheGamesCallPathDoesNoWork.unlisted(mutant, TheGamesCallPathDoesNoWork.PATH_NAMES), ["Helper"])

    def test_the_data_root_handler_sensor_catches_an_unlisted_call(self):
        mutant = self.mutant_of("const char* DataRootHandler(", "g_dataRootRedirect.load")
        self.assertEqual(
            TheGamesCallPathDoesNoWork.unlisted(mutant, TheGamesCallPathDoesNoWork.DATAROOT_NAMES), ["Helper"]
        )


class InstallCallsResolveRootUnconditionally(unittest.TestCase):
    """#757: `if (false) ResolveRoot(...)` and `#if 0` around it must not pass the sensor above."""

    def install_body(self):
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        return extract_braced_function(source, "bool InstallWith(const InstallEnv& env) noexcept {")

    def test_resolve_root_is_the_first_statement_of_install(self):
        body = self.install_body()
        self.assertRegex(body, r"^\{\s*ResolveRoot\(env\.dirExists, env\.fileExists\);")

    def test_install_has_no_preprocessor_conditional(self):
        self.assertIsNone(re.search(r"^\s*#\s*(if|ifdef|ifndef|elif|else)\b", self.install_body(), re.M))


if __name__ == "__main__":
    unittest.main()
