"""A Quest APK can carry a game-data pack as assets (#1608, docs/adr/0029-quest-bundled-content-pack.md).

Two layers. The unit tests call tools/quest_repack_libs.py (`stage-pack`, `pack-problems`, `verify-signed`) on small zip files
made here: every file of a pack gets every check, and a refusal writes nothing. The end-to-end tests run
`just android-repack-apk` for real (the fixture of test_quest_repack_version.py) with `pack` and `pack_meta`, and read the
SIGNED APK back with the standard zip reader; with `pack` empty the recipe carries nothing of it.

The data step's pins (the manifest id and the data version folder) are read from src/quest/data/plan.h, as the tool reads them.
"""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "tests"))
import quest_repack_libs as libs  # noqa: E402
import work_root  # noqa: E402
from quest_patcher_standin import MARKER, make_standin  # noqa: E402
from test_quest_repack_libs import make_zip  # noqa: E402
from test_quest_repack_version import DEV_VERSION, JUSTFILE, RepackFixture  # noqa: E402

VERSION_FOLDER, MANIFEST_ID = libs.pinned_manifest(REPO / "src/quest/data/plan.h")
PREFIX = f"_data/{VERSION_FOLDER}/rad15/android/"
MANIFEST = f"manifests/{MANIFEST_ID}"
PACKAGE = f"packages/{MANIFEST_ID}_1"
STOCK_A = hashlib.sha256(b"stock manifest a").hexdigest()
STOCK_B = hashlib.sha256(b"stock manifest b").hexdigest()
PACK_MANIFEST = b"pack manifest " * 40
PACK_PACKAGE = bytes(range(256)) * 300


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def meta(**changes) -> dict:
    document = {"id": "combat-pack", "version": "1.2.3", "requires_stock_manifest_sha256": [STOCK_B, STOCK_A],
                "zip_prefix": PREFIX}
    document.update(changes)
    return {k: v for k, v in document.items() if v is not None}


def good_entries() -> dict[str, bytes]:
    return {PREFIX + MANIFEST: PACK_MANIFEST, PREFIX + PACKAGE: PACK_PACKAGE}


def sparse_prefix(path: pathlib.Path, size: int, archive: bytes) -> None:
    """A file of `size` bytes that ends in the zip `archive` (a sparse hole in front: a zip reader finds the end record and
    takes the hole as a prefix), so a test can name an APK of any size without writing it."""
    with path.open("wb") as out:
        out.truncate(size - len(archive))
        out.seek(size - len(archive))
        out.write(archive)


class PackFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-pack-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.store = make_zip(self.tmp / "store.apk", {"AndroidManifest.xml": b"manifest", "lib/arm64-v8a/libr15.so": b"game"})
        self.stage = self.tmp / "stage"
        self.stage.mkdir()

    def make_pack(self, entries: dict[str, bytes] | None = None, **meta_changes) -> tuple[pathlib.Path, pathlib.Path]:
        pack = make_zip(self.tmp / "pack.zip", good_entries() if entries is None else entries)
        meta_path = self.tmp / "pack-meta.json"
        meta_path.write_text(json.dumps(meta(**meta_changes)))
        return pack, meta_path

    def stage_pack(self, entries: dict[str, bytes] | None = None, **meta_changes) -> list[str]:
        pack, meta_path = self.make_pack(entries, **meta_changes)
        return libs.stage_pack(pack, meta_path, self.stage, self.store)

    def staged_files(self) -> list[str]:
        root = self.stage / "assets"
        return sorted(p.relative_to(self.stage).as_posix() for p in root.rglob("*") if p.is_file()) if root.exists() else []

    def assert_refused(self, problems: list[str], *needles: str) -> None:
        self.assertTrue(problems, "a pack that must be refused was accepted")
        for needle in needles:
            self.assertTrue(any(needle in p for p in problems), (needle, problems))
        self.assertEqual(self.staged_files(), [], "a refused pack must write nothing")


class StagePackTest(PackFixture):
    def test_a_clean_pack_is_staged_with_the_bytes_and_a_description_of_them(self):
        self.assertEqual(self.stage_pack(), [])
        self.assertEqual(self.staged_files(), [f"assets/nevr-pack/files/{MANIFEST}", f"assets/nevr-pack/files/{PACKAGE}",
                                               "assets/nevr-pack/pack.json"])
        self.assertEqual((self.stage / "assets/nevr-pack/files" / MANIFEST).read_bytes(), PACK_MANIFEST)
        self.assertEqual((self.stage / "assets/nevr-pack/files" / PACKAGE).read_bytes(), PACK_PACKAGE)
        document = json.loads((self.stage / libs.PACK_JSON_ENTRY).read_bytes())
        zip_path = self.tmp / "pack.zip"
        self.assertEqual(document, {
            "schema": 1, "id": "combat-pack", "version": "1.2.3", "stock_data_version": VERSION_FOLDER,
            "requires_stock_manifest_sha256": sorted([STOCK_A, STOCK_B]),
            "files": [{"path": MANIFEST, "sha256": sha(PACK_MANIFEST), "size": len(PACK_MANIFEST)},
                      {"path": PACKAGE, "sha256": sha(PACK_PACKAGE), "size": len(PACK_PACKAGE)}],
            "source_zip_sha256": sha(zip_path.read_bytes()), "source_zip_size": zip_path.stat().st_size})
        self.assertEqual(libs.pack_problems(self.stage), [])

    def test_the_description_is_the_same_bytes_every_time(self):
        self.assertEqual(self.stage_pack(), [])
        first = (self.stage / libs.PACK_JSON_ENTRY).read_bytes()
        shutil.rmtree(self.stage / "assets")
        self.assertEqual(self.stage_pack(), [])
        self.assertEqual((self.stage / libs.PACK_JSON_ENTRY).read_bytes(), first)
        self.assertTrue(first.endswith(b"}\n") and b" " not in first.replace(b"combat-pack", b""))

    def test_one_digest_is_enough_and_the_digests_are_written_sorted(self):
        self.assertEqual(self.stage_pack(requires_stock_manifest_sha256=[STOCK_A]), [])
        self.assertEqual(json.loads((self.stage / libs.PACK_JSON_ENTRY).read_bytes())["requires_stock_manifest_sha256"], [STOCK_A])

    def test_the_command_line_stages_and_prints_each_files_digest(self):
        pack, meta_path = self.make_pack()
        run = subprocess.run([sys.executable, "-I", str(REPO / "tools/quest_repack_libs.py"), "stage-pack", "--pack-zip", str(pack),
                              "--meta", str(meta_path), "--stage", str(self.stage), "--store-apk", str(self.store)],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(f"pack file {PACKAGE} sha256 {sha(PACK_PACKAGE)} size {len(PACK_PACKAGE)}", run.stdout)
        self.assertIn("stage-pack OK", run.stdout)
        refused = subprocess.run([sys.executable, "-I", str(REPO / "tools/quest_repack_libs.py"), "stage-pack", "--pack-zip", str(pack),
                                  "--meta", str(self.tmp / "missing.json"), "--stage", str(self.tmp / "other"), "--store-apk",
                                  str(self.store)], capture_output=True, text=True)
        self.assertEqual(refused.returncode, 1)
        self.assertNotIn("OK", refused.stdout)

    # ---- every file of the zip gets every check -------------------------------------------------------------------------

    def test_an_entry_name_the_sentinels_reader_refuses_is_refused(self):
        for name in ("../escape", "/abs/path", "a\\b", "C:drive", "a/./b", "a//b", "x" * 300, "/".join("d" * 1 for _ in range(17)),
                     "bad\x01name"):
            with self.subTest(name=name):
                entries = good_entries()
                entries[PREFIX + name if not name.startswith("/") else name] = b"x"
                self.assert_refused(self.stage_pack(entries), "entry name the sentinel's zip reader refuses")

    def test_a_link_or_a_special_file_is_refused(self):
        for mode in (stat.S_IFLNK | 0o777, stat.S_IFCHR | 0o600, stat.S_IFIFO | 0o600):
            with self.subTest(mode=oct(mode)):
                pack = self.tmp / "pack.zip"
                with zipfile.ZipFile(pack, "w") as z:
                    z.writestr(PREFIX + MANIFEST, PACK_MANIFEST)
                    info = zipfile.ZipInfo(PREFIX + PACKAGE)
                    info.create_system = 3
                    info.external_attr = mode << 16
                    z.writestr(info, PACK_PACKAGE)
                meta_path = self.tmp / "m.json"
                meta_path.write_text(json.dumps(meta()))
                self.assert_refused(libs.stage_pack(pack, meta_path, self.stage, self.store), "a link or another special file")

    def test_an_encrypted_entry_is_refused(self):
        pack = make_zip(self.tmp / "pack.zip", good_entries())
        data = bytearray(pack.read_bytes())
        name = (PREFIX + PACKAGE).encode()
        at = next(i for i in range(len(data) - 46) if data[i:i + 4] == b"PK\x01\x02" and data[i + 46:i + 46 + len(name)] == name)
        data[at + 8] |= 0x1  # the central directory's general purpose flags: bit 0 is "encrypted"
        pack.write_bytes(bytes(data))
        meta_path = self.tmp / "m.json"
        meta_path.write_text(json.dumps(meta()))
        self.assert_refused(libs.stage_pack(pack, meta_path, self.stage, self.store), f"{PACKAGE}: encrypted")

    def test_two_names_that_are_one_name_are_refused(self):
        entries = good_entries()
        entries[PREFIX + "packages/Same_1"] = b"a"
        entries[PREFIX + "packages/same_1"] = b"b"
        self.assert_refused(self.stage_pack(entries), "the same name as")

    def test_the_same_name_twice_in_the_zip_is_refused(self):
        pack = self.tmp / "pack.zip"
        with zipfile.ZipFile(pack, "w") as z:
            z.writestr(PREFIX + MANIFEST, PACK_MANIFEST)
            z.writestr(PREFIX + PACKAGE, PACK_PACKAGE)
            with self.assertWarns(UserWarning):  # zipfile itself says "Duplicate name"
                z.writestr(PREFIX + PACKAGE, b"other bytes")
        meta_path = self.tmp / "m.json"
        meta_path.write_text(json.dumps(meta()))
        self.assert_refused(libs.stage_pack(pack, meta_path, self.stage, self.store), "the same name as")

    def test_a_file_outside_the_prefix_is_refused(self):
        entries = good_entries()
        entries["elsewhere/file"] = b"x"
        self.assert_refused(self.stage_pack(entries), "elsewhere/file: outside")

    def test_a_directory_entry_outside_the_prefix_is_refused_and_one_on_the_way_to_it_is_not(self):
        entries = good_entries()
        entries["_data/"] = b""
        entries[f"_data/{VERSION_FOLDER}/"] = b""
        entries[PREFIX + "packages/"] = b""
        self.assertEqual(self.stage_pack(entries), [])
        shutil.rmtree(self.stage / "assets")
        entries["other-dir/"] = b""
        self.assert_refused(self.stage_pack(entries), "a directory outside")

    def test_only_manifests_and_packages_are_places_for_a_file(self):
        for path in ("scripts/x", "packages/sub/x", "x", "manifests/other-manifest", "packages/.hidden",
                     "packages/x.nevr-pack.tmp", "packages/x.stock", "packages/x.nevr-owned", "packages/sp ace"):
            with self.subTest(path=path):
                entries = good_entries()
                entries[PREFIX + path] = b"x"
                self.assert_refused(self.stage_pack(entries), path)

    def test_a_pack_with_no_manifest_is_refused(self):
        self.assert_refused(self.stage_pack({PREFIX + PACKAGE: PACK_PACKAGE}), f"the zip has no {PREFIX}{MANIFEST}")

    def test_an_empty_file_is_refused(self):
        entries = good_entries()
        entries[PREFIX + PACKAGE] = b""
        self.assert_refused(self.stage_pack(entries), "0 bytes")

    def test_nine_files_are_refused(self):
        entries = good_entries()
        for i in range(8):
            entries[PREFIX + f"packages/extra{i}"] = b"x"
        self.assert_refused(self.stage_pack(entries), "at most 8")
        shutil.rmtree(self.stage, ignore_errors=True)
        self.stage.mkdir()
        del entries[PREFIX + "packages/extra7"]  # eight is fine: the manifest, the package and six more
        del entries[PREFIX + "packages/extra6"]
        self.assertEqual(self.stage_pack(entries), [])

    def test_an_entry_whose_recorded_size_is_not_its_size_is_refused(self):
        pack = make_zip(self.tmp / "pack.zip", good_entries())
        data = bytearray(pack.read_bytes())
        name = (PREFIX + PACKAGE).encode()
        at = next(i for i in range(len(data) - 46) if data[i:i + 4] == b"PK\x01\x02" and data[i + 46:i + 46 + len(name)] == name)
        # The central directory entry of the package: its uncompressed size (offset 24 in the header) is made one byte longer.
        size = int.from_bytes(data[at + 24:at + 28], "little")
        data[at + 24:at + 28] = (size + 1).to_bytes(4, "little")
        pack.write_bytes(bytes(data))
        meta_path = self.tmp / "m.json"
        meta_path.write_text(json.dumps(meta()))
        self.assert_refused(libs.stage_pack(pack, meta_path, self.stage, self.store), PACKAGE)

    def test_a_file_that_is_not_a_zip_is_refused(self):
        pack = self.tmp / "pack.zip"
        pack.write_bytes(b"not a zip")
        meta_path = self.tmp / "m.json"
        meta_path.write_text(json.dumps(meta()))
        self.assert_refused(libs.stage_pack(pack, meta_path, self.stage, self.store), "not a readable zip")

    # ---- the description (the meta file) ---------------------------------------------------------------------------------

    def test_every_unusable_meta_is_refused(self):
        cases = {
            "an id with a capital": {"id": "Combat"},
            "an id with a slash": {"id": "a/b"},
            "no id": {"id": None},
            "a two-part version": {"version": "1.2"},
            "a version with a letter": {"version": "1.2.x"},
            "no version": {"version": None},
            "no digest": {"requires_stock_manifest_sha256": []},
            "three digests": {"requires_stock_manifest_sha256": [sha(b"1"), sha(b"2"), sha(b"3")]},
            "a digest in capitals": {"requires_stock_manifest_sha256": [STOCK_A.upper()]},
            "a short digest": {"requires_stock_manifest_sha256": [STOCK_A[:-1]]},
            "the same digest twice": {"requires_stock_manifest_sha256": [STOCK_A, STOCK_A]},
            "a prefix that is not a data directory": {"zip_prefix": "_data/x/"},
            "a prefix without the trailing slash": {"zip_prefix": PREFIX[:-1]},
            "another data version than the data step's": {"zip_prefix": "_data/1234567890/rad15/android/"},
            "no prefix": {"zip_prefix": None},
        }
        for label, changes in cases.items():
            with self.subTest(label):
                self.assert_refused(self.stage_pack(**changes), "meta")

    def test_a_meta_that_is_not_json_is_refused(self):
        pack, meta_path = self.make_pack()
        meta_path.write_text("{not json")
        self.assert_refused(libs.stage_pack(pack, meta_path, self.stage, self.store), "cannot be read as a JSON object")

    def test_a_plan_header_with_no_manifest_pin_is_refused(self):
        pack, meta_path = self.make_pack()
        header = self.tmp / "plan.h"
        header.write_text("// no pin here\n")
        self.assert_refused(libs.stage_pack(pack, meta_path, self.stage, self.store, header), "names no kManifestPath")

    def test_the_pins_are_the_data_steps_not_literals_of_the_tool(self):
        header = self.tmp / "plan.h"
        header.write_text('inline constexpr const char* kManifestPath = "_data/7777/rad15/android/manifests/abc123";\n')
        entries = {"_data/7777/rad15/android/manifests/abc123": PACK_MANIFEST, "_data/7777/rad15/android/packages/abc123_1": PACK_PACKAGE}
        pack = make_zip(self.tmp / "pack.zip", entries)
        meta_path = self.tmp / "m.json"
        meta_path.write_text(json.dumps(meta(zip_prefix="_data/7777/rad15/android/")))
        self.assertEqual(libs.stage_pack(pack, meta_path, self.stage, self.store, header), [])
        self.assertEqual(json.loads((self.stage / libs.PACK_JSON_ENTRY).read_bytes())["stock_data_version"], "7777")
        self.assertEqual(libs.pack_problems(self.stage, header), [])
        self.assertTrue(libs.pack_problems(self.stage), "the real pins are other ones")

    # ---- the store APK -----------------------------------------------------------------------------------------------------

    def test_a_store_apk_that_already_has_a_pack_entry_is_refused(self):
        self.store = make_zip(self.tmp / "store.apk", {"AndroidManifest.xml": b"m", libs.PACK_JSON_ENTRY: b"{}"})
        self.assert_refused(self.stage_pack(), "already has the entry assets/nevr-pack/pack.json")

    def test_a_store_apk_that_cannot_be_read_is_refused(self):
        self.store = self.tmp / "no-such.apk"
        self.assert_refused(self.stage_pack(), "cannot be read as the store APK")

    def test_the_apk_must_stay_under_four_gib_with_the_files_the_slack_and_the_store(self):
        files = len(PACK_MANIFEST) + len(PACK_PACKAGE)
        archive = (self.tmp / "store.apk").read_bytes()
        slack = 8 * 1024 * 1024
        limit = 0xFFFFFFFF
        exact = limit - slack - files  # the largest store file that still fits
        sparse_prefix(self.tmp / "store.apk", exact, archive)
        self.assertEqual(self.stage_pack(), [], "an APK at exactly the limit must be accepted")
        shutil.rmtree(self.stage / "assets")
        sparse_prefix(self.tmp / "store.apk", exact + 1, archive)
        self.assert_refused(self.stage_pack(), "zip without zip64")


class PackProblemsTest(PackFixture):
    """The staged tree as the runtime reads it (the rules of src/quest/data/pack.cpp ParsePackJson)."""

    def setUp(self):
        super().setUp()
        self.assertEqual(self.stage_pack(), [])
        self.json_path = self.stage / libs.PACK_JSON_ENTRY

    def document(self) -> dict:
        return json.loads(self.json_path.read_bytes())

    def write(self, document: dict) -> None:
        self.json_path.write_text(json.dumps(document))

    def test_nothing_staged_is_fine_and_a_staged_pack_that_is_whole_is_fine(self):
        self.assertEqual(libs.pack_problems(self.stage), [])
        self.assertEqual(libs.pack_problems(self.tmp / "nothing-staged"), [])

    def test_every_rule_of_the_runtime_is_checked(self):
        pristine = self.document()

        def mutate(change):
            document = json.loads(json.dumps(pristine))  # every case starts from the whole description
            change(document)
            return document

        cases = {
            "schema 2": lambda d: d.update(schema=2),
            "schema true": lambda d: d.update(schema=True),
            "a bad id": lambda d: d.update(id="A B"),
            "a bad version": lambda d: d.update(version="1.2"),
            "another data version": lambda d: d.update(stock_data_version="1"),
            "no requires": lambda d: d.update(requires_stock_manifest_sha256=[]),
            "unsorted requires": lambda d: d.update(requires_stock_manifest_sha256=sorted([STOCK_A, STOCK_B], reverse=True)),
            "duplicate requires": lambda d: d.update(requires_stock_manifest_sha256=[STOCK_A, STOCK_A]),
            "no files": lambda d: d.update(files=[]),
            "a path that climbs": lambda d: d["files"][0].update(path="packages/../x"),
            "no manifest": lambda d: d.update(files=[d["files"][1]]),
            "a bad file digest": lambda d: d["files"][0].update(sha256="zz"),
            "a zero size": lambda d: d["files"][0].update(size=0),
            "a boolean size": lambda d: d["files"][0].update(size=True),
            "a size over the limit": lambda d: d["files"][0].update(size=1 << 31),
            "a duplicate path": lambda d: d["files"].append(dict(d["files"][0])),
            "no source digest": lambda d: d.pop("source_zip_sha256"),
            "a zero source size": lambda d: d.update(source_zip_size=0),
        }
        for label, change in cases.items():
            with self.subTest(label):
                self.write(mutate(change))
                self.assertTrue(libs.pack_problems(self.stage), f"{label} was accepted")
        self.write(mutate(lambda d: None))
        self.assertEqual(libs.pack_problems(self.stage), [])

    def test_a_pack_json_that_is_not_json_or_not_an_object_is_refused(self):
        self.json_path.write_text("{")
        self.assertTrue(any("cannot be read as JSON" in p for p in libs.pack_problems(self.stage)))
        self.json_path.write_text("[1]")
        self.assertTrue(any("not a JSON object" in p for p in libs.pack_problems(self.stage)))

    def test_files_with_no_pack_json_are_refused(self):
        self.json_path.unlink()
        self.assertTrue(any("without a pack.json" in p for p in libs.pack_problems(self.stage)))

    def test_a_listed_file_that_is_not_staged_is_named(self):
        (self.stage / libs.PACK_FILES_DIR / PACKAGE).unlink()
        self.assertTrue(any(PACKAGE in p and "not staged as a file" in p for p in libs.pack_problems(self.stage)))

    def test_a_staged_file_with_another_size_or_digest_is_named(self):
        path = self.stage / libs.PACK_FILES_DIR / PACKAGE
        path.write_bytes(PACK_PACKAGE + b"x")
        self.assertTrue(any("bytes staged, pack.json says" in p for p in libs.pack_problems(self.stage)))
        path.write_bytes(b"\x01" + PACK_PACKAGE[1:])
        self.assertTrue(any("sha256 pack.json lists" in p for p in libs.pack_problems(self.stage)))

    def test_a_staged_file_that_is_a_link_is_named(self):
        path = self.stage / libs.PACK_FILES_DIR / PACKAGE
        path.unlink()
        path.symlink_to(self.stage / libs.PACK_FILES_DIR / MANIFEST)
        self.assertTrue(any("not staged as a file" in p for p in libs.pack_problems(self.stage)))

    def test_a_staged_file_pack_json_does_not_list_is_named(self):
        (self.stage / libs.PACK_FILES_DIR / "packages" / "extra").write_bytes(b"x")
        self.assertTrue(any("packages/extra: staged, not listed" in p for p in libs.pack_problems(self.stage)))

    def test_something_else_in_the_pack_directory_is_named(self):
        (self.stage / libs.PACK_DIR / "notes.txt").write_bytes(b"x")
        self.assertTrue(any("notes.txt: not part of a pack" in p for p in libs.pack_problems(self.stage)))

    def test_the_command_line_says_ok_or_exits_one(self):
        command = [sys.executable, "-I", str(REPO / "tools/quest_repack_libs.py"), "pack-problems", "--stage", str(self.stage)]
        ok = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual((ok.returncode, "pack-problems OK" in ok.stdout), (0, True))
        self.json_path.unlink()
        bad = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1)
        self.assertIn("without a pack.json", bad.stderr)
        self.assertNotIn("OK", bad.stdout)


class VerifySignedPackTest(PackFixture):
    """verify-signed over the pack: every staged pack file is in the signed APK, STORED, with the staged bytes; nothing of a
    pack that was not staged is there; the APK is under 4 GiB."""

    def setUp(self):
        super().setUp()
        self.assertEqual(self.stage_pack(), [])
        (self.stage / "lib/arm64-v8a").mkdir(parents=True)
        (self.stage / "lib/arm64-v8a/libovrplatformloader.so").write_bytes(b"shim")
        self.patcher = make_standin(self.tmp / "bin", "ok")
        # The store APK, and the signed one: the loader replaced, libr15 the stand-in patcher's output, the pack's files STORED.
        self.store = make_zip(self.tmp / "store.apk", {"AndroidManifest.xml": b"m", "lib/arm64-v8a/libr15.so": b"game",
                                                       "lib/arm64-v8a/libovrplatformloader.so": b"loader"})
        (self.stage / "lib/arm64-v8a/libr15.so").write_bytes(b"game" + MARKER)
        self.signed_entries: dict[str, tuple[bytes, int]] = {
            "AndroidManifest.xml": (b"m", zipfile.ZIP_DEFLATED),
            "lib/arm64-v8a/libovrplatformloader.so": (b"shim", zipfile.ZIP_STORED),
            "lib/arm64-v8a/libr15.so": (b"game" + MARKER, zipfile.ZIP_DEFLATED),
        }
        for path in sorted(p for p in (self.stage / "assets").rglob("*") if p.is_file()):
            self.signed_entries[path.relative_to(self.stage).as_posix()] = (path.read_bytes(), zipfile.ZIP_STORED)

    def verify(self, signed: pathlib.Path | None = None) -> list[str]:
        if signed is None:
            signed = self.tmp / "signed.apk"
            with zipfile.ZipFile(signed, "w") as z:
                for name, (data, method) in self.signed_entries.items():
                    z.writestr(zipfile.ZipInfo(name), data, compress_type=method)
        return libs.verify_signed(self.store, signed, self.stage, self.patcher, None, self.tmp)

    def test_a_signed_apk_that_holds_every_pack_file_stored_passes(self):
        self.assertEqual(self.verify(), [])

    def test_a_pack_file_missing_from_the_signed_apk_is_named(self):
        del self.signed_entries[f"assets/nevr-pack/files/{PACKAGE}"]
        problems = self.verify()
        self.assertTrue(any(f"assets/nevr-pack/files/{PACKAGE}: staged, missing from the signed APK" in p for p in problems), problems)

    def test_a_pack_file_with_other_bytes_is_named(self):
        self.signed_entries[f"assets/nevr-pack/files/{PACKAGE}"] = (b"other", zipfile.ZIP_STORED)
        self.assertTrue(any("different bytes than the staged file" in p and PACKAGE in p for p in self.verify()))

    def test_a_pack_file_that_is_deflated_is_named(self):
        data, _ = self.signed_entries[f"assets/nevr-pack/files/{PACKAGE}"]
        self.signed_entries[f"assets/nevr-pack/files/{PACKAGE}"] = (data, zipfile.ZIP_DEFLATED)
        self.assertTrue(any("not STORED" in p and PACKAGE in p for p in self.verify()))

    def test_pack_json_is_held_to_the_same_rule(self):
        data, _ = self.signed_entries[libs.PACK_JSON_ENTRY]
        self.signed_entries[libs.PACK_JSON_ENTRY] = (data, zipfile.ZIP_DEFLATED)
        self.assertTrue(any("not STORED" in p and "pack.json" in p for p in self.verify()))

    def test_a_pack_entry_that_was_not_staged_is_named(self):
        self.signed_entries["assets/nevr-pack/files/packages/smuggled"] = (b"x", zipfile.ZIP_STORED)
        self.assertTrue(any("smuggled: in the signed APK, not staged" in p for p in self.verify()))

    def test_a_build_with_no_pack_carries_none(self):
        shutil.rmtree(self.stage / "assets")
        for name in [n for n in self.signed_entries if n.startswith("assets/")]:
            del self.signed_entries[name]
        self.assertEqual([p for p in self.verify() if "nevr-pack" in p], [])
        self.signed_entries[libs.PACK_JSON_ENTRY] = (b"{}", zipfile.ZIP_STORED)
        self.assertTrue(any("pack.json: in the signed APK, not staged" in p for p in self.verify()))

    def test_a_staged_pack_the_runtime_would_refuse_is_named_before_anything_is_signed(self):
        path = self.stage / libs.PACK_FILES_DIR / PACKAGE
        path.write_bytes(b"changed after staging")
        self.assertTrue(any("bytes staged, pack.json says" in p or "sha256 pack.json lists" in p for p in self.verify()))

    def test_a_signed_apk_over_four_gib_is_named(self):
        signed = self.tmp / "signed.apk"
        with zipfile.ZipFile(self.tmp / "inner.apk", "w") as z:
            for name, (data, method) in self.signed_entries.items():
                z.writestr(zipfile.ZipInfo(name), data, compress_type=method)
        archive = (self.tmp / "inner.apk").read_bytes()
        sparse_prefix(signed, 0xFFFFFFFF, archive)
        self.assertFalse(any("4 GiB" in p for p in self.verify(signed)), "an APK of 0xFFFFFFFF bytes is addressable")
        sparse_prefix(signed, 0x100000000, archive)
        self.assertTrue(any("4 GiB" in p for p in self.verify(signed)))


@unittest.skipIf(not shutil.which("just"), "needs just")
class RepackWithAPackTest(RepackFixture):
    """`just android-repack-apk` end to end, with `pack` and `pack_meta` and without."""

    def make_pack(self, entries=None, **meta_changes):
        pack = make_zip(self.tmp / "pack.zip", good_entries() if entries is None else entries)
        meta_path = self.tmp / "pack-meta.json"
        meta_path.write_text(json.dumps(meta(**meta_changes)))
        return pack, meta_path

    def recipe(self, *, pack="", pack_meta="", work="work"):
        workdir = self.tmp / work
        result = subprocess.run(
            ["just", "--justfile", str(JUSTFILE), "--working-directory", str(REPO), "android-repack-apk", str(self.apk),
             str(self.shim), os.path.relpath(workdir, REPO), "", "", "", str(self.patcher), "", "", "", str(pack), str(pack_meta)],
            capture_output=True, text=True, env=self.env, timeout=300)
        self.signed = workdir / "r15_nevr-sentinel_signed.apk"
        return result

    def entries(self, apk: pathlib.Path) -> dict[str, tuple[int, int, int]]:
        with zipfile.ZipFile(apk) as z:
            return {i.filename: (i.compress_type, i.CRC, i.file_size) for i in z.infolist()}

    def test_a_pack_is_carried_stored_with_its_description_and_every_library_survives(self):
        self.make_inputs(DEV_VERSION)
        pack, meta_path = self.make_pack()
        result = self.recipe(pack=pack, pack_meta=meta_path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify-signed OK", result.stdout)
        self.assertIn("stage-pack OK", result.stdout)
        self.assertIn("pack-problems OK", result.stdout)
        with zipfile.ZipFile(self.signed) as z:
            for name, data in ((f"assets/nevr-pack/files/{MANIFEST}", PACK_MANIFEST), (f"assets/nevr-pack/files/{PACKAGE}", PACK_PACKAGE)):
                self.assertEqual(z.read(name), data)
                self.assertEqual(z.getinfo(name).compress_type, zipfile.ZIP_STORED, name)
            self.assertEqual(z.getinfo(libs.PACK_JSON_ENTRY).compress_type, zipfile.ZIP_STORED)
            document = json.loads(z.read(libs.PACK_JSON_ENTRY))
            self.assertEqual([f["path"] for f in document["files"]], [MANIFEST, PACKAGE])
            self.assertEqual(document["source_zip_sha256"], sha(pack.read_bytes()))
            self.assertEqual(z.read("assets/data.bin"), bytes(range(256)) * 20)  # the store's own assets are untouched
        verify = subprocess.run(["apksigner", "verify", str(self.signed)], capture_output=True, text=True)
        self.assertEqual(verify.returncode, 0, verify.stdout + verify.stderr)

    def test_without_a_pack_the_apk_carries_nothing_of_it_and_a_pack_changes_only_its_own_entries(self):
        self.make_inputs(DEV_VERSION)
        bare = self.recipe(work="bare")
        self.assertEqual(bare.returncode, 0, bare.stdout + bare.stderr)
        without = self.entries(self.signed)
        self.assertEqual([n for n in without if n.startswith("assets/nevr-pack")], [])
        self.assertNotIn("stage-pack", bare.stdout)
        self.assertNotIn("pack-problems", bare.stdout)
        # The entries a run with no pack writes: the store's (less the old signature files) and the renamed original loader.
        with zipfile.ZipFile(self.apk) as store:
            store_names = {n for n in store.namelist() if not n.startswith("META-INF/")}
        self.assertEqual(set(without) - {n for n in without if n.startswith("META-INF/")},
                         store_names | {"lib/arm64-v8a/libovrplatformloader_orig.so"})
        pack, meta_path = self.make_pack()
        packed = self.recipe(pack=pack, pack_meta=meta_path, work="packed")
        self.assertEqual(packed.returncode, 0, packed.stdout + packed.stderr)
        with_pack = self.entries(self.signed)
        added = {n for n in with_pack if n not in without}
        self.assertEqual(added, {libs.PACK_JSON_ENTRY, f"assets/nevr-pack/files/{MANIFEST}", f"assets/nevr-pack/files/{PACKAGE}"})
        for name, entry in without.items():
            if not name.startswith("META-INF/"):
                self.assertEqual(with_pack[name], entry, f"{name} differs between the run with a pack and the run without")

    def test_a_pack_the_tool_refuses_stops_the_recipe_with_no_signed_apk(self):
        self.make_inputs(DEV_VERSION)
        pack, meta_path = self.make_pack({PREFIX + PACKAGE: PACK_PACKAGE})  # no manifest
        result = self.recipe(pack=pack, pack_meta=meta_path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"the zip has no {PREFIX}{MANIFEST}", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_pack_with_no_description_file_is_refused_and_so_is_a_description_with_no_pack(self):
        self.make_inputs(DEV_VERSION)
        pack, meta_path = self.make_pack()
        result = self.recipe(pack=pack)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pack_meta (its description file) is not a file", result.stderr)
        self.assertFalse(self.signed.exists())
        result = self.recipe(pack_meta=meta_path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pack_meta describes a pack, and no pack is given", result.stderr)
        self.assertFalse(self.signed.exists())
        result = self.recipe(pack=self.tmp / "missing.zip", pack_meta=meta_path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pack not found", result.stderr)

    def test_a_store_apk_that_has_a_pack_entry_already_is_refused(self):
        self.make_inputs(DEV_VERSION)
        with zipfile.ZipFile(self.apk, "a") as z:
            z.writestr(libs.PACK_JSON_ENTRY, b"{}")
        pack, meta_path = self.make_pack()
        result = self.recipe(pack=pack, pack_meta=meta_path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already has the entry assets/nevr-pack/pack.json", result.stderr)
        self.assertFalse(self.signed.exists())


if __name__ == "__main__":
    unittest.main()
