"""#1774: the wiring of the loading-tip hooks, which the host GTest (test_loading_tips) cannot reach.

The GTest drives src/runtime/patch/loading_tips.h over a hand-assembled image. These checks read the places the
installer is CALLED from, the byte patch it replaced (which must be gone), the three detours' first statements and
the liveness rows. They are source-shape checks (evidence ladder rank 3): they say nothing about whether a
server run reaches the loading-tip nodes. Only a server run shows that (`[NEVR.PATCH] loading tip node skipped`).
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_runtime_lifecycle_invariants import extract_braced_function, strip_comments  # noqa: E402


def read(rel):
    return (ROOT / rel).read_text()


class BootWiringTest(unittest.TestCase):
    def test_the_installer_is_called_in_the_server_block_and_the_byte_patch_is_not(self):
        boot = strip_comments(read("src/runtime/lifecycle/boot.cpp"))
        self.assertRegex(
            boot,
            r"if \(g_isServer\) \{\s*PatchEnableServer\(\);\s*"
            r"nevr_loading_tips::Install\(reinterpret_cast<std::uintptr_t>\(EchoVR::g_GameBaseAddress\)\);")
        self.assertNotIn("PatchDisableLoadingTips", boot)

    def test_the_byte_patch_is_gone_from_mode_patches(self):
        for rel in ("src/runtime/patch/mode_patches.cpp", "src/runtime/patch/mode_patches.h"):
            text = strip_comments(read(rel))
            self.assertNotIn("PatchDisableLoadingTips", text, rel)
            self.assertNotRegex(text, r"LOADING_TIP_(PICK|SELECT|SELECT_2)\b", rel)

    def test_addresses_name_the_three_entries_and_the_removed_sizes_are_gone(self):
        addresses = strip_comments(read("src/runtime/hook/addresses.h"))
        self.assertRegex(addresses, r"constexpr uintptr_t LOADING_TIP_PICK_ENTER = 0xBD9670;")
        self.assertRegex(addresses, r"constexpr uintptr_t LOADING_TIP_SELECT_BY_INDEX_ENTER = 0xBE6D10;")
        self.assertRegex(addresses, r"constexpr uintptr_t LOADING_TIP_SELECT_ENTER = 0xBE7C90;")
        self.assertNotIn("LOADING_TIP_PICK_SIZE", addresses)
        self.assertNotRegex(addresses, r"LOADING_TIP_(PICK|SELECT|SELECT_2)\b")


class DetourTest(unittest.TestCase):
    def setUp(self):
        self.cpp = strip_comments(read("src/runtime/patch/loading_tips.cpp"))

    def test_each_detour_marks_its_liveness_id_as_its_first_statement(self):
        for detour, liveness, site in (
            ("PickDetour", "kLoadingTipPick", "0"),
            ("SelectByIndexDetour", "kLoadingTipSelectByIndex", "1"),
            ("SelectDetour", "kLoadingTipSelect", "2"),
        ):
            pattern = (r"void " + detour + r"\(void\* node,\s*void\*\s*\)\s*\{\s*"
                       r"nevr_hook_liveness::Mark\(nevr_hook_liveness::" + liveness + r"\);\s*"
                       r"Skipped\(" + site + r", node\);\s*\}")
            self.assertRegex(self.cpp, pattern, detour)

    def test_the_detour_table_is_in_site_order(self):
        self.assertRegex(self.cpp, r"kDetours\[kSiteCount\]\s*=\s*\{&PickDetour, &SelectByIndexDetour, &SelectDetour\};")

    def test_the_aggregate_line_keeps_the_text_the_smoke_rows_match(self):
        # tests/smoke/score-log.sh S03 and docs/testing/smoke-checklist.md SV03 match this phrase.
        self.assertIn("Disabled loading tips system for server mode hooks=%zu/%zu failed=%zu", self.cpp)

    def test_each_site_row_names_its_entry_and_holds_sixteen_prologue_bytes(self):
        rows = re.findall(r"\{\"([^\"]+)\",\s*(LOADING_TIP_\w+),\s*\{([^}]*)\}\}", self.cpp)
        self.assertEqual([name for name, _, _ in rows], [
            "CR15PickLoadingTipNode::Enter",
            "CR15SelectLoadingTipByIndexNode::Enter",
            "CR15SelectLoadingTipNode::Enter",
        ])
        self.assertEqual([rva for _, rva, _ in rows],
                         ["LOADING_TIP_PICK_ENTER", "LOADING_TIP_SELECT_BY_INDEX_ENTER", "LOADING_TIP_SELECT_ENTER"])
        for name, _, prologue in rows:
            self.assertEqual(len(re.findall(r"0x[0-9A-Fa-f]{2}", prologue)), 16, name)

    def test_the_stock_install_is_the_only_writer_and_refuses_a_mismatch(self):
        self.assertIn("if (!nevr::ValidatePrologue(target, site.prologue.data(), site.prologue.size()))", self.cpp)
        self.assertIn("PatchDetour(&original, reinterpret_cast<void*>(kDetours[i]), site.name)", self.cpp)
        self.assertNotRegex(self.cpp, r"ApplyPatch\(|ProcessMemcpy\(")


class LivenessRowTest(unittest.TestCase):
    def test_the_three_ids_follow_the_last_existing_id_in_order(self):
        header = strip_comments(read("src/runtime/hook/hook_liveness.h"))
        self.assertRegex(header, r"kOvrGetPerfStats,\s*kLoadingTipPick,\s*kLoadingTipSelectByIndex,\s*"
                                 r"kLoadingTipSelect,\s*kCount")

    def test_the_three_rows_follow_the_same_order_and_none_is_guaranteed_absent(self):
        cpp = strip_comments(read("src/runtime/hook/hook_liveness.cpp"))
        self.assertRegex(
            cpp,
            r'\{"ovr_GetPerfStats", "PCVR session only", false\},\s*'
            r'\{"CR15PickLoadingTipNode::Enter", "server mode, loading screen", false\},\s*'
            r'\{"CR15SelectLoadingTipByIndexNode::Enter", "server mode, loading screen", false\},\s*'
            r'\{"CR15SelectLoadingTipNode::Enter", "server mode, loading screen", false\},')
        self.assertEqual(cpp.count(", true}"), 1)


if __name__ == "__main__":
    unittest.main()
