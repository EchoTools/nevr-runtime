"""The host creates its log mutex in one place, under the configured name (docs/adr/0008).

The unit tests look for the mutex with OpenMutexW under a name of their own, so no other process can make them
fail or pass. That finds a creation under that name only. A line that creates a mutex under the production name
(a direct CreateMutexW in an append, or one that creates it and closes it again) is invisible to them, so this
test reads the source: CreateMutex* appears exactly once in src/runtime/host/, inside CreateLogMutex, taking
LogMutexName(), and the production name constant is used only by LogMutexName().
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOST = REPO / "src" / "runtime" / "host"
CREATE = re.compile(r"\bCreateMutex(?:Ex)?[AW]?\s*\(")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def creations(files):
    """[(path, enclosing function or None, first argument list text)] for every CreateMutex* call in the
    given {path: text}."""
    found = []
    for path, text in sorted(files.items()):
        code = strip_comments(text)
        for match in CREATE.finditer(code):
            before = code[:match.start()]
            functions = list(re.finditer(r"\n[A-Za-z_][\w:<>\s\*&,]*?\b(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", before))
            owner = functions[-1].group(1) if functions else None
            depth, at = 1, match.end()
            while depth and at < len(code):
                depth += {"(": 1, ")": -1}.get(code[at], 0)
                at += 1
            found.append((path, owner, code[match.end():at - 1].strip()))
    return found


def violations(files):
    found = creations(files)
    problems = []
    if len(found) != 1:
        problems.append(f"expected exactly one CreateMutex* call in the host units, found {len(found)}: {found}")
    for path, owner, args in found:
        if owner != "CreateLogMutex":
            problems.append(f"{path}: CreateMutex* is called from {owner}, not CreateLogMutex")
        if "LogMutexName()" not in args:
            problems.append(f"{path}: CreateMutex* does not take LogMutexName(): ({args})")
    uses = [path for path, text in files.items()
            if path.endswith(".cpp") and "kBootstrapLogMutexName" in strip_comments(text)]
    uses_count = sum(strip_comments(text).count("kBootstrapLogMutexName") for path, text in files.items()
                     if path.endswith(".cpp"))
    if uses_count != 1:
        problems.append(f"kBootstrapLogMutexName is used {uses_count} times in the host .cpp files (only LogMutexName() may): {uses}")
    return problems


def host_files():
    return {str(p.relative_to(REPO)): p.read_text(encoding="utf-8")
            for p in sorted(HOST.iterdir()) if p.suffix in (".cpp", ".h", ".inl", ".inc")}


class LogMutexCreationTest(unittest.TestCase):
    def test_the_real_host_creates_the_mutex_in_one_place(self):
        files = host_files()
        self.assertEqual(violations(files), [])
        self.assertEqual([(owner, args) for _, owner, args in creations(files)],
                         [("CreateLogMutex", "nullptr, FALSE, LogMutexName()")])

    def _with(self, replacement_old, replacement_new):
        files = host_files()
        path = "src/runtime/host/bootstrap_diagnostics.cpp"
        self.assertIn(replacement_old, files[path])
        files[path] = files[path].replace(replacement_old, replacement_new, 1)
        return violations(files)

    APPEND_LOAD = "  HANDLE lock = g_log_mutex.load(std::memory_order_acquire);\n"

    def test_a_direct_creation_in_an_append_is_found(self):  # the 263c57d4 shape
        self.assertNotEqual(self._with(self.APPEND_LOAD, self.APPEND_LOAD +
            "  if (lock == nullptr) {\n    lock = CreateMutexW(nullptr, FALSE, kBootstrapLogMutexName);\n"
            "    g_log_mutex.store(lock);\n  }\n"), [])

    def test_a_creation_that_closes_the_mutex_again_is_found(self):
        self.assertNotEqual(self._with(self.APPEND_LOAD, self.APPEND_LOAD +
            "  if (lock == nullptr) {\n    HANDLE t = CreateMutexW(nullptr, FALSE, kBootstrapLogMutexName);\n"
            "    if (t != nullptr) CloseHandle(t);\n  }\n"), [])

    def test_a_creation_under_the_configured_name_in_an_append_is_found(self):
        self.assertNotEqual(self._with(self.APPEND_LOAD, self.APPEND_LOAD +
            "  if (lock == nullptr) {\n    HANDLE t = CreateMutexExW(nullptr, LogMutexName(), 0, SYNCHRONIZE);\n"
            "    if (t != nullptr) CloseHandle(t);\n  }\n"), [])

    def test_the_production_name_in_the_one_creation_is_found(self):
        self.assertNotEqual(self._with("CreateMutexW(nullptr, FALSE, LogMutexName())",
                                       "CreateMutexW(nullptr, FALSE, kBootstrapLogMutexName)"), [])

    def test_a_creation_in_a_comment_is_not_one(self):
        self.assertEqual(self._with(self.APPEND_LOAD, self.APPEND_LOAD + "  // CreateMutexW(nullptr, FALSE, x);\n"), [])


if __name__ == "__main__":
    unittest.main()
