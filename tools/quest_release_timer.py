#!/usr/bin/env python3
"""The Quest half of a release, run by a user timer on the publisher's machine (RELEASING.md, "The Quest half of the
train"; #1210). Usage: quest_release_timer.py [--tag vX.Y.Z] | --notify-failure

The store APK, the Quest signing key and the R2 credential never leave this machine, so no workflow builds the Quest APK.
The train (release-train.yml) publishes the GitHub release; this tool sees the new tag and finishes the release:

  token, list      the newest published vX.Y.Z (or --tag) that is not done
  build_signer     the release signer, built from a snapshot of its main
  check_listed     the live manifest already lists the tag: done, nothing to build
  worktree         a clean worktree at the tag, set up for a build
  build_slot       waits while two or more ninja processes run
  build_apk        `just release-quest-apk` in the shared build slice
  publish_apk      tools/publish_quest_apk.py: R2, fetched back, and the manifest claim
  dispatch, watch  the beta manifest publish, live, in the release-signer repository
  check_jobs       the jobs that publish all succeeded (a green run with a skipped job is a failure)
  verify_live      `verify-manifest --live` lists the tag on beta
  cleanup          the worktree and the signer snapshot, by exact path

Every step appends a JSON line to $XDG_STATE_HOME/nevr-runtime/quest-release-timer.jsonl; a log that cannot be written is
a failure. A failed step exits 1 and names itself. A tag that has failed three times is not retried. The environment is
the R2 credential (R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY) and RELEASE_PUBLIC_BASE; a missing one
is a refusal that names it. No value is printed or logged: every value read from the environment, and the GitHub token, is
replaced by <redacted> in anything written.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from publish_quest_apk import Failure, Log  # noqa: E402 - the sibling tool's log and failure types

REQUIRED_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "RELEASE_PUBLIC_BASE")
SECRET_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")
TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
RUN_ID = re.compile(r"/actions/runs/([0-9]+)")
RELEASE_SLUG = "EchoTools/nevr-runtime"
SIGNER_SLUG = "thesprockee/release-signer"
PUBLISHING_JOBS = ("Commit the manifest record", "Publish to R2", "A run that published nothing is red")
MAX_FAILURES = 3
SLOT_WAITS = 60
SLOT_WAIT_SECONDS = 60
TAIL = 2000

Runner = Callable[..., subprocess.CompletedProcess]


class StepFailure(Failure):
    def __init__(self, step: str, message: str) -> None:
        super().__init__(message)
        self.step = step


def subprocess_runner(argv: list[str], cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, check=False)
    except OSError as error:
        return subprocess.CompletedProcess(argv, 127, "", f"cannot run {argv[0]}: {error.strerror}")


def state_log_path(env: dict[str, str]) -> pathlib.Path:
    state = env.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return pathlib.Path(state) / "nevr-runtime" / "quest-release-timer.jsonl"


def read_events(path: pathlib.Path) -> list[dict]:
    if not path.exists():
        return []
    events = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
    return events


def newest_published(listing: str) -> str:
    try:
        releases = json.loads(listing)
    except ValueError as error:
        raise StepFailure("list", f"the release list is not JSON: {error}") from error
    tags = [r["tagName"] for r in releases if not r.get("isDraft") and TAG.match(r.get("tagName", ""))]
    if not tags:
        raise StepFailure("list", "no published vX.Y.Z release")
    return max(tags, key=lambda t: tuple(int(n) for n in t[1:].split(".")))


class Timer:
    def __init__(self, env: dict[str, str], runner: Runner, sleep: Callable[[float], None], log: Log) -> None:
        self.env, self.runner, self.sleep, self.log = env, runner, sleep, log
        self.secrets = [env[name] for name in SECRET_ENV if env.get(name)]
        self.gh = env.get("GH_BIN", "gh-spritz")
        home = pathlib.Path(os.path.expanduser("~"))
        self.repo = pathlib.Path(env.get("QUEST_RELEASE_REPO") or pathlib.Path(__file__).resolve().parents[1])
        self.signer_repo = pathlib.Path(env.get("QUEST_RELEASE_SIGNER_REPO") or home / "src" / "release-signer")
        self.work = pathlib.Path(env.get("QUEST_RELEASE_WORK_ROOT") or home / "src" / "nevr-runtime-work" / "release-timer")
        self.child = dict(env)

    def redact(self, text: str) -> str:
        for secret in self.secrets:
            text = text.replace(secret, "<redacted>")
        return text

    def cmd(self, step: str, argv: list[str], cwd: pathlib.Path | str | None = None, extra: dict | None = None,
            ok_codes: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
        env = {**self.child, **(extra or {})}
        result = self.runner(argv, cwd=str(cwd) if cwd else None, env=env)
        if result.returncode not in ok_codes:
            tail = self.redact((result.stderr or "")[-TAIL:])
            raise StepFailure(step, f"{pathlib.Path(argv[0]).name} exited {result.returncode}: {tail}".strip())
        return result

    def ok(self, step: str, **fields: object) -> None:
        self.log.event("step_ok", step=step, **fields)

    def live_beta(self, bin_path: pathlib.Path, signer_dir: pathlib.Path, step: str) -> str:
        out = self.cmd(step, [str(bin_path), "verify-manifest", "--live"], signer_dir).stdout
        for line in out.splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if isinstance(record, dict) and "channels" in record:
                return str(record["channels"].get("beta", ""))
        raise StepFailure(step, "verify-manifest --live printed no manifest")

    def build_signer(self, signer_dir: pathlib.Path, tarball: pathlib.Path) -> pathlib.Path:
        step = "build_signer"
        repo = str(self.signer_repo)
        self.cmd(step, ["git", "-C", repo, "fetch", "origin", "main"])
        sha = self.cmd(step, ["git", "-C", repo, "rev-parse", "origin/main"]).stdout.strip()
        self.cmd(step, ["git", "-C", repo, "archive", "--format=tar", "-o", str(tarball), "origin/main"])
        signer_dir.mkdir(parents=True, exist_ok=True)
        self.cmd(step, ["tar", "-xf", str(tarball), "-C", str(signer_dir)])
        binary = signer_dir / "release-signer"
        self.cmd(step, ["go", "build", "-o", str(binary), "./cmd/release-signer"], signer_dir)
        self.ok(step, signer_commit=sha)
        return binary

    def wait_for_build_slot(self) -> None:
        for _ in range(SLOT_WAITS):
            result = self.cmd("build_slot", ["pgrep", "-x", "ninja"], ok_codes=(0, 1))
            if len([line for line in result.stdout.splitlines() if line.strip()]) < 2:
                return
            self.sleep(SLOT_WAIT_SECONDS)
        raise StepFailure("build_slot", f"two or more ninja processes ran for {SLOT_WAITS * SLOT_WAIT_SECONDS} seconds")

    def check_jobs(self, run_id: str) -> None:
        out = self.cmd("check_jobs", [self.gh, "run", "view", run_id, "-R", SIGNER_SLUG, "--json", "jobs"]).stdout
        try:
            jobs = {j["name"]: j.get("conclusion") for j in json.loads(out)["jobs"]}
        except (ValueError, KeyError, TypeError) as error:
            raise StepFailure("check_jobs", f"the run's jobs are unreadable: {error}") from error
        bad = [f"{name} is {jobs.get(name, 'missing')}" for name in PUBLISHING_JOBS if jobs.get(name) != "success"]
        if bad:
            raise StepFailure("check_jobs", "run %s did not publish: %s" % (run_id, "; ".join(bad)))
        self.ok("check_jobs", run=run_id)

    def release(self, tag: str) -> None:
        version = tag[1:]
        signer_dir = self.work / f"release-signer-{tag}"
        tarball = self.work / f"release-signer-{tag}.tar"
        extra_file = self.work / f"extra-{tag}.json"
        worktree = self.work / tag
        made = {"worktree": False}
        try:
            self._release_steps(tag, version, worktree, signer_dir, tarball, extra_file, made)
        except StepFailure:
            self.cleanup(worktree if made["worktree"] else None, signer_dir, tarball, extra_file, raising=False)
            raise
        self.cleanup(worktree if made["worktree"] else None, signer_dir, tarball, extra_file, raising=True)

    def _release_steps(self, tag: str, version: str, worktree: pathlib.Path, signer_dir: pathlib.Path,
                       tarball: pathlib.Path, extra_file: pathlib.Path, made: dict) -> None:
        self.work.mkdir(parents=True, exist_ok=True)
        readme = self.work / "README.md"
        if not readme.exists():
            readme.write_text("Scratch of tools/quest_release_timer.py (the Quest half of a release), owned by the "
                              "user timer nevr-quest-release. Each run removes what it makes here when it ends.\n",
                              encoding="utf-8")
        binary = self.build_signer(signer_dir, tarball)
        if self.live_beta(binary, signer_dir, "check_listed") == version:
            self.log.event("already_listed", beta=version)
            self.ok("check_listed", listed=True)
            return
        self.ok("check_listed", listed=False)

        repo = str(self.repo)
        self.cmd("worktree", ["git", "-C", repo, "fetch", "origin", "tag", tag])
        commit = self.cmd("worktree", ["git", "-C", repo, "rev-parse", f"{tag}^{{commit}}"]).stdout.strip()
        self.cmd("worktree", ["git", "-C", repo, "worktree", "add", "--detach", str(worktree), commit])
        made["worktree"] = True
        self.cmd("worktree", ["just", "worktree-setup"], worktree)
        self.ok("worktree", commit=commit)

        self.wait_for_build_slot()
        self.ok("build_slot")
        build_env = {"VCPKG_MAX_CONCURRENCY": "6", "CMAKE_BUILD_PARALLEL_LEVEL": "6", "GH_BIN": self.gh}
        self.cmd("build_apk", ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G",
                               "just", "release-quest-apk", tag], worktree, build_env)
        self.ok("build_apk")

        publish_env = {"RELEASE_SIGNER_BIN": str(binary), "RELEASE_SIGNER_DIR": str(signer_dir), "GH_BIN": self.gh}
        self.cmd("publish_apk", ["python3", "tools/publish_quest_apk.py", tag, "--extra-out", str(extra_file)],
                 worktree, publish_env)
        try:
            claim = json.dumps(json.loads(extra_file.read_text(encoding="utf-8")), separators=(",", ":"))
        except (OSError, ValueError) as error:
            raise StepFailure("publish_apk", f"the manifest claim {extra_file.name} is unreadable: {error}") from error
        self.ok("publish_apk")

        dispatched = self.cmd("dispatch", [self.gh, "workflow", "run", "sign-manifest.yml", "-R", SIGNER_SLUG, "--ref",
                                           "main", "-f", "action=publish", "-f", f"tag={tag}", "-f", "channel=beta",
                                           "-f", f"extra_artifacts={claim}", "-f", "dry_run=false"])
        found = RUN_ID.search(dispatched.stdout)
        if not found:
            raise StepFailure("dispatch", "the dispatch printed no run URL")
        run_id = found.group(1)
        self.ok("dispatch", run=run_id)
        self.cmd("watch", [self.gh, "run", "watch", run_id, "-R", SIGNER_SLUG, "--exit-status"])
        self.ok("watch", run=run_id)
        self.check_jobs(run_id)
        beta = self.live_beta(binary, signer_dir, "verify_live")
        if beta != version:
            raise StepFailure("verify_live", f"the live manifest lists beta {beta or 'nothing'}, not {version}")
        self.ok("verify_live", beta=beta)

    def cleanup(self, worktree: pathlib.Path | None, signer_dir: pathlib.Path, tarball: pathlib.Path,
                extra_file: pathlib.Path, raising: bool) -> None:
        """Remove exactly what this run made. A failed removal is logged either way; it fails the run only when the
        release itself succeeded, so it never hides the failure that came first."""
        problems = []
        if worktree is not None:
            result = self.runner(["just", "remove-worktree", str(worktree)], cwd=str(self.repo), env=self.child)
            if result.returncode != 0:
                problems.append(f"remove-worktree exited {result.returncode}: {self.redact((result.stderr or '')[-TAIL:])}")
        try:
            if signer_dir.exists():
                shutil.rmtree(signer_dir)
            tarball.unlink(missing_ok=True)
            extra_file.unlink(missing_ok=True)
        except OSError as error:
            problems.append(f"cannot remove scratch: {error.strerror}")
        if problems:
            self.log.event("cleanup_failed", error="; ".join(problems))
            if raising:
                raise StepFailure("cleanup", "; ".join(problems))
            return
        self.ok("cleanup")


def notify_failure(env: dict[str, str], runner: Runner, log_path: pathlib.Path) -> int:
    failed = [e for e in read_events(log_path) if e.get("event") == "failed"]
    last = failed[-1] if failed else {}
    body = "quest-release-timer failed: tag %s at step %s: %s (log: %s)" % (
        last.get("tag", "unknown"), last.get("step", "unknown"), str(last.get("error", "no failed event recorded"))[:300],
        log_path)
    result = runner(["courier", "send", "--to", "spritz", "--from", "quest-release-timer", "--msg", body], cwd=None, env=env)
    if result.returncode != 0:
        sys.stderr.write("quest-release-timer: the failure notice could not be sent\n")
        return 1
    return 0


def main(argv: list[str], env: dict[str, str], runner: Runner = subprocess_runner,
         sleep: Callable[[float], None] = time.sleep) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", help="release this vX.Y.Z instead of the newest published one")
    parser.add_argument("--notify-failure", action="store_true", help="courier the last recorded failure to spritz")
    args = parser.parse_args(argv)
    path = state_log_path(env)
    if args.notify_failure:
        return notify_failure(env, runner, path)
    if args.tag and not TAG.match(args.tag):
        sys.stderr.write(f"quest-release-timer: {args.tag} is not a release tag (vX.Y.Z)\n")
        return 1
    log = None
    try:
        log = Log(path, args.tag or "")
        log.event("start")
        missing = [name for name in REQUIRED_ENV if not env.get(name)]
        if missing:
            log.event("refused", missing=missing)
            raise Failure(f"refused: set {', '.join(missing)}")
        timer = Timer(env, runner, sleep, log)
        token = timer.cmd("token", [timer.gh, "auth", "token"]).stdout.strip()
        if not token:
            raise StepFailure("token", f"{timer.gh} auth token printed nothing")
        timer.secrets.append(token)
        timer.child["GH_TOKEN"] = token
        log.event("step_ok", step="token")
        tag = args.tag
        if not tag:
            tag = newest_published(timer.cmd("list", [timer.gh, "release", "list", "--repo", RELEASE_SLUG, "--json",
                                                      "tagName,isDraft", "--limit", "20"]).stdout)
            log.event("step_ok", step="list", newest=tag)
        log.base["tag"] = tag
        history = [e for e in read_events(path) if e.get("tag") == tag]
        if any(e.get("event") == "done" for e in history):
            log.event("nothing_to_do", reason="done")
            return 0
        if sum(1 for e in history if e.get("event") == "failed") >= MAX_FAILURES:
            log.event("skipped_after_failures", failures=MAX_FAILURES)
            return 0
        timer.release(tag)
        log.event("done")
        return 0
    except StepFailure as failure:
        sys.stderr.write(f"quest-release-timer: {failure.step}: {failure}\n")
        if log is not None:
            _record_failure(log, failure.step, str(failure))
        return 1
    except Failure as failure:
        sys.stderr.write(f"quest-release-timer: {failure}\n")
        if log is not None and not str(failure).startswith("refused"):
            _record_failure(log, "start", str(failure))
        return 1
    finally:
        if log is not None:
            log.close()


def _record_failure(log: Log, step: str, error: str) -> None:
    try:
        log.event("failed", step=step, error=error)
    except Failure as log_failure:
        sys.stderr.write(f"quest-release-timer: {log_failure}\n")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
