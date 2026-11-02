"""docs/standards/naming.md, enforced: the `Nvr` plugin/module ABI spelling is a frozen set that does not
grow, and a non-canonical spelling of the project name does not appear in any file or identifier that does
not already carry it. Both sets are written out below on purpose: changing one is a visible edit here."""
from __future__ import annotations

import json
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
# The brand is spelled nEVR in prose (docs/standards/naming.md); these project-name phrasings in the prose of a
# document are the uppercase spelling.
UPPERCASE_BRAND_IN_DOCS = re.compile(
    r"(?<![A-Za-z])NEVR(?: Runtime| project|['’]s| refuses| subsystem|-authored)\b"
)
# These files define the anti-patterns themselves.
SELF = frozenset({"docs/standards/naming.md", "tools/tests/test_naming.py"})
EXCLUDED_PREFIXES = ("extern/", "gen/", "docs/audits/")

# The published plugin/module C ABI (sdk/include/nevr/plugin_interface.h, src/extension/module_interface.h) and
# everything that names it. Nothing is added.
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
# Plugins other people publish keep their own names (docs/standards/naming.md, "Names that belong to ... third
# parties"). These are named in config examples and docs because a host lists them in config.yaml. Each is one
# third party's file name; nothing here is ours, and nothing is added for a name we choose.
THIRD_PARTY_NVR = frozenset({"NvrContentOverlay"})
# Namespaces that were renamed to nevr_<area> (#131), one JSON file per area under
# tools/tests/renamed_namespaces/ so area PRs do not edit the same lines. The old name is not used as a
# namespace again; a PascalCase namespace in no file is still on the rename list
# (`python3 tools/naming_inventory.py --category pascal-namespaces`).
RENAMED_NAMESPACES = {}
for _area_file in sorted((REPO / "tools/tests/renamed_namespaces").glob("*.json")):
    RENAMED_NAMESPACES.update(json.loads(_area_file.read_text(encoding="utf-8")))
# Existing non-canonical spellings outside prose, per file and exact token. The map only shrinks.
LEGACY_SPELLINGS = {
    "certs/code-signing.conf": {"nEVR"},
    "certs/generate-ca.sh": {"nEVR"},
    "certs/intermediate-ca.conf": {"nEVR"},
    "certs/root-ca.conf": {"nEVR"},
    "cmake/codesign/sign.sh": {"nEVR"},
}
# Documents and comments whose prose spells the brand `nEVR`, per file and exact token.
BRAND_PROSE_SPELLINGS = {
    "AGENTS.md": {"nEVR"},
    "CONTRIBUTING.md": {"nEVR"},
    "README.md": {"nEVR"},
    "docs/README.md": {"nEVR"},
    "RELEASING.md": {"nEVR"},
    "tools/package-release/RELEASE-BODY.template.md": {"nEVR"},
    "tools/tests/test_package_release.py": {"nEVR"},
    "docs/engine/asset-system.md": {"nEVR"},
    "docs/adr/0003-quest-networking-port.md": {"nEVR"},
    "docs/adr/0004-quest-verification-regime.md": {"nEVR"},
    "docs/adr/0010-signed-plugins-only.md": {"nEVR"},
    "docs/adr/0012-static-install-root.md": {"nEVR"},
    "docs/adr/0028-game-derived-packs-on-the-cdn.md": {"nEVR"},
    "docs/reference/content-pack-hosting.md": {"nEVR"},
    "docs/design/2026-09-21-mic-provider-voip-fix.md": {"nEVR"},
    "docs/reference/example-config.yaml": {"nEVR"},
    "docs/reference/launcher-install-root.md": {"nEVR"},
    "docs/reference/verifying-a-release.md": {"nEVR"},
    "docs/standards/logging.md": {"nEVR"},
    "cmake/nevr_version_info.rc.in": {"nEVR"},  # ProductName and FileDescription are brand prose
    "docs/standards/verification.md": {"nEVR"},
    "src/launcher/CMakeLists.txt": {"nEVR"},  # the exe's FileDescription
    "src/runtime/CMakeLists.txt": {"nEVR"},  # the DLL's FileDescription
    "src/core/build_identity.h": {"nEVR"},  # MenuVersionLabel: the text a player sees
    "src/core/build_identity.cpp": {"nEVR"},  # MenuVersionLabel: the text a player sees
    "src/quest/social/menu_version_label.h": {"nEVR"},  # the Quest menu label: the text a player sees
    "src/quest/social/menu_version_label.cpp": {"nEVR"},  # the Quest menu label: the text a player sees
    "src/runtime/ext/plugin_refusal_notice.h": {"nEVR"},
    "src/runtime/host/stop_notice.h": {"nEVR"},  # the player's text when the runtime did not load
    "src/runtime/host/stop_line.cpp": {"nEVR"},  # the detail an operator reads: "no nEVR root"
    "src/runtime/tests/test_host_stop_line.cpp": {"nEVR"},  # the dialog text the tests check
    "src/runtime/tests/test_host_runtime_verify.cpp": {"nEVR"},  # the dialog's version line
    "src/runtime/tests/rendezvous/rendezvous_test.cpp": {"nEVR"},  # the dialog text the harness reads
    "src/runtime/tests/test_host_rendezvous.cpp": {"nEVR"},  # asserts that text
    "src/quest/tests/menu_version_label_test.cpp": {"nEVR"},  # the label text the Quest menu shows
    "src/runtime/tests/test_menu_version_label.cpp": {"nEVR"},  # the label text the menu shows
    "src/runtime/tests/test_behavioral.cpp": {"nEVR"},  # asserts the refusal dialog's player prose
    "tools/package-release/README.template.txt": {"nEVR"},
    "tools/pe_version_info.py": {"nEVR"},  # the pinned ProductName and FileDescription
    "tools/tests/test_pe_version_info.py": {"nEVR"},  # the same pins, read back from built files
    "tools/package-release/RELEASE-NOTES.template.md": {"nEVR"},
    "tools/winvm/README.md": {"nEVR"},
    "src/core/nevr_root.h": {"nEVR"},  # comment prose: where nEVR keeps its files
    "src/runtime/lifecycle/nevr_root_move.h": {"nEVR"},  # comment prose about the one-time move
    "src/runtime/lifecycle/nevr_root_move.cpp": {"nEVR"},  # comment prose, and the MOVED-TO-NEVR-ROOT.txt text a player reads
    "src/runtime/tests/test_nevr_root.cpp": {"nEVR"},  # comment prose and a failure message
    "tools/tests/test_nevr_root_wiring.py": {"nEVR"},  # docstring prose
    "docs/beta/INSTALL.md": {"nEVR"},  # prose: update nEVR through the launcher
    "installer/src/launcher.rs": {"nEVR"},  # comment prose, and the version text the refusal line shows
    "installer/src/session.rs": {"nEVR"},  # the refusal line a player reads: "nEVR 5.4.3 is installed"
    "installer/src/main.rs": {"nEVR"},  # the usage line and the root refusal an operator reads (#1353)
    "installer/tests/nevr_root.rs": {"nEVR"},  # the refusal reason the test asserts (#1353)
    "tools/package-release/install.ps1": {"nEVR"},  # the header comment on -Root (#1353)
}
# Namespaces the game's own binary uses. src/reconstruction takes each from the binary: it is the qualifier of a
# function the file cites by its game address (`// @0x... — NRadEngine::CMemory::Compare`), or of a game API the file
# calls (`CSysWindow::IsVirtual`). docs/standards/naming.md: names that belong to the game keep their own spelling.
GAME_NAMESPACES = frozenset({
    "CBroadcaster", "CGRenderConfig", "CGS", "CHashTable", "CMatSym", "CMemBlock", "CMemory", "CMemoryContext",
    "CSoundEmitter", "CSoundPlayer", "CSysNet::SAddress", "CSysTime", "CSysWindow", "CTimer", "CVR", "NR15RestAPI",
    "NRadEngine::CGRenderConfig", "NRadEngine::CMemBlock", "NRadEngine::CMemory", "NRadEngine::CSoundEmitter",
    "NRadEngine::CSoundPlayer", "NRadEngine::CSysNet", "NRadEngine::CSysTime", "NRadEngine::NRadGame",
    "NRadEngine::NRadGame::NR15RestAPI", "NRadGame", "NWriteLog", "SBlackboardCD", "SBroadcasterData",
    "SComponentNodeUtils",
})
# The last component of a namespace is `nevr` or `nevr_<area>`; a game namespace may be the prefix before it, as in
# `NRadEngine::nevr_reconstruction`.
NEVR_LAST_COMPONENT = re.compile(r"nevr(?:_[a-z][a-z0-9_]*)?")


def nevr_named(name):
    """True when the namespace's last component is `nevr` or `nevr_<area>` (docs/standards/naming.md)."""
    return NEVR_LAST_COMPONENT.fullmatch(name.rsplit("::", 1)[-1]) is not None


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
        self.assertIn("sdk/include/nevr/plugin_interface.h", files)

    def test_no_nvr_identifier_outside_the_frozen_set(self):
        stray = {}
        for path, text in project_files():
            for token in {re.sub(r"(_fn|_t)$", "", t) for t in NVR.findall(text)} - FROZEN_NVR - THIRD_PARTY_NVR:
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
            extra = noncanonical(text) - LEGACY_SPELLINGS.get(path, set()) - BRAND_PROSE_SPELLINGS.get(path, set())
            if extra:
                offenders[path] = sorted(extra)
        self.assertEqual(offenders, {}, "use NEVR, Nevr or nevr in a name, and nEVR in prose (docs/standards/naming.md)")

    def test_one_spelling_of_the_lifecycle_namespace(self):
        # The runtime's lifecycle code is in `nevr::lifecycle`; the two older spellings were unified (#131).
        old = re.compile(r"nevr_runtime::lifecycle|Nevr::Lifecycle|nevr::Lifecycle")
        found = {path: sorted(set(old.findall(text))) for path, text in project_files() if old.search(text)}
        self.assertEqual(found, {}, "the lifecycle namespace is nevr::lifecycle (docs/standards/naming.md)")

    def test_cmake_targets_carry_the_prefix(self):
        # A library or executable target is nevr_<name> (#131); the output file name is OUTPUT_NAME and does
        # not move. These four are a file name users touch or a third party's name.
        exceptions = {"echovr_server", "LibOVRPlatform64_1", "ovrplatformloader", "breakpad_client"}
        add = re.compile(r"^\s*add_(?:library|executable)\(\s*([A-Za-z0-9_]+)", re.M)
        stray = {}
        for path, text in project_files():
            if not (path.endswith("CMakeLists.txt") or path.endswith(".cmake")):
                continue
            names = {n for n in add.findall(text)
                     if not n.startswith(("nevr", "test_")) and not n.endswith(("_test", "_probe", "_probe_", "_test_hooks"))}
            names -= exceptions
            if names:
                stray[path] = sorted(names)
        self.assertEqual(stray, {}, "CMake targets are nevr_<name> (docs/standards/naming.md)")

    def test_project_macros_carry_the_prefix(self):
        # The build identity and the hook selector are NEVR_ macros (#131). CMake VARIABLES of the same name
        # (`${PROJECT_VERSION}`, `set(GIT_DESCRIBE ...)`) are CMake's own and stay.
        macro = re.compile(r"(?<![A-Za-z0-9_${])(USE_MINHOOK|PROJECT_VERSION|GIT_COMMIT_HASH|GIT_DESCRIBE)(?![A-Za-z0-9_}])")
        stray = {}
        for path, text in project_files():
            if not path.endswith((".h", ".hpp", ".cpp", ".cc", ".inc")):
                continue
            found = sorted({m.group(1) for m in macro.finditer(text)})
            if found:
                stray[path] = found
        self.assertEqual(stray, {}, "project macros are NEVR_<NAME> (docs/standards/naming.md)")

    def test_renamed_namespaces_do_not_come_back(self):
        def in_string(line, pos):
            quotes, i = 0, 0
            while i < pos:
                if line[i] == "\\":
                    i += 2
                    continue
                quotes += line[i] == '"'
                i += 1
            return quotes % 2 == 1

        # One pattern for every old name, tried only on lines that hold `::` or `namespace`.
        olds = "|".join(re.escape(old) for old in RENAMED_NAMESPACES)
        pattern = re.compile(r"(?<![A-Za-z0-9_:])(" + olds + r")::|\bnamespace\s+(" + olds + r")\b")
        stray = {}
        for path, text in project_files():
            if not path.endswith((".h", ".hpp", ".cpp", ".cc", ".inc")):
                continue
            for n, line in enumerate(text.split("\n"), start=1):
                if "::" not in line and "namespace" not in line:
                    continue
                for m in pattern.finditer(line):
                    if not in_string(line, m.start()):
                        stray.setdefault(path, []).append("%d:%s" % (n, m.group(1) or m.group(2)))
        self.assertEqual(stray, {}, "these namespaces are nevr_<area> now (docs/standards/naming.md)")

    def test_every_namespace_is_nevr_or_the_games(self):
        # The rule is `nevr` or `nevr_<area>` as the last component (#131). The game's own namespaces are
        # GAME_NAMESPACES; the two PascalCase namespaces left are named here with the reason; a new one fails this
        # test, and an entry whose namespace is gone fails it too.
        exceptions = {
            # The hooking abstraction every patch calls; the name predates the rule.
            "src/core/hooking.h": {"Hooking"},
            # Nested in the game's EchoVR namespace and named after the game's own symbol tables.
            "src/abi/symbols.h": {"EchoVR::Symbols", "Social", "Tcp"},
        }
        decl = re.compile(r"^\s*namespace\s+([A-Z][A-Za-z0-9_]*(?:::[A-Za-z0-9_]+)*)\s*\{")
        found = {}
        for path, text in project_files():
            if not path.endswith((".h", ".hpp", ".cpp", ".cc", ".inc")) or path.startswith("plugins/"):
                continue
            for line in text.split("\n"):
                m = decl.match(line)
                if m and m.group(1) not in {"EchoVR", "NRadEngine"}:
                    found.setdefault(path, set()).add(m.group(1))
        declared = set().union(*found.values())
        self.assertEqual(GAME_NAMESPACES - declared, set(),
                         "GAME_NAMESPACES names a namespace the tree no longer declares")
        found = {path: {n for n in names - GAME_NAMESPACES if not nevr_named(n)} for path, names in found.items()}
        found = {path: names for path, names in found.items() if names}
        self.assertEqual(found, exceptions, "namespaces are nevr or nevr_<area> (docs/standards/naming.md)")

    def test_every_legacy_spelling_is_still_present(self):
        files = dict(project_files())
        gone = {p: sorted(tokens - noncanonical(files.get(p, ""))) for p, tokens in LEGACY_SPELLINGS.items()
                if tokens - noncanonical(files.get(p, ""))}
        self.assertEqual(gone, {}, "remove cleaned entries from LEGACY_SPELLINGS")

    def test_every_brand_prose_spelling_is_still_present(self):
        files = dict(project_files())
        gone = {p: sorted(tokens - noncanonical(files.get(p, ""))) for p, tokens in BRAND_PROSE_SPELLINGS.items()
                if tokens - noncanonical(files.get(p, ""))}
        self.assertEqual(gone, {}, "remove cleaned entries from BRAND_PROSE_SPELLINGS")

    def test_project_brand_spelling_in_docs(self):
        offenders = {}
        for path, text in project_files():
            if not path.endswith((".md", ".yaml", ".yml")):
                continue
            matches = sorted({m.group() for m in UPPERCASE_BRAND_IN_DOCS.finditer(text)})
            if matches:
                offenders[path] = matches
        self.assertEqual(offenders, {}, "spell the project brand nEVR in prose (docs/standards/naming.md)")


if __name__ == "__main__":
    unittest.main()
