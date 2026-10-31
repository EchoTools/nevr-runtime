"""tools/quest_apk_rename_package.py: the reference APK's package rename (afk to r15), with every count asserted (#1737).

The fixture APK is built here: a manifest whose UTF-16 string pool holds the source package once, two fake libraries
holding it 5 and 2 times in ASCII, an entry that holds the target string (as the real reference APK's classes.dex does)
and entries that must not change. The real-file case runs when NEVR_REFERENCE_APK names the reference APK
(`Echo_Combat_port_4987566.apk`); its libr15.so must come out with the sha256 that is kLibR15ReferenceSha256[0] in
src/quest/patch/libr15_patch_table.h. The launcher label cases build a manifest whose `<application>` label is one raw string
(`axml_fixtures.build_label_manifest`) and check that `--label` sets it after the rename, in a renamed and an already-renamed
input, and changes no other entry; the real-file case does the same on the reference APK.
"""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "tests"))
import axml_fixtures as fx  # noqa: E402
import quest_apk_label as ql  # noqa: E402
import quest_apk_rename_package as rp  # noqa: E402

TOOL = REPO / "tools" / "quest_apk_rename_package.py"
JUSTFILE = REPO / "justfile"
REFERENCE_LABEL, PROJECT_LABEL = "Echo Combat (port)", "Echo VR"
OLD, NEW = rp.DEFAULT_FROM, rp.DEFAULT_TO
LIBR15, ASSETPATCH = "lib/arm64-v8a/libr15.so", "lib/arm64-v8a/libassetpatch.so"
REAL_LIBR15_SHA_PREFIX = "936aa6a3"  # kLibR15ReferenceSha256[0], src/quest/patch/libr15_patch_table.h
REAL_APK = os.environ.get("NEVR_REFERENCE_APK")


def wide(text: str) -> bytes:
    return text.encode("utf-16-le")


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def afk_manifest(utf8: bool = False, extra_wide: int = 0) -> bytes:
    """A manifest whose pool holds the source package: the fixture's manifest with its target string swapped for the source."""
    data = fx.build_manifest(utf8=utf8)
    ascii_form = NEW.encode("ascii")
    swapped = data.replace(ascii_form if utf8 else wide(NEW), OLD.encode("ascii") if utf8 else wide(OLD))
    assert swapped != data
    return swapped


def lib_with(old: str, count: int, salt: bytes) -> bytes:
    body = salt
    for i in range(count):
        body += b"\0/data/data/" + old.encode() + b"/files\0pad" + bytes([i])
    return body + b"\0tail" * 3


def entries(ascii_in_libr15: int = 5, ascii_in_assetpatch: int = 2, manifest: bytes = None, extra: dict = None) -> list:
    """(name, data, compress_type) for a fixture reference APK, in a fixed order."""
    out = [
        ("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\n", zipfile.ZIP_DEFLATED),
        ("META-INF/ECHOVRTE.SF", b"Signature-Version: 1.0\n", zipfile.ZIP_DEFLATED),
        ("META-INF/ECHOVRTE.RSA", b"\x30\x82rsa", zipfile.ZIP_STORED),
        ("AndroidManifest.xml", manifest if manifest is not None else afk_manifest(), zipfile.ZIP_DEFLATED),
        ("classes.dex", b"dex\n035\0" + NEW.encode() + b"\0" + bytes(range(256)) * 4, zipfile.ZIP_DEFLATED),
        ("resources.arsc", b"arsc" + wide("Echo Combat (port)") + wide(NEW), zipfile.ZIP_STORED),
        (LIBR15, lib_with(OLD, ascii_in_libr15, b"\x7fELF-r15-") + NEW.encode(), zipfile.ZIP_DEFLATED),
        ("lib/arm64-v8a/libvrapi.so", b"\x7fELF-vrapi" + NEW.encode() + b"\0" + NEW.encode(), zipfile.ZIP_DEFLATED),
        (ASSETPATCH, lib_with(OLD, ascii_in_assetpatch, b"\x7fELF-ap-"), zipfile.ZIP_DEFLATED),
        ("assets/afk_default_on.txt", b"ecplayo.on\ndronekill2.on\n", zipfile.ZIP_STORED),
    ]
    for name, data in (extra or {}).items():
        out.append((name, data, zipfile.ZIP_DEFLATED))
    return out


def write_apk(path: pathlib.Path, items: list) -> None:
    with zipfile.ZipFile(path, "w") as z:
        for name, data, method in items:
            info = zipfile.ZipInfo(name, (2026, 10, 30, 12, 0, 0))
            info.compress_type = method
            z.writestr(info, data)


def read_apk(path: pathlib.Path) -> dict:
    with zipfile.ZipFile(path) as z:
        return {i.filename: (z.read(i), i.compress_type, i.date_time) for i in z.infolist()}


class FixtureTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = pathlib.Path(self._tmp.name)
        self.src = self.dir / "in.apk"
        self.out = self.dir / "out.apk"

    def make(self, **kwargs) -> list:
        items = entries(**kwargs)
        write_apk(self.src, items)
        return items

    def refused(self, **kwargs) -> str:
        with self.assertRaises(rp.RenameError) as ctx:
            rp.rename_apk(self.src, self.out, **kwargs)
        self.assertFalse(self.out.exists(), "a refusal wrote the output")
        self.assertFalse(self.out.with_name(self.out.name + ".part").exists(), "a refusal left its partial file")
        return str(ctx.exception)


class RenameTest(FixtureTest):
    def test_renames_exactly_the_three_entries_with_their_counts(self):
        items = self.make()
        record = rp.rename_apk(self.src, self.out)
        before, after = {n: d for n, d, _ in items}, read_apk(self.out)
        self.assertEqual(record["event"], "package_renamed")
        self.assertEqual(record["counts"], {"AndroidManifest.xml": [0, 1], LIBR15: [5, 0], ASSETPATCH: [2, 0]})
        self.assertEqual(set(record["changed"]), {"AndroidManifest.xml", LIBR15, ASSETPATCH})
        # manifest: the one UTF-16LE string, same length
        manifest = after["AndroidManifest.xml"][0]
        self.assertEqual(len(manifest), len(before["AndroidManifest.xml"]))
        self.assertEqual(manifest.count(wide(OLD)), 0)
        self.assertEqual(manifest, before["AndroidManifest.xml"].replace(wide(OLD), wide(NEW)))
        self.assertIn(NEW, rp._pool_strings(manifest))
        self.assertNotIn(OLD, rp._pool_strings(manifest))
        # libraries: every ASCII occurrence, nothing else moved
        for name, n in ((LIBR15, 5), (ASSETPATCH, 2)):
            data = after[name][0]
            self.assertEqual(len(data), len(before[name]), name)
            self.assertEqual(data.count(OLD.encode()), 0, name)
            self.assertEqual(data, before[name].replace(OLD.encode(), NEW.encode()), name)
            self.assertGreaterEqual(data.count(NEW.encode()), n, name)
        for name, entry in record["changed"].items():
            self.assertEqual(entry["sha256_in"], sha(before[name]))
            self.assertEqual(entry["sha256_out"], sha(after[name][0]))
            self.assertNotEqual(entry["sha256_in"], entry["sha256_out"])

    def test_strips_meta_inf_and_keeps_every_other_entry_byte_identical(self):
        items = self.make()
        record = rp.rename_apk(self.src, self.out)
        after = read_apk(self.out)
        self.assertEqual(record["dropped_meta_inf"], 3)
        self.assertFalse([n for n in after if n.startswith("META-INF")])
        kept = [(n, d, m) for n, d, m in items if not n.startswith("META-INF")]
        self.assertEqual(list(after), [n for n, _, _ in kept], "entry order")
        for name, data, method in kept:
            if name in rp.EXPECTED:
                self.assertEqual(after[name][1], method, f"{name}: compression")
                continue
            self.assertEqual(after[name][0], data, f"{name}: bytes")
            self.assertEqual(after[name][1], method, f"{name}: compression")
            self.assertEqual(after[name][2], (2026, 10, 30, 12, 0, 0), f"{name}: timestamp")
        # the target string already in classes.dex, resources.arsc and libvrapi.so is not touched, and nothing is renamed there
        self.assertEqual(after["classes.dex"][0].count(NEW.encode()), 1)
        self.assertEqual(after["resources.arsc"][0].count(wide(NEW)), 1)

    def test_record_carries_the_output_sha_and_names(self):
        self.make()
        record = rp.rename_apk(self.src, self.out)
        self.assertEqual(record["tag"], "NEVR.QUEST.RENAME")
        self.assertEqual((record["apk"], record["out"], record["from"], record["to"]), ("in.apk", "out.apk", OLD, NEW))
        self.assertEqual(record["sha256_out"], sha(self.out.read_bytes()))
        self.assertEqual(record["entries_out"], 7)


class RefusalTest(FixtureTest):
    def test_refuses_a_libr15_count_that_differs(self):
        for n in (4, 6, 0):
            with self.subTest(n=n):
                self.make(ascii_in_libr15=n)
                # n == 0 is a partial state (the others still hold the source string), not "already renamed"
                self.assertIn(LIBR15, self.refused())

    def test_refuses_an_assetpatch_count_that_differs(self):
        self.make(ascii_in_assetpatch=3)
        self.assertIn("expected (2, 0)", self.refused())

    def test_refuses_a_second_manifest_string(self):
        data = afk_manifest()
        # a second UTF-16LE copy of the package appended inside the chunk size is not a pool string, but it is a count of 2
        data = data + wide(OLD) + b"\0\0"
        data = data[:4] + (len(data)).to_bytes(4, "little") + data[8:]
        self.make(manifest=data)
        self.assertIn("AndroidManifest.xml", self.refused())

    def test_refuses_a_utf8_manifest_pool(self):
        self.make(manifest=afk_manifest(utf8=True))
        self.assertIn("AndroidManifest.xml", self.refused())

    def test_refuses_a_source_string_in_an_entry_the_rename_does_not_name(self):
        for name, data, form in (("classes.dex", b"dex\0" + OLD.encode(), "ascii=1 utf16le=0"),
                                 ("assets/other.bin", wide(OLD), "ascii=0 utf16le=1"),
                                 ("lib/arm64-v8a/libvrapi.so", OLD.encode() * 2, "ascii=2 utf16le=0")):
            with self.subTest(entry=name):
                items = [(n, d, m) for n, d, m in entries() if n != name]
                items.append((name, data, zipfile.ZIP_DEFLATED))
                write_apk(self.src, items)
                message = self.refused()
                self.assertIn(name, message)
                self.assertIn(form, message)
                self.assertIn("does not name", message)

    def test_reports_every_unnamed_entry_not_only_the_first(self):
        self.make(extra={"a.bin": OLD.encode(), "b.bin": wide(OLD)})
        message = self.refused()
        self.assertIn("a.bin", message)
        self.assertIn("b.bin", message)

    def test_refuses_the_source_string_in_neither_form_when_the_target_is_not_there_either(self):
        items = [(n, d.replace(OLD.encode(), b"com.other.packageXXX") if n != "AndroidManifest.xml" else d.replace(wide(OLD), wide("com.other.packageXXX")), m)
                 for n, d, m in entries() if n not in ("classes.dex", "resources.arsc", "lib/arm64-v8a/libvrapi.so")]
        write_apk(self.src, items)
        self.assertIn("neither the source package string nor the target", self.refused())

    def test_refuses_a_missing_named_entry(self):
        write_apk(self.src, [(n, d, m) for n, d, m in entries() if n != ASSETPATCH])
        self.assertIn(ASSETPATCH, self.refused())

    def test_refuses_names_of_different_length_or_equal_or_non_ascii(self):
        self.make()
        for old, new in ((OLD, "com.readyatdawn.r150"), (OLD, OLD), ("com.x", "com.x.y"), (OLD, "com.readyatdawn.ä15")):
            with self.subTest(new=new):
                with self.assertRaises(rp.RenameError):
                    rp.rename_apk(self.src, self.out, old, new)
                self.assertFalse(self.out.exists())

    def test_refuses_to_write_over_its_input(self):
        self.make()
        before = self.src.read_bytes()
        with self.assertRaises(rp.RenameError):
            rp.rename_apk(self.src, self.src)
        self.assertEqual(self.src.read_bytes(), before)


class IdempotenceTest(FixtureTest):
    def test_an_already_renamed_input_is_reported_and_copied(self):
        self.make()
        rp.rename_apk(self.src, self.out)
        again = self.dir / "again.apk"
        record = rp.rename_apk(self.out, again)
        self.assertEqual(record["event"], "package_already_renamed")
        self.assertEqual(record["changed"], {})
        self.assertEqual(read_apk(self.out), read_apk(again))

    def test_an_already_renamed_input_keeps_meta_inf_dropped(self):
        self.make()
        rp.rename_apk(self.src, self.out)
        # re-sign simulation: put META-INF back, run again, it goes again
        signed = self.dir / "signed.apk"
        items = [(n, d, m) for n, (d, m, _) in read_apk(self.out).items()] + [("META-INF/X.SF", b"sf", zipfile.ZIP_DEFLATED)]
        write_apk(signed, items)
        again = self.dir / "again.apk"
        record = rp.rename_apk(signed, again)
        self.assertEqual(record["dropped_meta_inf"], 1)
        self.assertEqual(read_apk(again), read_apk(self.out))


class CliTest(FixtureTest):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)], capture_output=True, text=True)

    def test_rename_logs_one_json_line(self):
        self.make()
        done = self.run_tool("rename", "--apk", self.src, "--out", self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = done.stdout.splitlines()
        self.assertEqual(len(lines), 1)
        record = json.loads(lines[0])
        self.assertEqual((record["tag"], record["event"]), ("NEVR.QUEST.RENAME", "package_renamed"))
        self.assertTrue(self.out.exists())

    def test_a_refusal_is_one_json_line_on_stderr_and_exit_1(self):
        self.make(ascii_in_libr15=4)
        done = self.run_tool("rename", "--apk", self.src, "--out", self.out)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        record = json.loads(done.stderr)
        self.assertEqual(record["event"], "package_rename_refused")
        self.assertIn(LIBR15, record["reason"])
        self.assertFalse(self.out.exists())

    def test_a_missing_input_is_a_refusal_not_a_traceback(self):
        done = self.run_tool("rename", "--apk", self.dir / "nope.apk", "--out", self.out)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(json.loads(done.stderr)["event"], "package_rename_refused")

    def test_from_and_to_default_to_afk_and_r15(self):
        self.make()
        explicit = self.dir / "explicit.apk"
        self.assertEqual(self.run_tool("rename", "--apk", self.src, "--out", self.out).returncode, 0)
        self.assertEqual(self.run_tool("rename", "--apk", self.src, "--out", explicit, "--from", OLD, "--to", NEW).returncode, 0)
        self.assertEqual(read_apk(self.out), read_apk(explicit))


class LabelTest(FixtureTest):
    """`label=` sets the manifest's launcher label after the package rename, and nothing else moves."""

    def make_labelled(self, **kwargs) -> list:
        items = entries(manifest=fx.build_label_manifest(OLD, REFERENCE_LABEL, **kwargs))
        write_apk(self.src, items)
        return items

    def test_the_fixture_manifest_reads_as_the_reference_build_does(self):
        manifest = fx.build_label_manifest(OLD, REFERENCE_LABEL)
        self.assertEqual(ql.label_of(manifest), REFERENCE_LABEL)
        self.assertEqual(manifest.count(wide(OLD)), 1)

    def test_without_a_label_the_manifest_label_and_the_record_are_as_they_were(self):
        self.make_labelled()
        record = rp.rename_apk(self.src, self.out)
        self.assertEqual(ql.label_of(read_apk(self.out)["AndroidManifest.xml"][0]), REFERENCE_LABEL)
        self.assertNotIn("label_from", record)
        self.assertNotIn("label_to", record)

    def test_a_renamed_input_gets_the_package_and_the_label_and_no_other_entry_changes(self):
        items = self.make_labelled()
        plain = self.dir / "plain.apk"
        rp.rename_apk(self.src, plain)
        record = rp.rename_apk(self.src, self.out, label=PROJECT_LABEL)
        self.assertEqual(record["event"], "package_renamed")
        self.assertEqual((record["label_from"], record["label_to"]), (REFERENCE_LABEL, PROJECT_LABEL))
        after, without = read_apk(self.out), read_apk(plain)
        manifest, source = after["AndroidManifest.xml"][0], dict((n, d) for n, d, _ in items)["AndroidManifest.xml"]
        self.assertEqual(ql.label_of(manifest), PROJECT_LABEL)
        self.assertIn(NEW, rp._pool_strings(manifest))
        self.assertNotIn(OLD, rp._pool_strings(manifest))
        self.assertEqual(len(manifest), len(source))
        self.assertEqual(manifest, ql.set_label(without["AndroidManifest.xml"][0], PROJECT_LABEL))
        self.assertEqual(list(after), list(without), "entry order")
        for name in without:
            if name != "AndroidManifest.xml":
                self.assertEqual(after[name], without[name], f"{name}: the label changed another entry")
        self.assertEqual(set(record["changed"]), {"AndroidManifest.xml", LIBR15, ASSETPATCH})
        self.assertEqual(record["changed"]["AndroidManifest.xml"]["sha256_in"], sha(source))
        self.assertEqual(record["changed"]["AndroidManifest.xml"]["sha256_out"], sha(manifest))
        self.assertEqual(record["sha256_out"], sha(self.out.read_bytes()))

    def test_an_already_renamed_input_still_gets_the_label(self):
        self.make_labelled()
        renamed = self.dir / "renamed.apk"
        rp.rename_apk(self.src, renamed)
        record = rp.rename_apk(renamed, self.out, label=PROJECT_LABEL)
        self.assertEqual(record["event"], "package_already_renamed")
        self.assertEqual((record["label_from"], record["label_to"]), (REFERENCE_LABEL, PROJECT_LABEL))
        before, after = read_apk(renamed), read_apk(self.out)
        manifest = after["AndroidManifest.xml"][0]
        self.assertEqual(ql.label_of(manifest), PROJECT_LABEL)
        self.assertIn(NEW, rp._pool_strings(manifest))
        self.assertEqual(manifest, ql.set_label(before["AndroidManifest.xml"][0], PROJECT_LABEL))
        self.assertEqual(list(after), list(before), "entry order")
        for name in before:
            if name != "AndroidManifest.xml":
                self.assertEqual(after[name], before[name], f"{name}: the label changed another entry")
        self.assertEqual(set(record["changed"]), {"AndroidManifest.xml"})
        self.assertEqual(record["changed"]["AndroidManifest.xml"]["sha256_in"], sha(before["AndroidManifest.xml"][0]))
        self.assertEqual(record["changed"]["AndroidManifest.xml"]["sha256_out"], sha(manifest))

    def test_a_second_run_with_the_same_label_writes_the_same_entries(self):
        self.make_labelled()
        rp.rename_apk(self.src, self.out, label=PROJECT_LABEL)
        again = self.dir / "again.apk"
        record = rp.rename_apk(self.out, again, label=PROJECT_LABEL)
        self.assertEqual(record["event"], "package_already_renamed")
        self.assertEqual((record["label_from"], record["label_to"]), (PROJECT_LABEL, PROJECT_LABEL))
        self.assertEqual(record["changed"], {})
        self.assertEqual(read_apk(self.out), read_apk(again))

    def test_a_label_longer_than_the_old_one_is_a_refusal_and_writes_nothing(self):
        self.make_labelled()
        message = self.refused(label=REFERENCE_LABEL + " and more")
        self.assertIn("AndroidManifest.xml", message)
        self.assertIn("only edits in place", message)

    def test_a_label_longer_than_the_old_one_on_an_already_renamed_input_writes_nothing(self):
        self.make_labelled()
        renamed = self.dir / "renamed.apk"
        rp.rename_apk(self.src, renamed)
        self.src = renamed
        self.assertIn("only edits in place", self.refused(label=REFERENCE_LABEL + " and more"))

    def test_an_empty_or_non_ascii_label_is_a_refusal_and_writes_nothing(self):
        self.make_labelled()
        for label in ("", "Echo V\u00ff"):
            with self.subTest(label=label):
                self.assertIn("non-empty ASCII", self.refused(label=label))

    def test_a_manifest_without_an_application_label_is_a_refusal_and_writes_nothing(self):
        self.make()  # the fixture manifest has no <application>
        self.assertIn("android:label", self.refused(label=PROJECT_LABEL))

    def test_a_label_referenced_by_another_attribute_is_a_refusal_and_writes_nothing(self):
        self.make_labelled(share_label=True)
        self.assertIn("referenced by", self.refused(label=PROJECT_LABEL))


class LabelCliTest(FixtureTest):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)], capture_output=True, text=True)

    def test_label_flag_sets_the_label_and_the_record_names_both(self):
        write_apk(self.src, entries(manifest=fx.build_label_manifest(OLD, REFERENCE_LABEL)))
        done = self.run_tool("rename", "--apk", self.src, "--out", self.out, "--label", PROJECT_LABEL)
        self.assertEqual(done.returncode, 0, done.stderr)
        record = json.loads(done.stdout)
        self.assertEqual((record["event"], record["label_from"], record["label_to"]), ("package_renamed", REFERENCE_LABEL, PROJECT_LABEL))
        self.assertEqual(ql.label_of(read_apk(self.out)["AndroidManifest.xml"][0]), PROJECT_LABEL)

    def test_no_label_flag_leaves_the_label(self):
        write_apk(self.src, entries(manifest=fx.build_label_manifest(OLD, REFERENCE_LABEL)))
        done = self.run_tool("rename", "--apk", self.src, "--out", self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("label_to", json.loads(done.stdout))
        self.assertEqual(ql.label_of(read_apk(self.out)["AndroidManifest.xml"][0]), REFERENCE_LABEL)

    def test_a_refused_label_is_one_json_line_on_stderr_and_exit_1(self):
        write_apk(self.src, entries(manifest=fx.build_label_manifest(OLD, REFERENCE_LABEL)))
        done = self.run_tool("rename", "--apk", self.src, "--out", self.out, "--label", REFERENCE_LABEL + " and more")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        record = json.loads(done.stderr)
        self.assertEqual(record["event"], "package_rename_refused")
        self.assertIn("only edits in place", record["reason"])
        self.assertFalse(self.out.exists())
        self.assertFalse(self.out.with_name(self.out.name + ".part").exists())


class JustfileRecipeTest(unittest.TestCase):
    def recipe(self) -> tuple:
        text = JUSTFILE.read_text(encoding="utf-8")
        found = re.search(r"^(quest-reference-base[^\n]*:)\n((?:[ \t]+[^\n]*\n)+)", text, re.MULTILINE)
        self.assertIsNotNone(found, "the justfile has no quest-reference-base recipe")
        return found.group(1), found.group(2)

    def test_quest_reference_base_passes_the_label_to_the_rename_tool(self):
        header, body = self.recipe()
        self.assertIn('label="Echo VR"', header)
        self.assertIn("quest_apk_rename_package.py rename", body)
        self.assertIn('--label "{{ label }}"', body)


@unittest.skipUnless(REAL_APK and pathlib.Path(REAL_APK).is_file(), "NEVR_REFERENCE_APK does not name the reference APK")
class RealReferenceApkTest(unittest.TestCase):
    def test_real_apk_renames_with_the_lane_counts_and_the_pinned_libr15_sha(self):
        src = pathlib.Path(REAL_APK)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "ref-r15.apk"
            record = rp.rename_apk(src, out)
            print("real-file record:", json.dumps(record, sort_keys=True))
            self.assertEqual(record["event"], "package_renamed")
            self.assertEqual(record["counts"], {"AndroidManifest.xml": [0, 1], LIBR15: [5, 0], ASSETPATCH: [2, 0]})
            with zipfile.ZipFile(src) as zin, zipfile.ZipFile(out) as zout:
                self.assertEqual(zout.testzip(), None)
                libr15 = zout.read(LIBR15)
                self.assertTrue(sha(libr15).startswith(REAL_LIBR15_SHA_PREFIX), sha(libr15))
                self.assertEqual(sha(libr15), record["changed"][LIBR15]["sha256_out"])
                self.assertEqual(zout.read(LIBR15).count(OLD.encode()), 0)
                self.assertEqual(zout.read(ASSETPATCH).count(OLD.encode()), 0)
                self.assertEqual(zout.read("AndroidManifest.xml").count(wide(OLD)), 0)
                self.assertIn(NEW, rp._pool_strings(zout.read("AndroidManifest.xml")))
                source = [i for i in zin.infolist() if not i.filename.startswith("META-INF/")]
                self.assertEqual([i.filename for i in zout.infolist()], [i.filename for i in source])
                for info in source:
                    out_info = zout.getinfo(info.filename)
                    self.assertEqual(out_info.compress_type, info.compress_type, info.filename)
                    if info.filename in rp.EXPECTED:
                        continue
                    self.assertEqual(sha(zout.read(info.filename)), sha(zin.read(info)), info.filename)
            # a second run over the output reports and copies
            again = pathlib.Path(tmp) / "again.apk"
            second = rp.rename_apk(out, again)
            self.assertEqual(second["event"], "package_already_renamed")

    def test_real_apk_label_reads_echo_vr_and_nothing_else_moves(self):
        src = pathlib.Path(REAL_APK)
        with tempfile.TemporaryDirectory() as tmp:
            plain, labelled = pathlib.Path(tmp) / "plain.apk", pathlib.Path(tmp) / "labelled.apk"
            rp.rename_apk(src, plain)
            record = rp.rename_apk(src, labelled, label=PROJECT_LABEL)
            print("real-file label record:", json.dumps(record, sort_keys=True))
            self.assertEqual((record["label_from"], record["label_to"]), (REFERENCE_LABEL, PROJECT_LABEL))
            with zipfile.ZipFile(plain) as zplain, zipfile.ZipFile(labelled) as zlabelled:
                self.assertEqual(zlabelled.testzip(), None)
                self.assertEqual([i.filename for i in zlabelled.infolist()], [i.filename for i in zplain.infolist()])
                manifest = zlabelled.read(rp.MANIFEST)
                self.assertEqual(ql.label_of(manifest), PROJECT_LABEL)
                self.assertEqual(manifest, ql.set_label(zplain.read(rp.MANIFEST), PROJECT_LABEL))
                self.assertIn(NEW, rp._pool_strings(manifest))
                self.assertNotIn(OLD, rp._pool_strings(manifest))
                for info in zplain.infolist():
                    if info.filename != rp.MANIFEST:
                        self.assertEqual(sha(zlabelled.read(info.filename)), sha(zplain.read(info)), info.filename)


if __name__ == "__main__":
    unittest.main()
