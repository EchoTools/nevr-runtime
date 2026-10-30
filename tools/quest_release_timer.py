#!/usr/bin/env python3
"""The Quest half of a release, run by a user timer on the publisher's machine (RELEASING.md, "The Quest half of the
train"; #1210). Usage: quest_release_timer.py [--tag vX.Y.Z] | --notify-failure

The store APK, the Quest signing key and the R2 credential never leave this machine, so no workflow builds the Quest APK.
The train (release-train.yml) publishes the GitHub release; this tool sees the new tag and finishes the release:

  checkout         the checkout this runs from is clean and HEAD is the freshly fetched origin/main, or it refuses
  tags             every local tag is on origin at the same commit (the release preflight's rule), or it refuses naming up to
                   ten tags with their short ids, counting the rest, and printing the quoted commands that remove and restore each
  token, list      the newest published vX.Y.Z (or --tag) that is not done
  build_signer     the release signer, built from a snapshot of its main
  check_listed     the live manifest already lists the tag: done, nothing to build; else the game data it carries
  worktree         a clean worktree at the tag, set up for a build
  build_slot       waits while two or more ninja processes run
  build_apk        `just release-quest-apk` in the shared build slice
  publish_apk      tools/publish_quest_apk.py: R2, fetched back, and the manifest claim
  dispatch, watch  the beta manifest publish, live, in the release-signer repository
  check_jobs       the jobs that publish all succeeded (a green run with a skipped job is a failure)
  verify_live      `verify-manifest --live` lists the tag on beta, and the manifest it verified lists the carried game data
  notify           courier Teth to verify the live publish, and Spritz (the owner is never bonked, #1410)
  cleanup          the worktree and the signer snapshot, by exact path

Every step appends a JSON line to $XDG_STATE_HOME/nevr-runtime/quest-release-timer.jsonl; a log that cannot be written is
a failure. `--notify-failure` couriers the last failure once per step and error in six hours (`failure_notified`, then
`failure_notice_suppressed`). A failed step exits 1 and names itself, with the redacted tail of the failing command's stderr and of its stdout (not the stdout of curl, gh or the signer, which is remote content). A refusal
(`refused`: a dirty or stale checkout, a tag origin does not have) exits 1 and is not counted. A tag that has failed three times is not retried, and says so once (a
courier to Spritz) before the run exits 0. The R2 credential goes only to publish_apk, and the GitHub token only to publish_apk and
the dispatch: every other command gets an environment without them. The environment is
the R2 credential (R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY) and RELEASE_PUBLIC_BASE; a missing one
is a refusal that names it. No value is printed or logged: every value read from the environment, and the GitHub token, is
replaced by <redacted> in anything written.
"""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Callable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from publish_quest_apk import Failure, Log  # noqa: E402 - the sibling tool's log and failure types
from release_preflight import commit_of, parse_ls_remote  # noqa: E402 - the preflight's reading of tags and its commit rule

REQUIRED_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "RELEASE_PUBLIC_BASE")
SECRET_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")
SENDER = "quest-release-timer"
MANIFEST_PATH = "nevr-runtime/manifest.json"
USER_AGENT = "nevr-runtime-quest-release-timer"  # the release host answers 403 to the default agent
TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
RUN_ID = re.compile(r"/actions/runs/([0-9]+)")
RELEASE_SLUG = "EchoTools/nevr-runtime"
SIGNER_SLUG = "thesprockee/release-signer"
PUBLISHING_JOBS = ("Commit the manifest record", "Publish to R2", "A run that published nothing is red")
MAX_FAILURES = 3
SLOT_WAITS = 60
SLOT_WAIT_SECONDS = 60
TAIL = 2000
SIGNER_BINARY = "release-signer"  # what build_signer builds; its `verify-manifest --live` prints the live manifest on stdout
# Programs whose stdout is remote content, never a log field (docs/standards/logging.md, Rule 4).
REMOTE_OUTPUT_PROGRAMS = frozenset({"curl", "gh", "gh-spritz", SIGNER_BINARY})
NOTICE_QUIET = datetime.timedelta(hours=6)  # the same failure is couriered at most this often
MAX_TAGS_LISTED = 10
LOCAL_TAGS_FORMAT = "--format=%(refname:strip=2)\t%(objectname)\t%(*objectname)"  # release_preflight.local_tags' format

Runner = Callable[..., subprocess.CompletedProcess]


class StepFailure(Failure):
    def __init__(self, step: str, message: str) -> None:
        super().__init__(message)
        self.step = step


class Refusal(StepFailure):
    """A precondition that is not met (a dirty checkout, a checkout not at origin/main): logged as `refused`, never counted."""


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
        self.r2 = {name: env[name] for name in SECRET_ENV if env.get(name)}
        self.token = ""
        self.gh = env.get("GH_BIN", "gh-spritz")
        home = pathlib.Path(os.path.expanduser("~"))
        self.repo = pathlib.Path(env.get("QUEST_RELEASE_REPO") or pathlib.Path(__file__).resolve().parents[1])
        self.signer_repo = pathlib.Path(env.get("QUEST_RELEASE_SIGNER_REPO") or home / "src" / "release-signer")
        self.work = pathlib.Path(env.get("QUEST_RELEASE_WORK_ROOT") or home / "src" / "nevr-runtime-work" / "release-timer")
        # What every command gets. The R2 credential and the GitHub token are added by the one or two steps that use them.
        self.child = {k: v for k, v in env.items() if k not in SECRET_ENV and k != "GH_TOKEN"}

    def redact(self, text: str) -> str:
        for secret in self.secrets:
            text = text.replace(secret, "<redacted>")
        return text

    def cmd(self, step: str, argv: list[str], cwd: pathlib.Path | str | None = None, extra: dict | None = None,
            ok_codes: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
        env = {**self.child, **(extra or {})}
        result = self.runner(argv, cwd=str(cwd) if cwd else None, env=env)
        if result.returncode not in ok_codes:
            raise StepFailure(step, f"{pathlib.Path(argv[0]).name} exited {result.returncode}:{self.tails(argv[0], result)}")
        return result

    def tails(self, program: str, result: subprocess.CompletedProcess) -> str:
        """` stderr: <tail> | stdout: <tail>`, an empty stream left out, and no stdout of a program in
        REMOTE_OUTPUT_PROGRAMS (or the gh binary in use). The secrets go before the cut, so a secret the cut would split
        leaves no fragment."""
        name = pathlib.Path(program).name
        remote = name in REMOTE_OUTPUT_PROGRAMS or name == pathlib.Path(self.gh).name
        parts = []
        for label, text in (("stderr", result.stderr), ("stdout", "" if remote else result.stdout)):
            tail = self.redact(text or "")[-TAIL:].strip()
            if tail:
                parts.append(f"{label}: {tail}")
        return (" " + " | ".join(parts)) if parts else ""

    def ok(self, step: str, **fields: object) -> None:
        self.log.event("step_ok", step=step, **fields)

    def live_state(self, bin_path: pathlib.Path, signer_dir: pathlib.Path, step: str) -> dict:
        out = self.cmd(step, [str(bin_path), "verify-manifest", "--live"], signer_dir).stdout
        for line in out.splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if isinstance(record, dict) and "channels" in record:
                return record
        raise StepFailure(step, "verify-manifest --live printed no manifest")

    def live_beta(self, bin_path: pathlib.Path, signer_dir: pathlib.Path, step: str) -> str:
        return str(self.live_state(bin_path, signer_dir, step)["channels"].get("beta", ""))

    def live_manifest(self, state: dict, step: str) -> dict:
        """The manifest the public host serves, held to the bytes the signer just verified (their sha256)."""
        base = self.env["RELEASE_PUBLIC_BASE"].rstrip("/")
        body = self.cmd(step, ["curl", "--fail", "--silent", "--show-error", "--max-time", "60", "-A", USER_AGENT,
                               f"{base}/{MANIFEST_PATH}"]).stdout
        digest = hashlib.sha256(body.encode("utf-8")).hexdigest()
        if digest != state.get("sha256"):
            raise StepFailure(step, f"the manifest the host serves has sha256 {digest}, not the {state.get('sha256')} "
                                    "that verify-manifest --live verified")
        try:
            return json.loads(body)
        except ValueError as error:
            raise StepFailure(step, f"the manifest the host serves is not JSON: {error}") from error

    @staticmethod
    def game_data(manifest: dict, version: str) -> dict | None:
        release = manifest.get("releases", {}).get(version, {})
        found = [a for a in release.get("artifacts", []) if a.get("kind") == "game-data"]
        return {"name": found[0].get("name"), "size": found[0].get("size")} if found else None

    def check_checkout(self) -> None:
        """The checkout this runs from is the code that ships: clean, and exactly the freshly fetched origin/main."""
        step = "checkout"
        repo = str(self.repo)
        self.cmd(step, ["git", "-C", repo, "fetch", "--quiet", "origin", "main"])
        dirty = self.cmd(step, ["git", "-C", repo, "status", "--porcelain"]).stdout.strip()
        if dirty:
            raise Refusal(step, f"the checkout the timer runs from is not clean: {dirty.splitlines()[0]}")
        head = self.cmd(step, ["git", "-C", repo, "rev-parse", "HEAD"]).stdout.strip()
        main = self.cmd(step, ["git", "-C", repo, "rev-parse", "origin/main"]).stdout.strip()
        if head != main:
            raise Refusal(step, f"the checkout the timer runs from is at {head[:12]}, not the fetched origin/main {main[:12]}")
        self.ok(step, head=head)

    def check_tags(self) -> None:
        """Every local tag is on origin at the same commit, or the run refuses before it takes a token or builds: the
        preflight inside `just release-quest-apk` refuses the same checkout, after the signer and the worktree are made.
        The rule is tools/release_preflight.py's (its parse_ls_remote and commit_of): a tag that origin lacks, or that
        peels to another commit there. A tag only origin has is not an offender."""
        step = "tags"
        repo = str(self.repo)
        local_text = self.cmd(step, ["git", "-C", repo, "for-each-ref", "refs/tags", LOCAL_TAGS_FORMAT]).stdout
        remote_text = self.cmd(step, ["git", "-C", repo, "ls-remote", "--tags", "origin"]).stdout
        local = {}
        for line in local_text.splitlines():
            name, obj, peeled = (line.split("\t") + ["", ""])[:3]
            local[name] = (obj, peeled or None)
        _, remote = parse_ls_remote(remote_text)
        offenders = []
        for name, entry in sorted(local.items()):
            if name not in remote:
                why = "local-only, origin does not have it"
            elif commit_of(entry) != commit_of(remote[name]):
                why = f"points at {commit_of(entry)[:12]} here, {commit_of(remote[name])[:12]} on origin"
            else:
                continue
            offenders.append((name, entry[0], why))
        if offenders:
            # The head is a compact summary that survives the courier's cut of the error; the commands follow it. A tag name
            # is whatever git allows, `;`, `$(...)` and quotes included, so every name and id is shell-quoted.
            shown, rest = offenders[:MAX_TAGS_LISTED], max(0, len(offenders) - MAX_TAGS_LISTED)
            head = ("local tags origin lacks or has elsewhere: "
                    + ", ".join(f"{shlex.quote(name)} ({obj[:12]})" for name, obj, _ in shown)
                    + (f", and {rest} more" if rest else "") + f" [{len(offenders)} of {len(local)}]")
            commands = [f"{shlex.quote(name)} ({why}): remove: git tag -d {shlex.quote(name)}; rollback: "
                        f"git update-ref {shlex.quote('refs/tags/' + name)} {shlex.quote(obj)}" for name, obj, why in shown]
            raise Refusal(step, " | ".join([head, *commands]))
        self.ok(step, count=len(local))

    def notify_published(self, tag: str) -> None:
        """A live publish couriers Teth to verify it and Spritz the same text. The owner is never bonked for it (#1410)."""
        step = "notify"
        text = f"{tag} is live on the beta manifest (built and published by the Quest release timer); verify it through the installer"
        self.cmd(step, ["courier", "send", "--to", "teth", "--from", SENDER, "--msg", f"{text}. Log: {self.log.path}"])
        self.cmd(step, ["courier", "send", "--to", "spritz", "--from", SENDER, "--msg", f"{text}. Log: {self.log.path}"])
        self.ok(step)

    def build_signer(self, signer_dir: pathlib.Path, tarball: pathlib.Path) -> pathlib.Path:
        step = "build_signer"
        repo = str(self.signer_repo)
        self.cmd(step, ["git", "-C", repo, "fetch", "origin", "main"])
        sha = self.cmd(step, ["git", "-C", repo, "rev-parse", "origin/main"]).stdout.strip()
        self.cmd(step, ["git", "-C", repo, "archive", "--format=tar", "-o", str(tarball), "origin/main"])
        signer_dir.mkdir(parents=True, exist_ok=True)
        self.cmd(step, ["tar", "-xf", str(tarball), "-C", str(signer_dir)])
        binary = signer_dir / SIGNER_BINARY
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

    def notify_skip(self, tag: str, last: dict) -> None:
        """The third failure ends the retries: Spritz hears it once by courier, before the run exits 0 (never a bonk, #1410)."""
        step = "notify"
        text = (f"quest release timer gave up on {tag} after {MAX_FAILURES} failures; last: step "
                f"{last.get('step', 'unknown')}: {str(last.get('error', ''))[:200]}")
        self.cmd(step, ["courier", "send", "--to", "spritz", "--from", SENDER, "--msg", f"{text}. Log: {self.log.path}"])

    def notice_owed(self, tag: str) -> bool:
        """This timer published the tag and its notice was never sent (the run ended between the two)."""
        steps = [e.get("step") for e in read_events(self.log.path) if e.get("tag") == tag and e.get("event") == "step_ok"]
        return "verify_live" in steps and "notify" not in steps

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
        before = self.live_state(binary, signer_dir, "check_listed")
        beta = str(before["channels"].get("beta", ""))
        if beta == version:
            self.log.event("already_listed", beta=version)
            self.ok("check_listed", listed=True)
            if self.notice_owed(tag):
                self.notify_published(tag)
            return
        carried = self.game_data(self.live_manifest(before, "check_listed"), beta)
        if carried is None:
            raise StepFailure("check_listed", f"the live manifest's beta release {beta or 'none'} lists no game-data "
                                              "artifact to carry into the new release")
        self.ok("check_listed", listed=False, carried=carried["name"], carried_size=carried["size"])

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
        # A new tag's worktree has no vcpkg install yet (worktree-setup seeds the build tree only at a commit the main
        # checkout built), and release-quest-apk needs the nlohmann/json headers that install provides.
        self.cmd("configure", ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G",
                               "just", "configure"], worktree, build_env)
        self.ok("configure")
        self.cmd("build_apk", ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G",
                               "just", "release-quest-apk", tag], worktree, build_env)
        self.ok("build_apk")

        publish_env = {**self.r2, "GH_TOKEN": self.token, "RELEASE_SIGNER_BIN": str(binary),
                       "RELEASE_SIGNER_DIR": str(signer_dir), "GH_BIN": self.gh}
        self.cmd("publish_apk", ["python3", "tools/publish_quest_apk.py", tag, "--extra-out", str(extra_file)],
                 worktree, publish_env)
        try:
            claim = json.dumps(json.loads(extra_file.read_text(encoding="utf-8")), separators=(",", ":"))
        except (OSError, ValueError) as error:
            raise StepFailure("publish_apk", f"the manifest claim {extra_file.name} is unreadable: {error}") from error
        self.ok("publish_apk")

        dispatched = self.cmd("dispatch", [self.gh, "workflow", "run", "sign-manifest.yml", "-R", SIGNER_SLUG, "--ref",
                                           "main", "-f", "action=publish", "-f", f"tag={tag}", "-f", "channel=beta",
                                           "-f", f"extra_artifacts={claim}", "-f", "dry_run=false"],
                              extra={"GH_TOKEN": self.token})
        found = RUN_ID.search(dispatched.stdout)
        if not found:
            raise StepFailure("dispatch", "the dispatch printed no run URL")
        run_id = found.group(1)
        self.ok("dispatch", run=run_id)
        self.cmd("watch", [self.gh, "run", "watch", run_id, "-R", SIGNER_SLUG, "--exit-status"])
        self.ok("watch", run=run_id)
        self.check_jobs(run_id)
        after = self.live_state(binary, signer_dir, "verify_live")
        listed = str(after["channels"].get("beta", ""))
        if listed != version:
            raise StepFailure("verify_live", f"the live manifest lists beta {listed or 'nothing'}, not {version}")
        survived = self.game_data(self.live_manifest(after, "verify_live"), version)
        if survived != {"name": carried["name"], "size": carried["size"]}:
            raise StepFailure("verify_live", f"the carried game data {carried['name']} ({carried['size']} bytes) is "
                                             f"{'missing from' if survived is None else 'changed in'} the {version} release: {survived}")
        self.ok("verify_live", beta=listed, game_data=carried["name"])
        self.notify_published(tag)

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


def recent_notice(events: list[dict], step: str, digest: str, now: datetime.datetime) -> bool:
    """The newest `failure_notified` is for this step and error and under NOTICE_QUIET old. A time that cannot be read, or that
    lies in the future, does not suppress: a notice is then sent again."""
    notified = [e for e in events if e.get("event") == "failure_notified"]
    if not notified or (notified[-1].get("step"), notified[-1].get("error_sha256")) != (step, digest):
        return False
    try:
        sent = datetime.datetime.fromisoformat(notified[-1]["ts"])
    except (KeyError, TypeError, ValueError):
        return False
    if sent.tzinfo is None:
        return False
    return datetime.timedelta(0) <= now - sent < NOTICE_QUIET


def notify_failure(env: dict[str, str], runner: Runner, log_path: pathlib.Path) -> int:
    """Courier the last failed or refused event to Spritz, unless the same step and error was couriered under NOTICE_QUIET ago
    (a standing refusal repeats on every 30-minute run). The courier is recorded as `failure_notified` and a skipped one as
    `failure_notice_suppressed`; a log that cannot take the record is a failure, never silence."""
    events = read_events(log_path)
    recorded = [e for e in events if e.get("event") in ("failed", "refused")]
    last = recorded[-1] if recorded else {}
    what = last.get("error") or ("missing credentials: " + ", ".join(last["missing"]) if last.get("missing") else "no failed event recorded")
    step = last.get("step", "start")
    digest = hashlib.sha256(str(what).encode("utf-8")).hexdigest()
    suppress = recent_notice(events, step, digest, datetime.datetime.now(datetime.timezone.utc))
    if not suppress:
        body = "quest-release-timer failed: tag %s at step %s: %s (log: %s)" % (
            last.get("tag") or "not chosen yet", step, str(what)[:300], log_path)
        result = runner(["courier", "send", "--to", "spritz", "--from", "quest-release-timer", "--msg", body], cwd=None, env=env)
        if result.returncode != 0:
            sys.stderr.write("quest-release-timer: the failure notice could not be sent\n")
            return 1
    log = None
    try:
        log = Log(log_path, last.get("tag") or "")
        log.event("failure_notice_suppressed" if suppress else "failure_notified", step=step, error_sha256=digest)
    except Failure as failure:
        sys.stderr.write(f"quest-release-timer: {failure}\n")
        return 1
    finally:
        if log is not None:
            log.close()
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
        timer.check_checkout()
        timer.check_tags()
        token = timer.cmd("token", [timer.gh, "auth", "token"]).stdout.strip()
        if not token:
            raise StepFailure("token", f"{timer.gh} auth token printed nothing")
        timer.secrets.append(token)
        timer.token = token
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
        failures = [e for e in history if e.get("event") == "failed"]
        if len(failures) >= MAX_FAILURES:
            if not any(e.get("event") == "skip_notified" for e in history):
                timer.notify_skip(tag, failures[-1])
                log.event("skip_notified")
            log.event("skipped_after_failures", failures=MAX_FAILURES)
            return 0
        timer.release(tag)
        log.event("done")
        return 0
    except Refusal as refusal:
        sys.stderr.write(f"quest-release-timer: {refusal.step}: {refusal}\n")
        if log is not None:
            _record(log, "refused", refusal.step, str(refusal))
        return 1
    except StepFailure as failure:
        sys.stderr.write(f"quest-release-timer: {failure.step}: {failure}\n")
        if log is not None:
            _record(log, "failed", failure.step, str(failure))
        return 1
    except Failure as failure:
        sys.stderr.write(f"quest-release-timer: {failure}\n")
        if log is not None and not str(failure).startswith("refused"):
            _record(log, "failed", "start", str(failure))
        return 1
    finally:
        if log is not None:
            log.close()


def _record(log: Log, event: str, step: str, error: str) -> None:
    try:
        log.event(event, step=step, error=error)
    except Failure as log_failure:
        sys.stderr.write(f"quest-release-timer: {log_failure}\n")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
