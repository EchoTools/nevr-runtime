"""#892: the README's "Built on, and thanks" section and the in-game credits read one file,
config/credits.json. tools/render_credits.py renders the README half; these tests pin the file's
shape, that the README block is exactly its rendering, where the section sits, and that an empty
heading shows nowhere."""
from __future__ import annotations

import copy
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import work_root  # noqa: E402
README = ROOT / "README.md"
CREDITS = ROOT / "config" / "credits.json"
TOOL = ROOT / "tools" / "render_credits.py"

SECTION_HEADING = "## Built on, and thanks"
# A link in the section is a person's or organisation's GitHub profile, or a repository.
LINK_SHAPE = re.compile(r"^https://github\.com/[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)?$")


def load_tool():
    spec = importlib.util.spec_from_file_location("render_credits", TOOL)
    assert spec is not None and spec.loader is not None, f"cannot load {TOOL}"
    module = importlib.util.module_from_spec(spec)
    sys.modules["render_credits"] = module
    spec.loader.exec_module(module)
    return module


def h2_titles(text: str) -> list[str]:
    return re.findall(r"^## (.+)$", text, flags=re.M)


def readme_section(text: str) -> str:
    start = text.index(SECTION_HEADING)
    match = re.search(r"^## ", text[start + len(SECTION_HEADING):], flags=re.M)
    end = start + len(SECTION_HEADING) + match.start() if match else len(text)
    return text[start:end]


class ReadmePlacement(unittest.TestCase):
    def test_section_is_the_first_h2_and_precedes_what_it_does(self):
        titles = h2_titles(README.read_text(encoding="utf-8"))
        self.assertIn("Built on, and thanks", titles)
        self.assertIn("What it does", titles)
        self.assertEqual(titles[0], "Built on, and thanks", "the section follows the intro, before any other section")
        self.assertLess(titles.index("Built on, and thanks"), titles.index("What it does"))

    def test_links_in_the_section_are_github_profiles_or_repositories(self):
        section = readme_section(README.read_text(encoding="utf-8"))
        urls = re.findall(r"https?://[^\s)>\]]+", section)
        self.assertTrue(urls, "the section links the projects and people it names")
        for url in urls:
            self.assertRegex(url, LINK_SHAPE, f"{url}: only github.com/<owner> or github.com/<owner>/<repo>")


class SharedFile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool()
        cls.data = json.loads(CREDITS.read_text(encoding="utf-8"))

    def test_the_file_is_valid(self):
        self.assertEqual(self.tool.validate(self.data, ROOT), [])

    def test_readme_block_is_exactly_the_rendering(self):
        self.assertEqual(self.tool.check(CREDITS, README), [])

    def test_check_command_exits_zero_on_the_tree(self):
        proc = subprocess.run([sys.executable, str(TOOL), "--check"], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_a_name_appears_once(self):
        names = [entry["name"] for entry in self.data["entries"]]
        self.assertEqual(len(names), len(set(names)))

    def test_empty_headings_show_nowhere(self):
        groups = self.tool.ingame_groups(self.data)
        shown = {group["id"] for group in groups}
        for heading in self.data["headings"]:
            has_names = any(heading["id"] in entry["headings"] for entry in self.data["entries"])
            self.assertEqual(heading["id"] in shown, has_names, heading["id"])
        for group in groups:
            self.assertTrue(group["names"], f"{group['ingame']} has a heading and no names")
        readme_text = self.tool.render_readme(self.data)
        for heading in self.data["headings"]:
            if heading["id"] not in shown:
                self.assertNotIn(heading["ingame"], readme_text)

    def test_ingame_groups_hold_names_only_in_file_order(self):
        data = {
            "headings": [{"id": "a", "ingame": "A HEADING"}, {"id": "empty", "ingame": "EMPTY"}],
            "sections": [{"id": "people", "title": "People"}],
            "entries": [
                {"name": "One", "section": "people", "headings": ["a"], "link": "", "what": "x.", "sources": ["#1"]},
                {"name": "Two", "section": "people", "headings": [], "link": "", "what": "y.", "sources": ["#2"]},
                {"name": "Three", "section": None, "headings": ["a"], "link": "", "what": "", "sources": ["#3"]},
            ],
            "closing": "",
        }
        self.assertEqual(self.tool.ingame_groups(data), [{"id": "a", "ingame": "A HEADING", "names": ["One", "Three"]}])

    def test_an_entry_with_no_section_is_not_in_the_readme(self):
        data = copy.deepcopy(self.data)
        data["entries"].append(
            {"name": "SupporterNameForTest", "section": None, "headings": ["supporters"], "link": "", "what": "", "sources": ["#892"]}
        )
        self.assertNotIn("SupporterNameForTest", self.tool.render_readme(data))
        self.assertIn("SupporterNameForTest", [n for g in self.tool.ingame_groups(data) for n in g["names"]])

    def entry(self, name):
        found = [e for e in self.data["entries"] if e["name"] == name]
        self.assertEqual(len(found), 1, f"{name} appears once in the file")
        return found[0]

    def test_in_game_code_contributions_are_these_names_in_file_order(self):
        self.assertEqual(
            self.tool.ingame_groups(self.data),
            [{"id": "code-contributions", "ingame": "CODE CONTRIBUTIONS", "names": ["Xenomega", "kfc", "Dualgame", "marshmallow-mia"]}],
        )

    def test_names_the_project_owner_gave_say_so_in_their_sources(self):
        owner = re.compile(r"^named by the project owner(, \d{2}-\d{2})?$")
        for name, date in (("kfc", "10-19"), ("Ducky", "10-13"), ("marshmallow-mia", None)):
            sources = [s for s in self.entry(name)["sources"] if owner.match(s)]
            self.assertTrue(sources, f"{name}: no 'named by the project owner' source")
            if date:
                self.assertIn(f"named by the project owner, {date}", sources)

    def test_kfc_and_ducky_are_in_the_readme_and_the_linux_port_is_credited(self):
        block = self.tool.render_readme(self.data)
        self.assertIn("**kfc**: Helped write a lot of EchoRelay.", block)
        self.assertIn("**Ducky**: The reverse engineer behind the Echo Combat Quest port.", block)
        self.assertIn("Linux port", self.entry("marshmallow-mia")["what"])
        self.assertNotIn("quest.echovr.de", block)

    def test_ducky_has_no_in_game_heading_and_crafter_is_one_entry(self):
        self.assertEqual(self.entry("Ducky")["headings"], [])
        self.assertEqual(self.entry("kfc")["headings"], ["code-contributions"])
        self.assertNotIn("Crafter-1", [e["name"] for e in self.data["entries"]])

    def test_no_pronoun_for_a_person_anywhere_in_the_text(self):
        pronoun = re.compile(r"\b(he|she|him|her|hers|his|they|them|their|theirs)\b", re.I)
        for entry in self.data["entries"]:
            self.assertIsNone(pronoun.search(entry["what"]), f"{entry['name']}: {entry['what']}")
        self.assertIsNone(pronoun.search(self.data["closing"]))

    def test_a_source_is_public_or_says_the_project_owner_named_it(self):
        def problems(source):
            data = copy.deepcopy(self.data)
            data["entries"][0]["sources"] = [source]
            return self.tool.validate(data, ROOT)

        self.assertEqual(problems("named by the project owner, 10-19"), [])
        self.assertEqual(problems("named by the project owner"), [])
        self.assertTrue(problems("sprockee said so"))
        self.assertTrue(problems("named by the project owner in chat, 10-19"))

    def test_validate_refuses_a_bad_file(self):
        def problems(mutate):
            data = copy.deepcopy(self.data)
            mutate(data)
            return self.tool.validate(data, ROOT)

        self.assertTrue(problems(lambda d: d["entries"].append(copy.deepcopy(d["entries"][0]))), "duplicate name")
        self.assertTrue(problems(lambda d: d["entries"][0]["headings"].append("nope")), "unknown heading tag")
        self.assertTrue(problems(lambda d: d["entries"][0].update(section="nope")), "unknown section")
        self.assertTrue(problems(lambda d: d["entries"][0].update(sources=[])), "no source")
        self.assertTrue(problems(lambda d: d["entries"][0].update(sources=["a chat message"])), "source not public")
        self.assertTrue(problems(lambda d: d["entries"][0].update(link="http://example.com/x")), "link shape")
        self.assertTrue(problems(lambda d: d["entries"][0].update(what="")), "README entry says nothing")


class CheckerCatchesDrift(unittest.TestCase):
    """Mutants of the pair: change one side only and --check must say so."""

    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool()

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="credits-test-"))
        self.json = self.tmp / "credits.json"
        self.readme = self.tmp / "README.md"
        shutil.copy(CREDITS, self.json)
        shutil.copy(README, self.readme)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_baseline_is_clean(self):
        self.assertEqual(self.tool.check(self.json, self.readme), [])

    def test_changing_a_name_in_the_file_only_fails(self):
        data = json.loads(self.json.read_text(encoding="utf-8"))
        data["entries"][0]["name"] = data["entries"][0]["name"] + "X"
        self.json.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertTrue(self.tool.check(self.json, self.readme))

    def test_changing_the_readme_only_fails(self):
        text = self.readme.read_text(encoding="utf-8")
        data = json.loads(self.json.read_text(encoding="utf-8"))
        name = data["entries"][0]["name"]
        self.assertIn(name, text)
        self.readme.write_text(text.replace(name, name + "X", 1), encoding="utf-8")
        self.assertTrue(self.tool.check(self.json, self.readme))

    def test_a_readme_without_the_block_fails(self):
        text = self.readme.read_text(encoding="utf-8")
        self.readme.write_text(re.sub(r"<!-- credits:begin.*?<!-- credits:end -->\n", "", text, flags=re.S), encoding="utf-8")
        self.assertTrue(self.tool.check(self.json, self.readme))

    def test_write_regenerates_the_block(self):
        text = self.readme.read_text(encoding="utf-8")
        data = json.loads(self.json.read_text(encoding="utf-8"))
        data["entries"][0]["name"] = data["entries"][0]["name"] + "X"
        self.json.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertTrue(self.tool.check(self.json, self.readme))
        proc = subprocess.run(
            [sys.executable, str(TOOL), "--write", "--json", str(self.json), "--readme", str(self.readme)],
            capture_output=True, text=True,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.tool.check(self.json, self.readme), [])
        self.assertNotEqual(self.readme.read_text(encoding="utf-8"), text)


def c_literal_bytes(source: str, symbol: str) -> bytes:
    """Decode the C string literal assigned to `symbol` in generated source: adjacent quoted pieces, with
    \\ooo octal, \\\\ and \\" escapes only (all the generator emits)."""
    match = re.search(re.escape(symbol) + r"\[\]\s*=\s*((?:\s*\"(?:[^\"\\\n]|\\.)*\")+)\s*;", source)
    assert match, f"no literal for {symbol}"
    out = bytearray()
    for piece in re.findall(r'"((?:[^"\\\n]|\\.)*)"', match.group(1)):
        i = 0
        while i < len(piece):
            if piece[i] != "\\":
                out.append(ord(piece[i]))
                i += 1
            elif piece[i + 1] in '"\\':
                out.append(ord(piece[i + 1]))
                i += 2
            else:
                out.append(int(piece[i + 1:i + 4], 8))
                i += 4
    return bytes(out)


class IngameText(unittest.TestCase):
    """#938: the block the runtime puts above Ready At Dawn's credits, rendered here at build time."""

    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool()
        cls.data = json.loads(CREDITS.read_text(encoding="utf-8"))

    def data_with(self, groups):
        """A file whose in-game groups are exactly `groups` ({heading text: [names]})."""
        headings = [{"id": f"h{i}", "ingame": text} for i, text in enumerate(groups)]
        entries = []
        for i, names in enumerate(groups.values()):
            for name in names:
                entries.append({"name": name, "section": None, "headings": [f"h{i}"], "link": "", "what": "",
                                "sources": ["#938"]})
        return {"schema": 1, "headings": headings, "sections": [], "entries": entries, "closing": ""}

    def test_the_real_file_renders_the_heading_a_blank_the_names_and_three_blanks(self):
        names = [n for g in self.tool.ingame_groups(self.data) for n in g["names"]]
        self.assertEqual(
            self.tool.ingame_text(self.data),
            ("CODE CONTRIBUTIONS\n\n" + "".join(n + "\n" for n in names) + "\n\n\n").encode("cp1252"),
        )

    def test_groups_are_separated_by_three_blank_lines(self):
        data = self.data_with({"ONE": ["a", "b"], "TWO": ["c"]})
        self.assertEqual(self.tool.ingame_text(data), b"ONE\n\na\nb\n\n\n\nTWO\n\nc\n\n\n\n")

    def test_an_empty_heading_emits_nothing_not_even_its_blank_lines(self):
        data = self.data_with({"ONE": ["a"], "EMPTY": [], "TWO": ["c"]})
        self.assertEqual(self.tool.ingame_text(data), b"ONE\n\na\n\n\n\nTWO\n\nc\n\n\n\n")
        self.assertNotIn(b"EMPTY", self.tool.ingame_text(data))

    def test_no_names_at_all_is_an_empty_block(self):
        self.assertEqual(self.tool.ingame_text(self.data_with({"ONE": [], "TWO": []})), b"")

    def test_names_are_encoded_as_the_games_single_byte_text(self):
        data = self.data_with({"ONE": ["\u00a9 Name", "plain"]})
        text = self.tool.ingame_text(data)
        self.assertIn(b"\xa9 Name\n", text)
        self.assertIn(b"plain\n", text)
        self.assertNotIn(b"\xc3", text)  # no UTF-8 lead byte

    def problems(self, groups):
        return self.tool.validate(self.data_with(groups), ROOT)

    def test_a_character_the_font_cannot_draw_is_refused_naming_the_entry_and_the_field(self):
        # Measured on the running game: the font draws printable ASCII and the copyright sign; U+00EB draws as '?'.
        for char in ("\u00eb", "\u00fc", "\u0141", "\U0001F600", "\u00e9", "\u00a0", "\u2019", "\x7f", "\u0080"):
            problems = self.problems({"ONE": [f"Zo{char}"]})
            self.assertEqual(len(problems), 1, (char, problems))
            self.assertIn(repr(f"Zo{char}"), problems[0])
            self.assertIn("field name", problems[0])
            self.assertIn(f"U+{ord(char):04X}", problems[0])
        heading = self.problems({"H\u00ebad": ["a"]})
        self.assertEqual(len(heading), 1, heading)
        self.assertIn("field ingame", heading[0])
        self.assertIn("U+00EB", heading[0])

    def test_every_undrawn_character_is_listed_once_in_code_point_order(self):
        problems = self.problems({"ONE": ["a\u00fc\u00eb\u00eb"]})
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("'\u00eb' (U+00EB), '\u00fc' (U+00FC)", problems[0])
        self.assertEqual(problems[0].count("U+00EB"), 1)

    def test_an_undrawn_name_is_one_problem_even_when_it_is_also_too_long(self):
        self.assertEqual(len(self.problems({"ONE": ["\u00eb" * 70]})), 1)

    def test_printable_ascii_and_the_copyright_sign_are_drawn(self):
        ascii_printable = "".join(chr(c) for c in range(0x21, 0x7F) if chr(c) != "|")
        self.assertEqual(self.problems({"ONE": [ascii_printable[:64], ascii_printable[64:], "a b", "\u00a9 9", "\u00a9"]}), [])

    def test_ingame_text_itself_refuses_a_character_the_font_cannot_draw(self):
        # Not only validate(): a caller that skips it must not get bytes for a name the page would show as '?'.
        with self.assertRaises(ValueError):
            self.tool.ingame_text(self.data_with({"ONE": ["Zo\u00eb"]}))

    def test_a_name_that_would_change_the_games_parse_is_refused(self):
        for bad in ("a|b", "a\nb", "a\rb", "a\x00b", "a\tb", " lead", "trail ", ""):
            self.assertTrue(self.problems({"ONE": [bad]}), repr(bad))

    def test_the_games_sixty_four_byte_line_is_the_limit_for_names_and_headings(self):
        self.assertEqual(self.problems({"ONE": ["W" * 64]}), [])
        self.assertTrue(self.problems({"ONE": ["W" * 65]}))
        self.assertEqual(self.problems({"H" * 64: ["a"]}), [])
        self.assertTrue(self.problems({"H" * 65: ["a"]}))

    def test_a_drawn_non_ascii_character_counts_one_byte(self):
        # The copyright sign is one byte in the game's text (two in UTF-8), so 64 of them fit and 65 do not.
        self.assertEqual(self.problems({"ONE": ["\u00a9" * 64]}), [])
        self.assertTrue(self.problems({"ONE": ["\u00a9" * 65]}))

    def test_two_hundred_lines_is_the_cap(self):
        # one group is N + 5 lines: heading, blank, N names, three blanks
        self.assertEqual(self.problems({"ONE": [f"n{i}" for i in range(195)]}), [])
        self.assertTrue(self.problems({"ONE": [f"n{i}" for i in range(196)]}))
        self.assertEqual(self.tool.ingame_text(self.data_with({"ONE": [f"n{i}" for i in range(195)]})).count(b"\n"), 200)

    def test_a_name_over_the_review_length_is_a_warning_not_a_problem(self):
        data = self.data_with({"ONE": ["W" * 41, "ok"]})
        self.assertEqual(self.tool.validate(data, ROOT), [])
        self.assertEqual(len(self.tool.ingame_warnings(data)), 1)
        self.assertEqual(self.tool.ingame_warnings(self.data_with({"ONE": ["W" * 40]})), [])

    def test_the_shipped_file_has_no_in_game_warning(self):
        self.assertEqual(self.tool.ingame_warnings(self.data), [])

    def test_the_c_source_round_trips_every_byte(self):
        for groups in ({"ONE": ["Zoe", '"quoted"', "back\\slash", "\u00a9 9"]}, {"ONE": []}, {"A": ["x"], "B": ["y"]}):
            data = self.data_with(groups)
            source = self.tool.ingame_source(data)
            block = self.tool.ingame_text(data)
            self.assertEqual(c_literal_bytes(source, "kText"), block)
            self.assertIn(f"kSize = {len(block)};", source)

    def test_the_c_source_uses_octal_escapes_so_a_digit_cannot_extend_one(self):
        # "\xa9" followed by the digit 9 would read as one hex escape; octal is fixed at three digits.
        source = self.tool.ingame_source(self.data_with({"ONE": ["\u00a9 9", "\u00a99"]}))
        self.assertNotIn("\\x", source)
        self.assertEqual(c_literal_bytes(source, "kText").count(b"\xa99"), 1)

    def test_a_question_mark_is_escaped_so_no_trigraph_can_form(self):
        source = self.tool.ingame_source(self.data_with({"ONE": ["a??/b", "what?"]}))
        self.assertNotIn("??", source)
        self.assertEqual(c_literal_bytes(source, "kText").count(b"a??/b\n"), 1)

    def test_validate_includes_the_in_game_rules_so_check_and_the_build_agree(self):
        data = copy.deepcopy(self.data)
        data["entries"][0]["name"] = "Xeno|mega"
        self.assertTrue(self.tool.validate(data, ROOT))

    def test_the_command_writes_the_text_and_the_source(self):
        with tempfile.TemporaryDirectory(prefix="credits-ingame-", dir=work_root.test_scratch()) as tmp:
            text, source = Path(tmp) / "block.txt", Path(tmp) / "block.cpp"
            for flag, out in (("--ingame-text", text), ("--ingame-source", source)):
                proc = subprocess.run([sys.executable, str(TOOL), flag, str(out)], cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(text.read_bytes(), self.tool.ingame_text(self.data))
            self.assertEqual(c_literal_bytes(source.read_text(encoding="ascii"), "kText"), text.read_bytes())

    def test_the_command_refuses_a_bad_file_and_writes_nothing(self):
        with tempfile.TemporaryDirectory(prefix="credits-ingame-", dir=work_root.test_scratch()) as tmp:
            bad = copy.deepcopy(self.data)
            bad["entries"][0]["name"] = "bad|name"  # encodes fine: only validation refuses it
            bad_json, out = Path(tmp) / "credits.json", Path(tmp) / "block.cpp"
            bad_json.write_text(json.dumps(bad), encoding="utf-8")
            proc = subprocess.run([sys.executable, str(TOOL), "--ingame-source", str(out), "--json", str(bad_json)],
                                  cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            self.assertFalse(out.exists())

    def test_the_command_refuses_a_name_the_font_cannot_draw_and_names_it(self):
        for flag in ("--ingame-source", "--ingame-text"):
            with tempfile.TemporaryDirectory(prefix="credits-ingame-", dir=work_root.test_scratch()) as tmp:
                bad = copy.deepcopy(self.data)
                bad["entries"][0]["name"] = "Zo\u00eb"  # in Windows-1252, so only the font rule refuses it
                bad_json, out = Path(tmp) / "credits.json", Path(tmp) / "block.out"
                bad_json.write_text(json.dumps(bad), encoding="utf-8")
                proc = subprocess.run([sys.executable, str(TOOL), flag, str(out), "--json", str(bad_json)],
                                      cwd=ROOT, capture_output=True, text=True)
                self.assertNotEqual(proc.returncode, 0, flag)
                self.assertFalse(out.exists(), flag)
                self.assertIn("Zo\u00eb", proc.stderr)
                self.assertIn("field name", proc.stderr)
                self.assertIn("U+00EB", proc.stderr)


if __name__ == "__main__":
    unittest.main()
