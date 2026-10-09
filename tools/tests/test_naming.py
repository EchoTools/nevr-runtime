"""docs/standards/naming.md, enforced: the `Nvr` plugin/module ABI spelling is a frozen set, and the
mixed-case spellings of the project name do not spread."""
from __future__ import annotations

import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
ABI_HEADERS = ("src/extension/plugin_interface.h", "src/extension/module_interface.h")
NVR = re.compile(r"\bNvr[A-Z][A-Za-z0-9_]*")
# Spellings that are not ABI symbols: the prose name of the lifecycle, a wildcard in a comment, and the
# exports of the test plugin DLL. The set only shrinks.
NVR_NON_ABI = frozenset({"NvrPlugin", "NvrPluginInterface", "NvrTestPluginGetFrameCount",
                         "NvrTestPluginGetInitCount"})
MIXED = re.compile(r"nEVR|NeVR|NEvR|NEVr|neVR|nEvr")
# Frozen code and vendored trees are out of scope; so are binary and generated files.
EXCLUDED_PREFIXES = ("src/legacy/", "extern/", "gen/", "docs/audits/")
TEXT_SUFFIXES = {".cpp", ".h", ".hpp", ".c", ".cc", ".py", ".sh", ".md", ".txt", ".cmake", ".yaml", ".yml",
                 ".json", ".def", ".rc", ".in", ".conf", ".go"}
# Files that already carry a mixed-case spelling in prose, a comment or a certificate subject.
# The list only shrinks; a new file is not added to it.
MIXED_CASE_FILES = frozenset({
    "README.md",
    "certs/code-signing.conf",
    "certs/generate-ca.sh",
    "certs/intermediate-ca.conf",
    "certs/root-ca.conf",
    "cmake/codesign/sign.sh",
    "docs/standards/naming.md",
    "plugins/common/include/address_registry.h",
    "plugins/example/README.md",
    "plugins/example/src/plugin.cpp",
    "src/extension/plugin_interface.h",
    "src/runtime/compat/ws_bridge.cpp",
    "tools/tests/test_naming.py",
})


def frozen_abi_names():
    """Every Nvr name the two ABI headers define, with the _fn/_t typedef suffixes folded away."""
    names = set()
    for header in ABI_HEADERS:
        for token in NVR.findall((REPO / header).read_text(encoding="utf-8")):
            names.add(re.sub(r"(_fn|_t)$", "", token))
    return names


def tracked_text_files():
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z"], check=True, capture_output=True,
                         text=True).stdout
    for path in (p for p in out.split("\0") if p):
        if path.startswith(EXCLUDED_PREFIXES) or pathlib.PurePosixPath(path).suffix not in TEXT_SUFFIXES:
            continue
        try:
            yield path, (REPO / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue


class NamingTest(unittest.TestCase):
    def test_the_abi_headers_define_a_nonempty_nvr_set(self):
        self.assertGreaterEqual(len(frozen_abi_names()), 20, "the sensor reads almost nothing from the ABI headers")

    def test_no_nvr_identifier_exists_outside_the_frozen_abi_set(self):
        frozen = frozen_abi_names() | NVR_NON_ABI
        stray = {}
        for path, text in tracked_text_files():
            for name in {re.sub(r"(_fn|_t)$", "", t) for t in NVR.findall(text)} - frozen:
                stray.setdefault(name, []).append(path)
        self.assertEqual(stray, {}, "`Nvr` is the frozen plugin/module ABI spelling; new names use Nevr/NEVR_ "
                         "(docs/standards/naming.md)")

    def test_mixed_case_project_spellings_do_not_spread(self):
        offenders = [path for path, text in tracked_text_files()
                     if MIXED.search(text) and path not in MIXED_CASE_FILES]
        self.assertEqual(offenders, [], "nEVR/NeVR/NEvR: use NEVR, Nevr or nevr (docs/standards/naming.md)")

    def test_every_listed_mixed_case_file_still_has_one(self):
        present = {path for path, text in tracked_text_files() if MIXED.search(text)}
        self.assertEqual(sorted(MIXED_CASE_FILES - present), [], "remove cleaned files from MIXED_CASE_FILES")


if __name__ == "__main__":
    unittest.main()
