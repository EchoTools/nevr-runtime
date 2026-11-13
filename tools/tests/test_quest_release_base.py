"""#1994: a release Quest APK is repacked from the pinned reference build and keeps every entry of that base.

v6.16.0 to v6.16.3 were repacked from the store APK and lost the reference build's combat content without any check
failing. tools/quest_release_base.py refuses an input that is not the pinned reference APK, and a release APK that lacks
an entry of its base.
"""

import hashlib
import io
import json
import pathlib
import shutil
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import axml_fixtures as fx  # noqa: E402
import quest_apk_label as ql  # noqa: E402
import quest_release_base as qb  # noqa: E402
import quest_runtime_pin as pin  # noqa: E402
import test_quest_apk_rename_package as rename_fixture  # noqa: E402

REFERENCE = ("AndroidManifest.xml", "assets/afk_default_on.txt", "lib/arm64-v8a/libassetpatch.so",
             "lib/arm64-v8a/libr15.so", "lib/arm64-v8a/lib0123456789abcdef.so", "META-INF/CERT.SF")


def write_zip(path, names):
    with zipfile.ZipFile(path, "w") as z:
        for n in names:
            z.writestr(n, f"bytes of {n}")
    return path


def copy_rename(src, dst, label=None):
    shutil.copyfile(src, dst)


class Prepare(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.ref = write_zip(self.tmp / "reference.apk", REFERENCE)
        self.sha = hashlib.sha256(self.ref.read_bytes()).hexdigest()

    def test_the_pinned_reference_apk_is_renamed_into_the_base(self):
        calls = []

        def rename(src, dst, label=None):
            calls.append((src, dst, label))
            copy_rename(src, dst)

        record = qb.prepare(self.ref, self.tmp / "out" / "base.apk", expected_sha256=self.sha, rename=rename)
        self.assertEqual(calls, [(self.ref, self.tmp / "out" / "base.apk", "Echo VR")])
        self.assertEqual(record["event"], "base_prepared")
        self.assertEqual(record["sha256"], self.sha)

    def test_any_other_apk_is_refused_before_the_rename(self):
        store = write_zip(self.tmp / "store.apk", ("AndroidManifest.xml", "lib/arm64-v8a/libr15.so"))
        calls = []
        with self.assertRaises(qb.Refused) as caught:
            qb.prepare(store, self.tmp / "base.apk", expected_sha256=self.sha, rename=lambda *a, **k: calls.append(a))
        self.assertEqual(caught.exception.event, "not_the_reference_apk")
        self.assertEqual(calls, [])
        self.assertFalse((self.tmp / "base.apk").exists())

    def test_a_missing_input_is_refused(self):
        with self.assertRaises(qb.Refused) as caught:
            qb.prepare(self.tmp / "nope.apk", self.tmp / "base.apk", expected_sha256=self.sha, rename=copy_rename)
        self.assertEqual(caught.exception.event, "reference_apk_missing")

    def test_a_base_without_the_reference_entries_is_refused(self):
        def rename_dropping(src, dst, label=None):
            write_zip(dst, ("AndroidManifest.xml", "lib/arm64-v8a/libr15.so"))

        with self.assertRaises(qb.Refused) as caught:
            qb.prepare(self.ref, self.tmp / "base.apk", expected_sha256=self.sha, rename=rename_dropping)
        self.assertEqual(caught.exception.event, "base_not_reference")
        self.assertEqual(caught.exception.fields["missing"],
                         ["assets/afk_default_on.txt", "lib/arm64-v8a/libassetpatch.so"])

    def test_the_real_rename_makes_an_r15_base_labelled_echo_vr(self):
        ref = self.tmp / "afk-reference.apk"
        rename_fixture.write_apk(ref, rename_fixture.entries(
            manifest=fx.build_label_manifest(rename_fixture.OLD, rename_fixture.REFERENCE_LABEL)))
        sha = hashlib.sha256(ref.read_bytes()).hexdigest()
        out = self.tmp / "out" / "base.apk"
        record = qb.prepare(ref, out, expected_sha256=sha)
        self.assertEqual(record["event"], "base_prepared")
        base = rename_fixture.read_apk(out)
        self.assertEqual(ql.label_of(base["AndroidManifest.xml"][0]), "Echo VR")
        self.assertNotIn(rename_fixture.OLD.encode(), base[rename_fixture.LIBR15][0])
        self.assertIn("assets/afk_default_on.txt", base)

    def test_a_refused_base_is_not_left_behind(self):
        def rename_dropping(src, dst, label=None):
            write_zip(dst, ("AndroidManifest.xml",))

        out = self.tmp / "base.apk"
        with self.assertRaises(qb.Refused):
            qb.prepare(self.ref, out, expected_sha256=self.sha, rename=rename_dropping)
        self.assertFalse(out.exists())

    def test_the_pin_is_the_reference_build_v6_12_0_was_made_from(self):
        self.assertEqual(qb.REFERENCE_APK_SHA256, "d0b51a014647643a30469ed318968b17374f39286b7d9ded5723f39299eedb48")


class CheckRelease(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.base = write_zip(self.tmp / "base.apk", REFERENCE)

    def test_a_release_that_keeps_every_base_entry_passes(self):
        release = write_zip(self.tmp / "release.apk",
                            [n for n in REFERENCE if not n.startswith("META-INF/")] +
                            ["lib/arm64-v8a/libovrplatformloader_orig.so", "META-INF/NEVR.SF"])
        record = qb.check_release(release, self.base)
        self.assertEqual(record["event"], "release_keeps_base")
        self.assertEqual(record["base_entries"], 5)

    def test_a_release_that_lacks_a_base_entry_is_refused_naming_it(self):
        release = write_zip(self.tmp / "release.apk",
                            [n for n in REFERENCE if n not in ("lib/arm64-v8a/lib0123456789abcdef.so", "META-INF/CERT.SF")])
        with self.assertRaises(qb.Refused) as caught:
            qb.check_release(release, self.base)
        self.assertEqual(caught.exception.event, "release_lacks_base_entries")
        self.assertEqual(caught.exception.fields["missing"], ["lib/arm64-v8a/lib0123456789abcdef.so"])

    def test_a_store_based_release_is_refused(self):
        store = write_zip(self.tmp / "store.apk", ("AndroidManifest.xml", "lib/arm64-v8a/libr15.so"))
        release = write_zip(self.tmp / "release.apk", ("AndroidManifest.xml", "lib/arm64-v8a/libr15.so",
                                                      "lib/arm64-v8a/libovrplatformloader_orig.so"))
        with self.assertRaises(qb.Refused) as caught:
            qb.check_release(release, store)
        self.assertEqual(caught.exception.event, "base_not_reference")

    def test_the_base_itself_is_not_a_release(self):
        with self.assertRaises(qb.Refused) as caught:
            qb.check_release(self.base, self.base)
        self.assertEqual(caught.exception.event, "release_is_the_base")

    def test_meta_inf_of_the_base_is_not_required(self):
        release = write_zip(self.tmp / "release.apk", [n for n in REFERENCE if not n.startswith("META-INF/")])
        self.assertEqual(qb.check_release(release, self.base)["event"], "release_keeps_base")


class CheckArena(unittest.TestCase):
    """#2298: the arena APK is the store APK with the release's loader and no combat."""

    STORE = ("AndroidManifest.xml", "lib/arm64-v8a/libr15.so", "lib/arm64-v8a/libovrplatformloader.so",
             "lib/arm64-v8a/libovrplatformloader_orig.so", "lib/arm64-v8a/lib0123456789abcdef.so")

    def make(self, extra=(), stamp=b"arena", extra_bytes=None):
        path = pathlib.Path(tempfile.mkdtemp()) / "arena.apk"
        self.addCleanup(shutil.rmtree, path.parent)
        with zipfile.ZipFile(path, "w") as z:
            for n in (*self.STORE, *extra):
                z.writestr(n, f"bytes of {n}")
            for n, data in (extra_bytes or {}).items():
                z.writestr(n, data)
            if stamp is not None:
                z.writestr(qb.ARENA_STAMP_ENTRY, stamp)
        return path

    def refused(self, path):
        with self.assertRaises(qb.Refused) as caught:
            qb.check_arena(path)
        return caught.exception

    def test_a_store_apk_with_the_loader_and_the_arena_stamp_passes(self):
        self.assertEqual(qb.check_arena(self.make())["event"], "arena_has_no_combat")

    def test_each_combat_entry_is_refused_by_name(self):
        for entry in (*qb.REFERENCE_ENTRIES, "lib/arm64-v8a/libq15t.so", "lib/arm64-v8a/libnevr_plugin_manifest.so"):
            with self.subTest(entry=entry):
                refusal = self.refused(self.make(extra=(entry,)))
                self.assertEqual(refusal.event, "arena_has_combat_entries")
                self.assertEqual(refusal.fields["entries"], [entry])

    def test_the_reference_runtime_is_refused_and_the_stores_own_libvrapi_passes(self):
        """docs/adr/0047: the default Quest build carries no libvrapi.so from the reference build. The reference runtime is known by
        its libr15 checksum table (twelve copies of one md5 text, quest_runtime_pin.carries_table); the store's own library has none."""
        table = (pin.STOCK_ENTRY_MD5.encode() + b"\0") * pin.SITES
        refusal = self.refused(self.make(extra_bytes={qb.RUNTIME_ENTRY: b"\x7fELF reference runtime\0" + table}))
        self.assertEqual(refusal.event, "arena_has_combat_entries")
        self.assertEqual(refusal.fields["entries"], [qb.RUNTIME_ENTRY])
        store = self.make(extra_bytes={qb.RUNTIME_ENTRY: b"\x7fELF the store's VrApi\0" + pin.STOCK_ENTRY_MD5.encode() + b"\0"})
        self.assertEqual(qb.check_arena(store)["event"], "arena_has_no_combat")

    def test_a_stamp_other_than_arena_or_none_is_refused(self):
        for stamp in (None, b"", b"combat", b"q15t", b"Arena", b"arena\n"):
            with self.subTest(stamp=stamp):
                self.assertEqual(self.refused(self.make(stamp=stamp)).event, "arena_stamp_wrong")

    def test_the_command_line_prints_one_json_line_either_way(self):
        good, bad = self.make(), self.make(extra=("assets/afk_default_on.txt",))
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(qb.main(["check-arena", "--apk", str(good)]), 0)
            self.assertEqual(qb.main(["check-arena", "--apk", str(bad)]), 1)
        self.assertEqual(json.loads(out.getvalue())["event"], "arena_has_no_combat")
        self.assertEqual(json.loads(err.getvalue())["event"], "arena_has_combat_entries")


class Command(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = qb.main(list(argv))
        return rc, out.getvalue(), err.getvalue()

    def test_a_refusal_is_one_json_line_on_stderr_and_exit_1(self):
        base = write_zip(self.tmp / "base.apk", REFERENCE)
        release = write_zip(self.tmp / "release.apk", ("AndroidManifest.xml",))
        rc, out, err = self.run_main("check-release", "--apk", str(release), "--base", str(base))
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        record = json.loads(err)
        self.assertEqual((record["tag"], record["event"]), ("NEVR.QUEST.BASE", "release_lacks_base_entries"))
        self.assertEqual(record["missing_count"], 4)

    def test_a_pass_is_one_json_line_on_stdout(self):
        base = write_zip(self.tmp / "base.apk", REFERENCE)
        release = write_zip(self.tmp / "release.apk", REFERENCE + ("lib/arm64-v8a/libovrplatformloader_orig.so",))
        rc, out, err = self.run_main("check-release", "--apk", str(release), "--base", str(base))
        self.assertEqual((rc, err), (0, ""))
        self.assertEqual(json.loads(out)["event"], "release_keeps_base")

    def test_prepare_refuses_the_store_apk_on_the_command_line(self):
        store = write_zip(self.tmp / "store.apk", ("AndroidManifest.xml",))
        rc, out, err = self.run_main("prepare", "--apk", str(store), "--out", str(self.tmp / "base.apk"))
        self.assertEqual(rc, 1)
        self.assertEqual(json.loads(err)["event"], "not_the_reference_apk")


if __name__ == "__main__":
    unittest.main()
