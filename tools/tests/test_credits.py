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


if __name__ == "__main__":
    unittest.main()
