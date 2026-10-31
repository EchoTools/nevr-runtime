"""`tools/quest_release_timer.py` (#1210): the Quest half of a release, run by a user timer on the publisher's machine.
For the newest published tag that is not done it builds the release signer, makes a clean worktree at the tag, builds and
publishes the Quest APK, dispatches the beta manifest publish live, requires every job that publishes to have run, and
requires `verify-manifest --live` to list the tag. The commands are fakes that record what they were called with; the
log is real, and no secret may reach it."""

import contextlib
import datetime
import fcntl
import gzip
import hashlib
import http.server
import io
import json
import os
import pathlib
import random
import re
import shlex
import shutil
import socket
import socketserver
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from collections.abc import Callable
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import quest_release_timer as timer  # noqa: E402

TAG = "v5.17.0"
DRAFT = "v5.18.0"  # a draft release, newer than TAG: the draft stage's tag
SECRETS = {
    "R2_ENDPOINT": "https://sentinel-endpoint.invalid",
    "R2_BUCKET": "sentinel-bucket",
    "R2_ACCESS_KEY_ID": "sentinel-access-key-id",
    "R2_SECRET_ACCESS_KEY": "sentinel-secret-access-key",
}
TOKEN = "sentinel-gh-token"
STEPS = ["checkout", "tags", "token", "list", "build_signer", "check_listed", "worktree", "build_slot", "configure", "build_apk",
         "publish_apk", "dispatch", "watch", "check_jobs", "verify_live", "notify", "cleanup"]
CARRIED = {"name": "echovr-data-fc2eedeacc50.zip", "kind": "game-data", "platform": "android-arm64", "size": 937395400}
PC_DATA = {"name": "echovr-data-win10-0123456789ab.zip", "kind": "game-data", "platform": "windows-x86_64", "size": 5120}
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
        self.extra_data: list[dict] = []  # other game-data entries every manifest lists, before the Android one
        self.extra_data_after: list[dict] = []  # ... and after it
        self.jobs = [{"name": name, "conclusion": "success"} for name in PUBLISHING_JOBS]
        self.local_tags: dict = dict(TAGS)  # what the checkout holds
        self.remote_tags: dict = dict(TAGS)  # what origin holds
        self.api_listing: str | None = None  # the REST release list (`gh api`); None is default_api()
        self.draft_polls: list[bool] = [False]  # `isDraft` of each `gh release view`, in order; the last value repeats
        self.poll_answers: list[bool] = []  # what each `isDraft` poll answered, in order
        self.view_rc = 0  # nonzero: a `gh release view` fails, which is a release that is gone
        self.loader_rc = 0  # the exit of `quest_release_loader.py verify-apk`
        self.loader_digest = "sha256:" + "cd" * 32  # the digest GitHub gives the published loader asset
        self.loader_state = "uploaded"  # the loader asset's state in the release listing

    def assets_for(self, tag: str) -> list[dict]:
        return [{"name": timer.loader_asset_name(tag[1:]), "state": self.loader_state, "digest": self.loader_digest}]

    def default_api(self) -> str:
        """The REST list of a repository whose newest draft carries its uploaded loader asset."""
        return rest_releases(DRAFT)

    def step_of(self, argv: list[str]) -> str:
        joined = " ".join(argv)
        repo = f"{self.root}/repo"
        table = [
            ("releases?per_page", "list_api"), ("release view", "release_view"), ("verify-apk", "verify_apk"),
            (f"{repo} fetch --quiet origin main", "checkout.fetch"), (f"{repo} status --porcelain", "checkout.status"),
            (f"{repo} for-each-ref refs/tags", "tags.local"), (f"{repo} ls-remote --tags origin", "tags.remote"),
            (f"{repo} rev-parse HEAD", "checkout.head"), (f"{repo} rev-parse origin/main", "checkout.main"),
            ("curl", "manifest"), ("bonk", "bonk"), ("courier send", "courier"),
            ("auth token", "token"), ("release list", "list"), (" fetch origin main", "build_signer.fetch"),
            ("rev-parse origin/main", "build_signer.rev"), (" archive ", "build_signer.archive"), ("tar -xf", "build_signer.tar"),
            ("go build", "build_signer.go"), ("verify-manifest", "verify"), (" fetch origin tag ", "worktree.fetch"),
            ("^{commit}", "worktree.rev"), ("worktree add", "worktree.add"), ("worktree-setup", "worktree.setup"),
            ("pgrep", "build_slot"), ("just configure", "configure"), ("release-quest-apk", "build_apk"),
            ("release-quest-variant-apk", "variant_build"), (" --variant ", "variant_publish"),
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
        if step == "list_api":
            return completed(argv, 0, self.api_listing if self.api_listing is not None else self.default_api())
        if step == "release_view":
            if "assets" in argv:
                return completed(argv, 0, json.dumps({"assets": self.assets_for(DRAFT)}))
            if self.view_rc:
                return completed(argv, self.view_rc, "", "release not found")
            draft = self.draft_polls.pop(0) if len(self.draft_polls) > 1 else self.draft_polls[0]
            self.poll_answers.append(draft)
            return completed(argv, 0, json.dumps({"isDraft": draft}))
        if step == "verify_apk":
            return completed(argv, self.loader_rc, "", "the loader differs" if self.loader_rc else "")
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
        artifacts[:0] = [dict(a) for a in self.extra_data]
        artifacts.extend(dict(a) for a in self.extra_data_after)
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

    def test_a_pc_game_data_entry_listed_before_the_android_one_is_never_the_one_carried(self) -> None:
        fake = Fake(self.root)
        fake.extra_data = [dict(PC_DATA)]
        self.assertEqual(self.run_timer(fake), 0)
        listed = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "check_listed" and not e["listed"])
        self.assertEqual((listed["carried"], listed["carried_size"]), (CARRIED["name"], CARRIED["size"]))
        verified = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "verify_live")
        self.assertEqual(verified["game_data"], CARRIED["name"])

    def test_a_pc_game_data_entry_listed_after_the_android_one_is_never_the_one_carried(self) -> None:
        fake = Fake(self.root)
        fake.extra_data_after = [dict(PC_DATA)]
        self.assertEqual(self.run_timer(fake), 0)
        listed = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "check_listed" and not e["listed"])
        self.assertEqual((listed["carried"], listed["carried_size"]), (CARRIED["name"], CARRIED["size"]))
        verified = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "verify_live")
        self.assertEqual(verified["game_data"], CARRIED["name"])

    def test_a_changed_android_entry_is_a_failure_whatever_order_the_pc_entry_is_in(self) -> None:
        for position in ("extra_data", "extra_data_after"):
            with self.subTest(position=position):
                fake = Fake(self.root)
                setattr(fake, position, [dict(PC_DATA)])
                fake.published_game_data = {**CARRIED, "size": CARRIED["size"] - 1}
                self.assertEqual(self.run_timer(fake), 1)
                failed = [e for e in self.events() if e["event"] == "failed"][-1]
                self.assertEqual(failed["step"], "verify_live")
                self.assertIn(CARRIED["name"], failed["error"])

    def test_two_android_game_data_entries_stop_before_the_build(self) -> None:
        fake = Fake(self.root)
        fake.extra_data = [{**CARRIED, "name": "echovr-data-aaaaaaaaaaaa.zip"}]
        self.assertEqual(self.run_timer(fake), 1)
        failed = next(e for e in self.events() if e["event"] == "failed")
        self.assertEqual(failed["step"], "check_listed")
        self.assertIn("5.16.1", failed["error"])
        self.assertIn("android-arm64", failed["error"])
        self.assertNotIn("worktree.add", fake.steps())

    def test_two_android_game_data_entries_in_the_published_release_are_a_failure(self) -> None:
        fake = Fake(self.root)
        genuine = fake.manifest_bytes

        def doubled(argv: list[str]) -> subprocess.CompletedProcess:
            body = json.loads(genuine())
            if any(c["step"] == "watch" for c in fake.calls):
                release = body["releases"][body["channels"]["beta"]]
                release["artifacts"].append({**CARRIED, "name": "echovr-data-aaaaaaaaaaaa.zip"})
            return completed(argv, 0, json.dumps(body))

        fake.overrides["manifest"] = doubled
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(next(e for e in self.events() if e["event"] == "failed")["step"], "verify_live")

    def test_a_manifest_with_only_a_pc_game_data_entry_has_none_to_carry(self) -> None:
        fake = Fake(self.root)
        fake.carried = None
        fake.extra_data = [dict(PC_DATA)]
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
        self.assertIn("git tag -d -- v9.9.9", error)
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
                self.assertIn(f"remove: git tag -d -- {quoted};", error)
                self.assertIn(f"rollback: git update-ref {shlex.quote('refs/tags/' + name)} {shlex.quote(self.OFFENDER)}", error)
                self.assertNotIn(f"git tag -d -- {name}", error)
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
        self.assertIn("git tag -d -- v5.16.1", error)

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
        self.assertEqual(sum(f"git tag -d -- v8.0.{n};" in error for n in range(12)), 10)
        self.assertIn("and 2 more", error)
        self.assertIn(", and 2 more [12 of 14]", error)
        self.assertLess(len(error), 3000)

    def test_ten_offenders_are_all_listed_with_no_summary(self) -> None:
        fake = Fake(self.root)
        for n in range(10):
            fake.local_tags[f"v8.0.{n}"] = (f"{n:x}" * 40, None)
        error = self.refused(fake)["error"]
        self.assertEqual(sum(f"git tag -d -- v8.0.{n};" in error for n in range(10)), 10)
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
        self.assertEqual(sorted(n for n in ("v1.0.0", "v1.1.0", *self.FLAGGED) if f"git tag -d -- {n};" in message), flagged)

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

    def printed_commands(self, error: str) -> dict[str, tuple[str, str]]:
        """name -> (remove command, rollback command), read back from a refusal message's command entries."""
        found = {}
        for entry in error.split(" | ")[1:]:
            name = shlex.split(entry.split(" (", 1)[0])[0]
            found[name] = (entry.split("remove: ", 1)[1].split("; rollback: ", 1)[0], entry.split("rollback: ", 1)[1])
        return found

    def test_the_printed_commands_remove_and_restore_tags_named_like_git_switches(self) -> None:
        clone = self.clone_with_tags(offenders=False)
        names = ["-dash", "-rf", "-d"]
        ids = {}
        for name in names:
            self.git(clone, "update-ref", f"refs/tags/{name}", "HEAD")
            ids[name] = self.git(clone, "rev-parse", f"refs/tags/{name}")
        self.assertEqual(timer.main([], self.timer_env(clone), runner=timer.subprocess_runner, sleep=self.sleeps.append), 1)
        [refusal] = [e for e in self.events() if e["event"] == "refused"]
        commands = self.printed_commands(refusal["error"])
        self.assertEqual(sorted(commands), sorted(names))
        for name in names:
            with self.subTest(name=name):
                remove, rollback = commands[name]
                done = subprocess.run(["sh", "-c", remove], cwd=clone, capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertNotIn(name, self.git(clone, "tag", "--list").splitlines(), "the printed remove left the tag")
                subprocess.run(["sh", "-c", rollback], cwd=clone, check=True, capture_output=True)
                self.assertEqual(self.git(clone, "rev-parse", f"refs/tags/{name}"), ids[name])

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
    """The unit couriers the last failure after every failed run. A standing refusal is the same text on every run, so
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
        self.append(self.refusal())  # the next run refuses the same way
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

    def unwritable_log(self) -> None:
        """The log refuses every record. A read-only file does not do it: CI runs as root, which writes through the mode."""
        refuse = mock.patch.object(timer.Log, "event", side_effect=timer.Failure("cannot write the log: Permission denied"))
        refuse.start()
        self.addCleanup(refuse.stop)

    def test_a_log_that_cannot_record_the_notice_is_loud(self) -> None:
        self.write_log(self.refusal())
        self.unwritable_log()
        self.assertEqual(self.notify(), 1)
        self.assertEqual(len(self.sent), 1, "the courier went before the record failed")

    def test_a_log_that_cannot_record_the_suppression_is_loud(self) -> None:
        self.write_log(self.refusal(), self.notified())
        self.unwritable_log()
        self.assertEqual(self.notify(), 1)
        self.assertEqual(self.sent, [])


class LogLinesThatAreNotEvents(TimerCase):
    """A line of the log that is valid JSON but not an object (a stray write, a hand edit) is not an event: every reader of the
    log skips it, so it cannot crash a run or the failure notice."""

    JUNK = ["[1, 2]", '"x"', "3", "null", "true", "{"]

    def logged(self) -> list[str]:
        """The events the log holds, read the way the tool reads it (this log has lines that are not JSON on purpose)."""
        return [e["event"] for e in timer.read_events(self.log)]

    def write_mixed(self, *records: dict) -> None:
        lines = []
        for record in records:
            lines += [*self.JUNK, json.dumps(record)]
        lines += self.JUNK
        self.log.parent.mkdir(parents=True, exist_ok=True)
        self.log.write_text("\n".join(lines) + "\n")

    def test_the_failure_notice_uses_the_real_events_and_does_not_crash(self) -> None:
        self.write_mixed({"event": "failed", "tag": TAG, "step": "publish_apk", "error": "publish failed with exit 1"})
        sent: list[list[str]] = []

        def courier(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
            sent.append(argv)
            return completed(argv)

        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=courier, sleep=self.sleeps.append), 0)
        self.assertIn("publish_apk", sent[0][-1])
        self.assertIn("publish failed with exit 1", sent[0][-1])
        self.assertEqual(self.logged()[-1], "failure_notified")

    def test_a_run_reads_its_history_around_the_junk(self) -> None:
        self.write_mixed({"event": "done", "tag": TAG})
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("nothing_to_do", self.logged())

    def test_a_notice_owed_is_found_around_the_junk(self) -> None:
        self.write_mixed({"event": "step_ok", "tag": TAG, "step": "verify_live"})
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(len(couriers(fake, "teth")), 1)

    def test_read_events_returns_only_objects(self) -> None:
        self.write_mixed({"event": "x"})
        self.assertEqual(timer.read_events(self.log), [{"event": "x"}])


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


VARIANT = "q15t"
VARIANT_APK = f"nevr-runtime-{TAG}-{VARIANT}-quest.apk"
LIBRARIES = ("libq15t.so", "libq15t_early.so")
# Unique strings in the names of the private directory and the pins file: nothing a run writes or runs may carry them.
VDIR_MARK = "VARIANTDIRMARK-6f1c2a"
PINS_MARK = "PINSFILEMARK-9d4e7b"
MAIN_STEPS = ["checkout", "tags", "token", "list", "build_signer", "check_listed", "worktree", "build_slot", "configure",
              "build_apk", "publish_apk", "dispatch", "watch", "check_jobs", "verify_live", "notify"]


def library_bytes(name: str) -> bytes:
    """A synthetic library: random bytes, the same on every run."""
    return random.Random(name).randbytes(4096)


HOST_APK = b"apk bytes the host serves"


class PublicHost:
    """The release host, as the timer's own check of the variant's pair sees it. `mode` is the answer for the APK; `sidecar`
    is the answer for its `.sha256`: ok (the line sha256sum writes for the APK), absent, wrong_digest (the line of other
    bytes), other_name, html, error, or custom (the text in `sidecar_text`)."""

    def __init__(self, case: unittest.TestCase) -> None:
        self.mode = "absent"
        self.sidecar = "ok"
        self.sidecar_text = ""
        self.html_type = "text/html; HOSTCTYPE-MARK-5d2"
        self.requests: list[tuple[str, str]] = []
        self.agents: list[str | None] = []
        host = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args: object) -> None:
                pass

            def reply(self, code: int, content_type: str, body: bytes) -> None:
                self.send_response(code)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self) -> None:  # noqa: N802 - the handler's own name
                host.requests.append((self.command, self.path))
                host.agents.append(self.headers.get("User-Agent"))
                if self.path.endswith(".sha256"):
                    self.sidecar_reply()
                    return
                if host.mode == "nocontent":
                    self.send_response(204)
                    self.end_headers()
                    return
                if host.mode == "redirect":
                    self.send_response(302)
                    self.send_header("Location", "http://127.0.0.1:1/elsewhere")
                    self.end_headers()
                    return
                code, content_type = {"absent": (404, "text/plain"), "present": (200, "application/vnd.android.package-archive"),
                                      "html": (200, host.html_type), "error": (500, "text/plain"),
                                      "forbidden": (403, "text/plain")}[host.mode]
                self.reply(code, content_type, HOST_APK)

            def sidecar_reply(self) -> None:
                good = hashlib.sha256(HOST_APK).hexdigest()
                lines = {"ok": f"{good}  {VARIANT_APK}\n", "other_name": f"{good}  nevr-runtime-{TAG}-quest.apk\n",
                         "wrong_digest": f"{hashlib.sha256(b'other bytes').hexdigest()}  {VARIANT_APK}\n",
                         "custom": host.sidecar_text}
                if host.sidecar == "absent":
                    self.reply(404, "text/plain", b"")
                elif host.sidecar == "html":
                    self.reply(200, host.html_type, b"<html></html>")
                elif host.sidecar == "error":
                    self.reply(500, "text/plain", b"")
                else:
                    self.reply(200, "text/plain", lines[host.sidecar].encode())

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        case.addCleanup(self.server.server_close)
        case.addCleanup(self.server.shutdown)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"


def http_response(body: bytes, declared: int | None = None, content_type: str = "application/octet-stream",
                  extra: str = "") -> bytes:
    length = len(body) if declared is None else declared
    return (f"HTTP/1.1 200 OK\r\nContent-Type: {content_type}\r\nContent-Length: {length}\r\n{extra}"
            "Connection: close\r\n\r\n").encode() + body


def http_without_length(body: bytes) -> bytes:
    """A body that ends where the connection does: no Content-Length, no chunking."""
    return b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n" + body


def http_chunked(*chunks: bytes, finish: bool = True) -> bytes:
    body = b"".join(f"{len(c):x}\r\n".encode() + c + b"\r\n" for c in chunks) + (b"0\r\n\r\n" if finish else b"")
    return (b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n"
            b"Connection: close\r\n\r\n" + body)


class RawHost:
    """A host that writes the bytes it is given and closes: a broken chunked body, a garbage status line, a body shorter than
    its Content-Length. `apk` answers the variant APK and `sidecar` its `.sha256`."""

    def __init__(self, case: unittest.TestCase, apk: bytes, sidecar: bytes) -> None:
        self.apk, self.sidecar = apk, sidecar
        self.requests: list[bytes] = []
        host = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self) -> None:
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = self.request.recv(4096)
                    if not chunk:
                        return
                    data += chunk
                host.requests.append(data)
                path = data.split(b" ", 2)[1].decode()
                self.request.sendall(host.sidecar if path.endswith(".sha256") else host.apk)

        socketserver.ThreadingTCPServer.allow_reuse_address = True
        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        case.addCleanup(self.server.server_close)
        case.addCleanup(self.server.shutdown)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"


class VariantCase(TimerCase):
    """A timer whose environment names the variant's directory and pins file, and a public host the test controls."""

    def setUp(self) -> None:
        super().setUp()
        self.vdir = self.root / VDIR_MARK
        self.vdir.mkdir()
        self.pins = self.root / f"{PINS_MARK}.txt"
        for name in LIBRARIES:
            (self.vdir / name).write_bytes(library_bytes(name))
        self.write_pins()
        self.host = PublicHost(self)
        self.env = {**self.env, "NEVR_VARIANT_DIR": str(self.vdir), "NEVR_VARIANT_PINS": str(self.pins),
                    "RELEASE_PUBLIC_BASE": self.host.base}
        self.scratch = self.root / "work" / f"variant-{TAG}"

    @staticmethod
    def pin_line(name: str, data: bytes | None = None) -> str:
        return f"{hashlib.sha256(library_bytes(name) if data is None else data).hexdigest()}  {name}"

    def write_pins(self, *lines: str) -> None:
        self.pins.write_text("\n".join(lines or [self.pin_line(name) for name in LIBRARIES]) + "\n")

    def run_captured(self, fake: Fake, *args: str, env: dict | None = None) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = self.run_timer(fake, *args, env=env)
        return code, err.getvalue()

    def step_oks(self) -> list[str]:
        return [e["step"] for e in self.events() if e["event"] == "step_ok"]

    def failed(self) -> list[dict]:
        return [e for e in self.events() if e["event"] == "failed"]

    def assert_no_private_path(self, *texts: str) -> None:
        for text in texts:
            for private in (VDIR_MARK, PINS_MARK, str(self.vdir), str(self.pins)):
                self.assertNotIn(private, text)

    def assert_nothing_private_anywhere(self, fake: Fake, stderr: str) -> None:
        self.assert_no_private_path(self.log.read_text(), stderr, json.dumps(fake.calls, default=str))


class VariantAfterTheMainRelease(VariantCase):
    def test_a_configured_run_builds_and_publishes_the_variant_after_the_main_release_is_complete(self) -> None:
        fake = Fake(self.root)
        code, stderr = self.run_captured(fake)
        self.assertEqual((code, stderr), (0, ""))
        self.assertEqual(self.step_oks(), [*MAIN_STEPS, "variant_pins", "variant_build", "variant_publish", "cleanup"])
        self.assertEqual(self.names()[-1], "done")
        self.assertNotIn("variant_not_configured", self.names())
        steps = fake.steps()
        self.assertLess(steps.index("courier"), steps.index("variant_build"), "the main release is announced first")
        self.assertLess(max(i for i, s in enumerate(steps) if s == "verify"), steps.index("variant_build"))
        self.assertLess(steps.index("variant_build"), steps.index("variant_publish"))
        self.assertLess(steps.index("variant_publish"), steps.index("cleanup.worktree"))

    def test_the_main_events_are_the_same_with_and_without_the_variant(self) -> None:
        def main_events(case_events: list[dict]) -> list[dict]:
            return [{k: v for k, v in e.items() if k not in ("ts", "pid")} for e in case_events
                    if not str(e.get("step", "")).startswith("variant_") and not str(e["event"]).startswith("variant_")]

        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        with_variant = main_events(self.events())
        self.log.unlink()
        env = {k: v for k, v in self.env.items() if not k.startswith("NEVR_VARIANT_")}
        self.assertEqual(self.run_timer(Fake(self.root), env=env), 0)
        without = [e for e in main_events(self.events()) if e["event"] != "variant_not_configured"]
        self.assertEqual(with_variant, without)

    def test_one_record_per_library_names_the_file_its_size_and_its_pin_and_never_a_directory(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        records = [e for e in self.events() if e["event"] == "variant_library"]
        self.assertEqual([(e["name"], e["size"], e["sha256"]) for e in records],
                         [(name, 4096, hashlib.sha256(library_bytes(name)).hexdigest()) for name in LIBRARIES])
        pins_ok = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "variant_pins")
        self.assertEqual(pins_ok["count"], 2)
        self.assert_no_private_path(self.log.read_text())

    def test_the_variant_reuses_the_worktree_the_configure_and_the_build_slot(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        for once in ("worktree.add", "worktree.setup", "build_slot", "configure", "build_apk", "dispatch"):
            self.assertEqual(fake.steps().count(once), 1, once)
        build = next(c for c in fake.calls if c["step"] == "build_apk")
        variant = next(c for c in fake.calls if c["step"] == "variant_build")
        publish = next(c for c in fake.calls if c["step"] == "variant_publish")
        self.assertEqual(variant["cwd"], build["cwd"])
        self.assertEqual(publish["cwd"], build["cwd"])
        self.assertEqual(variant["argv"][:6], build["argv"][:6], "the same slice and cap")
        self.assertEqual(variant["argv"][6:9], ["just", "release-quest-variant-apk", TAG])
        self.assertEqual(variant["env"], build["env"])
        self.assertEqual(self.host.requests, [], "a tag the run just published needs no look at the host")

    def test_the_build_gets_checked_copies_in_a_private_scratch_directory_not_the_originals(self) -> None:
        seen: dict = {}

        def build(argv: list[str]) -> subprocess.CompletedProcess:
            paths = [pathlib.Path(p) for p in argv[-1].split(":")]
            seen["paths"] = paths
            seen["dir_mode"] = stat.S_IMODE(paths[0].parent.stat().st_mode)
            seen["file_modes"] = [stat.S_IMODE(p.stat().st_mode) for p in paths]
            seen["bytes"] = [p.read_bytes() for p in paths]
            return completed(argv)

        fake = Fake(self.root, overrides={"variant_build": build})
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual([p.name for p in seen["paths"]], list(LIBRARIES))
        self.assertEqual({p.parent for p in seen["paths"]}, {self.scratch})
        self.assertNotEqual(self.scratch, self.vdir)
        self.assertEqual(seen["dir_mode"], 0o700)
        self.assertEqual(seen["file_modes"], [0o600, 0o600])
        self.assertEqual(seen["bytes"], [library_bytes(name) for name in LIBRARIES])
        self.assertFalse(self.scratch.exists(), "the copies are removed by exact path when the run ends")
        self.assertTrue(all((self.vdir / name).exists() for name in LIBRARIES), "the originals are not touched")

    def test_the_variant_publish_gets_the_r2_credential_and_no_token_and_the_build_neither(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        publish = next(c for c in fake.calls if c["step"] == "variant_publish")
        self.assertEqual(publish["argv"], ["python3", "tools/publish_quest_apk.py", TAG, "--variant", VARIANT])
        for name, value in SECRETS.items():
            self.assertEqual(publish["env"][name], value)
        self.assertNotIn("GH_TOKEN", publish["env"])
        self.assertNotIn("RELEASE_SIGNER_BIN", publish["env"])
        build = next(c for c in fake.calls if c["step"] == "variant_build")
        self.assertEqual(sorted(set(SECRETS) & set(build["env"])), [])
        self.assertNotIn("GH_TOKEN", build["env"])

    def test_no_command_is_given_the_directory_or_the_pins_file(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        for call in fake.calls:
            with self.subTest(step=call["step"]):
                self.assertTrue({"NEVR_VARIANT_DIR", "NEVR_VARIANT_PINS"}.isdisjoint(call["env"] or {}))
        self.assert_no_private_path(json.dumps(fake.calls, default=str))

    def test_a_value_of_the_variant_environment_in_a_failing_commands_output_is_redacted(self) -> None:
        leak = {"variant_build": lambda argv: completed(argv, 1, f"read {self.vdir} and {self.pins}", f"in {self.vdir}")}
        fake = Fake(self.root, overrides=leak)
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1)
        output = (self.root / "work" / f"variant-{TAG}-build.log").read_text()
        self.assertIn("<redacted>", output, "the output file is redacted like everything written")
        self.assert_no_private_path(output, self.log.read_text(), stderr)

    def test_the_variant_steps_log_through_the_same_log_with_the_tag(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        for step in ("variant_pins", "variant_build", "variant_publish"):
            [record] = [e for e in self.events() if e["event"] == "step_ok" and e["step"] == step]
            self.assertEqual(record["tag"], TAG)
        publish = next(e for e in self.events() if e["event"] == "step_ok" and e["step"] == "variant_publish")
        self.assertEqual(publish["apk"], VARIANT_APK)


class VariantFailureNeverUndoesTheRelease(VariantCase):
    def assert_release_stands(self, fake: Fake) -> None:
        """The main release is published, listed, verified and announced; nothing of it was rolled back or run again."""
        self.assertEqual([s for s in self.step_oks() if s in MAIN_STEPS], MAIN_STEPS)
        self.assertEqual(fake.live_beta(), TAG[1:])
        for once in ("dispatch", "watch", "publish_apk", "build_apk"):
            self.assertEqual(fake.steps().count(once), 1, once)
        self.assertEqual(len(couriers(fake, "teth")), 1)
        self.assertEqual(len(couriers(fake, "spritz")), 1)
        self.assertIn("cleanup.worktree", fake.steps(), "the worktree is still removed")
        self.assertFalse(self.scratch.exists())

    def test_a_failing_variant_step_after_a_complete_release_exits_1_and_names_itself(self) -> None:
        for step in ("variant_build", "variant_publish"):
            with self.subTest(step=step):
                self.log.unlink(missing_ok=True)
                fake = Fake(self.root, fail=step)
                code, stderr = self.run_captured(fake)
                self.assertEqual(code, 1)
                self.assertEqual([e["step"] for e in self.failed()], [step])
                self.assertIn(f"quest-release-timer: {step}:", stderr)
                self.assertNotIn("done", self.names())
                self.assertEqual([e for e in self.events() if e["event"] == "refused"], [])
                self.assert_release_stands(fake)
                self.assert_nothing_private_anywhere(fake, stderr)

    def test_a_variant_pin_failure_after_a_complete_release_leaves_the_release_standing(self) -> None:
        (self.vdir / LIBRARIES[0]).write_bytes(library_bytes(LIBRARIES[0]) + b"x")
        fake = Fake(self.root)
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1)
        self.assertEqual([e["step"] for e in self.failed()], ["variant_pins"])
        self.assertNotIn("variant_build", fake.steps())
        self.assertNotIn("done", self.names())
        self.assert_release_stands(fake)

    def test_the_variant_failure_is_the_one_the_failure_notice_couriers(self) -> None:
        fake = Fake(self.root, fail="variant_build")
        self.assertEqual(self.run_timer(fake), 1)
        sent: list[list[str]] = []

        def courier(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
            sent.append(argv)
            return completed(argv)

        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=courier, sleep=self.sleeps.append), 0)
        self.assertIn("variant_build", sent[0][-1])
        self.assertIn(TAG, sent[0][-1])

    def test_the_next_run_builds_only_the_variant_and_then_the_tag_is_done(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, fail="variant_build")), 1)
        self.assertNotIn("done", self.names())
        fake = Fake(self.root)
        fake.beta = TAG[1:]  # the first run published the release
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(self.names()[-1], "done")
        self.assertEqual(self.host.requests, [("GET", f"/nevr-runtime/{TAG}/{VARIANT_APK}")])
        for never in ("build_apk", "publish_apk", "dispatch", "watch", "check_jobs", "courier"):
            self.assertNotIn(never, fake.steps())
        self.assertEqual(fake.steps().count("variant_build"), 1)
        self.assertEqual(fake.steps().count("variant_publish"), 1)
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertEqual(self.names()[-1], "nothing_to_do")

    def test_a_variant_failure_counts_toward_the_three_failures_and_the_skip_names_the_variant(self) -> None:
        for _ in range(3):
            self.assertEqual(self.run_timer(Fake(self.root, fail="variant_build")), 1)
        self.assertEqual([e["step"] for e in self.failed()], ["variant_build"] * 3)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("skipped_after_failures", self.names())
        [spritz] = couriers(fake, "spritz")
        self.assertIn("variant", spritz[-1])
        self.assertIn("variant_build", spritz[-1])
        self.assertIn("main release is complete", spritz[-1])
        self.assertIn(f"gave up on the variant APK of {TAG}", spritz[-1])
        self.assertNotIn("(main release)", spritz[-1])
        self.assertNotIn("variant_build", fake.steps())

    def test_a_main_skip_names_the_main_release(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(3)))
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        [spritz] = couriers(fake, "spritz")
        self.assertIn("main release", spritz[-1])
        self.assertNotIn("variant", spritz[-1])

    def test_main_failures_and_a_variant_failure_share_the_three_tries(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(2)))
        self.assertEqual(self.run_timer(Fake(self.root, fail="variant_publish")), 1)
        self.assertEqual(len(self.failed()), 3)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("skipped_after_failures", self.names())


class VariantPins(VariantCase):
    """A pin that does not hold is a loud variant failure that names the FILE, never the directory."""

    def refused_with(self, want: str, fake: Fake | None = None) -> str:
        fake = fake or Fake(self.root)
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1, stderr)
        [failure] = self.failed()
        self.assertEqual(failure["step"], "variant_pins")
        self.assertIn(want, failure["error"])
        self.assertNotIn("variant_build", fake.steps(), "nothing is built from a library that did not hold")
        self.assertNotIn("done", self.names())
        self.assertFalse(self.scratch.exists())
        self.assertEqual(fake.live_beta(), TAG[1:], "the main release is published whatever the pins say")
        self.assert_nothing_private_anywhere(fake, stderr)
        return failure["error"]

    def test_a_pin_that_differs_by_one_byte_names_the_file_and_both_digests(self) -> None:
        data = library_bytes(LIBRARIES[1])
        self.write_pins(self.pin_line(LIBRARIES[0]), self.pin_line(LIBRARIES[1], data[:-1] + bytes([data[-1] ^ 1])))
        error = self.refused_with(LIBRARIES[1])
        self.assertIn(hashlib.sha256(data).hexdigest(), error)

    def test_a_library_that_changed_after_it_was_pinned_is_refused(self) -> None:
        (self.vdir / LIBRARIES[0]).write_bytes(library_bytes(LIBRARIES[0])[:-1])
        self.refused_with(LIBRARIES[0])

    def test_a_missing_pin_line_names_the_library(self) -> None:
        self.write_pins(self.pin_line(LIBRARIES[0]))
        self.refused_with(f"no pin for {LIBRARIES[1]}")

    def test_an_extra_pin_line_is_refused_and_does_not_echo_its_name(self) -> None:
        self.write_pins(*(self.pin_line(n) for n in LIBRARIES), f"{'a' * 64}  /secret/{VDIR_MARK}/libother.so")
        error = self.refused_with("pins line 3")
        self.assertNotIn("libother", error)

    def test_a_duplicate_pin_line_is_refused(self) -> None:
        self.write_pins(*(self.pin_line(n) for n in LIBRARIES), self.pin_line(LIBRARIES[0]))
        self.refused_with(f"pins name {LIBRARIES[0]} twice")

    def test_a_malformed_pin_line_is_refused_by_its_line_number(self) -> None:
        for bad in ("not a pin", f"{'a' * 63}  {LIBRARIES[1]}", f"{'a' * 64} {LIBRARIES[1]}", f"{'g' * 64}  {LIBRARIES[1]}", ""):
            with self.subTest(line=bad):
                self.log.unlink(missing_ok=True)
                self.write_pins(self.pin_line(LIBRARIES[0]), bad)
                self.refused_with("pins line 2")

    def test_a_missing_library_file_names_it(self) -> None:
        (self.vdir / LIBRARIES[1]).unlink()
        self.refused_with(f"{LIBRARIES[1]} is missing")

    def test_a_library_that_cannot_be_read_is_named_without_the_path_the_system_reports(self) -> None:
        (self.vdir / LIBRARIES[0]).unlink()
        (self.vdir / LIBRARIES[0]).mkdir()  # reading a directory fails with an error that names the path
        self.refused_with(f"{LIBRARIES[0]} cannot be read")

    def test_an_empty_pins_file_and_an_unreadable_one_are_refused(self) -> None:
        self.pins.write_text("")
        self.refused_with(f"no pin for {LIBRARIES[0]}")
        self.log.unlink()
        self.pins.unlink()
        self.refused_with("the pins file cannot be read")

    def test_one_of_the_two_variables_alone_is_a_failure_that_names_the_variable(self) -> None:
        for unset, other in (("NEVR_VARIANT_PINS", "NEVR_VARIANT_DIR"), ("NEVR_VARIANT_DIR", "NEVR_VARIANT_PINS")):
            with self.subTest(unset=unset):
                self.log.unlink(missing_ok=True)
                env = {k: v for k, v in self.env.items() if k != unset}
                fake = Fake(self.root)
                code, stderr = self.run_captured(fake, env=env)
                self.assertEqual(code, 1)
                [failure] = self.failed()
                self.assertEqual(failure["step"], "variant_pins")
                self.assertIn(unset, failure["error"])
                self.assertNotIn("variant_build", fake.steps())
                self.assert_nothing_private_anywhere(fake, stderr)
                self.assertNotIn("variant_not_configured", self.names())

    def test_an_empty_value_is_unset(self) -> None:
        env = {**self.env, "NEVR_VARIANT_DIR": "", "NEVR_VARIANT_PINS": ""}
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, env=env), 0)
        self.assertIn("variant_not_configured", self.names())

    def test_pins_in_upper_case_hex_are_the_same_pins(self) -> None:
        self.write_pins(*(self.pin_line(n).upper().replace(n.upper(), n) for n in LIBRARIES))
        self.assertEqual(self.run_timer(Fake(self.root)), 0)

    def variant_pins_directly(self) -> timer.Timer:
        log = timer.Log(self.log, TAG)
        self.addCleanup(log.close)
        subject = timer.Timer(self.env, Fake(self.root), self.sleeps.append, log)
        subject.work.mkdir(parents=True, exist_ok=True)
        return subject

    def test_a_restrictive_umask_leaves_the_scratch_usable_and_the_modes_exact(self) -> None:
        subject = self.variant_pins_directly()
        old = os.umask(0o277)
        try:
            copies = subject.variant_pins(TAG, self.vdir, self.pins)
        finally:
            os.umask(old)
        self.assertEqual(stat.S_IMODE(self.scratch.stat().st_mode), 0o700)
        self.assertEqual([stat.S_IMODE(c.stat().st_mode) for c in copies], [0o600, 0o600])

    def test_a_library_changed_after_it_was_copied_is_neither_checked_nor_packaged(self) -> None:
        subject = self.variant_pins_directly()
        real, original = shutil.copyfileobj, self.vdir / LIBRARIES[0]

        def copy_then_change(source, target, *args):  # noqa: ANN001, ANN002 - shutil.copyfileobj's own signature
            real(source, target, *args)
            original.write_bytes(b"changed after the copy was made")

        with mock.patch.object(timer.shutil, "copyfileobj", side_effect=copy_then_change):
            copies = subject.variant_pins(TAG, self.vdir, self.pins)
        self.assertEqual([c.parent for c in copies], [self.scratch, self.scratch])
        self.assertEqual(copies[0].read_bytes(), library_bytes(LIBRARIES[0]), "the copy is what was pinned")
        self.assertNotEqual(original.read_bytes(), library_bytes(LIBRARIES[0]))

    def test_a_scratch_directory_left_by_a_killed_run_of_this_timer_is_replaced(self) -> None:
        self.scratch.mkdir(parents=True)
        (self.scratch / timer.SCRATCH_MARKER).write_text("")
        (self.scratch / "stale.so").write_bytes(b"left over")
        (self.scratch / LIBRARIES[0]).write_bytes(b"left over")
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertFalse(self.scratch.exists())

    def test_a_directory_of_that_name_without_the_marker_is_never_removed_and_the_pins_step_says_so(self) -> None:
        self.scratch.mkdir(parents=True)
        (self.scratch / "not-ours.txt").write_text("kept")
        fake = Fake(self.root)
        code, _ = self.run_captured(fake)
        self.assertEqual(code, 1)
        self.assertEqual([e["step"] for e in self.failed()], ["variant_pins"])
        self.assertIn("cannot make the scratch directory", self.failed()[0]["error"])
        self.assertEqual((self.scratch / "not-ours.txt").read_text(), "kept")
        self.assertNotIn("variant_build", fake.steps())

    def test_the_scratch_directory_carries_the_marker_while_the_build_runs(self) -> None:
        seen: dict = {}

        def build(argv: list[str]) -> subprocess.CompletedProcess:
            seen["marker"] = (self.scratch / timer.SCRATCH_MARKER).is_file()
            seen["files"] = sorted(path.name for path in self.scratch.iterdir())
            return completed(argv)

        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": build})), 0)
        self.assertTrue(seen["marker"])
        self.assertEqual(seen["files"], sorted([timer.SCRATCH_MARKER, *LIBRARIES]))

    def test_a_scratch_path_with_a_colon_is_refused_because_the_recipe_splits_on_it(self) -> None:
        env = {**self.env, "QUEST_RELEASE_WORK_ROOT": str(self.root / "wo:rk")}
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, env=env), 1)
        self.assertEqual(self.failed()[0]["step"], "variant_pins")
        self.assertNotIn("variant_build", fake.steps())


class VariantOfAListedRelease(VariantCase):
    """The main release is already listed (this run published nothing): the host decides whether the variant is built."""

    def listed(self) -> Fake:
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        return fake

    def test_an_absent_variant_is_built_and_published_alone(self) -> None:
        fake = self.listed()
        code, stderr = self.run_captured(fake)
        self.assertEqual((code, stderr), (0, ""))
        self.assertEqual(self.step_oks(), ["checkout", "tags", "token", "list", "build_signer", "check_listed", "variant_exists",
                                           "variant_pins", "worktree", "build_slot", "configure", "variant_build", "variant_publish", "cleanup"])
        self.assertEqual(self.host.requests, [("GET", f"/nevr-runtime/{TAG}/{VARIANT_APK}")])
        self.assertEqual(self.host.agents, [timer.USER_AGENT], "the release host answers 403 to the default agent")
        [asked] = [e for e in self.events() if e["event"] == "step_ok" and e["step"] == "variant_exists"]
        self.assertEqual((asked["listed"], asked["apk"]), (False, VARIANT_APK))
        for never in ("dispatch", "watch", "check_jobs", "publish_apk", "build_apk", "courier"):
            self.assertNotIn(never, fake.steps())
        self.assertEqual(fake.steps().count("worktree.add"), 1)
        self.assertEqual(fake.steps().count("cleanup.worktree"), 1)
        added = next(c for c in fake.calls if c["step"] == "worktree.add")
        removed = next(c for c in fake.calls if c["step"] == "cleanup.worktree")
        self.assertEqual(removed["argv"][-1], added["argv"][-2], "removed by exact path")
        self.assertEqual(next(c for c in fake.calls if c["step"] == "variant_build")["cwd"], added["argv"][-2])
        self.assertEqual(self.names()[-1], "done")
        self.assertIn("already_listed", self.names())
        self.assert_nothing_private_anywhere(fake, stderr)
        self.assertFalse(self.scratch.exists())

    def test_the_second_manifest_publish_never_happens(self) -> None:
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        self.assertNotIn("dispatch", fake.steps())
        self.assertNotIn("publish_apk", fake.steps())

    def test_a_variant_already_on_the_host_builds_nothing_and_the_tag_is_done(self) -> None:
        self.host.mode = "present"
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(self.step_oks(), ["checkout", "tags", "token", "list", "build_signer", "check_listed", "variant_exists",
                                           "cleanup"])
        [asked] = [e for e in self.events() if e["event"] == "step_ok" and e["step"] == "variant_exists"]
        self.assertEqual((asked["listed"], asked["apk"]), (True, VARIANT_APK))
        [listed] = [e for e in self.events() if e["event"] == "variant_listed"]
        self.assertEqual(listed["apk"], VARIANT_APK)
        self.assertEqual(self.host.requests, [("GET", f"/nevr-runtime/{TAG}/{VARIANT_APK}"),
                                              ("GET", f"/nevr-runtime/{TAG}/{VARIANT_APK}.sha256")], "the pair is checked")
        self.assertNotIn("worktree.add", fake.steps())
        self.assertNotIn("variant_build", fake.steps())
        self.assertNotIn("variant_library", self.names())
        self.assertEqual(self.names()[-1], "done")

    def test_a_pin_that_does_not_hold_matters_only_when_there_is_something_to_build(self) -> None:
        self.host.mode = "present"
        (self.vdir / LIBRARIES[0]).unlink()
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.host.mode = "absent"
        self.log.unlink()
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(self.failed()[0]["step"], "variant_pins")
        self.assertNotIn("worktree.add", fake.steps(), "no worktree is made for a variant that cannot be built")

    def test_other_answers_are_failures_that_build_nothing_and_never_done(self) -> None:
        for mode, want in (("html", "HTML"), ("error", "500"), ("forbidden", "403"), ("redirect", "302"),
                           ("nocontent", "204")):
            with self.subTest(mode=mode):
                self.log.unlink(missing_ok=True)
                self.host.mode = mode
                fake = self.listed()
                code, stderr = self.run_captured(fake)
                self.assertEqual(code, 1)
                [failure] = self.failed()
                self.assertEqual(failure["step"], "variant_exists")
                self.assertIn(want, failure["error"])
                self.assertNotIn("HOSTCTYPE-MARK", failure["error"], "a header of the host is not copied into a failure")
                self.assertIn(VARIANT_APK, failure["error"])
                self.assertNotIn("worktree.add", fake.steps())
                self.assertNotIn("done", self.names())
                self.assert_nothing_private_anywhere(fake, stderr)

    def test_a_host_that_cannot_be_reached_is_a_failure(self) -> None:
        env = {**self.env, "RELEASE_PUBLIC_BASE": "http://127.0.0.1:1"}
        fake = self.listed()
        self.assertEqual(self.run_timer(fake, env=env), 1)
        self.assertEqual(self.failed()[0]["step"], "variant_exists")
        self.assertNotIn("worktree.add", fake.steps())

    def test_a_listed_release_whose_notice_is_owed_is_noticed_before_the_variant(self) -> None:
        self.write_log({"event": "step_ok", "tag": TAG, "step": "verify_live"})
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        steps = fake.steps()
        self.assertEqual(len(couriers(fake, "teth")), 1)
        self.assertLess(steps.index("courier"), steps.index("variant_build"))

    def test_a_failing_variant_build_of_a_listed_release_is_a_counted_failure_and_cleans_up(self) -> None:
        fake = self.listed()
        fake.fail = "variant_build"
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([e["step"] for e in self.failed()], ["variant_build"])
        self.assertIn("cleanup.worktree", fake.steps())
        self.assertNotIn("done", self.names())
        self.assertFalse(self.scratch.exists())


class VariantPairCheck(VariantCase):
    """The tag is listed and the host has the variant APK: it counts as listed only when its `.sha256` is there and says
    the digest of the very bytes the host serves. A half-published pair or a foreign APK is a loud failure, and nothing
    is rebuilt (an upload never replaces an object, so a rebuild could not repair it)."""

    def listed(self) -> Fake:
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        return fake

    def refused_with(self, *wants: str, repair: bool = True) -> dict:
        fake = self.listed()
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1, stderr)
        [failure] = self.failed()
        self.assertEqual(failure["step"], "variant_exists")
        for want in wants:
            self.assertIn(want, failure["error"])
        self.assertIn(VARIANT_APK, failure["error"])
        if repair:
            self.assertIn("RELEASING.md", failure["error"], "the message points at the repair")
        for never in ("worktree.add", "variant_build", "variant_publish", "configure"):
            self.assertNotIn(never, fake.steps(), "nothing is rebuilt or republished")
        self.assertNotIn("done", self.names())
        self.assertNotIn("variant_listed", self.names())
        self.assert_nothing_private_anywhere(fake, stderr)
        return failure

    def test_a_pair_that_agrees_is_listed_and_nothing_is_built(self) -> None:
        self.host.mode = "present"
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("variant_listed", self.names())
        self.assertEqual(self.names()[-1], "done")
        self.assertNotIn("variant_build", fake.steps())

    def test_an_apk_with_no_sidecar_is_a_half_published_pair_and_a_failure(self) -> None:
        self.host.mode, self.host.sidecar = "present", "absent"
        self.refused_with(f"{VARIANT_APK}.sha256", "missing")

    def test_an_apk_whose_sidecar_is_the_digest_of_other_bytes_is_a_failure_naming_both_digests(self) -> None:
        self.host.mode, self.host.sidecar = "present", "wrong_digest"
        failure = self.refused_with(f"{VARIANT_APK}.sha256", "sha256")
        self.assertIn(hashlib.sha256(HOST_APK).hexdigest(), failure["error"], "the digest of the bytes the host serves")
        self.assertIn(hashlib.sha256(b"other bytes").hexdigest(), failure["error"], "the digest the sidecar states")

    def test_a_sidecar_that_names_another_file_is_a_failure(self) -> None:
        self.host.mode, self.host.sidecar = "present", "other_name"
        self.refused_with(f"{VARIANT_APK}.sha256", "names")

    def test_a_sidecar_that_names_a_file_does_not_copy_that_text_into_the_failure(self) -> None:
        good = hashlib.sha256(HOST_APK).hexdigest()
        self.host.mode, self.host.sidecar = "present", "custom"
        self.host.sidecar_text = f"{good}  HOSTNAME-MARK-xyz{'a' * 3000}\n"
        failure = self.refused_with(f"{VARIANT_APK}.sha256", "names a different file (3017 bytes)")
        self.assertNotIn("HOSTNAME-MARK", failure["error"])
        self.assertLess(len(failure["error"]), 700)

    def test_a_malformed_sidecar_is_a_failure(self) -> None:
        good = hashlib.sha256(HOST_APK).hexdigest()
        for text in ("", "not a sidecar\n", f"{good} {VARIANT_APK}\n", f"{good}  {VARIANT_APK}\n{good}  {VARIANT_APK}\n",
                     f"{good[:-1]}  {VARIANT_APK}\n", f"{good.upper()}  {VARIANT_APK}\n", f"{good}  {VARIANT_APK} extra\n",
                     "ÿþ not utf-8 \n"):
            with self.subTest(text=text):
                self.log.unlink(missing_ok=True)
                self.host.mode, self.host.sidecar, self.host.sidecar_text = "present", "custom", text
                self.refused_with(f"{VARIANT_APK}.sha256", "malformed")

    def test_a_sidecar_answered_with_html_or_a_server_error_is_a_failure(self) -> None:
        for sidecar, want in (("html", "HTML"), ("error", "500")):
            with self.subTest(sidecar=sidecar):
                self.log.unlink(missing_ok=True)
                self.host.mode, self.host.sidecar = "present", sidecar
                self.refused_with(f"{VARIANT_APK}.sha256", want)
                self.assertNotIn("HOSTCTYPE-MARK", self.failed()[0]["error"])

    def test_an_absent_apk_is_built_whatever_the_sidecar_is(self) -> None:
        self.host.mode, self.host.sidecar = "absent", "absent"
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(self.host.requests, [("GET", f"/nevr-runtime/{TAG}/{VARIANT_APK}")], "the sidecar is not asked for")
        self.assertIn("variant_build", fake.steps())

    def test_the_apk_is_hashed_as_it_streams_in_pieces(self) -> None:
        big = random.Random("big").randbytes(3 * 1024 * 1024 + 17)
        with mock.patch.object(sys.modules[__name__], "HOST_APK", big):
            self.host.mode = "present"
            fake = self.listed()
            self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("variant_listed", self.names())

    def test_an_apk_or_a_sidecar_over_its_limit_is_a_failure(self) -> None:
        self.host.mode = "present"
        with mock.patch.object(timer, "APK_LIMIT", len(HOST_APK) - 1):
            self.refused_with("more than the expected", repair=False)
        self.log.unlink()
        self.host.sidecar, self.host.sidecar_text = "custom", "a" * 5000
        with mock.patch.object(timer, "SIDECAR_LIMIT", 4096):
            self.refused_with("more than the expected", f"{VARIANT_APK}.sha256", repair=False)

    def test_the_failure_is_counted_and_a_standing_half_published_pair_stops_after_three(self) -> None:
        self.host.mode, self.host.sidecar = "present", "absent"
        for _ in range(3):
            self.assertEqual(self.run_timer(self.listed()), 1)
        self.assertEqual([e["step"] for e in self.failed()], ["variant_exists"] * 3)
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        [spritz] = couriers(fake, "spritz")
        self.assertIn("variant_exists", spritz[-1])
        self.assertIn(f"gave up on the variant APK of {TAG}", spritz[-1])


class VariantBuildOutput(VariantCase):
    """A failing variant step puts no output of the build in the record, stderr, journal or courier text: the plugin check
    prints the plugin's own symbol names. The full output is in a 0600 file in the timer's work directory."""

    LOG_NAME = f"variant-{TAG}-build.log"

    def build_log(self) -> pathlib.Path:
        return self.root / "work" / self.LOG_NAME

    def noisy(self, out: str = "OUTMARK-7c1e", err: str = "ERRMARK-3b9d", rc: int = 1):
        return lambda argv: completed(argv, rc, out, err)

    def couriered(self) -> str:
        sent: list[list[str]] = []

        def courier(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
            sent.append(argv)
            return completed(argv)

        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=courier, sleep=self.sleeps.append), 0)
        return " ".join(" ".join(argv) for argv in sent)

    def test_the_output_is_in_the_0600_file_and_nowhere_else(self) -> None:
        old = os.umask(0)  # a file opened with the default mode would be 0666 here
        self.addCleanup(os.umask, old)
        fake = Fake(self.root, overrides={"variant_build": self.noisy()})
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1)
        text = self.build_log().read_text()
        self.assertIn("OUTMARK-7c1e", text)
        self.assertIn("ERRMARK-3b9d", text)
        self.assertEqual(stat.S_IMODE(self.build_log().stat().st_mode), 0o600)
        self.assertEqual(self.build_log().parent, self.root / "work", "in the work directory, not the private one")
        for where in (self.log.read_text(), stderr, self.couriered(), json.dumps(fake.calls, default=str)):
            self.assertNotIn("OUTMARK-7c1e", where)
            self.assertNotIn("ERRMARK-3b9d", where)

    def test_the_failure_names_the_step_the_program_the_exit_code_and_the_file(self) -> None:
        fake = Fake(self.root, overrides={"variant_build": self.noisy(rc=3)})
        self.assertEqual(self.run_timer(fake), 1)
        [failure] = self.failed()
        self.assertEqual(failure["step"], "variant_build")
        self.assertEqual(failure["error"], f"systemd-run exited 3; its full output is in {self.build_log()}")

    def test_the_file_is_kept_on_failure_and_removed_when_the_variant_is_done(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.noisy()})), 1)
        self.assertTrue(self.build_log().exists())
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(self.names()[-1], "done")
        self.assertFalse(self.build_log().exists())

    def test_the_file_of_an_earlier_failure_is_removed_when_the_variant_turns_out_to_be_listed(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.noisy()})), 1)
        self.host.mode = "present"
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertFalse(self.build_log().exists())

    def test_a_successful_build_leaves_no_file_once_the_variant_is_published(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.noisy(rc=0)})), 0)
        self.assertFalse(self.build_log().exists())

    def test_a_rerun_replaces_the_file_it_does_not_append(self) -> None:
        for marker in ("FIRST-RUN-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "SECOND-RUN-BBB"):
            self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.noisy(err=marker)})), 1)
        text = self.build_log().read_text()
        self.assertIn("SECOND-RUN-BBB", text)
        self.assertNotIn("FIRST-RUN-AAA", text)
        self.assertEqual(text.count("--- stdout ---"), 1, "nothing of the longer first file is left behind")
        self.assertEqual(text, "exit 1\n--- stderr ---\nSECOND-RUN-BBB\n--- stdout ---\nOUTMARK-7c1e\n")

    def test_a_link_planted_at_the_path_is_replaced_never_followed(self) -> None:
        victim = self.root / "victim.txt"
        victim.write_text("precious")
        self.build_log().parent.mkdir(parents=True, exist_ok=True)
        self.build_log().symlink_to(victim)
        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.noisy()})), 1)
        self.assertEqual(victim.read_text(), "precious")
        self.assertFalse(self.build_log().is_symlink())
        self.assertIn("OUTMARK-7c1e", self.build_log().read_text())

    def test_a_link_planted_between_the_removal_and_the_open_is_not_followed_either(self) -> None:
        victim = self.root / "victim.txt"
        victim.write_text("precious")
        real = os.unlink
        log = self.build_log()

        def unlink_then_plant(path, *args, **kwargs):  # noqa: ANN001, ANN002, ANN003 - os.unlink's own signature
            real(path, *args, **kwargs)
            if pathlib.Path(path) == log:
                log.symlink_to(victim)

        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("an earlier output")
        with mock.patch.object(timer.os, "unlink", side_effect=unlink_then_plant):
            code, _ = self.run_captured(Fake(self.root, overrides={"variant_build": self.noisy()}))
        self.assertEqual(code, 1)
        self.assertEqual(victim.read_text(), "precious", "nothing was written through the planted link")
        self.assertEqual([e["step"] for e in self.failed()], ["variant_build"])
        self.assertIn("cannot write the output file", self.failed()[0]["error"])

    def test_a_strict_umask_still_leaves_the_file_0600(self) -> None:
        old = os.umask(0o022)
        self.addCleanup(os.umask, old)

        def strict_build(argv: list[str]) -> subprocess.CompletedProcess:
            os.umask(0o277)  # in force when the timer opens its output file, which is right after this returns
            return completed(argv, 1, "OUTMARK-7c1e", "ERRMARK-3b9d")

        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": strict_build})), 1)
        os.umask(0o022)
        self.assertEqual(stat.S_IMODE(self.build_log().stat().st_mode), 0o600)

    def test_a_file_left_with_a_loose_mode_is_0600_again(self) -> None:
        self.build_log().parent.mkdir(parents=True, exist_ok=True)
        self.build_log().write_text("left by something else")
        self.build_log().chmod(0o644)
        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.noisy()})), 1)
        self.assertEqual(stat.S_IMODE(self.build_log().stat().st_mode), 0o600)

    def test_a_secret_in_the_build_output_is_redacted_in_the_file_too(self) -> None:
        secret = SECRETS["R2_SECRET_ACCESS_KEY"]
        self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.noisy(err=f"key {secret}")})), 1)
        self.assertNotIn(secret, self.build_log().read_text())

    def test_a_failing_variant_publish_carries_only_the_program_and_the_exit_code(self) -> None:
        fake = Fake(self.root, overrides={"variant_publish": self.noisy(out="PUBOUT-1", err="PUBERR-2", rc=4)})
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1)
        [failure] = self.failed()
        self.assertEqual((failure["step"], failure["error"]), ("variant_publish", "python3 exited 4"))
        for marker in ("PUBOUT-1", "PUBERR-2"):
            self.assertNotIn(marker, self.log.read_text() + stderr + self.couriered())

    def test_the_other_steps_still_carry_their_tails(self) -> None:
        fake = Fake(self.root, overrides={"build_apk": lambda argv: completed(argv, 1, "MAINOUT", "MAINERR")})
        self.assertEqual(self.run_timer(fake), 1)
        self.assertIn("stderr: MAINERR | stdout: MAINOUT", self.failed()[0]["error"])


class VariantPartOfTheRelease(VariantCase):
    """Every failed or refused record says whether the main release or the variant was running."""

    def listed(self) -> Fake:
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        return fake

    def test_a_failure_before_the_release_and_inside_it_is_main(self) -> None:
        for step in ("checkout", "build_apk", "publish_apk", "dispatch"):
            with self.subTest(step=step):
                self.log.unlink(missing_ok=True)
                self.assertEqual(self.run_timer(Fake(self.root, fail=step)), 1)
                self.assertEqual([e["part"] for e in self.failed()], ["main"])

    def test_a_refusal_is_main(self) -> None:
        fake = Fake(self.root)
        fake.dirty = " M x\n"
        self.assertEqual(self.run_timer(fake), 1)
        [refusal] = [e for e in self.events() if e["event"] == "refused"]
        self.assertEqual(refusal["part"], "main")
        env = {k: v for k, v in self.env.items() if k != "R2_BUCKET"}
        self.log.unlink()
        self.assertEqual(self.run_timer(Fake(self.root), env=env), 1)
        self.assertEqual([e["part"] for e in self.events() if e["event"] == "refused"], ["main"])

    def test_a_failure_after_the_notice_is_the_variant(self) -> None:
        for step in ("variant_pins", "variant_build", "variant_publish"):
            with self.subTest(step=step):
                self.log.unlink(missing_ok=True)
                if step == "variant_pins":
                    (self.vdir / LIBRARIES[0]).write_bytes(b"changed")
                    fake = Fake(self.root)
                else:
                    fake = Fake(self.root, fail=step)
                self.assertEqual(self.run_timer(fake), 1)
                self.assertEqual([e["part"] for e in self.failed()], ["variant"])
                (self.vdir / LIBRARIES[0]).write_bytes(library_bytes(LIBRARIES[0]))

    def test_a_failing_notice_is_still_the_main_release(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, fail="courier")), 1)
        self.assertEqual([(e["step"], e["part"]) for e in self.failed()], [("notify", "main")])

    def test_everything_in_the_already_listed_path_is_the_variant(self) -> None:
        for step in ("configure", "worktree", "build_slot", "variant_exists", "variant_pins"):
            with self.subTest(step=step):
                self.log.unlink(missing_ok=True)
                self.host.mode = "absent"
                if step == "variant_exists":
                    self.host.mode = "error"
                if step == "variant_pins":
                    (self.vdir / LIBRARIES[0]).write_bytes(b"changed")
                fake = self.listed()
                fake.fail = step if step not in ("variant_exists", "variant_pins") else ""
                if step == "build_slot":
                    fake.ninja = [2]
                self.assertEqual(self.run_timer(fake), 1)
                self.assertEqual([(e["step"], e["part"]) for e in self.failed()], [(step, "variant")])
                (self.vdir / LIBRARIES[0]).write_bytes(library_bytes(LIBRARIES[0]))

    def test_a_failing_notice_owed_in_the_already_listed_path_is_the_main_release(self) -> None:
        self.write_log({"event": "step_ok", "tag": TAG, "step": "verify_live"})
        fake = self.listed()
        fake.fail = "courier"
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([(e["step"], e["part"]) for e in self.failed()], [("notify", "main")])
        self.assertNotIn("main_complete", self.names())

    def test_the_give_up_note_after_a_configure_failure_in_the_already_listed_path_names_the_variant(self) -> None:
        for _ in range(3):
            self.assertEqual(self.run_timer(self.listed_failing("configure")), 1)
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        [spritz] = couriers(fake, "spritz")
        self.assertIn(f"gave up on the variant APK of {TAG}", spritz[-1])
        self.assertIn("step configure", spritz[-1])
        self.assertNotIn("(main release)", spritz[-1])

    def listed_failing(self, step: str) -> Fake:
        fake = self.listed()
        fake.fail = step
        return fake

    def test_the_give_up_note_reads_the_part_of_the_last_failure_and_falls_back_for_old_records(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "configure", "part": "variant", "error": "x"} for _ in range(3)))
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("variant APK", couriers(fake, "spritz")[0][-1])
        self.log.unlink()
        self.write_log(*({"event": "failed", "tag": TAG, "step": "configure", "part": "main", "error": "x"} for _ in range(3)))
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("(main release)", couriers(fake, "spritz")[0][-1])
        self.log.unlink()
        self.write_log(*({"event": "failed", "tag": TAG, "step": "variant_build", "error": "x"} for _ in range(3)))
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("variant APK", couriers(fake, "spritz")[0][-1], "a record from before `part` is read by its step")


class MainComplete(VariantCase):
    """`main_complete` is written once the main release's notice has gone out. A run whose tag has it starts as the variant,
    so a failure of build_signer or check_listed on a variant-only retry is the variant's."""

    def listed(self) -> Fake:
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        return fake

    def test_it_is_written_after_the_notice_and_before_any_variant_record(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        names = self.names()
        self.assertEqual(names.count("main_complete"), 1)
        notified = max(i for i, e in enumerate(self.events()) if e["event"] == "step_ok" and e["step"] == "notify")
        self.assertLess(notified, names.index("main_complete"))
        self.assertLess(names.index("main_complete"), self.events().index(next(
            e for e in self.events() if e["event"] == "step_ok" and e["step"] == "variant_pins")))

    def test_it_is_not_written_when_the_notice_fails(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, fail="courier")), 1)
        self.assertNotIn("main_complete", self.names())

    def test_it_is_written_even_when_the_variant_is_not_configured(self) -> None:
        env = {k: v for k, v in self.env.items() if not k.startswith("NEVR_VARIANT_")}
        self.assertEqual(self.run_timer(Fake(self.root), env=env), 0)
        self.assertEqual(self.names().count("main_complete"), 1)

    def test_a_variant_only_retry_runs_neither_the_signer_nor_check_listed(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, fail="variant_build")), 1)
        fake = self.listed()
        fake.fail = "build_signer"  # would fail the run if the retry still built the signer
        self.assertEqual(self.run_timer(fake), 0)
        for never in ("build_signer.fetch", "build_signer.go", "verify", "manifest"):
            self.assertNotIn(never, fake.steps())

    def test_the_main_complete_of_another_tag_changes_nothing(self) -> None:
        self.write_log({"event": "main_complete", "tag": "v5.16.1"})
        fake = self.listed()
        fake.fail = "build_signer"
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual(self.failed()[-1]["part"], "main")
        self.log.unlink()
        self.write_log({"event": "main_complete", "tag": "v5.16.1"})
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.assertEqual([e["tag"] for e in self.events() if e["event"] == "main_complete"], ["v5.16.1", TAG])

    def test_a_tag_without_it_still_fails_as_the_main_release_before_check_listed_says_listed(self) -> None:
        for step in ("build_signer", "verify"):
            with self.subTest(step=step):
                self.log.unlink(missing_ok=True)
                fake = self.listed()
                fake.fail = step
                self.assertEqual(self.run_timer(fake), 1)
                self.assertEqual(self.failed()[-1]["part"], "main", step)

    def test_the_give_up_note_after_three_failures_of_a_variant_retry_names_the_variant(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, fail="variant_build")), 1)
        for _ in range(2):
            fake = self.listed()
            fake.fail = "configure"
            self.assertEqual(self.run_timer(fake), 1)
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        [spritz] = couriers(fake, "spritz")
        self.assertIn(f"gave up on the variant APK of {TAG}", spritz[-1])

    def test_a_listed_tag_with_no_notice_owed_writes_it_once_and_the_next_run_does_not_repeat_it(self) -> None:
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.assertEqual(self.names().count("main_complete"), 1)
        self.log_without = [e for e in self.events() if e["event"] != "done"]
        self.write_log(*self.log_without)
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.assertEqual(self.names().count("main_complete"), 1)

    def test_an_owed_notice_that_goes_out_on_a_listed_tag_is_followed_by_it(self) -> None:
        self.write_log({"event": "step_ok", "tag": TAG, "step": "verify_live"})
        fake = self.listed()
        self.assertEqual(self.run_timer(fake), 0)
        notified = next(i for i, e in enumerate(self.events()) if e["event"] == "step_ok" and e["step"] == "notify")
        self.assertLess(notified, self.names().index("main_complete"))

    def test_the_variant_failure_after_the_notice_of_an_owed_listed_tag_is_the_variant(self) -> None:
        self.write_log({"event": "step_ok", "tag": TAG, "step": "verify_live"})
        fake = self.listed()
        fake.fail = "configure"
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([(e["step"], e["part"]) for e in self.failed()], [("configure", "variant")])


class VariantOnlyAfterTheMainRelease(VariantCase):
    """A tag whose history has `main_complete` is main-done whatever beta lists: the variant does not depend on beta, and a
    beta that moved (a rollback, a hand publish, a hand `--tag` of an older release) must not make the timer publish the
    main release again."""

    MAIN_ONLY = ("build_signer.fetch", "build_signer.go", "verify", "manifest", "build_apk", "publish_apk",
                 "dispatch", "watch", "check_jobs", "courier")

    def moved(self) -> Fake:
        fake = Fake(self.root)
        fake.beta = "5.16.0"  # beta lists another release
        return fake

    def test_a_moved_beta_runs_only_the_variant_steps(self) -> None:
        self.write_log({"event": "main_complete", "tag": TAG})
        fake = self.moved()
        code, stderr = self.run_captured(fake)
        self.assertEqual((code, stderr), (0, ""))
        for never in self.MAIN_ONLY:
            self.assertNotIn(never, fake.steps(), never)
        self.assertEqual(self.step_oks(), ["checkout", "tags", "token", "list", "variant_exists", "variant_pins", "worktree", "build_slot",
                                           "configure", "variant_build", "variant_publish", "cleanup"])
        self.assertEqual(self.names()[-1], "done")
        self.assertEqual(self.names().count("main_complete"), 1, "written once")

    def test_a_moved_beta_with_the_variant_already_on_the_host_finishes_the_tag(self) -> None:
        self.write_log({"event": "main_complete", "tag": TAG})
        self.host.mode = "present"
        fake = self.moved()
        self.assertEqual(self.run_timer(fake), 0)
        for never in (*self.MAIN_ONLY, "worktree.add", "variant_build"):
            self.assertNotIn(never, fake.steps(), never)
        self.assertIn("variant_listed", self.names())
        self.assertEqual(self.names()[-1], "done")

    def test_a_hand_run_of_an_older_main_complete_tag_does_not_republish_it(self) -> None:
        self.write_log({"event": "main_complete", "tag": "v5.16.1"})
        fake = self.moved()
        fake.beta = TAG[1:]  # beta lists the newer release; the hand run is for the older one
        self.assertEqual(self.run_timer(fake, "--tag", "v5.16.1"), 0)
        for never in self.MAIN_ONLY:
            self.assertNotIn(never, fake.steps(), never)

    def test_failures_in_the_variant_only_path_are_the_variants_and_counted(self) -> None:
        self.write_log({"event": "main_complete", "tag": TAG})
        fake = self.moved()
        fake.fail = "variant_build"
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([(e["step"], e["part"]) for e in self.failed()], [("variant_build", "variant")])

    def test_a_tag_without_it_and_a_moved_beta_still_takes_the_full_path(self) -> None:
        fake = self.moved()
        self.assertEqual(self.run_timer(fake), 0)
        for once in ("build_apk", "publish_apk", "dispatch"):
            self.assertEqual(fake.steps().count(once), 1, once)


class RunLock(TimerCase):
    """One run at a time: a run that does work holds a non-blocking lock on <work>/timer.lock; another that finds it held
    says so and does nothing."""

    def hold(self) -> int:
        (self.root / "work").mkdir(parents=True, exist_ok=True)
        fd = os.open(self.root / "work" / "timer.lock", os.O_RDWR | os.O_CREAT, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.addCleanup(os.close, fd)
        return fd

    def test_a_run_that_finds_the_lock_held_does_nothing_and_says_so(self) -> None:
        self.hold()
        scratch = self.root / "work" / f"variant-{TAG}"
        scratch.mkdir()
        (scratch / timer.SCRATCH_MARKER).write_text("")
        fake = Fake(self.root)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual(fake.calls, [], "a busy run runs no command")
        [busy] = [e for e in self.events() if e["event"] == "busy"]
        self.assertIn("lock", busy["reason"])
        self.assertEqual(len(err.getvalue().splitlines()), 1)
        self.assertIn("busy", err.getvalue())
        self.assertTrue(scratch.exists(), "the live run's scratch is not swept by a run that did not get the lock")
        self.assertNotIn("done", self.names())

    def test_the_notice_and_the_reset_do_not_wait_for_the_lock(self) -> None:
        self.hold()
        self.write_log({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"})
        sent: list[list[str]] = []

        def courier(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
            sent.append(argv)
            return completed(argv)

        self.assertEqual(timer.main(["--notify-failure"], self.env, runner=courier, sleep=self.sleeps.append), 0)
        self.assertEqual(len(sent), 1)
        self.assertEqual(self.run_timer(Fake(self.root), "--tag", TAG, "--reset-failures"), 0)
        self.assertIn("failures_reset", self.names())

    def test_the_lock_is_released_when_a_run_ends_whatever_way(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, fail="build_apk")), 1)
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertNotIn("busy", self.names())
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertNotIn("busy", self.names())

    def test_the_lock_file_is_private(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertEqual(stat.S_IMODE((self.root / "work" / "timer.lock").stat().st_mode), 0o600)

    def test_a_real_second_process_finds_the_lock_held(self) -> None:
        self.hold()
        env = {**self.env, "PATH": "/usr/bin"}
        result = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "quest_release_timer.py")], env=env,
                                capture_output=True, text=True, cwd=self.tmp.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("busy", result.stderr)
        self.assertIn("busy", self.names())


class HostBreakage(VariantCase):
    """What a broken answer from the host does: a recorded, counted failure with the file named and no host text, never a
    traceback; a short body is a transient failure, not a mismatched pair."""

    GOOD = hashlib.sha256(HOST_APK).hexdigest()
    SIDECAR = f"{GOOD}  {VARIANT_APK}\n".encode()

    def listed(self) -> Fake:
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        return fake

    def against(self, apk: bytes, sidecar: bytes) -> RawHost:
        raw = RawHost(self, apk, sidecar)
        self.env = {**self.env, "RELEASE_PUBLIC_BASE": raw.base}
        return raw

    def failed_once(self, want: str) -> dict:
        fake = self.listed()
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1, stderr)
        self.assertNotIn("Traceback", stderr)
        [failure] = self.failed()
        self.assertEqual(failure["step"], "variant_exists")
        self.assertIn(want, failure["error"])
        self.assertNotIn("worktree.add", fake.steps())
        self.assertNotIn("done", self.names())
        return failure

    def test_a_complete_answer_over_a_raw_socket_is_listed(self) -> None:
        self.against(http_response(HOST_APK), http_response(self.SIDECAR))
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.assertIn("variant_listed", self.names())

    def test_a_content_length_that_is_not_a_number_is_ignored(self) -> None:
        head = (b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: \xb2\r\n"
                b"Connection: close\r\n\r\n")
        self.against(head + HOST_APK, http_response(self.SIDECAR))
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.assertIn("variant_listed", self.names())

    def test_a_garbage_status_line_is_a_recorded_failure(self) -> None:
        self.against(b"this is not http\r\n\r\n", http_response(self.SIDECAR))
        failure = self.failed_once("no answer")
        self.assertIn(VARIANT_APK, failure["error"])

    def test_a_chunked_body_that_ends_early_or_is_malformed_is_a_recorded_failure(self) -> None:
        head = b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n\r\n"
        for body in (b"ZZZ\r\nnot a size\r\n", b"5\r\nab"):
            with self.subTest(body=body):
                self.log.unlink(missing_ok=True)
                self.against(head + body, http_response(self.SIDECAR))
                self.failed_once("broke off")

    def test_a_sidecar_that_breaks_off_is_a_failure_of_the_sidecar(self) -> None:
        head = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n"
        self.against(http_response(HOST_APK), head + b"ZZ\r\n")
        failure = self.failed_once("broke off")
        self.assertIn(f"{VARIANT_APK}.sha256", failure["error"])

    def test_a_short_apk_is_a_transient_failure_with_the_counts_not_a_mismatch(self) -> None:
        self.against(http_response(HOST_APK[:10], declared=len(HOST_APK)), http_response(self.SIDECAR))
        failure = self.failed_once(f"received 10 of {len(HOST_APK)} bytes")
        self.assertIn("broke off", failure["error"])
        self.assertNotIn("RELEASING.md", failure["error"], "no repair is pointed at for an incomplete read")
        self.assertNotIn("does not match", failure["error"])

    def test_a_short_sidecar_is_a_transient_failure_not_a_malformed_one(self) -> None:
        self.against(http_response(HOST_APK), http_response(self.SIDECAR[:20], declared=len(self.SIDECAR)))
        failure = self.failed_once(f"received 20 of {len(self.SIDECAR)} bytes")
        self.assertNotIn("RELEASING.md", failure["error"])
        self.assertNotIn("malformed", failure["error"])

    def test_the_request_asks_for_the_identity_encoding(self) -> None:
        raw = self.against(http_response(HOST_APK), http_response(self.SIDECAR))
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.assertTrue(raw.requests)
        for request in raw.requests:
            self.assertIn(b"Accept-Encoding: identity", request)

    def unconfirmed(self) -> dict:
        failure = self.failed_once("could not confirm the answer was complete")
        self.assertNotIn("RELEASING.md", failure["error"], "no repair is pointed at when the read cannot be called complete")
        return failure

    def test_an_encoded_answer_that_disagrees_could_not_be_confirmed_complete(self) -> None:
        zipped = gzip.compress(HOST_APK)
        self.against(http_response(zipped, extra="Content-Encoding: gzip\r\n"), http_response(self.SIDECAR))
        self.unconfirmed()

    def test_an_answer_with_no_declared_length_that_disagrees_could_not_be_confirmed_complete(self) -> None:
        self.against(http_without_length(HOST_APK[:-3]), http_response(self.SIDECAR))
        self.unconfirmed()

    def test_a_chunked_answer_that_ended_cleanly_but_disagrees_could_not_be_confirmed_complete(self) -> None:
        self.against(http_chunked(HOST_APK[:-3]), http_response(self.SIDECAR))
        self.unconfirmed()

    def test_an_answer_with_no_declared_length_that_agrees_is_listed(self) -> None:
        self.against(http_without_length(HOST_APK), http_response(self.SIDECAR))
        self.assertEqual(self.run_timer(self.listed()), 0)
        self.assertIn("variant_listed", self.names())

    def test_a_chunked_sidecar_that_does_not_parse_could_not_be_confirmed_complete(self) -> None:
        self.against(http_response(HOST_APK), http_chunked(b"not a sid"))
        self.unconfirmed()

    def test_a_chunked_sidecar_that_names_another_file_could_not_be_confirmed_complete(self) -> None:
        self.against(http_response(HOST_APK), http_chunked(f"{self.GOOD}  another.apk\n".encode()))
        self.unconfirmed()

    def test_a_complete_apk_read_that_disagrees_with_its_sidecar_points_at_the_repair(self) -> None:
        self.against(http_response(HOST_APK[:-3]), http_response(self.SIDECAR))
        failure = self.failed_once("RELEASING.md")
        self.assertNotIn("could not confirm", failure["error"])

    def test_a_chunked_answer_cut_short_reports_the_bytes_it_received(self) -> None:
        self.against(http_chunked(b"abcde", b"ab", finish=False), http_response(self.SIDECAR))
        failure = self.failed_once("received 7 bytes")
        self.assertIn("broke off", failure["error"])

    def test_a_complete_read_that_disagrees_still_points_at_the_repair(self) -> None:
        self.against(http_response(HOST_APK), http_response(b"x" * len(self.SIDECAR)))
        self.failed_once("RELEASING.md")

    def test_a_failure_of_this_kind_is_counted_toward_the_three(self) -> None:
        self.against(b"garbage\r\n\r\n", http_response(self.SIDECAR))
        for _ in range(3):
            self.assertEqual(self.run_timer(self.listed()), 1)
        self.assertEqual([e["step"] for e in self.failed()], ["variant_exists"] * 3)

    def test_a_url_the_standard_library_rejects_is_a_failure_not_a_traceback(self) -> None:
        for base in ("http://127.0.0.1:99999", "https://" + "a" * 70 + ".invalid"):  # InvalidURL; a label too long (UnicodeError)
            with self.subTest(base=base):
                self.log.unlink(missing_ok=True)
                env = {**self.env, "RELEASE_PUBLIC_BASE": base}
                code, stderr = self.run_captured(self.listed(), env=env)
                self.assertEqual(code, 1)
                self.assertNotIn("Traceback", stderr)
                self.assertEqual([e["step"] for e in self.failed()], ["variant_exists"])


class UnexpectedExceptions(VariantCase):
    """Anything a step does not expect is recorded as `unexpected`, counted, cleaned up after, and exits 1."""

    def boom(self, argv: list[str]) -> subprocess.CompletedProcess:
        raise RuntimeError(f"boom with {VDIR_MARK} and {SECRETS['R2_SECRET_ACCESS_KEY']}")

    def test_an_exception_in_a_variant_step_is_recorded_cleaned_up_after_and_exit_1(self) -> None:
        fake = Fake(self.root, overrides={"variant_build": self.boom})
        code, stderr = self.run_captured(fake)
        self.assertEqual(code, 1)
        [failure] = self.failed()
        self.assertEqual((failure["step"], failure["part"]), ("unexpected", "variant"))
        self.assertIn("RuntimeError", failure["error"])
        self.assertIn("quest_release_timer.py:", failure["error"], "where it happened")
        self.assertNotIn("Traceback", stderr)
        for where in (self.log.read_text(), stderr):
            self.assertNotIn("boom", where, "the text of an unexpected exception is not copied")
            self.assertNotIn(VDIR_MARK, where)
            self.assertNotIn(SECRETS["R2_SECRET_ACCESS_KEY"], where)
        self.assertIn("cleanup.worktree", fake.steps(), "the worktree is removed")
        self.assertFalse(self.scratch.exists(), "the private copies are removed")
        self.assertNotIn("done", self.names())

    def test_an_exception_in_the_main_release_is_main(self) -> None:
        fake = Fake(self.root, overrides={"build_apk": self.boom})
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([(e["step"], e["part"]) for e in self.failed()], [("unexpected", "main")])
        self.assertIn("cleanup.worktree", fake.steps())

    def test_an_exception_before_the_release_is_recorded_too(self) -> None:
        fake = Fake(self.root, overrides={"checkout.status": self.boom})
        self.assertEqual(self.run_timer(fake), 1)
        self.assertEqual([(e["step"], e["part"]) for e in self.failed()], [("unexpected", "main")])

    def test_it_counts_toward_the_three_tries(self) -> None:
        for _ in range(3):
            self.assertEqual(self.run_timer(Fake(self.root, overrides={"variant_build": self.boom})), 1)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertIn("skipped_after_failures", self.names())

    def test_a_step_failure_is_still_recorded_under_its_own_step(self) -> None:
        self.assertEqual(self.run_timer(Fake(self.root, fail="variant_build")), 1)
        self.assertEqual([e["step"] for e in self.failed()], ["variant_build"])


class TagIsWholeString(TimerCase):
    """A tag with a trailing newline matches `$`; it is not a release tag."""

    def test_a_tag_with_a_trailing_newline_is_refused_on_the_command_line(self) -> None:
        for tag in (f"{TAG}\n", f"{TAG} ", f"x{TAG}"):
            with self.subTest(tag=repr(tag)):
                fake = Fake(self.root)
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    self.assertEqual(self.run_timer(fake, "--tag", tag), 1)
                self.assertIn("is not a release tag", err.getvalue())
                self.assertEqual(fake.calls, [])

    def test_a_release_name_with_a_trailing_newline_is_never_the_newest(self) -> None:
        listing = json.dumps([{"tagName": "v9.9.9\n", "isDraft": False}, {"tagName": TAG, "isDraft": False}])
        self.assertEqual(timer.newest_published(listing), TAG)
        with self.assertRaises(timer.StepFailure):
            timer.newest_published(json.dumps([{"tagName": "v9.9.9\n", "isDraft": False}]))


class ResetFailures(TimerCase):
    """`--reset-failures --tag vX.Y.Z` clears the three tries of one tag by recording `failures_reset`."""

    def fail_three(self) -> None:
        for _ in range(3):
            self.assertEqual(self.run_timer(Fake(self.root, fail="build_apk")), 1)

    def test_a_reset_records_the_event_for_the_tag_runs_nothing_and_exits_0(self) -> None:
        self.fail_three()
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG, "--reset-failures"), 0)
        self.assertEqual(fake.calls, [], "a reset runs no command")
        [reset] = [e for e in self.events() if e["event"] == "failures_reset"]
        self.assertEqual((reset["tag"], reset["cleared"]), (TAG, 3))
        self.assertEqual(self.names()[-1], "failures_reset")

    def test_after_a_reset_the_tag_is_retried_and_finishes(self) -> None:
        self.fail_three()
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertIn("skipped_after_failures", self.names(), "without a reset the hand run is skipped")
        self.assertEqual(self.run_timer(Fake(self.root), "--tag", TAG, "--reset-failures"), 0)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertIn("build_apk", fake.steps())
        self.assertEqual(self.names()[-1], "done")

    def test_only_failures_after_the_newest_reset_count(self) -> None:
        self.fail_three()
        self.assertEqual(self.run_timer(Fake(self.root), "--tag", TAG, "--reset-failures"), 0)
        for _ in range(2):
            self.assertEqual(self.run_timer(Fake(self.root, fail="build_apk"), "--tag", TAG), 1)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertEqual(self.names()[-1], "done", "two failures since the reset are retried")
        self.log.unlink()
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk"} for _ in range(3)),
                       {"event": "failures_reset", "tag": TAG},
                       *({"event": "failed", "tag": TAG, "step": "build_apk"} for _ in range(3)))
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertIn("skipped_after_failures", self.names())

    def test_the_newest_of_two_resets_is_the_one_that_counts(self) -> None:
        failed = {"event": "failed", "tag": TAG, "step": "build_apk"}
        self.write_log(failed, failed, failed, {"event": "failures_reset", "tag": TAG}, failed, failed, failed,
                       {"event": "failures_reset", "tag": TAG}, failed)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertEqual(self.names()[-1], "done", "one failure since the newest reset is retried")

    def test_a_skip_notice_sent_before_the_reset_does_not_silence_the_next_one(self) -> None:
        self.write_log(*({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(3)),
                       {"event": "skip_notified", "tag": TAG}, {"event": "failures_reset", "tag": TAG},
                       *({"event": "failed", "tag": TAG, "step": "build_apk", "error": "x"} for _ in range(3)))
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertEqual(len(couriers(fake, "spritz")), 1)

    def test_a_reset_of_another_tag_changes_nothing(self) -> None:
        self.fail_three()
        self.assertEqual(self.run_timer(Fake(self.root), "--tag", "v5.16.1", "--reset-failures"), 0)
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertIn("skipped_after_failures", self.names())

    def test_a_reset_does_not_undo_done(self) -> None:
        self.write_log({"event": "done", "tag": TAG}, {"event": "failures_reset", "tag": TAG})
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake, "--tag", TAG), 0)
        self.assertIn("nothing_to_do", self.names())

    def test_a_reset_needs_a_tag_and_records_nothing_without_one(self) -> None:
        fake = Fake(self.root)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(self.run_timer(fake, "--reset-failures"), 1)
        self.assertIn("--tag", err.getvalue())
        self.assertNotIn("failures_reset", self.names())
        self.assertEqual(fake.calls, [])

    def test_a_reset_with_a_tag_that_is_not_a_release_tag_is_refused(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(self.run_timer(Fake(self.root), "--tag", "main", "--reset-failures"), 1)
        self.assertNotIn("failures_reset", self.names())


class ScratchOnlyWhatThisRunMade(TimerCase):
    """The variant scratch directory is removed only when this run made it: an unconfigured run must not delete a
    directory of that name that something else put there."""

    def foreign(self) -> pathlib.Path:
        scratch = self.root / "work" / f"variant-{TAG}"
        scratch.mkdir(parents=True)
        (scratch / "not-ours.txt").write_text("kept")
        return scratch

    def test_an_unconfigured_run_of_a_new_tag_leaves_a_directory_it_did_not_make(self) -> None:
        scratch = self.foreign()
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertEqual((scratch / "not-ours.txt").read_text(), "kept")

    def test_an_unconfigured_run_of_a_listed_tag_leaves_it_too(self) -> None:
        scratch = self.foreign()
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake), 0)
        self.assertTrue((scratch / "not-ours.txt").exists())

    def marked(self) -> pathlib.Path:
        scratch = self.root / "work" / f"variant-{TAG}"
        scratch.mkdir(parents=True)
        (scratch / timer.SCRATCH_MARKER).write_text("")
        (scratch / "libq15t.so").write_bytes(b"a private copy of a killed run")
        return scratch

    def test_a_directory_of_this_timer_left_by_a_killed_run_is_removed_by_any_run(self) -> None:
        for listed in (False, True):
            with self.subTest(listed=listed):
                scratch = self.marked()
                fake = Fake(self.root)
                fake.beta = TAG[1:] if listed else "5.16.1"
                self.log.unlink(missing_ok=True)
                self.assertEqual(self.run_timer(fake), 0)
                self.assertFalse(scratch.exists())
                self.assertIn("variant_scratch_removed", self.names())

    def test_a_marked_directory_is_removed_even_when_the_run_fails_early(self) -> None:
        scratch = self.marked()
        self.assertEqual(self.run_timer(Fake(self.root, fail="build_signer")), 1)
        self.assertFalse(scratch.exists())

    def test_a_marker_that_is_a_link_does_not_count(self) -> None:
        scratch = self.root / "work" / f"variant-{TAG}"
        scratch.mkdir(parents=True)
        elsewhere = self.root / "marker-target"
        elsewhere.write_text("")
        (scratch / timer.SCRATCH_MARKER).symlink_to(elsewhere)
        (scratch / "keep.txt").write_text("kept")
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertTrue((scratch / "keep.txt").exists())

    def fail_removing(self, name: str):
        real = os.unlink

        def unlink(path, *args, **kwargs):  # noqa: ANN001, ANN002, ANN003 - os.unlink's own signature
            if pathlib.Path(path).name == name:
                raise PermissionError(13, "Permission denied")
            return real(path, *args, **kwargs)

        return mock.patch.object(timer.os, "unlink", side_effect=unlink)

    def test_a_sweep_that_fails_part_way_leaves_the_marker_so_the_next_run_retries(self) -> None:
        scratch = self.marked()
        (scratch / "libq15t_early.so").write_bytes(b"another private copy")
        with self.fail_removing("libq15t_early.so"):
            code = self.run_timer(Fake(self.root))
        self.assertEqual(code, 1)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "failed"], ["cleanup"])
        self.assertTrue((scratch / timer.SCRATCH_MARKER).is_file(), "the directory is still marked, so it is still ours to retry")
        self.assertNotIn("variant_scratch_removed", self.names())
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertFalse(scratch.exists())
        self.assertIn("variant_scratch_removed", self.names())

    def test_a_sweep_removes_the_marker_after_the_files_and_the_directory_after_the_marker(self) -> None:
        scratch = self.marked()
        order: list[str] = []
        real = os.unlink

        def unlink(path, *args, **kwargs):  # noqa: ANN001, ANN002, ANN003
            order.append(pathlib.Path(path).name)
            return real(path, *args, **kwargs)

        with mock.patch.object(timer.os, "unlink", side_effect=unlink):
            self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertLess(order.index("libq15t.so"), order.index(timer.SCRATCH_MARKER))

    def test_a_sweep_that_fails_is_the_part_the_tag_was_in(self) -> None:
        for complete, part in ((False, "main"), (True, "variant")):
            with self.subTest(complete=complete):
                self.log.unlink(missing_ok=True)
                shutil.rmtree(self.root / "work", ignore_errors=True)
                scratch = self.marked()
                if complete:
                    self.write_log({"event": "main_complete", "tag": TAG})
                with self.fail_removing("libq15t.so"):
                    self.assertEqual(self.run_timer(Fake(self.root)), 1)
                self.assertEqual([(e["step"], e["part"]) for e in self.events() if e["event"] == "failed"], [("cleanup", part)])

    def test_a_link_to_a_marked_directory_is_not_followed(self) -> None:
        target = self.root / "elsewhere"
        target.mkdir()
        (target / timer.SCRATCH_MARKER).write_text("")
        (target / "keep.txt").write_text("kept")
        (self.root / "work").mkdir(parents=True)
        (self.root / "work" / f"variant-{TAG}").symlink_to(target)
        self.assertEqual(self.run_timer(Fake(self.root)), 0)
        self.assertEqual((target / "keep.txt").read_text(), "kept")

    def test_an_unconfigured_run_that_fails_leaves_it_too(self) -> None:
        scratch = self.foreign()
        self.assertEqual(self.run_timer(Fake(self.root, fail="publish_apk")), 1)
        self.assertTrue((scratch / "not-ours.txt").exists())


class VariantNotConfigured(TimerCase):
    """The machine without the private build keeps working: one record per run, and nothing else."""

    def test_a_new_tag_runs_the_main_release_and_says_once_that_the_variant_is_off(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "step_ok"], STEPS)
        [off] = [e for e in self.events() if e["event"] == "variant_not_configured"]
        self.assertIn("NEVR_VARIANT_DIR", off["reason"])
        self.assertIn("NEVR_VARIANT_PINS", off["reason"])
        self.assertEqual(self.names()[-1], "done")
        for step in ("variant_build", "variant_publish"):
            self.assertNotIn(step, fake.steps())

    def test_a_listed_tag_says_so_makes_no_request_and_is_done(self) -> None:
        host = PublicHost(self)
        env = {**self.env, "RELEASE_PUBLIC_BASE": host.base}
        fake = Fake(self.root)
        fake.beta = TAG[1:]
        self.assertEqual(self.run_timer(fake, env=env), 0)
        self.assertEqual(self.names().count("variant_not_configured"), 1)
        self.assertEqual(host.requests, [])
        self.assertNotIn("worktree.add", fake.steps())
        self.assertEqual(self.names()[-1], "done")


FAKE_TOOL = r'''#!PYTHON
import hashlib, json, os, pathlib, shutil, sys, tarfile

name = os.path.basename(sys.argv[0])
args = sys.argv[1:]
open(os.environ["CALLS"], "a").write(json.dumps({"tool": name, "args": args, "cwd": os.getcwd()}) + "\n")
tag = os.environ["FAKE_TAG"]
head = "c0ffee" + "0" * 34
if name == "git":
    rest = args[args.index("-C") + 2:] if "-C" in args else args
    if rest[0] == "rev-parse":
        sys.stdout.write(("3b77" + "0" * 36 if rest[1].endswith("^{commit}") else head) + "\n")
    elif rest[0] == "for-each-ref":
        sys.stdout.write(f"{tag}\t{'2' * 40}\t{'3' * 40}\n")
    elif rest[0] == "ls-remote":
        sys.stdout.write(f"{'2' * 40}\trefs/tags/{tag}\n{'3' * 40}\trefs/tags/{tag}^{{}}\n")
    elif rest[0] == "archive":
        tarfile.open(args[args.index("-o") + 1], "w").close()
    elif rest[:2] == ["worktree", "add"]:
        path = pathlib.Path(rest[-2])
        path.mkdir(parents=True)
        os.symlink(os.environ["FAKE_TOOLS"], path / "tools")
elif name == "gh-fake":
    sys.stdout.write("sentinel-gh-token\n")
elif name == "go":
    binary = pathlib.Path(args[args.index("-o") + 1])
    binary.write_text("#!" + sys.executable + "\nimport json, os, sys\n"
                      "open(os.environ['CALLS'], 'a').write(json.dumps({'tool': 'signer', 'args': sys.argv[1:]}) + '\\n')\n"
                      "print(json.dumps({'channels': {'beta': '" + tag[1:] + "'}, 'sha256': '0'}))\n")
    binary.chmod(0o755)
elif name == "pgrep":
    sys.exit(1)
elif name == "systemd-run":
    rest = args[args.index("MemoryMax=8G") + 1:]
    os.execvp(rest[0], rest)
elif name == "just":
    if args[0] == "release-quest-variant-apk":
        digests = b"".join(hashlib.sha256(pathlib.Path(p).read_bytes()).digest() for p in args[2].split(":"))
        out = pathlib.Path("build/quest-release") / args[1] / "variant"
        out.mkdir(parents=True)
        apk = b"a stand-in for the variant APK " + digests
        file = f"nevr-runtime-{args[1]}-q15t-quest.apk"
        (out / file).write_bytes(apk)
        (out / f"{file}.sha256").write_text(f"{hashlib.sha256(apk).hexdigest()}  {file}\n")
    elif args[0] == "remove-worktree":
        shutil.rmtree(args[1])
elif name == "rclone":
    source, dest = [a for a in args if not a.startswith("-")][-2:]
    target = pathlib.Path(os.environ["FAKE_SERVE"]) / os.path.basename(dest)
    if not ("--ignore-existing" in args and target.exists()):
        shutil.copy(source, target)
'''


class VariantRealProcess(TimerCase):
    """The tool as the unit starts it, with the real publish tool behind it: a real interpreter, the real logs, a real
    host on 127.0.0.1, and stand-ins for git, gh, go, just, rclone and the rest on the path."""

    def setUp(self) -> None:
        super().setUp()
        self.vdir = self.root / VDIR_MARK
        self.vdir.mkdir()
        for name in LIBRARIES:
            (self.vdir / name).write_bytes(library_bytes(name))
        self.pins = self.root / f"{PINS_MARK}.txt"
        self.pins.write_text("".join(f"{hashlib.sha256(library_bytes(n)).hexdigest()}  {n}\n" for n in LIBRARIES))
        (self.root / "repo").mkdir()  # the checkout the timer runs from: its commands run with it as the working directory
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for tool in ("git", "gh-fake", "go", "tar", "pgrep", "systemd-run", "just", "rclone", "courier"):
            (self.bin / tool).write_text(FAKE_TOOL.replace("PYTHON", sys.executable))
            (self.bin / tool).chmod(0o755)
        self.served = self.root / "served"
        (self.served / "nevr-runtime" / TAG).mkdir(parents=True)
        handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=str(self.served), **k)  # noqa: E731
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.calls = self.root / "calls.jsonl"
        self.run_env = {**self.env, "PATH": f"{self.bin}{os.pathsep}/usr/bin", "HOME": str(self.root),
                        "GH_BIN": str(self.bin / "gh-fake"), "CALLS": str(self.calls), "FAKE_TAG": TAG,
                        "FAKE_SERVE": str(self.served / "nevr-runtime" / TAG), "FAKE_TOOLS": str(ROOT / "tools"),
                        "RELEASE_PUBLIC_BASE": f"http://127.0.0.1:{server.server_address[1]}",
                        "NEVR_VARIANT_DIR": str(self.vdir), "NEVR_VARIANT_PINS": str(self.pins)}
        self.publish_log = self.root / "state" / "nevr-runtime" / "publish-quest-apk.jsonl"

    def start(self) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "quest_release_timer.py"), "--tag", TAG],
                              capture_output=True, text=True, env=self.run_env, cwd=self.tmp.name)

    def tool_calls(self) -> list[dict]:
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def assert_nothing_private(self, result: subprocess.CompletedProcess) -> None:
        texts = [result.stdout, result.stderr, self.log.read_text(), self.calls.read_text()]
        if self.publish_log.exists():
            texts.append(self.publish_log.read_text())
        for text in texts:
            for private in (VDIR_MARK, PINS_MARK):
                self.assertNotIn(private, text)

    def test_a_listed_release_gets_its_variant_through_the_real_timer_and_the_real_publish_tool(self) -> None:
        result = self.start()
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "step_ok"],
                         ["checkout", "tags", "token", "build_signer", "check_listed", "variant_exists", "variant_pins", "worktree",
                          "build_slot", "configure", "variant_build", "variant_publish", "cleanup"])
        libraries = [e for e in self.events() if e["event"] == "variant_library"]
        self.assertEqual([(e["name"], e["size"]) for e in libraries], [(n, 4096) for n in LIBRARIES])
        self.assertEqual(self.names()[-1], "done")
        publish = [json.loads(line) for line in self.publish_log.read_text().splitlines()]
        self.assertEqual([e["event"] for e in publish],
                         ["start", "local_files", "put_variant_apk", "put_sidecar", "fetch", "fetch", "published"])
        self.assertEqual({e["variant"] for e in publish}, {VARIANT})
        self.assertEqual(sorted(p.name for p in (self.served / "nevr-runtime" / TAG).iterdir()),
                         sorted([VARIANT_APK, f"{VARIANT_APK}.sha256"]), "the variant is on the host and nothing else is")
        apk = (self.served / "nevr-runtime" / TAG / VARIANT_APK).read_bytes()
        self.assertEqual((self.served / "nevr-runtime" / TAG / f"{VARIANT_APK}.sha256").read_text(),
                         f"{hashlib.sha256(apk).hexdigest()}  {VARIANT_APK}\n")
        signer = [c["args"] for c in self.tool_calls() if c["tool"] == "signer"]
        self.assertEqual(signer, [["verify-manifest", "--live"]], "the signer only reads the manifest: no claim, no put")
        self.assert_nothing_private(result)

    def test_a_pin_that_does_not_hold_stops_the_variant_and_names_the_file_in_the_real_output(self) -> None:
        data = library_bytes(LIBRARIES[1])
        (self.vdir / LIBRARIES[1]).write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        result = self.start()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"quest-release-timer: variant_pins: {LIBRARIES[1]} does not match its pin", result.stderr)
        [failure] = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual(failure["step"], "variant_pins")
        self.assertNotIn("variant_build", [c["args"][0] for c in self.tool_calls() if c["tool"] == "just"])
        self.assertEqual(list((self.served / "nevr-runtime" / TAG).iterdir()), [])
        self.assertNotIn("done", self.names())
        self.assert_nothing_private(result)

    def test_a_published_variant_is_found_on_the_host_and_nothing_is_built_again(self) -> None:
        self.assertEqual(self.start().returncode, 0)
        before = self.tool_calls()
        self.log.unlink()
        self.assertEqual(self.start().returncode, 0)
        self.assertEqual([e for e in self.events() if e["event"] == "variant_listed"][0]["apk"], VARIANT_APK)
        new = self.tool_calls()[len(before):]
        self.assertEqual([c for c in new if c["tool"] in ("just", "rclone")], [])


def rest_releases(draft: str | None = None, loader: str | None = "uploaded") -> str:
    """The REST release list (`gh api repos/<slug>/releases`): TAG published, and `draft` (when given) with its loader asset in
    the state `loader` (None: no loader asset)."""
    releases = [{"tag_name": TAG, "draft": False, "assets": []}]
    if draft is not None:
        assets = [] if loader is None else [{"name": timer.loader_asset_name(draft[1:]), "state": loader, "digest": ""}]
        releases.insert(0, {"tag_name": draft, "draft": True, "assets": assets})
    return json.dumps(releases)


class ChooseTag(unittest.TestCase):
    """choose_tag is the one rule for which release a run takes; the probe and the draft stage read it the same way."""

    def test_a_draft_newer_than_the_published_release_with_its_loader_uploaded_is_chosen(self) -> None:
        self.assertEqual(timer.choose_tag(rest_releases(DRAFT), []), (DRAFT, "draft"))

    def test_a_draft_whose_loader_is_not_uploaded_is_passed_over(self) -> None:
        for state in (None, "starter"):
            with self.subTest(loader=state):
                self.assertEqual(timer.choose_tag(rest_releases(DRAFT, state), []), (TAG, "published"))

    def test_a_draft_older_than_the_newest_published_release_is_passed_over(self) -> None:
        self.assertEqual(timer.choose_tag(rest_releases("v5.16.0"), []), (TAG, "published"))

    def test_a_draft_with_a_draft_abandoned_record_is_passed_over(self) -> None:
        events = [{"event": "draft_abandoned", "tag": DRAFT, "reason": "gone"}]
        self.assertEqual(timer.choose_tag(rest_releases(DRAFT), events), (TAG, "published"))

    def test_no_published_release_and_no_draft_is_a_failure(self) -> None:
        with self.assertRaises(timer.StepFailure):
            timer.choose_tag("[]", [])


class DraftStage(TimerCase):
    """`--stage-draft`: the APK is built while the release is a draft, and nothing reaches R2 until the release is published,
    its loader is the published one, and beta has not moved."""

    def polls(self, fake: Fake) -> list[int]:
        return [i for i, c in enumerate(fake.calls) if c["step"] == "release_view" and "isDraft" in c["argv"]]

    def test_a_draft_with_its_loader_builds_then_publishes_on_the_first_poll_that_sees_it_published(self) -> None:
        fake = Fake(self.root)
        fake.draft_polls = [True, True, False]
        self.assertEqual(self.run_timer(fake, "--stage-draft"), 0)
        order = [c["step"] for c in fake.calls]
        build, publish = order.index("build_apk"), order.index("publish_apk")
        polls = self.polls(fake)
        self.assertEqual(len(polls), 3)
        self.assertTrue(all(build < i < publish for i in polls), "every poll falls after the build and before the publish")
        self.assertEqual(self.sleeps, [10, 10], "one wait after each poll that still sees a draft")
        self.assertEqual(self.names()[-1], "done")
        self.assertEqual(self.events()[-1]["tag"], DRAFT)
        dispatch = next(c["argv"] for c in fake.calls if c["step"] == "dispatch")
        # The same dispatch the published path makes, for the same release: only the tag differs.
        plain_root = self.root / "plain"
        plain_env = {**self.env, "XDG_STATE_HOME": str(plain_root / "state"), "QUEST_RELEASE_WORK_ROOT": str(plain_root / "work")}
        plain = Fake(self.root)
        self.assertEqual(timer.main([], plain_env, runner=plain, sleep=self.sleeps.append), 0)
        plain_dispatch = next(c["argv"] for c in plain.calls if c["step"] == "dispatch")
        self.assertEqual([a.replace(DRAFT, TAG) for a in dispatch], plain_dispatch)

    def test_publish_apk_never_runs_while_the_release_is_a_draft(self) -> None:
        fake = Fake(self.root)
        fake.draft_polls = [True, True, False]

        def publish(argv: list[str]) -> subprocess.CompletedProcess:
            if not fake.poll_answers or fake.poll_answers[-1]:
                raise AssertionError("publish_apk ran while the release was a draft")
            pathlib.Path(argv[argv.index("--extra-out") + 1]).write_text(json.dumps([{"name": f"nevr-runtime-{TAG}-quest.apk"}]))
            return completed(argv)

        fake.overrides["publish_apk"] = publish
        self.assertEqual(self.run_timer(fake, "--stage-draft"), 0)
        self.assertEqual(fake.poll_answers, [True, True, False])
        self.assertIn("publish_apk", fake.steps())

    def test_a_draft_that_is_never_published_is_abandoned_and_reaches_no_r2(self) -> None:
        fake = Fake(self.root)
        fake.draft_polls = [True]
        self.assertEqual(self.run_timer(fake, "--stage-draft"), 0)
        self.assertEqual(len(self.sleeps), 270, "2700 seconds of 10-second waits")
        abandoned = [e for e in self.events() if e["event"] == "draft_abandoned"]
        self.assertEqual(abandoned[-1]["reason"], "wait_expired")
        self.assertNotIn("publish_apk", fake.steps())
        self.assertNotIn("dispatch", fake.steps())
        self.assertEqual([c["step"] for c in fake.calls if c["env"] and "R2_SECRET_ACCESS_KEY" in c["env"]], [])
        removed = [c["argv"] for c in fake.calls if c["step"] == "cleanup.worktree"]
        self.assertEqual(removed[-1][-1], str(self.root / "work" / DRAFT))
        self.assertNotIn("failed", self.names())

    def test_a_draft_deleted_while_waiting_is_abandoned_not_failed(self) -> None:
        fake = Fake(self.root)
        fake.draft_polls = [True]

        def sleep(seconds: float) -> None:
            self.sleeps.append(seconds)
            fake.view_rc = 1  # the release is deleted during the first wait

        self.assertEqual(timer.main(["--stage-draft"], self.env, runner=fake, sleep=sleep), 0)
        self.assertEqual(len(self.sleeps), 1)
        self.assertNotIn("failed", self.names())
        abandoned = [e for e in self.events() if e["event"] == "draft_abandoned"]
        self.assertEqual(abandoned[-1]["reason"], "gone")

    def test_a_draft_stage_failure_is_not_one_of_the_three(self) -> None:
        failing = Fake(self.root, fail="build_apk")
        self.assertEqual(self.run_timer(failing, "--stage-draft"), 0)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual([(e["step"], e["part"], e["tag"]) for e in failed], [("build_apk", "draft", DRAFT)])
        abandoned = [e for e in self.events() if e["event"] == "draft_abandoned"]
        self.assertEqual(abandoned[-1]["reason"], "failed:build_apk")
        self.assertEqual(timer.counted_failures([e for e in self.events() if e.get("tag") == DRAFT]), [])
        # The next run takes the published release, because the draft is abandoned.
        second = Fake(self.root)
        self.assertEqual(self.run_timer(second, "--stage-draft"), 0)
        self.assertIn("publish_apk", second.steps())
        self.assertEqual((self.events()[-1]["event"], self.events()[-1]["tag"]), ("done", TAG))

    def test_a_loader_that_changed_before_publication_is_refused_before_the_upload(self) -> None:
        fake = Fake(self.root)
        fake.draft_polls = [True, False]
        fake.loader_rc = 1
        self.assertEqual(self.run_timer(fake, "--stage-draft"), 1)
        self.assertIn("verify_apk", fake.steps())
        self.assertNotIn("publish_apk", fake.steps())
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual((failed[-1]["step"], failed[-1]["part"]), ("verify_loader", "main"))

    def test_beta_moving_during_the_wait_fails_before_the_dispatch(self) -> None:
        fake = Fake(self.root)
        fake.draft_polls = [True, False]

        def sleep(seconds: float) -> None:
            self.sleeps.append(seconds)
            fake.beta = "5.17.0"  # beta moves while the draft waits

        self.assertEqual(timer.main(["--stage-draft"], self.env, runner=fake, sleep=sleep), 1)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual(failed[-1]["step"], "check_listed")
        self.assertIn("beta moved", failed[-1]["error"])
        self.assertNotIn("publish_apk", fake.steps())
        self.assertNotIn("dispatch", fake.steps())

    def test_without_the_flag_a_draft_is_ignored(self) -> None:
        fake = Fake(self.root)
        self.assertEqual(self.run_timer(fake), 0)
        self.assertEqual([e["step"] for e in self.events() if e["event"] == "step_ok"], STEPS)
        self.assertNotIn("release_view", fake.steps())
        self.assertNotIn("list_api", fake.steps())
        self.assertNotIn("draft_abandoned", self.names())


class Probe(TimerCase):
    """`--probe`, the unit's ExecCondition: one GitHub call, then 0 when a run has work, 1 when it is idle, 255 when the probe
    itself failed."""

    def at(self, minutes_ago: float) -> str:
        return (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(minutes=minutes_ago)).isoformat()

    def record(self, event: str, minutes_ago: float, tag: str = TAG, **fields: object) -> dict:
        return {"ts": self.at(minutes_ago), "event": event, "tag": tag, **fields}

    def probe(self, fake: Fake, now: datetime.datetime | None = None) -> int:
        if now is None:
            return self.run_timer(fake, "--probe")
        return timer.probe(self.env, fake, self.log, now)

    def test_idle_when_the_newest_published_tag_is_done(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases()
        self.write_log(self.record("start", 1), self.record("done", 1))
        self.assertEqual(self.probe(fake), 1)

    def test_work_for_a_published_tag_that_is_not_done(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases()
        self.assertEqual(self.probe(fake), 0)

    def test_work_for_a_draft_with_its_loader(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases(DRAFT)
        self.write_log(self.record("start", 1), self.record("done", 1))
        self.assertEqual(self.probe(fake), 0)

    def test_idle_for_a_draft_without_its_loader_or_with_it_not_uploaded(self) -> None:
        for loader in (None, "starter"):
            with self.subTest(loader=loader):
                fake = Fake(self.root)
                fake.api_listing = rest_releases(DRAFT, loader)
                self.write_log(self.record("start", 1), self.record("done", 1))
                self.assertEqual(self.probe(fake), 1)

    def test_a_counted_failure_holds_its_tag_for_thirty_minutes(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases()
        self.write_log(self.record("start", 1), self.record("failed", 5, step="publish_apk", part="main", error="x"))
        self.assertEqual(self.probe(fake), 1, "a failure five minutes old is still holding the tag")
        self.write_log(self.record("start", 1), self.record("failed", 31, step="publish_apk", part="main", error="x"))
        self.assertEqual(self.probe(fake), 0, "a failure thirty-one minutes old is retried")

    def test_heartbeat_asks_for_a_run_after_thirty_minutes_without_one(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases()
        self.write_log(self.record("start", 31), self.record("done", 31))
        self.assertEqual(self.probe(fake), 0)
        self.write_log(self.record("start", 5), self.record("done", 5))
        self.assertEqual(self.probe(fake), 1)

    def test_one_gh_call_and_no_git(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases()
        self.write_log(self.record("start", 1), self.record("done", 1))
        self.assertEqual(self.probe(fake), 1)
        self.assertEqual([c["step"] for c in fake.calls], ["list_api"])

    def test_a_gh_error_exits_255_and_logs_a_failed_probe(self) -> None:
        fake = Fake(self.root)
        fake.overrides["list_api"] = lambda argv: completed(argv, 1, "", "gh: boom")
        self.assertEqual(self.probe(fake), 255)
        failed = [e for e in self.events() if e["event"] == "failed"]
        self.assertEqual(failed[-1]["step"], "probe")

    def test_twenty_idle_probes_write_one_probe_record(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases()
        self.write_log(self.record("start", 1), self.record("done", 1))
        for _ in range(20):
            self.assertEqual(self.probe(fake), 1)
        probes = [e for e in self.events() if e["event"] == "probe"]
        self.assertEqual(len(probes), 1)
        self.assertEqual(probes[0]["decision"], "idle")

    def test_idle_while_a_run_holds_the_lock(self) -> None:
        fake = Fake(self.root)
        fake.api_listing = rest_releases()
        held = timer.take_lock(self.root / "work")
        self.assertIsNotNone(held)
        try:
            self.assertEqual(self.probe(fake), 1)
        finally:
            os.close(held)
        self.assertEqual(self.probe(fake), 0)


class UnitsAndRecipe(unittest.TestCase):
    units = ROOT / "tools" / "systemd"

    def read(self, name: str) -> str:
        return (self.units / name).read_text(encoding="utf-8")

    def test_the_timer_runs_the_service_often_and_catches_up(self) -> None:
        text = self.read("nevr-quest-release.timer")
        self.assertIn("OnCalendar=*:*:00", text)
        self.assertIn("AccuracySec=1s", text)
        self.assertIn("Persistent=true", text)
        self.assertNotIn("*:0/30", text)
        self.assertIn("Unit=nevr-quest-release.service", text)
        self.assertIn("WantedBy=timers.target", text)

    def test_the_probe_is_the_condition_and_comes_before_every_other_command(self) -> None:
        text = self.read("nevr-quest-release.service")
        self.assertIn("ExecCondition=/usr/bin/python3 -I %h/src/nevr-runtime-work/release-timer/checkout/tools/"
                      "quest_release_timer.py --probe", text)
        self.assertLess(text.index("ExecCondition="), text.index("ExecStartPre="))

    def test_the_run_is_the_draft_stage(self) -> None:
        text = self.read("nevr-quest-release.service")
        self.assertRegex(text, r"(?m)^ExecStart=/usr/bin/python3 -I \S+quest_release_timer\.py --stage-draft$")

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

    def test_the_service_loads_the_optional_variant_environment_file_and_holds_no_value(self) -> None:
        text = self.read("nevr-quest-release.service")
        self.assertIn("EnvironmentFile=-%h/.config/metis/quest-variant.env", text, "the dash makes the file optional")
        self.assertIn("EnvironmentFile=%h/.config/metis/r2-evr-echo-taxi.env", text, "the credential file stays required")
        self.assertNotRegex(text, r"NEVR_VARIANT_(DIR|PINS)\s*=")

    def test_the_doc_and_the_module_docstring_state_the_variant_steps_the_environment_and_the_failure_rule(self) -> None:
        text = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
        section = text.split("## The Quest half of the train", 1)[1].split("\n## ", 1)[0]
        for want in ("variant_pins", "variant_build", "variant_publish", "variant_exists", "variant_listed",
                     "NEVR_VARIANT_DIR", "NEVR_VARIANT_PINS", "quest-variant.env", "variant_not_configured",
                     "never blocks", "release-quest-variant-apk", "--variant"):
            self.assertIn(want, section)
        for want in ("variant_pins", "variant_build", "variant_publish", "NEVR_VARIANT_DIR", "NEVR_VARIANT_PINS"):
            self.assertIn(want, timer.__doc__)

    def test_the_doc_states_the_pair_check_its_repair_the_build_log_the_part_and_the_reset(self) -> None:
        text = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
        section = text.split("## The Quest half of the train", 1)[1].split("\n## ", 1)[0]
        for want in ("--reset-failures", "failures_reset", "-build.log", "`part`", "by hand", "the publisher's credential",
                     "half-published", "variant_exists", "`local_files`", "main_complete", "`unexpected`",
                     "never copy the digest from the error message", "delete BOTH objects"):
            self.assertIn(want, section)
        self.assertNotIn("`variant_publish` record in", section)
        self.assertNotIn("when that worktree still exists", section)
        for want in ("run that reaches the release list", "timer.lock", "`busy`", "variant-only", "could not confirm"):
            self.assertIn(want, section)
        self.assertIn("`.made-by-quest-release-timer`", section)
        self.assertNotIn("A tag that beta already lists is done.", section)
        for stale in ("the live manifest already lists the tag: done, nothing to build",
                      "The R2 credential goes only to publish_apk,"):
            self.assertNotIn(stale, timer.__doc__)
        self.assertIn("variant_publish", timer.__doc__.split("The R2 credential goes only to", 1)[1].split("\n", 2)[0] +
                      timer.__doc__.split("The R2 credential goes only to", 1)[1].split("\n", 2)[1])
        self.assertIn("--reset-failures", timer.__doc__)

    def test_the_doc_says_the_timer_takes_the_newest_tag_only(self) -> None:
        text = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
        section = text.split("## The Quest half of the train", 1)[1].split("\n## ", 1)[0]
        self.assertIn("newest tag only", section)
        self.assertIn("dedicated checkout", section)

    def test_the_doc_names_the_probe_the_draft_wait_and_the_abandonment(self) -> None:
        text = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
        section = text.split("## The Quest half of the train", 1)[1].split("\n## ", 1)[0]
        for want in ("--probe", "--stage-draft", "draft_abandoned", "await_published", "verify_loader", "recheck_beta",
                     "every minute"):
            self.assertIn(want, section)
        self.assertNotIn("every 30 minutes", section)
        for want in ("--probe", "--stage-draft", "draft_abandoned"):
            self.assertIn(want, timer.__doc__)


if __name__ == "__main__":
    unittest.main()
