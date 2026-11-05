"""The test programs `just test-scripting` runs are the ones its build declares and made (#2069).

A reused build tree keeps the executables of earlier builds. A target that a later configure removes leaves its
executable on disk, and a recipe that runs that file by path then passes on a program no build makes.
tools/check_fresh_test_programs.py fails such a run. These tests drive the tool on fixture trees, and check the
recipe's wiring: each program is removed before the build, judged after it, and judged before it is run; every program
a run_suite, wine or check_test.sh line runs is in the list check_fresh judges; and the list removed before a build is
the list judged after it.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "check_fresh_test_programs.py"

LISTING = """all: phony
build.ninja: RERUN_CMAKE
nevr_script_registry_test.exe: CXX_EXECUTABLE_LINK
vm/luau/nevr_luau_memory_test.exe: CXX_EXECUTABLE_LINK
"""


class Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.listing = self.root / "targets.txt"
        self.listing.write_text(LISTING, encoding="utf-8")

    def tearDown(self):
        self._tmp.cleanup()

    def touch(self, rel, age_days=0):
        path = self.build / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"program")
        if age_days:
            old = time.time() - age_days * 86400
            os.utime(path, (old, old))
        return path

    def tool(self, *programs, targets=None, stdin=None):
        argv = [sys.executable, "-I", str(TOOL), "--build-dir", str(self.build),
                "--targets", "-" if stdin is not None else str(targets or self.listing), *programs]
        return subprocess.run(argv, input=stdin, capture_output=True, text=True, check=False)


class ToolJudgesEachProgram(Fixture):
    def test_declared_and_on_disk_is_fresh(self):
        self.touch("nevr_script_registry_test.exe")
        self.touch("vm/luau/nevr_luau_memory_test.exe")
        r = self.tool("nevr_script_registry_test.exe", "vm/luau/nevr_luau_memory_test.exe")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ok nevr_script_registry_test.exe", r.stdout)
        self.assertIn("ok vm/luau/nevr_luau_memory_test.exe", r.stdout)

    def test_stale_program_fails_and_is_named(self):
        self.touch("nevr_script_registry_test.exe")
        self.touch("nevr_luau_shared_test.exe")
        r = self.tool("nevr_script_registry_test.exe", "nevr_luau_shared_test.exe")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("FAIL", r.stderr)
        self.assertIn("nevr_luau_shared_test.exe: stale", r.stderr)
        self.assertNotIn("nevr_script_registry_test.exe: stale", r.stderr)

    def test_declared_program_not_on_disk_fails_as_missing(self):
        r = self.tool("nevr_script_registry_test.exe")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("nevr_script_registry_test.exe: missing", r.stderr)

    def test_undeclared_and_absent_program_fails_as_undeclared(self):
        r = self.tool("nevr_luau_shared_test.exe")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("nevr_luau_shared_test.exe: undeclared", r.stderr)

    def test_every_program_is_judged_even_after_a_failure(self):
        self.touch("nevr_luau_shared_test.exe")
        self.touch("nevr_script_registry_test.exe")
        r = self.tool("nevr_luau_shared_test.exe", "nevr_script_registry_test.exe")
        self.assertEqual(r.returncode, 1)
        self.assertIn("ok nevr_script_registry_test.exe", r.stdout)
        self.assertIn("nevr_luau_shared_test.exe: stale", r.stderr)

    def test_a_declared_name_is_matched_whole(self):
        # A prefix of a declared output, and a bare name for a declared output in a subdirectory, are not declared.
        self.touch("nevr_script_registry.exe")
        self.touch("luau_memory_test.exe")
        r = self.tool("nevr_script_registry.exe", "luau_memory_test.exe")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("nevr_script_registry.exe: stale", r.stderr)
        self.assertIn("luau_memory_test.exe: stale", r.stderr)

    def test_an_old_declared_program_is_still_fresh(self):
        # An up-to-date program keeps the modification time of the link that made it: no age test.
        self.touch("nevr_script_registry_test.exe", age_days=365)
        r = self.tool("nevr_script_registry_test.exe")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_listing_read_from_standard_input(self):
        self.touch("nevr_script_registry_test.exe")
        r = self.tool("nevr_script_registry_test.exe", stdin=LISTING)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_listing_file_that_cannot_be_read_is_refused(self):
        self.touch("nevr_script_registry_test.exe")
        r = self.tool("nevr_script_registry_test.exe", targets=self.root / "absent.txt")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("cannot read listing", r.stderr)


class ToolRefusesWhatItCannotJudge(Fixture):
    def test_absolute_program_path_is_refused(self):
        self.touch("nevr_script_registry_test.exe")
        r = self.tool(str(self.build / "nevr_script_registry_test.exe"))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("relative to the build directory", r.stderr)

    def test_program_path_that_leaves_the_build_directory_is_refused(self):
        r = self.tool("../nevr_script_registry_test.exe")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("stays inside it", r.stderr)

    def test_empty_listing_is_refused(self):
        self.touch("nevr_script_registry_test.exe")
        r = self.tool("nevr_script_registry_test.exe", stdin="\n\n")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("declares no output", r.stderr)

    def test_malformed_listing_line_is_refused_and_quoted(self):
        self.touch("nevr_script_registry_test.exe")
        r = self.tool("nevr_script_registry_test.exe", stdin="this is not a target line\n")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("this is not a target line", r.stderr)

    def test_missing_build_directory_is_refused(self):
        argv = [sys.executable, "-I", str(TOOL), "--build-dir", str(self.root / "absent"),
                "--targets", str(self.listing), "nevr_script_registry_test.exe"]
        r = subprocess.run(argv, capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("does not exist", r.stderr)

    def test_no_program_named_is_refused(self):
        argv = [sys.executable, "-I", str(TOOL), "--build-dir", str(self.build), "--targets", str(self.listing)]
        r = subprocess.run(argv, capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)


# The recipe's shape, read from the justfile. Each mode removes its programs, builds, judges them, then runs them.
# A program the recipe runs is an argument under $out (a scripting tree) or $check (the script-check tree) on a line
# that runs it: run_suite, wine (the runaway probes) or check_test.sh. Each one must be in the list check_fresh judges
# for its tree: the programs=() list for $out, the words after check_fresh "$check" for $check. The list the recipe
# removes before a tree's build must be the list check_fresh judges after it.
PROGRAMS_ASSIGN = re.compile(r"^\s*programs\+?=\((?P<words>[^)]*)\)", re.M)
RUNNER = re.compile(r"\b(?P<runner>run_suite|wine|check_test\.sh)\b")
PROGRAM_ARG = re.compile(r"\$\{?(?P<tree>out|check)\}?/(?P<program>[^\"'\s;)]+)")
REMOVAL_LOOP = re.compile(r'for p in (?P<words>[^;]*); do rm -f -- "\$(?P<tree>out|check)/\$p"; done')
CHECK_FRESH_CALL = re.compile(r'check_fresh "\$(?P<tree>out|check)"(?P<words>.*)$')


def _code(body):
    # A command continued with a backslash is one line, so a program argument on its second line is still found.
    joined = re.sub(r"\\\n\s*", " ", body)
    return [line for line in joined.splitlines() if not line.lstrip().startswith("#")]


def _index(lines, needle):
    for i, line in enumerate(lines):
        if needle in line:
            return i
    return None


def wiring_problems(body):
    """What is wrong with the test-scripting recipe body as far as the freshness check goes. [] when nothing is."""
    lines = _code(body)
    text = "\n".join(lines)
    problems = []

    def need(label, needle):
        i = _index(lines, needle)
        if i is None:
            problems.append(f"no line for {label} ({needle!r})")
        return i

    rm_out = need("removing the mode's programs before its build", 'rm -f -- "$out/$p"')
    build_out = need("the mode's build", 'cmake --build --preset "scripting-mingw-$mode"')
    check_out = need("the freshness check of the mode's programs", 'check_fresh "$out" "${programs[@]}"')
    first_run = need("the first run of the mode's programs", 'run_suite "$out/')
    if None not in (rm_out, build_out, check_out, first_run) and not rm_out < build_out < check_out < first_run:
        problems.append("the mode's programs are not removed, built, judged, then run, in that order")

    rm_check = need("removing the script-check programs before their build", 'rm -f -- "$check/$p"')
    build_check = need("the script-check build", "cmake --build --preset script-check")
    check_check = need("the freshness check of the script-check programs", 'check_fresh "$check"')
    test_check = need("the script-check runner", "src/scripting/check/check_test.sh")
    if None not in (rm_check, build_check, check_check, test_check) and not rm_check < build_check < check_check < test_check:
        problems.append("the script-check programs are not removed, built, judged, then run, in that order")

    removed = {"out": set(), "check": set()}
    judged = {"out": set(), "check": set()}
    for line in lines:
        for match in REMOVAL_LOOP.finditer(line):
            removed[match.group("tree")].update(match.group("words").split())
        match = CHECK_FRESH_CALL.search(line)
        if match:
            judged[match.group("tree")].update(match.group("words").split())
    for tree in ("out", "check"):
        if removed[tree] and judged[tree] and removed[tree] != judged[tree]:
            problems.append(
                f"the programs removed before the ${tree} build are not the programs check_fresh judges after it "
                f"(only removed: {' '.join(sorted(removed[tree] - judged[tree])) or 'none'}; "
                f"only judged: {' '.join(sorted(judged[tree] - removed[tree])) or 'none'})")

    listed = {"out": set(), "check": judged["check"]}
    for match in PROGRAMS_ASSIGN.finditer(text):
        listed["out"].update(match.group("words").split())
    names = {"out": "no programs=() list names it", "check": 'the check_fresh "$check" list does not name it'}
    for line in lines:
        runner = RUNNER.search(line)
        if runner is None:
            continue
        args = list(PROGRAM_ARG.finditer(line))
        if runner.group("runner") == "check_test.sh":
            args = args[:-1]  # its last argument is the output directory (src/scripting/check/check_test.sh, usage)
        for arg in args:
            tree, program = arg.group("tree"), arg.group("program")
            if program not in listed[tree]:
                problems.append(f"{runner.group('runner')} runs ${tree}/{program}, but {names[tree]}, "
                                "so check_fresh does not judge it")
    return problems


class RecipeWiring(unittest.TestCase):
    def setUp(self):
        self.body = just_recipe.body(just_recipe.read_justfile(), "test-scripting")
        self.assertIsNotNone(self.body, "no test-scripting recipe")

    def replaced(self, old, new=""):
        self.assertIn(old, self.body, "the mutant's text is not in the recipe, so the mutant tests nothing")
        return self.body.replace(old, new, 1)

    def test_the_recipe_judges_every_program_it_runs(self):
        self.assertEqual(wiring_problems(self.body), [])

    def test_a_mode_without_its_freshness_check_is_reported(self):
        problems = wiring_problems(self.replaced('check_fresh "$out" "${programs[@]}"'))
        self.assertTrue(any("freshness check of the mode's programs" in p for p in problems), problems)

    def test_a_mode_without_its_removal_is_reported(self):
        problems = wiring_problems(self.replaced('for p in "${programs[@]}"; do rm -f -- "$out/$p"; done'))
        self.assertTrue(any("removing the mode's programs" in p for p in problems), problems)

    def test_a_check_after_the_run_is_reported(self):
        body = self.replaced('check_fresh "$out" "${programs[@]}"')
        anchor = 'run_suite "$out/nevr_script_registry_test.exe" 20'
        self.assertIn(anchor, body)
        body = body.replace(anchor, anchor + '\n        check_fresh "$out" "${programs[@]}"', 1)
        problems = wiring_problems(body)
        self.assertTrue(any("not removed, built, judged, then run" in p for p in problems), problems)

    def test_a_program_run_outside_every_list_is_reported(self):
        body = self.replaced('run_suite "$out/nevr_script_host_test.exe" 12',
                             'run_suite "$out/unlisted_test.exe" 12')
        problems = wiring_problems(body)
        self.assertTrue(any("unlisted_test.exe" in p for p in problems), problems)

    def test_a_script_check_program_without_its_judgement_is_reported(self):
        problems = wiring_problems(self.replaced(
            'check_fresh "$check" nevr_script_stubs_host_test nevr_script_check'))
        self.assertTrue(any("freshness check of the script-check programs" in p for p in problems), problems)

    def with_probe(self, probe):
        # A probe line added after the mode's two runaway probes, inside the mode loop.
        anchor = 'echo "test-scripting: [$mode] --pattern-dos and --gc-dos stopped"'
        return self.replaced(anchor, probe + "\n        " + anchor)

    def test_a_wine_probe_of_an_unlisted_program_is_reported(self):
        problems = wiring_problems(self.with_probe(
            'dos=$(WINEDEBUG=-all timeout -k 10 120 wine "$out/unlisted.exe" --probe 2>&1)'))
        self.assertTrue(any("wine runs $out/unlisted.exe" in p for p in problems), problems)

    def test_a_wine_probe_that_spans_two_lines_is_reported(self):
        problems = wiring_problems(self.with_probe(
            'dos=$(WINEDEBUG=-all timeout -k 10 120 wine \\\n            "$out/unlisted.exe" --probe 2>&1)'))
        self.assertTrue(any("wine runs $out/unlisted.exe" in p for p in problems), problems)

    def test_a_wine_probe_spelled_with_braces_is_reported(self):
        problems = wiring_problems(self.with_probe('wine "${out}/unlisted.exe" --probe'))
        self.assertTrue(any("wine runs $out/unlisted.exe" in p for p in problems), problems)

    def test_a_listed_probe_is_not_reported(self):
        problems = wiring_problems(self.with_probe('wine "$out/nevr_script_host_test.exe" --probe'))
        self.assertEqual(problems, [])

    def test_a_wine_run_of_an_unjudged_script_check_program_is_reported(self):
        anchor = 'src/scripting/check/check_test.sh "$check/nevr_script_stubs_host_test"'
        problems = wiring_problems(self.replaced(anchor, 'wine "$check/unlisted" --probe\n    ' + anchor))
        self.assertTrue(any("wine runs $check/unlisted" in p for p in problems), problems)

    def test_a_check_test_program_outside_the_judged_list_is_reported(self):
        problems = wiring_problems(self.replaced('"$check/nevr_script_check" "$check/check"',
                                                 '"$check/nevr_script_check" "$check/unlisted" "$check/check"'))
        self.assertTrue(any("check_test.sh runs $check/unlisted" in p for p in problems), problems)

    def test_a_script_check_removal_list_that_differs_from_the_judged_list_is_reported(self):
        problems = wiring_problems(self.replaced(
            'for p in nevr_script_stubs_host_test nevr_script_check; do',
            'for p in nevr_script_stubs_host_test; do'))
        self.assertTrue(any("$check" in p and "only judged: nevr_script_check" in p for p in problems), problems)

    def test_a_script_check_judged_list_that_differs_from_the_removal_list_is_reported(self):
        problems = wiring_problems(self.replaced(
            'check_fresh "$check" nevr_script_stubs_host_test nevr_script_check',
            'check_fresh "$check" nevr_script_check'))
        self.assertTrue(any("$check" in p and "only removed: nevr_script_stubs_host_test" in p for p in problems),
                        problems)

    def test_a_mode_removal_list_that_is_not_its_programs_is_reported(self):
        problems = wiring_problems(self.replaced(
            'for p in "${programs[@]}"; do rm -f -- "$out/$p"; done',
            'for p in nevr_script_registry_test.exe; do rm -f -- "$out/$p"; done'))
        self.assertTrue(any("$out" in p and "only removed: nevr_script_registry_test.exe" in p for p in problems),
                        problems)


if __name__ == "__main__":
    unittest.main()
