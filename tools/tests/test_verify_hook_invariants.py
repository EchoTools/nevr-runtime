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


if __name__ == "__main__":
    unittest.main()
