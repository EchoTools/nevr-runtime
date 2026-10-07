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
    def test_format_symbol_id_in_the_template_matches_the_committed_file(self):
        generator = load_generator()
        template = next(v for k, v in vars(generator).items() if k.endswith("FOOTER") and isinstance(v, str)
                        and "FormatSymbolId" in v)
        committed = (REPO / "src/runtime/hook/symbol_corpus.cpp").read_text()
        signature = "int FormatSymbolId(char* buf, int maxLen, uint64_t hash)"
        self.assertEqual(normalise(function_body(template, signature)), normalise(function_body(committed, signature)))

    def test_the_template_keeps_the_argument_guard(self):
        generator = load_generator()
        template = next(v for k, v in vars(generator).items() if k.endswith("FOOTER") and isinstance(v, str)
                        and "FormatSymbolId" in v)
        self.assertIn("if (maxLen <= 0 || buf == nullptr) return 0;", template)


if __name__ == "__main__":
    unittest.main()
