"""The symbol-corpus generator's C++ footer must match the committed symbol_corpus.cpp.

Regenerating the corpus rewrites symbol_corpus.cpp from the template in tools/gen_symbol_corpus.py; a
hand-edit of the committed file that the template does not carry (the FormatSymbolId argument guard,
N66) is silently lost on the next regeneration.
"""
import importlib.util
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_symbol_corpus", REPO / "tools/gen_symbol_corpus.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError(f"unterminated function: {signature}")


def normalise(code: str) -> str:
    return re.sub(r"\s+", " ", code).strip()


class SymbolCorpusTemplateTest(unittest.TestCase):
    def test_the_committed_file_ends_with_the_templates_footer_byte_for_byte(self):
        # Regeneration writes CPP_FOOTER verbatim after the table, so the committed file's tail must be the
        # template: any edit made to one and not the other (a guard, a format string, whitespace) fails here.
        footer = load_generator().CPP_FOOTER
        committed = (REPO / "src/runtime/hook/symbol_corpus.cpp").read_text()
        self.assertTrue(committed.endswith(footer), "symbol_corpus.cpp no longer ends with the generator's CPP_FOOTER")

    def test_format_symbol_id_in_the_template_matches_the_committed_file_and_keeps_its_guard(self):
        footer = load_generator().CPP_FOOTER
        committed = (REPO / "src/runtime/hook/symbol_corpus.cpp").read_text()
        signature = "int FormatSymbolId(char* buf, int maxLen, uint64_t hash)"
        self.assertEqual(normalise(function_body(footer, signature)), normalise(function_body(committed, signature)))
        self.assertIn("if (maxLen <= 0 || buf == nullptr) return 0;", footer)


if __name__ == "__main__":
    unittest.main()
