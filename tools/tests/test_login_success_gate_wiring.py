"""The LogInSuccess bit 9 gate (patch/login_success_gate.cpp) is one detour, installed from boot, and no byte patch
writes its branch any more.

The GTest in src/runtime/tests/test_login_success_gate.cpp drives the hook; these are the source sensors for what it
cannot reach: the boot call and its mode condition, and the removal of the two byte writers.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


class BootInstallsOneDetourForFourModes(unittest.TestCase):
    def test_install_is_called_under_the_four_mode_condition(self):
        boot = read("src/runtime/lifecycle/boot.cpp")
        self.assertIn(
            "if (g_isOffline || g_isServer || g_isHeadless || g_isWindowed) {", boot
        )
        self.assertIn("nevr_login_success_gate::Install(", boot)

    def test_a_server_stops_when_the_detour_does_not_install(self):
        boot = read("src/runtime/lifecycle/boot.cpp")
        call = boot.index("nevr_login_success_gate::Install(")
        tail = boot[call : call + 400]
        self.assertIn("g_isServer", tail)
        self.assertIn("ServerFatal(\"LogInSuccess hook not installed", tail)


class TheByteWritersAreGone(unittest.TestCase):
    def test_mode_patches_no_longer_writes_the_login_branch(self):
        source = read("src/runtime/patch/mode_patches.cpp")
        self.assertNotIn("LOGIN_CAP_CHECK", source)
        self.assertNotIn("transaction_1", source)
        self.assertNotIn("OFFLINE_TRANSACTION_1", source)
        self.assertIn('apply("transaction_2", OFFLINE_TRANSACTION_2', source)

    def test_addresses_header_has_no_first_transaction_constant(self):
        self.assertNotIn("OFFLINE_TRANSACTION_1", read("src/runtime/hook/addresses.h"))


class TheSourceIsBuiltAndInventoried(unittest.TestCase):
    def test_source_and_test_are_in_cmake(self):
        cmake = read("src/runtime/CMakeLists.txt")
        self.assertIn('"patch/login_success_gate.cpp"', cmake)
        self.assertIn("add_executable(test_login_success_gate", cmake)

    def test_source_is_in_the_patch_inventory(self):
        self.assertIn("patch/login_success_gate.cpp", read("tools/verify_patch_source_inventory.py"))


if __name__ == "__main__":
    unittest.main()
