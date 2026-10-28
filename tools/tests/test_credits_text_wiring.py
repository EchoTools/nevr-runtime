"""#938 / N148: the in-game credits section is wired once, on a client only, and the two byte-patch sites are
named in one place. The byte logic is tested by src/runtime/tests/test_credits_text.cpp and the rendering by
tools/tests/test_credits.py; these are source-order sensors for what neither can see: the call in boot.cpp,
its mode gate and the log for the other mode, the build step that renders the block, and who may name the
sites. Each check takes the text it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
RUNTIME_CMAKE = ROOT / "src/runtime/CMakeLists.txt"
HEADER = ROOT / "src/runtime/patch/credits_text.h"
INSTALL = ROOT / "src/runtime/patch/credits_text.cpp"

CALL = "nevr_credits_text::Install(nevr_credits_block::kText, nevr_credits_block::kSize);"
SERVER_LOG = "[NEVR.PATCH] credits_text skipped reason=server_mode"
SITE_NUMBERS = ("0x973520", "0x973AAF", "0x1C745E0")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def gated_call_problems(boot: str) -> list[str]:
    """The Install call is in the `if (!g_isServer) {` branch of one if/else, and the else logs the skip."""
    code = strip_comments(boot)
    if code.count(CALL) != 1:
        return [f"expected exactly one `{CALL}` in boot.cpp, found {code.count(CALL)}"]
    start = code.index(CALL)
    # Walk back from the call to the `{` that encloses it and read the statement that opens that block.
    depth = 0
    open_at = -1
    for i in range(start - 1, -1, -1):
        if code[i] == "}":
            depth += 1
        elif code[i] == "{":
            if depth == 0:
                open_at = i
                break
            depth -= 1
    if open_at < 0 or not code[:open_at].rstrip().endswith("if (!g_isServer)"):
        return ["the Install call is not directly inside an `if (!g_isServer) {` block"]
    after = code[start + len(CALL):]
    close = re.match(r"\s*\}\s*else\s*\{(.*?)\}", after, flags=re.S)
    if not close:
        return ["the `if (!g_isServer)` block has no else: the server-mode decision is not logged (N122/N124)"]
    if SERVER_LOG not in close.group(1):
        return [f"the else branch does not log `{SERVER_LOG}`"]
    return []


def cmake_problems(cmake: str) -> list[str]:
    code = "\n".join(l for l in cmake.splitlines() if not l.lstrip().startswith("#"))
    problems = []
    if not re.search(r"add_custom_command\(\s*OUTPUT\s+\"\$\{NEVR_CREDITS_BLOCK_CPP\}\"", code):
        problems.append("no add_custom_command producing NEVR_CREDITS_BLOCK_CPP")
    if "--ingame-source" not in code or "tools/render_credits.py" not in code:
        problems.append("the custom command does not run tools/render_credits.py --ingame-source")
    if not re.search(r"DEPENDS[^)]*config/credits\.json", code):
        problems.append("the block does not depend on config/credits.json")
    if not re.search(r"target_sources\(nevr_runtime\s+PRIVATE\s+\"\$\{NEVR_CREDITS_BLOCK_CPP\}\"\)", code):
        problems.append("the generated source is not added to nevr_runtime")
    if '"patch/credits_text.cpp"' not in code:
        problems.append("patch/credits_text.cpp is not in the runtime sources")
    return problems


def site_naming_problems(files: dict[str, str]) -> list[str]:
    """The sites' numbers appear only in credits_text.h (and tests); nothing else names them."""
    problems = []
    for path, text in files.items():
        if path.endswith("credits_text.h") or "/tests/" in path:
            continue
        code = strip_comments(text)
        for number in SITE_NUMBERS:
            if re.search(re.escape(number), code, flags=re.I):
                problems.append(f"{path} names {number}; only runtime/patch/credits_text.h may")
    return problems


def retarget_call_problems(files: dict[str, str]) -> list[str]:
    """Retarget( is called from one place, credits_text.cpp's Install, so the sites are never written twice."""
    callers = []
    for path, text in files.items():
        if "/tests/" in path or path.endswith("credits_text.h"):
            continue
        callers += [path] * len(re.findall(r"\bnevr_credits_text::Retarget\(|[^:\w]Retarget\(", strip_comments(text)))
    if callers != ["src/runtime/patch/credits_text.cpp"]:
        return [f"Retarget( is called from {callers}, expected only src/runtime/patch/credits_text.cpp"]
    return []


def runtime_sources() -> dict[str, str]:
    return {str(p.relative_to(ROOT)): p.read_text(encoding="utf-8", errors="replace")
            for p in sorted((ROOT / "src/runtime").rglob("*")) if p.suffix in {".cpp", ".h"}}


class CreditsWiring(unittest.TestCase):
    def test_the_install_is_called_once_on_a_client_and_the_server_skip_is_logged(self):
        self.assertEqual(gated_call_problems(BOOT.read_text(encoding="utf-8")), [])

    def test_the_gate_check_fails_when_the_call_is_not_gated(self):
        boot = BOOT.read_text(encoding="utf-8")
        ungated = boot.replace("  if (!g_isServer) {\n    " + CALL, "  {\n    " + CALL)
        self.assertNotEqual(ungated, boot)
        self.assertTrue(gated_call_problems(ungated))

    def test_the_gate_check_fails_when_the_server_branch_does_not_log(self):
        boot = BOOT.read_text(encoding="utf-8").replace(SERVER_LOG, "[NEVR.PATCH] something else")
        self.assertTrue(gated_call_problems(boot))

    def test_the_gate_check_fails_when_the_call_is_gone_or_doubled(self):
        boot = BOOT.read_text(encoding="utf-8")
        self.assertTrue(gated_call_problems(boot.replace(CALL, "")))
        self.assertTrue(gated_call_problems(boot + "\n" + CALL + "\n"))

    def test_the_gate_check_ignores_a_call_that_only_a_comment_names(self):
        boot = BOOT.read_text(encoding="utf-8")
        commented = boot.replace(CALL, "// " + CALL)
        self.assertTrue(gated_call_problems(commented), "a commented-out call must not satisfy the check")

    def test_the_build_renders_the_block_and_compiles_it_in(self):
        self.assertEqual(cmake_problems(RUNTIME_CMAKE.read_text(encoding="utf-8")), [])

    def test_the_cmake_check_fails_without_each_piece(self):
        cmake = RUNTIME_CMAKE.read_text(encoding="utf-8")
        removals = (
            "--ingame-source",
            'DEPENDS "${CMAKE_SOURCE_DIR}/config/credits.json" ',
            'target_sources(nevr_runtime PRIVATE "${NEVR_CREDITS_BLOCK_CPP}")',
            '"patch/credits_text.cpp"',
        )
        for needle in removals:
            self.assertIn(needle, cmake, needle)
            self.assertTrue(cmake_problems(cmake.replace(needle, "", 1)), f"removing {needle!r} was not noticed")

    def test_only_the_header_names_the_sites(self):
        self.assertEqual(site_naming_problems(runtime_sources()), [])

    def test_the_site_check_fails_when_another_file_names_one(self):
        files = runtime_sources()
        files["src/runtime/patch/other.cpp"] = "constexpr unsigned kX = 0x973520;"
        self.assertTrue(site_naming_problems(files))
        files["src/runtime/patch/other.cpp"] = "// 0x973520 is named in a comment only"
        self.assertEqual(site_naming_problems(files), [])

    def test_retarget_is_called_from_install_only(self):
        self.assertEqual(retarget_call_problems(runtime_sources()), [])

    def test_the_retarget_check_fails_on_a_second_caller(self):
        files = runtime_sources()
        files["src/runtime/patch/other.cpp"] = "void F() { nevr_credits_text::Retarget(a, b, 0, w); }"
        self.assertTrue(retarget_call_problems(files))

    def test_the_generated_source_is_not_checked_in(self):
        self.assertEqual(list((ROOT / "src").rglob("credits_block.cpp")), [])
        self.assertTrue(HEADER.exists() and INSTALL.exists())


if __name__ == "__main__":
    unittest.main()
