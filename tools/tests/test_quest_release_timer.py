"""`tools/quest_release_timer.py` (#1210): the Quest half of a release, run by a user timer on the publisher's machine.
For the newest published tag that is not done it builds the release signer, makes a clean worktree at the tag, builds and
publishes the Quest APK, dispatches the beta manifest publish live, requires every job that publishes to have run, and
requires `verify-manifest --live` to list the tag. The commands are fakes that record what they were called with; the
log is real, and no secret may reach it."""

import hashlib
import json
import pathlib
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
STEPS = ["checkout", "token", "list", "build_signer", "check_listed", "worktree", "build_slot", "build_apk", "publish_apk",
         "dispatch", "watch", "check_jobs", "verify_live", "notify", "cleanup"]
CARRIED = {"name": "echovr-data-fc2eedeacc50.zip", "kind": "game-data", "size": 937395400}
HEAD = "c0ffee" + "0" * 34
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

    def step_of(self, argv: list[str]) -> str:
        joined = " ".join(argv)
        repo = f"{self.root}/repo"
        table = [
            (f"{repo} fetch --quiet origin main", "checkout.fetch"), (f"{repo} status --porcelain", "checkout.status"),
            (f"{repo} rev-parse HEAD", "checkout.head"), (f"{repo} rev-parse origin/main", "checkout.main"),
            ("curl", "manifest"), ("bonk", "bonk"), ("courier send", "courier"),
            ("auth token", "token"), ("release list", "list"), (" fetch origin main", "build_signer.fetch"),
            ("rev-parse origin/main", "build_signer.rev"), (" archive ", "build_signer.archive"), ("tar -xf", "build_signer.tar"),
            ("go build", "build_signer.go"), ("verify-manifest", "verify"), (" fetch origin tag ", "worktree.fetch"),
            ("^{commit}", "worktree.rev"), ("worktree add", "worktree.add"), ("worktree-setup", "worktree.setup"),
            ("pgrep", "build_slot"), ("release-quest-apk", "build_apk"), ("publish_quest_apk.py", "publish_apk"),
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
                 for step in ("build_signer", "worktree", "build_slot", "build_apk", "publish_apk", "dispatch", "watch")}
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
        self.assertEqual(fake.steps()[4:], ["token", "list"])
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
        self.assertEqual([s for s in fake.steps() if s not in ("bonk", "courier")][4:], ["token", "list"])
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
    def test_a_live_publish_bonks_sprockee_and_couriers_teth_before_it_is_done(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        steps = fake.steps()
        self.assertLess(steps.index("verify"), steps.index("bonk"))
        bonk = next(c["argv"] for c in fake.calls if c["step"] == "bonk")
        self.assertEqual(bonk[:3], ["bonk", "--from", "quest-release-timer"])
        self.assertIn(TAG, bonk[-1])
        courier = next(c["argv"] for c in fake.calls if c["step"] == "courier")
        self.assertEqual(courier[:6], ["courier", "send", "--to", "teth", "--from", "quest-release-timer"])
        self.assertIn(TAG, courier[-1])
        for secret in [*SECRETS.values(), TOKEN]:
            self.assertNotIn(secret, " ".join(bonk + courier))
        self.assertEqual(self.names()[-1], "done")

    def test_a_notice_that_fails_is_a_failure_and_the_tag_is_not_done(self) -> None:
        for step in ("bonk", "courier"):
            with self.subTest(step=step):
                self.log.unlink(missing_ok=True)
                fake = Fake(self.root, fail=step)
                self.assertEqual(self.run_timer(fake), 1)
                self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "notify")
                self.assertNotIn("done", self.names())

    def test_a_live_tag_whose_notice_was_never_sent_is_noticed_on_the_next_run(self) -> None:
        self.write_log({"event": "step_ok", "tag": TAG, "step": "verify_live"})
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("bonk", fake.steps())
        self.assertIn("courier", fake.steps())
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
    def test_the_third_failure_skip_bonks_and_couriers_before_it_exits_0(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(3)))
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("skipped_after_failures", self.names())
        bonk = next(c["argv"] for c in fake.calls if c["step"] == "bonk")
        self.assertIn(TAG, bonk[-1])
        courier = next(c["argv"] for c in fake.calls if c["step"] == "courier")
        self.assertEqual(courier[:6], ["courier", "send", "--to", "spritz", "--from", "quest-release-timer"])
        self.assertIn("build_apk", courier[-1])
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
        bonk = next(c["argv"] for c in fake.calls if c["step"] == "bonk")
        self.assertIn("gave up", bonk[-1])
        self.assertIn("build_apk", bonk[-1])

    def test_refusals_between_real_failures_do_not_hide_the_third_failure(self) -> None:
        self.write_log({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"},
                       {"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"},
                       {"event": "refused", "tag": "", "step": "checkout", "error": "not clean"})
        self.assertEqual(self.run_timer(Fake(self.root, fail="build_apk")), 1)
        self.assertEqual(len([e for e in self.events() if e["event"] == "failed"]), 3)
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertIn("skipped_after_failures", self.names())


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
