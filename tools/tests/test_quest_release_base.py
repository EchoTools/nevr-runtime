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
import quest_release_base as qb  # noqa: E402

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
        release = write_zip(self.tmp / "release.apk", ("AndroidManifest.xml", "lib/arm64-v8a/libr15.so"))
        with self.assertRaises(qb.Refused) as caught:
            qb.check_release(release, store)
        self.assertEqual(caught.exception.event, "base_not_reference")

    def test_meta_inf_of_the_base_is_not_required(self):
        release = write_zip(self.tmp / "release.apk", [n for n in REFERENCE if not n.startswith("META-INF/")])
        self.assertEqual(qb.check_release(release, self.base)["event"], "release_keeps_base")


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
        release = write_zip(self.tmp / "release.apk", REFERENCE)
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
