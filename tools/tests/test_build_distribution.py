"""Failure-closed signing and package publication tests."""

from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

from tools import build_distribution

REPO = pathlib.Path(__file__).resolve().parents[2]


def valid_pe() -> bytes:
    image = bytearray(64 + 24 + 240)
    image[:2] = b"MZ"
    image[0x3C:0x40] = (64).to_bytes(4, "little")
    image[64:68] = b"PE\0\0"
    image[68:70] = (0x8664).to_bytes(2, "little")
    image[84:86] = (240).to_bytes(2, "little")
    image[88:90] = (0x20B).to_bytes(2, "little")
    return bytes(image)


FAKE_SIGNER = r'''#!/usr/bin/env python3
import os, pathlib, shutil, sys
args = sys.argv[1:]
if args[0] == "sign":
    source = pathlib.Path(args[args.index("-in") + 1])
    target = pathlib.Path(args[args.index("-out") + 1])
    if os.environ.get("FAKE_FAIL_NAME") == source.name:
        raise SystemExit(7)
    data = source.read_bytes()
    target.write_bytes(data + (b"TAMPERED" if os.environ.get("FAKE_TAMPER") else b"SIGNED"))
elif args[0] == "verify":
    source = pathlib.Path(args[args.index("-in") + 1])
    if not source.read_bytes().endswith(b"SIGNED"):
        raise SystemExit(8)
    ca = pathlib.Path(args[args.index("-CAfile") + 1])
    if ca.read_bytes() != pathlib.Path(os.environ["FAKE_EXPECTED_CA"]).read_bytes():
        raise SystemExit(9)
    pin = args[args.index("-require-leaf-hash") + 1].split(":", 1)[1].upper()
    if pin != os.environ.get("FAKE_PIN", "A" * 64):
        raise SystemExit(10)
else:
    raise SystemExit(11)
'''


class BuildDistributionTest(unittest.TestCase):
    def setUp(self):
        scratch = pathlib.Path("/var/tmp/work-nevr-runtime")
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.root = pathlib.Path(self.temp.name)
        self.stage = self.root / "stage"
        self.stage.mkdir()
        (self.stage / "BugSplat64.dll").write_bytes(valid_pe())
        (self.stage / "echovr_server.exe").write_bytes(valid_pe())
        self.destination = self.root / "dist" / "runtime"
        self.tar_path = self.root / "dist" / "runtime.tar.zst"
        self.zip_path = self.root / "dist" / "runtime.zip"
        self.dist = self.root / "dist"
        self.dist.mkdir(exist_ok=True)
        self.fake_tools = self.root / "fake-tools"
        self.fake_tools.mkdir()
        self.signer = self.fake_tools / "osslsigncode"
        self.signer.write_text(FAKE_SIGNER)
        self.signer.chmod(0o755)
        self.ca_file = self.root / "ca.pem"
        self.ca_file.write_bytes(b"trusted-root")
        self.cert = self.root / "signer.pem"
        self.key = self.root / "signer.key"
        self.cert.write_text("fake cert")
        self.key.write_text("fake key")
        generated = subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(self.root / "root.key"), "-out", str(self.root / "root.crt"),
            "-subj", "/CN=DistributionTestRoot", "-days", "2",
        ], capture_output=True, text=True, check=False)
        self.assertEqual(generated.returncode, 0, generated.stderr)
        wrong_root = subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(self.root / "wrong-root.key"), "-out", str(self.root / "wrong-root.crt"),
            "-subj", "/CN=WrongDistributionRoot", "-days", "2",
        ], capture_output=True, text=True, check=False)
        self.assertEqual(wrong_root.returncode, 0, wrong_root.stderr)
        fp = subprocess.run(["openssl", "x509", "-in", str(self.root / "root.crt"),
                             "-noout", "-fingerprint", "-sha256"], capture_output=True,
                            text=True, check=True).stdout.strip().rsplit("=", 1)[1]
        self.root_pin = fp.replace(":", "").upper()
        self.env = {
            "PATH": os.environ["PATH"],
            "OSSLSIGNCODE": str(self.signer),
            "CODESIGN_CERT": str(self.cert),
            "CODESIGN_KEY": str(self.key),
            "CODESIGN_CA_FILE": str(self.root / "root.crt"),
            "CODESIGN_SIGNER_SHA256": "A" * 64,
            "CODESIGN_ROOT_SHA256": self.root_pin,
            "FAKE_PIN": "A" * 64,
            "FAKE_EXPECTED_CA": str(self.root / "root.crt"),
            "NEVR_CODESIGN_TEMP": str(self.root),
        }

    def tearDown(self):
        self.temp.cleanup()

    def run_driver(self, required=False, env=None):
        command = [
            "python3", str(REPO / "tools/build_distribution.py"),
            "--package", str(self.stage), "--destination", str(self.destination),
            "--tar", str(self.tar_path), "--zip", str(self.zip_path),
        ]
        if required:
            command.append("--required-signing")
        return subprocess.run(command, env=env, capture_output=True, text=True)

    def test_pe_manifest_rejects_bad_offsets_machine_and_truncated_optional_header(self):
        bad_cases = []
        bad_offset = bytearray(valid_pe())
        bad_offset[0x3C:0x40] = (0xFFFFFFFF).to_bytes(4, "little")
        bad_cases.append(bad_offset)
        bad_machine = bytearray(valid_pe())
        bad_machine[68:70] = (0x14C).to_bytes(2, "little")
        bad_cases.append(bad_machine)
        bad_optional = bytearray(valid_pe())
        bad_optional[84:86] = (2).to_bytes(2, "little")
        bad_cases.append(bad_optional)
        for index, image in enumerate(bad_cases):
            with self.subTest(index=index):
                (self.stage / "BugSplat64.dll").write_bytes(image)
                with self.assertRaisesRegex(RuntimeError, "PE|header"):
                    build_distribution.pe_manifest(self.stage)
                (self.stage / "BugSplat64.dll").write_bytes(valid_pe())

    def test_required_mode_rejects_missing_credentials_and_optional_is_unsigned(self):
        with mock.patch.dict(os.environ, {"NEVR_CODESIGN_TEMP": str(self.root)}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "credentials"):
                build_distribution.sign_package(self.stage, required=True)
            build_distribution.sign_package(self.stage, required=False)

    def test_required_skip_is_rejected_but_local_skip_is_allowed(self):
        with mock.patch.dict(os.environ, {"CODESIGN_SKIP": "1"}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "forbidden"):
                build_distribution.sign_package(self.stage, required=True)
            build_distribution.sign_package(self.stage, required=False)

    def test_required_mode_rejects_missing_signer_tool(self):
        env = dict(self.env, OSSLSIGNCODE=str(self.root / "missing-osslsigncode"))
        with mock.patch.dict(os.environ, env, clear=True):
            with self.assertRaisesRegex(RuntimeError, "not found"):
                build_distribution.sign_package(self.stage, required=True)

    def test_all_signatures_verify_before_any_original_is_replaced(self):
        original = {path.name: path.read_bytes() for path in self.stage.iterdir()}
        env = dict(self.env, FAKE_FAIL_NAME="echovr_server.exe")
        with mock.patch.dict(os.environ, env, clear=True):
            with self.assertRaisesRegex(RuntimeError, "command failed"):
                build_distribution.sign_package(self.stage, required=True)
        self.assertEqual({path.name: path.read_bytes() for path in self.stage.iterdir()}, original)

    def test_tampered_output_wrong_signer_and_wrong_ca_fail_closed(self):
        original = {path.name: path.read_bytes() for path in self.stage.iterdir()}
        cases = [dict(self.env, FAKE_TAMPER="1"),
                 dict(self.env, CODESIGN_SIGNER_SHA256="B" * 64),
                 dict(self.env, CODESIGN_CA_FILE=str(self.ca_file)),
                 dict(self.env, CODESIGN_CA_FILE=str(self.root / "wrong-root.crt"))]
        for env in cases:
            with self.subTest(env=env.get("FAKE_TAMPER", env.get("CODESIGN_SIGNER_SHA256"))):
                with mock.patch.dict(os.environ, env, clear=True):
                    with self.assertRaises(RuntimeError):
                        build_distribution.sign_package(self.stage, required=True)
                self.assertEqual({path.name: path.read_bytes() for path in self.stage.iterdir()}, original)

    def test_later_signing_failure_preserves_published_package_and_archives(self):
        self.destination.mkdir()
        old_package = self.destination / "old-marker"
        old_package.write_bytes(b"old package")
        self.tar_path.write_bytes(b"old tar")
        self.zip_path.write_bytes(b"old zip")
        originals = {path.name: path.read_bytes() for path in self.stage.iterdir()}
        result = self.run_driver(required=True, env=dict(self.env, FAKE_FAIL_NAME="echovr_server.exe"))
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(old_package.read_bytes(), b"old package")
        self.assertEqual(self.tar_path.read_bytes(), b"old tar")
        self.assertEqual(self.zip_path.read_bytes(), b"old zip")
        self.assertEqual({path.name: path.read_bytes() for path in self.stage.iterdir()}, originals)

    def test_archive_failure_preserves_existing_outputs_and_success_publishes_both(self):
        self.destination.mkdir()
        old = self.destination / "old-marker"
        old.write_bytes(b"old")
        self.tar_path.write_bytes(b"previous tar")
        self.zip_path.write_bytes(b"previous zip")
        failing_bin = self.root / "failing-bin"
        failing_bin.mkdir()
        fake_tar = failing_bin / "tar"
        fake_tar.write_text("#!/bin/sh\nexit 19\n")
        fake_tar.chmod(0o755)
        failure_env = {"PATH": str(failing_bin) + os.pathsep + os.environ["PATH"],
                       "NEVR_CODESIGN_TEMP": str(self.root)}
        failed = self.run_driver(env=failure_env)
        self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
        self.assertEqual(old.read_bytes(), b"old")
        self.assertEqual(self.tar_path.read_bytes(), b"previous tar")
        self.assertEqual(self.zip_path.read_bytes(), b"previous zip")

        succeeded = self.run_driver(env={"PATH": os.environ["PATH"],
                                         "NEVR_CODESIGN_TEMP": str(self.root)})
        self.assertEqual(succeeded.returncode, 0, succeeded.stdout + succeeded.stderr)
        self.assertTrue((self.destination / "BugSplat64.dll").is_file())
        self.assertGreater(self.tar_path.stat().st_size, 0)
        self.assertGreater(self.zip_path.stat().st_size, 0)
        build_distribution.verify_archives(self.tar_path, self.zip_path, self.destination, None)

    def test_missing_required_runtime_pe_is_rejected(self):
        for missing in ("echovr_server.exe", "BugSplat64.dll"):
            with self.subTest(missing=missing):
                artifact = self.stage / missing
                content = artifact.read_bytes()
                artifact.unlink()
                with self.assertRaisesRegex(RuntimeError, missing):
                    build_distribution.pe_manifest(self.stage)
                artifact.write_bytes(content)

    @unittest.skipUnless(shutil.which("osslsigncode") and
                         (REPO / "build/mingw-release/bin/BugSplat64.dll").is_file() and
                         (REPO / "build/mingw-release/bin/echovr_server.exe").is_file(),
                         "real signer and built runtime binaries are required")
    def test_throwaway_certificate_signs_and_verifies_real_runtime_binaries(self):
        root_key = self.root / "test-root.key"
        root_cert = self.root / "test-root.pem"
        root_generated = subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(root_key), "-out", str(root_cert), "-days", "2",
            "-subj", "/CN=NEVR Distribution Test Root",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], capture_output=True, text=True, check=False)
        self.assertEqual(root_generated.returncode, 0, root_generated.stderr)
        leaf_key = self.root / "test-signer.key"
        leaf_csr = self.root / "test-signer.csr"
        leaf_cert = self.root / "test-signer.pem"
        generated = subprocess.run([
            "openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(leaf_key), "-out", str(leaf_csr),
            "-subj", "/CN=NEVR Throwaway Code Signer",
        ], capture_output=True, text=True, check=False)
        self.assertEqual(generated.returncode, 0, generated.stderr)
        extensions = self.root / "signer.ext"
        extensions.write_text(
            "basicConstraints=critical,CA:FALSE\n"
            "keyUsage=critical,digitalSignature\n"
            "extendedKeyUsage=codeSigning\n"
            "subjectKeyIdentifier=hash\n"
            "authorityKeyIdentifier=keyid,issuer\n"
        )
        signed_cert = subprocess.run([
            "openssl", "x509", "-req", "-in", str(leaf_csr), "-CA", str(root_cert),
            "-CAkey", str(root_key), "-CAcreateserial", "-out", str(leaf_cert),
            "-days", "2", "-sha256", "-extfile", str(extensions),
        ], capture_output=True, text=True, check=False)
        self.assertEqual(signed_cert.returncode, 0, signed_cert.stderr)
        leaf_der = subprocess.run(["openssl", "x509", "-in", str(leaf_cert), "-outform", "DER"],
                                  capture_output=True, check=True).stdout
        signer_pin = hashlib.sha256(leaf_der).hexdigest().upper()
        (self.stage / "BugSplat64.dll").write_bytes(
            (REPO / "build/mingw-release/bin/BugSplat64.dll").read_bytes())
        (self.stage / "echovr_server.exe").write_bytes(
            (REPO / "build/mingw-release/bin/echovr_server.exe").read_bytes())
        before = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in self.stage.iterdir()}
        signing_env = {
            "PATH": os.environ["PATH"],
            "CODESIGN_CERT": str(leaf_cert),
            "CODESIGN_KEY": str(leaf_key),
            "CODESIGN_CHAIN": str(root_cert),
            "CODESIGN_CA_FILE": str(root_cert),
            "CODESIGN_SIGNER_SHA256": signer_pin,
            "CODESIGN_ROOT_SHA256": self.root_pin,
        }
        root_actual = subprocess.run(["openssl", "x509", "-in", str(root_cert), "-outform", "DER"],
                                     capture_output=True, check=True).stdout
        signing_env["CODESIGN_ROOT_SHA256"] = hashlib.sha256(root_actual).hexdigest().upper()
        with mock.patch.dict(os.environ, signing_env, clear=True):
            build_distribution.sign_package(self.stage, required=True)
        after = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in self.stage.iterdir()}
        self.assertNotEqual(before, after)
        with mock.patch.dict(os.environ, signing_env, clear=True):
            for artifact in self.stage.iterdir():
                build_distribution.verify_one("linux", ["osslsigncode"], artifact)


if __name__ == "__main__":
    unittest.main()
