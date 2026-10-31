"""The combat content pack is made from the owner's game-data zip (tools/quest_pack_from_game_data.py).

The game-data zip is a synthetic one, and the header of pins is a fixture written with the digests of the synthetic bytes
(the tool reads the pins from the header text, so the fixture is the same shape as src/quest/data/package_pins.h). The
data-folder version and manifest id come from the real src/quest/data/plan.h. No game file is used.
"""

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "quest_pack_from_game_data.py"
REPACK = ROOT / "tools" / "quest_repack_libs.py"
BASE = "_data/5932408047/rad15/android/"
MANIFEST = BASE + "manifests/48037dc70b0ecab2"
PACKAGE = BASE + "packages/48037dc70b0ecab2_1"
STOCK_PACKAGE = BASE + "packages/48037dc70b0ecab2_0"
REQUIRES = "762c16ae58c9bc70eee071d8c0e2e8fc0408d1f0691032164029294c5cb7ec48"
SHA = lambda data: hashlib.sha256(data).hexdigest()  # noqa: E731
MANIFEST_BYTES = b"synthetic manifest bytes for the combat pack test"
PACKAGE_BYTES = b"synthetic package bytes, the one the pack adds" * 3


def header_text(manifest, package):
    return f"""// Fixture of the pins header (the shape of src/quest/data/package_pins.h).
// They describe ONE pack release (combatw19s 1.1.9, which requires the stock manifest
// {REQUIRES}).
inline constexpr PinnedPackage kCombatPackPackages[] = {{
    {{0, 9u, "{SHA(b'stock package, length checked only')}", false}},
    {{1, {len(package)}u, "{SHA(package)}", true}},
}};
inline constexpr PinnedDataSet kPinnedDataSets[] = {{
    {{"{SHA(manifest)}", kCombatPackPackages,
     sizeof(kCombatPackPackages) / sizeof(kCombatPackPackages[0])}},
}};
"""


class QuestCombatPack(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.header = self.root / "package_pins.h"
        self.header.write_text(header_text(MANIFEST_BYTES, PACKAGE_BYTES))
        self.game = self.root / "echovr-data.zip"
        self.write_game({MANIFEST: MANIFEST_BYTES, PACKAGE: PACKAGE_BYTES, STOCK_PACKAGE: b"stock"})
        self.out = self.root / "out"

    def write_game(self, entries):
        with zipfile.ZipFile(self.game, "w") as zf:
            for name, data in entries.items():
                zf.writestr(name, data)

    def run_tool(self, expected=None):
        expected = expected or SHA(self.game.read_bytes())
        return subprocess.run(
            [sys.executable, "-I", str(TOOL), "--game-data-zip", str(self.game), "--expected-sha256", expected,
             "--out", str(self.out), "--header", str(self.header)],
            capture_output=True, text=True)

    def test_the_pack_holds_the_two_entries_and_a_meta_file_naming_the_pack(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.out / "pack.zip") as zf:
            self.assertEqual(sorted(zf.namelist()), sorted([MANIFEST, PACKAGE]))
            self.assertEqual(zf.read(MANIFEST), MANIFEST_BYTES)
            self.assertEqual(zf.read(PACKAGE), PACKAGE_BYTES)
        meta = json.loads((self.out / "pack_meta.json").read_text())
        self.assertEqual(meta, {"id": "combat-pack", "requires_stock_manifest_sha256": [REQUIRES],
                                "version": "1.1.9", "zip_prefix": BASE})

    def test_the_output_is_accepted_by_stage_pack_and_pack_problems(self):
        self.assertEqual(self.run_tool().returncode, 0)
        store = self.root / "store.apk"
        with zipfile.ZipFile(store, "w") as zf:
            zf.writestr("lib/arm64-v8a/libr15.so", b"stock")
        stage = self.root / "stage"
        stage.mkdir()
        staged = subprocess.run(
            [sys.executable, "-I", str(REPACK), "stage-pack", "--pack-zip", str(self.out / "pack.zip"),
             "--meta", str(self.out / "pack_meta.json"), "--stage", str(stage), "--store-apk", str(store)],
            capture_output=True, text=True)
        self.assertEqual(staged.returncode, 0, staged.stderr)
        problems = subprocess.run(
            [sys.executable, "-I", str(REPACK), "pack-problems", "--stage", str(stage)], capture_output=True, text=True)
        self.assertEqual(problems.returncode, 0, problems.stderr)

    def test_a_changed_manifest_is_refused_against_the_pinned_digest(self):
        self.write_game({MANIFEST: MANIFEST_BYTES + b"!", PACKAGE: PACKAGE_BYTES})
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"{MANIFEST} digest does not match the data set's pinned manifest digest", result.stderr)
        self.assertFalse((self.out / "pack.zip").exists())

    def test_a_changed_package_is_refused_against_its_pinned_length_and_digest(self):
        self.write_game({MANIFEST: MANIFEST_BYTES, PACKAGE: PACKAGE_BYTES[:-1] + b"?"})
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"{PACKAGE} digest does not match its pinned digest", result.stderr)

    def test_a_missing_package_is_refused(self):
        self.write_game({MANIFEST: MANIFEST_BYTES})
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"the game-data zip has no {PACKAGE}", result.stderr)

    def test_a_game_zip_whose_digest_is_not_the_signed_one_is_refused_before_it_is_read(self):
        result = self.run_tool(expected="0" * 64)
        self.assertEqual(result.returncode, 1)
        self.assertIn("game-data zip sha256 is", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_pack_of_nine_files_is_refused_by_stage_pack(self):
        # ADR 0029 section 1: a pack holds one to eight files. The format allows a third file; the ninth is refused.
        self.assertEqual(self.run_tool().returncode, 0)
        with zipfile.ZipFile(self.out / "pack.zip", "a") as zf:
            for i in range(2, 9):
                zf.writestr(BASE + f"packages/48037dc70b0ecab2_{i}", b"extra %d" % i)
        store = self.root / "store.apk"
        with zipfile.ZipFile(store, "w") as zf:
            zf.writestr("lib/arm64-v8a/libr15.so", b"stock")
        stage = self.root / "stage"
        stage.mkdir()
        staged = subprocess.run(
            [sys.executable, "-I", str(REPACK), "stage-pack", "--pack-zip", str(self.out / "pack.zip"),
             "--meta", str(self.out / "pack_meta.json"), "--stage", str(stage), "--store-apk", str(store)],
            capture_output=True, text=True)
        self.assertNotEqual(staged.returncode, 0, "a pack of nine files must be refused")


if __name__ == "__main__":
    unittest.main()
