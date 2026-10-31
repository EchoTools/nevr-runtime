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
# The six names the community installer's patcher thanks, spelled exactly as it spells them (#974).
THANKS = ["marshmallow_mia", "AvaGoosa", "h4ck3r210", "Elominator", "sprockee", "he_is_the_cat"]
# Everyone named as having tested in #nevr-runtime-testing (#974): the set, not an order.
TESTERS = {"Crafter", "Nebula", "CyanoTex", "spike4887", "Trodd-", "DinkyBoi", "he_is_the_cat", "TekEmperor",
           "marshmallow-mia", "arii", "berg", "zebra"}
PATCHER_SOURCE = "echoApkPatch credit strings"
TESTING_SOURCE = "tested in #nevr-runtime-testing (Discord)"
WEBSITE_SOURCE = "echovrce.com website"
TESTED_PHRASE = "tested nevr-runtime in #nevr-runtime-testing"
# The one page of a person's own work, outside GitHub, that the credits link to (the person has no GitHub account).
PORTFOLIO = "https://www.dccoffey.com/work/echo-vr"
PORTFOLIO_LINKS = {PORTFOLIO}
# A link in the section is a person's or organisation's GitHub profile, or a repository, or that page.
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
            if url in PORTFOLIO_LINKS:
                continue
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

    def test_in_game_groups_are_these_names_in_file_order(self):
        self.assertEqual(
            self.tool.ingame_groups(self.data),
            [
                {"id": "code-contributions", "ingame": "CODE CONTRIBUTIONS",
                 "names": ["Xenomega", "kfc", "Dualgame", "marshmallow-mia", "Ducky", "babs", "EchoCombat",
                           "DC Coffey", "chinmayee5599", "brysonak"]},
                {"id": "thanks", "ingame": "THANKS", "names": THANKS},
                {"id": "testers", "ingame": "TESTERS",
                 "names": ["marshmallow-mia", "Crafter", "Trodd-", "DinkyBoi", "berg", "zebra", "Nebula",
                           "CyanoTex", "spike4887", "TekEmperor", "arii", "he_is_the_cat"]},
                {"id": "supporters", "ingame": "SUPPORTERS", "names": ["Furo"]},
            ],
        )

    def test_dc_coffey_is_credited_by_name_and_portfolio_for_the_design_of_the_invite_and_create_game_menu(self):
        entry = self.entry("DC Coffey")
        self.assertEqual(entry["section"], "people")
        self.assertEqual(entry["headings"], ["code-contributions"])
        self.assertEqual(entry["link"], PORTFOLIO)
        self.assertEqual(entry["sources"], [PORTFOLIO])
        self.assertIn("Designed the invite and create-game menu", entry["what"])
        self.assertIn("Dualgame implemented", entry["what"])
        readme = README.read_text(encoding="utf-8")
        self.assertIn(f"- **[DC Coffey]({PORTFOLIO})**: Designed the invite and create-game menu", readme)

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

    def test_ducky_babs_and_the_code_contributors_have_a_heading_and_crafter_is_one_entry(self):
        for name in ("Ducky", "babs", "chinmayee5599", "brysonak"):
            self.assertEqual(self.entry(name)["headings"], ["code-contributions"], name)
        self.assertEqual(self.entry("Furo")["headings"], ["supporters"])
        self.assertEqual(self.entry("kfc")["headings"], ["code-contributions"])
        self.assertNotIn("Crafter-1", [e["name"] for e in self.data["entries"]])

    def readme_lines(self):
        """(section title, line) for every entry line the README block renders."""
        title, found = None, []
        for line in self.tool.render_readme(self.data).splitlines():
            if line.startswith("### "):
                title = line[4:]
            elif line.startswith("- **"):
                found.append((title, line))
        return found

    def test_the_thanks_group_is_the_patchers_six_strings_spelled_as_it_spells_them(self):
        group = next(g for g in self.tool.ingame_groups(self.data) if g["id"] == "thanks")
        self.assertEqual(group["names"], THANKS)
        for name in THANKS:
            entry = self.entry(name)
            self.assertIn(PATCHER_SOURCE, entry["sources"], name)
            self.assertIn("thanks", entry["headings"], name)
            self.assertNotIn("code-contributions", entry["headings"], name)

    def test_elominator_is_its_own_string_and_marshmallow_mia_keeps_her_code_entry(self):
        self.assertEqual(self.entry("Elominator")["headings"], ["thanks"])
        mia = self.entry("marshmallow-mia")
        self.assertIn("code-contributions", mia["headings"])
        self.assertIn(PATCHER_SOURCE, mia["sources"], "the patcher is her own work")
        # her patcher string is a separate in-game name; the README shows her once
        self.assertIsNone(self.entry("marshmallow_mia")["section"])
        self.assertEqual(sum(1 for _title, line in self.readme_lines() if "marshmallow" in line.lower()), 1)

    def test_the_testers_are_exactly_these_people_each_sourced_and_said_so_in_the_readme(self):
        group = next(g for g in self.tool.ingame_groups(self.data) if g["id"] == "testers")
        self.assertEqual(set(group["names"]), TESTERS)
        self.assertEqual(len(group["names"]), len(TESTERS), "each tester once")
        for name in TESTERS:
            entry = self.entry(name)
            self.assertIn(TESTING_SOURCE, entry["sources"], name)
            self.assertIn(TESTED_PHRASE, entry["what"].lower(), name)
        for entry in self.data["entries"]:
            if "testers" not in entry["headings"]:
                self.assertNotIn(TESTED_PHRASE, entry["what"].lower(), f"{entry['name']} is not named as a tester")

    def test_someone_under_another_heading_is_also_a_tester_if_they_tested(self):
        for name in ("marshmallow-mia", "Crafter", "Trodd-", "DinkyBoi", "berg", "zebra", "Nebula", "he_is_the_cat"):
            self.assertIn("testers", self.entry(name)["headings"], name)
        self.assertEqual(self.entry("he_is_the_cat")["headings"], ["thanks", "testers"])
        self.assertEqual(self.entry("marshmallow-mia")["headings"], ["code-contributions", "testers"])

    def test_trodd_is_one_entry_shown_as_trodd_dash_under_people_and_testers(self):
        names = [e["name"] for e in self.data["entries"]]
        self.assertNotIn("Trodd", names)
        self.assertEqual(names.count("Trodd-"), 1)
        entry = self.entry("Trodd-")
        self.assertEqual(entry["section"], "people")
        self.assertIn("testers", entry["headings"])
        self.assertIn("https://github.com/EchoTools/nevr-runtime/issues/877", entry["sources"])
        self.assertIn(TESTING_SOURCE, entry["sources"])
        trodd_lines = [(t, line) for t, line in self.readme_lines() if "trodd" in line.lower()]
        self.assertEqual(len(trodd_lines), 1, trodd_lines)
        self.assertEqual(trodd_lines[0][0], "People")
        self.assertTrue(trodd_lines[0][1].startswith("- **Trodd-**: "))
        block = self.tool.ingame_text(self.data)
        self.assertEqual(block.count(b"Trodd-\n"), 1)
        self.assertNotIn(b"Trodd\n", block)

    def test_trodd_dash_built_the_website_and_it_is_said_neutrally(self):
        entry = self.entry("Trodd-")
        self.assertIn("Built the echovrce.com website.", entry["what"])
        self.assertIn("XInput fix", entry["what"])
        self.assertIn(TESTED_PHRASE, entry["what"].lower())
        self.assertIn(WEBSITE_SOURCE, entry["sources"])
        # the same one entry, one README line carrying all three facts, under both headings
        self.assertEqual(entry["headings"], ["testers"])
        lines = [line for _title, line in self.readme_lines() if line.startswith("- **Trodd-**")]
        self.assertEqual(len(lines), 1)
        for fact in ("XInput fix", "Built the echovrce.com website.", "Tested nevr-runtime in #nevr-runtime-testing."):
            self.assertIn(fact, lines[0])
        # no owner or lead naming for him: not in his sources, not in his line, not in the file's notes
        owner = re.compile(r"named by the project owner")
        self.assertFalse([src for src in entry["sources"] if owner.search(src)])
        self.assertIsNone(re.search(r"owner|lead|maintainer|founder", lines[0], re.I), lines[0])
        self.assertNotRegex(" ".join(self.data["notes"]), r"Trodd-?[^.]*(owner|lead)")

    def test_sprockee_appears_once_in_every_rendered_list_and_never_first(self):
        # The ruled placement: one name among the helpers' thanks, in that group's ordinary order; never first,
        # never under a heading or a line of his own, never described as the owner or the lead.
        lines = [line for line in self.tool.ingame_text(self.data).decode("cp1252").split("\n")]
        self.assertEqual(lines.count("sprockee"), 1, "once in the in-game block")
        self.assertEqual(sum("sprockee" in line for line in lines), 1)
        groups = {g["id"]: g["names"] for g in self.tool.ingame_groups(self.data)}
        self.assertEqual([i for i, names in groups.items() if "sprockee" in names], ["thanks"])
        self.assertGreater(groups["thanks"].index("sprockee"), 0, "not first in THANKS")
        for heading in self.data["headings"]:
            self.assertNotIn("sprockee", (heading["id"] + heading["ingame"]).lower())

        readme_lines = self.readme_lines()
        mine = [(title, line) for title, line in readme_lines if "sprockee" in line]
        self.assertEqual(len(mine), 1, "once among the README lines")
        title, line = mine[0]
        self.assertEqual(title, "Thanks")
        in_group = [l for t, l in readme_lines if t == title]
        self.assertGreater(in_group.index(line), 0, "not first in the README thanks group")
        self.assertIsNone(re.search(r"owner|lead|maintainer|founder|created", line, re.I), line)

        block = self.tool.render_readme(self.data)
        self.assertEqual(block.count("sprockee"), 1, "the credits block names him once, nowhere else")
        section = readme_section(README.read_text(encoding="utf-8"))
        self.assertEqual(section.count("sprockee"), 1, "nor does the section's own text, intro or closing")

    def test_the_rule_is_recorded_in_the_files_notes(self):
        self.assertTrue(self.data["notes"])
        joined = " ".join(self.data["notes"])
        self.assertIn("sprockee", joined)
        self.assertRegex(joined, r"once")
        self.assertRegex(joined, r"never first")
        # a note is not rendered: README and in-game text carry none of it
        self.assertNotIn(self.data["notes"][0], self.tool.render_readme(self.data))

    def test_the_closing_line_thanks_everyone_in_the_testing_channel_named_or_not(self):
        self.assertIn("#nevr-runtime-testing", self.data["closing"])
        self.assertIn("named or not", self.data["closing"])
        self.assertIn(self.data["closing"], self.tool.render_readme(self.data))

    def test_every_name_shown_in_game_is_drawn_by_the_games_font(self):
        shown = [n for g in self.tool.ingame_groups(self.data) for n in g["names"]]
        for name in shown:
            self.assertEqual(self.tool._undrawn(name), "", name)
            self.assertTrue(name.isascii(), name)

    def test_notes_are_a_list_of_non_empty_texts_and_an_unknown_key_is_refused(self):
        def problems(mutate):
            data = copy.deepcopy(self.data)
            mutate(data)
            return self.tool.validate(data, ROOT)

        self.assertEqual(problems(lambda d: None), [])
        self.assertTrue(problems(lambda d: d.pop("notes")), "notes are required")
        self.assertTrue(problems(lambda d: d.update(notes="one string")), "notes is a list")
        self.assertTrue(problems(lambda d: d.update(notes=[""])), "a note says something")
        self.assertTrue(problems(lambda d: d.update(notes=[3])), "a note is text")
        self.assertTrue(problems(lambda d: d.update(extra=[])), "an unknown top-level key is refused")
        self.assertTrue(problems(lambda d: d["entries"][0].update(extra=1)), "an unknown entry key is refused")

    def test_the_three_exact_source_texts_are_exact(self):
        def problems(source):
            data = copy.deepcopy(self.data)
            data["entries"][0]["sources"] = [source]
            return self.tool.validate(data, ROOT)

        self.assertEqual(problems(PATCHER_SOURCE), [])
        self.assertEqual(problems(TESTING_SOURCE), [])
        self.assertEqual(problems(WEBSITE_SOURCE), [])
        for near in ("echoApkPatch credit string", "echoApkPatch credit strings, sprockee", "tested in #nevr-runtime-testing",
                     "tested in #other-channel (Discord)", "tested in #nevr-runtime-testing (Discord) and more",
                     "echovrce.com", "echovrce.com website, named by the project owner", "the echovrce.com website",
                     "echovrce.com website and more", "echovrce.com website "):
            self.assertTrue(problems(near), near)

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
        self.assertTrue(problems("a person said so"))
        self.assertTrue(problems("named by the project owner in a message, 10-19"))

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
        self.assertTrue(problems(lambda d: d["entries"][0].update(link="https://www.dccoffey.com/work/other")),
                        "a page on the portfolio's host that is not the listed page")
        self.assertTrue(problems(lambda d: d["entries"][0].update(link=PORTFOLIO + "/")), "the listed page with a slash")
        self.assertTrue(problems(lambda d: d["entries"][0].update(what="See https://www.dccoffey.com/work/other.")),
                        "an unlisted page in the text")
        self.assertTrue(problems(lambda d: d["entries"][0].update(sources=["https://www.dccoffey.com/work/other"])),
                        "an unlisted page as a source")

    def test_validate_accepts_the_listed_portfolio_page_as_link_text_and_source(self):
        def problems(mutate):
            data = copy.deepcopy(self.data)
            mutate(data)
            return self.tool.validate(data, ROOT)

        self.assertEqual(problems(lambda d: d["entries"][0].update(link=PORTFOLIO)), [], "as the link")
        self.assertEqual(problems(lambda d: d["entries"][0].update(what=f"Designed it, see {PORTFOLIO} for the work.")), [],
                         "in the text")
        self.assertEqual(problems(lambda d: d["entries"][0].update(sources=[PORTFOLIO])), [], "as the source")
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
        return {"schema": 1, "headings": headings, "sections": [], "entries": entries, "notes": [], "closing": ""}

    def test_the_real_file_renders_each_heading_a_blank_the_names_and_three_blanks(self):
        expected = "".join(
            group["ingame"] + "\n\n" + "".join(n + "\n" for n in group["names"]) + "\n\n\n"
            for group in self.tool.ingame_groups(self.data)
        )
        self.assertEqual(self.tool.ingame_text(self.data), expected.encode("cp1252"))
        self.assertTrue(expected.startswith("CODE CONTRIBUTIONS\n\nXenomega\n"))
        self.assertEqual(expected.count("\n"), 49, "four groups of 10, 6, 12 and 1 names: 15 + 11 + 17 + 6 lines")

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
        problems = self.problems({"ONE": ["a\u00fc\u00eb\u00e9\u00e8\u00e0\u00ef\u00eb\u00fc"]})
        self.assertEqual(len(problems), 1, problems)
        listed = ", ".join(f"{chr(c)!r} (U+{c:04X})" for c in (0xE0, 0xE8, 0xE9, 0xEB, 0xEF, 0xFC))
        self.assertIn(listed, problems[0])  # six characters: a set's own order passes this once in 720 runs

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
