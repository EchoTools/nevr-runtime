#!/usr/bin/env python3
"""The Quest half of a release, run on a schedule or by hand by the maintainer who holds the Quest signing key (RELEASING.md,
"The Quest half of the train"; #1210). Usage: quest_release_timer.py [--tag vX.Y.Z | --stage-draft] | --probe | --tag vX.Y.Z --reset-failures

The store APK, the Quest signing key and the R2 credential stay with the maintainer who runs this, so no workflow builds the Quest APK.
The train (release-train.yml) publishes the GitHub release; this tool sees the new tag and finishes the release:

  checkout         the checkout this runs from is clean and HEAD is the freshly fetched origin/main, or it refuses
  tags             every local tag is on origin at the same commit (the release preflight's rule), or it refuses naming up to
                   ten tags with their short ids, counting the rest, and printing the quoted remove and restore commands (with
                   the full object id) for each tag it lists
  token, list      the newest published vX.Y.Z (or --tag) that is not done
  build_signer     the release signer, built from a snapshot of the main of the checkout QUEST_RELEASE_SIGNER_REPO names
  check_listed     the live manifest already lists the tag: no release build is made and the run goes on to the variant;
                   else the game data it carries
  worktree         a clean worktree at the tag, set up for a build
  build_slot       waits while two or more ninja processes run
  build_apk        `just release-quest-apk` in the shared build slice
  publish_apk      tools/publish_quest_apk.py: R2, fetched back, and the manifest claim
  dispatch, watch  the beta manifest publish, live, in the release-signer repository
  check_jobs       the jobs that publish all succeeded (a green run with a skipped job is a failure)
  verify_live      `verify-manifest --live` lists the tag on beta, and the manifest it verified lists the carried game data
  variant_*        the unlisted variant APK, below
  cleanup          the worktree, the signer snapshot and the variant scratch, by exact path

The draft stage (`--stage-draft`, which a scheduled run passes). The release is cut as a draft and its APK can be built before
it is published, so this stage takes the newest release that is a draft newer than the newest published one and carries its
loader asset uploaded (`choose_tag`); otherwise the newest published one, as without the flag. For a draft it does the
steps up to build_apk, then waits for the publication (`await_published`, a poll every 10 seconds for up to 45 minutes),
checks the APK's loader against the published asset (`verify_loader`) and that beta has not moved (`recheck_beta`), and
runs publish_apk, the dispatch and everything after them unchanged. Nothing reaches R2 while the release is a draft. A
draft that is deleted, is not published within 45 minutes, or fails before it is published is recorded as
`draft_abandoned` and left to the published path: its failures are not counted, and the run exits 0. `--probe` (a
scheduler's pre-check) makes one GitHub call and exits 0 when a run has work, 1 when it is idle and 255 on an error; it
tests whether a run holds the lock, without keeping it, and runs no step.

The variant. Every release also gets an unlisted second APK, nevr-runtime-<tag>-q15t-quest.apk, in the same run, built
from the release's own loader asset plus two plugin libraries from a private build and published beside the main APK. It
is optional: the environment may hold NEVR_VARIANT_DIR (the directory with the two libraries) and
NEVR_VARIANT_PINS (a `sha256sum`-format file with one line per library). With both unset a run logs one
`variant_not_configured` and does nothing else; with exactly one set, or with a pins file that has a missing, extra,
duplicate or malformed line, or with a library that is missing or whose sha256 differs from its pin, the variant fails
loudly (a failure, never a skip). The variant steps run after the main release is complete (published, listed and verified
live) and never before:

  variant_exists   (a listed tag only) the host's variant pair: 404 for the APK is absent (it is built and published); an
                   APK that is there (200, not HTML) is listed only when its `.sha256` is there too, is the one line
                   `<sha256>  <variant name>` and states the sha256 of the APK as the host serves it (`variant_listed`,
                   nothing built); a missing, malformed or disagreeing sidecar, and any other answer, is a failure that
                   names the files and rebuilds nothing, because an upload never replaces an object
  variant_pins     each library is copied into a private scratch directory (mode 0700, files 0600), the COPY is hashed
                   and compared with its pin, and the copies are what is packaged; the log names the file, its size and
                   its pin, never a directory
  variant_build    `just release-quest-variant-apk` in the same worktree, configure and build slice as the main build; for
                   a tag that was already listed the run makes the worktree and the configure itself. Its output goes to
                   the 0600 file <work>/variant-<tag>-build.log, kept when the step fails and removed when the variant is
                   done
  variant_publish  tools/publish_quest_apk.py --variant: R2 and the public host only, no signer, no manifest claim

A failure of a variant step carries the step, the program and its exit code, never a tail of the program's output (the
plugin check prints the plugin's own symbol names); variant_build's failure names the 0600 file.

A failing variant step is recorded as `failed` with its step name, exits 1 and
counts toward the same three tries, but the main release is by then complete and nothing of it is undone. `done` is written
only when the variant is done as well, so the next run (the tag is listed by then) builds only the variant. NEVR_VARIANT_DIR
and NEVR_VARIANT_PINS are never given to a command and their values are replaced by <redacted> in anything written. Every
`failed` and `refused` record carries `part`: "main" until the main release is complete (`main_complete` is written then, once,
and a run whose tag has it starts as "variant"), "variant"
after that; the record that the timer gave up on a tag says which part it was. An exception no step expected is recorded as a
`failed` of the step `unexpected` (its class and where, never its text), what the run made is removed, and the run exits 1.

A tag whose history has `main_complete` takes the variant-only path whatever beta lists (`variant_only`: no signer, no
check_listed, no dispatch), so a beta that moved never makes the main release run again. A run that does work holds
a non-blocking lock on <work>/timer.lock; one that finds it held records `busy` and exits 0 having done nothing.

Every step appends a JSON line to $XDG_STATE_HOME/nevr-runtime/quest-release-timer.jsonl; a log that cannot be written is
a failure, and that log is the record of each run. A failed step exits 1 and names itself, with the redacted tail of the failing command's stderr and of its stdout (not the stdout of curl, gh or the signer, which is remote content). A refusal
(`refused`: a dirty or stale checkout, a tag origin does not have) exits 1 and is not counted. A tag that has failed three times is not retried, and says so in a `skipped_after_failures` record
(with the last failed step and its part) before the run exits 0. `--tag vX.Y.Z --reset-failures` records `failures_reset` for that tag and does
nothing else; only the failures after the newest one count toward the three. The R2 credential goes only to publish_apk and
variant_publish, and the GitHub token only to publish_apk and the dispatch: every other command gets an environment without them. The environment is
the R2 credential (R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY), NEVR_RCLONE_REMOTE and RELEASE_PUBLIC_BASE; a missing one
is a refusal that names it. No value is printed or logged: every value read from the environment, and the GitHub token, is
replaced by <redacted> in anything written.
"""

import argparse
import datetime
import fcntl
import hashlib
import http.client
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.request
from collections.abc import Callable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from publish_quest_apk import FETCH_TIMEOUT_SECONDS, Failure, Log, NoRedirect, public_base, sha256_file  # noqa: E402 - the sibling tool's log, failure, fetch and hash
from release_preflight import commit_of, parse_ls_remote  # noqa: E402 - the preflight's reading of tags and its commit rule
from quest_release_loader import asset_name as loader_asset_name  # noqa: E402 - the loader asset's name, from the loader tool's one source

# QUEST_RELEASE_SIGNER_REPO is the path of a local clone of the release-signer repository (build_signer builds from its main).
REQUIRED_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "RELEASE_PUBLIC_BASE",
                "NEVR_RCLONE_REMOTE", "QUEST_RELEASE_SIGNER_REPO")
SECRET_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "NEVR_RCLONE_REMOTE")
# The optional variant: where the private libraries are and the file of their pins. They are redacted like a secret and
# never given to a command; the timer reads them itself.
VARIANT_ENV = ("NEVR_VARIANT_DIR", "NEVR_VARIANT_PINS")
VARIANT = "q15t"
VARIANT_LIBRARIES = ("libq15t.so", "libq15t_early.so")
PIN_LINE = re.compile(r"([0-9a-fA-F]{64})  (\S+)")  # `sha256sum` output: the digest, two spaces, the name
MANIFEST_PATH = "nevr-runtime/manifest.json"
USER_AGENT = "nevr-runtime-quest-release-timer"  # the release host answers 403 to the default agent
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")  # always fullmatch: `$` would pass a trailing newline
RUN_ID = re.compile(r"/actions/runs/([0-9]+)")
RELEASE_SLUG = "EchoTools/nevr-runtime"
RELEASE_LISTING = f"repos/{RELEASE_SLUG}/releases?per_page=10"  # the REST list a draft is chosen from (`gh api`)
SIGNER_SLUG = "thesprockee/release-signer"
PUBLISHING_JOBS = ("Commit the manifest record", "Publish to R2", "A run that published nothing is red")
MAX_FAILURES = 3
# The probe's clock (`--probe`, a scheduler's pre-check). A counted failure holds its tag back for RETRY_SPACING, and a
# run that has not started for HEARTBEAT is asked for even with nothing to do, so the run history shows the timer is alive.
RETRY_SPACING = datetime.timedelta(minutes=30)
HEARTBEAT = datetime.timedelta(minutes=30)
PROBE_RECORD_SPACING = datetime.timedelta(minutes=10)  # the probe writes its decision when it changes, else this often
# A draft's APK is built while the draft waits for its publication: polled every PUBLISH_POLL_SECONDS, given up after
# PUBLISH_WAIT_SECONDS. The signer's upload accepts only a published release, so a draft that never publishes uploads nothing.
PUBLISH_POLL_SECONDS = 10
PUBLISH_WAIT_SECONDS = 2700
GAME_DATA_PLATFORM = "android-arm64"  # the Quest half of a release carries the game-data entry of this platform only
SLOT_WAITS = 60
SLOT_WAIT_SECONDS = 60
TAIL = 2000
APK_LIMIT = 4 << 30  # the most the variant APK check reads from the host
SIDECAR_LIMIT = 4096
SIDECAR_LINE = re.compile(r"([0-9a-f]{64})  (\S+)\n?")  # what `sha256sum` writes, one line
READ_CHUNK = 1 << 20
SCRATCH_MARKER = ".made-by-quest-release-timer"  # in the variant scratch directory this timer makes, and only in it
SIGNER_BINARY = "release-signer"  # what build_signer builds; its `verify-manifest --live` prints the live manifest on stdout
# Programs whose stdout is remote content, never a log field (docs/standards/logging.md, Rule 4).
REMOTE_OUTPUT_PROGRAMS = frozenset({"curl", "gh", SIGNER_BINARY})
MAX_TAGS_LISTED = 10
LOCAL_TAGS_FORMAT = "--format=%(refname:strip=2)\t%(objectname)\t%(*objectname)"  # release_preflight.local_tags' format

Runner = Callable[..., subprocess.CompletedProcess]


class StepFailure(Failure):
    def __init__(self, step: str, message: str) -> None:
        super().__init__(message)
        self.step = step


class Refusal(StepFailure):
    """A precondition that is not met (a dirty checkout, a checkout not at origin/main): logged as `refused`, never counted."""


class DraftAbandoned(Exception):
    """A draft that will not be published (deleted while it waited, or not published within PUBLISH_WAIT_SECONDS): logged as
    `draft_abandoned` with its reason, never counted, and the run exits 0. Nothing of the draft reaches R2."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


def work_root(env: dict[str, str]) -> pathlib.Path:
    """The scratch directory of the timer (QUEST_RELEASE_WORK_ROOT, else beside the log under $XDG_STATE_HOME); the lock the
    probe tests lives in it."""
    state = env.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return pathlib.Path(env.get("QUEST_RELEASE_WORK_ROOT") or pathlib.Path(state) / "nevr-runtime" / "quest-release-timer-work")


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
            record = json.loads(line)
        except ValueError:
            continue
        if isinstance(record, dict):  # a line that is JSON but not an object is not an event
            events.append(record)
    return events


def newest_published(listing: str) -> str:
    try:
        releases = json.loads(listing)
    except ValueError as error:
        raise StepFailure("list", f"the release list is not JSON: {error}") from error
    tags = [r["tagName"] for r in releases if not r.get("isDraft") and TAG.fullmatch(r.get("tagName", ""))]
    if not tags:
        raise StepFailure("list", "no published vX.Y.Z release")
    return max(tags, key=lambda t: tuple(int(n) for n in t[1:].split(".")))


class Timer:
    def __init__(self, env: dict[str, str], runner: Runner, sleep: Callable[[float], None], log: Log) -> None:
        self.env, self.runner, self.sleep, self.log = env, runner, sleep, log
        self.secrets = [env[name] for name in (*SECRET_ENV, *VARIANT_ENV) if env.get(name)]
        self.r2 = {name: env[name] for name in SECRET_ENV if env.get(name)}
        self.token = ""
        self.gh = env.get("GH_BIN", "gh")
        self.repo = pathlib.Path(env.get("QUEST_RELEASE_REPO") or pathlib.Path(__file__).resolve().parents[1])
        self.signer_repo = pathlib.Path(env["QUEST_RELEASE_SIGNER_REPO"])  # REQUIRED_ENV: main() refuses a run without it
        self.work = work_root(env)
        # What every command gets. The R2 credential and the GitHub token are added by the one or two steps that use them.
        self.child = {k: v for k, v in env.items() if k not in SECRET_ENV and k not in VARIANT_ENV and k != "GH_TOKEN"}
        self.part = "main"  # what a failure now belongs to: the main release, or the variant that follows it
        self.main_done = False  # the tag's history has `main_complete`: only the variant is left, whatever beta lists
        self.scratch_made = False  # this run made the variant scratch directory
        self.stage = "published"  # "draft" when this run builds a draft's APK and publishes it once the release is published

    def redact(self, text: str) -> str:
        for secret in self.secrets:
            text = text.replace(secret, "<redacted>")
        return text

    def cmd(self, step: str, argv: list[str], cwd: pathlib.Path | str | None = None, extra: dict | None = None,
            ok_codes: tuple[int, ...] = (0,), quiet: bool = False,
            output_file: pathlib.Path | None = None) -> subprocess.CompletedProcess:
        """Run a command. A failure carries the program, its exit code and the redacted tails of its output, unless `quiet`
        (the program and the exit code only) or `output_file` (the whole output goes to that 0600 file, whose path the
        failure names): a variant step's output can hold what the private build prints."""
        env = {**self.child, **(extra or {})}
        result = self.runner(argv, cwd=str(cwd) if cwd else None, env=env)
        if output_file is not None:
            self.keep_output(step, output_file, result)
        if result.returncode not in ok_codes:
            name = pathlib.Path(argv[0]).name
            if output_file is not None:
                raise StepFailure(step, f"{name} exited {result.returncode}; its full output is in {output_file}")
            if quiet:
                raise StepFailure(step, f"{name} exited {result.returncode}")
            raise StepFailure(step, f"{name} exited {result.returncode}:{self.tails(argv[0], result)}")
        return result

    def keep_output(self, step: str, path: pathlib.Path, result: subprocess.CompletedProcess) -> None:
        """The command's whole output, redacted, in a file that is 0600 from the moment it is opened (a run replaces it)."""
        try:
            # A previous file (or a link planted in its place) is removed, and the new one is made exclusively, 0600, without
            # following a link, so nothing is written through a path somebody else controls.
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8", errors="replace") as handle:
                handle.write(f"exit {result.returncode}\n--- stderr ---\n{self.redact(result.stderr or '')}\n"
                             f"--- stdout ---\n{self.redact(result.stdout or '')}\n")
        except OSError as error:
            raise StepFailure(step, f"cannot write the output file: {error.strerror}") from error

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
    def game_data(manifest: dict, version: str, step: str) -> dict | None:
        """The release's one game-data entry of the Quest platform. A release lists one game-data artifact per platform, so
        an entry of another platform (the PC data) is never this timer's and never decides anything here. Zero Quest entries
        is None; two is a failure, never a pick of the first."""
        release = manifest.get("releases", {}).get(version, {})
        found = [a for a in release.get("artifacts", [])
                 if a.get("kind") == "game-data" and a.get("platform") == GAME_DATA_PLATFORM]
        if len(found) > 1:
            raise StepFailure(step, f"release {version} lists {len(found)} game-data artifacts for {GAME_DATA_PLATFORM} "
                                    f"({', '.join(str(a.get('name')) for a in found)}), not one")
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
            # The head is a compact summary that survives a cut of the error; the commands follow it. A tag name
            # is whatever git allows, `;`, `$(...)` and quotes included, so every name and id is shell-quoted.
            shown, rest = offenders[:MAX_TAGS_LISTED], max(0, len(offenders) - MAX_TAGS_LISTED)
            head = ("local tags origin lacks or has elsewhere: "
                    + ", ".join(f"{shlex.quote(name)} ({obj[:12]})" for name, obj, _ in shown)
                    + (f", and {rest} more" if rest else "") + f" [{len(offenders)} of {len(local)}]")
            commands = [f"{shlex.quote(name)} ({why}): remove: git tag -d -- {shlex.quote(name)}; rollback: "
                        f"git update-ref {shlex.quote('refs/tags/' + name)} {shlex.quote(obj)}" for name, obj, why in shown]
            raise Refusal(step, " | ".join([head, *commands]))
        self.ok(step, count=len(local))

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

    def await_published(self, tag: str) -> None:
        """A draft's APK waits here for the release to be published. Each poll reads the release's `isDraft` with no credential
        (the waiting steps get none). A poll that finds it published returns; a view that fails means the release is gone
        (DraftAbandoned "gone"); PUBLISH_WAIT_SECONDS of polls without publication is DraftAbandoned "wait_expired". The sleep
        follows each poll that still sees a draft, so a publication at the Nth poll is N-1 sleeps."""
        step = "await_published"
        argv = [self.gh, "release", "view", tag, "-R", RELEASE_SLUG, "--json", "isDraft"]
        for poll in range(PUBLISH_WAIT_SECONDS // PUBLISH_POLL_SECONDS):
            view = self.runner(argv, cwd=None, env=self.child)
            if view.returncode != 0:
                raise DraftAbandoned("gone")
            try:
                draft = json.loads(view.stdout)["isDraft"]
            except (ValueError, KeyError, TypeError) as error:
                raise StepFailure(step, "the release view is unreadable") from error
            if not isinstance(draft, bool):
                raise StepFailure(step, "the release view's isDraft is not a boolean")
            if not draft:
                self.ok(step, polls=poll + 1)
                return
            self.sleep(PUBLISH_POLL_SECONDS)
        raise DraftAbandoned("wait_expired")

    def verify_loader(self, tag: str, worktree: pathlib.Path) -> None:
        """The APK was built before the publication, from the loader the draft carried. Before anything reaches R2 its loader is
        held to the digest of the PUBLISHED loader asset: a loader that changed in between is a failure, and nothing is uploaded."""
        step = "verify_loader"
        name = loader_asset_name(tag[1:])
        listing = self.cmd(step, [self.gh, "release", "view", tag, "-R", RELEASE_SLUG, "--json", "assets"]).stdout
        try:
            assets = json.loads(listing)["assets"]
        except (ValueError, KeyError, TypeError) as error:
            raise StepFailure(step, "the published release's assets are unreadable") from error
        found = [a for a in assets if isinstance(a, dict) and a.get("name") == name and a.get("state") == "uploaded"]
        digest = str(found[0].get("digest", "")) if len(found) == 1 else ""
        if not digest.startswith("sha256:"):
            raise StepFailure(step, f"the published release has no uploaded {name} with a sha256 digest")
        apk = worktree / "build" / "quest-release" / tag / f"nevr-runtime-{tag}-quest.apk"
        self.cmd(step, ["python3", "-I", str(worktree / "tools" / "quest_release_loader.py"), "verify-apk", "--apk", str(apk),
                        "--sha256", digest.removeprefix("sha256:")], worktree)
        self.ok(step, asset=name)

    def recheck_beta(self, binary: pathlib.Path, signer_dir: pathlib.Path, beta: str) -> None:
        """Beta is read again after the wait. A beta that moved while the draft was built means the decision check_listed made
        (the game data carried from `beta`) no longer holds, so nothing is published or dispatched."""
        step = "check_listed"
        now = self.live_beta(binary, signer_dir, step)
        if now != beta:
            raise StepFailure(step, f"the live manifest's beta moved from {beta or 'none'} to {now or 'none'} while the release "
                                    "was a draft; nothing was published")
        self.ok(step, beta_rechecked=beta)

    def release(self, tag: str) -> None:
        version = tag[1:]
        signer_dir = self.work / f"release-signer-{tag}"
        tarball = self.work / f"release-signer-{tag}.tar"
        extra_file = self.work / f"extra-{tag}.json"
        worktree = self.work / tag
        made = {"worktree": False}
        try:
            self._release_steps(tag, version, worktree, signer_dir, tarball, extra_file, made)
        except Exception:  # a StepFailure, or anything a step did not expect: what this run made is removed either way
            self.cleanup(worktree if made["worktree"] else None, signer_dir, tarball, extra_file, raising=False,
                         scratch=self.variant_scratch(tag) if self.scratch_made else None)
            raise
        self.cleanup(worktree if made["worktree"] else None, signer_dir, tarball, extra_file, raising=True,
                     scratch=self.variant_scratch(tag) if self.scratch_made else None)

    def _release_steps(self, tag: str, version: str, worktree: pathlib.Path, signer_dir: pathlib.Path,
                       tarball: pathlib.Path, extra_file: pathlib.Path, made: dict) -> None:
        self.work.mkdir(parents=True, exist_ok=True)
        readme = self.work / "README.md"
        if not readme.exists():
            readme.write_text("Scratch of tools/quest_release_timer.py (the Quest half of a release). "
                              "Each run removes what it makes here when it ends, except the output file of a\n"
                              "failed variant build, which stays until the variant is done.\n",
                              encoding="utf-8")
        if self.main_done:
            # The main release was published, listed and verified by an earlier run: only the variant is left, and it
            # does not depend on what beta lists now (a rollback, a hand publish and a hand `--tag` of an older release move beta).
            self.log.event("variant_only", reason="the main release is complete; beta is not consulted")
            self.finish_variant(tag, worktree, made, None)
            return
        binary = self.build_signer(signer_dir, tarball)
        before = self.live_state(binary, signer_dir, "check_listed")
        beta = str(before["channels"].get("beta", ""))
        if beta == version:
            self.log.event("already_listed", beta=version)
            self.ok("check_listed", listed=True)
            self.mark_main_complete(tag)
            self.finish_variant(tag, worktree, made, None)
            return
        carried = self.game_data(self.live_manifest(before, "check_listed"), beta, "check_listed")
        if carried is None:
            raise StepFailure("check_listed", f"the live manifest's beta release {beta or 'none'} lists no game-data "
                                              "artifact to carry into the new release")
        self.ok("check_listed", listed=False, carried=carried["name"], carried_size=carried["size"])

        self.make_worktree(tag, worktree, made)
        build_env = self.prepare_build(worktree)
        self.cmd("build_apk", ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G",
                               "just", "release-quest-apk", tag], worktree, build_env)
        self.ok("build_apk")
        if self.stage == "draft":
            # The APK was built while the release was a draft. Nothing below runs before the release is published, and the
            # loader and beta are held to what they were when the build started (RELEASING.md, "The Quest half of the train").
            self.await_published(tag)
            self.part = "main"
            self.verify_loader(tag, worktree)
            self.recheck_beta(binary, signer_dir, beta)

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
        survived = self.game_data(self.live_manifest(after, "verify_live"), version, "verify_live")
        if survived != {"name": carried["name"], "size": carried["size"]}:
            raise StepFailure("verify_live", f"the carried game data {carried['name']} ({carried['size']} bytes) is "
                                             f"{'missing from' if survived is None else 'changed in'} the {version} release: {survived}")
        self.ok("verify_live", beta=listed, game_data=carried["name"])
        self.mark_main_complete(tag)
        self.finish_variant(tag, worktree, made, build_env)

    def mark_main_complete(self, tag: str) -> None:
        """The main release is published, listed and verified live: from here a failure is the variant's, and a later run
        starts as the variant. Written once per tag."""
        if not any(e.get("tag") == tag and e.get("event") == "main_complete" for e in read_events(self.log.path)):
            self.log.event("main_complete")
        self.part = "variant"

    def make_worktree(self, tag: str, worktree: pathlib.Path, made: dict) -> None:
        """A clean worktree at the tag, set up for a build."""
        repo = str(self.repo)
        self.cmd("worktree", ["git", "-C", repo, "fetch", "origin", "tag", tag])
        commit = self.cmd("worktree", ["git", "-C", repo, "rev-parse", f"{tag}^{{commit}}"]).stdout.strip()
        self.cmd("worktree", ["git", "-C", repo, "worktree", "add", "--detach", str(worktree), commit])
        made["worktree"] = True
        self.cmd("worktree", ["just", "worktree-setup"], worktree)
        self.ok("worktree", commit=commit)

    def prepare_build(self, worktree: pathlib.Path) -> dict:
        """Wait for a build slot and configure the worktree; the environment every build command in it gets."""
        self.wait_for_build_slot()
        self.ok("build_slot")
        build_env = {"VCPKG_MAX_CONCURRENCY": "6", "CMAKE_BUILD_PARALLEL_LEVEL": "6", "GH_BIN": self.gh}
        # A new tag's worktree has no vcpkg install yet (worktree-setup seeds the build tree only at a commit the main
        # checkout built), and release-quest-apk needs the nlohmann/json headers that install provides.
        self.cmd("configure", ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G",
                               "just", "configure"], worktree, build_env)
        self.ok("configure")
        return build_env

    def variant_scratch(self, tag: str) -> pathlib.Path:
        return self.work / f"variant-{tag}"

    def sweep_scratch(self, tag: str) -> None:
        """Remove the private copies a killed run left: the scratch directory of the tag, when it carries this timer's marker.
        A link, or a directory without the marker, is not ours and is never touched. The marker goes last (the files first,
        then the marker, then the directory), so a sweep that fails part way leaves a directory that is still marked and is
        retried by the next run."""
        scratch = self.variant_scratch(tag)
        marker = scratch / SCRATCH_MARKER
        if scratch.is_symlink() or not scratch.is_dir() or marker.is_symlink() or not marker.is_file():
            return
        try:
            for child in scratch.iterdir():
                if child.name == SCRATCH_MARKER:
                    continue
                if child.is_dir() and not child.is_symlink():
                    shutil.rmtree(child)
                else:
                    child.unlink()
            marker.unlink()
            scratch.rmdir()
        except OSError as error:
            raise StepFailure("cleanup", f"cannot remove the scratch a killed run left: {error.strerror}") from error
        self.log.event("variant_scratch_removed", reason="left by a run that did not finish")

    @staticmethod
    def variant_apk_name(tag: str) -> str:
        return f"nevr-runtime-{tag}-{VARIANT}-quest.apk"

    def variant_settings(self) -> tuple[pathlib.Path, pathlib.Path] | None:
        """The directory and the pins file, None when the variant is not configured (both unset or empty). Exactly one set is
        a failure: a half-configured variant is never silently skipped."""
        values = [self.env.get(name, "") for name in VARIANT_ENV]
        if not any(values):
            return None
        unset = [name for name, value in zip(VARIANT_ENV, values) if not value]
        if unset:
            raise StepFailure("variant_pins", f"{unset[0]} is not set and the other variable of the pair is: the variant "
                                              f"needs {' and '.join(VARIANT_ENV)}")
        return pathlib.Path(values[0]), pathlib.Path(values[1])

    def read_pins(self, pins_path: pathlib.Path) -> dict[str, str]:
        """{library: sha256} from a `sha256sum`-format file with exactly one line per library. Every problem is named once, by
        line number or by library, and never by the path or by text from the file."""
        step = "variant_pins"
        try:
            text = pins_path.read_text(encoding="utf-8")
        except OSError as error:
            raise StepFailure(step, f"the pins file cannot be read: {error.strerror}") from error
        except ValueError as error:
            raise StepFailure(step, "the pins file is not UTF-8 text") from error
        pins: dict[str, str] = {}
        problems = []
        for number, line in enumerate(text.splitlines(), 1):
            found = PIN_LINE.fullmatch(line)
            if not found:
                problems.append(f"pins line {number} is not '<sha256>  <name>'")
            elif found.group(2) not in VARIANT_LIBRARIES:
                problems.append(f"pins line {number} names no variant library")
            elif found.group(2) in pins:
                problems.append(f"pins name {found.group(2)} twice (line {number})")
            else:
                pins[found.group(2)] = found.group(1).lower()
        problems += [f"no pin for {name}" for name in VARIANT_LIBRARIES if name not in pins]
        if problems:
            raise StepFailure(step, "; ".join(problems))
        return pins

    def variant_pins(self, tag: str, directory: pathlib.Path, pins_path: pathlib.Path) -> list[pathlib.Path]:
        """Copy each library into a private scratch directory, hash the COPY, hold it to its pin, and return the copies, which
        are what is packaged: a file that changes after this is not what is built. A failure names the file."""
        step = "variant_pins"
        pins = self.read_pins(pins_path)
        scratch = self.variant_scratch(tag)
        if ":" in str(scratch):
            raise StepFailure(step, "the scratch directory's path holds a colon, which separates the libraries in the recipe")
        try:
            scratch.mkdir(mode=0o700)  # a directory of that name that is not this timer's is never removed: this fails
            self.scratch_made = True
            scratch.chmod(0o700)
            os.close(os.open(scratch / SCRATCH_MARKER, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600))
        except OSError as error:
            raise StepFailure(step, f"cannot make the scratch directory: {error.strerror}") from error
        copies = []
        for name in VARIANT_LIBRARIES:
            copy = scratch / name
            try:
                with (directory / name).open("rb") as source:
                    with os.fdopen(os.open(copy, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as target:
                        shutil.copyfileobj(source, target)
                copy.chmod(0o600)
                digest, size = sha256_file(copy)
            except FileNotFoundError as error:
                raise StepFailure(step, f"{name} is missing from the variant directory") from error
            except OSError as error:
                raise StepFailure(step, f"{name} cannot be read: {error.strerror}") from error
            if digest != pins[name]:
                raise StepFailure(step, f"{name} does not match its pin: its sha256 is {digest}, the pin is {pins[name]}")
            self.log.event("variant_library", name=name, size=size, sha256=pins[name])
            copies.append(copy)
        self.ok(step, count=len(copies))
        return copies

    def get_object(self, step: str, url: str, limit: int, keep: bool = False) -> tuple[int, str, str, bytes, bool]:
        """GET `url` without following a redirect: (status, content type, sha256 of the body as it streamed in, the body when
        `keep`, whether the read was provably complete). A status other than 200 is returned for the caller to judge, with no
        body read; a host that does not answer, or a body over `limit`, is a failure. Complete means the bytes received are
        the declared Content-Length and the host sent them unencoded: a body that ends with the connection, a chunked one or an
        encoded one cannot be called the file, and a digest that disagrees with it proves nothing."""
        file = url.rsplit("/", 1)[-1]
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept-Encoding": "identity"})
        try:
            response = urllib.request.build_opener(NoRedirect).open(request, timeout=FETCH_TIMEOUT_SECONDS)
        except urllib.error.HTTPError as error:
            return error.code, "", "", b"", False
        except (OSError, http.client.HTTPException, ValueError) as error:  # a bad status line, a URL the library rejects
            raise StepFailure(step, f"{file} on the host: no answer ({error.__class__.__name__})") from error
        digest = hashlib.sha256()
        kept = bytearray()
        size = 0
        with response:
            content_type = response.headers.get("Content-Type", "")
            if response.status != 200 or content_type.lower().startswith("text/html"):
                return response.status, content_type, "", b"", False
            declared = response.headers.get("Content-Length", "").strip()
            encoding = response.headers.get("Content-Encoding", "").strip().lower()
            try:
                while chunk := response.read(READ_CHUNK):
                    size += len(chunk)
                    if size > limit:
                        raise StepFailure(step, f"{file} on the host: more than the expected {limit} bytes")
                    digest.update(chunk)
                    if keep:
                        kept += chunk
            except (OSError, http.client.HTTPException, ValueError) as error:  # IncompleteRead, a bad chunk size
                received = size + len(getattr(error, "partial", b""))  # what the failing read had already taken in
                raise StepFailure(step, f"{file} on the host: the answer broke off ({error.__class__.__name__}), "
                                        f"received {received} bytes") from error
            if declared.isascii() and declared.isdigit() and size != int(declared):
                raise StepFailure(step, f"{file} on the host: the answer broke off (received {size} of {int(declared)} bytes)")
        complete = declared.isascii() and declared.isdigit() and size == int(declared) and encoding in ("", "identity")
        return 200, content_type, digest.hexdigest(), bytes(kept), complete

    def variant_exists(self, tag: str) -> bool:
        """Whether the host has the variant PAIR. 404 for the APK is absent. An APK that is there is listed only when its
        `.sha256` is there, is one `<sha256>  <variant name>` line and states the sha256 of the APK as the host serves it;
        any other state of the pair, and any other answer, is a failure that names the files and the check, never a guess.
        Nothing is rebuilt for a failed pair: an upload never replaces an object, so a rebuild could not repair it."""
        step = "variant_exists"
        name = self.variant_apk_name(tag)
        sidecar = f"{name}.sha256"
        try:
            base = public_base(self.env)
        except Failure as failure:
            raise StepFailure(step, str(failure)) from failure

        def answered(file: str, status: int, content_type: str) -> StepFailure:
            note = " (a redirect is not followed)" if 300 <= status < 400 else ""
            what = "with an HTML content type, which is not the file" if status == 200 else "instead of the file"
            return StepFailure(step, f"{file} on the host: answered {status} {what}{note}")

        status, content_type, apk_digest, _, apk_complete = self.get_object(step, f"{base}/nevr-runtime/{tag}/{name}", APK_LIMIT)
        if status == 404:
            return False
        if status != 200 or content_type.lower().startswith("text/html"):
            raise answered(name, status, content_type)
        repair = "; nothing is rebuilt or replaced, see RELEASING.md (The Quest half of the train, the variant pair) for the repair"
        broken = f"{name} is on the host but {sidecar} does not match it: "
        status, content_type, _, body, sidecar_complete = self.get_object(step, f"{base}/nevr-runtime/{tag}/{sidecar}",
                                                                           SIDECAR_LIMIT, keep=True)
        unconfirmed = (f"{name} is on the host but {sidecar} does not match it, and we could not confirm the answer was complete "
                       "(no declared length, or an encoded or chunked body), so nothing is concluded; the next run asks again")
        if status == 404:
            raise StepFailure(step, f"{broken}{sidecar} is missing (a half-published pair){repair}")
        if status != 200 or content_type.lower().startswith("text/html"):
            raise StepFailure(step, f"{broken}{answered(sidecar, status, content_type)}{repair}")
        try:
            found = SIDECAR_LINE.fullmatch(body.decode("utf-8"))
        except UnicodeDecodeError:
            found = None
        if not found:
            if not sidecar_complete:
                raise StepFailure(step, unconfirmed)
            raise StepFailure(step, f"{broken}{sidecar} is malformed (not one '<sha256>  <name>' line){repair}")
        if found.group(2) != name:
            if not sidecar_complete:
                raise StepFailure(step, unconfirmed)
            raise StepFailure(step, f"{broken}{sidecar} names a different file ({len(found.group(2).encode())} bytes){repair}")
        if found.group(1) != apk_digest:
            if not (apk_complete and sidecar_complete):
                raise StepFailure(step, unconfirmed)
            raise StepFailure(step, f"{broken}{sidecar} states sha256 {found.group(1)}, the APK on the host has {apk_digest}{repair}")
        return True

    def variant_build_log(self, tag: str) -> pathlib.Path:
        return self.work / f"variant-{tag}-build.log"

    def forget_build_log(self, tag: str) -> None:
        """The variant is done: its build output has nothing left to explain."""
        try:
            self.variant_build_log(tag).unlink(missing_ok=True)
        except OSError as error:
            raise StepFailure("cleanup", f"cannot remove the variant build output: {error.strerror}") from error

    def finish_variant(self, tag: str, worktree: pathlib.Path, made: dict, build_env: dict | None) -> None:
        """The variant, after the main release is complete. `build_env` is the environment of the build the main release just
        made, whose worktree and configure the variant reuses; None when the tag was already listed, which asks the host first
        and, when the variant is absent, makes its own worktree and configure. A failure here never undoes the release."""
        settings = self.variant_settings()
        if settings is None:
            self.log.event("variant_not_configured", reason="NEVR_VARIANT_DIR and NEVR_VARIANT_PINS are unset, so no variant "
                                                            "APK is built for this release")
            return
        if build_env is None:
            listed = self.variant_exists(tag)
            self.ok("variant_exists", listed=listed, apk=self.variant_apk_name(tag))
            if listed:
                self.log.event("variant_listed", apk=self.variant_apk_name(tag))
                self.forget_build_log(tag)
                return
        copies = self.variant_pins(tag, *settings)
        if build_env is None:
            self.make_worktree(tag, worktree, made)
            build_env = self.prepare_build(worktree)
        self.cmd("variant_build", ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G",
                                   "just", "release-quest-variant-apk", tag, ":".join(str(copy) for copy in copies)],
                 worktree, build_env, output_file=self.variant_build_log(tag))
        self.ok("variant_build", apk=self.variant_apk_name(tag))
        # Only the R2 credential: the variant has no signer and no manifest claim. The publish tool's own log has its reasons.
        self.cmd("variant_publish", ["python3", "tools/publish_quest_apk.py", tag, "--variant", VARIANT], worktree, dict(self.r2),
                 quiet=True)
        self.ok("variant_publish", apk=self.variant_apk_name(tag))
        self.forget_build_log(tag)

    def cleanup(self, worktree: pathlib.Path | None, signer_dir: pathlib.Path, tarball: pathlib.Path,
                extra_file: pathlib.Path, raising: bool, scratch: pathlib.Path | None = None) -> None:
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
            if scratch is not None and scratch.exists():
                shutil.rmtree(scratch)
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


def main(argv: list[str], env: dict[str, str], runner: Runner = subprocess_runner,
         sleep: Callable[[float], None] = time.sleep) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", help="release this vX.Y.Z instead of the newest published one")
    parser.add_argument("--reset-failures", action="store_true",
                        help="with --tag: record that the failures of that tag so far no longer count toward the three tries")
    parser.add_argument("--stage-draft", action="store_true",
                        help="also take a draft release: its APK is built while it waits to be published (a scheduled run's flag)")
    parser.add_argument("--probe", action="store_true",
                        help="one GitHub call: exit 0 when a run has work, 1 when idle, 255 on an error (a scheduler's pre-check)")
    args = parser.parse_args(argv)
    path = state_log_path(env)
    if args.probe:
        if args.tag or args.stage_draft or args.reset_failures:
            sys.stderr.write("quest-release-timer: --probe takes no other option\n")
            return 1
        return probe(env, runner, path, datetime.datetime.now(datetime.timezone.utc))
    if args.tag and not TAG.fullmatch(args.tag):
        sys.stderr.write(f"quest-release-timer: {args.tag} is not a release tag (vX.Y.Z)\n")
        return 1
    if args.reset_failures and not args.tag:
        sys.stderr.write("quest-release-timer: --reset-failures needs --tag vX.Y.Z\n")
        return 1
    if args.stage_draft and (args.tag or args.reset_failures):
        sys.stderr.write("quest-release-timer: --stage-draft chooses its tag itself: it takes no --tag or --reset-failures\n")
        return 1
    log = None
    timer = None
    lock = None
    try:
        log = Log(path, args.tag or "")
        log.event("start")
        if args.reset_failures:
            cleared = len(counted_failures([e for e in read_events(path) if e.get("tag") == args.tag]))
            log.event("failures_reset", cleared=cleared)
            return 0
        missing = [name for name in REQUIRED_ENV if not env.get(name)]
        if missing:
            log.event("refused", missing=missing, part="main")
            raise Failure(f"refused: set {', '.join(missing)}")
        timer = Timer(env, runner, sleep, log)
        lock = take_lock(timer.work)
        if lock is None:
            log.event("busy", reason="another run holds the lock on timer.lock in the work directory")
            sys.stderr.write("quest-release-timer: busy: another run holds the lock; nothing was done\n")
            return 0
        timer.check_checkout()
        timer.check_tags()
        token = timer.cmd("token", [timer.gh, "auth", "token"]).stdout.strip()
        if not token:
            raise StepFailure("token", f"{timer.gh} auth token printed nothing")
        timer.secrets.append(token)
        timer.token = token
        log.event("step_ok", step="token")
        tag = args.tag
        stage = "published"
        if not tag and args.stage_draft:
            tag, stage = choose_tag(timer.cmd("list", [timer.gh, "api", RELEASE_LISTING]).stdout, read_events(path))
            log.event("step_ok", step="list", newest=tag, stage=stage)
        elif not tag:
            tag = newest_published(timer.cmd("list", [timer.gh, "release", "list", "--repo", RELEASE_SLUG, "--json",
                                                      "tagName,isDraft", "--limit", "20"]).stdout)
            log.event("step_ok", step="list", newest=tag)
        log.base["tag"] = tag
        history = [e for e in read_events(path) if e.get("tag") == tag]
        timer.main_done = any(e.get("event") == "main_complete" for e in history)
        timer.stage = stage
        # Before the sweep, so a failure of it carries the right part. A draft's failures are "draft" until it is published.
        timer.part = "draft" if stage == "draft" else ("variant" if timer.main_done else "main")
        timer.sweep_scratch(tag)
        if any(e.get("event") == "done" for e in history):
            log.event("nothing_to_do", reason="done")
            return 0
        failures = counted_failures(history)
        if len(failures) >= MAX_FAILURES:
            last = failures[-1]
            failing = str(last.get("step", "unknown"))
            # A record from before `part` is read by its step: a variant step's failure is the variant's.
            part = str(last.get("part") or ("variant" if failing.startswith("variant_") else "main"))
            log.event("skipped_after_failures", failures=MAX_FAILURES, last_step=failing, last_part=part)
            return 0
        timer.release(tag)
        log.event("done")
        return 0
    except DraftAbandoned as abandoned:
        # The draft's worktree and scratch were removed by release(). Exit 0: nothing is counted.
        sys.stderr.write(f"quest-release-timer: draft abandoned: {abandoned.reason}\n")
        log.event("draft_abandoned", reason=abandoned.reason)
        return 0
    except Refusal as refusal:
        sys.stderr.write(f"quest-release-timer: {refusal.step}: {refusal}\n")
        if log is not None:
            _record(log, "refused", refusal.step, str(refusal), part_of(timer))
        return 1
    except StepFailure as failure:
        sys.stderr.write(f"quest-release-timer: {failure.step}: {failure}\n")
        if log is not None:
            _record(log, "failed", failure.step, str(failure), part_of(timer))
            if draft_pending(timer):
                log.event("draft_abandoned", reason=f"failed:{failure.step}")
                return 0
        return 1
    except Failure as failure:
        sys.stderr.write(f"quest-release-timer: {failure}\n")
        if log is not None and not str(failure).startswith("refused"):
            _record(log, "failed", "start", str(failure), part_of(timer))
        return 1
    except Exception as error:  # noqa: BLE001 - the last resort: whatever a step did not expect is a counted failure, not a traceback
        # The class and where it happened, never its text: that may hold a path, a secret or text from the host.
        frames = traceback.extract_tb(error.__traceback__)
        mine = [frame for frame in frames if frame.filename == __file__] or frames
        where = f"{pathlib.Path(mine[-1].filename).name}:{mine[-1].lineno}" if mine else "unknown"
        message = f"{error.__class__.__name__} at {where}"
        sys.stderr.write(f"quest-release-timer: unexpected: {message}\n")
        if log is not None:
            _record(log, "failed", "unexpected", message, part_of(timer))
            if draft_pending(timer):
                log.event("draft_abandoned", reason="failed:unexpected")
                return 0
        return 1
    finally:
        if lock is not None:
            os.close(lock)  # the lock is released with the descriptor, however the run ended
        if log is not None:
            log.close()


def take_lock(work: pathlib.Path) -> int | None:
    """A non-blocking exclusive lock on <work>/timer.lock for the life of the process, so two runs (the timer's and a hand
    `--tag` run) never work at once: one would sweep the other's scratch or publish twice. None when it is held."""
    work.mkdir(parents=True, exist_ok=True)
    fd = os.open(work / "timer.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        os.fchmod(fd, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        return None
    return fd


def part_of(timer: "Timer | None") -> str:
    """What a failure belongs to: the main release (also before there is a timer) or the variant."""
    return timer.part if timer is not None else "main"


def history_since_reset(history: list[dict]) -> list[dict]:
    """The events of one tag after its newest `failures_reset`: the failures before it no longer count."""
    for index in range(len(history) - 1, -1, -1):
        if history[index].get("event") == "failures_reset":
            return history[index + 1:]
    return history


def counted_failures(history: list[dict]) -> list[dict]:
    # A failure before a draft's publication is the draft's own (it abandons the draft, see draft_pending) and never one of the three.
    return [e for e in history_since_reset(history) if e.get("event") == "failed" and e.get("part") != "draft"]


_EPOCH = datetime.datetime.min.replace(tzinfo=datetime.timezone.utc)


def event_time(event: dict) -> datetime.datetime:
    """The record's own time. One that cannot be read counts as the oldest time, so it never holds back a retry or the heartbeat."""
    try:
        when = datetime.datetime.fromisoformat(str(event["ts"]))
    except (KeyError, ValueError):
        return _EPOCH
    return when if when.tzinfo else when.replace(tzinfo=datetime.timezone.utc)


def version_key(tag: str) -> tuple[int, ...]:
    return tuple(int(n) for n in tag[1:].split("."))


def loader_uploaded(release: dict, tag: str) -> bool:
    """The release's Quest loader asset, which build.yml attaches to the draft, is uploaded (GitHub's listing says so)."""
    name = loader_asset_name(tag[1:])
    assets = release.get("assets") or []
    return any(isinstance(a, dict) and a.get("name") == name and a.get("state") == "uploaded" for a in assets)


def choose_tag(listing: str, events: list[dict]) -> tuple[str, str]:
    """(tag, stage). The stage is "draft" for the newest draft that is newer than the newest published release, carries its
    uploaded loader asset and has no `draft_abandoned` in its history; otherwise the newest published release, "published".
    The listing is the REST list (`gh api repos/<slug>/releases`), whose fields are `tag_name` and `draft`."""
    try:
        releases = json.loads(listing)
    except ValueError as error:
        raise StepFailure("list", "the release list is not JSON") from error
    if not isinstance(releases, list):
        raise StepFailure("list", "the release list is not a list")
    releases = [r for r in releases if isinstance(r, dict) and TAG.fullmatch(str(r.get("tag_name", "")))]
    published = [r["tag_name"] for r in releases if not r.get("draft")]
    newest = max(published, key=version_key) if published else None
    abandoned = {e.get("tag") for e in events if e.get("event") == "draft_abandoned"}
    drafts = [r["tag_name"] for r in releases
              if r.get("draft") and r["tag_name"] not in abandoned
              and (newest is None or version_key(r["tag_name"]) > version_key(newest))
              and loader_uploaded(r, r["tag_name"])]
    if drafts:
        return max(drafts, key=version_key), "draft"
    if newest is None:
        raise StepFailure("list", "no published vX.Y.Z release")
    return newest, "published"


def decide(tag: str, events: list[dict], now: datetime.datetime, busy: bool) -> tuple[str, str]:
    """(decision, reason) of one probe: "work" when a run would do something, else "idle". A held lock is idle, because a run
    is going. Work: the tag is not done and holds fewer than MAX_FAILURES counted failures with the newest RETRY_SPACING old;
    or no run has started within HEARTBEAT."""
    if busy:
        return "idle", "a run holds the lock"
    history = [e for e in events if e.get("tag") == tag]
    if not any(e.get("event") == "done" for e in history):
        failures = counted_failures(history)
        if len(failures) < MAX_FAILURES and (not failures or now - event_time(failures[-1]) >= RETRY_SPACING):
            return "work", f"{tag} is not done"
    if not any(e.get("event") == "start" and now - event_time(e) < HEARTBEAT for e in events):
        return "work", "no run has started within the heartbeat"
    return "idle", f"nothing for {tag} yet"


def lock_held(work: pathlib.Path) -> bool:
    """Whether a run holds timer.lock. The probe tries the lock without waiting and gives it back at once."""
    fd = take_lock(work)
    if fd is None:
        return True
    os.close(fd)
    return False


def probe(env: dict[str, str], runner: Runner, path: pathlib.Path, now: datetime.datetime) -> int:
    """`--probe`, a scheduler's pre-check: one GitHub call (no git, no step, no credential), then 0 when a run has work, 1 when
    it is idle, 255 when the probe itself failed. A failure is recorded as `failed` at step `probe`. A decision is written when it changes and at most every PROBE_RECORD_SPACING, so an idle timer adds no lines."""
    gh = env.get("GH_BIN", "gh")
    child = {k: v for k, v in env.items() if k not in SECRET_ENV and k not in VARIANT_ENV and k != "GH_TOKEN"}
    log = None
    try:
        events = read_events(path)
        log = Log(path, "")
        try:
            result = runner([gh, "api", RELEASE_LISTING], cwd=None, env=child)
            if result.returncode != 0:
                raise StepFailure("probe", f"{pathlib.Path(gh).name} api exited {result.returncode}")
            tag, _stage = choose_tag(result.stdout, events)
            decision, reason = decide(tag, events, now, lock_held(work_root(env)))
        except StepFailure as failure:
            sys.stderr.write(f"quest-release-timer: {failure.step}: {failure}\n")
            _record(log, "failed", "probe", str(failure), "main")
            return 255
        previous = [e for e in events if e.get("event") == "probe"]
        if not previous or previous[-1].get("decision") != decision or now - event_time(previous[-1]) >= PROBE_RECORD_SPACING:
            log.event("probe", decision=decision, reason=reason, tag=tag)
        return 0 if decision == "work" else 1
    except (Failure, OSError) as failure:
        sys.stderr.write(f"quest-release-timer: probe: {failure}\n")
        return 255
    finally:
        if log is not None:
            log.close()


def draft_pending(timer: "Timer | None") -> bool:
    """A draft run whose release is not published yet: a failure of it abandons the draft (`draft_abandoned`), not counted."""
    return timer is not None and timer.stage == "draft" and timer.part == "draft"


def _record(log: Log, event: str, step: str, error: str, part: str = "main") -> None:
    try:
        log.event(event, step=step, error=error, part=part)
    except Failure as log_failure:
        sys.stderr.write(f"quest-release-timer: {log_failure}\n")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
