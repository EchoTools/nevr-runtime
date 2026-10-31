#!/usr/bin/env python3
"""Point the beta channel of the signed manifest at an earlier listed release (RELEASING.md, "Emergency: roll beta back";
#1513). Usage: rollback_beta.py vX.Y.Z (--dry-run | --live) [--forward] [--keep-train] [--yes]

A bad release is already on beta and installers read `channels.beta`. This publishes the manifest again with beta on a
release the manifest already lists, through the same `sign-manifest.yml` publish the Quest release timer uses, and checks
that `verify-manifest --live` lists it with every artifact it had. It cannot target a tag the manifest does not list: the
fix for a bad release is a new release, which reaches beta through the release train and the Quest release timer.

  read_live        `verify-manifest --live` of the release signer, and the manifest the public host serves, held to the
                   sha256 the signer verified
  check_target     refuses a tag the manifest does not list, the tag that already is beta, a higher tag unless --forward
                   (the roll-forward path), and --forward for a tag that is not higher; `override=true` goes into the
                   dispatch only when the tag is lower than beta
  claims           `release-signer listed-claims --tag vX.Y.Z --live`: the publish's `extra_artifacts`. They must be exactly
                   the target's artifacts that GitHub does not hold (every kind but windows-zip and quest-library), by name,
                   and each must be what the manifest lists for the tag
  plan             printed: beta now, the target, the direction, manifest_version, the target's artifacts with sizes, and
                   whether the train will be paused
  --dry-run        reads (and only reads) the variable and the Quest release timer's lock, and says whether a live run would
                   be refused; dispatches with dry_run=true, watches, checks the jobs (a dry run records nothing, so the
                   record job is skipped), checks that beta did not move, and downloads the run's signed manifest and
                   compares it with the live one. It changes nothing else: no variable.
  --live --yes     pause_variable  RELEASE_TRAIN := false on EchoTools/nevr-runtime (the release train stops cutting), read back
                                   (--keep-train skips it). This tool does not stop whatever schedules
                                   tools/quest_release_timer.py: stop that scheduler yourself before a live run, or the timer
                                   can publish a release to beta while the rollback is under way
                   check_timer     no run of tools/quest_release_timer.py holds its lock (<work>/timer.lock, found as the timer
                                   finds it: QUEST_RELEASE_WORK_ROOT, else under $XDG_STATE_HOME/nevr-runtime), else it refuses
                   check_signer_runs   no `sign-manifest.yml` run on the signer repository is queued or running
                   recheck         beta has not moved, and the claims still match, while pausing
                   dispatch, watch, check_jobs   the live publish; the jobs that publish all succeeded
                   verify_live     beta is the target and every artifact the target listed is still listed
                   It never resumes the train: it prints the two resume commands, says the train stays paused until someone
                   runs them, and prints what ships the fix (the train, not this tool) and the false-alarm command.

Inputs, from the environment: GH_BIN (default gh), RELEASE_PUBLIC_BASE
(required), RELEASE_SIGNER_BIN and RELEASE_SIGNER_DIR (both, or neither: then the signer is built from a snapshot of the
main of the clone QUEST_RELEASE_SIGNER_REPO names, required in that case, the way the Quest release timer builds it, and the
snapshot is removed when the run ends). No R2 credential is read, and none is passed to a child. Every command has a timeout (600 s,
1800 s for the signer build and the watch, 7200 s for the watch when the target lists PC game data). Every step appends a
JSON line (ts, run, event, step, tag, direction, outcome, error) to $XDG_STATE_HOME/nevr-runtime/rollback-beta.jsonl; a log
that cannot be written is a failure. A failed, interrupted or crashed run exits non-zero and names the step; after it began
to pause or dispatch it says what is paused, what was dispatched (with the run id when known) and how to resume. A rollback
that was verified but whose scratch could not be removed exits 0 and says so: the outcome the command exists for is done and
verified, and a non-zero exit would invite a second run. No token, credential or hosting URL is printed or logged: every
value read from the environment for a credential or the host is replaced by <redacted>, and the stdout of gh and the signer
(remote content) is never put in an error.
"""

import argparse
import collections
import fcntl
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import uuid
from collections.abc import Callable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import quest_release_timer  # noqa: E402 - where the timer's work folder (and its lock) is found
from publish_quest_apk import Failure, Log, fetch, public_base  # noqa: E402 - the sibling tool's log, failure and fetch

TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")  # used with fullmatch: `$` would pass a trailing newline
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
RUN_ID = re.compile(r"/actions/runs/([0-9]+)")
SHA256 = re.compile(r"[0-9a-f]{64}")
MANIFEST_PATH = "nevr-runtime/manifest.json"  # as in quest_release_timer.py
MANIFEST_LIMIT = 16 << 20
RELEASE_SLUG = "EchoTools/nevr-runtime"
SIGNER_SLUG = "thesprockee/release-signer"
WORKFLOW = "sign-manifest.yml"
TRAIN_VARIABLE = "RELEASE_TRAIN"
SIGNER_BINARY = "release-signer"
DEFAULT_GH = "gh"  # as in quest_release_timer.py and release_train_guard.py
# The jobs of .github/workflows/sign-manifest.yml in the release-signer repository. A live run records and uploads; a dry run
# checks, signs and verifies the upload, and the record job is skipped. A green run with a skipped job that should have run
# published nothing, so the conclusions are read job by job.
LIVE_JOBS = {"Commit the manifest record": "success", "Publish to R2": "success", "A run that published nothing is red": "success"}
DRY_JOBS = {"Check the release and build the manifest": "success", "Sign manifest.json": "success",
            "Commit the manifest record": "skipped", "Publish to R2": "success", "A run that published nothing is red": "success"}
DEFAULT_TIMEOUT = 600  # seconds, for every command that names no other
BUILD_TIMEOUT = 1800  # the signer snapshot's go build
WATCH_TIMEOUT = 1800  # a run takes about six minutes
WATCH_TIMEOUT_PC_DATA = 7200  # the check job downloads the PC game data (about 5 GB) when the target lists it
GITHUB_KINDS = frozenset({"windows-zip", "quest-library"})  # the artifacts GitHub holds; every other kind is claimed
IGNORED_ENTRY_FIELDS = ("notes", "published")  # a re-published listed release may differ in these (the signer's sameBytes)
SECRET_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "GH_TOKEN", "GITHUB_TOKEN")
R2_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")
LOCAL_OUTPUT = frozenset({"git", "tar", "go"})  # programs whose stdout is ours; everything else is remote content
TAIL = 2000

Runner = Callable[..., subprocess.CompletedProcess]


class StepFailure(Failure):
    def __init__(self, step: str, message: str, code: int | None = None) -> None:
        super().__init__(message)
        self.step = step
        self.code = code  # the exit code of the command that failed, when one did


class Refusal(StepFailure):
    """A precondition that is not met: nothing was changed."""


def subprocess_runner(argv: list[str], cwd: str | None = None, env: dict | None = None,
                      timeout: float | None = None) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(argv, 124, "", f"{pathlib.Path(argv[0]).name} ran longer than {timeout} s and was stopped")
    except OSError as error:
        return subprocess.CompletedProcess(argv, 127, "", f"cannot run {argv[0]}: {error.strerror}")


def state_log_path(env: dict[str, str]) -> pathlib.Path:
    state = env.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return pathlib.Path(state) / "nevr-runtime" / "rollback-beta.jsonl"


def version_key(version: str) -> tuple[int, int, int]:
    found = VERSION.fullmatch(version)
    if not found:
        raise StepFailure("check_target", f"{version!r} is not a vX.Y.Z version")
    return int(found.group(1)), int(found.group(2)), int(found.group(3))


def note_lines(value: object) -> set[str]:
    if isinstance(value, str):
        return set(value.splitlines())
    if isinstance(value, list):
        return {str(line) for line in value}
    return set()


def has_pc_game_data(artifacts: list[dict]) -> bool:
    return any(a.get("kind") == "game-data" and a.get("platform") == "windows-x86_64" for a in artifacts)


class Rollback:
    def __init__(self, args: argparse.Namespace, env: dict[str, str], runner: Runner, log: Log) -> None:
        self.args, self.env, self.runner, self.log = args, env, runner, log
        self.tag = args.tag
        self.version = args.tag[1:]
        self.gh = env.get("GH_BIN") or DEFAULT_GH
        base = env.get("RELEASE_PUBLIC_BASE", "")
        redacted = [env[name] for name in SECRET_ENV if env.get(name)]
        if base:
            redacted += [base.rstrip("/"), urllib.parse.urlsplit(base).hostname or ""]
        self.secrets = sorted({s for s in redacted if s}, key=len, reverse=True)
        self.child = {k: v for k, v in env.items() if k not in SECRET_ENV}  # the signer and git get no credential
        self.gh_env = {k: v for k, v in env.items() if k not in R2_ENV}  # gh keeps its own token and never sees R2
        self.pausing = args.live and not args.keep_train
        self.paused = {"variable": False}
        self.dispatched: str | None = None
        self.dispatch_unknown = False  # gh may have dispatched a publish and left no run id
        self.outcome = ""  # what a finished run did, for the line a failed cleanup adds to
        self.current = ""  # the step the run is in, for an interrupted or crashed run
        self.scratch: pathlib.Path | None = None
        self.signer_bin = pathlib.Path()
        self.signer_dir = pathlib.Path()

    # ---- plumbing -------------------------------------------------------------------------------------------------

    def redact(self, text: str) -> str:
        for secret in self.secrets:
            text = text.replace(secret, "<redacted>")
        return text

    def say(self, line: str) -> None:
        print(self.redact(line), flush=True)

    def ok(self, step: str, **fields: object) -> None:
        self.current = step
        self.log.event("step", step=step, outcome="ok", error="", **fields)

    def cmd(self, step: str, argv: list[str], cwd: pathlib.Path | str | None = None, env: dict | None = None,
            ok_codes: tuple[int, ...] = (0,), timeout: float | None = None) -> subprocess.CompletedProcess:
        self.current = step
        result = self.runner(argv, cwd=str(cwd) if cwd else None, env=env if env is not None else self.child,
                             timeout=timeout or DEFAULT_TIMEOUT)
        if result.returncode not in ok_codes:
            name = pathlib.Path(argv[0]).name
            raise StepFailure(step, f"{name} {' '.join(argv[1:3])} exited {result.returncode}:{self.tails(name, result)}",
                              result.returncode)
        return result

    def gh_cmd(self, step: str, args: list[str], timeout: float | None = None) -> subprocess.CompletedProcess:
        return self.cmd(step, [self.gh, *args], env=self.gh_env, timeout=timeout)

    def tails(self, program: str, result: subprocess.CompletedProcess) -> str:
        parts = []
        for label, text in (("stderr", result.stderr), ("stdout", result.stdout if program in LOCAL_OUTPUT else "")):
            tail = self.redact(text or "")[-TAIL:].strip()
            if tail:
                parts.append(f"{label}: {tail}")
        return (" " + " | ".join(parts)) if parts else ""

    # ---- the signer and the scratch -------------------------------------------------------------------------------

    def make_scratch(self, step: str) -> pathlib.Path:
        """This run's own folder under the work root, made once and removed when the run ends."""
        if self.scratch is None:
            state = self.env.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
            root = pathlib.Path(self.env.get("NEVR_WORK_ROOT") or pathlib.Path(state) / "nevr-runtime") / "rollback-beta"
            try:
                root.mkdir(parents=True, exist_ok=True)
                readme = root / "README.md"
                if not readme.exists():
                    readme.write_text("Scratch of tools/rollback_beta.py (the beta rollback). Each run removes what it makes here "
                                      "when it ends.\n", encoding="utf-8")
                scratch = root / f"run-{self.log.base['run']}"
                scratch.mkdir()
            except OSError as error:
                raise StepFailure(step, f"cannot make the scratch folder {root}: {error.strerror}") from error
            self.scratch = scratch
        return self.scratch

    def prepare_signer(self) -> None:
        if self.env.get("RELEASE_SIGNER_BIN"):
            self.signer_bin = pathlib.Path(self.env["RELEASE_SIGNER_BIN"])
            self.signer_dir = pathlib.Path(self.env["RELEASE_SIGNER_DIR"])
            return
        step = "build_signer"
        repo = str(pathlib.Path(self.env["QUEST_RELEASE_SIGNER_REPO"]))  # run() refused a run without it
        scratch = self.make_scratch(step)
        tarball = scratch / "release-signer.tar"
        self.signer_dir = scratch / "snapshot"
        self.signer_bin = self.signer_dir / SIGNER_BINARY
        self.cmd(step, ["git", "-C", repo, "fetch", "origin", "main"])
        sha = self.cmd(step, ["git", "-C", repo, "rev-parse", "origin/main"]).stdout.strip()
        self.cmd(step, ["git", "-C", repo, "archive", "--format=tar", "-o", str(tarball), "origin/main"])
        self.signer_dir.mkdir()
        self.cmd(step, ["tar", "-xf", str(tarball), "-C", str(self.signer_dir)])
        self.cmd(step, ["go", "build", "-o", str(self.signer_bin), "./cmd/release-signer"], self.signer_dir, timeout=BUILD_TIMEOUT)
        self.ok(step, signer_commit=sha)

    def cleanup(self) -> str:
        """Remove exactly the folder this run made. Returns what went wrong, or an empty string."""
        if self.scratch is None:
            return ""
        scratch, self.scratch = self.scratch, None
        try:
            shutil.rmtree(scratch)
        except OSError as error:
            return f"cleanup of {scratch} failed: {error.strerror}"
        return ""

    def read_live(self, step: str) -> dict:
        """`verify-manifest --live` and the manifest the public host serves, held to the bytes the signer verified."""
        out = self.cmd(step, [str(self.signer_bin), "verify-manifest", "--live"], self.signer_dir).stdout
        state = None
        for line in out.splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if isinstance(record, dict) and "channels" in record:
                state = record
                break
        if state is None:
            raise StepFailure(step, "verify-manifest --live printed no manifest")
        try:
            base = public_base(self.env)
            body, digest, _ = fetch(f"{base}/{MANIFEST_PATH}", MANIFEST_LIMIT, keep=True)
        except Failure as failure:
            raise StepFailure(step, str(failure)) from failure
        if digest != state.get("sha256"):
            raise StepFailure(step, f"the manifest the host serves has sha256 {digest}, not the {state.get('sha256')} "
                                    "that verify-manifest --live verified")
        try:
            manifest = json.loads(body)
        except ValueError as error:
            raise StepFailure(step, f"the manifest the host serves is not JSON: {error}") from error
        if not isinstance(manifest, dict):
            raise StepFailure(step, "the manifest the host serves is not a JSON object")
        return {"state": state, "manifest": manifest}

    @staticmethod
    def beta_of(live: dict) -> str:
        return str(live["manifest"].get("channels", {}).get("beta", ""))

    @staticmethod
    def artifacts_of(manifest: dict, version: str) -> list[dict]:
        release = manifest.get("releases", {}).get(version, {})
        return [a for a in release.get("artifacts", []) if isinstance(a, dict)]

    # ---- the steps ------------------------------------------------------------------------------------------------

    def check_target(self, live: dict) -> tuple[str, str, bool]:
        step = "check_target"
        beta = self.beta_of(live)
        if not beta:
            raise Refusal(step, "the live manifest has no beta release: there is nothing to roll back from")
        if self.version not in live["manifest"].get("releases", {}):
            raise Refusal(step, f"{self.tag} is not listed in the live manifest")
        if beta == self.version:
            raise Refusal(step, f"{self.tag} is already beta")
        higher = version_key(self.version) > version_key(beta)
        if higher and not self.args.forward:
            raise Refusal(step, f"{self.tag} is higher than the current beta v{beta}: a roll forward needs --forward")
        if not higher and self.args.forward:
            raise Refusal(step, f"--forward was given but {self.tag} is not higher than the current beta v{beta}")
        return beta, ("forward" if higher else "back"), not higher

    def claims(self, live: dict) -> list[dict]:
        step = "claims"
        out = self.cmd(step, [str(self.signer_bin), "listed-claims", "--tag", self.tag, "--live"], self.signer_dir).stdout
        try:
            claims = json.loads(out)
        except ValueError as error:
            raise StepFailure(step, f"listed-claims did not print a JSON array: {error}") from error
        if not isinstance(claims, list):
            raise StepFailure(step, "listed-claims did not print a JSON array")
        if not claims:
            raise Refusal(step, f"listed-claims printed no artifacts for {self.tag}: the release must list its Quest APK and "
                                "game data before beta can point at it")
        for claim in claims:
            if not (isinstance(claim, dict) and isinstance(claim.get("name"), str) and SHA256.fullmatch(str(claim.get("sha256")))):
                raise StepFailure(step, "listed-claims printed an entry without a name and a sha256")
        self.check_claims_listed(step, claims, live["manifest"])
        # The publish takes the bytes of every artifact it is not handed from GitHub, so what it is handed must be all of the
        # others: a claim list without the Quest APK would publish the release without one.
        expected = collections.Counter(str(a.get("name")) for a in self.artifacts_of(live["manifest"], self.version)
                                       if a.get("kind") not in GITHUB_KINDS)
        got = collections.Counter(c["name"] for c in claims)
        if got != expected:
            missing, extra = sorted((expected - got).elements()), sorted((got - expected).elements())
            raise Refusal(step, f"the claims for {self.tag} are not the artifacts GitHub does not hold (every kind but "
                                f"{', '.join(sorted(GITHUB_KINDS))}): missing {missing or 'none'}; not expected {extra or 'none'}")
        return claims

    def check_claims_listed(self, step: str, claims: list[dict], manifest: dict) -> None:
        listed = {(a.get("name"), a.get("sha256")) for a in self.artifacts_of(manifest, self.version)}
        for claim in claims:
            if (claim["name"], claim["sha256"]) not in listed:
                raise Refusal(step, f"the claim for {claim['name']} is not what the manifest lists for {self.tag}")

    def plan(self, live: dict, beta: str, direction: str, override: bool) -> None:
        manifest = live["manifest"]
        if self.args.live and self.pausing:
            train = f"will be paused first ({TRAIN_VARIABLE}=false; the Quest release timer is not stopped by this tool)"
        elif self.args.live:
            train = "not touched (--keep-train)"
        else:
            train = "not touched (dry run)"
        self.say(f"plan: beta v{beta} -> {self.tag} ({direction}; override={str(override).lower()})")
        self.say(f"plan: manifest_version {live['state'].get('manifest_version', manifest.get('manifest_version'))}")
        for artifact in self.artifacts_of(manifest, self.version):
            self.say(f"plan: artifact {artifact.get('name')} kind={artifact.get('kind')} size={artifact.get('size')}")
        self.say(f"plan: train {train}")
        self.ok("plan", beta=beta, override=override)

    # ---- what a live run needs and a dry run only reports ---------------------------------------------------------

    def variable_value(self, step: str) -> object:
        listed = self.gh_cmd(step, ["variable", "list", "-R", RELEASE_SLUG, "--json", "name,value"]).stdout
        try:
            return next((v.get("value") for v in json.loads(listed) if v.get("name") == TRAIN_VARIABLE), None)
        except (ValueError, AttributeError, TypeError) as error:
            raise StepFailure(step, f"the variable list is unreadable: {error}") from error

    def timer_lock_held(self) -> bool:
        """Whether a run of tools/quest_release_timer.py holds its lock (<work>/timer.lock) now. Read-only: a work folder or a
        lock file that does not exist means no run has ever held it, and none is made."""
        try:
            fd = os.open(quest_release_timer.work_root(self.env) / "timer.lock", os.O_RDONLY | os.O_NOFOLLOW)
        except FileNotFoundError:
            return False
        except OSError as error:
            raise StepFailure(self.current or "check_timer", f"cannot open the Quest release timer's lock: {error.strerror}") from error
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        finally:
            os.close(fd)
        return False

    def active_signer_runs(self, step: str) -> list[str]:
        out = self.gh_cmd(step, ["run", "list", "--workflow", WORKFLOW, "-R", SIGNER_SLUG, "--json", "status,databaseId",
                                 "--limit", "30"]).stdout
        try:
            return [f"{run['databaseId']} ({run['status']})" for run in json.loads(out) if run["status"] != "completed"]
        except (ValueError, KeyError, TypeError) as error:
            raise StepFailure(step, f"the run list is unreadable: {error}") from error

    def pause_variable(self) -> None:
        step = "pause_variable"
        self.gh_cmd(step, ["variable", "set", TRAIN_VARIABLE, "--body", "false", "-R", RELEASE_SLUG])
        value = self.variable_value(step)
        if value != "false":
            raise StepFailure(step, f"{TRAIN_VARIABLE} is {value!r} after the set, not 'false'")
        self.paused["variable"] = True
        self.ok(step)

    def check_timer(self) -> None:
        step = "check_timer"
        self.current = step
        if self.timer_lock_held():
            raise StepFailure(step, "a run of tools/quest_release_timer.py holds its lock: a Quest release is in progress and "
                                    "publishes to beta when it ends. Wait for it to end, then run this again")
        self.ok(step)

    def check_signer_runs(self) -> None:
        step = "check_signer_runs"
        active = self.active_signer_runs(step)
        if active:
            raise StepFailure(step, f"{WORKFLOW} has runs in progress on {SIGNER_SLUG}: {', '.join(active)}. A publish is "
                                    "already under way; wait for it to end, then run this again")
        self.ok(step)

    def dry_preflight(self) -> None:
        """Read-only: what a live run would meet, as plan lines. A value that cannot be read is said, not guessed."""
        refused = []
        try:
            self.say(f"plan: {TRAIN_VARIABLE} is {self.variable_value('preflight')!r}; a live run sets it to false")
        except StepFailure as failure:
            self.say(f"plan: the variable could not be read: {failure}")
        try:
            held = self.timer_lock_held()
            self.say(f"plan: a run of the Quest release timer {'holds' if held else 'does not hold'} its lock")
            if held:
                refused.append("a Quest release timer run holds its lock")
        except StepFailure as failure:
            self.say(f"plan: the Quest release timer's lock could not be read: {failure}")
        try:
            active = self.active_signer_runs("preflight")
            self.say(f"plan: {WORKFLOW} runs in progress on {SIGNER_SLUG}: {', '.join(active) or 'none'}")
            if active:
                refused.append(f"{WORKFLOW} has runs in progress ({', '.join(active)})")
        except StepFailure as failure:
            self.say(f"plan: the signer runs could not be read: {failure}")
        self.say("plan: a live run would be refused now: " + "; ".join(refused) if refused else
                 "plan: a live run would not be refused by the Quest release timer or a running signer workflow")
        self.ok("preflight", refused=refused)

    def recheck(self, beta: str, claims: list[dict]) -> dict:
        step = "recheck"
        live = self.read_live(step)
        now = self.beta_of(live)
        if now != beta:
            raise StepFailure(step, f"beta moved while the train was being paused: it was v{beta}, it is now v{now or 'none'}; "
                                    "nothing was dispatched")
        self.check_claims_listed(step, claims, live["manifest"])
        self.ok(step, beta=now)
        return live

    def dispatch(self, claims: list[dict], override: bool, dry: bool) -> str:
        step = "dispatch"
        inputs = {"action": "publish", "tag": self.tag, "channel": "beta",
                  "extra_artifacts": json.dumps(claims, separators=(",", ":")), "override": str(override).lower(),
                  "dry_run": str(dry).lower()}
        argv = ["workflow", "run", WORKFLOW, "-R", SIGNER_SLUG, "--ref", "main"]
        for key, value in inputs.items():
            argv += ["-f", f"{key}={value}"]
        try:
            out = self.gh_cmd(step, argv).stdout
        except StepFailure as failure:
            if failure.code == 124:  # a call that timed out may have been accepted
                self.dispatch_unknown = True
                raise StepFailure(step, f"{failure}. A publish MAY have been dispatched: list the runs on the signer repo "
                                        "before dispatching again", 124) from failure
            raise
        found = RUN_ID.search(out)
        if not found:
            self.dispatch_unknown = True
            raise StepFailure(step, "gh exited 0 but printed no run URL: a publish MAY have been dispatched: list the runs on "
                                    "the signer repo before dispatching again")
        self.dispatched = found.group(1)
        # The operator reads the run id before the log is asked to take it: a log that cannot be written must not hide it.
        self.say(f"run {self.dispatched} dispatched (dry_run={str(dry).lower()}, override={str(override).lower()})")
        self.ok(step, run_id=self.dispatched, dry_run=dry, override=override)
        return self.dispatched

    def watch(self, run_id: str, timeout: int) -> None:
        step = "watch"
        try:
            self.gh_cmd(step, ["run", "watch", run_id, "-R", SIGNER_SLUG, "--exit-status"], timeout=timeout)
        except StepFailure as failure:
            raise StepFailure(step, f"run {run_id}: {failure}") from failure
        self.ok(step, run_id=run_id)

    def check_jobs(self, run_id: str, dry: bool) -> str:
        step = "check_jobs"
        out = self.gh_cmd(step, ["run", "view", run_id, "-R", SIGNER_SLUG, "--json", "jobs"]).stdout
        try:
            jobs = {j["name"]: j.get("conclusion") for j in json.loads(out)["jobs"]}
        except (ValueError, KeyError, TypeError) as error:
            raise StepFailure(step, f"run {run_id}: the jobs are unreadable: {error}") from error
        want = DRY_JOBS if dry else LIVE_JOBS
        bad = [f"{name} is {jobs.get(name) or 'missing'}, want {state}" for name, state in want.items() if jobs.get(name) != state]
        if bad:
            raise StepFailure(step, f"run {run_id} did not do what a {'dry' if dry else 'live'} publish does: " + "; ".join(bad))
        self.ok(step, run_id=run_id)
        return ", ".join(f"{name}={state}" for name, state in want.items())

    def verify_unchanged(self, beta: str) -> None:
        step = "verify_unchanged"
        now = self.beta_of(self.read_live(step))
        if now != beta:
            raise StepFailure(step, f"the dry run moved beta: it was v{beta}, it is now v{now or 'none'}")
        self.ok(step, beta=now)

    def check_dry_manifest(self, run_id: str, live: dict) -> None:
        """The manifest the dry run signed (with a throwaway key) is what a live run would publish: compare it, read-only, with
        the live one. Beta must be the target and nothing else may move; a notes difference is reported, not a failure."""
        step = "check_dry_manifest"
        dest = self.make_scratch(step) / f"signed-{run_id}"
        self.gh_cmd(step, ["run", "download", run_id, "-R", SIGNER_SLUG, "-n", "manifest-signed", "-D", str(dest)])
        try:
            signed = json.loads((dest / "manifest.json").read_text(encoding="utf-8"))
            signed_releases, signed_channels = dict(signed["releases"]), dict(signed["channels"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise StepFailure(step, f"the dry run's manifest-signed artifact has no readable manifest.json: {error}") from error
        manifest = live["manifest"]
        releases, channels = manifest.get("releases", {}), manifest.get("channels", {})
        if signed_channels.get("beta") != self.version:
            raise StepFailure(step, f"the dry run's manifest has beta {signed_channels.get('beta')!r}, not {self.version!r}")
        others = {k: v for k, v in channels.items() if k != "beta"}
        if {k: v for k, v in signed_channels.items() if k != "beta"} != others:
            raise StepFailure(step, "the dry run's manifest changes a channel other than beta")
        if set(signed_releases) != set(releases):
            added, removed = sorted(set(signed_releases) - set(releases)), sorted(set(releases) - set(signed_releases))
            raise StepFailure(step, f"the dry run's manifest changes the set of releases: adds {added or 'none'}, removes "
                                    f"{removed or 'none'}")
        changed = sorted(v for v in releases if v != self.version and signed_releases[v] != releases[v])
        if changed:
            raise StepFailure(step, f"the dry run's manifest changes the entries of other releases: {', '.join(changed)}")
        target, signed_target = releases[self.version], signed_releases[self.version]

        def artifact_key(artifacts: object) -> list[tuple]:
            return sorted((str(a.get("name")), str(a.get("kind")), str(a.get("sha256")), str(a.get("size")))
                          for a in (artifacts or []) if isinstance(a, dict))

        if artifact_key(signed_target.get("artifacts")) != artifact_key(target.get("artifacts")):
            raise StepFailure(step, f"the dry run's manifest lists other artifacts (names, kinds, sha256, sizes) for {self.tag} "
                                    "than the live one")
        for field in sorted((set(target) | set(signed_target)) - set(IGNORED_ENTRY_FIELDS) - {"artifacts"}):
            if target.get(field) != signed_target.get(field):
                self.say(f"note: the dry run's entry for {self.tag} differs from the live one in {field}")
        if note_lines(target.get("notes")) != note_lines(signed_target.get("notes")):
            gone = sorted(note_lines(target.get("notes")) - note_lines(signed_target.get("notes")))
            new = sorted(note_lines(signed_target.get("notes")) - note_lines(target.get("notes")))
            self.say(f"note: the notes of {self.tag} differ (lines only in the live manifest: {len(gone)}, only in the dry "
                     f"run's: {len(new)}); a live run publishes the dry run's")
        self.say(f"dry run manifest: beta is {self.tag}, the {len(releases)} releases and the other channels are unchanged, "
                 f"{len(artifact_key(target.get('artifacts')))} artifacts of {self.tag} equal the live ones")
        self.ok(step, run_id=run_id)

    def verify_live(self, before: dict) -> dict:
        step = "verify_live"
        after = self.read_live(step)
        now = self.beta_of(after)
        if now != self.version:
            raise StepFailure(step, f"the live manifest lists beta {now or 'nothing'}, not {self.version}")
        listed = {(a.get("name"), a.get("sha256")) for a in self.artifacts_of(after["manifest"], self.version)}
        # `before` is the manifest read after the pause, and the claims were held to it there, so this covers them.
        wanted = {(a.get("name"), a.get("sha256")) for a in self.artifacts_of(before["manifest"], self.version)}
        lost = sorted(name for name, _ in wanted - listed)
        if lost:
            raise StepFailure(step, f"the live manifest's {self.tag} is missing or has changed: {', '.join(lost)}")
        self.ok(step, beta=now, manifest_version=after["state"].get("manifest_version"))
        return after

    # ---- the run --------------------------------------------------------------------------------------------------

    def run(self) -> None:
        args = self.args
        if not self.env.get("RELEASE_PUBLIC_BASE"):
            raise Refusal("env", "set RELEASE_PUBLIC_BASE (the public host of the manifest)")
        try:
            public_base(self.env)
        except Failure as failure:
            raise Refusal("env", str(failure)) from failure
        if bool(self.env.get("RELEASE_SIGNER_BIN")) != bool(self.env.get("RELEASE_SIGNER_DIR")):
            missing = "RELEASE_SIGNER_DIR" if self.env.get("RELEASE_SIGNER_BIN") else "RELEASE_SIGNER_BIN"
            raise Refusal("env", f"set {missing} too, or neither (then the signer is built from a snapshot)")
        if not self.env.get("RELEASE_SIGNER_BIN") and not self.env.get("QUEST_RELEASE_SIGNER_REPO"):
            raise Refusal("env", "set QUEST_RELEASE_SIGNER_REPO (a clone of the release-signer repository, whose main the "
                                 "signer is built from), or RELEASE_SIGNER_BIN and RELEASE_SIGNER_DIR")
        if args.live and not args.yes:
            raise Refusal("confirm", "--live moves the beta channel for every installer: pass --yes to confirm")
        self.prepare_signer()
        live = self.read_live("read_live")
        self.ok("read_live", beta=self.beta_of(live), manifest_version=live["state"].get("manifest_version"))
        beta, direction, override = self.check_target(live)
        self.log.base["direction"] = direction
        self.ok("check_target", beta=beta, override=override)
        claims = self.claims(live)
        self.ok("claims", count=len(claims))
        self.plan(live, beta, direction, override)
        if not args.live:
            self.dry_preflight()
        if self.pausing:
            self.pause_variable()
        if args.live:
            self.check_timer()
            self.check_signer_runs()
        if self.pausing:
            live = self.recheck(beta, claims)
        timeout = WATCH_TIMEOUT_PC_DATA if has_pc_game_data(self.artifacts_of(live["manifest"], self.version)) else WATCH_TIMEOUT
        started = time.monotonic()
        run_id = self.dispatch(claims, override, dry=not args.live)
        self.watch(run_id, timeout)
        jobs = self.check_jobs(run_id, dry=not args.live)
        if not args.live:
            self.verify_unchanged(beta)
            self.check_dry_manifest(run_id, live)
            self.outcome = "dry run ok"
            self.say(f"dry run ok: {jobs}; beta is still v{beta}")
            self.say(f"elapsed {round(time.monotonic() - started)} s")
            return
        after = self.verify_live(live)
        self.outcome = "rolled forward and verified" if direction == "forward" else "rolled back and verified"
        self.say(f"{'rolled forward' if direction == 'forward' else 'rolled back'}: beta is {self.tag}, listed with every artifact "
                 f"by verify-manifest --live (manifest_version {after['state'].get('manifest_version')})")
        self.say(f"run {run_id}")
        self.say(f"elapsed {round(time.monotonic() - started)} s (dispatch to verified)")
        self.say_next(beta, direction)

    def say_next(self, beta: str, direction: str) -> None:
        if self.pausing:
            self.say("the train is STILL PAUSED and stays paused until someone resumes it.")
        if direction == "forward":
            self.say(f"undo: python3 tools/rollback_beta.py v{beta} --live --yes")
            if self.pausing:
                self.say("resume the train with:")
                self.say(self.resume_commands())
            return
        self.say("the fix does not ship through this tool, which cannot target a tag the manifest does not list: it ships "
                 "through the train. The guard cuts the fixed release and the Quest timer publishes it to beta as a forward move "
                 "that needs no override.")
        if self.pausing:
            self.say("to ship the fix, resume the train:")
            self.say(self.resume_commands())
        else:
            self.say("the train was not paused (--keep-train): it will cut the fixed release when main has one.")
        self.say(f"false alarm only (it re-targets v{beta}, the release you rolled back from): "
                 f"python3 tools/rollback_beta.py v{beta} --live --forward --yes")

    def resume_commands(self) -> str:
        return (f"  {self.gh} variable set {TRAIN_VARIABLE} --body true -R {RELEASE_SLUG}\n"
                "  and start again whatever schedules tools/quest_release_timer.py, if you stopped it")

    def state_report(self) -> list[str]:
        """What a run that failed after it began to pause or dispatch did and did not do, one fact per line."""
        facts = []
        undone = []
        if self.pausing:
            facts.append(f"{TRAIN_VARIABLE} is false" if self.paused["variable"] else f"{TRAIN_VARIABLE} is NOT false")
            if not self.paused["variable"]:
                undone.append("pausing the variable")
        if self.dispatched:
            kind = "a dry run: it publishes nothing" if not self.args.live else "its outcome is not confirmed"
            facts.append(f"publish run {self.dispatched} was dispatched ({kind})")
        elif self.dispatch_unknown:
            facts.append("a publish MAY have been dispatched (gh left no run id): list the runs on the signer repo before "
                         "dispatching again")
        elif self.pausing:
            facts.append("no publish was dispatched")
            undone.append("the publish")
        if not facts:
            return []
        lines = ["state: " + "; ".join(facts)]
        if undone:
            lines.append("NOT done: " + ", ".join(undone))
        if self.paused["variable"]:
            lines.append("the pauses that were made stay; undo them with:\n" + self.resume_commands())
        return lines


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="rollback_beta.py", description=__doc__.splitlines()[0])
    parser.add_argument("tag", help="the listed release beta should point at, vX.Y.Z")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true", help="dispatch the publish as a dry run and change nothing else")
    mode.add_argument("--live", action="store_true", help="pause the train, publish, verify (needs --yes)")
    parser.add_argument("--forward", action="store_true", help="the target is higher than beta (a roll forward)")
    parser.add_argument("--keep-train", action="store_true", help="a live run does not pause the train")
    parser.add_argument("--yes", action="store_true", help="confirm a live run")
    return parser


def may_have_dispatched(rollback: "Rollback | None") -> None:
    """Ctrl-C or a crash while `gh workflow run` was running: gh may have been accepted and left no run id behind. A run id
    that is known wins in the report, so it need not be tested here."""
    if rollback is not None and rollback.current == "dispatch":
        rollback.dispatch_unknown = True


def execute(args: argparse.Namespace, env: dict[str, str], runner: Runner, held: dict) -> int:
    """The run, with every way out reported: a refusal, a failure, a log that cannot be written, Ctrl-C and a crash."""
    try:
        log = Log(state_log_path(env), args.tag)
        held["log"] = log
        log.base.update({"run": uuid.uuid4().hex, "direction": ""})
        log.event("start", step="start", outcome="ok", error="", dry_run=args.dry_run, forward=args.forward,
                  keep_train=args.keep_train)
        rollback = held["rollback"] = Rollback(args, env, runner, log)
        if not TAG.fullmatch(args.tag):
            raise Refusal("tag", f"{args.tag!r} is not a release tag (vX.Y.Z)")
        rollback.run()
        problem = rollback.cleanup()
        if problem:
            sys.stderr.write(f"rollback-beta: {rollback.redact(rollback.outcome)}; {rollback.redact(problem)}\n")
            log.event("cleanup_failed", step="cleanup", outcome="failed", error=rollback.redact(problem))
        log.event("finish", step="done", outcome="ok", error="")
        return 0
    except StepFailure as failure:
        return fail(held, failure.step, str(failure), "refused" if isinstance(failure, Refusal) else "failed")
    except Failure as failure:
        return fail(held, "log", str(failure), "failed")
    except KeyboardInterrupt:
        rollback = held.get("rollback")
        may_have_dispatched(rollback)
        return fail(held, (rollback.current if rollback else "") or "start", "interrupted (Ctrl-C)", "interrupted", 130)
    except Exception as error:  # noqa: BLE001 - a crash is reported with the state, whatever it is
        rollback = held.get("rollback")
        may_have_dispatched(rollback)
        return fail(held, (rollback.current if rollback else "") or "start", f"crashed: {type(error).__name__}: {error}", "crashed")


def main(argv: list[str], env: dict[str, str], runner: Runner = subprocess_runner) -> int:
    args = build_parser().parse_args(argv)
    held: dict = {"log": None, "rollback": None}
    code = execute(args, env, runner, held)
    rollback, log = held["rollback"], held["log"]
    if rollback is not None:
        leftover = rollback.cleanup()
        if leftover:
            sys.stderr.write(f"rollback-beta: {rollback.redact(leftover)}\n")
    if log is not None:
        try:
            log.close()
        except OSError as error:
            sys.stderr.write(f"rollback-beta: cannot close the log {log.path}: {error.strerror}\n")
            code = code or 1
    return code


def fail(held: dict, step: str, message: str, outcome: str, code: int = 1) -> int:
    rollback, log = held.get("rollback"), held.get("log")
    redact = rollback.redact if rollback is not None else (lambda text: text)
    sys.stderr.write(f"rollback-beta: {step}: {redact(message)}\n")
    if rollback is not None:
        for line in rollback.state_report():
            sys.stderr.write(redact(line) + "\n")
    if log is not None:
        try:
            log.event("finish", step=step, outcome=outcome, error=redact(message))
        except Exception as log_failure:  # noqa: BLE001 - the report above is out; a log that cannot take this is said, not raised
            sys.stderr.write(f"rollback-beta: {redact(str(log_failure))}\n")
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
