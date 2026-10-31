"""#947: the game's permission request is made with a defined request code, wired the way the hooks need.

The host tests (permission_request_test, permission_wait_hook_test) drive the request against a fake JNI and the
handler through its thunk entry. What they cannot see is that the production entry arms the requester before the hook
is installed, binds it to the game's own activity, and that the Android build compiles the file without exceptions.
That is pinned here by source shape.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]
SENTINEL = ROOT / "src/quest/sentinel"


def code(name):
    return re.sub(r"//[^\n]*", "", (SENTINEL / name).read_text())


class TheEntryArmsTheRequester(unittest.TestCase):
    def test_the_requester_is_set_before_the_hook_is_installed(self):
        body = extract_braced_function(code("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("nevr_quest::permission_wait::SetRequester(&RequestOnGameActivity);", body)
        self.assertLess(body.index("SetRequester(&RequestOnGameActivity)"), body.index("permission_wait::Install()"))

    def test_the_requester_asks_on_the_activity_android_main_was_given(self):
        body = extract_braced_function(
            code("entry.cpp"),
            "const char* RequestOnGameActivity(const char* const* names, std::uint64_t count, int requestCode) noexcept {",
        )
        self.assertIn("permission_request::Request(nevr_quest::process_end::CurrentApp(), names, count, requestCode)", body)


class TheHandlerCallsTheGameOnlyWhenTheRequesterCouldNot(unittest.TestCase):
    def test_the_original_is_behind_the_requested_test(self):
        body = extract_braced_function(code("permission_wait_hook.cpp"), "void RequestHandler(")
        self.assertIn("request(askNames, askCount, kRequestCode)", body)
        self.assertRegex(
            body,
            r'const bool ours = std::strcmp\(result, "requested"\) == 0;\s*path = "ours";\s*made = true;\s*if \(!ours\) \{\s*original\(askNames, askCount\);',
        )
        self.assertEqual(body.count("original("), 1)


class TheHandlerWaitsOnlyWhenStorageWasAskedAndARequestReachedJava(unittest.TestCase):
    def test_the_wait_is_behind_needed_and_made(self):
        body = extract_braced_function(code("permission_wait_hook.cpp"), "void RequestHandler(")
        self.assertIn("if (needed && made) {", body)
        self.assertLess(body.index("if (needed && made) {"), body.index("wait(names, count)"))


class TheEntryDecidesStorageNeededFromTheEngineRoot(unittest.TestCase):
    def test_the_predicate_is_set_before_the_hook_is_installed(self):
        body = extract_braced_function(code("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("nevr_quest::permission_wait::SetStorageNeeded(&nevr_quest::data_wiring::StorageNeeded);", body)
        self.assertLess(
            body.index("SetStorageNeeded(&nevr_quest::data_wiring::StorageNeeded)"),
            body.index("permission_wait::Install()"),
        )


class TheRequestIsBuiltLikeTheHookPath(unittest.TestCase):
    def test_it_is_compiled_without_exceptions_and_linked_into_the_sentinel(self):
        cmake = (SENTINEL / "CMakeLists.txt").read_text()
        library = cmake[cmake.index("add_library(ovrplatformloader SHARED"):]
        self.assertIn("permission_request.cpp", library[:library.index(")")])
        flags = re.search(r"set_source_files_properties\(([^)]*)PROPERTIES COMPILE_OPTIONS \"-fno-exceptions\"\)", cmake)
        self.assertIsNotNone(flags)
        self.assertIn("permission_request.cpp", flags.group(1).split())

    def test_every_jni_call_is_checked_for_an_exception_before_its_result(self):
        text = code("permission_request.cpp")
        self.assertNotRegex(text, r"if \([a-zA-Z]+ == nullptr \|\| Pending\(env\)\)")
        self.assertIn("ExceptionClear", text)
        self.assertIsNone(re.search(r"catch\s*\(", text))
        self.assertIn('"([Ljava/lang/String;I)V"', text)
        self.assertIn("static_cast<jint>(requestCode)", text)


if __name__ == "__main__":
    unittest.main()
