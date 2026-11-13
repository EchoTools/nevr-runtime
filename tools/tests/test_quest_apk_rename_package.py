"""Fixtures of tools/quest_apk_rename_package.py for the Python release tests (#1737).

The cases that stood here are ported to Go: cmd/quest-apk-rename-package/main_test.go and internal/questapk/rename_test.go.
What stays is the fixture helpers test_quest_release_base.py imports (entries, write_apk, read_apk, OLD, REFERENCE_LABEL,
LIBR15); it ends with that test's port.

The original description follows.

tools/quest_apk_rename_package.py: the reference APK's package rename (afk to r15), with every count asserted (#1737).

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
