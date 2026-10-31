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
LAMBDA = """[](void* dest, const void* src, size_t n) {
        DWORD error = 0;
        return g_spectatorWriteErrors.Note(ProcessMemcpy(dest, src, n, &error), error);
      }"""
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

    def test_shipped_source_inventories_every_offline_site_written_through_the_apply_helper(self):
        # PatchEnableOffline writes through `apply("site", OFFSET, ...)`, a lambda over ApplyPatch (#728); the
        # verifier reads each call site's RVA, and skips only the lambda's own ApplyPatch(offset, ...).
        rewrites, covered = self.run_verifier()
        offline = {0xFDE0E, 0x17F0B1, 0x17F77B, 0x17F823, 0x1AC83E, 0xA7C685}
        self.assertLessEqual(offline, rewrites)
        self.assertLessEqual(offline, covered)

    def test_an_apply_call_site_that_is_not_a_named_rva_is_not_recognised(self):
        needle = 'apply("tutorial", OFFLINE_TUTORIAL,'
        source = SOURCE.read_text()
        self.assertIn(needle, source)
        with self.assertRaises(verifier.InventoryError):
            self.run_verifier(source=source.replace(needle, 'apply("tutorial", tutorialOffset,'))

    def test_shipped_source_uses_the_form_the_verifier_is_taught(self):
        # Guards the test itself: the lambda below must be what the source really contains.
        self.assertIn(LAMBDA, SOURCE.read_text())

    def test_a_lambda_that_writes_something_else_is_not_recognised(self):
        for wrong in (
            "[](void* dest, const void* src, size_t n) { return ProcessMemcpy(dest, src, 0); }",
            "[](void* dest, const void* src, size_t n) { return ProcessMemcpy(dest, dest, n); }",
            "[](void* dest, const void* src, size_t n) { return ProcessMemcpy(src, dest, n); }",
            # The error must be noted, not dropped: a writer that discards it is the defect this pins.
            "[](void* dest, const void* src, size_t n) { DWORD error = 0; return ProcessMemcpy(dest, src, n, &error); }",
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

    def decoy_function(self, rva_name: str) -> str:
        return (
            "VOID DecoySite() {\n"
            "  uint8_t* const site = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress) + "
            f"nevr_patch_addresses::{rva_name};\n"
            "}\n\n"
        )

    def test_a_decoy_site_earlier_in_the_file_does_not_stand_in_for_the_real_one(self):
        # SERVER_FLAGS_CHECK has ground truth; the spectator site's does not (0x116F3D -> 0x116F3E).
        # The decoy declares `site` for a covered RVA and the span to the real Apply holds no
        # ProcessMemcpy, so an unbounded span attributes the real write to the decoy and passes.
        truth = GROUND_TRUTH.read_text().replace("0x116F3D", "0x116F3E")
        source = SOURCE.read_text()
        anchor = "nevr_checked_patch::Result PatchSpectatorStreamCheck("
        self.assertIn(anchor, source)
        mutated = source.replace(anchor, self.decoy_function("SERVER_FLAGS_CHECK") + anchor)
        with self.assertRaisesRegex(verifier.InventoryError, "0x116F3D"):
            self.run_verifier(source=mutated, ground_truth=truth)

    def test_a_decoy_in_the_same_function_does_not_stand_in_for_the_real_one(self):
        truth = GROUND_TRUTH.read_text().replace("0x116F3D", "0x116F3E")
        source = SOURCE.read_text()
        real = "  uint8_t* const site = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress) + nevr_patch_addresses::SPECTATORSTREAM_CHECK;"
        self.assertIn(real, source)
        decoy = real.replace("SPECTATORSTREAM_CHECK", "SERVER_FLAGS_CHECK")
        mutated = source.replace(real, decoy + "\n" + real)
        with self.assertRaisesRegex(verifier.InventoryError, "0x116F3D"):
            self.run_verifier(source=mutated, ground_truth=truth)

    def test_a_decoy_does_not_stand_in_for_a_site_declared_another_way(self):
        # An earlier function declares `site` for a covered RVA and closes at column 0.  The real
        # function declares its `site` with brace-init, so no `site =` stands between the decoy and
        # the real Apply.  Only the column-0 closing brace stops the span: without it the decoy is
        # attributed to the real Apply, the real site drops out of the inventory unflagged, and the
        # census is satisfied by the decoy.
        source = SOURCE.read_text()
        anchor = "nevr_checked_patch::Result PatchSpectatorStreamCheck("
        real = (
            "uint8_t* const site = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress) + "
            "nevr_patch_addresses::SPECTATORSTREAM_CHECK;"
        )
        self.assertIn(anchor, source)
        self.assertIn(real, source)
        braced = real.replace("site = ", "site{").replace(";", "};")
        mutated = source.replace(real, braced).replace(
            anchor, self.decoy_function("SERVER_FLAGS_CHECK") + anchor
        )
        with self.assertRaisesRegex(verifier.InventoryError, "unrecognized ProcessMemcpy"):
            self.run_verifier(source=mutated)

    def test_the_shipped_site_is_the_one_inventoried_when_a_decoy_precedes_it(self):
        source = SOURCE.read_text()
        anchor = "nevr_checked_patch::Result PatchSpectatorStreamCheck("
        mutated = source.replace(anchor, self.decoy_function("SERVER_FLAGS_CHECK") + anchor)
        rewrites, _covered = self.run_verifier(source=mutated)
        self.assertIn(SPECTATOR_RVA, rewrites)


if __name__ == "__main__":
    unittest.main()
