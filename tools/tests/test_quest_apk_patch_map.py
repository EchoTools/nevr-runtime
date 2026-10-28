"""tools/quest_apk_patch_map.py (#1187): every change the community patch repo applies to the Quest APK, mapped.

The real table must pass on its own; hand-made tables show each row rule fails; a hand-made patch repo (a git
checkout with YAMLs, a script and ELF files carrying build IDs) and a hand-made tree show each --repo rule fails.
"""

import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "quest_apk_patch_map.py"
sys.path.insert(0, str(REPO / "tools"))
import quest_apk_patch_map as patch_map  # noqa: E402

PIN_LINE = "#@ echoApkPatch " + "a" * 40
HOOK = "src/quest/sentinel/os_build_gate_hook.cpp:Handler"


def row(source="configs/libr15.so.yaml", site="0x1085260", feature="F", change="ret", status="done", ours=HOOK,
        reason="why") -> str:
    return "\t".join([source, site, feature, change, status, ours, reason])


def scratch(test: unittest.TestCase, prefix: str) -> Path:
    directory = Path(tempfile.mkdtemp(prefix=prefix))
    test.addCleanup(shutil.rmtree, directory)
    return directory


class TableTest(unittest.TestCase):
    def failures(self, *lines: str, pin: str = PIN_LINE) -> list:
        path = scratch(self, "apk-patch-map-") / "map.tsv"
        path.write_text("# header\n" + pin + "\n" + "".join(line + "\n" for line in lines), encoding="utf-8")
        return patch_map.check_rows(*patch_map.read_table(path), REPO)

    def test_the_real_table_passes(self):
        result = subprocess.run([sys.executable, str(TOOL)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("OK 117 rows", result.stdout)

    def test_the_real_table_has_every_libr15_site(self):
        _, rows = patch_map.read_table(patch_map.TABLE)
        libr15 = [r for r in rows if r["source"] == "configs/libr15.so.yaml"]
        self.assertEqual(sum(patch_map.site_address(r["site"]) is not None for r in libr15), 73)
        self.assertEqual(sum(r["site"] == "feature" for r in libr15), 3)

    def test_a_row_of_each_status_passes(self):
        self.assertEqual(self.failures(
            row(), row(site="0x1", status="not_needed", ours="-"), row(site="0x2", status="open", ours="#947"),
            row(source="configs/libpnsovr.so.yaml", site="file", status="table", ours="tools/quest_patcher_sites.tsv"),
            row(source="a.sh", site="@function x", status="done", ours=HOOK + ",#945"),
            row(site="feature", status="not_needed", ours="-")), [])

    def test_done_needs_our_code(self):
        self.assertIn("names no <path>:<symbol>", self.failures(row(ours="-"))[0])
        self.assertIn("names no <path>:<symbol>", self.failures(row(ours="#945"))[0])

    def test_open_needs_an_issue(self):
        self.assertIn("names no issue", self.failures(row(status="open", ours="-"))[0])
        self.assertIn("names no issue", self.failures(row(status="open", ours=HOOK))[0])

    def test_table_names_the_patcher_sites_table(self):
        self.assertIn("nothing else", self.failures(row(status="table", ours="tools/other.tsv"))[0])
        self.assertIn("nothing else", self.failures(row(status="table", ours="tools/quest_patcher_sites.tsv,#1"))[0])

    def test_an_unknown_status_fails(self):
        self.assertIn("is not one of", self.failures(row(status="maybe"))[0])

    def test_a_hook_that_is_not_in_the_tree_fails(self):
        self.assertIn("not found", self.failures(row(ours="src/quest/sentinel/os_build_gate_hook.cpp:NoSuch"))[0])
        self.assertIn("existing", self.failures(row(ours="src/quest/no_such_file.cpp:Handler"))[0])
        self.assertIn("existing", self.failures(row(ours="src/quest/sentinel/os_build_gate_hook.cpp"))[0])
        # A symbol that is only part of a longer name does not count, at either end.
        self.assertIn("not found", self.failures(row(ours="src/quest/sentinel/os_build_gate_hook.cpp:Handl"))[0])
        self.assertIn("not found", self.failures(row(ours="src/quest/sentinel/os_build_gate_hook.cpp:andler"))[0])

    def test_every_item_of_ours_is_checked(self):
        self.assertEqual(len(self.failures(row(ours=HOOK + ",src/quest/sentinel/os_build_gate_hook.cpp:Missing"))), 1)

    def test_an_empty_reason_or_ours_fails(self):
        self.assertIn("empty reason", self.failures(row(reason="-"))[0])
        self.assertIn("empty ours", self.failures(row(ours=""))[0])
        self.assertIn("empty change", self.failures(row(change=""))[0])

    def test_a_site_listed_twice_fails(self):
        self.assertIn("already at line", self.failures(row(), row(site="0x01085260"))[0])
        self.assertEqual(self.failures(row(site="feature", feature="A", status="not_needed", ours="-"),
                                       row(site="feature", feature="B", status="not_needed", ours="-")), [])

    def test_a_short_row_fails(self):
        self.assertIn("fields", self.failures("configs/libr15.so.yaml\t0x1")[0])

    def test_a_table_without_the_pin_fails(self):
        self.assertIn("pins the patch repo", self.failures(row(), pin="# no pin")[0])

    def test_a_table_row_needs_the_patcher_sites_table_in_the_tree(self):
        path = scratch(self, "apk-patch-map-") / "map.tsv"
        path.write_text(PIN_LINE + "\n" + row(status="table", ours="tools/quest_patcher_sites.tsv") + "\n")
        failures = patch_map.check_rows(*patch_map.read_table(path), scratch(self, "apk-patch-root-"))
        self.assertEqual(1, len(failures))
        self.assertIn("is not in the tree", failures[0])

    def test_the_tool_exits_1_on_a_failing_table(self):
        path = scratch(self, "apk-patch-map-") / "map.tsv"
        path.write_text(PIN_LINE + "\n" + row(ours="-") + "\n")
        result = subprocess.run([sys.executable, str(TOOL), "--table", str(path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("quest_apk_patch_map: FAIL line 2: done but names no", result.stdout)


def elf_with_build_id(build_id: str) -> bytes:
    """A 64-bit little-endian ELF with one SHT_NOTE section holding an NT_GNU_BUILD_ID note."""
    desc = bytes.fromhex(build_id)
    note = struct.pack("<III", 4, len(desc), 3) + b"GNU\0" + desc
    note_at = 0x40
    shoff = note_at + len(note) + (-len(note) % 8)
    header = bytearray(0x40)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<HH", header, 0x3A, 64, 2)
    section = bytearray(64)
    struct.pack_into("<I", section, 4, 7)
    struct.pack_into("<QQ", section, 0x18, note_at, len(note))
    return bytes(header) + note + bytes(shoff - note_at - len(note)) + bytes(64) + bytes(section)


R15_ID, MM_ID, OVR_ID = "11" * 20, "22" * 20, "33" * 20
LIBR15_YAML = """- feature_name: "On"
  enabled: true
  patches:
    - address: 0x01000010
      type: instruction
    - address: 0x01000020
      type: instruction

- feature_name: "Off"
  enabled: false
  patches:
    - address: 0x01000030
      type: instruction
"""


class RepoTest(unittest.TestCase):
    def setUp(self):
        self.patch_repo = scratch(self, "apk-patch-repo-")
        self.root = scratch(self, "apk-patch-root-")
        files = {
            "configs/libr15.so.yaml": LIBR15_YAML,
            "configs/libpnsovr.so.yaml": '- feature_name: "Ovr"\n  enabled: true\n  patches:\n    - address: 0x2ece7c\n',
            "patcher/license_patch.yaml": '- feature_name: "Lic"\n  enabled: true\n  patches:\n    - address: 0x2ed000\n',
            "pack.sh": "function stage_x() {\n  echo 0x0330ae50\n}\n",
        }
        for path, text in files.items():
            (self.patch_repo / path).parent.mkdir(parents=True, exist_ok=True)
            (self.patch_repo / path).write_text(text, encoding="utf-8")
        (self.patch_repo / "binary/source").mkdir(parents=True)
        for name, build_id in (("libr15.so", R15_ID), ("libpnsradmatchmaking.so", MM_ID), ("libpnsovr.so", OVR_ID)):
            (self.patch_repo / "binary/source" / name).write_bytes(elf_with_build_id(build_id))
        self.commit()
        headers = {
            "src/quest/sentinel/pinned_targets.h":
                f'inline constexpr const char* kLibR15BuildId = "{R15_ID}";\n'
                f'inline constexpr const char* kMatchmakingBuildId = "{MM_ID}";\n',
            "src/quest/login/login_prerequisite_targets.h":
                f'inline constexpr const char* kPnsovrBuildId = "{OVR_ID}";\n',
            "src/hook.cpp": "void Handler() {}\n",
            "tools/quest_patcher_sites.tsv": "# header\nlibpnsovr.so.yaml:1\t0x2ece7c\nlicense_patch.yaml:1\t0x2ed000\n",
        }
        for path, text in headers.items():
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_text(text, encoding="utf-8")
        self.rows = [
            row(site="0x1000010", feature="On", ours="src/hook.cpp:Handler"),
            row(site="0x1000020", feature="On", status="open", ours="#1"),
            row(site="feature", feature="Off", status="not_needed", ours="-"),
            row(source="configs/libpnsovr.so.yaml", site="file", status="table", ours="tools/quest_patcher_sites.tsv"),
            row(source="patcher/license_patch.yaml", site="file", status="table",
                ours="tools/quest_patcher_sites.tsv"),
            row(source="pack.sh", site="@function stage_x", status="open", ours="#2"),
            row(source="pack.sh", site="0x0330ae50", status="open", ours="#3"),
        ] + [row(source=f"binary/source/{n}", site="-", status="not_needed", ours="-")
             for n in ("libr15.so", "libpnsradmatchmaking.so", "libpnsovr.so")]

    def commit(self):
        git = ["git", "-C", str(self.patch_repo)]
        if not (self.patch_repo / ".git").exists():
            subprocess.run(git + ["init", "-q"], check=True)
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", "commit",
                              "-q", "--allow-empty", "-m", "t"], check=True)
        self.head = subprocess.run(git + ["rev-parse", "HEAD"], check=True, capture_output=True,
                                   text=True).stdout.strip()

    def failures(self, rows=None, pin=None) -> list:
        path = scratch(self, "apk-patch-map-") / "map.tsv"
        lines = self.rows if rows is None else rows
        path.write_text(f"#@ echoApkPatch {pin or self.head}\n" + "".join(line + "\n" for line in lines),
                        encoding="utf-8")
        table_pin, table_rows = patch_map.read_table(path)
        return patch_map.check_rows(table_pin, table_rows, self.root) + patch_map.check_repo(
            table_pin, table_rows, self.patch_repo, self.root)

    def test_a_complete_table_passes(self):
        self.assertEqual(self.failures(), [])

    def test_another_commit_fails(self):
        self.assertIn("the table was read at", self.failures(pin="b" * 40)[0])

    def test_a_tracked_file_with_no_row_fails(self):
        (self.patch_repo / "new.py").write_text("print(1)\n")
        self.commit()
        self.assertEqual(["new.py is tracked in the patch repo and is the source of no row"], self.failures())

    def test_a_row_for_an_untracked_file_fails(self):
        failures = self.failures(self.rows + [row(source="gone.sh", site="-", status="not_needed", ours="-")])
        self.assertEqual(["gone.sh is the source of a row and is not tracked in the patch repo"], failures)

    def test_an_enabled_address_with_no_row_fails(self):
        failures = self.failures(self.rows[1:])
        self.assertEqual(1, len(failures))
        self.assertIn("0x1000010 (On) has no row", failures[0])

    def test_a_row_with_the_wrong_feature_fails(self):
        rows = [row(site="0x1000010", feature="Other", ours="src/hook.cpp:Handler")] + self.rows[1:]
        self.assertIn("is in feature 'On'", self.failures(rows)[0])

    def test_an_address_the_yaml_does_not_apply_fails(self):
        rows = self.rows + [row(site="0x1000030", feature="Off", ours="src/hook.cpp:Handler")]
        self.assertIn("applies no 0x1000030", self.failures(rows)[0])

    def test_a_disabled_feature_needs_a_not_needed_row(self):
        rows = [r for r in self.rows if "\tfeature\t" not in r]
        self.assertIn("disabled feature 'Off' has no row", self.failures(rows)[0])
        rows.append(row(site="feature", feature="Off", status="open", ours="#4"))
        self.assertIn("want not_needed", self.failures(rows)[0])
        stale = self.rows + [row(site="feature", feature="Gone", status="not_needed", ours="-")]
        self.assertIn("no disabled feature 'Gone'", self.failures(stale)[0])

    def test_a_libpnsovr_address_outside_the_patcher_sites_table_fails(self):
        (self.root / "tools/quest_patcher_sites.tsv").write_text("libpnsovr.so.yaml:1\t0x2ece7c\n")
        self.assertEqual(["patcher/license_patch.yaml: 0x2ed000 is not a row of tools/quest_patcher_sites.tsv"],
                         self.failures())

    def test_a_script_site_must_be_in_the_script(self):
        rows = self.rows + [row(source="pack.sh", site="@function stage_y", status="open", ours="#5")]
        self.assertIn("'function stage_y' is not in pack.sh", self.failures(rows)[0])
        rows = self.rows + [row(source="pack.sh", site="0x0330ae54", status="open", ours="#5")]
        self.assertIn("pack.sh does not write 0x0330ae54", self.failures(rows)[0])

    def test_an_applied_yaml_with_no_feature_or_no_enabled_line_fails(self):
        (self.patch_repo / "configs/libx.so.yaml").write_text("# nothing\n")
        (self.patch_repo / "configs/liby.so.yaml").write_text('- feature_name: "Y"\n  patches:\n')
        self.commit()
        rows = self.rows + [row(source=f"configs/{n}.so.yaml", site="-", status="not_needed", ours="-")
                            for n in ("libx", "liby")]
        self.assertEqual(["configs/libx.so.yaml: no feature read", "configs/liby.so.yaml: feature 'Y' has no enabled line"],
                         self.failures(rows))

    def test_a_pinned_build_id_missing_from_our_header_fails(self):
        (self.root / "src/quest/login/login_prerequisite_targets.h").write_text("// none\n")
        self.assertEqual(["kPnsovrBuildId not found in src/quest/login/login_prerequisite_targets.h"], self.failures())

    def test_a_stock_library_of_another_build_fails(self):
        (self.patch_repo / "binary/source/libr15.so").write_bytes(elf_with_build_id("44" * 20))
        self.commit()
        failures = self.failures()
        self.assertEqual(1, len(failures))
        self.assertIn(f"has build ID {'44' * 20}", failures[0])

    def test_the_build_id_reader_reads_a_real_note(self):
        self.assertEqual(patch_map.gnu_build_id(elf_with_build_id(R15_ID)), R15_ID)
        self.assertIsNone(patch_map.gnu_build_id(b"not an elf"))

    def test_the_recipe_runs_the_repo_check(self):
        justfile = (REPO / "justfile").read_text()
        start = justfile.index("\ntest-quest-apk-patch-map ")
        end = justfile.index("\n\n", start)
        body = [line.strip() for line in justfile[start:end].splitlines() if not line.strip().startswith("#")]
        self.assertIn('python3 tools/quest_apk_patch_map.py --repo "{{ patch_repo }}"', body)


if __name__ == "__main__":
    unittest.main()
