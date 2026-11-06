"""The scenario and scenario-all recipes pass each argument to the runner as one argument, spaces and all.

The recipe is taken from the repo's justfile and run with a fake `just` (so the mingw-scenario build step does nothing)
and fake runners that record their argv.
"""

import json
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import just_recipe

REAL_JUST = shutil.which("just")


def recipe_with_attributes(text: str, name: str) -> str:
    """The recipe as the justfile writes it, with the attribute line directly above its header."""
    head = just_recipe.header(text, name)
    assert head is not None, name
    lines = text.splitlines()
    index = lines.index(head)
    above = [lines[index - 1] + "\n"] if index > 0 and lines[index - 1].startswith("[") else []
    return "".join(above) + just_recipe.recipe(text, name)


class ScenarioRecipeArgs(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="scenario-recipe-args-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        fake_just = bin_dir / "just"
        fake_just.write_text("#!/bin/sh\nexit 0\n")
        fake_just.chmod(fake_just.stat().st_mode | stat.S_IXUSR)
        scenario_dir = self.tmp / "tools" / "scenario"
        scenario_dir.mkdir(parents=True)
        for runner in ("run_scenario.py", "run_all.py"):
            (scenario_dir / runner).write_text(
                "import json, os, sys\n"
                "json.dump(sys.argv[1:], open(os.path.join(os.environ['ARGV_OUT'], os.path.basename(sys.argv[0]) + '.json'), 'w'))\n"
            )
        text = just_recipe.read_justfile()
        (self.tmp / "justfile").write_text(recipe_with_attributes(text, "scenario") + "\n" +
                                           recipe_with_attributes(text, "scenario-all"))

    def run_recipe(self, *args):
        env = dict(os.environ, PATH=f"{self.tmp / 'bin'}:{os.environ['PATH']}", ARGV_OUT=str(self.tmp))
        result = subprocess.run([REAL_JUST, "--justfile", str(self.tmp / "justfile"), *args], cwd=self.tmp, env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def argv(self, runner):
        return json.loads((self.tmp / f"{runner}.json").read_text())

    def test_scenario_keeps_an_argument_with_a_space_whole(self):
        self.run_recipe("scenario", "invite", "--config", "/a dir/config.json")
        self.assertEqual(self.argv("run_scenario.py"),
                         ["tools/scenario/scenarios/invite.yaml", "--config", "/a dir/config.json"])

    def test_scenario_without_extra_arguments(self):
        self.run_recipe("scenario", "invite")
        self.assertEqual(self.argv("run_scenario.py"), ["tools/scenario/scenarios/invite.yaml"])

    def test_scenario_all_keeps_an_argument_with_a_space_whole(self):
        self.run_recipe("scenario-all", "--config", "/a dir/config.json")
        self.assertEqual(self.argv("run_all.py"), ["--config", "/a dir/config.json"])

    def test_scenario_all_without_arguments(self):
        self.run_recipe("scenario-all")
        self.assertEqual(self.argv("run_all.py"), [])


if __name__ == "__main__":
    unittest.main()
