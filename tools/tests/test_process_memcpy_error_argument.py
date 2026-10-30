"""Every production ProcessMemcpy call under src/runtime passes its error out-parameter (logging Rule 10).

ProcessMemcpy(dest, src, n) without the fourth argument drops the Win32 error of a failed write, so the Warning that
reports the failed write has no error code. The header keeps the argument optional for the tests, which call the
three-argument form on purpose; src/runtime/tests is not scanned, and neither is the header's own definition.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "src" / "runtime"
EXEMPT = {RUNTIME / "hook" / "process_memory.h"}
CALL = re.compile(r"(?<![\w:])ProcessMemcpy\s*\(")
EXPECTED_ARGUMENTS = 4


def code_only(text: str) -> str:
    """The text with comments removed and every newline kept, so a match's line number is still right."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif text[i] in "\"'":
            end = literal_end(text, i)
            literal = text[i:end]
            # The contents are blanked, so a call spelled inside a string is not a call; the quotes stay as bounds.
            out.append(literal[0] + re.sub(r"[^\n]", " ", literal[1:-1]) + literal[-1] if len(literal) > 1 else literal)
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def literal_end(text: str, start: int) -> int:
    """The index just past the string or character literal that opens at `start`; a literal ends at its line's end."""
    quote = text[start]
    i = start + 1
    while i < len(text) and text[i] not in (quote, "\n"):
        i += 2 if text[i] == "\\" else 1
    return min(i + 1, len(text))


def argument_count(code: str, open_at: int) -> int:
    """The number of top-level arguments of the call whose '(' is at `open_at`; raises if it never closes."""
    depth, commas, content = 0, 0, False
    i = open_at + 1
    while i < len(code):
        c = code[i]
        if c in "\"'":
            i = literal_end(code, i)
            content = True
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return commas + 1 if content else 0
            depth -= 1
        elif c == "," and depth == 0:
            commas += 1
        if not c.isspace():
            content = True
        i += 1
    raise ValueError(f"unclosed ProcessMemcpy( at offset {open_at}")


def violations_in(text: str) -> list[tuple[int, int]]:
    """(line, argument count) of each ProcessMemcpy call in `text` that does not pass the error argument."""
    code = code_only(text)
    found = []
    for match in CALL.finditer(code):
        count = argument_count(code, match.end() - 1)
        if count != EXPECTED_ARGUMENTS:
            found.append((code.count("\n", 0, match.start()) + 1, count))
    return found


def production_sources() -> list[Path]:
    return sorted(
        path
        for path in RUNTIME.rglob("*")
        if path.suffix in (".cpp", ".h") and "tests" not in path.relative_to(RUNTIME).parts and path not in EXEMPT
    )


class ProcessMemcpyErrorArgument(unittest.TestCase):
    def test_every_production_call_passes_the_error_argument(self):
        problems = []
        calls = 0
        for path in production_sources():
            text = path.read_text(encoding="utf-8", errors="replace")
            calls += len(CALL.findall(code_only(text)))
            for line, count in violations_in(text):
                problems.append(f"{path.relative_to(ROOT)}:{line} passes {count} arguments")
        self.assertGreater(calls, 0, "found no ProcessMemcpy call under src/runtime: the scan is watching nothing")
        self.assertEqual(problems, [], "ProcessMemcpy without its error out-parameter (logging Rule 10)")

    def test_a_three_argument_call_is_refused(self):
        self.assertEqual(violations_in("void F() { ProcessMemcpy(dest, src, n); }"), [(1, 3)])

    def test_a_call_with_the_error_argument_passes(self):
        self.assertEqual(violations_in("void F() { DWORD e = 0; ProcessMemcpy(dest, src, n, &e); }"), [])

    def test_nested_commas_do_not_count_as_arguments(self):
        self.assertEqual(violations_in("ProcessMemcpy(Offset(a, b), c, sizeof(x), &e);"), [])
        self.assertEqual(violations_in("ProcessMemcpy(Offset(a, b), c, sizeof(x));"), [(1, 3)])

    def test_a_comment_a_string_and_a_longer_name_are_not_calls(self):
        self.assertEqual(violations_in("// ProcessMemcpy(dest, src, n)\n"), [])
        self.assertEqual(violations_in('/* ProcessMemcpy(dest, src, n) */ const char* s = "ProcessMemcpy(a, b)";'), [])
        self.assertEqual(violations_in("ProcessMemcpyWithFlush(dest, src, n, &e, f, g);"), [])


if __name__ == "__main__":
    unittest.main()
