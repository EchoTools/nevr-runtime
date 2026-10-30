"""The hook registry's budget (hook_budget.h) matches the GotHook handles the Quest source declares.

A GotHook handle reserves one registry entry when it installs, so the handles declared in the source are an upper bound on
the entries the build can hold. hook_budget.h lists them per file and a static_assert keeps the sum, plus headroom, inside
the registry (got_hook.h, kRegistryCapacity). This test is the other half: it counts the declarations in the source and
fails when the table and the source differ in either direction, so a hook added without a budget row cannot reach a build
(the hook that would be refused for want of room is the one added last, and the first sign of it would be a game that
cannot find a match).
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"
BACKEND = {"sentinel/got_hook.h", "sentinel/got_hook.cpp"}


def stripped(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def sources():
    for path in sorted(QUEST.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        rel = path.relative_to(QUEST).as_posix()
        if "tests/" in rel or rel.startswith("tests") or rel in BACKEND or "/plugins/" in f"/{rel}":
            continue
        yield rel, path


def constants(text: str) -> dict:
    return {name: int(value) for name, value in re.findall(r"constexpr\s+[\w:\s]+?\b(k\w+)\s*=\s*(\d+)\s*;", text)}


def declarators(text: str):
    """Yield (name, size) for each handle declared by `GotHook a, b[3];`-shaped statements in `text`."""
    for match in re.finditer(r"\bGotHook\s+(?![&*])([^;(){}]*);", text):
        for part in re.split(r",(?![^\[]*\])", match.group(1)):
            part = part.strip()
            if not part:
                continue
            decl = re.fullmatch(r"([\w#]+)\s*(?:\[\s*(\w+)\s*\])?", part)
            if decl is None:
                raise AssertionError(f"cannot read the GotHook declarator {part!r}")
            yield decl.group(1), decl.group(2)


def count_handles() -> dict:
    headers = {}
    for rel, path in sources():
        if path.suffix == ".h":
            headers.update(constants(stripped(path.read_text())))
    counts = {}
    for rel, path in sources():
        text = stripped(path.read_text())
        sizes = {**headers, **constants(text)}  # a constant the file declares wins over a header's of the same name
        # A macro that declares a handle declares it once per use.
        macros = {}
        for define in re.finditer(r"#define\s+(\w+)\([^)]*\)((?:[^\n]*\\\n)*[^\n]*)", text):
            macros[define.group(1)] = define.group(2)
        total = 0
        outside = text
        for name, bodytext in macros.items():
            outside = outside.replace(bodytext, "")
            per_use = sum(1 for _ in declarators(bodytext.replace("\\\n", "\n")))
            uses = len(re.findall(rf"^\s*{name}\(", text, flags=re.M))
            total += per_use * uses
        for _, size in declarators(outside):
            if size is None:
                total += 1
            elif size.isdigit():
                total += int(size)
            else:
                total += sizes[size]
        if total:
            counts[rel] = total
    return counts


def budget() -> dict:
    text = stripped((QUEST / "sentinel/hook_budget.h").read_text())
    rows = {}
    for rel, handles, critical in re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', text):
        assert rel not in rows, f"{rel} has two budget rows"
        rows[rel] = (int(handles), int(critical))
    return rows


class HookBudgetMatchesTheSource(unittest.TestCase):
    def test_every_file_that_declares_handles_has_a_row_with_that_many(self):
        self.assertEqual({rel: handles for rel, (handles, _) in budget().items()}, count_handles())

    def test_the_counter_reads_the_shapes_the_source_uses(self):
        counts = count_handles()
        self.assertEqual(counts["sentinel/crash_first_hook.cpp"], 8)  # 2 + the macro's 2, used three times
        self.assertEqual(counts["login/login_prerequisites_install.cpp"], 22)  # arrays of 4, 8, 4, 4 and singles
        self.assertEqual(counts["diag/hwdump_install.cpp"], 18)  # g_hooks[kHookCount]

    def test_critical_hooks_are_exactly_the_login_and_the_matchmaking_redirect(self):
        critical = {rel for rel, (_, n) in budget().items() if n}
        self.assertEqual(critical, {"login/login_hook.cpp", "login/login_prerequisites_install.cpp", "redirect/hook_adapter.cpp"})
        for rel in critical:
            text = stripped((QUEST / rel).read_text())
            self.assertTrue(re.search(r"\.Critical\(\)|HookPriority::kCritical", text), f"{rel} budgets critical hooks and never asks for the reserve")
        for rel, path in sources():
            if rel in critical:
                continue
            self.assertNotRegex(stripped(path.read_text()), r"\.Critical\(\)|HookPriority::kCritical", f"{rel} takes the reserve without a critical budget row")


if __name__ == "__main__":
    unittest.main()
