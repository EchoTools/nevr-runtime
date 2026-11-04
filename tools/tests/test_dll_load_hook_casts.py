"""src/runtime/hook/dll_load_hook.cpp uses no C-style cast (#665).

A C-style cast does not say which C++ cast it is, so it can reinterpret a pointer where a static_cast was meant.
This check strips comments and string literals, then looks for a cast to a builtin or Windows type followed by an
operand. The file's conversions are static_cast and reinterpret_cast.
"""

import pathlib
import re
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
TARGET = "src/runtime/hook/dll_load_hook.cpp"

TYPES = (
    "void", "char", "wchar_t", "bool", "int", "long", "short", "float", "double",
    "size_t", "uintptr_t", "intptr_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "std::size_t", "std::uintptr_t",
    "DWORD", "BYTE", "WORD", "UINT", "BOOL", "HMODULE", "HANDLE", "FARPROC", "LPVOID",
    "LPCSTR", "LPCWSTR", "LPSTR", "LPWSTR", "MH_STATUS",
)

# A cast: "(" type ")" then an operand. The lookahead refuses the words that follow a parameter list
# (a function's const, noexcept, override), so a declaration is not a cast.
CAST = re.compile(
    r"\(\s*(?:(?:const|unsigned|signed)\s+)*(?:" + "|".join(re.escape(t) for t in TYPES) + r")"
    r"\s*(?:\*\s*)?\)\s*(?!(?:const|noexcept|override|final|volatile|throw)\b)(?=[\w(&*!~+\-])"
)

# Comments and string or character literals, replaced by spaces so line numbers stay put.
SKIP = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)


def code_only(text):
    return SKIP.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def c_style_casts(text):
    """(line number, line text) of each C-style cast in the C++ source text."""
    code = code_only(text)
    lines = text.splitlines()
    hits = []
    for m in CAST.finditer(code):
        line_no = code.count("\n", 0, m.start()) + 1
        hits.append((line_no, lines[line_no - 1].strip()))
    return hits


class CStyleCastInDllLoadHook(unittest.TestCase):
    def read_source(self, path, label):
        """The text of the source to scan: the file must exist and be the real file, not a stub."""
        self.assertTrue(path.is_file(), "missing %s" % label)
        text = path.read_text(encoding="utf-8")
        self.assertGreater(len(text), 1000, "%s is too short to be the real file" % label)
        return text

    def assert_no_c_style_cast(self, text, label):
        hits = c_style_casts(text)
        self.assertEqual(
            [],
            hits,
            "C-style casts in %s (use static_cast or reinterpret_cast):\n%s"
            % (label, "\n".join("  %s:%d: %s" % (label, n, s) for n, s in hits)),
        )

    def test_the_file_has_no_c_style_cast(self):
        text = self.read_source(REPO / TARGET, TARGET)
        self.assert_no_c_style_cast(text, TARGET)

    def test_the_source_guards_refuse_a_missing_or_stub_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            stub = pathlib.Path(tmp) / "stub.cpp"
            stub.write_text("int x;\n", encoding="utf-8")
            with self.assertRaises(AssertionError):
                self.read_source(stub, "stub.cpp")
            with self.assertRaises(AssertionError):
                self.read_source(pathlib.Path(tmp) / "missing.cpp", "missing.cpp")

    def test_the_assertion_fails_on_a_cast(self):
        with self.assertRaises(AssertionError):
            self.assert_no_c_style_cast("x = (char)c;\n", "fixture.cpp")

    def test_matches_each_cast_shape(self):
        for text in (
            "x = (char)c;",
            "f((void*)&LoadLibraryA);",
            "x = (int) y;",
            "out = (char)(c + 32);",
            "p = (const char*)q;",
            "c = '\\\"'; p = (char)c; s = \"ok\";",
        ):
            self.assertEqual(1, len(c_style_casts(text)), text)

    def test_ignores_what_is_not_a_cast(self):
        for text in (
            "x = static_cast<char>(c);",
            "p = reinterpret_cast<void*>(&LoadLibraryA);",
            "void f(void);",
            "int F(int) const;",
            "void (*cb)(int) = nullptr;",
            "n = sizeof(char);",
            "// (char)c in a comment",
            "/* (void*)p in a block comment */",
            'log("(char)c in a string");',
        ):
            self.assertEqual([], c_style_casts(text), text)

    def test_a_cast_put_back_is_found_at_its_line(self):
        fixed = (
            "void Shutdown() {\n"
            "    MH_DisableHook(reinterpret_cast<void*>(&LoadLibraryA));\n"
            "}\n"
        )
        self.assertEqual([], c_style_casts(fixed))
        regressed = fixed.replace("reinterpret_cast<void*>(&LoadLibraryA)", "(void*)&LoadLibraryA")
        self.assertEqual([(2, "MH_DisableHook((void*)&LoadLibraryA);")], c_style_casts(regressed))


if __name__ == "__main__":
    unittest.main()
