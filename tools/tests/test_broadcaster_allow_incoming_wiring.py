"""#1774 P13: the allow_incoming detour on CBroadcaster::Initialize (0x140F7FE50) replaces the byte patch.

Source sensors for what the GTest in src/runtime/tests/test_broadcaster_allow_incoming.cpp cannot reach: the install
and the server escalation live in mode_patches.cpp (PatchEnableServer), which no test target links.

  - the byte patch is gone: no ALLOW_INCOMING constant and no MOV EAX,1 write to the site;
  - PatchEnableServer installs the hook, and the install checks the prologue before it attaches or writes anything;
  - the install's failure goes to ServerFatal on a server and to a Warning on a client, and never sets
    g_bootHookFailed (boot.cpp reads that flag before PatchEnableServer runs, so it would do nothing here);
  - the detour forwards all fifteen parameters and returns the original's result.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def mode_patches() -> str:
    return strip_comments(read("src/runtime/patch/mode_patches.cpp"))


def hook_source() -> str:
    return strip_comments(read("src/runtime/patch/broadcaster_allow_incoming.cpp"))


class TheBytePatchIsGone(unittest.TestCase):
    def test_no_constant_or_write_for_the_old_site_remains(self):
        self.assertNotIn("ALLOW_INCOMING", strip_comments(read("src/runtime/hook/addresses.h")))
        self.assertNotIn("allowIncoming", mode_patches())
        self.assertNotIn("0xF7F904", mode_patches())


class TheInstallIsOnTheServerPath(unittest.TestCase):
    def test_patch_enable_server_installs_it(self):
        body = extract_braced_function(mode_patches(), "VOID PatchEnableServer()")
        self.assertIn("InstallBroadcasterAllowIncoming();", body)

    def test_the_prologue_is_checked_before_the_attach_and_the_write(self):
        body = extract_braced_function(mode_patches(), "static void InstallBroadcasterAllowIncoming()")
        check = body.index("nevr::ValidatePrologue(")
        self.assertLess(check, body.index("OriginalBroadcasterInitialize ="))
        self.assertLess(check, body.index("PatchDetour("))
        self.assertIn("nevr_patch_addresses::BROADCASTER_INITIALIZE", body)

    def test_the_detour_is_attached_with_a_name_and_the_hook_is_logged(self):
        body = extract_braced_function(mode_patches(), "static void InstallBroadcasterAllowIncoming()")
        self.assertIn('"BroadcasterInitialize")', body)
        self.assertIn("[NEVR.PATCH] hooked name=BroadcasterInitialize", body)

    def test_a_failed_check_logs_the_expected_and_actual_bytes_at_warning(self):
        body = extract_braced_function(mode_patches(), "static void InstallBroadcasterAllowIncoming()")
        self.assertIn("LogLevel::Warning", body)
        self.assertIn("expected=%s actual=%s", body)


class TheFailureEscalatesOnAServer(unittest.TestCase):
    def test_server_fatal_only_under_the_server_flag(self):
        body = extract_braced_function(mode_patches(), "static void EscalateBroadcasterAllowIncomingFailure(")
        self.assertRegex(body, r"if \(g_isServer\) \{\s*ServerFatal\(")
        self.assertIn("} else {", body)
        self.assertIn("LogLevel::Warning", body.split("} else {", 1)[1])

    def test_it_does_not_use_the_boot_flag(self):
        body = extract_braced_function(mode_patches(), "static void EscalateBroadcasterAllowIncomingFailure(")
        self.assertNotIn("g_bootHookFailed", body)
        install = extract_braced_function(mode_patches(), "static void InstallBroadcasterAllowIncoming()")
        self.assertNotIn("g_bootHookFailed", install)


class TheDetourForwardsEverything(unittest.TestCase):
    def test_the_original_is_called_with_all_fifteen_parameters(self):
        body = extract_braced_function(hook_source(), "std::uint64_t BroadcasterInitializeHook(")
        self.assertIn(
            "return OriginalBroadcasterInitialize(self, nameHash, port, mask, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);",
            " ".join(body.split()),
        )

    def test_the_mask_is_forced_before_the_original_runs(self):
        body = extract_braced_function(hook_source(), "std::uint64_t BroadcasterInitializeHook(")
        self.assertLess(body.index("ForceAllowIncoming(mask)"), body.index("return OriginalBroadcasterInitialize("))


class TheSourceIsInTheBuild(unittest.TestCase):
    def test_cmake_compiles_the_source_and_the_inventory_pins_it(self):
        cmake = read("src/runtime/CMakeLists.txt")
        self.assertIn('"patch/broadcaster_allow_incoming.cpp"', cmake)
        self.assertIn("patch/broadcaster_allow_incoming.cpp", read("tools/verify_patch_source_inventory.py"))

    def test_the_identity_manifest_pins_the_site(self):
        self.assertIn('"va": "0x140F7FE50"', read("tools/hook_identity_manifest.json"))


if __name__ == "__main__":
    unittest.main()
