"""`tools/quest_release_timer.py` (#1210): the Quest half of a release, run by a user timer on the publisher's machine.
For the newest published tag that is not done it builds the release signer, makes a clean worktree at the tag, builds and
publishes the Quest APK, dispatches the beta manifest publish live, requires every job that publishes to have run, and
requires `verify-manifest --live` to list the tag. The commands are fakes that record what they were called with; the
log is real, and no secret may reach it."""

import datetime
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Callable

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import quest_release_timer as timer  # noqa: E402

TAG = "v5.17.0"
SECRETS = {
    "R2_ENDPOINT": "https://sentinel-endpoint.invalid",
    "R2_BUCKET": "sentinel-bucket",
    "R2_ACCESS_KEY_ID": "sentinel-access-key-id",
    "R2_SECRET_ACCESS_KEY": "sentinel-secret-access-key",
}
TOKEN = "sentinel-gh-token"
STEPS = ["checkout", "tags", "token", "list", "build_signer", "check_listed", "worktree", "build_slot", "configure", "build_apk",
         "publish_apk", "dispatch", "watch", "check_jobs", "verify_live", "notify", "cleanup"]
CARRIED = {"name": "echovr-data-fc2eedeacc50.zip", "kind": "game-data", "size": 937395400}
HEAD = "c0ffee" + "0" * 34
# name -> (object, peeled commit or None): the tags the checkout holds, and the same tags on origin
TAGS = {"v5.16.1": ("1" * 40, None), "v5.17.0": ("2" * 40, "3" * 40)}
PUBLISHING_JOBS = ["Commit the manifest record", "Publish to R2", "A run that published nothing is red"]


def completed(argv: list[str], rc: int = 0, out: str = "", err: str = "") -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess(argv, rc, out, err)


class Fake:
    """A scripted command runner. `fail` is a step name whose command exits 1; `overrides` replaces a step's output."""

    def __init__(self, root: pathlib.Path, fail: str = "", overrides: dict | None = None, ninja: list | None = None) -> None:
        self.root, self.fail, self.overrides = root, fail, overrides or {}
        self.calls: list[dict] = []
        self.ninja = list(ninja or [1])
        self.beta = "5.16.1"
        self.dirty = ""
        self.head = HEAD
        self.main = HEAD
        self.carried = dict(CARRIED)  # the game-data artifact the live manifest carries before the publish
        self.published_game_data: dict | None = dict(CARRIED)  # what the new release lists after it
        self.jobs = [{"name": name, "conclusion": "success"} for name in PUBLISHING_JOBS]
        self.local_tags: dict = dict(TAGS)  # what the checkout holds
        self.remote_tags: dict = dict(TAGS)  # what origin holds

    def step_of(self, argv: list[str]) -> str:
        joined = " ".join(argv)
        repo = f"{self.root}/repo"
        table = [
            (f"{repo} fetch --quiet origin main", "checkout.fetch"), (f"{repo} status --porcelain", "checkout.status"),
            (f"{repo} for-each-ref refs/tags", "tags.local"), (f"{repo} ls-remote --tags origin", "tags.remote"),
            (f"{repo} rev-parse HEAD", "checkout.head"), (f"{repo} rev-parse origin/main", "checkout.main"),
            ("curl", "manifest"), ("bonk", "bonk"), ("courier send", "courier"),
            ("auth token", "token"), ("release list", "list"), (" fetch origin main", "build_signer.fetch"),
            ("rev-parse origin/main", "build_signer.rev"), (" archive ", "build_signer.archive"), ("tar -xf", "build_signer.tar"),
            ("go build", "build_signer.go"), ("verify-manifest", "verify"), (" fetch origin tag ", "worktree.fetch"),
            ("^{commit}", "worktree.rev"), ("worktree add", "worktree.add"), ("worktree-setup", "worktree.setup"),
            ("pgrep", "build_slot"), ("just configure", "configure"), ("release-quest-apk", "build_apk"),
            ("publish_quest_apk.py", "publish_apk"),
            ("workflow run", "dispatch"), ("run watch", "watch"), ("run view", "check_jobs"),
            ("remove-worktree", "cleanup.worktree"),
        ]
        for needle, step in table:
            if needle in joined:
                return step
        raise AssertionError(f"unexpected command: {argv}")

    def __call__(self, argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
        step = self.step_of(argv)
        self.calls.append({"argv": argv, "cwd": cwd, "env": env, "step": step})
        top = step.split(".")[0]
        if self.fail and self.fail in (step, top):
            return completed(argv, 2, "", f"{step} broke")
        if step in self.overrides:
            return self.overrides[step](argv)
        if step == "checkout.status":
            return completed(argv, 0, self.dirty)
        if step == "checkout.head":
            return completed(argv, 0, self.head + "\n")
        if step == "checkout.main":
            return completed(argv, 0, self.main + "\n")
        if step == "tags.local":
            return completed(argv, 0, "".join(f"{n}\t{o}\t{c or ''}\n" for n, (o, c) in self.local_tags.items()))
        if step == "tags.remote":
            lines = []
            for n, (o, c) in self.remote_tags.items():
                lines.append(f"{o}\trefs/tags/{n}\n")
                if c:
                    lines.append(f"{c}\trefs/tags/{n}^{{}}\n")
            return completed(argv, 0, "".join(lines))
        if step == "manifest":
            return completed(argv, 0, self.manifest_bytes())
        if step == "token":
            return completed(argv, 0, TOKEN + "\n")
        if step == "list":
            return completed(argv, 0, json.dumps([{"tagName": "v5.18.0", "isDraft": True}, {"tagName": TAG, "isDraft": False},
                                                  {"tagName": "v5.16.1", "isDraft": False}]))
        if step == "build_signer.tar":
            pathlib.Path(argv[argv.index("-C") + 1]).mkdir(parents=True, exist_ok=True)
        if step == "build_signer.rev":
            return completed(argv, 0, "d89eb9d" + "0" * 33 + "\n")
        if step == "worktree.rev":
            return completed(argv, 0, "3b775972" + "0" * 32 + "\n")
        if step == "worktree.add":
            pathlib.Path(argv[-2]).mkdir(parents=True, exist_ok=True)
        if step == "build_slot":
            lines = self.ninja.pop(0) if len(self.ninja) > 1 else self.ninja[0]
            return completed(argv, 0 if lines else 1, "1234\n" * lines)
        if step == "publish_apk":
            pathlib.Path(argv[argv.index("--extra-out") + 1]).write_text(
                json.dumps([{"name": f"nevr-runtime-{TAG}-quest.apk", "sha256": "ab" * 32, "size": 7}]))
        if step == "dispatch":
            return completed(argv, 0, "https://github.com/thesprockee/release-signer/actions/runs/40746571311\n")
        if step == "check_jobs":
            return completed(argv, 0, json.dumps({"jobs": self.jobs}))
        if step == "verify":
            digest = hashlib.sha256(self.manifest_bytes().encode()).hexdigest()
            return completed(argv, 0, json.dumps({"channels": {"beta": self.live_beta()}, "manifest_version": 5,
                                                  "sha256": digest}) + "\n")
        return completed(argv)

    def live_beta(self) -> str:
        if any(c["step"] == "watch" for c in self.calls):
            dispatched = [c["argv"] for c in self.calls if c["step"] == "dispatch"]
            return next(a for a in dispatched[0] if a.startswith("tag=")).split("=", 1)[1][1:]
        return self.beta

    def manifest_bytes(self) -> str:
        """The live manifest: the release beta points at lists its artifacts, game data included."""
        beta = self.live_beta()
        published = beta != self.beta
        artifact = self.published_game_data if published else self.carried
        artifacts = [{"name": f"nevr-runtime-v{beta}-quest.apk", "kind": "quest-apk", "size": 7}]
        if artifact is not None:
            artifacts.insert(0, artifact)
        return json.dumps({"channels": {"beta": beta}, "releases": {beta: {"version": beta, "artifacts": artifacts}}})

    def steps(self) -> list[str]:
        return [c["step"] for c in self.calls]


def bonk_calls(fake: Fake) -> list[list[str]]:
    """Every command whose program is `bonk`, whatever step it was mapped to: the owner is never bonked (#1410)."""
    return [c["argv"] for c in fake.calls if c["argv"][0] == "bonk"]


def couriers(fake: Fake, to: str) -> list[list[str]]:
    """The `courier send --to <to>` commands the run made, in order."""
    return [c["argv"] for c in fake.calls if c["step"] == "courier" and c["argv"][:4] == ["courier", "send", "--to", to]]


class TimerCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.env = {**SECRETS, "RELEASE_PUBLIC_BASE": "https://release.invalid", "GH_BIN": "gh-fake",
                    "XDG_STATE_HOME": str(self.root / "state"), "QUEST_RELEASE_WORK_ROOT": str(self.root / "work"),
                    "QUEST_RELEASE_REPO": str(self.root / "repo"), "QUEST_RELEASE_SIGNER_REPO": str(self.root / "signer"),
                    "PATH": "/usr/bin"}
        self.log = self.root / "state" / "nevr-runtime" / "quest-release-timer.jsonl"
        self.sleeps: list[float] = []

    def run_timer(self, fake: Fake, *args: str, env: dict | None = None) -> int:
        return timer.main(list(args), env or self.env, runner=fake, sleep=self.sleeps.append)

    def events(self) -> list[dict]:
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def names(self) -> list[str]:
        return [e["event"] for e in self.events()]

    def write_log(self, *records: dict) -> None:
        self.log.parent.mkdir(parents=True, exist_ok=True)
        self.log.write_text("".join(json.dumps(r) + "\n" for r in records))


class RunsInOrder(TimerCase):
    def test_a_new_tag_runs_every_step_in_order_and_is_done(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        steps = [e["step"] for e in self.events() if e["event"] == "step_ok"]
        self.assertEqual(steps, STEPS)
        self.assertEqual(self.names()[-1], "done")
        self.assertEqual(self.events()[-1]["tag"], TAG)
        # the newest published tag, not the draft v5.18.0 and not the older v5.16.1
        build = next(c for c in fake.calls if c["step"] == "build_apk")
        self.assertEqual(build["argv"][-1], TAG)

    def test_the_build_runs_in_the_slice_and_waits_for_a_free_slot(self) -> None:
        fake = Fake(self.root, ninja=[2, 2, 1])
        self.assertEqual(self.run_timer(fake), 0)
        build = next(c for c in fake.calls if c["step"] == "build_apk")
        self.assertEqual(build["argv"][:6], ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G"])
        self.assertEqual(build["env"]["VCPKG_MAX_CONCURRENCY"], "6")
        self.assertEqual(build["env"]["CMAKE_BUILD_PARALLEL_LEVEL"], "6")
        self.assertEqual(len(self.sleeps), 2, "two busy readings, two waits")

    def test_a_slot_that_never_frees_fails_the_step(self) -> None:
        fake = Fake(self.root, ninja=[2])
        self.assertEqual(self.run_timer(fake), 1)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual(failed[0]["step"], "build_slot")
        self.assertNotIn("build_apk", fake.steps())

    def test_the_manifest_publish_is_live_beta_with_the_apk_claim(self) -> None:
        fake = Fake(self.root)
        self.run_timer(fake)
        argv = next(c["argv"] for c in fake.calls if c["step"] == "dispatch")
        self.assertIn("action=publish", argv)
        self.assertIn(f"tag={TAG}", argv)
        self.assertIn("channel=beta", argv)
        self.assertIn("dry_run=false", argv)
        extra = next(a for a in argv if a.startswith("extra_artifacts="))
        self.assertEqual(json.loads(extra.split("=", 1)[1])[0]["name"], f"nevr-runtime-{TAG}-quest.apk")

    def test_the_watch_follows_the_dispatched_run_and_fails_with_it(self) -> None:
        fake = Fake(self.root)
        self.run_timer(fake)
        argv = next(c["argv"] for c in fake.calls if c["step"] == "watch")
        self.assertEqual(argv[argv.index("watch") + 1], "40746571311")
        self.assertIn("thesprockee/release-signer", argv)
        self.assertIn("--exit-status", argv)

    def test_the_newest_tag_is_the_highest_version_not_the_last_string(self) -> None:
        listing = json.dumps([{"tagName": t, "isDraft": False} for t in ("v5.9.0", "v5.17.0", "v5.10.2")])
        fake = Fake(self.root, overrides={"list": lambda argv: completed(argv, 0, listing)})
        self.run_timer(fake)
        build = next(c for c in fake.calls if c["step"] == "build_apk")
        self.assertEqual(build["argv"][-1], "v5.17.0")

    def test_the_publish_gets_the_credentials_and_the_signer_it_built(self) -> None:
        fake = Fake(self.root)
        self.run_timer(fake)
        publish = next(c for c in fake.calls if c["step"] == "publish_apk")
        for name, value in SECRETS.items():
            self.assertEqual(publish["env"][name], value)
        self.assertEqual(publish["env"]["GH_TOKEN"], TOKEN)
        self.assertTrue(publish["env"]["RELEASE_SIGNER_BIN"].endswith("/release-signer"))
        self.assertEqual(publish["env"]["RELEASE_PUBLIC_BASE"], "https://release.invalid")
        self.assertEqual(publish["cwd"], next(c["cwd"] for c in fake.calls if c["step"] == "build_apk"))

    def test_what_it_created_is_removed_by_exact_path(self) -> None:
        fake = Fake(self.root)
        self.run_timer(fake)
        removed = next(c for c in fake.calls if c["step"] == "cleanup.worktree")
        added = next(c for c in fake.calls if c["step"] == "worktree.add")
        self.assertEqual(removed["argv"][-1], added["argv"][-2])
        self.assertEqual(removed["cwd"], str(self.root / "repo"))
        self.assertEqual(sorted(p.name for p in (self.root / "work").glob("*release-signer-*")), [], "the signer snapshot is gone")


class ConfigureBeforeTheBuild(TimerCase):
    """A new tag's worktree has no vcpkg install, so `just configure` (the vcpkg install) runs before the APK build."""

    def configure_call(self, fake: Fake) -> dict:
        return next(c for c in fake.calls if c["step"] == "configure")

    def test_just_configure_runs_before_the_build_in_the_same_worktree_with_no_secret(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        steps = fake.steps()
        self.assertLess(steps.index("configure"), steps.index("build_apk"))
        configure, build = self.configure_call(fake), next(c for c in fake.calls if c["step"] == "build_apk")
        self.assertEqual(configure["argv"][-2:], ["just", "configure"])
        self.assertEqual(configure["cwd"], build["cwd"], "configure runs in the worktree the build runs in")
        self.assertIsNotNone(configure["cwd"])
        self.assertEqual(sorted(set(SECRETS) & set(configure["env"])), [], "configure was given R2 credentials")
        self.assertNotIn("GH_TOKEN", configure["env"], "configure was given the GitHub token")

    def test_configure_runs_in_the_build_slice_with_the_build_env(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        configure = self.configure_call(fake)
        self.assertEqual(configure["argv"][:6], ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p",
                                                 "MemoryMax=8G"])
        self.assertEqual(configure["env"]["VCPKG_MAX_CONCURRENCY"], "6")
        self.assertEqual(configure["env"]["CMAKE_BUILD_PARALLEL_LEVEL"], "6")
        self.assertEqual(configure["env"]["GH_BIN"], "gh-fake")

    def test_a_failing_configure_fails_at_configure_and_counts_as_a_failed_try(self) -> None:
        for _ in range(3):
            self.assertEqual(self.run_timer(Fake(self.root, fail="configure")), 1)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "failed"], ["configure"] * 3)
        self.assertEqual([e for e in self.events() if e["event"] == "refused"], [], "a failed configure is not a refusal")
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("skipped_after_failures", self.names())
        self.assertEqual(bonk_calls(fake), [])
        [spritz] = couriers(fake, "spritz")
        self.assertIn("configure", spritz[-1])
        self.assertNotIn("build_apk", fake.steps())

    def test_a_failing_configure_stops_before_the_build_and_names_itself(self) -> None:
        fake = Fake(self.root, fail="configure")
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "failed"], ["configure"])
        self.assertNotIn("build_apk", fake.steps())
        self.assertNotIn("publish_apk", fake.steps())


class NoSecretInTheLog(TimerCase):
    def test_no_secret_reaches_the_log_even_when_a_command_prints_one(self) -> None:
        leak = {"publish_apk": lambda argv: completed(argv, 1, "", f"boom {SECRETS['R2_SECRET_ACCESS_KEY']} {TOKEN}")}
        fake = Fake(self.root, overrides=leak)
        self.assertEqual(self.run_timer(fake), 1)
        text = self.log.read_text()
        for secret in [*SECRETS.values(), TOKEN]:
            self.assertNotIn(secret, text)
        self.assertIn("<redacted>", text)


class FailsLoud(TimerCase):
    def test_each_failing_step_exits_1_names_itself_and_stops_the_rest(self) -> None:
        later = {step: [s for s in STEPS[STEPS.index(step) + 1:] if s != "cleanup"]
                 for step in ("build_signer", "worktree", "build_slot", "configure", "build_apk", "publish_apk", "dispatch",
                              "watch")}
        for step, after in later.items():
            with self.subTest(step=step):
                self.log.unlink(missing_ok=True)
                fake = Fake(self.root, fail=step)
                self.assertEqual(self.run_timer(fake), 1)
                failed = [e for e in self.events() if e["event"] == "failed"]
                self.assertEqual([e["step"] for e in failed], [step])
                self.assertNotIn("done", self.names())
                ran = [e["step"] for e in self.events() if e["event"] == "step_ok"]
                self.assertTrue(set(ran).isdisjoint(after), (step, ran))

    def test_a_cleanup_that_fails_after_a_good_release_fails_the_run(self) -> None:
        fake = Fake(self.root, fail="cleanup")
        self.assertEqual(self.run_timer(fake), 1)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual([e["step"] for e in failed], ["cleanup"])
        self.assertIn("cleanup_failed", self.names())
        self.assertNotIn("done", self.names())

    def test_a_cleanup_that_fails_after_a_failed_step_does_not_hide_that_step(self) -> None:
        fake = Fake(self.root, overrides={"cleanup.worktree": lambda argv: completed(argv, 1, "", "busy")})
        fake.fail = "publish_apk"
        self.assertEqual(self.run_timer(fake), 1)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual([e["step"] for e in failed], ["publish_apk"])
        self.assertIn("cleanup_failed", self.names())

    def test_a_failure_still_removes_the_worktree(self) -> None:
        for step in ("build_apk", "publish_apk", "watch"):
            with self.subTest(step=step):
                fake = Fake(self.root, fail=step)
                self.run_timer(fake)
                self.assertIn("cleanup.worktree", fake.steps())

    def test_a_green_watch_with_a_skipped_publishing_job_is_a_failure(self) -> None:
        # Run 40744204687 ended "success" with record and upload skipped. The watch exit is not trusted.
        fake = Fake(self.root)
        fake.jobs = [{"name": "Commit the manifest record", "conclusion": "skipped"},
                     {"name": "Publish to R2", "conclusion": "skipped"},
                     {"name": "A run that published nothing is red", "conclusion": "success"}]
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "check_jobs")
        self.assertIn("Commit the manifest record", failed["error"])
        self.assertNotIn("verify_live", fake.steps())

    def test_a_missing_publishing_job_is_a_failure(self) -> None:
        fake = Fake(self.root)
        fake.jobs = fake.jobs[:2]
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "check_jobs")

    def test_a_live_manifest_that_does_not_list_the_tag_is_a_failure(self) -> None:
        fake = Fake(self.root)

        def stale(argv: list[str]) -> subprocess.CompletedProcess:
            digest = hashlib.sha256(fake.manifest_bytes().encode()).hexdigest()
            return completed(argv, 0, json.dumps({"channels": {"beta": "5.16.1"}, "sha256": digest}) + "\n")

        fake.overrides["verify"] = stale
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "verify_live")
        self.assertIn(TAG[1:], failed["error"])

    def test_a_dispatch_that_prints_no_run_id_fails(self) -> None:
        fake = Fake(self.root, overrides={"dispatch": lambda argv: completed(argv, 0, "queued\n")})
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "dispatch")
        self.assertNotIn("watch", fake.steps())

    def test_a_missing_environment_value_is_a_refusal_that_names_it(self) -> None:
        for name in (*SECRETS, "RELEASE_PUBLIC_BASE"):
            with self.subTest(name=name):
                self.log.unlink(missing_ok=True)
                fake = Fake(self.root)
                env = {k: v for k, v in self.env.items() if k != name}
                self.assertEqual(self.run_timer(fake, env=env), 1)
                refused = [e for e in self.events() if e["event"] == "refused"]
                self.assertEqual(refused[0]["missing"], [name])
                self.assertEqual(fake.calls, [])

    def test_a_log_that_cannot_be_written_is_a_failure(self) -> None:
        blocker = self.root / "state"
        blocker.write_text("a file where the directory should be")
        self.assertEqual(self.run_timer(Fake(self.root)), 1)


class SkipsWhatIsDone(TimerCase):
    def test_a_done_tag_is_not_rebuilt(self) -> None:
        self.write_log({"event": "done", "tag": TAG})
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(fake.steps()[6:], ["token", "list"])
        self.assertIn("nothing_to_do", self.names())

    def test_a_tag_the_live_manifest_already_lists_is_marked_done_without_a_build(self) -> None:
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertNotIn("worktree.add", fake.steps())
        self.assertNotIn("build_apk", fake.steps())
        self.assertIn("already_listed", self.names())
        self.assertEqual(self.names()[-1], "done")

    def test_three_failures_stop_the_retries_and_say_so(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk"} for _ in range(3)))
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual([s for s in fake.steps() if s not in ("bonk", "courier")][6:], ["token", "list"])
        self.assertIn("skipped_after_failures", self.names())

    def test_two_failures_are_retried(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk"} for _ in range(2)))
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertEqual(self.names()[-1], "done")

    def test_a_tag_given_on_the_command_line_is_the_one_built(self) -> None:
        fake = Fake(self.root)
        fake.beta = "5.15.0"
        self.assertEqual(self.run_timer(fake, "--tag", "v5.16.1"), 0)
        build = next(c for c in fake.calls if c["step"] == "build_apk")
        self.assertEqual(build["argv"][-1], "v5.16.1")
        self.assertNotIn("list", fake.steps())


class NotifiesOnFailure(TimerCase):
    def test_the_failure_notice_names_the_tag_and_the_step_and_holds_no_secret(self) -> None:
        self.write_log({"event": "failed", "tag": TAG, "step": "publish_apk", "error": "publish failed with exit 1"})
        sent: list[list[str]] = []

        def courier(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
            sent.append(argv)
            return completed(argv)

        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=courier, sleep=self.sleeps.append), 0)
        self.assertEqual(sent[0][:6], ["courier", "send", "--to", "spritz", "--from", "quest-release-timer"])
        body = sent[0][-1]
        self.assertIn(TAG, body)
        self.assertIn("publish_apk", body)
        for secret in [*SECRETS.values(), TOKEN]:
            self.assertNotIn(secret, " ".join(sent[0]))

    def test_a_notice_that_cannot_be_sent_is_a_failure(self) -> None:
        self.write_log({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"})
        broken = lambda argv, cwd=None, env=None: completed(argv, 1, "", "no courier")  # noqa: E731
        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=broken, sleep=self.sleeps.append), 1)


class CredentialsPerStep(TimerCase):
    def test_only_the_publish_gets_the_r2_credentials_and_only_it_and_the_dispatch_get_the_token(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        for call in fake.calls:
            env = call["env"] or {}
            step = call["step"]
            leaked_r2 = sorted(set(SECRETS) & set(env))
            with self.subTest(step=step):
                if step == "publish_apk":
                    self.assertEqual(leaked_r2, sorted(SECRETS))
                else:
                    self.assertEqual(leaked_r2, [], f"{step} was given R2 credentials")
                if step in ("publish_apk", "dispatch"):
                    self.assertEqual(env.get("GH_TOKEN"), TOKEN)
                else:
                    self.assertNotIn("GH_TOKEN", env, f"{step} was given the GitHub token")

    def test_the_build_gets_neither_even_when_the_unit_environment_holds_both(self) -> None:
        fake = Fake(self.root)
        env = {**self.env, "GH_TOKEN": "sentinel-ambient-token"}
        self.assertEqual(self.run_timer(fake, env=env), 0)
        for step in ("build_apk", "worktree.setup", "build_signer.go", "cleanup.worktree"):
            call = next(c for c in fake.calls if c["step"] == step)
            self.assertTrue(set(SECRETS).isdisjoint(call["env"]), step)
            self.assertNotIn("GH_TOKEN", call["env"], step)


class CheckoutGuard(TimerCase):
    def test_a_dirty_checkout_is_refused_and_nothing_else_runs(self) -> None:
        fake = Fake(self.root)
        fake.dirty = " M tools/quest_release_timer.py\n"
        self.assertEqual(self.run_timer(fake), 1)
        refused = [e for e in self.events() if e["event"] == "refused"]
        self.assertEqual([e["step"] for e in refused], ["checkout"])
        self.assertIn("not clean", refused[0]["error"])
        self.assertEqual([e for e in self.events() if e["event"] == "failed"], [])
        self.assertEqual({c["step"] for c in fake.calls}, {"checkout.fetch", "checkout.status"})

    def test_a_head_that_is_not_the_fetched_origin_main_is_refused(self) -> None:
        fake = Fake(self.root)
        fake.main = "deadbeef" + "0" * 32
        self.assertEqual(self.run_timer(fake), 1)
        refused = [e for e in self.events() if e["event"] == "refused"]
        self.assertEqual([e["step"] for e in refused], ["checkout"])
        self.assertIn("origin/main", refused[0]["error"])
        self.assertEqual([e for e in self.events() if e["event"] == "failed"], [])
        self.assertNotIn("token", fake.steps())

    def test_a_checkout_at_origin_main_that_is_clean_proceeds(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(fake.steps()[:4], ["checkout.fetch", "checkout.status", "checkout.head", "checkout.main"])
        checkout = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "checkout")
        self.assertEqual(checkout["head"], HEAD)


class GameDataSurvives(TimerCase):
    def test_a_release_that_lists_no_game_data_is_a_failure_never_done(self) -> None:
        fake = Fake(self.root)
        fake.published_game_data = None
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "verify_live")
        self.assertIn(CARRIED["name"], failed["error"])
        self.assertNotIn("done", self.names())
        self.assertNotIn("bonk", fake.steps())

    def test_a_carried_zip_with_another_size_is_a_failure(self) -> None:
        fake = Fake(self.root)
        fake.published_game_data = {**CARRIED, "size": CARRIED["size"] - 1}
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "verify_live")

    def test_a_carried_zip_with_another_name_is_a_failure(self) -> None:
        fake = Fake(self.root)
        fake.published_game_data = {**CARRIED, "name": "echovr-data-other.zip"}
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "verify_live")

    def test_a_manifest_that_is_not_the_one_the_signer_verified_is_a_failure(self) -> None:
        fake = Fake(self.root)
        genuine = fake.manifest_bytes  # before the publish the host serves what the signer verified

        def swapped(argv: list[str]) -> subprocess.CompletedProcess:
            published = any(c["step"] == "watch" for c in fake.calls)
            return completed(argv, 0, json.dumps({"channels": {"beta": "5.17.0"}}) if published else genuine())

        fake.overrides["manifest"] = swapped
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "verify_live")
        self.assertIn("sha256", failed["error"])

    def test_a_live_manifest_with_no_game_data_to_carry_stops_before_the_build(self) -> None:
        fake = Fake(self.root)
        fake.carried = None
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "check_listed")
        self.assertNotIn("worktree.add", fake.steps())

    def test_the_fetch_asks_the_public_host_for_the_manifest(self) -> None:
        fake = Fake(self.root)
        self.run_timer(fake)
        argv = next(c["argv"] for c in fake.calls if c["step"] == "manifest")
        self.assertIn("https://release.invalid/nevr-runtime/manifest.json", argv)


class NotifiesOnPublish(TimerCase):
    def test_a_live_publish_couriers_teth_and_spritz_before_it_is_done_and_never_bonks(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(bonk_calls(fake), [], "the owner is never bonked for a publish (#1410)")
        steps = fake.steps()
        self.assertLess(max(i for i, s in enumerate(steps) if s == "verify"), steps.index("courier"))
        [teth] = couriers(fake, "teth")
        [spritz] = couriers(fake, "spritz")
        self.assertEqual(teth[:6], ["courier", "send", "--to", "teth", "--from", "quest-release-timer"])
        self.assertEqual(spritz[:6], ["courier", "send", "--to", "spritz", "--from", "quest-release-timer"])
        self.assertIn(TAG, teth[-1])
        self.assertIn(TAG, spritz[-1])
        self.assertEqual(spritz[-1], teth[-1], "Spritz gets the same text Teth does")
        self.assertTrue(spritz[-1].endswith(f". Log: {self.log}"))
        for secret in [*SECRETS.values(), TOKEN]:
            self.assertNotIn(secret, " ".join(teth + spritz))
        self.assertEqual(self.names()[-1], "done")

    def test_a_notice_that_fails_is_a_failure_and_the_tag_is_not_done(self) -> None:
        fake = Fake(self.root, fail="courier")
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "notify")
        self.assertNotIn("done", self.names())
        self.assertEqual(bonk_calls(fake), [])

    def test_a_live_tag_whose_notice_was_never_sent_is_noticed_on_the_next_run(self) -> None:
        self.write_log({"event": "step_ok", "tag": TAG, "step": "verify_live"})
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(bonk_calls(fake), [])
        self.assertEqual(len(couriers(fake, "teth")), 1)
        self.assertEqual(len(couriers(fake, "spritz")), 1)
        self.assertNotIn("worktree.add", fake.steps())
        self.assertEqual(self.names()[-1], "done")

    def test_a_tag_that_was_noticed_is_not_noticed_again(self) -> None:
        self.write_log({"event": "step_ok", "tag": TAG, "step": "verify_live"},
                       {"event": "step_ok", "tag": TAG, "step": "notify"})
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertNotIn("bonk", fake.steps())

    def test_a_tag_the_timer_never_published_is_not_noticed(self) -> None:
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertNotIn("bonk", fake.steps())


class FinalFailureIsNotSilent(TimerCase):
    def test_the_third_failure_skip_couriers_spritz_once_and_never_bonks_before_it_exits_0(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(3)))
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("skipped_after_failures", self.names())
        self.assertEqual(bonk_calls(fake), [], "the third failure is couriered to Spritz, never bonked (#1410)")
        [courier] = couriers(fake, "spritz")
        self.assertEqual(courier[:6], ["courier", "send", "--to", "spritz", "--from", "quest-release-timer"])
        self.assertIn(TAG, courier[-1])
        self.assertIn("build_apk", courier[-1])
        self.assertEqual(couriers(fake, "teth"), [])
        self.assertLess(self.names().index("skip_notified"), self.names().index("skipped_after_failures"))

    def test_the_skip_is_announced_once_not_every_run(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(3)),
                       {"event": "skip_notified", "tag": TAG})
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertNotIn("bonk", fake.steps())
        self.assertNotIn("courier", fake.steps())

    def test_a_skip_whose_notice_cannot_be_sent_exits_1(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(3)))
        fake = Fake(self.root, fail="courier")
        self.assertEqual(self.run_timer(fake), 1)
        self.assertNotIn("skip_notified", self.names())


class RefusalIsNotAFailure(TimerCase):
    """A checkout precondition (dirty, or not at origin/main) is a refusal: logged, exit 1, never one of the three failures."""

    def run_refused(self, make_fake: Callable[[], Fake], want: str) -> None:
        for _ in range(3):
            fake = make_fake()
            self.assertEqual(self.run_timer(fake), 1)
            self.assertNotIn("bonk", fake.steps())
            self.assertNotIn("courier", fake.steps())
        refused = [e for e in self.events() if e["event"] == "refused"]
        self.assertEqual(len(refused), 3)
        self.assertEqual({e["step"] for e in refused}, {"checkout"})
        self.assertIn(want, refused[-1]["error"])
        self.assertEqual([e for e in self.events() if e["event"] == "failed"], [])

    def assert_fourth_run_proceeds(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("token", fake.steps())
        self.assertNotIn("skipped_after_failures", self.names())
        self.assertNotIn("skip_notified", self.names())
        self.assertEqual(self.names()[-1], "done")
        for call in fake.calls:
            if call["step"] in ("bonk", "courier"):
                self.assertNotIn("gave up", " ".join(call["argv"]), "the skip notice was sent")

    def test_three_dirty_checkouts_leave_refusals_and_the_clean_fourth_run_proceeds(self) -> None:
        def dirty() -> Fake:
            fake = Fake(self.root)
            fake.dirty = " M tools/quest_release_timer.py\n"
            return fake

        self.run_refused(dirty, "not clean")
        self.assert_fourth_run_proceeds()

    def test_three_stale_checkouts_leave_refusals_and_the_clean_fourth_run_proceeds(self) -> None:
        def stale() -> Fake:
            fake = Fake(self.root)
            fake.main = "deadbeef" + "0" * 32
            return fake

        self.run_refused(stale, "origin/main")
        self.assert_fourth_run_proceeds()

    def test_a_failing_build_still_counts_and_the_third_sends_the_skip(self) -> None:
        for _ in range(3):
            self.assertEqual(self.run_timer(Fake(self.root, fail="build_apk")), 1)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "failed"], ["build_apk"] * 3)
        self.assertEqual([e for e in self.events() if e["event"] == "refused"], [])
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("skipped_after_failures", self.names())
        self.assertEqual(bonk_calls(fake), [])
        [spritz] = couriers(fake, "spritz")
        self.assertIn("gave up", spritz[-1])
        self.assertIn("build_apk", spritz[-1])

    def test_refusals_between_real_failures_do_not_hide_the_third_failure(self) -> None:
        self.write_log({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"},
                       {"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"},
                       {"event": "refused", "tag": "", "step": "checkout", "error": "not clean"})
        self.assertEqual(self.run_timer(Fake(self.root, fail="build_apk")), 1)
        self.assertEqual(len([e for e in self.events() if e["event"] == "failed"]), 3)
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertIn("skipped_after_failures", self.names())


class FailureKeepsBothStreams(TimerCase):
    """A failing command's record holds the tail of stderr and of stdout, each bounded and redacted: a tool such as the
    release preflight prints its PROBLEM lines on stdout, and a record without them names no cause."""

    def make(self, rc: int = 3, out: str = "", err: str = "") -> tuple[timer.Timer, Fake]:
        fake = Fake(self.root, overrides={"tags.local": lambda argv: completed(argv, rc, out, err)})
        log = timer.Log(self.log, "")
        self.addCleanup(log.close)
        return timer.Timer(self.env, fake, self.sleeps.append, log), fake

    def message(self, rc: int = 3, out: str = "", err: str = "") -> str:
        subject, _ = self.make(rc, out, err)
        with self.assertRaises(timer.StepFailure) as raised:
            subject.cmd("tags", ["git", "-C", f"{self.root}/repo", "for-each-ref", "refs/tags"])
        return str(raised.exception)

    def test_both_tails_are_labelled_in_one_message(self) -> None:
        self.assertEqual(self.message(3, "the stdout\n", "the stderr\n"), "git exited 3: stderr: the stderr | stdout: the stdout")

    def test_a_stdout_only_failure_names_stdout_and_no_stderr(self) -> None:
        text = self.message(1, "release-preflight: PROBLEM: local tag v9 exists only in this clone\n", "")
        self.assertEqual(text, "git exited 1: stdout: release-preflight: PROBLEM: local tag v9 exists only in this clone")
        self.assertNotIn("stderr", text)

    def test_an_empty_stdout_adds_no_label(self) -> None:
        text = self.message(2, "", "fatal: nope")
        self.assertEqual(text, "git exited 2: stderr: fatal: nope")
        self.assertNotIn("stdout", text)

    def test_a_failure_that_printed_nothing_names_only_the_exit_code(self) -> None:
        self.assertEqual(self.message(4, "", ""), "git exited 4:")

    def test_both_tails_are_bounded_together(self) -> None:
        text = self.message(1, "z" * 5000 + "END-OUT", "q" * 5000 + "END-ERR")
        stderr_part, stdout_part = text.split(" | stdout: ")
        self.assertEqual(stderr_part.count("q"), timer.TAIL - len("END-ERR"))
        self.assertEqual(stdout_part.count("z"), timer.TAIL - len("END-OUT"))
        self.assertTrue(stderr_part.endswith("END-ERR"))
        self.assertTrue(stdout_part.endswith("END-OUT"))

    def test_the_stdout_tail_alone_is_bounded(self) -> None:
        text = self.message(1, "z" * 5000 + "END", "")
        self.assertEqual(text.count("z"), timer.TAIL - 3)
        self.assertTrue(text.endswith("END"))

    def test_the_stderr_tail_alone_is_bounded(self) -> None:
        text = self.message(1, "", "y" * 5000 + "END")
        self.assertEqual(text.count("y"), timer.TAIL - 3)
        self.assertTrue(text.endswith("END"))

    def fail_with(self, argv: list[str], rc: int, out: str, err: str) -> str:
        runner = lambda argv, cwd=None, env=None: completed(argv, rc, out, err)  # noqa: E731
        log = timer.Log(self.log, "")
        self.addCleanup(log.close)
        subject = timer.Timer(self.env, runner, self.sleeps.append, log)
        with self.assertRaises(timer.StepFailure) as raised:
            subject.cmd("probe", argv)
        return str(raised.exception)

    def test_a_curl_failure_records_stderr_and_never_the_remote_body(self) -> None:
        for program in ("curl", "/usr/bin/curl"):
            with self.subTest(program=program):
                text = self.fail_with([program, "--fail", "https://release.invalid/x"], 22, "REMOTE BODY", "curl: (22) 404")
                self.assertEqual(text, "curl exited 22: stderr: curl: (22) 404")

    def test_a_gh_failure_records_stderr_and_never_the_remote_body(self) -> None:
        for program in ("gh", "gh-spritz", "/home/x/.local/bin/gh-spritz", self.env["GH_BIN"]):
            with self.subTest(program=program):
                text = self.fail_with([program, "run", "view", "1"], 1, "REMOTE BODY", "HTTP 502")
                self.assertEqual(text, f"{pathlib.Path(program).name} exited 1: stderr: HTTP 502")

    def test_a_remote_command_with_only_stdout_records_just_the_exit_code(self) -> None:
        self.assertEqual(self.fail_with(["curl", "x"], 22, "REMOTE BODY", ""), "curl exited 22:")

    def test_a_local_command_keeps_its_stdout(self) -> None:
        for program in ("just", "git", "systemd-run", "python3"):
            with self.subTest(program=program):
                out = "release-preflight: PROBLEM: local tag v9 exists only in this clone"
                self.assertEqual(self.fail_with([program, "x"], 1, out, ""), f"{program} exited 1: stdout: {out}")

    def test_the_signers_stdout_is_a_manifest_and_is_never_recorded(self) -> None:
        text = self.fail_with(["/work/release-signer-v5.17.0/release-signer", "verify-manifest", "--live"], 1, "REMOTE BODY", "no")
        self.assertEqual(text, "release-signer exited 1: stderr: no")

    def test_the_signer_the_timer_builds_is_the_program_whose_stdout_is_left_out(self) -> None:
        fake = Fake(self.root, overrides={"verify": lambda argv: completed(argv, 1, "REMOTE BODY", "signer failed")})
        self.assertEqual(self.run_timer(fake), 1)
        verify = next(c["argv"] for c in fake.calls if c["step"] == "verify")
        self.assertEqual(pathlib.Path(verify[0]).name, timer.SIGNER_BINARY)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertIn("signer failed", failed["error"])
        self.assertNotIn("REMOTE BODY", self.log.read_text())

    def test_a_failing_manifest_fetch_leaves_the_remote_body_out_of_the_log(self) -> None:
        fake = Fake(self.root, overrides={"manifest": lambda argv: completed(argv, 22, "REMOTE BODY", "curl: (22) 404")})
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "check_listed")
        self.assertIn("curl: (22) 404", failed["error"])
        self.assertNotIn("REMOTE BODY", self.log.read_text())

    def test_a_failing_gh_call_leaves_the_remote_body_out_of_the_log(self) -> None:
        fake = Fake(self.root, overrides={"check_jobs": lambda argv: completed(argv, 1, "REMOTE BODY", "HTTP 502")})
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "check_jobs")
        self.assertIn("HTTP 502", failed["error"])
        self.assertNotIn("REMOTE BODY", self.log.read_text())

    def test_a_secret_on_stdout_is_redacted_in_the_message_and_the_log(self) -> None:
        leak = {"publish_apk": lambda argv: completed(argv, 1, f"out {SECRETS['R2_SECRET_ACCESS_KEY']} {TOKEN}", "")}
        fake = Fake(self.root, overrides=leak)
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertIn("stdout: out <redacted> <redacted>", failed["error"])
        for secret in [*SECRETS.values(), TOKEN]:
            self.assertNotIn(secret, self.log.read_text())

    def test_a_secret_on_stderr_is_still_redacted(self) -> None:
        text = self.message(1, "", f"boom {SECRETS['R2_ACCESS_KEY_ID']}")
        self.assertEqual(text, "git exited 1: stderr: boom <redacted>")

    def test_a_secret_cut_by_the_tail_leaves_no_fragment(self) -> None:
        secret = SECRETS["R2_SECRET_ACCESS_KEY"]
        for stream in ("out", "err"):
            with self.subTest(stream=stream):
                body = secret + "B" * (timer.TAIL - 4)
                text = self.message(1, body if stream == "out" else "", body if stream == "err" else "")
                self.assertNotIn(secret[-4:], text, "the end of a secret the tail cut survived redaction")


class TagsStep(TimerCase):
    """Every local tag is on origin at the same commit (the rule of tools/release_preflight.py), or the run refuses before
    any token or release step: the preflight inside `just release-quest-apk` would refuse it later, after the build."""

    OFFENDER = "9" * 40

    def refused(self, fake: Fake) -> dict:
        self.assertEqual(self.run_timer(fake), 1)
        [refusal] = [e for e in self.events() if e["event"] == "refused"]
        self.assertEqual(refusal["step"], "tags")
        self.assertEqual([e for e in self.events() if e["event"] == "failed"], [])
        self.assertNotIn("token", fake.steps())
        return refusal

    def test_matching_tags_pass_and_log_the_count_between_checkout_and_token(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        steps = fake.steps()
        self.assertLess(steps.index("checkout.main"), steps.index("tags.local"))
        self.assertLess(steps.index("tags.remote"), steps.index("token"))
        ok = [e for e in self.events() if e["event"] == "step_ok" and e["step"] == "tags"]
        self.assertEqual([e["count"] for e in ok], [2])

    def test_the_tag_commands_ask_for_every_local_tag_and_origins_tags(self) -> None:
        fake = Fake(self.root)
        self.run_timer(fake)
        local = next(c["argv"] for c in fake.calls if c["step"] == "tags.local")
        remote = next(c["argv"] for c in fake.calls if c["step"] == "tags.remote")
        self.assertEqual(local[:5], ["git", "-C", str(self.root / "repo"), "for-each-ref", "refs/tags"])
        self.assertIn("%(*objectname)", local[-1], "the peeled commit of an annotated tag")
        self.assertEqual(remote[3:], ["ls-remote", "--tags", "origin"])

    def test_a_local_only_tag_is_refused_with_its_name_id_and_the_commands(self) -> None:
        fake = Fake(self.root)
        fake.local_tags["v9.9.9"] = (self.OFFENDER, None)
        error = self.refused(fake)["error"]
        self.assertIn("v9.9.9", error)
        self.assertIn(self.OFFENDER, error)
        self.assertIn("local-only", error)
        self.assertIn("git tag -d v9.9.9", error)
        self.assertIn(f"git update-ref refs/tags/v9.9.9 {self.OFFENDER}", error)
        self.assertNotIn("v5.16.1", error, "a tag that matches origin is not named")

    def test_an_annotated_local_only_tag_is_rolled_back_to_its_tag_object_not_its_commit(self) -> None:
        fake = Fake(self.root)
        fake.local_tags["v9.9.9"] = (self.OFFENDER, "4" * 40)
        error = self.refused(fake)["error"]
        self.assertTrue(error.endswith(f"git update-ref refs/tags/v9.9.9 {self.OFFENDER}"), error[-120:])
        self.assertNotIn("4" * 40, error)

    HOSTILE = ["x;touch$IFS/pwn", "k`id`", "s$(id)", "q'uote", "a;b$IFS`c`$(d)'e"]

    def test_every_tag_name_and_id_in_a_printed_command_is_shell_quoted(self) -> None:
        for name in self.HOSTILE:
            with self.subTest(name=name):
                fake = Fake(self.root)
                fake.local_tags[name] = (self.OFFENDER, None)
                error = self.refused(fake)["error"]
                quoted = shlex.quote(name)
                self.assertNotEqual(quoted, name, "the fixture name needs quoting")
                self.assertIn(f"remove: git tag -d {quoted};", error)
                self.assertIn(f"rollback: git update-ref {shlex.quote('refs/tags/' + name)} {shlex.quote(self.OFFENDER)}", error)
                self.assertNotIn(f"git tag -d {name}", error)
                self.assertNotIn(f"refs/tags/{name} ", error)
                self.assertIn(f"{quoted} ({self.OFFENDER[:12]})", error, "the summary shows the name as the commands do")
                self.log.unlink()

    def test_a_malformed_object_id_is_quoted_too(self) -> None:
        fake = Fake(self.root)
        fake.local_tags["v9.9.9"] = ("dead;touch$IFS/x", None)
        error = self.refused(fake)["error"]
        self.assertIn(f"rollback: git update-ref refs/tags/v9.9.9 {shlex.quote('dead;touch$IFS/x')}", error)
        self.assertNotIn("v9.9.9 dead;touch", error)

    def test_the_head_of_the_message_survives_the_couriers_300_character_cut(self) -> None:
        fake = Fake(self.root)
        for n in range(4):
            fake.local_tags[f"v8.0.{n}"] = (f"{n + 1:x}" * 40, None)
        error = self.refused(fake)["error"]
        self.assertTrue(error.startswith("local tags origin lacks or has elsewhere: "), error[:80])
        for n in range(4):
            self.assertIn(f"v8.0.{n} ({(f'{n + 1:x}' * 12)})", error[:300])
        self.assertIn("[4 of 6]", error[:300])

    def test_the_head_lists_ten_then_counts_the_rest(self) -> None:
        fake = Fake(self.root)
        for n in range(13):
            fake.local_tags[f"v8.0.{n}"] = (f"{n % 10}" * 40, None)
        head = self.refused(fake)["error"].split(" | ")[0]
        self.assertEqual(head.count(" ("), 10)
        self.assertTrue(head.endswith(", and 3 more [13 of 15]"), head[-60:])

    def test_a_tag_on_another_commit_than_origin_is_refused(self) -> None:
        fake = Fake(self.root)
        fake.local_tags["v5.16.1"] = (self.OFFENDER, None)
        error = self.refused(fake)["error"]
        self.assertIn("v5.16.1", error)
        self.assertIn(self.OFFENDER, error)
        self.assertIn("points at", error)
        self.assertIn(("1" * 40)[:12], error, "origin's commit")
        self.assertNotIn("local-only", error)
        self.assertIn("git tag -d v5.16.1", error)

    def test_an_annotated_tag_is_compared_by_its_peeled_commit(self) -> None:
        fake = Fake(self.root)
        fake.local_tags["v5.17.0"] = ("7" * 40, "3" * 40)  # another tag object, the same commit as origin's
        self.assertEqual(self.run_timer(fake), 0)
        fake = Fake(self.root)
        fake.local_tags["v5.17.0"] = ("2" * 40, "8" * 40)  # the same tag object id is not enough: the commit differs
        self.assertEqual(self.run_timer(fake), 1)

    def test_a_lightweight_tag_matches_an_annotated_one_on_the_same_commit(self) -> None:
        fake = Fake(self.root)
        fake.local_tags["v5.17.0"] = ("3" * 40, None)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual([e for e in self.events() if e["event"] == "refused"], [])
        ok = [e for e in self.events() if e["event"] == "step_ok" and e["step"] == "tags"]
        self.assertEqual([e["count"] for e in ok], [2], "the tags step ran, found nothing to refuse and said so")

    def test_a_tag_only_origin_has_is_not_an_offender(self) -> None:
        fake = Fake(self.root)
        fake.remote_tags["v6.0.0"] = ("5" * 40, None)
        fake.remote_tags["v6.0.1"] = ("6" * 40, None)
        self.assertEqual(self.run_timer(fake), 0)
        ok = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "tags")
        self.assertEqual(ok["count"], 2, "the count is of the local tags")

    def test_more_than_ten_offenders_are_summarised(self) -> None:
        fake = Fake(self.root)
        for n in range(12):
            fake.local_tags[f"v8.0.{n}"] = (f"{n:x}" * 40, None)
        error = self.refused(fake)["error"]
        self.assertEqual(sum(f"git tag -d v8.0.{n};" in error for n in range(12)), 10)
        self.assertIn("and 2 more", error)
        self.assertIn(", and 2 more [12 of 14]", error)
        self.assertLess(len(error), 3000)

    def test_ten_offenders_are_all_listed_with_no_summary(self) -> None:
        fake = Fake(self.root)
        for n in range(10):
            fake.local_tags[f"v8.0.{n}"] = (f"{n:x}" * 40, None)
        error = self.refused(fake)["error"]
        self.assertEqual(sum(f"git tag -d v8.0.{n};" in error for n in range(10)), 10)
        self.assertNotIn("more", error)

    def test_a_refusal_is_not_counted_and_the_clean_run_proceeds(self) -> None:
        self.write_log({"event": "refused", "tag": "", "step": "tags", "error": "x"},
                       {"event": "refused", "tag": "", "step": "tags", "error": "x"})
        dirty = Fake(self.root)
        dirty.local_tags["v9.9.9"] = (self.OFFENDER, None)
        self.assertEqual(self.run_timer(dirty), 1)
        self.assertEqual(len([e for e in self.events() if e["event"] == "refused"]), 3)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("token", fake.steps())
        self.assertNotIn("skipped_after_failures", self.names())
        for call in fake.calls:
            if call["step"] == "courier":
                self.assertNotIn("gave up", " ".join(call["argv"]), "the skip notice was sent")
        self.assertEqual(self.names()[-1], "done")

    def test_a_refusal_runs_nothing_after_the_tag_check_and_couriers_no_one_itself(self) -> None:
        fake = Fake(self.root)
        fake.local_tags["v9.9.9"] = (self.OFFENDER, None)
        self.refused(fake)
        self.assertEqual({c["step"] for c in fake.calls}, {"checkout.fetch", "checkout.status", "checkout.head",
                                                            "checkout.main", "tags.local", "tags.remote"})

    def test_a_failing_ls_remote_is_a_failure_not_a_refusal_with_its_output_in_the_record(self) -> None:
        cut = {"tags.remote": lambda argv: completed(argv, 128, "partial listing", "fatal: unable to access origin")}
        fake = Fake(self.root, overrides=cut)
        self.assertEqual(self.run_timer(fake), 1)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual([e["step"] for e in failed], ["tags"])
        self.assertEqual([e for e in self.events() if e["event"] == "refused"], [])
        self.assertEqual(failed[0]["error"], "git exited 128: stderr: fatal: unable to access origin | stdout: partial listing")
        self.assertNotIn("token", fake.steps())

    def test_a_failing_ls_remote_counts_toward_the_three_failures_of_a_named_tag(self) -> None:
        cut = {"tags.remote": lambda argv: completed(argv, 128, "", "fatal: unable to access origin")}
        for _ in range(3):
            self.assertEqual(self.run_timer(Fake(self.root, overrides=cut), "--tag", TAG), 1)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "failed"], ["tags"] * 3)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertIn("skipped_after_failures", self.names())
        self.assertNotIn("worktree.add", fake.steps())

    def test_a_failing_for_each_ref_is_a_counted_failure(self) -> None:
        fake = Fake(self.root, fail="tags.local")
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "failed"], ["tags"])
        self.assertNotIn("tags.remote", fake.steps())


class TagsAgreeWithThePreflight(TimerCase):
    """The same real repositories, judged by tools/release_preflight.py and by the timer, name the same tags."""

    FLAGGED = ["v2.0.0", "v2.1.0", "v3.0.0"]

    def git(self, repo: pathlib.Path, *args: str) -> str:
        argv = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false",
                "-c", "tag.gpgsign=false", *args]
        return subprocess.run(argv, check=True, capture_output=True, text=True).stdout.strip()

    def clone_with_tags(self, offenders: bool) -> pathlib.Path:
        """A clone whose HEAD is origin's main; v1.0.0 (lightweight) and v1.1.0 (annotated) are on origin; with
        `offenders`, v2.0.0 and v2.1.0 exist only here and v3.0.0 points at another commit than on origin."""
        origin, clone = self.root / "origin.git", self.root / "repo"
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", str(origin)], check=True)
        subprocess.run(["git", "clone", "-q", str(origin), str(clone)], check=True, capture_output=True)
        self.git(clone, "checkout", "-q", "-b", "main")
        self.git(clone, "commit", "-q", "--allow-empty", "-m", "one")
        first = self.git(clone, "rev-parse", "HEAD")
        self.git(clone, "commit", "-q", "--allow-empty", "-m", "two")
        self.git(clone, "tag", "v1.0.0")
        self.git(clone, "tag", "-a", "-m", "v1.1.0", "v1.1.0")
        self.git(clone, "push", "-q", "origin", "main", "v1.0.0", "v1.1.0")
        if offenders:
            self.git(clone, "tag", "v2.0.0")
            self.git(clone, "tag", "-a", "-m", "v2.1.0", "v2.1.0")
            self.git(clone, "tag", "v3.0.0", first)
            self.git(clone, "push", "-q", "origin", "v3.0.0")
            self.git(clone, "tag", "-f", "v3.0.0")
        return clone

    def preflight_flagged(self, clone: pathlib.Path) -> list[str]:
        import release_preflight

        problems, _ = release_preflight.check(str(clone), "main", expect_origin=".*")
        return sorted(p.split()[2] if p.startswith("local tag") else p.split()[1] for p in problems
                      if p.startswith(("local tag ", "tag ")))

    def timer_env(self, clone: pathlib.Path, gh_exit: int = 4) -> dict:
        gh = self.root / "gh-fake"
        gh.write_text(f"#!{sys.executable}\nimport sys\nsys.exit({gh_exit})\n")
        gh.chmod(0o755)
        return {**self.env, "QUEST_RELEASE_REPO": str(clone), "GH_BIN": str(gh)}

    def test_the_two_rules_flag_the_same_tags(self) -> None:
        clone = self.clone_with_tags(offenders=True)
        flagged = self.preflight_flagged(clone)
        self.assertEqual(flagged, self.FLAGGED)
        log = timer.Log(self.log, "")
        self.addCleanup(log.close)
        subject = timer.Timer({**self.env, "QUEST_RELEASE_REPO": str(clone)}, timer.subprocess_runner, self.sleeps.append, log)
        with self.assertRaises(timer.Refusal) as raised:
            subject.check_tags()
        message = str(raised.exception)
        self.assertEqual(sorted(n for n in ("v1.0.0", "v1.1.0", *self.FLAGGED) if f"git tag -d {n};" in message), flagged)

    def test_through_main_the_run_is_refused_for_the_tags_the_preflight_flags(self) -> None:
        clone = self.clone_with_tags(offenders=True)
        flagged = self.preflight_flagged(clone)
        self.assertEqual(timer.main([], self.timer_env(clone), runner=timer.subprocess_runner, sleep=self.sleeps.append), 1)
        [refusal] = [e for e in self.events() if e["event"] == "refused"]
        self.assertEqual(refusal["step"], "tags")
        head = refusal["error"].split(" | ")[0]
        self.assertEqual(sorted(n for n in ("v1.0.0", "v1.1.0", *self.FLAGGED) if f"{n} (" in head), flagged)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "step_ok"], ["checkout"], "no token was asked for")

    def test_through_main_a_clone_the_preflight_accepts_passes_the_tags_step(self) -> None:
        clone = self.clone_with_tags(offenders=False)
        self.assertEqual(self.preflight_flagged(clone), [])
        self.assertEqual(timer.main([], self.timer_env(clone), runner=timer.subprocess_runner, sleep=self.sleeps.append), 1)
        self.assertEqual([e for e in self.events() if e["event"] == "refused"], [])
        ok = [e for e in self.events() if e["event"] == "step_ok"]
        self.assertEqual([(e["step"], e.get("count")) for e in ok if e["step"] == "tags"], [("tags", 2)])
        self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "token", "it got past the tags step")

    def test_the_printed_commands_remove_and_restore_a_hostile_tag_in_a_real_shell(self) -> None:
        clone = self.clone_with_tags(offenders=False)
        marker = self.root / "pwned"
        hostile = f"x;touch$IFS{marker}"
        self.git(clone, "tag", hostile)
        object_id = self.git(clone, "rev-parse", f"refs/tags/{hostile}")
        self.assertEqual(timer.main([], self.timer_env(clone), runner=timer.subprocess_runner, sleep=self.sleeps.append), 1)
        [refusal] = [e for e in self.events() if e["event"] == "refused"]
        remove = refusal["error"].split("remove: ", 1)[1].split("; rollback: ", 1)[0]
        rollback = refusal["error"].split("rollback: ", 1)[1].split(" | ", 1)[0]
        subprocess.run(["sh", "-c", remove], cwd=clone, check=True, capture_output=True)
        self.assertNotIn(hostile, self.git(clone, "tag", "--list").splitlines())
        subprocess.run(["sh", "-c", rollback], cwd=clone, check=True, capture_output=True)
        self.assertEqual(self.git(clone, "rev-parse", f"refs/tags/{hostile}"), object_id)
        self.assertFalse(marker.exists(), "a printed command ran the tag name as a command")


class NotifiesOnRefusal(TimerCase):
    def notify(self) -> list[list[str]]:
        sent: list[list[str]] = []

        def courier(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
            sent.append(argv)
            return completed(argv)

        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=courier, sleep=self.sleeps.append), 0)
        return sent

    def test_the_notice_after_a_refusal_couriers_the_refusal(self) -> None:
        self.write_log({"event": "failed", "tag": TAG, "step": "build_apk", "error": "old build failure"},
                       {"event": "refused", "tag": "", "step": "checkout",
                        "error": "the checkout the timer runs from is not clean: ' M x'"})
        body = self.notify()[0][-1]
        self.assertIn("checkout", body)
        self.assertIn("not clean", body)
        self.assertNotIn("old build failure", body)

    def test_a_failure_after_a_refusal_is_the_one_couriered(self) -> None:
        self.write_log({"event": "refused", "tag": "", "step": "checkout", "error": "not clean"},
                       {"event": "failed", "tag": TAG, "step": "build_apk", "error": "build broke"})
        body = self.notify()[0][-1]
        self.assertIn("build_apk", body)
        self.assertIn("build broke", body)
        self.assertNotIn("not clean", body)

    def test_a_missing_credential_refusal_names_what_is_missing_and_a_tag_not_chosen_yet(self) -> None:
        self.write_log({"event": "refused", "tag": "", "missing": ["R2_BUCKET", "RELEASE_PUBLIC_BASE"]})
        body = self.notify()[0][-1]
        self.assertIn("R2_BUCKET, RELEASE_PUBLIC_BASE", body)
        self.assertIn("not chosen yet", body)
        self.assertNotIn("no failed event recorded", body)


class FailureNoticeIsNotRepeated(TimerCase):
    """The unit couriers the last failure after every failed run. A standing refusal is the same text every 30 minutes, so
    the same step and error is couriered at most once in six hours (`failure_notified`, then `failure_notice_suppressed`)."""

    ERROR = "local tags origin lacks or has elsewhere: v9.9.9 (999999999999) [1 of 3]"

    def setUp(self) -> None:
        super().setUp()
        self.sent: list[list[str]] = []

    def notify(self) -> int:
        def courier(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
            self.sent.append(argv)
            return completed(argv)

        return timer.main(["--notify-failure"], self.env, runner=courier, sleep=self.sleeps.append)

    def refusal(self, error: str = ERROR, step: str = "tags") -> dict:
        return {"event": "refused", "tag": "", "step": step, "error": error}

    def append(self, *records: dict) -> None:
        with self.log.open("a", encoding="utf-8") as handle:
            handle.writelines(json.dumps(r) + "\n" for r in records)

    @staticmethod
    def digest(error: str) -> str:
        return hashlib.sha256(error.encode("utf-8")).hexdigest()

    @staticmethod
    def hours_ago(hours: float) -> str:
        return (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(hours=hours)).isoformat()

    def notified(self, error: str = ERROR, step: str = "tags", hours: float = 1) -> dict:
        return {"event": "failure_notified", "ts": self.hours_ago(hours), "tag": "", "step": step, "error_sha256": self.digest(error)}

    def test_the_first_notice_is_sent_and_recorded_with_the_step_and_the_hash_of_the_error(self) -> None:
        self.write_log(self.refusal())
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 1)
        [record] = [e for e in self.events() if e["event"] == "failure_notified"]
        self.assertEqual((record["step"], record["error_sha256"]), ("tags", self.digest(self.ERROR)))

    def test_the_same_error_twice_in_a_row_sends_one_courier_and_logs_the_suppression(self) -> None:
        self.write_log(self.refusal())
        self.assertEqual(self.notify(), 0)
        self.append(self.refusal())  # the next 30-minute run refuses the same way
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.names().count("failure_notified"), 1)
        [suppressed] = [e for e in self.events() if e["event"] == "failure_notice_suppressed"]
        self.assertEqual((suppressed["step"], suppressed["error_sha256"]), ("tags", self.digest(self.ERROR)))

    def test_a_different_error_is_sent_again(self) -> None:
        self.write_log(self.refusal())
        self.assertEqual(self.notify(), 0)
        self.append(self.refusal("local tags origin lacks or has elsewhere: v9.9.8 (888888888888) [1 of 3]"))
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 2)
        self.assertNotIn("failure_notice_suppressed", self.names())

    def test_the_same_error_on_another_step_is_sent_again(self) -> None:
        self.write_log(self.refusal(), self.notified(step="checkout"))
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 1)

    def test_the_same_error_after_six_hours_is_sent_again(self) -> None:
        self.write_log(self.refusal(), self.notified(hours=6.5))
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.names().count("failure_notified"), 2)

    def test_the_same_error_just_under_six_hours_is_suppressed(self) -> None:
        self.write_log(self.refusal(), self.notified(hours=5.9))
        self.assertEqual(self.notify(), 0)
        self.assertEqual(self.sent, [])
        self.assertIn("failure_notice_suppressed", self.names())

    def test_only_the_newest_notice_counts(self) -> None:
        self.write_log(self.refusal(), self.notified(hours=1), self.notified("another error", hours=0.5))
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 1, "the newest notice was for another error")

    def test_a_notice_dated_in_the_future_or_unreadable_does_not_suppress(self) -> None:
        for ts in (self.hours_ago(-3), "not a time", None, (datetime.datetime.now() - datetime.timedelta(hours=1)).isoformat()):
            with self.subTest(ts=ts):
                self.write_log(self.refusal(), {**self.notified(hours=1), "ts": ts})
                self.sent.clear()
                self.assertEqual(self.notify(), 0)
                self.assertEqual(len(self.sent), 1)

    def test_a_failed_run_is_deduplicated_like_a_refusal(self) -> None:
        failed = {"event": "failed", "tag": TAG, "step": "build_apk", "error": "build broke"}
        self.write_log(failed, self.notified("build broke", "build_apk", hours=1))
        self.assertEqual(self.notify(), 0)
        self.assertEqual(self.sent, [])
        self.assertIn("failure_notice_suppressed", self.names())

    def test_a_credential_refusal_is_deduplicated_by_what_it_names(self) -> None:
        missing = {"event": "refused", "tag": "", "missing": ["R2_BUCKET"]}
        self.write_log(missing)
        self.assertEqual(self.notify(), 0)
        self.append(missing)
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 1)
        self.append({"event": "refused", "tag": "", "missing": ["R2_BUCKET", "RELEASE_PUBLIC_BASE"]})
        self.assertEqual(self.notify(), 0)
        self.assertEqual(len(self.sent), 2)

    def test_a_notice_that_could_not_be_sent_is_not_recorded_as_sent(self) -> None:
        self.write_log(self.refusal())
        broken = lambda argv, cwd=None, env=None: completed(argv, 1, "", "no courier")  # noqa: E731
        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=broken, sleep=self.sleeps.append), 1)
        self.assertNotIn("failure_notified", self.names())

    def test_a_log_that_cannot_record_the_notice_is_loud(self) -> None:
        self.write_log(self.refusal())
        self.log.chmod(0o444)
        self.addCleanup(self.log.chmod, 0o644)
        self.assertEqual(self.notify(), 1)
        self.assertEqual(len(self.sent), 1, "the courier went before the record failed")

    def test_a_log_that_cannot_record_the_suppression_is_loud(self) -> None:
        self.write_log(self.refusal(), self.notified())
        self.log.chmod(0o444)
        self.addCleanup(self.log.chmod, 0o644)
        self.assertEqual(self.notify(), 1)
        self.assertEqual(self.sent, [])


class RealProcess(TimerCase):
    """The tool as the unit starts it: a real interpreter, a real log file, a fake `gh` first on the path."""

    def start(self, env: dict, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "quest_release_timer.py"), *args],
                              capture_output=True, text=True, env=env, cwd=self.tmp.name)

    def test_a_missing_credential_is_logged_and_exits_1(self) -> None:
        env = {k: v for k, v in self.env.items() if k != "R2_BUCKET"}
        result = self.start(env)
        self.assertEqual(result.returncode, 1)
        self.assertIn("refused: set R2_BUCKET", result.stderr)
        self.assertEqual([e["event"] for e in self.events()], ["start", "refused"])
        self.assertEqual(self.events()[1]["missing"], ["R2_BUCKET"])

    def real_checkout(self) -> pathlib.Path:
        """A clean git checkout whose origin/main is its own HEAD, so the checkout step passes with real git."""
        repo = self.root / "repo"
        repo.mkdir()
        git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false"]
        for argv in (["init", "-q", "-b", "main"], ["commit", "-q", "--allow-empty", "-m", "x"],
                     ["remote", "add", "origin", str(repo)]):
            subprocess.run([*git, *argv], check=True, capture_output=True)
        return repo

    def test_a_checkout_that_is_not_a_repository_is_logged_and_exits_1(self) -> None:
        result = self.start(self.env)
        self.assertEqual(result.returncode, 1)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "failed"], ["checkout"])

    def test_a_real_clean_checkout_at_origin_main_passes_the_checkout_step(self) -> None:
        self.real_checkout()
        gh = self.root / "gh-fake"
        gh.write_text(f"#!{sys.executable}\nimport sys\nsys.exit(4)\n")
        gh.chmod(0o755)
        self.start({**self.env, "GH_BIN": str(gh)})
        self.assertIn("checkout", [e["step"] for e in self.events() if e["event"] == "step_ok"])

    def test_a_failing_first_command_is_logged_with_its_step_and_no_secret(self) -> None:
        self.real_checkout()
        gh = self.root / "gh-fake"
        gh.write_text(f"#!{sys.executable}\nimport sys\nsys.stderr.write('denied {SECRETS['R2_SECRET_ACCESS_KEY']}')\nsys.exit(4)\n")
        gh.chmod(0o755)
        result = self.start({**self.env, "GH_BIN": str(gh)})
        self.assertEqual(result.returncode, 1)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual([e["step"] for e in failed], ["token"])
        self.assertNotIn(SECRETS["R2_SECRET_ACCESS_KEY"], self.log.read_text())
        self.assertNotIn(SECRETS["R2_SECRET_ACCESS_KEY"], result.stderr)


class UnitsAndRecipe(unittest.TestCase):
    units = ROOT / "tools" / "systemd"

    def read(self, name: str) -> str:
        return (self.units / name).read_text(encoding="utf-8")

    def test_the_timer_runs_the_service_often_and_catches_up(self) -> None:
        text = self.read("nevr-quest-release.timer")
        self.assertIn("OnCalendar=*:0/30", text)
        self.assertIn("Persistent=true", text)
        self.assertIn("Unit=nevr-quest-release.service", text)
        self.assertIn("WantedBy=timers.target", text)

    def test_the_service_runs_from_a_dedicated_checkout_it_moves_to_origin_main_itself(self) -> None:
        text = self.read("nevr-quest-release.service")
        checkout = "%h/src/nevr-runtime-work/release-timer/checkout"
        self.assertIn(f"WorkingDirectory={checkout}", text)
        self.assertIn(f"ExecStart=/usr/bin/python3 -I {checkout}/tools/quest_release_timer.py", text)
        self.assertIn(f"ExecStartPre=/usr/bin/git -C {checkout} fetch --quiet origin main", text)
        self.assertIn(f"ExecStartPre=/usr/bin/git -C {checkout} switch --quiet --detach origin/main", text)
        self.assertLess(text.index("fetch --quiet"), text.index("switch --quiet"))
        self.assertLess(text.index("switch --quiet"), text.index("ExecStart=/usr/bin/python3"))

    def test_the_service_runs_the_tool_loads_the_credential_file_and_notifies_on_failure(self) -> None:
        text = self.read("nevr-quest-release.service")
        self.assertIn("Type=oneshot", text)
        self.assertIn("quest_release_timer.py", text)
        self.assertIn("EnvironmentFile=%h/.config/metis/r2-evr-echo-taxi.env", text)
        self.assertIn("OnFailure=nevr-quest-release-failed.service", text)
        self.assertIn("RELEASE_PUBLIC_BASE=", text)
        self.assertNotRegex(text, r"(?i)(secret|password|token)\s*=\s*\S")

    def test_the_failure_service_runs_the_notifier(self) -> None:
        text = self.read("nevr-quest-release-failed.service")
        self.assertIn("quest_release_timer.py --notify-failure", text)
        self.assertIn("release-timer/checkout/tools/quest_release_timer.py", text)

    def test_the_recipe_links_the_units_from_the_repo_and_arms_the_timer(self) -> None:
        recipe = (ROOT / "justfile").read_text(encoding="utf-8")
        start = recipe.index("install-quest-release-timer")
        body = recipe[start:start + 2600]
        for want in ("systemctl --user link", "nevr-quest-release.timer", "nevr-quest-release.service",
                     "nevr-quest-release-failed.service", "daemon-reload", "enable --now nevr-quest-release.timer",
                     "worktree add --detach", "release-timer/checkout"):
            self.assertIn(want, body)
        self.assertIn('link "$CHECKOUT/tools/systemd/$unit"', body, "the units are linked from the dedicated checkout")

    def test_the_doc_says_the_timer_takes_the_newest_published_tag_only(self) -> None:
        text = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
        section = text.split("## The Quest half of the train", 1)[1].split("\n## ", 1)[0]
        self.assertIn("newest published tag only", section)
        self.assertIn("dedicated checkout", section)


if __name__ == "__main__":
    unittest.main()
