"""The test programs `just test-scripting` runs are the ones its build declares and made (#2069).

A reused build tree keeps the executables of earlier builds. A target that a later configure removes leaves its
executable on disk, and a recipe that runs that file by path then passes on a program no build makes.
tools/check_fresh_test_programs.py fails such a run. These tests drive the tool on fixture trees, and check the
recipe's wiring: each program is removed before the build, judged after it, and judged before it is run; every program
a run_suite, wine or check_test.sh line runs is in the list check_fresh judges for its tree, in each mode that runs it;
and the list removed before a build is the list judged after it.
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


# The recipe's shape, read from the justfile. Each mode of the `for mode in ...` loop removes its programs, builds,
# judges them, then runs them. A program the recipe runs is an argument under $out (a scripting tree) or $check (the
# script-check tree) on a line that runs it: run_suite, wine (the runaway probes) or check_test.sh.
# The scan walks the loop once per mode. A line in an `if [[ "$mode" == <mode> ]]` branch belongs to that mode only,
# and every other line of the loop to each mode, so a mode's programs=() list is the base list plus what its own
# branch appends (programs+=()). A program a mode runs must be in the list check_fresh "$out" judges in that mode; a
# $check program must be in the words after check_fresh "$check". The list the recipe removes before a tree's build
# must be the list check_fresh judges after it. A line the scan cannot place is reported, not passed: another test of
# $mode, an else or elif branch of one, a run of $out outside the loop, or a run that names a build tree by its path or
# names $out or $check in another form.
PROGRAMS_ASSIGN = re.compile(r"^\s*programs(?P<op>\+?=)\((?P<words>[^)]*)\)")
RUNNER = re.compile(r"\b(?P<runner>run_suite|wine|check_test\.sh)\b")
PROGRAM_ARG = re.compile(r"\$\{?(?P<tree>out|check)\}?/(?P<program>[^\"'\s;)]+)")
TREE_REF = re.compile(r"\$\{?(?:out|check)")
TREE_PATH = re.compile(r"build/(?:scripting-mingw-|script-check)")
REMOVAL_LOOP = re.compile(r'for p in (?P<words>[^;]*); do rm -f -- "\$(?P<tree>out|check)/\$p"; done')
CHECK_FRESH_CALL = re.compile(r'check_fresh "\$(?P<tree>out|check)"(?P<words>.*)$')
MODE_LOOP = re.compile(r"^(?P<indent>\s*)for mode in (?P<modes>[^;]+); do\s*$")
MODE_IF = re.compile(r'^\s*if \[\[ "\$mode" == (?P<mode>[\w-]+) \]\]; then\s*$')
MODE_TEST = re.compile(r'(?:\[\[?|\btest)\s[^]]*\$\{?mode\b|\bcase\s+"?\$\{?mode\b')
ALL_PROGRAMS = "${programs[@]}"


def _code(body):
    # A command continued with a backslash is one line, so a program argument on its second line is still found.
    joined = re.sub(r"\\\n\s*", " ", body)
    return [line for line in joined.splitlines() if not line.lstrip().startswith("#")]


def _index(lines, needle):
    for i, line in enumerate(lines):
        if needle in line:
            return i
    return None


def _words(text):
    return [word.strip("\"'") for word in text.split()]


def _runs(line, problems):
    """The programs a line runs, as (runner, tree, program). A build tree it names another way is a problem."""
    runner = RUNNER.search(line)
    if runner is None:
        return []
    name = runner.group("runner")
    args = list(PROGRAM_ARG.finditer(line))
    if TREE_PATH.search(line):
        problems.append(f"{name} names a build tree by its path, not through $out or $check, so no list check_fresh "
                        f"judges covers what it runs: {line.strip()}")
    if len(TREE_REF.findall(line)) != len(args):
        problems.append(f"{name} names $out or $check in a form the scan does not read: {line.strip()}")
    if name == "check_test.sh":
        args = args[:-1]  # its last argument is the output directory (src/scripting/check/check_test.sh, usage)
    return [(name, arg.group("tree"), arg.group("program")) for arg in args]


def _place(lines, problems):
    """The modes of the mode loop, and each line with the modes it runs in (None outside the loop) and its runs."""
    start = end = None
    modes = []
    for i, line in enumerate(lines):
        loop = MODE_LOOP.match(line)
        if loop is not None:
            start, modes = i, loop.group("modes").split()
            done = loop.group("indent") + "done"
            end = next((j for j in range(i + 1, len(lines)) if lines[j].rstrip() == done), None)
            break
    if start is None or end is None:
        problems.append("no `for mode in ...; do` loop with its `done`, so no mode's programs can be read")
    placed = []
    branches = []  # one entry per open `if`: the mode its branch runs in, or None for a test of anything else
    for i, line in enumerate(lines):
        stripped = line.strip()
        mode_if = MODE_IF.match(line)
        if mode_if is None and MODE_TEST.search(line):
            problems.append(f"a test of $mode the scan does not read, so it cannot tell which mode runs: {stripped}")
        if mode_if is not None:
            branches.append(mode_if.group("mode"))
        elif stripped == "fi":
            if not branches:
                problems.append("a `fi` with no `if` the scan read")
            else:
                branches.pop()
        elif re.match(r"(?:else|elif)\b", stripped):
            if branches and branches[-1] is not None:
                problems.append(f"an {stripped.split()[0]} branch of a test of $mode, which the scan does not read: "
                                f"{stripped}")
        elif re.match(r"if\b", stripped) and not re.search(r"\bfi$", stripped):
            branches.append(None)
        in_loop = start is not None and end is not None and start < i < end
        line_modes = None
        if in_loop:
            line_modes = {m for m in modes if all(b is None or b == m for b in branches)}
        placed.append((line, line_modes, _runs(line, problems)))
    if branches:
        problems.append("an `if` with no `fi` the scan read")
    return modes, placed


def _expand(words, programs, label, problems):
    expanded = []
    for word in _words(words):
        if word != ALL_PROGRAMS:
            expanded.append(word)
        elif programs is None:
            problems.append(f'{label} names "{ALL_PROGRAMS}" before any programs=() list')
        else:
            expanded.extend(programs)
    return expanded


def _mode_problems(modes, placed, problems):
    """Each mode's runs of $out programs against the list check_fresh "$out" judges in that mode."""
    unjudged = {}
    for mode in modes:
        programs = removed = judged = None
        runs = []
        for line, line_modes, line_runs in placed:
            if line_modes is None or mode not in line_modes:
                continue
            assign = PROGRAMS_ASSIGN.match(line)
            if assign is not None:
                words = _words(assign.group("words"))
                programs = (programs or []) + words if assign.group("op") == "+=" else words
            for match in REMOVAL_LOOP.finditer(line):
                if match.group("tree") == "out":
                    removed = _expand(match.group("words"), programs, f"the removal loop of mode {mode}", problems)
            match = CHECK_FRESH_CALL.search(line)
            if match is not None and match.group("tree") == "out":
                judged = _expand(match.group("words"), programs, f'check_fresh "$out" in mode {mode}', problems)
            runs.extend((runner, program) for runner, tree, program in line_runs if tree == "out")
        if removed is None:
            problems.append(f"no removal loop runs in mode {mode}, so a program on disk there can be an earlier "
                            "build's")
        if judged is None:
            problems.append(f'check_fresh "$out" does not run in mode {mode}, so no program of that mode is judged')
            continue
        if removed is not None and set(removed) != set(judged):
            problems.append(
                f"in mode {mode}, the programs removed before the $out build are not the programs check_fresh judges "
                f"after it (only removed: {' '.join(sorted(set(removed) - set(judged))) or 'none'}; "
                f"only judged: {' '.join(sorted(set(judged) - set(removed))) or 'none'})")
        for runner, program in runs:
            if program not in judged:
                in_modes = unjudged.setdefault((runner, program), [])
                if mode not in in_modes:
                    in_modes.append(mode)
    for (runner, program), in_modes in unjudged.items():
        problems.append(f"{runner} runs $out/{program} in mode{'s' if len(in_modes) > 1 else ''} "
                        f"{', '.join(in_modes)}, but check_fresh does not judge it there")


def wiring_problems(body):
    """What is wrong with the test-scripting recipe body as far as the freshness check goes. [] when nothing is."""
    lines = _code(body)
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

    modes, placed = _place(lines, problems)
    _mode_problems(modes, placed, problems)

    check_removed, check_judged = set(), set()
    for line, _, _ in placed:
        for match in REMOVAL_LOOP.finditer(line):
            if match.group("tree") == "check":
                check_removed.update(_words(match.group("words")))
        match = CHECK_FRESH_CALL.search(line)
        if match is not None and match.group("tree") == "check":
            check_judged.update(_words(match.group("words")))
    if check_removed and check_judged and check_removed != check_judged:
        problems.append(
            "the programs removed before the $check build are not the programs check_fresh judges after it "
            f"(only removed: {' '.join(sorted(check_removed - check_judged)) or 'none'}; "
            f"only judged: {' '.join(sorted(check_judged - check_removed)) or 'none'})")
    for line, line_modes, line_runs in placed:
        for runner, tree, program in line_runs:
            if tree == "check" and program not in check_judged:
                problems.append(f'{runner} runs $check/{program}, but the check_fresh "$check" list does not name it, '
                                "so check_fresh does not judge it")
            elif tree == "out" and line_modes is None:
                problems.append(f"{runner} runs $out/{program} outside the mode loop, where no mode's check_fresh "
                                "judges it")
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
        self.assertTrue(any("$out" in p and "nevr_script_host_test.exe" in p.partition("only judged: ")[2]
                            for p in problems), problems)

    # The shared state's own test: the shared mode appends it to its list and runs it in a branch of its own.
    SHARED_RUN = ('        if [[ "$mode" == shared ]]; then\n'
                  '            run_suite "$out/nevr_luau_shared_test.exe" 10\n'
                  '        fi\n')
    SHARED_RUN_COMMON = '        run_suite "$out/nevr_luau_shared_test.exe" 10\n'

    def shared_branch_with(self, line):
        # The shared run branch with `line` added before its `fi`.
        return self.replaced(self.SHARED_RUN, self.SHARED_RUN.replace("        fi\n", line + "        fi\n"))

    def only_in_shared(self, line):
        # `line`, a whole line of the mode loop, moved into a branch that runs in the shared mode only.
        return self.replaced(line, '        if [[ "$mode" == shared ]]; then\n    ' + line + "        fi\n")

    def test_a_shared_only_program_run_in_both_modes_is_reported(self):
        # The run moved into the common loop: the pervm mode runs a program its list does not name.
        problems = wiring_problems(self.replaced(self.SHARED_RUN, self.SHARED_RUN_COMMON))
        self.assertTrue(any("run_suite runs $out/nevr_luau_shared_test.exe in mode pervm" in p for p in problems),
                        problems)

    def test_a_shared_only_program_run_in_the_other_mode_is_reported(self):
        problems = wiring_problems(self.replaced(self.SHARED_RUN, self.SHARED_RUN.replace("== shared", "== pervm")))
        self.assertTrue(any("run_suite runs $out/nevr_luau_shared_test.exe in mode pervm" in p for p in problems),
                        problems)

    def test_a_shared_only_probe_in_its_own_branch_is_not_reported(self):
        body = self.shared_branch_with('            wine "$out/nevr_luau_shared_test.exe" --probe\n')
        self.assertEqual(wiring_problems(body), [])

    def test_a_mode_test_the_scan_does_not_read_is_reported(self):
        one_line = '        [[ "$mode" == shared ]] && ' + self.SHARED_RUN_COMMON.lstrip()
        problems = wiring_problems(self.replaced(self.SHARED_RUN, one_line))
        self.assertTrue(any("test of $mode" in p for p in problems), problems)

    def test_an_else_branch_of_a_mode_test_is_reported(self):
        problems = wiring_problems(self.shared_branch_with(
            '        else\n            wine "$out/nevr_script_host_test.exe" --probe\n'))
        self.assertTrue(any("else" in p and "$mode" in p for p in problems), problems)

    def test_a_freshness_check_in_one_mode_only_is_reported(self):
        problems = wiring_problems(self.only_in_shared('        check_fresh "$out" "${programs[@]}"\n'))
        self.assertTrue(any('check_fresh "$out"' in p and "mode pervm" in p for p in problems), problems)

    def test_a_removal_in_one_mode_only_is_reported(self):
        problems = wiring_problems(self.only_in_shared(
            '        for p in "${programs[@]}"; do rm -f -- "$out/$p"; done\n'))
        self.assertTrue(any("no removal loop runs in mode pervm" in p for p in problems), problems)

    def test_a_scripting_program_run_after_the_mode_loop_is_reported(self):
        anchor = 'check="$PWD/build/script-check"'
        body = self.replaced(anchor, 'wine "$out/nevr_script_host_test.exe" --probe\n    ' + anchor)
        problems = wiring_problems(body)
        self.assertTrue(any("wine runs $out/nevr_script_host_test.exe outside the mode loop" in p for p in problems),
                        problems)

    def test_a_probe_that_names_its_tree_by_path_is_reported(self):
        problems = wiring_problems(self.with_probe('wine "$PWD/build/scripting-mingw-$mode/unlisted.exe" --probe'))
        self.assertTrue(any("wine" in p and "build/scripting-mingw-" in p and "$out" in p for p in problems), problems)

    def test_a_script_check_run_that_names_its_tree_by_path_is_reported(self):
        anchor = 'src/scripting/check/check_test.sh "$check/nevr_script_stubs_host_test"'
        body = self.replaced(anchor, 'wine "$PWD/build/script-check/unlisted" --probe\n    ' + anchor)
        problems = wiring_problems(body)
        self.assertTrue(any("wine" in p and "build/script-check" in p and "$check" in p for p in problems), problems)

    def test_a_tree_variable_in_a_form_the_scan_does_not_read_is_reported(self):
        problems = wiring_problems(self.with_probe('wine "$out"/unlisted.exe --probe'))
        self.assertTrue(any("wine" in p and "in a form the scan does not read" in p for p in problems), problems)

    def test_a_probe_inside_another_if_is_still_judged(self):
        probe = 'if true; then\n            wine "$out/unlisted.exe" --probe\n        fi'
        problems = wiring_problems(self.with_probe(probe))
        self.assertTrue(any("wine runs $out/unlisted.exe in modes shared, pervm" in p for p in problems), problems)

    def test_a_recipe_without_the_mode_loop_is_reported(self):
        problems = wiring_problems(self.replaced("for mode in shared pervm; do", "for m in shared pervm; do"))
        self.assertTrue(any("no `for mode in ...; do` loop" in p for p in problems), problems)

    def test_an_if_without_its_fi_is_reported(self):
        problems = wiring_problems(self.replaced(self.SHARED_RUN, self.SHARED_RUN.replace("        fi\n", "")))
        self.assertTrue(any("an `if` with no `fi`" in p for p in problems), problems)

    def test_a_fi_without_its_if_is_reported(self):
        problems = wiring_problems(self.replaced(self.SHARED_RUN, self.SHARED_RUN_COMMON + "        fi\n"))
        self.assertTrue(any("a `fi` with no `if`" in p for p in problems), problems)

    def test_the_program_list_named_before_it_is_set_is_reported(self):
        problems = wiring_problems(self.replaced("programs=(nevr_script_registry_test.exe",
                                                 "progs=(nevr_script_registry_test.exe"))
        self.assertTrue(any('names "${programs[@]}" before any programs=() list' in p for p in problems), problems)

if __name__ == "__main__":
    unittest.main()
