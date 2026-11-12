"""#2189: the reconstruction sources written for the Quest gun and drone call surface carry no name of the deleted q15t plugin.

docs/standards/naming.md: a namespace the reconstruction wrote is `NRadEngine::nevr_reconstruction` (the game's prefix, then
`nevr_<area>`). Sixteen files under src/reconstruction/r15 opened `q15t::r15::reconstruction` instead (the plugin's name, a PC/Quest
word the namespace rule has no place for). They include each other through cnca_game.hpp, so a file left on the old namespace
would not see the shared types: the library would stop building, or it would build with the types declared twice.
"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAMESPACE = "NRadEngine::nevr_reconstruction"
FILES = [
    "cactor_alias_cs_resolve.cpp",
    "clocal_component_handle_tag_find.cpp",
    "cnca_game.hpp",
    "cnca_game_loaded_space.cpp",
    "cnca_game_space.cpp",
    "cnca_game_space_wrapper.cpp",
    "cr15_echo_unit_base_transform.cpp",
    "cr15_game_space_mt.cpp",
    "cr15_net_follow_player_cs_follow.cpp",
    "cr15_net_game_user_actor.cpp",
    "cr15_player_nav_final_transform.cpp",
    "cr15_player_nav_is_component_valid.cpp",
    "cr15_resolve_actor_alias.cpp",
    "ctag_cs_has_tag.cpp",
    "ctransfqs_append.cpp",
    "fill_resource_file_request.cpp",
]
OPEN = re.compile(r"^namespace\s+([A-Za-z0-9_:]+)\s*\{\s*$", re.MULTILINE)
CLOSE = re.compile(r"^\}\s*//\s*namespace\s+([A-Za-z0-9_:]+)\s*$", re.MULTILINE)


def tracked_reconstruction_sources() -> list[Path]:
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z", "--", "src/reconstruction"], check=True,
                         capture_output=True).stdout
    return [ROOT / p for p in out.decode().split("\0") if p.endswith((".cpp", ".hpp", ".h"))]


class ReconstructionNamespace(unittest.TestCase):
    def test_the_sixteen_files_exist(self):
        for name in FILES:
            self.assertTrue((ROOT / "src/reconstruction/r15" / name).is_file(), name)

    def test_each_file_opens_and_closes_the_one_namespace_once(self):
        for name in FILES:
            text = (ROOT / "src/reconstruction/r15" / name).read_text(encoding="utf-8")
            self.assertEqual(OPEN.findall(text).count(NAMESPACE), 1, f"{name}: one `namespace {NAMESPACE} {{`")
            self.assertEqual(CLOSE.findall(text).count(NAMESPACE), 1, f"{name}: one `}} // namespace {NAMESPACE}`")

    def test_no_reconstruction_source_names_the_old_namespace_or_a_q15t_namespace(self):
        sources = tracked_reconstruction_sources()
        self.assertGreater(len(sources), len(FILES))
        for path in sources:
            text = path.read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"q15t::", text), f"{path.relative_to(ROOT)} still names a q15t namespace")
            for name in OPEN.findall(text) + CLOSE.findall(text):
                self.assertNotRegex(name, r"(?i)q15t", f"{path.relative_to(ROOT)}: namespace {name}")
                self.assertNotIn("r15::reconstruction", name, f"{path.relative_to(ROOT)}: namespace {name}")


if __name__ == "__main__":
    unittest.main()
