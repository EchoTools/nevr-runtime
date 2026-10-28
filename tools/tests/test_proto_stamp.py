"""tools/proto_stamp.py: gen/ is accepted only as the generator's output for the pin and buf.gen.yaml the justfile names.

buf is replaced by a fake that writes the files a real run writes, so no network and no protoc are needed.
"""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
STAMPER = REPO / "tools/proto_stamp.py"
PIN = "259a506a3e1b490ba3482e24e92ba4db"
NEW_PIN = "0123456789abcdef0123456789abcdef"
BUF_GEN = "version: v2\nplugins:\n  - protoc_builtin: cpp\n    out: gen/cpp\n"

# Writes the files named in FAKE_BUF_FILES ({relative path under gen/: content}) into the -o directory,
# records its argv and the first PATH entry, and fails when FAKE_BUF_FAIL is set (a BSR outage).
FAKE_BUF = """#!/usr/bin/env python3
import json, os, pathlib, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_BUF_LOG"], "a", encoding="utf-8") as f:
    f.write(json.dumps({"argv": argv, "path0": os.environ["PATH"].split(os.pathsep)[0]}) + "\\n")
if os.environ.get("FAKE_BUF_FAIL"):
    print("fake buf: BSR unreachable", file=sys.stderr)
    sys.exit(1)
out = pathlib.Path(argv[argv.index("-o") + 1])
for rel, text in json.loads(os.environ["FAKE_BUF_FILES"]).items():
    p = out / "gen" / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")
"""


class ProtoStampTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="proto-stamp-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.root = self.tmp / "root"
        (self.root / "tools").mkdir(parents=True)
        shutil.copy(STAMPER, self.root / "tools/proto_stamp.py")
        self.write_pin(PIN)
        (self.root / "buf.gen.yaml").write_text(BUF_GEN)
        self.protoc = self.tmp / "protoc-bin"
        self.protoc.mkdir()
        (self.protoc / "protoc").write_text("#!/bin/sh\nexit 0\n")
        (self.protoc / "protoc").chmod(0o755)
        self.shim = self.tmp / "shim"
        self.shim.mkdir()
        (self.shim / "buf").write_text(FAKE_BUF)
        (self.shim / "buf").chmod(0o755)
        self.log = self.tmp / "buf.log"
        self.files = {"cpp/a.pb.cc": "a v1\n", "cpp/b.pb.h": "b v1\n"}
        self.env = dict(os.environ, PATH=f"{self.shim}{os.pathsep}{os.environ['PATH']}", FAKE_BUF_LOG=str(self.log),
                        FAKE_BUF_FILES=json.dumps(self.files), XDG_STATE_HOME=str(self.tmp / "state"))
        self.env.pop("FAKE_BUF_FAIL", None)

    def write_pin(self, pin):
        (self.root / "justfile").write_text(f'proto_pin := "{pin}"\n')

    def run_stamp(self, *args):
        return subprocess.run([sys.executable, "-I", str(self.root / "tools/proto_stamp.py"), "--root", str(self.root),
                               *args], capture_output=True, text=True, env=self.env, timeout=60)

    def generate(self):
        result = self.run_stamp("generate", "--protoc-dir", str(self.protoc))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def buf_runs(self):
        return len(self.log.read_text().splitlines()) if self.log.exists() else 0

    def stamp(self):
        return json.loads((self.root / "gen/.proto-stamp").read_text())

    def test_no_stamp_is_stale_and_says_so(self):
        (self.root / "gen/cpp").mkdir(parents=True)
        (self.root / "gen/cpp/a.pb.cc").write_text("a v1\n")
        result = self.run_stamp("check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no stamp", result.stderr)

    def test_generate_writes_the_files_and_a_stamp_that_checks_current(self):
        self.generate()
        self.assertEqual((self.root / "gen/cpp/a.pb.cc").read_text(), "a v1\n")
        self.assertEqual(self.stamp()["pin"], PIN)
        self.assertEqual(sorted(self.stamp()["files"]), ["cpp/a.pb.cc", "cpp/b.pb.h"])
        result = self.run_stamp("check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_changed_pin_is_stale(self):
        self.generate()
        self.write_pin(NEW_PIN)
        result = self.run_stamp("check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("pin changed", result.stderr)
        self.assertIn(PIN, result.stderr)
        self.assertIn(NEW_PIN, result.stderr)

    def test_an_edited_buf_gen_yaml_is_stale(self):
        self.generate()
        (self.root / "buf.gen.yaml").write_text(BUF_GEN + "# edited\n")
        result = self.run_stamp("check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("buf.gen.yaml changed", result.stderr)

    def test_a_missing_generated_file_is_stale_and_named(self):
        self.generate()
        (self.root / "gen/cpp/b.pb.h").unlink()
        result = self.run_stamp("check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("gen/cpp/b.pb.h is missing", result.stderr)

    def test_an_edited_generated_file_is_stale(self):
        self.generate()
        (self.root / "gen/cpp/a.pb.cc").write_text("trunc")
        result = self.run_stamp("check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("gen/cpp/a.pb.cc differs", result.stderr)

    def test_ensure_does_not_run_buf_when_gen_is_current(self):
        self.generate()
        self.assertEqual(self.buf_runs(), 1)
        result = self.run_stamp("ensure", "--protoc-dir", str(self.protoc))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.buf_runs(), 1, "ensure ran buf on a current gen/")

    def test_ensure_regenerates_at_the_new_pin_when_the_pin_changed(self):
        self.generate()
        self.write_pin(NEW_PIN)
        result = self.run_stamp("ensure", "--protoc-dir", str(self.protoc))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        last = json.loads(self.log.read_text().splitlines()[-1])
        self.assertIn(f"buf.build/echotools/nevr-api:{NEW_PIN}", last["argv"])
        self.assertEqual(self.stamp()["pin"], NEW_PIN)
        self.assertEqual(self.run_stamp("check").returncode, 0)

    def test_generate_puts_the_protoc_dir_first_on_path(self):
        self.generate()
        last = json.loads(self.log.read_text().splitlines()[-1])
        self.assertEqual(last["path0"], str(self.protoc))

    def test_a_failed_generate_leaves_gen_and_its_stamp_untouched(self):
        self.generate()
        self.write_pin(NEW_PIN)
        self.env["FAKE_BUF_FAIL"] = "1"
        result = self.run_stamp("generate", "--protoc-dir", str(self.protoc))
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("gen/ is unchanged", result.stderr)
        self.assertEqual((self.root / "gen/cpp/a.pb.cc").read_text(), "a v1\n")
        self.assertEqual(self.stamp()["pin"], PIN)
        self.assertEqual(sorted(p.name for p in self.root.iterdir() if p.name.startswith(".proto-gen-")), [])

    def test_generate_removes_only_the_files_the_old_stamp_listed(self):
        self.generate()
        (self.root / "gen/go").mkdir()
        (self.root / "gen/go/own.go").write_text("own\n")
        self.write_pin(NEW_PIN)
        self.env["FAKE_BUF_FILES"] = json.dumps({"cpp/a.pb.cc": "a v2\n"})
        result = self.run_stamp("generate", "--protoc-dir", str(self.protoc))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "gen/cpp/a.pb.cc").read_text(), "a v2\n")
        self.assertFalse((self.root / "gen/cpp/b.pb.h").exists(), "a file the old stamp listed was kept")
        self.assertEqual((self.root / "gen/go/own.go").read_text(), "own\n", "a file the user owns was removed")

    def test_a_missing_protoc_is_an_error_and_never_falls_back_to_path(self):
        result = self.run_stamp("generate", "--protoc-dir", str(self.tmp / "absent"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no protoc", result.stderr)
        self.assertEqual(self.buf_runs(), 0, "buf ran without the vcpkg protoc")

    def test_a_justfile_without_the_pin_is_an_error(self):
        (self.root / "justfile").write_text("preset := \"mingw-release\"\n")
        result = self.run_stamp("check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("proto_pin", result.stderr)

    def test_every_generate_is_recorded_with_its_pin(self):
        self.generate()
        records = [json.loads(line) for line in (self.tmp / "state/nevr-runtime/proto-stamp.jsonl").read_text().splitlines()]
        self.assertEqual(records[-1]["verb"], "generate")
        self.assertEqual(records[-1]["outcome"], "generated")
        self.assertEqual(records[-1]["pin"], PIN)


class JustfileWiringTest(unittest.TestCase):
    """configure regenerates a stale gen/ after vcpkg (which installs the protoc) and before its check."""

    def dry_run(self, recipe):
        out = subprocess.run(["just", "--dry-run", recipe], cwd=REPO, capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout + out.stderr

    def test_configure_regenerates_between_vcpkg_and_the_check(self):
        text = self.dry_run("configure")
        vcpkg = text.find("vcpkg install")
        ensure = text.find("proto_stamp.py")
        check = text.find("worktree-setup.sh --check")
        self.assertGreater(vcpkg, -1, text)
        self.assertGreater(ensure, vcpkg, "gen/ is regenerated before vcpkg has installed protoc")
        self.assertGreater(check, ensure, "gen/ is checked before it is regenerated")
        self.assertIn(" ensure ", text[ensure:ensure + 200])

    def test_proto_runs_the_generator_at_the_pin(self):
        text = self.dry_run("proto")
        self.assertIn("proto_stamp.py", text)
        self.assertIn(" generate ", text)


if __name__ == "__main__":
    unittest.main()
