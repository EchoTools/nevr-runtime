#!/usr/bin/env python3
"""Regression tests for the Tier-0 hook-invariant verifier."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
VERIFIER = REPO / "tools" / "verify_hook_invariants.py"


class VerifyHookInvariantsTest(unittest.TestCase):
    def test_rejects_new_self_collision_in_isolated_source_fixture(self):
        """A newly called detour target must fail instead of being silently accepted."""
        with tempfile.TemporaryDirectory(
            prefix="hook-invariants-", dir="/var/tmp/work-nevr-runtime"
        ) as temp_dir:
            fixture = pathlib.Path(temp_dir)
            shutil.copytree(REPO / "src", fixture / "src")
            shutil.copytree(REPO / "plugins", fixture / "plugins")
            (fixture / "tools").mkdir()
            shutil.copy2(VERIFIER, fixture / "tools" / VERIFIER.name)

            functions = fixture / "src" / "abi" / "echovr_functions.cpp"
            functions.write_text(
                functions.read_text()
                + "\nUnexpectedHook = (UnexpectedHookFunc*)(g_GameBaseAddress + 0x110AB0);\n"
            )

            result = subprocess.run(
                [sys.executable, fixture / "tools" / VERIFIER.name],
                cwd=fixture,
                capture_output=True,
                check=False,
                text=True,
            )

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("SELF-COLLISION: 0x140110AB0", result.stderr)
        self.assertIn("EchoVR::UnexpectedHook", result.stderr)
        self.assertIn("PatchAddresses::INIT_GLOBAL_GAMESPACE", result.stderr)

    def _run_in_fixture(self, extra_runtime_source=None):
        with tempfile.TemporaryDirectory(
            prefix="hook-invariants-", dir="/var/tmp/work-nevr-runtime"
        ) as temp_dir:
            fixture = pathlib.Path(temp_dir)
            shutil.copytree(REPO / "src", fixture / "src")
            shutil.copytree(REPO / "plugins", fixture / "plugins")
            (fixture / "tools").mkdir()
            shutil.copy2(VERIFIER, fixture / "tools" / VERIFIER.name)
            if extra_runtime_source is not None:
                (fixture / "src" / "runtime" / "patch" / "fixture_duplicate.cpp").write_text(
                    extra_runtime_source
                )
            return subprocess.run(
                [sys.executable, fixture / "tools" / VERIFIER.name],
                cwd=fixture,
                capture_output=True,
                check=False,
                text=True,
            )

    def test_rejects_two_runtime_detours_on_one_target(self):
        """#93: an EchoVR:: pointer detour on the address initialize.cpp already hooks inline."""
        result = self._run_in_fixture(
            "void Fixture() {\n"
            '  InstallBootDetour(&EchoVR::GetProcAddress, nullptr, "EchoVR::GetProcAddress",\n'
            "                    BootHookRequirement::kOptional);\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DUPLICATE-RUNTIME-DETOUR: 0x1400EAEF0", result.stderr)
        self.assertIn("fixture_duplicate.cpp: EchoVR::GetProcAddress", result.stderr)
        self.assertIn("initialize.cpp: inline 0x1400eaef0", result.stderr)

    def test_rejects_a_hook_table_row_without_a_prologue(self):
        """#254: a table-driven detour with a nullptr prologue installs without checking the bytes."""
        result = self._run_in_fixture(
            "static constexpr uint64_t VA_FIXTURE = 0x140123450;\n"
            "struct HookEntry { unsigned long long va; void* detour; void** original; const char* name;\n"
            "                   const unsigned char* prologue; unsigned char prologue_len; const char* why; };\n"
            "HookEntry hooks[] = {\n"
            '  { VA_FIXTURE, (void*)&FixtureHook, (void**)&s_origFixture, "FixtureHook", nullptr, 0, "why" },\n'
            "};\n"
            "void Install() { MH_CreateHook(target, hooks[0].detour, hooks[0].original); }\n"
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("UNVALIDATED-HOOK: FixtureHook (VA_FIXTURE)", result.stderr)

    def test_sees_a_table_row_that_duplicates_an_existing_detour(self):
        """#254: the 0x1400EAEF0 detour in initialize.cpp is also a table row."""
        result = self._run_in_fixture(
            "static constexpr uint64_t VA_DUP = 0x1400EAEF0;\n"
            "static const unsigned char kProlog[4] = {1, 2, 3, 4};\n"
            "HookEntry hooks[] = {\n"
            '  { VA_DUP, (void*)&DupHook, (void**)&s_origDup, "DupHook", kProlog, 4, "why" },\n'
            "};\n"
            "void Install() { MH_CreateHook(target, hooks[0].detour, hooks[0].original); }\n"
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DUPLICATE-RUNTIME-DETOUR: 0x1400EAEF0", result.stderr)
        self.assertIn("fixture_duplicate.cpp: MH_CreateHook table VA_DUP", result.stderr)

    def test_sees_an_mh_createhook_target_built_from_a_literal_rva(self):
        """#254: `g_GameBaseAddress + 0xEAEF0` passed to MH_CreateHook names 0x1400EAEF0."""
        result = self._run_in_fixture(
            "void Install() {\n"
            "  void* t = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + 0xEAEF0);\n"
            "  MH_CreateHook(t, (void*)&Hook, (void**)&orig);\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DUPLICATE-RUNTIME-DETOUR: 0x1400EAEF0", result.stderr)
        self.assertIn("fixture_duplicate.cpp: MH_CreateHook literal RVA 0xEAEF0", result.stderr)

    def test_accepts_the_tree_as_it_is(self):
        result = self._run_in_fixture()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("DUPLICATE-RUNTIME-DETOUR", result.stderr)


if __name__ == "__main__":
    unittest.main()
