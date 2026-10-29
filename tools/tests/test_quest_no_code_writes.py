"""N151 (docs/adr/0021 test 6): the Quest sentinel changes no code page at run time.

Horizon denies execmod on a written libr15 page, so libr15 is patched on the host at build time and the sentinel only stores
DATA into it (atomic stores into the slots of its tail, one anonymous credits buffer, anonymous trampoline pages). This
test pins that by source shape, over src/quest outside the tests, comments stripped:

  * a protection or mapping call (mprotect, mmap, mremap, PROT_EXEC, PROT_WRITE) appears only in the files that need one;
  * the run-time code writers of the old inline hooks (InstallInlineThunk, WriteWord, AllocateInReach) are gone;
  * credits_text.cpp makes one protection call, on its own anonymous buffer, and no other mprotect;
  * the STOCK build ID constant (kLibR15StockBuildId) is named only by the files that need the stock file's identity, so a
    hook target that carries it where it means the patched ID (the mutant class of M16-M19) is caught by name.

The `just verify` sensor N151 runs the same scans in bash. Its allowlists are parsed out of the justfile here and must equal
the lists below, and the sensor block itself is extracted and run against copies of the tree: it must pass on the real tree
and fail on each planted violation.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"
JUSTFILE = ROOT / "justfile"

# Files (relative to src/quest) that may name a memory-protection or mapping call. got_hook.cpp: the GOT's data pages;
# libr15_attach.cpp: its trampoline pages (anonymous memory); credits_text.cpp: its anonymous buffer. The two headers declare
# the seams those files take (their own comments name mprotect and mmap).
PROTECTION_ALLOWLIST = {
    "sentinel/got_hook.cpp",
    "sentinel/libr15_attach.cpp",
    "sentinel/libr15_attach.h",
    "sentinel/credits_text.cpp",
    "sentinel/credits_text.h",
}
# Files that may name the STOCK build ID constant, each with the reason.
STOCK_ID_ALLOWLIST = {
    "diag/hwdump_run.cpp",  # the hardware report names the file it ran on
    "patch/libr15_patch_identity.cpp",  # the patcher's identity derivation starts from the stock ID
    "patch/libr15_patch_table.h",  # the definition
    "patch/libr15_extra_rows.cpp",  # PublicSpec(): the stock file the patcher accepts
    "plugin/plugin_identity.h",  # the ID a plugin is shown on the verified patched file
    "sentinel/credits_text.cpp",  # picks the refusal reason: the stock file is libr15_not_patched
    "sentinel/data_wiring.cpp",  # the game build the downloaded data is for
    "sentinel/libr15_attach.cpp",  # the stock check and the log line that names it
    "sentinel/libr15_identity.cpp",  # the static_assert that ties the two definitions
    "sentinel/pinned_targets.h",  # the sentinel's definition of it
}
FORBIDDEN_WRITERS = r"InstallInlineThunk|\bWriteWord\b|AllocateInReach"
PROTECTION = r"\b(mprotect|mmap|mremap|pkey_mprotect)\b|PROT_(EXEC|WRITE)"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def sources(root: Path = QUEST):
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.suffix in (".cpp", ".h") and "tests/" not in rel and not rel.startswith("tests/"):
            yield rel, strip_comments(path.read_text())


def justfile_list(name: str) -> set:
    match = re.search(rf'^\s*{name}="([^"]*)"', JUSTFILE.read_text(), flags=re.M)
    assert match, f"{name} not found in the justfile"
    return set(match.group(1).split())


def sensor_script() -> str:
    """The `just verify` N151 block with the two plumbing helpers it calls, as a standalone bash script."""
    text = JUSTFILE.read_text()
    helpers = text[text.index("    sensor_stage1() {") : text.index("    # Same class, different shape")]
    nonempty = text[text.index("    sensor_nonempty() {") : text.index("    # SIGPIPE + pipefail")]
    block = text[text.index("    # BEGIN N151") : text.index("    # END N151")]
    return "set -euo pipefail\n" + helpers + nonempty + block + "echo N151-OK\n"


class NoQuestCodeWrites(unittest.TestCase):
    def test_protection_calls_appear_only_in_the_allowlist(self):
        files = dict(sources())
        self.assertGreater(len(files), 50)  # a scan that reads nothing watches nothing
        found = {rel for rel, text in files.items() if re.search(PROTECTION, text)}
        self.assertEqual(found, found & PROTECTION_ALLOWLIST, sorted(found - PROTECTION_ALLOWLIST))
        # The allowlist is not a wish list: each .cpp on it really makes the call it is there for.
        for rel in ("sentinel/got_hook.cpp", "sentinel/libr15_attach.cpp", "sentinel/credits_text.cpp"):
            self.assertIn(rel, found)

    def test_the_run_time_code_writers_are_gone(self):
        for rel, text in sources():
            self.assertIsNone(re.search(FORBIDDEN_WRITERS, text), f"{rel} has a run-time code writer")

    def test_credits_text_makes_one_protection_call_on_its_own_buffer(self):
        text = strip_comments((QUEST / "sentinel/credits_text.cpp").read_text())
        self.assertEqual(len(re.findall(r"\bprotect\(", text)), 1)
        self.assertEqual(len(re.findall(r"\bprotect\(memory,", text)), 1)
        # mprotect appears only as the default of the seam (`&mprotect`), never called.
        self.assertEqual(len(re.findall(r"\bmprotect\b", text)), len(re.findall(r": &mprotect;", text)))
        self.assertIsNone(re.search(r"\bmprotect\s*\(", text))

    def test_the_stock_build_id_is_named_only_by_its_allowlist(self):
        files = dict(sources())
        found = {rel for rel, text in files.items() if "kLibR15StockBuildId" in text}
        self.assertEqual(found, found & STOCK_ID_ALLOWLIST, sorted(found - STOCK_ID_ALLOWLIST))
        # The sentinel's own hook files carry the patched ID, through the pinned_targets.h factories.
        for rel in files:
            if rel.startswith(("social/", "integration/")):
                self.assertNotIn(rel, found)

    def test_every_allowlisted_stock_id_user_exists(self):
        for rel in STOCK_ID_ALLOWLIST | PROTECTION_ALLOWLIST:
            self.assertTrue((QUEST / rel).is_file(), rel)


class TheSensorMatchesThisTest(unittest.TestCase):
    def test_the_justfile_allowlists_equal_this_files(self):
        self.assertEqual(justfile_list("N151_PROT_OK"), PROTECTION_ALLOWLIST)
        self.assertEqual(justfile_list("N151_ID_OK"), STOCK_ID_ALLOWLIST)

    def run_sensor(self, mutate=None):
        """Runs the extracted sensor in a copy of src/quest; `mutate(copy_root)` plants a violation first."""
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            shutil.copytree(QUEST, work / "src/quest")
            if mutate is not None:
                mutate(work / "src/quest")
            script = work / "n149.sh"
            script.write_text(sensor_script())
            return subprocess.run(["bash", str(script)], cwd=work, capture_output=True, text=True)

    def append(self, rel: str, text: str):
        def mutate(root: Path):
            with open(root / rel, "a") as f:
                f.write(text)

        return mutate

    def test_it_passes_on_the_real_tree(self):
        r = self.run_sensor()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("N151-OK", r.stdout)

    def test_it_fails_on_each_planted_violation(self):
        cases = {
            "a PROT_EXEC mprotect in a sentinel file": self.append(
                "sentinel/ui_texture_mips.cpp", "\nvoid Bad(void* p) { mprotect(p, 4096, PROT_READ | PROT_EXEC); }\n"
            ),
            "a writable anonymous map in a social file": self.append(
                "social/social_install.cpp", "\nvoid* Bad() { return mmap(0, 4096, PROT_WRITE, 0, -1, 0); }\n"
            ),
            "a PROT_WRITE mention in an integration file": self.append("integration/stage_log.cpp", "\nint bad = PROT_WRITE;\n"),
            "the inline thunk writer back": self.append("integration/production_steps.cpp", "\nvoid Bad() { InstallInlineThunk(); }\n"),
            "WriteWord back": self.append("social/lobby_full_hooks.cpp", "\nvoid Bad() { WriteWord(0, 0); }\n"),
            "AllocateInReach back": self.append("sentinel/splash_skip_hook.cpp", "\nvoid Bad() { AllocateInReach(0); }\n"),
            "a second protection call in credits_text.cpp": self.append(
                "sentinel/credits_text.cpp", "\nvoid Bad(void* image) { protect(image, 4096, PROT_READ | PROT_EXEC); }\n"
            ),
            "a direct mprotect of an image page in credits_text.cpp": self.append(
                "sentinel/credits_text.cpp", "\nvoid Bad(void* image) { mprotect(image, 4096, 7); }\n"
            ),
            "an mremap in an integration file": self.append(
                "integration/production_steps.cpp", "\nvoid* Bad(void* a) { return mremap(a, 4096, 8192, 0); }\n"
            ),
            "the stock ID in a social hook file": self.append(
                "social/social_install.cpp", "\nconst char* Bad() { return nevr_quest::patch::kLibR15StockBuildId; }\n"
            ),
            "the stock ID in a sentinel hook file": self.append(
                "sentinel/login_prompt_hook.cpp", "\nconst char* Bad() { return sentinel::pinned::kLibR15StockBuildId; }\n"
            ),
        }
        for name, mutate in cases.items():
            with self.subTest(name):
                r = self.run_sensor(mutate)
                self.assertNotEqual(r.returncode, 0, f"the sensor passed on: {name}")
                self.assertIn("N151", r.stderr)

    def test_it_ignores_prose_and_the_allowlisted_files_own_calls(self):
        r = self.run_sensor(
            self.append(
                "social/social_install.cpp",
                "\n// mprotect(p, 4096, PROT_EXEC) and kLibR15StockBuildId are named here in prose only.\n"
                "/*\n * InstallInlineThunk WriteWord AllocateInReach\n */\n",
            )
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_sensor(self.append("sentinel/got_hook.cpp", "\nint Ok(void* p) { return mprotect(p, 4096, PROT_READ | PROT_WRITE); }\n"))
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_it_fails_closed_when_its_subject_is_missing(self):
        def remove(root: Path):
            (root / "sentinel/credits_text.cpp").unlink()

        r = self.run_sensor(remove)
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
