"""Every workflow job that runs a just recipe using the NDK sets the NDK up first, with the one composite action.

`test-quest-hooks` copies the NDK's jni.h (`$ANDROID_NDK_HOME`, with no default), so a runner without ANDROID_NDK_HOME
fails that suite and with it `just verify`: the v5.5.0 release build stopped at
"Verify" for this (#837). The NDK is located, downloaded and selected in `.github/actions/setup-ndk` only; a job that
carries its own copy of those steps can drift from the others, which is the fault this test exists to catch.
"""
from __future__ import annotations

import pathlib
import re
import unittest

import yaml

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = sorted((REPO / ".github/workflows").glob("*.yml"))
ACTION = REPO / ".github/actions/setup-ndk/action.yml"
JUSTFILE = REPO / "justfile"
USES = "./.github/actions/setup-ndk"


def verify_suites(justfile_text: str) -> list[str]:
    """The recipes `just verify` hands to tools/verify-parallel.sh."""
    match = re.search(r"^verify:\n(.*?)(?=^\S)", justfile_text, re.S | re.M)
    if match is None:
        raise AssertionError("no `verify:` recipe in justfile")
    code = "\n".join(l for l in match.group(1).splitlines() if not l.lstrip().startswith("#"))
    call = re.search(r"verify-parallel\.sh((?:[ \t]*\\\n[ \t]*[\w-]+|[ \t]+[\w-]+)+)", code)
    if call is None:
        raise AssertionError("`verify:` does not call tools/verify-parallel.sh")
    suites = re.findall(r"[\w-]+", call.group(1).replace("\\", " "))
    if not suites:
        raise AssertionError("tools/verify-parallel.sh is called with no suites")
    return suites


def recipe(justfile_text: str, name: str) -> tuple[list[str], str]:
    """One recipe's dependencies (the names after the header's colon) and its body (to the next column-0 line)."""
    match = re.search(
        rf"^{re.escape(name)}(?:[ \t][^\n:]*)?:[ \t]*([^\n]*)\n((?:[ \t][^\n]*\n|\n)*)", justfile_text, re.M
    )
    if match is None:
        raise AssertionError(f"no recipe `{name}` in justfile")
    header_tail = match.group(1).split("#", 1)[0]
    deps = [d for d in re.findall(r"[\w-]+", header_tail) if re.search(rf"^{re.escape(d)}(?:[ \t][^\n:]*)?:", justfile_text, re.M)]
    return deps, match.group(2)


def uses_ndk(justfile_text: str, name: str, seen: frozenset[str] = frozenset()) -> bool:
    """Does the recipe, or a recipe it depends on, read the NDK (`{{ ndk }}`, ANDROID_NDK_HOME)?"""
    if name in seen or not re.search(rf"^{re.escape(name)}(?:[ \t][^\n:]*)?:", justfile_text, re.M):
        return False  # a word after `just` in a script that is not a recipe
    deps, body = recipe(justfile_text, name)
    if re.search(r"\{\{\s*ndk\s*\}\}|ANDROID_NDK_HOME", body):
        return True
    return any(uses_ndk(justfile_text, d, seen | {name}) for d in deps)


def recipes_run_by(step: dict, justfile_text: str) -> list[str]:
    """The just recipes one workflow step runs; `just verify` stands for its suites."""
    found: list[str] = []
    code = "\n".join(l for l in str(step.get("run", "")).splitlines() if not l.lstrip().startswith("#"))
    for name in re.findall(r"\bjust\"?[ \t]+([\w][\w-]*)", code):
        found.extend(verify_suites(justfile_text) if name == "verify" else [name])
    return found


def jobs_needing_the_ndk(workflow: dict, justfile_text: str) -> dict[str, int]:
    """job name -> index of its first step that runs a recipe using the NDK."""
    result: dict[str, int] = {}
    for job_name, job in workflow["jobs"].items():
        for index, step in enumerate(job.get("steps", [])):
            if any(uses_ndk(justfile_text, r) for r in recipes_run_by(step, justfile_text)):
                result[job_name] = index
                break
    return result


def sets_up_ndk(step: dict) -> bool:
    return step.get("uses") == USES


def load_workflows() -> dict[str, dict]:
    return {path.name: yaml.safe_load(path.read_text()) for path in WORKFLOWS}


class WorkflowsNdkTest(unittest.TestCase):
    def setUp(self):
        self.workflows = load_workflows()
        self.justfile = JUSTFILE.read_text()

    def needing(self) -> dict[tuple[str, str], int]:
        return {
            (name, job): first
            for name, workflow in self.workflows.items()
            for job, first in jobs_needing_the_ndk(workflow, self.justfile).items()
        }

    def test_the_scan_sees_verify_the_quest_build_and_the_android_workflow(self):
        for suite in verify_suites(self.justfile):
            recipe(self.justfile, suite)  # raises when a verify suite is not a recipe
        self.assertIn("test-quest-hooks", verify_suites(self.justfile))
        self.assertTrue(uses_ndk(self.justfile, "test-quest-hooks"))
        self.assertTrue(uses_ndk(self.justfile, "test-android"), "test-android reaches {{ ndk }} through a dependency")
        needing = self.needing()
        self.assertIn(("build.yml", "build"), needing, "the build job runs `just verify`, which runs test-quest-hooks")
        self.assertIn(("build.yml", "quest"), needing, "the quest job runs `just test-android`")
        self.assertIn(("android.yml", "android"), needing, "the android job runs `just build-android`")

    def test_each_such_job_uses_the_composite_action_before_the_recipe(self):
        for (name, job_name), first in self.needing().items():
            steps = self.workflows[name]["jobs"][job_name]["steps"]
            before = [i for i, s in enumerate(steps) if sets_up_ndk(s)]
            self.assertTrue(before, f"{name} job `{job_name}` runs a recipe using the NDK and never uses {USES}")
            self.assertLess(
                min(before), first, f"{name} job `{job_name}` uses {USES} after the step that runs a recipe using the NDK"
            )

    def test_no_job_sets_the_ndk_up_by_itself(self):
        for path in WORKFLOWS:
            text = path.read_text()
            self.assertNotIn("ANDROID_NDK_HOME=", text, f"{path.name}: the composite action exports ANDROID_NDK_HOME")
            self.assertNotIn("android-ndk-r26d", text, f"{path.name}: the NDK download belongs to the composite action")
        for name, workflow in self.workflows.items():
            for job_name, job in workflow["jobs"].items():
                self.assertNotIn("NDK_REVISION", job.get("env", {}), f"{name} job `{job_name}` pins its own NDK revision")

    def test_a_job_that_reads_the_selected_revision_has_the_step_that_makes_it(self):
        for name, workflow in self.workflows.items():
            for job_name, job in workflow["jobs"].items():
                if "steps.ndk." not in yaml.safe_dump(job.get("steps", [])):
                    continue
                made = [s for s in job["steps"] if s.get("id") == "ndk" and sets_up_ndk(s)]
                self.assertEqual(
                    len(made), 1, f"{name} job `{job_name}` reads steps.ndk.* but has no `id: ndk` step using {USES}"
                )

    def test_the_action_exports_the_ndk_and_its_revision(self):
        action = yaml.safe_load(ACTION.read_text())
        self.assertEqual(action["runs"]["using"], "composite")
        text = ACTION.read_text()
        self.assertRegex(text, r'echo "ANDROID_NDK_HOME=\$ndk" >> "\$GITHUB_ENV"')
        self.assertIn("revision", action["outputs"])
        self.assertRegex(text, r"26\.3\.11579264")
        self.assertIn("Pkg.Revision", text, "the action checks the revision it selected")


class ScanTest(unittest.TestCase):
    """The scan itself, on text it can be wrong about."""

    JUSTFILE = (
        'ndk := env("ANDROID_NDK_HOME", "/x")\n'
        "\n"
        "verify:\n"
        "    #!/usr/bin/env bash\n"
        "    bash tools/verify-parallel.sh \\\n"
        "        alpha \\\n"
        "        beta\n"
        "    echo done\n"
        "\n"
        "alpha:\n"
        "    echo plain\n"
        "\n"
        'beta arg="x":\n'
        '    ls "{{ ndk }}/include"\n'
        "\n"
        "gamma: delta\n"
        "    echo none\n"
        "\n"
        "delta:\n"
        "    echo {{ndk}}\n"
    )

    def test_it_reads_the_suites_dependencies_and_which_use_the_ndk(self):
        self.assertEqual(verify_suites(self.JUSTFILE), ["alpha", "beta"])
        self.assertFalse(uses_ndk(self.JUSTFILE, "alpha"))
        self.assertTrue(uses_ndk(self.JUSTFILE, "beta"))
        self.assertTrue(uses_ndk(self.JUSTFILE, "gamma"), "through its dependency")
        self.assertFalse(uses_ndk(self.JUSTFILE, "verify"))

    def test_it_finds_the_recipes_a_step_runs(self):
        self.assertEqual(recipes_run_by({"run": "just verify"}, self.JUSTFILE), ["alpha", "beta"])
        self.assertEqual(recipes_run_by({"run": '"$HOME/.local/bin/just" gamma'}, self.JUSTFILE), ["gamma"])
        self.assertEqual(recipes_run_by({"run": '"$HOME/.local/bin/just" --version'}, self.JUSTFILE), [])
        self.assertEqual(recipes_run_by({"run": "# the job runs just verify later\nmake"}, self.JUSTFILE), [])


if __name__ == "__main__":
    unittest.main()
