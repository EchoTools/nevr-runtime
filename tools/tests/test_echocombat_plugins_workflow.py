"""The EchoCombat plugin workflow: hand-started, the Windows exception, no secrets, every action pinned."""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"
NAME = "echocombat-plugins.yml"
# The overlay is built from marshmallow-mia's source at this commit, the default of the overlay_sha input; the
# v1.0.0 release file's hash is retired.
OVERLAY_COMMIT = "309e2196029250729c8e349b92bfcb1eda9f682b"
OVERLAY_REPO = "marshmallow-mia/NvrContentOverlay"
RETIRED_OVERLAY_SHA256 = "0d37e3fe6d99b4e087969d4292278d4b68d1c52c890d8208db3397d5a1aacbbc"
# The v1.1.0 release asset of the overlay, as its author published it, and the commit that release is tagged at.
PUBLISHED_SHA256 = "0cce65cf9f75eecea7042af4a38d895db9b96946b42f39be2f4ada1b4eb1bcdf"
EXPECTED_INPUTS = ["echocombat_sha", "echocombat_repo", "overlay_repo", "overlay_sha", "nevr_runtime_sha"]
REPO_RE = "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$"
SHA_RE = "^[0-9a-f]{40}$"
# The plugin SDK headers (sdk/include) come from this public repository at the nevr_runtime_sha input's default.
SDK_REPO = "EchoTools/nevr-runtime"
NEVR_RUNTIME_SHA_DEFAULT = "9b90be1b228abeb297b216aa4af49e3836a8d31d"
# The cl.exe flags that make the plugins build against the SDK headers: C++17, and the SDK include path relative to
# the EchoCombat checkout (the working directory of the build step).
CL_SDK_FLAGS = ["/std:c++17", r"/I..\nevr-runtime\sdk\include"]
# Every workflow allowed a windows- runner, by file name. defender-scan.yml is a hand-started Defender
# measurement that predates this file; any other file that gains a Windows runner fails the test.
WINDOWS_ALLOWED = {NAME, "defender-scan.yml"}


def code_lines(text):
    """The workflow's lines without comments, so prose about a rule cannot satisfy or break its check."""
    return [line for line in text.splitlines() if not line.lstrip().startswith("#")]


def run_blocks(text):
    """The body of every `run:` key (its own line and the more-indented lines under it)."""
    lines = text.splitlines()
    blocks = []
    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)(?:- )?run:(.*)$", line)
        if not m:
            continue
        indent = len(m.group(1))
        body = [m.group(2)]
        for nxt in lines[i + 1:]:
            if nxt.strip() and len(nxt) - len(nxt.lstrip()) <= indent:
                break
            body.append(nxt)
        blocks.append("\n".join(body))
    return blocks


def job_block(text, name):
    """The text of one job under `jobs:` (its header line and everything up to the next job header), or None."""
    m = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  \w+:\n|\Z)", text, re.M | re.S)
    return m.group(1) if m else None


def job_steps(job):
    """Each step of a job's text, from its `- name:` line to the next step, in order."""
    return ["    - name:" + part for part in job.split("    - name:")[1:]]


def input_block(on_text, name):
    """The attributes of one workflow_dispatch input (the lines indented under its name), or None."""
    m = re.search(rf"(?m)^      {re.escape(name)}:\n((?:        .*\n)+)", on_text)
    return m.group(1) if m else None


def runs_on(text):
    """The value of every `runs-on:` key in the text's code lines, in order."""
    return [line.split(":", 1)[1].strip() for line in code_lines(text) if re.match(r"^\s*runs-on:", line)]


def pinned_uses(text):
    """(action, sha) for every `uses:` in the workflow's code lines; a local or unpinned one gives sha None."""
    found = []
    for line in code_lines(text):
        m = re.match(r"^\s*(?:- )?uses:\s*(\S+)", line)
        if m:
            ref = m.group(1)
            action, _, sha = ref.partition("@")
            found.append((action, sha if re.fullmatch(r"[0-9a-f]{40}", sha) else None))
    return found


class EchocombatPluginsWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.text = (WORKFLOWS / NAME).read_text(encoding="utf-8")

    def test_it_triggers_only_on_workflow_dispatch(self):
        on_block = self.text.split("\non:\n", 1)[1].split("\npermissions:", 1)[0]
        events = re.findall(r"^  (\w+):", on_block, re.M)
        self.assertEqual(events, ["workflow_dispatch"])
        self.assertIn("echocombat_sha:", on_block)

    def test_it_is_the_only_workflow_with_a_windows_runner_other_than_the_known_one(self):
        windows = set()
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for line in code_lines(path.read_text(encoding="utf-8")):
                if re.match(r"^\s*(?:- )?runs-on:\s*windows-", line):
                    windows.add(path.name)
        self.assertEqual(windows, WINDOWS_ALLOWED)

    def test_it_references_no_secret(self):
        for line in code_lines(self.text):
            self.assertNotRegex(line, r"\bsecrets[.\[]", line)

    def test_every_uses_is_pinned_to_a_full_sha_that_build_yml_pins_for_the_same_action(self):
        build_pins = {}
        for action, sha in pinned_uses((WORKFLOWS / "build.yml").read_text(encoding="utf-8")):
            if sha:
                build_pins.setdefault(action, set()).add(sha)
        uses = pinned_uses(self.text)
        self.assertGreaterEqual(len(uses), 4)
        for action, sha in uses:
            self.assertIsNotNone(sha, f"{action} is not pinned to a 40-hex SHA")
            self.assertIn(sha, build_pins.get(action, set()), f"{action}@{sha} is not a pin build.yml uses")

    def test_every_checkout_keeps_no_credentials(self):
        checkouts = [a for a, _ in pinned_uses(self.text) if a == "actions/checkout"]
        # this repository (build), EchoCombat, Detours, this repository (overlay), NvrContentOverlay
        self.assertEqual(len(checkouts), 5)
        self.assertEqual(self.text.count("persist-credentials: false"), len(checkouts))

    def test_the_input_is_validated_against_a_full_sha_before_any_checkout(self):
        self.assertIn("^[0-9a-f]{40}$", self.text)
        self.assertLess(self.text.index("^[0-9a-f]{40}$"), self.text.index("actions/checkout@"))

    def test_no_input_is_interpolated_into_a_run_block(self):
        blocks = run_blocks(self.text)
        self.assertGreaterEqual(len(blocks), 8)
        for block in blocks:
            self.assertNotIn("${{ inputs.", block)
        self.assertIn("${{ inputs.echocombat_sha }}", self.text)

    def test_inputs_appear_only_as_a_whole_env_or_with_value_or_an_if(self):
        value = r"^\s*(?:- )?(?:[A-Za-z_][A-Za-z0-9_]*:\s*)?\$\{\{ inputs\.\w+ \}\}\s*$"
        condition = r"^\s*if: \$\{\{ inputs\.\w+ == '[0-9a-f]{40}' \}\}\s*$"
        seen = 0
        for line in code_lines(self.text):
            if "${{ inputs." in line:
                seen += 1
                self.assertTrue(re.match(value, line) or re.match(condition, line), line)
        self.assertGreaterEqual(seen, 8)

    def test_the_five_inputs_exist_as_strings_with_their_defaults(self):
        on = self.text.split("\non:\n", 1)[1].split("\npermissions:", 1)[0]
        self.assertEqual(re.findall(r"(?m)^      (\w+):$", on), EXPECTED_INPUTS)
        for name in EXPECTED_INPUTS:
            block = input_block(on, name)
            self.assertIsNotNone(block, name)
            self.assertIn("type: string", block, name)
        sha = input_block(on, "echocombat_sha")
        self.assertIn("required: true", sha)
        self.assertNotIn("default:", sha)
        self.assertRegex(input_block(on, "echocombat_repo"), r'(?m)^        default: "heisthecat31/EchoCombat"$')
        self.assertRegex(input_block(on, "overlay_repo"), rf'(?m)^        default: "{re.escape(OVERLAY_REPO)}"$')
        self.assertRegex(input_block(on, "overlay_sha"), rf'(?m)^        default: "{OVERLAY_COMMIT}"$')
        self.assertRegex(input_block(on, "nevr_runtime_sha"), rf'(?m)^        default: "{NEVR_RUNTIME_SHA_DEFAULT}"$')

    def test_each_job_that_checks_out_a_third_party_validates_repo_and_sha_in_its_first_step(self):
        for job, repo_input in (("build", "echocombat_repo"), ("overlay", "overlay_repo")):
            steps = job_steps(job_block(self.text, job))
            self.assertTrue(steps, job)
            first = "\n".join(code_lines(steps[0]))
            self.assertIn(REPO_RE, first, job)
            self.assertIn(SHA_RE, first, job)
            self.assertIn("exit 1", first, job)
            self.assertIn("${{ inputs." + repo_input + " }}", first, job)
            self.assertNotIn("actions/checkout", first, job)

    def test_each_job_validates_the_nevr_runtime_sha_input_in_its_first_step(self):
        for job in ("build", "overlay"):
            first = "\n".join(code_lines(job_steps(job_block(self.text, job))[0]))
            self.assertIn("NEVR_RUNTIME_SHA: ${{ inputs.nevr_runtime_sha }}", first, job)
            self.assertIn('[[ ! "$NEVR_RUNTIME_SHA" =~ ^[0-9a-f]{40}$ ]]', first, job)
            self.assertIn("exit 1", first, job)
            self.assertNotIn("actions/checkout", first, job)

    def test_each_job_checks_out_the_nevr_sdk_headers_from_echotools_at_the_input_commit(self):
        for job in ("build", "overlay"):
            steps = job_steps(job_block(self.text, job))
            sdk = [s for s in steps if "uses: actions/checkout@" in s and re.search(r"(?m)^        path: nevr-runtime$", s)]
            self.assertEqual(len(sdk), 1, job)
            step = sdk[0]
            self.assertTrue(step.startswith("    - name: Check out the nevr-runtime plugin SDK headers"), step[:80])
            for line in (
                f"repository: {SDK_REPO}",
                "ref: ${{ inputs.nevr_runtime_sha }}",
                "sparse-checkout: sdk/include",
                "fetch-depth: 1",
                "persist-credentials: false",
            ):
                self.assertRegex(step, rf"(?m)^        {re.escape(line)}$", job)
            self.assertNotIn("persist-credentials: true", step, job)
            # the verify step follows the checkout and proves the checked-out commit and the SDK header
            verify = [s for s in steps if "git -C nevr-runtime rev-parse HEAD" in "\n".join(code_lines(s))]
            self.assertEqual(len(verify), 1, job)
            self.assertIn("NEVR_RUNTIME_SHA: ${{ inputs.nevr_runtime_sha }}", verify[0], job)
            self.assertIn('"$head" != "$NEVR_RUNTIME_SHA"', verify[0], job)
            self.assertIn("nevr-runtime/sdk/include/nevr/plugin_logger.h", verify[0], job)
            self.assertLess(steps.index(step), steps.index(verify[0]), job)

    def test_every_checkout_of_the_nevr_path_names_the_sdk_repository(self):
        for job in ("build", "overlay"):
            for step in job_steps(job_block(self.text, job)):
                if re.search(r"(?m)^        path: nevr-runtime$", step):
                    self.assertIn(f"repository: {SDK_REPO}", step, job)

    def test_the_three_cl_lines_build_as_c17_against_the_nevr_sdk_headers(self):
        build = job_block(self.text, "build")
        cl = [line.split() for line in code_lines(build) if line.strip().startswith("cl.exe ")]
        self.assertEqual(len(cl), 3, cl)
        for tokens in cl:
            for flag in CL_SDK_FLAGS:
                self.assertIn(flag, tokens, " ".join(tokens))
            self.assertIn("/I..\\detours\\include", tokens, " ".join(tokens))

    def test_the_overlay_is_built_from_the_input_commit_and_checked_by_rev_parse(self):
        overlay = job_block(self.text, "overlay")
        self.assertIsNotNone(overlay)
        self.assertRegex(overlay, r"(?m)^\s*repository: \$\{\{ inputs\.overlay_repo \}\}$")
        self.assertRegex(overlay, r"(?m)^\s*ref: \$\{\{ inputs\.overlay_sha \}\}$")
        verify = [s for s in job_steps(overlay) if "git -C overlay rev-parse HEAD" in "\n".join(code_lines(s))]
        self.assertEqual(len(verify), 1)
        self.assertIn("OVERLAY_SHA: ${{ inputs.overlay_sha }}", verify[0])
        # The commit is literal only as the input's default, the published step's condition and the record's constant.
        on = self.text.split("\non:\n", 1)[1].split("\npermissions:", 1)[0]
        self.assertIn(OVERLAY_COMMIT, input_block(on, "overlay_sha"))
        literal_lines = [line for line in code_lines(self.text) if OVERLAY_COMMIT in line]
        self.assertEqual(len(literal_lines), 3, literal_lines)

    def test_the_retired_v1_0_0_release_hash_is_gone(self):
        self.assertNotIn(RETIRED_OVERLAY_SHA256, self.text)
        self.assertNotIn("overlay_sha256", self.text)

    def test_the_overlay_job_runs_on_ubuntu_and_has_no_windows_runner(self):
        overlay = job_block(self.text, "overlay")
        self.assertIsNotNone(overlay)
        self.assertEqual(runs_on(overlay), ["ubuntu-latest"])
        self.assertNotIn("windows", "\n".join(code_lines(overlay)).lower())

    def test_the_only_windows_runner_in_the_file_is_the_build_job(self):
        build = job_block(self.text, "build")
        self.assertEqual(runs_on(build), ["windows-latest"])
        self.assertEqual([r for r in runs_on(self.text) if r.startswith("windows-")], ["windows-latest"])
        self.assertEqual(runs_on(job_block(self.text, "overlay")), ["ubuntu-latest"])
        self.assertEqual(runs_on(job_block(self.text, "attest")), ["ubuntu-latest"])

    def test_attest_needs_both_the_build_and_the_overlay_job(self):
        attest = job_block(self.text, "attest")
        self.assertIsNotNone(attest)
        needs = re.findall(r"(?m)^    needs: \[(.*)\]$", attest)
        self.assertEqual([set(n.split(", ")) for n in needs], [{"build", "overlay"}])
        self.assertIn("artifact/*.dll", attest)
        self.assertIn("overlay-artifact/*.dll", attest)

    def test_top_level_permissions_are_contents_read(self):
        m = re.search(r"^permissions:\n((?:  .*\n)+)", self.text, re.M)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1).split(), ["contents:", "read"])

    def test_the_overlay_job_passes_the_sdk_include_to_cmake_after_the_sdk_checkout(self):
        steps = job_steps(job_block(self.text, "overlay"))
        repo = [i for i, s in enumerate(steps) if "uses: actions/checkout@" in s and re.search(r"(?m)^\s+path: nevr-runtime$", s)]
        self.assertEqual(len(repo), 1)
        self.assertIn(f"repository: {SDK_REPO}", steps[repo[0]])
        cmake = [i for i, s in enumerate(steps) if any("cmake -S overlay" in line for line in code_lines(s))]
        self.assertEqual(len(cmake), 1)
        self.assertLess(repo[0], cmake[0])
        cmake_lines = [line for line in code_lines(steps[cmake[0]]) if "cmake -S overlay" in line]
        self.assertEqual(len(cmake_lines), 1)
        self.assertIn('-DNEVR_SDK_INCLUDE="$PWD/nevr-runtime/sdk/include"', cmake_lines[0])

    def test_the_published_asset_is_pinned_and_its_step_runs_only_for_the_published_commit(self):
        self.assertEqual(self.text.count(PUBLISHED_SHA256), 1)
        steps = [s for s in job_steps(job_block(self.text, "overlay")) if PUBLISHED_SHA256 in s]
        self.assertEqual(len(steps), 1)
        code = "\n".join(code_lines(steps[0]))
        conditions = [line for line in code.splitlines() if re.match(r"^\s*if:", line)]
        self.assertEqual(len(conditions), 1)
        self.assertIn("inputs.overlay_sha ==", conditions[0])
        self.assertIn(OVERLAY_COMMIT, conditions[0])
        self.assertIn(
            f"gh release download v1.1.0 --repo {OVERLAY_REPO} --pattern NvrContentOverlay.dll --dir published", code)

    def test_github_token_is_used_once_in_the_download_steps_env(self):
        self.assertEqual(self.text.count("github.token"), 1)
        steps = [s for s in job_steps(job_block(self.text, "overlay")) if "github.token" in s]
        self.assertEqual(len(steps), 1)
        self.assertTrue(steps[0].startswith("    - name: Download the published"), steps[0][:80])
        self.assertRegex("\n".join(code_lines(steps[0])), r"(?m)^      env:\n        GH_TOKEN: \$\{\{ github\.token \}\}\n")

    def test_the_header_comment_names_seh_and_detours(self):
        header = self.text.split("\non:", 1)[0]
        self.assertIn("#", header)
        self.assertIn("SEH", header)
        self.assertIn("Detours", header)


if __name__ == "__main__":
    unittest.main()
