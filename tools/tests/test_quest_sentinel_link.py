"""verify_quest_sentinel_link.py: which Quest sentinel link graphs it accepts and which it rejects.

Each case is a small tree under build/test-scratch/ (inside the repo's ignored build directory):
src/quest/**/CMakeLists.txt and tests/quest/elf_groundtruth_test.go, as the tool reads them.
"""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("verify_quest_sentinel_link", ROOT / "tools" / "verify_quest_sentinel_link.py")
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)

SCRATCH = ROOT / "build" / "test-scratch" / "quest-sentinel-link"

SENTINEL_CMAKE = """\
add_library(ovrplatformloader SHARED entry.cpp)
target_link_libraries(ovrplatformloader PRIVATE {libs})
{options}
"""
FLAG = "target_link_options(ovrplatformloader PRIVATE -Wl,--exclude-libs,ALL)"
GO_OK = "package quest\n\nfunc TestExportAllowlist(t *testing.T) {\n\tcheck()\n}\n"


class Tree:
    def __init__(self, name):
        self.root = SCRATCH / name
        (self.root / "src" / "quest" / "sentinel").mkdir(parents=True, exist_ok=True)
        (self.root / "tests" / "quest").mkdir(parents=True, exist_ok=True)

    def sentinel(self, libs="nevr_quest_got_hook breakpad_client log dl", options=FLAG):
        (self.root / "src/quest/sentinel/CMakeLists.txt").write_text(SENTINEL_CMAKE.format(libs=libs, options=options))
        return self

    def go(self, text=GO_OK, name="elf_groundtruth_test.go"):
        (self.root / "tests/quest" / name).write_text(text)
        return self

    def add(self, rel, text):
        p = self.root / "src" / "quest" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return self

    def run(self):
        return TOOL.check(self.root)


class SentinelLinkGuard(unittest.TestCase):
    def assertOk(self, tree):
        code, message = tree.run()
        self.assertEqual(code, 0, message)

    def assertFails(self, tree, needle):
        code, message = tree.run()
        self.assertEqual(code, 1, message)
        self.assertIn(needle, message)

    def test_the_repository_as_it_is_passes(self):
        code, message = TOOL.check(ROOT)
        self.assertEqual(code, 0, message)

    def test_a_sentinel_that_links_nothing_heavy_needs_no_guard(self):
        self.assertOk(Tree("plain").sentinel(options="").go("package quest\n"))

    def test_token_auth_with_the_flag_and_a_live_allowlist_test_passes(self):
        self.assertOk(Tree("direct-ok").sentinel(libs="nevr_quest_token_auth breakpad_client").go())

    def test_token_auth_linked_directly_without_the_flag_fails(self):
        self.assertFails(Tree("direct-noflag").sentinel(libs="nevr_quest_token_auth", options="").go(),
                         "--exclude-libs,ALL")

    def test_the_flag_only_in_a_comment_does_not_count(self):
        self.assertFails(Tree("comment-flag").sentinel(libs="nevr_quest_token_auth",
                                                       options="# was: target_link_options(ovrplatformloader PRIVATE -Wl,--exclude-libs,ALL)").go(),
                         "--exclude-libs,ALL")

    def test_token_auth_reached_through_an_intermediate_target_is_seen(self):
        t = Tree("intermediate").sentinel(libs="nevr_quest_net breakpad_client", options="").go()
        t.add("net/CMakeLists.txt", "add_library(nevr_quest_net STATIC net.cpp)\n"
                                    "target_link_libraries(nevr_quest_net PUBLIC nevr_quest_session_router nevr_quest_token_auth CURL::libcurl)\n")
        self.assertFails(t, "nevr_quest_token_auth")

    def test_a_dependency_added_from_the_parent_cmake_file_is_seen(self):
        t = Tree("parent").sentinel(options="").go()
        t.add("CMakeLists.txt", "target_link_libraries(ovrplatformloader PRIVATE nevr_quest_token_auth)\n")
        self.assertFails(t, "nevr_quest_token_auth")

    def test_libcurl_and_openssl_linked_directly_are_seen(self):
        self.assertFails(Tree("curl-direct").sentinel(libs="CURL::libcurl OpenSSL::SSL", options="").go(), "CURL::libcurl")

    def test_a_dependency_in_an_included_cmake_file_is_seen(self):
        t = Tree("included").sentinel(options="").go()
        t.add("cmake/extra.cmake", "target_link_libraries(nevr_quest_net INTERFACE OpenSSL::Crypto)\n")
        t.add("net/CMakeLists.txt", "target_link_libraries(ovrplatformloader PRIVATE nevr_quest_net)\n")
        self.assertFails(t, "OpenSSL::Crypto")

    def test_an_allowlist_test_that_was_renamed_fails(self):
        self.assertFails(Tree("renamed").sentinel(libs="nevr_quest_token_auth").go(GO_OK.replace("TestExportAllowlist", "testExportAllowlistDisabled")),
                         "TestExportAllowlist")

    def test_an_allowlist_test_that_skips_itself_fails(self):
        self.assertFails(Tree("skipped").sentinel(libs="nevr_quest_token_auth").go(
            GO_OK.replace("check()", 't.Skip("disabled")')), "skips itself")

    def test_a_missing_test_file_fails(self):
        self.assertFails(Tree("nofile").sentinel(libs="nevr_quest_token_auth").go(name="renamed_test.go"), "missing")

    def test_unreadable_build_files_are_an_error_not_a_pass(self):
        t = Tree("empty")
        with self.assertRaises(OSError):
            t.run()


if __name__ == "__main__":
    unittest.main()
