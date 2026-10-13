"""#519: tools/verify_mode_patch_ground_truth.py must keep the spectator-stream site
(0x116F3D, SPECTATORSTREAM_CHECK) in its byte-rewrite inventory now that mode_patches.cpp
writes it through nevr_checked_patch::Apply with a ProcessMemcpy-forwarding lambda, and it
must still fail closed on any other ProcessMemcpy form or a site without ground truth."""

import tempfile
import unittest
from pathlib import Path

from tools import verify_mode_patch_ground_truth as verifier


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/runtime/patch/mode_patches.cpp"
ADDRESSES = ROOT / "src/runtime/hook/addresses.h"
GROUND_TRUTH = ROOT / "tests/system/mode_patches_test.go"
LAMBDA = "[](void* dest, const void* src, size_t n) { return ProcessMemcpy(dest, src, n); }"
SPECTATOR_RVA = 0x116F3D


class VerifyModePatchGroundTruth(unittest.TestCase):
    def run_verifier(self, source: str | None = None, ground_truth: str | None = None):
        with tempfile.TemporaryDirectory() as tmp:
            src = SOURCE
            truth = GROUND_TRUTH
            if source is not None:
                src = Path(tmp) / "mode_patches.cpp"
                src.write_text(source)
            if ground_truth is not None:
                truth = Path(tmp) / "mode_patches_test.go"
                truth.write_text(ground_truth)
            return verifier.verify(src, ADDRESSES, truth)

    def test_shipped_source_inventories_the_spectator_stream_site(self):
        rewrites, covered = self.run_verifier()
        self.assertIn(SPECTATOR_RVA, rewrites)
        self.assertIn(SPECTATOR_RVA, covered)

    def test_shipped_source_uses_the_form_the_verifier_is_taught(self):
        # Guards the test itself: the lambda below must be what the source really contains.
        self.assertIn(LAMBDA, SOURCE.read_text())

    def test_a_lambda_that_writes_something_else_is_not_recognised(self):
        for wrong in (
            "[](void* dest, const void* src, size_t n) { return ProcessMemcpy(dest, src, 0); }",
            "[](void* dest, const void* src, size_t n) { return ProcessMemcpy(dest, dest, n); }",
            "[](void* dest, const void* src, size_t n) { return ProcessMemcpy(src, dest, n); }",
        ):
            with self.subTest(lambda_body=wrong):
                source = SOURCE.read_text().replace(LAMBDA, wrong)
                self.assertNotEqual(source, SOURCE.read_text())
                with self.assertRaises(verifier.InventoryError):
                    self.run_verifier(source=source)

    def test_a_site_that_is_not_a_named_rva_is_not_recognised(self):
        needle = "reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress) + nevr_patch_addresses::SPECTATORSTREAM_CHECK;"
        source = SOURCE.read_text()
        self.assertIn(needle, source)
        mutated = source.replace(needle, "reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress) + offset;")
        with self.assertRaises(verifier.InventoryError):
            self.run_verifier(source=mutated)

    def test_a_site_without_binary_ground_truth_is_refused(self):
        truth = GROUND_TRUTH.read_text()
        self.assertIn("0x116F3D", truth)
        with self.assertRaisesRegex(verifier.InventoryError, "0x116F3D"):
            self.run_verifier(ground_truth=truth.replace("0x116F3D", "0x116F3E"))


if __name__ == "__main__":
    unittest.main()
