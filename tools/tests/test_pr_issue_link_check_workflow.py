"""The PR issue-link workflow (pr-issue-link-check.yml): its trust boundary, pinned by contract.

It runs on `pull_request` with a read-only token and no secrets, and passes the untrusted PR body through
the environment, never through an expression inside a script body. Nothing can make it pass or skip."""
import re
import shutil
import subprocess
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "pr-issue-link-check.yml"


def load():
    data = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    return data, data.get("on", data.get(True))  # PyYAML reads the bare key `on` as True


def all_steps(data):
    for name, job in data["jobs"].items():
        for step in job["steps"]:
            yield name, step


class PrIssueLinkCheckWorkflowTest(unittest.TestCase):
    def test_it_fires_on_pull_request_with_the_four_body_and_push_types(self):
        _, triggers = load()
        self.assertEqual(list(triggers), ["pull_request"])
        self.assertEqual(sorted(triggers["pull_request"]["types"]),
                         ["edited", "opened", "reopened", "synchronize"])

    def test_it_is_never_pull_request_target(self):
        code = "\n".join(line for line in WORKFLOW.read_text(encoding="utf-8").splitlines()
                         if not line.lstrip().startswith("#"))
        self.assertNotIn("pull_request_target", code)
        self.assertNotIn("workflow_run", code)

    def test_permissions_are_contents_read_and_nothing_else_at_every_level(self):
        data, _ = load()
        self.assertEqual(data["permissions"], {"contents": "read"})
        for name, job in data["jobs"].items():
            self.assertIn(job.get("permissions", {"contents": "read"}), ({"contents": "read"},), name)

    def test_no_secret_is_named_not_even_the_github_token(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(re.findall(r"secrets\.\w+|secrets\[|secrets:\s*inherit", text), [])
        self.assertNotIn("GITHUB_TOKEN", text)

    def test_no_expression_appears_inside_a_script_body(self):
        data, _ = load()
        runs = [(job, step) for job, step in all_steps(data) if "run" in step]
        self.assertTrue(runs, "the sensor sees no run step: it is blind")
        for job, step in runs:
            self.assertNotIn("${{", step["run"], f"{job}: {step.get('name')}")

    def test_the_pr_body_comes_in_through_env(self):
        data, _ = load()
        step = next(s for _, s in all_steps(data) if "check_pr_issue_link.py" in s.get("run", ""))
        self.assertEqual(step["env"], {"PR_BODY": "${{ github.event.pull_request.body }}"})

    def test_every_action_is_pinned_to_a_full_commit_sha(self):
        data, _ = load()
        uses = [s["uses"] for _, s in all_steps(data) if "uses" in s]
        self.assertTrue(uses)
        for ref in uses:
            self.assertRegex(ref, r"^[\w./-]+@[0-9a-f]{40}$", ref)

    def test_checkout_keeps_no_credential_in_the_clone(self):
        data, _ = load()
        checkout = next(s for _, s in all_steps(data) if s.get("uses", "").startswith("actions/checkout@"))
        self.assertIs(checkout["with"]["persist-credentials"], False)
        self.assertEqual(checkout["with"]["ref"], "${{ github.event.pull_request.head.sha }}")

    def test_the_job_runs_the_checker_script_and_nothing_publishes(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("python3 tools/check_pr_issue_link.py", text)
        for forbidden in ("gh pr", "gh api", "git push", "upload-artifact", "actions/cache", "id-token"):
            self.assertNotIn(forbidden, text)

    def test_nothing_can_make_the_check_pass_or_skip_silently(self):
        """`continue-on-error` or an `if:` on the job or a step would turn a failing check green or
        skipped without any test noticing; so would swallowing the script's exit status."""
        data, _ = load()
        self.assertNotIn("continue-on-error", data)
        self.assertNotIn("if", data)
        self.assertNotIn("defaults", data, "a workflow-level shell default could change error handling")
        for name, job in data["jobs"].items():
            for key in ("continue-on-error", "if", "needs", "strategy", "container", "services"):
                self.assertNotIn(key, job, f"{name}: job-level {key}")
            for step in job["steps"]:
                label = f"{name}: {step.get('name') or step.get('uses')}"
                self.assertNotIn("continue-on-error", step, label)
                self.assertNotIn("if", step, label)
                self.assertNotIn("shell", step, label)
        run = next(s for _, s in all_steps(data) if "check_pr_issue_link.py" in s.get("run", ""))["run"]
        for swallow in ("|| true", "|| :", "|| exit 0", "set +e", "set +o errexit"):
            self.assertNotIn(swallow, run)
        last = [l for l in run.splitlines() if l.strip()][-1]
        self.assertTrue(last.lstrip().startswith("python3 tools/check_pr_issue_link.py"),
                        "the checker is the last command, so its exit status is the step's")
        self.assertNotRegex(last, r"[|;&]|>\s*/dev/null")

    def test_the_pr_template_asks_for_a_reference_that_fails_until_it_is_filled_in(self):
        template = (REPO / ".github" / "pull_request_template.md").read_text(encoding="utf-8")
        result = subprocess.run(["python3", "-I", str(REPO / "tools" / "check_pr_issue_link.py")],
                                capture_output=True, text=True, env={"PR_BODY": template})
        self.assertEqual(result.returncode, 1, "the unedited template must fail the check")
        self.assertIn("Closes #", template)
        self.assertNotIn("why there is none", template)

    def test_actionlint_accepts_the_workflow_when_installed(self):
        actionlint = shutil.which("actionlint")
        if actionlint is None:
            self.skipTest("actionlint is not installed")
        result = subprocess.run([actionlint, str(WORKFLOW)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
