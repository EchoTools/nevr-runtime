"""docs/standards/naming.md, enforced: the `Nvr` plugin/module ABI spelling is a frozen set that does not
grow, and a non-canonical spelling of the project name does not appear in any file or identifier that does
not already carry it. Both sets are written out below on purpose: changing one is a visible edit here."""
from __future__ import annotations

import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
# `Nvr` followed by a capital, not preceded by a letter; the _fn/_t typedef suffixes are folded away.
NVR = re.compile(r"(?<![A-Za-z])Nvr[A-Z][A-Za-z0-9_]*")
# Any spelling of the name made of the letters n-e-v-r in some case, not preceded by a letter.
ANY_CASE = re.compile(r"(?<![A-Za-z])[Nn][Ee][Vv][Rr][A-Za-z0-9_]*")
CANONICAL = re.compile(r"(NEVR|Nevr|nevr)")
# These files define the anti-patterns themselves.
SELF = frozenset({"docs/standards/naming.md", "tools/tests/test_naming.py"})
EXCLUDED_PREFIXES = ("src/legacy/", "extern/", "gen/", "docs/audits/")

# The published plugin/module C ABI (src/extension/) and everything that names it. Nothing is added.
FROZEN_NVR = frozenset({
    "NvrGameContext", "NvrHostFlags", "NvrLoadedPluginInfo", "NvrModuleApiVersion",
    "NvrModuleApiVersionSupported", "NvrModuleContext", "NvrModuleGetApiVersion", "NvrModuleHostFlags",
    "NvrModuleInit", "NvrModuleOnFrame", "NvrModuleOnGameStateChange", "NvrModuleShutdown",
    "NvrPluginCapabilities", "NvrPluginGetApiVersion", "NvrPluginGetCapabilities", "NvrPluginGetInfo",
    "NvrPluginInfo", "NvrPluginInit", "NvrPluginInitEx", "NvrPluginOnFrame", "NvrPluginOnGameStateChange",
    "NvrPluginShutdown",
    # Not ABI symbols: a wildcard in a comment, the prose name of the lifecycle, and the exports of the
    # test plugin DLL (src/runtime/tests/plugin_onframe_test_dll.cpp).
    "NvrPlugin", "NvrPluginInterface", "NvrTestPluginGetFrameCount", "NvrTestPluginGetInitCount",
    "NvrTestPluginGetKeptInfo",
})
# Existing non-canonical spellings, per file and exact token. The map only shrinks.
LEGACY_SPELLINGS = {
    "README.md": {"nEVR"},
    "certs/code-signing.conf": {"nEVR"},
    "certs/generate-ca.sh": {"nEVR"},
    "certs/intermediate-ca.conf": {"nEVR"},
    "certs/root-ca.conf": {"nEVR"},
    "cmake/codesign/sign.sh": {"nEVR"},
    "plugins/common/include/address_registry.h": {"nEVR"},
    "plugins/example/README.md": {"nEVR"},
    "plugins/example/src/plugin.cpp": {"nEVR"},
    "src/extension/plugin_interface.h": {"nEVR"},
    "src/runtime/compat/ws_bridge.cpp": {"nEVR"},
    "src/runtime/hook/patching.h": {"NevRUPnPConfig"},
    "src/runtime/lifecycle/initialize.cpp": {"NevRUPnPConfig"},
    "src/runtime/server/gameserver.cpp": {"NevRUPnPConfig"},
    "src/runtime/server/gameserver_callbacks.cpp": {"NevRUPnPConfig"},
    "src/runtime/server/gameserver_internal.h": {"NevRUPnPConfig"},
    "src/runtime/server/gameserver_serverdb.cpp": {"NevRUPnPConfig"},
}


def project_files():
    """Tracked and untracked-but-not-ignored text files, so a new file is checked before it is added."""
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         check=True, capture_output=True, text=True).stdout
    for path in (p for p in out.split("\0") if p):
        if path.startswith(EXCLUDED_PREFIXES) or path in SELF:
            continue
        try:
            yield path, (REPO / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue


def noncanonical(text):
    return {m.group() for m in ANY_CASE.finditer(text) if not CANONICAL.match(m.group())}


class NamingTest(unittest.TestCase):
    def test_the_scan_sees_the_tree(self):
        files = dict(project_files())
        self.assertGreater(len(files), 300, "the sensor is reading almost nothing")
        self.assertIn("src/extension/plugin_interface.h", files)

    def test_no_nvr_identifier_outside_the_frozen_set(self):
        stray = {}
        for path, text in project_files():
            for token in {re.sub(r"(_fn|_t)$", "", t) for t in NVR.findall(text)} - FROZEN_NVR:
                stray.setdefault(token, []).append(path)
        self.assertEqual(stray, {}, "`Nvr` is the frozen plugin/module ABI spelling; new names use Nevr/NEVR_ "
                         "(docs/standards/naming.md)")

    def test_every_frozen_nvr_name_is_still_used(self):
        used = set()
        for _, text in project_files():
            used |= {re.sub(r"(_fn|_t)$", "", t) for t in NVR.findall(text)}
        self.assertEqual(sorted(FROZEN_NVR - used), [], "remove names that no longer exist from FROZEN_NVR")

    def test_no_new_noncanonical_spelling(self):
        offenders = {}
        for path, text in project_files():
            extra = noncanonical(text) - LEGACY_SPELLINGS.get(path, set())
            if extra:
                offenders[path] = sorted(extra)
        self.assertEqual(offenders, {}, "use NEVR, Nevr or nevr (docs/standards/naming.md)")

    def test_every_legacy_spelling_is_still_present(self):
        files = dict(project_files())
        gone = {p: sorted(tokens - noncanonical(files.get(p, ""))) for p, tokens in LEGACY_SPELLINGS.items()
                if tokens - noncanonical(files.get(p, ""))}
        self.assertEqual(gone, {}, "remove cleaned entries from LEGACY_SPELLINGS")


if __name__ == "__main__":
    unittest.main()
