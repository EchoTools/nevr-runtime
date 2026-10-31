"""tools/quest_apk_version.py: the versionCode numbering rule and the strict manifest reader and writer (#923).

The expected numbers are written out here by hand from the rule (major*10^7 + minor*10^5 + patch*10^3 + slot,
slot = dev distance 0..998, or 999 for a release), not computed by the tool. The manifests are built by
axml_fixtures.py; the real store APK's manifest (when the file is present) is parsed too, so a builder that
mirrors a misreading of the format is caught, and aapt2, a reader that is not ours, confirms every write.

The stamp floor is the major of the newest release tag git lists (#1371). Nothing here pins that major: the tests
that stamp read it from git, and the floor's own tests use throwaway repositories or a mocked newest release.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "tests"))
import axml_fixtures as fx  # noqa: E402
import quest_apk_version as qv  # noqa: E402
import work_root  # noqa: E402

TOOL = REPO / "tools" / "quest_apk_version.py"
# The store APK is named by NEVR_STORE_APK (no default); the one test that reads it says so when it is unset.
STORE_APK_ENV = os.environ.get("NEVR_STORE_APK", "")
INSTALLER = 4987570


def current_major() -> int:
    """The major of the newest release tag this clone lists: the stamp floor. Read, not pinned, so a new major does not
    stale the fixtures that must stamp at or above it."""
    return qv.newest_release()[1]


class NumberingTest(unittest.TestCase):
    # versionCode = major*10^7 + minor*10^5 + patch*10^3 + slot; slot = dev distance N, or 999 for a release. Worked by hand.
    TABLE = (
        ("5.6.0", 50_600_999),
        ("5.6.1-dev.0+3a35e0b", 50_601_000),
        ("5.6.1-dev.1+3a35e0b", 50_601_001),
        ("5.6.1-dev.37+3a35e0b", 50_601_037),
        ("5.6.1-dev.998+3a35e0b", 50_601_998),
        ("5.6.1", 50_601_999),
        ("5.7.0-dev.1+abcdef0", 50_700_001),
        ("5.7.0", 50_700_999),
        ("6.0.0", 60_000_999),
        ("10.20.30", 102_030_999),
        ("5.0.0", 50_000_999),
        ("5.0.0-dev.0+abcdef0", 50_000_000),
        ("5.99.99", 59_999_999),
        ("209.0.0", 2_090_000_999),
        ("209.99.98-dev.998+abcdef0", 2_099_998_998),
        ("209.99.99-dev.998+abcdef0", 2_099_999_998),
        ("209.99.99", 2_099_999_999),
    )

    def test_table(self):
        for version, code in self.TABLE:
            with self.subTest(version=version):
                self.assertEqual(qv.version_code_for(version), code)

    def test_every_code_beats_the_installers_and_fits_int32(self):
        for version, _ in self.TABLE:
            code = qv.version_code_for(version)
            self.assertGreater(code, INSTALLER)
            self.assertLess(code, 2**31)
            self.assertLess(code, 2_100_000_000)

    def test_the_largest_code_is_209_99_99_and_stays_under_int32_and_the_play_ceiling(self):
        # 209*10^7 + 99*10^5 + 99*10^3 + 999 = 2,090,000,000 + 9,900,000 + 99,000 + 999
        top = qv.version_code_for("209.99.99")
        self.assertEqual(top, 2_099_999_999)
        self.assertLess(top, 2**31 - 1)
        self.assertLess(top, 2_100_000_000)
        self.assertEqual(qv.MAX_VERSION_CODE, 2_100_000_000)

    def test_semver_precedence_is_code_precedence(self):
        # semver item 11: 5.6.0 < 5.6.1-dev.1 < 5.6.1-dev.2 < 5.6.1 < 5.7.0-dev.1 < 5.7.0
        order = ["5.6.0", "5.6.1-dev.1+aaaaaaa", "5.6.1-dev.2+bbbbbbb", "5.6.1-dev.10+ccccccc", "5.6.1-dev.998+ccccccc",
                 "5.6.1", "5.7.0-dev.1+ddddddd", "5.7.0", "6.0.0-dev.3+eeeeeee", "6.0.0"]
        codes = [qv.version_code_for(v) for v in order]
        self.assertEqual(codes, sorted(set(codes)), "codes must be strictly increasing in semver order")

    def test_same_distance_different_sha_gives_the_same_code(self):
        self.assertEqual(qv.version_code_for("5.6.1-dev.37+aaaaaaa"), qv.version_code_for("5.6.1-dev.37+bbbbbbb"))

    def test_refusals(self):
        for bad in ("", "5.6", "5.6.0.1", "v5.6.0", "5.6.0-rc.1", "5.6.0-dev.1", "5.6.0-dev.+abcdef0", "05.6.0",
                    "5.6.0-dev.01+abcdef0", "5.6.0-dev.1+XYZ", "-1.0.0", "5.6.0 ", "5.6.1-dev.1+abcdef0\n"):
            with self.subTest(version=bad):
                with self.assertRaises(qv.VersionError):
                    qv.version_code_for(bad)

    def test_each_part_is_refused_just_above_its_limit_and_named(self):
        cases = (
            ("210.0.0", "major 210 is above 209"),
            ("5.100.0", "minor 100 is above 99"),
            ("5.6.100", "patch 100 is above 99"),
            ("5.6.1-dev.999+abcdef0", "development distance N 999 is above 998"),
            ("5.6.1-dev.1000+abcdef0", "development distance N 1000 is above 998"),
            ("5.6.1-dev.99999+abcdef0", "development distance N 99999 is above 998"),
            ("999.0.0", "major 999 is above 209"),
        )
        for version, message in cases:
            with self.subTest(version=version):
                with self.assertRaisesRegex(qv.VersionError, message):
                    qv.version_code_for(version)

    def test_each_part_is_accepted_at_its_limit(self):
        for version in ("209.0.0", "5.99.0", "5.0.99", "5.6.1-dev.998+abcdef0", "5.6.1-dev.0+abcdef0"):
            with self.subTest(version=version):
                qv.version_code_for(version)

    def test_a_release_and_the_dev_slot_do_not_collide(self):
        # dev N tops out at 998 so that slot 999 is only ever a release
        self.assertLess(qv.version_code_for("5.6.1-dev.998+abcdef0"), qv.version_code_for("5.6.1"))

    def test_warnings_start_one_past_the_threshold_and_name_the_field_and_limit(self):
        # dev N > 900, minor > 90, patch > 90; the limits they warn about are 998, 99 and 99
        self.assertEqual(qv.field_warnings("5.6.1-dev.900+abcdef0"), [])
        (w,) = qv.field_warnings("5.6.1-dev.901+abcdef0")
        self.assertIn("development distance N 901", w)
        self.assertIn("998", w)
        self.assertEqual(qv.field_warnings("5.90.0"), [])
        (w,) = qv.field_warnings("5.91.0")
        self.assertIn("minor 91", w)
        self.assertIn("99", w)
        self.assertEqual(qv.field_warnings("5.6.90"), [])
        (w,) = qv.field_warnings("5.6.91")
        self.assertIn("patch 91", w)
        self.assertEqual(len(qv.field_warnings("5.91.91-dev.901+abcdef0")), 3)
        self.assertEqual(qv.field_warnings("5.6.1"), [])  # a release has no distance
        self.assertEqual(qv.field_warnings("not a version"), [])  # a warning never raises

    def test_field_problems_name_every_part_out_of_range_not_just_the_first(self):
        problems = qv.field_problems("210.100.100-dev.999+abcdef0")
        self.assertEqual(len(problems), 4, problems)
        for field in ("major 210", "minor 100", "patch 100", "development distance N 999"):
            self.assertTrue(any(field in p for p in problems), field)
        self.assertEqual(qv.field_problems("209.99.99-dev.998+abcdef0"), [])

    def test_the_rule_alone_has_no_floor(self):
        # the numbering rule is arithmetic; the floor (the newest release tag's major) applies at a stamp (FloorTest)
        self.assertEqual(qv.version_code_for("1.0.0"), 10_000_999)  # the code a build with no git reports
        self.assertEqual(qv.version_code_for("0.1.0"), 100_999)

    def test_every_accepted_code_beats_the_installer(self):
        smallest = qv.version_code_for("5.0.0-dev.1+abcdef0")
        self.assertEqual(smallest, 50_000_001)
        self.assertGreater(smallest, INSTALLER)
        self.assertEqual(qv.version_code_for("5.0.0-dev.0+abcdef0"), 50_000_000)  # the smallest code of major 5
        self.assertGreater(50_000_000, INSTALLER)


class NewestReleaseTest(unittest.TestCase):
    """newest_release(): the newest vX.Y.Z tag git lists, read from throwaway repositories so no release is pinned here."""

    def git(self, repo, *args):
        return subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                               "-c", "commit.gpgSign=false", *args], capture_output=True, text=True, check=True)

    def make_repo(self, tmp, tags):
        repo = pathlib.Path(tmp) / "repo"
        repo.mkdir()
        self.git(repo, "init", "-q")
        self.git(repo, "commit", "-q", "--allow-empty", "-m", "root")
        for name in tags:
            # a lightweight tag by ref: `git tag <name>` opens an editor when tag.gpgSign is set, as it is on this machine
            self.git(repo, "update-ref", f"refs/tags/{name}", "HEAD")
        return repo

    def test_the_newest_release_is_the_highest_by_number_ignoring_pre_and_malformed_tags(self):
        with tempfile.TemporaryDirectory(dir=work_root.test_scratch()) as tmp:
            repo = self.make_repo(tmp, ["v5.20.0", "v6.0.0", "vv7.0.0", "v7.0.0-rc.1", "v9.9.9", "v10.0.0", "v11.0.0-rc.1"])
            # "v9.9.9" sorts after "v10.0.0" as a string; the number decides, and a pre-release never does
            self.assertEqual(qv.newest_release(repo), ("v10.0.0", 10))

    def test_a_release_tag_HEAD_does_not_descend_from_still_counts(self):
        # every release tag the clone knows is the floor's input, not only those reachable from HEAD
        with tempfile.TemporaryDirectory(dir=work_root.test_scratch()) as tmp:
            repo = self.make_repo(tmp, ["v6.0.0"])
            self.git(repo, "checkout", "-q", "-b", "elsewhere")
            self.git(repo, "commit", "-q", "--allow-empty", "-m", "later")
            self.git(repo, "update-ref", "refs/tags/v12.0.0", "HEAD")
            self.git(repo, "checkout", "-q", "-")  # back on the root commit: v12.0.0 is not reachable from HEAD
            self.assertEqual(self.git(repo, "tag", "--merged", "HEAD", "--list", "v12.0.0").stdout.strip(), "")
            self.assertEqual(qv.newest_release(repo), ("v12.0.0", 12))

    def test_no_release_tag_is_refused_and_names_the_fetch(self):
        with tempfile.TemporaryDirectory(dir=work_root.test_scratch()) as tmp:
            repo = self.make_repo(tmp, ["v5.0.0-rc.1", "vv6.0.0", "release-6"])
            with self.assertRaisesRegex(qv.VersionError, "no release tag .*fetch"):
                qv.newest_release(repo)

    def test_a_path_git_cannot_read_is_refused_not_defaulted(self):
        # a missing path, not a plain directory: the work root above a scratch directory is itself a git repository,
        # so git would walk up from a plain directory and list that repository's tags
        with tempfile.TemporaryDirectory(dir=work_root.test_scratch()) as tmp:
            with self.assertRaisesRegex(qv.VersionError, "git cannot list the tags"):
                qv.newest_release(pathlib.Path(tmp) / "not-there")

    def test_this_clone_reads_the_same_newest_tag_as_git_lists_it(self):
        listed = subprocess.run(["git", "-C", str(REPO), "tag", "--list", "v*"], capture_output=True, text=True,
                                check=True).stdout.split()
        releases = [tuple(int(n) for n in t[1:].split(".")) for t in listed if re.fullmatch(r"v\d+\.\d+\.\d+", t)]
        if not releases:
            self.skipTest("this clone lists no release tag (a shallow checkout); the refusal is tested in the throwaway repositories")
        newest = max(releases)
        self.assertEqual(qv.newest_release(), ("v" + ".".join(str(n) for n in newest), newest[0]))


class FloorTest(unittest.TestCase):
    """stamp_code_for(): the rule, refused below the newest release tag's major. The newest release is fixed per test."""

    def newest_is(self, tag, major):
        patcher = mock.patch.object(qv, "newest_release", return_value=(tag, major))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_version_below_the_newest_major_is_refused_and_names_the_tag(self):
        # "1.0.0" is what a build with no git reports (cmake/set_project_version_from_git.cmake)
        self.newest_is("v6.0.0", 6)
        for low in ("1.0.0", "0.1.0", "5.20.0", "5.6.1-dev.37+3a35e0b", "5.99.99"):
            with self.subTest(version=low):
                with self.assertRaisesRegex(qv.VersionError, "below major 6, the major of the newest release tag v6.0.0"):
                    qv.stamp_code_for(low)

    def test_the_newest_major_is_accepted_with_the_rules_code(self):
        self.newest_is("v6.0.0", 6)
        self.assertEqual(qv.stamp_code_for("6.0.0"), 60_000_999)
        self.assertEqual(qv.stamp_code_for("6.0.1-dev.37+3a35e0b"), 60_001_037)

    def test_the_floor_follows_the_newest_release_when_it_moves(self):
        # a new major raises the floor with it: the major is counted from the tags, never stored
        self.newest_is("v7.0.0", 7)
        with self.assertRaisesRegex(qv.VersionError, "below major 7"):
            qv.stamp_code_for("6.0.1-dev.1+abcdef0")
        self.assertEqual(qv.stamp_code_for("7.0.0"), 70_000_999)

    def test_a_malformed_version_is_refused_by_the_rule_not_the_floor(self):
        self.newest_is("v6.0.0", 6)
        with self.assertRaisesRegex(qv.VersionError, "neither X.Y.Z"):
            qv.stamp_code_for("v6.0.0")

    def test_no_release_tag_means_no_stamp(self):
        with mock.patch.object(qv, "newest_release", side_effect=qv.VersionError("no release tag (vX.Y.Z) in repo")):
            with self.assertRaisesRegex(qv.VersionError, "no release tag"):
                qv.stamp_code_for("6.0.0")


class ParseTest(unittest.TestCase):
    def test_reads_the_code_and_its_offset(self):
        for utf8 in (True, False):
            with self.subTest(utf8=utf8):
                data = fx.build_manifest(4987566, utf8=utf8)
                got = qv.parse_manifest(data)
                self.assertEqual(got.version_code, 4987566)
                self.assertEqual(struct.unpack_from("<I", data, got.offset)[0], 4987566)

    def test_refuses_each_defect(self):
        cases = {
            "second versionCode": dict(version_code_count=2),
            "string-typed code": dict(version_code_type=fx.TYPE_STRING),
            "hex code": dict(version_code_type=fx.TYPE_INT_HEX),
            "text twin": dict(version_code_raw=0),
            "wrong resource id": dict(version_code_id=fx.RES_MIN_SDK),
            "no android namespace": dict(version_code_ns_android=False),
            "versionCodeMajor present": dict(with_version_code_major=True),
            "no resource map": dict(with_resource_map=False),
            "root is not manifest": dict(root="application"),
            "bytes after the document": dict(trailing=b"\0\0\0\0"),
        }
        for label, kwargs in cases.items():
            with self.subTest(label):
                with self.assertRaises(qv.ManifestError):
                    qv.parse_manifest(fx.build_manifest(**kwargs))

    def test_a_versionCode_on_a_child_element_is_not_the_apps(self):
        # only the root <manifest> counts; a nested attribute of the same name must not be found or confuse the read
        data = fx.build_manifest(4987566, nested_version_code=True)
        self.assertEqual(qv.parse_manifest(data).version_code, 4987566)

    def test_refuses_garbage(self):
        good = fx.build_manifest()
        for label, data in {
            "empty": b"",
            "short": b"\x03\x00\x08\x00",
            "wrong type": struct.pack("<HHI", 0x0001, 8, 8),
            "size mismatch": good[:-4],
            "size field lies": good[:4] + struct.pack("<I", len(good) + 4) + good[8:],
            "unknown chunk": fx.build_manifest(trailing=struct.pack("<HHI", 0x0999, 8, 8)),
            "plain xml": b"<manifest/>",
            "zeros": bytes(64),
        }.items():
            with self.subTest(label):
                with self.assertRaises(qv.ManifestError):
                    qv.parse_manifest(data)

    def test_the_real_store_manifest_parses_and_agrees_with_aapt2(self):
        if not STORE_APK_ENV:
            self.skipTest("NEVR_STORE_APK is not set: name the store APK to run this check")
        store_apk = pathlib.Path(STORE_APK_ENV)
        if not store_apk.exists() or shutil.which("aapt2") is None:
            self.skipTest(f"the store APK at NEVR_STORE_APK={STORE_APK_ENV} or aapt2 is not on this machine")
        badging = subprocess.run(["aapt2", "dump", "badging", str(store_apk)], check=True, capture_output=True, text=True).stdout
        expected = int(badging.split("versionCode='")[1].split("'")[0])
        self.assertEqual(qv.version_code_of_apk(store_apk), expected)


class PatchTest(unittest.TestCase):
    def test_a_four_byte_write_and_nothing_else(self):
        for utf8 in (True, False):
            with self.subTest(utf8=utf8):
                data = fx.build_manifest(4987566, utf8=utf8)
                out = qv.patch_manifest(data, 50_601_037)
                self.assertEqual(len(out), len(data))
                diff = [i for i, (a, b) in enumerate(zip(data, out)) if a != b]
                self.assertTrue(diff and diff[-1] - diff[0] < 4, diff)
                self.assertEqual(qv.parse_manifest(out).version_code, 50_601_037)
                # the same document built directly with the new code is byte-identical
                self.assertEqual(out, fx.build_manifest(50_601_037, utf8=utf8))

    def test_only_ever_raises(self):
        data = fx.build_manifest(50_601_037)
        for code in (50_601_037, 50_601_036, 4987566):
            with self.subTest(code=code):
                with self.assertRaisesRegex(qv.ManifestError, "only ever raises"):
                    qv.patch_manifest(data, code)

    def test_refuses_a_code_out_of_range(self):
        data = fx.build_manifest()
        for code in (0, -1, 2_100_000_000, 2**31, 2**32):
            with self.subTest(code=code):
                with self.assertRaises(qv.ManifestError):
                    qv.patch_manifest(data, code)

    def test_refuses_a_manifest_the_reader_refuses(self):
        with self.assertRaises(qv.ManifestError):
            qv.patch_manifest(fx.build_manifest(version_code_count=2), 50_601_037)

    def test_the_after_write_comparison_catches_a_write_that_lands_elsewhere_too(self):
        # A write that sets the right code but also changes another byte (here the next attribute's data, which the
        # reader does not validate) must be refused: the comparison, not the reader, is what notices.
        data = fx.build_manifest(4987566)
        real = struct.pack_into

        def stray(fmt, buffer, offset, *values):
            real(fmt, buffer, offset, *values)
            buffer[offset + 20] ^= 0xFF

        with mock.patch.object(qv.struct, "pack_into", stray):
            with self.assertRaisesRegex(qv.ManifestError, "exactly the versionCode bytes"):
                qv.patch_manifest(data, 50_601_037)

    def test_aapt2_reads_the_patched_manifest(self):
        if shutil.which("aapt2") is None or shutil.which("zip") is None:
            self.skipTest("aapt2 or zip is missing")
        with tempfile.TemporaryDirectory() as tmp:
            apk = pathlib.Path(tmp) / "m.apk"
            with zipfile.ZipFile(apk, "w") as z:
                z.writestr("AndroidManifest.xml", qv.patch_manifest(fx.build_manifest(), 50_601_037))
            out = subprocess.run(["aapt2", "dump", "badging", str(apk)], capture_output=True, text=True).stdout
            self.assertIn("versionCode='50601037'", out)
            self.assertEqual(qv.version_code_of_apk(apk), 50_601_037)


class BuildInfoTest(unittest.TestCase):
    def header(self, tmp, version="5.6.1-dev.37+3a35e0b", text=None):
        path = pathlib.Path(tmp) / "nevr_build_info.h"
        path.write_text(text if text is not None else f'namespace nevr {{ constexpr const char* kVersion = "{version}"; }}\n')
        return path

    def test_reads_kversion(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(qv.read_build_version(self.header(tmp)), "5.6.1-dev.37+3a35e0b")

    def test_refuses_no_kversion_or_two(self):
        with tempfile.TemporaryDirectory() as tmp:
            for text in ("", 'kVersion = "5.6.0"; kVersion = "5.6.1";'):
                with self.assertRaises(qv.VersionError):
                    qv.read_build_version(self.header(tmp, text=text))
            with self.assertRaises(qv.VersionError):
                qv.read_build_version(pathlib.Path(tmp) / "missing.h")

    def test_the_shim_must_carry_the_whole_version(self):
        version = "5.6.1-dev.37+3a35e0b"
        self.assertTrue(qv.shim_carries_version(b"\0" + version.encode() + b"\0", version))
        self.assertFalse(qv.shim_carries_version(b"\0" + version.encode() + b"9\0", version), "a longer string is another version")
        self.assertFalse(qv.shim_carries_version(b"x" + version.encode() + b"\0", version), "a longer prefix is another version")
        self.assertFalse(qv.shim_carries_version(b"\0" + b"5.6.1-dev.3+3a35e0b" + b"\0", version))


class CommandLineTest(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL), *args], capture_output=True, text=True)

    def test_code(self):
        done = self.run_tool("code", "5.6.1-dev.37+3a35e0b")
        self.assertEqual((done.returncode, done.stdout.strip()), (0, "50601037"))
        record = json.loads(done.stderr)
        self.assertEqual((record["tag"], record["event"], record["version_code"]), ("NEVR.QUEST.APK", "version_code", 50601037))
        self.assertEqual(record["stampable"], 5 >= current_major())  # a major 5 version is below any newer major

    def test_the_bar_v6_0_0_is_strictly_above_v5_20_0_by_the_tool(self):
        # a headset on 5.20.0 updates in place to 6.0.0 only if 6.0.0's code is the higher: Android refuses a lower one
        six = self.run_tool("code", "6.0.0")
        five = self.run_tool("code", "5.20.0")
        self.assertEqual((six.returncode, six.stdout.strip()), (0, "60000999"), six.stderr)
        self.assertEqual((five.returncode, five.stdout.strip()), (0, "52000999"), five.stderr)
        self.assertGreater(int(six.stdout), int(five.stdout))

    def test_code_below_the_newest_major_prints_the_rules_number_and_says_it_is_not_stampable(self):
        done = self.run_tool("code", "0.1.0")
        self.assertEqual((done.returncode, done.stdout.strip()), (0, "100999"), done.stderr)
        record = json.loads(done.stderr)
        self.assertEqual((record["event"], record["version_code"], record["stampable"]), ("version_code", 100999, False))

    def test_code_refusal_is_one_json_line_and_exit_1(self):
        done = self.run_tool("code", "210.0.0")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        record = json.loads(done.stderr)
        self.assertEqual((record["tag"], record["event"]), ("NEVR.QUEST.APK", "refused"))
        self.assertIn("major 210 is above 209", record["reason"])

    def test_code_with_no_release_tag_is_refused_and_prints_no_code(self):
        # the rule alone would print a code; the CLI also reads the newest release tag, so with none readable it refuses
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(qv, "newest_release", side_effect=qv.VersionError("no release tag (vX.Y.Z) in repo")):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = qv.main(["code", "6.0.0"])
        self.assertEqual((code, out.getvalue()), (1, ""))
        record = json.loads(err.getvalue())
        self.assertEqual((record["tag"], record["event"]), ("NEVR.QUEST.APK", "refused"))
        self.assertIn("no release tag", record["reason"])

    def test_patch_manifest_writes_in_place_and_logs_before_and_after(self):
        major = current_major()
        version = f"{major}.6.1-dev.37+3a35e0b"
        code = major * 10**7 + 6 * 10**5 + 1 * 10**3 + 37
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            manifest = tmp / "AndroidManifest.xml"
            manifest.write_bytes(fx.build_manifest(4987566))
            header = tmp / "nevr_build_info.h"
            header.write_text(f'constexpr const char* kVersion = "{version}";\n')
            shim = tmp / "shim.so"
            shim.write_bytes(b"\x7fELF\0" + version.encode() + b"\0")
            done = self.run_tool("patch-manifest", str(manifest), "--build-info", str(header), "--shim", str(shim))
            self.assertEqual((done.returncode, done.stdout.strip()), (0, str(code)), done.stderr)
            record = json.loads(done.stderr)
            self.assertEqual((record["version_code_before"], record["version_code_after"], record["bytes_changed"]),
                             (4987566, code, 4))
            self.assertEqual(manifest.read_bytes(), fx.build_manifest(code))

    def test_patch_manifest_refuses_a_shim_from_another_build_and_leaves_the_manifest_alone(self):
        major = current_major()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            original = fx.build_manifest(4987566)
            manifest = tmp / "AndroidManifest.xml"
            manifest.write_bytes(original)
            header = tmp / "nevr_build_info.h"
            header.write_text(f'constexpr const char* kVersion = "{major}.6.1-dev.37+3a35e0b";\n')
            shim = tmp / "shim.so"
            shim.write_bytes(b"\0" + f"{major}.6.1-dev.36+3a35e0b".encode() + b"\0")
            done = self.run_tool("patch-manifest", str(manifest), "--build-info", str(header), "--shim", str(shim))
            self.assertEqual(done.returncode, 1)
            self.assertIn("different builds", json.loads(done.stderr)["reason"])
            self.assertEqual(manifest.read_bytes(), original)

    def test_patch_manifest_takes_the_version_directly_where_no_build_info_exists(self):
        """A release APK packages CI's loader and has no build tree: the tag's version is the version (#1135)."""
        major = current_major()
        version = f"{major}.16.2"
        code = major * 10**7 + 16 * 10**5 + 2 * 10**3 + 999
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            manifest = tmp / "AndroidManifest.xml"
            manifest.write_bytes(fx.build_manifest(4987566))
            shim = tmp / "shim.so"
            shim.write_bytes(b"\x7fELF\0" + version.encode() + b"\0")
            done = self.run_tool("patch-manifest", str(manifest), "--version", version, "--shim", str(shim))
            self.assertEqual((done.returncode, done.stdout.strip()), (0, str(code)), done.stderr)
            self.assertEqual(manifest.read_bytes(), fx.build_manifest(code))

    def test_patch_manifest_with_a_version_still_refuses_a_shim_that_does_not_carry_it(self):
        major = current_major()
        version = f"{major}.16.2"
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            original = fx.build_manifest(4987566)
            manifest = tmp / "AndroidManifest.xml"
            manifest.write_bytes(original)
            shim = tmp / "shim.so"
            shim.write_bytes(b"\0" + f"{major}.16.1".encode() + b"\0")
            done = self.run_tool("patch-manifest", str(manifest), "--version", version, "--shim", str(shim))
            self.assertEqual(done.returncode, 1)
            self.assertIn(version, json.loads(done.stderr)["reason"])
            self.assertEqual(manifest.read_bytes(), original)

    def test_patch_manifest_refuses_a_version_below_the_newest_major_and_writes_nothing(self):
        # the shim carries the version, so only the floor can refuse: the previous major, and "1.0.0" (a build with no git)
        major = current_major()
        for low in (f"{major - 1}.20.0", "1.0.0"):
            with self.subTest(version=low):
                with tempfile.TemporaryDirectory() as tmp:
                    tmp = pathlib.Path(tmp)
                    original = fx.build_manifest(4987566)
                    manifest = tmp / "AndroidManifest.xml"
                    manifest.write_bytes(original)
                    shim = tmp / "shim.so"
                    shim.write_bytes(b"\0" + low.encode() + b"\0")
                    done = self.run_tool("patch-manifest", str(manifest), "--version", low, "--shim", str(shim))
                    self.assertEqual((done.returncode, done.stdout), (1, ""), done.stderr)
                    self.assertIn(f"below major {major}", json.loads(done.stderr)["reason"])
                    self.assertEqual(manifest.read_bytes(), original)

    def test_patch_manifest_needs_exactly_one_source_of_the_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            manifest = tmp / "AndroidManifest.xml"
            manifest.write_bytes(fx.build_manifest(4987566))
            header = tmp / "nevr_build_info.h"
            header.write_text('constexpr const char* kVersion = "5.16.2";\n')
            shim = tmp / "shim.so"
            shim.write_bytes(b"\0" + b"5.16.2" + b"\0")
            neither = self.run_tool("patch-manifest", str(manifest), "--shim", str(shim))
            both = self.run_tool("patch-manifest", str(manifest), "--version", "5.16.2", "--build-info", str(header),
                                 "--shim", str(shim))
            self.assertEqual((neither.returncode, both.returncode), (2, 2))
            self.assertEqual(manifest.read_bytes(), fx.build_manifest(4987566))

    def test_an_apk_with_two_manifests_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            apk = pathlib.Path(tmp) / "dup.apk"
            with warnings.catch_warnings(), zipfile.ZipFile(apk, "w") as z:
                warnings.simplefilter("ignore")  # zipfile warns about a duplicate name, which is the point
                z.writestr("AndroidManifest.xml", fx.build_manifest(4987566))
                z.writestr("AndroidManifest.xml", fx.build_manifest(999_999_999))
            with self.assertRaisesRegex(qv.ManifestError, "2 AndroidManifest.xml entries"):
                qv.version_code_of_apk(apk)

    def test_read_prints_the_code_of_an_apk(self):
        with tempfile.TemporaryDirectory() as tmp:
            apk = pathlib.Path(tmp) / "a.apk"
            with zipfile.ZipFile(apk, "w") as z:
                z.writestr("AndroidManifest.xml", fx.build_manifest(4987566))
            done = self.run_tool("read", str(apk))
            self.assertEqual((done.returncode, done.stdout.strip()), (0, "4987566"))
            bad = pathlib.Path(tmp) / "b.apk"
            with zipfile.ZipFile(bad, "w") as z:
                z.writestr("other", b"x")
            self.assertEqual(self.run_tool("read", str(bad)).returncode, 1)
            self.assertEqual(self.run_tool("read", str(pathlib.Path(tmp) / "missing.apk")).returncode, 1)


if __name__ == "__main__":
    unittest.main()
