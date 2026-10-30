#!/usr/bin/env python3
"""Point the beta channel of the signed manifest at an earlier listed release (RELEASING.md, "Emergency: roll beta back";
#1513). Usage: rollback_beta.py vX.Y.Z (--dry-run | --live) [--forward] [--keep-train] [--yes]

A bad release is already on beta and installers read `channels.beta`. This publishes the manifest again with beta on a
release the manifest already lists, through the same `sign-manifest.yml` publish the Quest release timer uses, and checks
that `verify-manifest --live` lists it with every artifact it had.

  read_live        `verify-manifest --live` of the release signer, and the manifest the public host serves, held to the
                   sha256 the signer verified
  check_target     refuses a tag the manifest does not list, the tag that already is beta, a higher tag unless --forward
                   (the roll-forward path), and --forward for a tag that is not higher; `override=true` goes into the
                   dispatch only when the tag is lower than beta
  claims           `release-signer listed-claims --tag vX.Y.Z --live`: the target's Quest APK, game data and content
                   packs, which are the publish's `extra_artifacts`; each must be what the manifest lists for the tag
  plan             printed: beta now, the target, the direction, manifest_version, the target's artifacts with sizes, and
                   whether the train will be paused
  --dry-run        dispatches with dry_run=true, watches, checks the jobs (a dry run records nothing, so the record job is
                   skipped) and checks that beta did not move. It changes nothing else: no variable, no timer.
  --live --yes     pause_variable  RELEASE_TRAIN := false on EchoTools/nevr-runtime (the release train stops cutting), read back
                   pause_timer     `systemctl --user stop nevr-quest-release.timer`, and refuses while a Quest release
                                   run is in progress (--keep-train skips both pauses)
                   recheck         beta has not moved while pausing
                   dispatch, watch, check_jobs   the live publish; the jobs that publish all succeeded
                   verify_live     beta is the target and every artifact the target listed, and every claim, is still listed
                   It never resumes the train: it prints the roll-forward command and the two resume commands and says the
                   train stays paused until someone runs them.

Inputs, from the environment: GH_BIN (default gh), RELEASE_PUBLIC_BASE (required), RELEASE_SIGNER_BIN and RELEASE_SIGNER_DIR
(both, or neither: then the signer is built from a snapshot of the main of QUEST_RELEASE_SIGNER_REPO, default
~/src/release-signer, the way the Quest release timer builds it, and the snapshot is removed when the run ends). No R2
credential is read, and none is passed to a child. Every step appends a JSON line (ts, run, event, step, tag, direction,
outcome, error) to $XDG_STATE_HOME/nevr-runtime/rollback-beta.jsonl; a log that cannot be written is a failure. A failed
step exits 1 and names itself; a live run that failed after it paused says what is and is not done. No token, credential or
hosting URL is printed or logged: every value read from the environment for a credential or the host is replaced by
<redacted>, and the stdout of gh and the signer (remote content) is never put in an error.
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
import urllib.parse
import uuid
from collections.abc import Callable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from publish_quest_apk import Failure, Log, fetch, public_base  # noqa: E402 - the sibling tool's log, failure and fetch

TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
RUN_ID = re.compile(r"/actions/runs/([0-9]+)")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
MANIFEST_PATH = "nevr-runtime/manifest.json"  # as in quest_release_timer.py
MANIFEST_LIMIT = 16 << 20
RELEASE_SLUG = "EchoTools/nevr-runtime"
SIGNER_SLUG = "thesprockee/release-signer"
WORKFLOW = "sign-manifest.yml"
TRAIN_VARIABLE = "RELEASE_TRAIN"
TIMER_UNIT = "nevr-quest-release.timer"
SERVICE_UNIT = "nevr-quest-release.service"
SIGNER_BINARY = "release-signer"
# The jobs of .github/workflows/sign-manifest.yml in the release-signer repository. A live run records and uploads; a dry run
# checks, signs and verifies the upload, and the record job is skipped. A green run with a skipped job that should have run
# published nothing, so the conclusions are read job by job.
LIVE_JOBS = {"Commit the manifest record": "success", "Publish to R2": "success", "A run that published nothing is red": "success"}
DRY_JOBS = {"Check the release and build the manifest": "success", "Sign manifest.json": "success",
            "Commit the manifest record": "skipped", "Publish to R2": "success", "A run that published nothing is red": "success"}
WATCH_TIMEOUT = 1800  # seconds; a run takes about six minutes
SECRET_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "GH_TOKEN", "GITHUB_TOKEN")
R2_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")
LOCAL_OUTPUT = frozenset({"systemctl", "git", "tar", "go"})  # programs whose stdout is ours; everything else is remote content
TAIL = 2000

Runner = Callable[..., subprocess.CompletedProcess]


class StepFailure(Failure):
    def __init__(self, step: str, message: str) -> None:
        super().__init__(message)
        self.step = step


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
    found = VERSION.match(version)
    if not found:
        raise StepFailure("check_target", f"{version!r} is not a vX.Y.Z version")
    return int(found.group(1)), int(found.group(2)), int(found.group(3))


class Rollback:
    def __init__(self, args: argparse.Namespace, env: dict[str, str], runner: Runner, log: Log) -> None:
        self.args, self.env, self.runner, self.log = args, env, runner, log
        self.tag = args.tag
        self.version = args.tag[1:]
        self.gh = env.get("GH_BIN") or "gh"
        base = env.get("RELEASE_PUBLIC_BASE", "")
        redacted = [env[name] for name in SECRET_ENV if env.get(name)]
        if base:
            redacted += [base.rstrip("/"), urllib.parse.urlsplit(base).hostname or ""]
        self.secrets = sorted({s for s in redacted if s}, key=len, reverse=True)
        self.child = {k: v for k, v in env.items() if k not in SECRET_ENV}  # the signer, systemctl and git get no credential
        self.gh_env = {k: v for k, v in env.items() if k not in R2_ENV}  # gh keeps its own token and never sees R2
        self.pausing = args.live and not args.keep_train
        self.paused = {"variable": False, "timer": False}
        self.dispatched: str | None = None
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
        self.log.event("step", step=step, outcome="ok", error="", **fields)

    def cmd(self, step: str, argv: list[str], cwd: pathlib.Path | str | None = None, env: dict | None = None,
            ok_codes: tuple[int, ...] = (0,), timeout: float | None = None) -> subprocess.CompletedProcess:
        result = self.runner(argv, cwd=str(cwd) if cwd else None, env=env if env is not None else self.child, timeout=timeout)
        if result.returncode not in ok_codes:
            name = pathlib.Path(argv[0]).name
            raise StepFailure(step, f"{name} {' '.join(argv[1:3])} exited {result.returncode}:{self.tails(name, result)}")
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

    # ---- the signer -----------------------------------------------------------------------------------------------

    def prepare_signer(self) -> None:
        if self.env.get("RELEASE_SIGNER_BIN"):
            self.signer_bin = pathlib.Path(self.env["RELEASE_SIGNER_BIN"])
            self.signer_dir = pathlib.Path(self.env["RELEASE_SIGNER_DIR"])
            return
        step = "build_signer"
        home = pathlib.Path(os.path.expanduser("~"))
        repo = str(pathlib.Path(self.env.get("QUEST_RELEASE_SIGNER_REPO") or home / "src" / "release-signer"))
        root = pathlib.Path(self.env.get("NEVR_WORK_ROOT") or home / "src" / "nevr-runtime-work") / "rollback-beta"
        try:
            root.mkdir(parents=True, exist_ok=True)
            readme = root / "README.md"
            if not readme.exists():
                readme.write_text("Scratch of tools/rollback_beta.py (the beta rollback). Each run removes what it makes here "
                                  "when it ends.\n", encoding="utf-8")
            self.scratch = root / f"signer-{self.log.base['run']}"
            self.scratch.mkdir()
        except OSError as error:
            raise StepFailure(step, f"cannot make the scratch folder {root}: {error.strerror}") from error
        tarball = self.scratch / "release-signer.tar"
        self.signer_dir = self.scratch / "snapshot"
        self.signer_bin = self.signer_dir / SIGNER_BINARY
        self.cmd(step, ["git", "-C", repo, "fetch", "origin", "main"])
        sha = self.cmd(step, ["git", "-C", repo, "rev-parse", "origin/main"]).stdout.strip()
        self.cmd(step, ["git", "-C", repo, "archive", "--format=tar", "-o", str(tarball), "origin/main"])
        self.signer_dir.mkdir()
        self.cmd(step, ["tar", "-xf", str(tarball), "-C", str(self.signer_dir)])
        self.cmd(step, ["go", "build", "-o", str(self.signer_bin), "./cmd/release-signer"], self.signer_dir)
        self.ok(step, signer_commit=sha)

    def cleanup(self) -> str:
        """Remove exactly the snapshot this run made. Returns an error text, or an empty string."""
        if self.scratch is None:
            return ""
        scratch, self.scratch = self.scratch, None
        try:
            shutil.rmtree(scratch)
        except OSError as error:
            return f"cannot remove {scratch}: {error.strerror}"
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
        listed = {(a.get("name"), a.get("sha256")) for a in self.artifacts_of(live["manifest"], self.version)}
        for claim in claims:
            if not (isinstance(claim, dict) and isinstance(claim.get("name"), str) and SHA256.match(str(claim.get("sha256")))):
                raise StepFailure(step, "listed-claims printed an entry without a name and a sha256")
            if (claim["name"], claim["sha256"]) not in listed:
                raise Refusal(step, f"the claim for {claim['name']} is not what the manifest lists for {self.tag}")
        return claims

    def plan(self, live: dict, beta: str, direction: str, override: bool) -> None:
        manifest = live["manifest"]
        if self.args.live and self.pausing:
            train = f"will be paused first ({TRAIN_VARIABLE}=false, {TIMER_UNIT} stopped)"
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

    def pause_variable(self) -> None:
        step = "pause_variable"
        self.gh_cmd(step, ["variable", "set", TRAIN_VARIABLE, "--body", "false", "-R", RELEASE_SLUG])
        listed = self.gh_cmd(step, ["variable", "list", "-R", RELEASE_SLUG, "--json", "name,value"]).stdout
        try:
            value = next((v.get("value") for v in json.loads(listed) if v.get("name") == TRAIN_VARIABLE), None)
        except (ValueError, AttributeError) as error:
            raise StepFailure(step, f"the variable list is unreadable: {error}") from error
        if value != "false":
            raise StepFailure(step, f"{TRAIN_VARIABLE} is {value!r} after the set, not 'false'")
        self.paused["variable"] = True
        self.ok(step)

    def pause_timer(self) -> None:
        step = "pause_timer"
        self.cmd(step, ["systemctl", "--user", "stop", TIMER_UNIT])
        active = self.cmd(step, ["systemctl", "--user", "is-active", TIMER_UNIT], ok_codes=(0, 3, 4))
        if active.returncode == 0:
            raise StepFailure(step, f"{TIMER_UNIT} is still active after the stop")
        self.paused["timer"] = True
        running = self.cmd(step, ["systemctl", "--user", "is-active", SERVICE_UNIT], ok_codes=(0, 3, 4))
        if running.returncode == 0:
            raise StepFailure(step, f"{SERVICE_UNIT} is running a Quest release now: wait for it to end, then run this again "
                                    "(the timer is stopped, so it will not start another)")
        self.ok(step)

    def recheck(self, beta: str) -> dict:
        step = "recheck"
        live = self.read_live(step)
        now = self.beta_of(live)
        if now != beta:
            raise StepFailure(step, f"beta moved while the train was being paused: it was v{beta}, it is now v{now or 'none'}; "
                                    "nothing was dispatched")
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
        found = RUN_ID.search(self.gh_cmd(step, argv).stdout)
        if not found:
            raise StepFailure(step, "the dispatch printed no run URL")
        self.dispatched = found.group(1)
        self.ok(step, run_id=self.dispatched, dry_run=dry, override=override)
        self.say(f"run {self.dispatched} dispatched (dry_run={str(dry).lower()}, override={str(override).lower()})")
        return self.dispatched

    def watch(self, run_id: str) -> None:
        step = "watch"
        try:
            self.gh_cmd(step, ["run", "watch", run_id, "-R", SIGNER_SLUG, "--exit-status"], timeout=WATCH_TIMEOUT)
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

    def verify_live(self, before: dict, claims: list[dict]) -> dict:
        step = "verify_live"
        after = self.read_live(step)
        now = self.beta_of(after)
        if now != self.version:
            raise StepFailure(step, f"the live manifest lists beta {now or 'nothing'}, not {self.version}")
        listed = {(a.get("name"), a.get("sha256")) for a in self.artifacts_of(after["manifest"], self.version)}
        wanted = {(a.get("name"), a.get("sha256")) for a in self.artifacts_of(before["manifest"], self.version)}
        wanted |= {(c["name"], c["sha256"]) for c in claims}
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
        if self.pausing:
            self.pause_variable()
            self.pause_timer()
            live = self.recheck(beta)
        started = time.monotonic()
        run_id = self.dispatch(claims, override, dry=not args.live)
        self.watch(run_id)
        jobs = self.check_jobs(run_id, dry=not args.live)
        if not args.live:
            self.verify_unchanged(beta)
            self.say(f"dry run ok: {jobs}; beta is still v{beta}")
            self.say(f"elapsed {round(time.monotonic() - started)} s")
            return
        after = self.verify_live(live, claims)
        self.say(f"{'rolled forward' if direction == 'forward' else 'rolled back'}: beta is {self.tag}, listed with every artifact "
                 f"by verify-manifest --live (manifest_version {after['state'].get('manifest_version')})")
        self.say(f"run {run_id}")
        self.say(f"elapsed {round(time.monotonic() - started)} s (dispatch to verified)")
        if direction == "forward":
            self.say(f"undo: python3 tools/rollback_beta.py v{beta} --live --yes")
        else:
            self.say(f"roll forward: python3 tools/rollback_beta.py v{beta} --live --forward --yes")
        if self.pausing:
            self.say("the train is STILL PAUSED and stays paused until someone resumes it. Resume it with:")
            self.say(self.resume_commands())

    def resume_commands(self) -> str:
        return (f"  {self.gh} variable set {TRAIN_VARIABLE} --body true -R {RELEASE_SLUG}\n"
                f"  systemctl --user start {TIMER_UNIT}")

    def state_report(self) -> list[str]:
        """What a live run that failed after it began to pause did and did not do, one fact per line."""
        if not self.pausing:
            return []
        facts = [f"{TRAIN_VARIABLE} is false" if self.paused["variable"] else f"{TRAIN_VARIABLE} is NOT false",
                 "the timer is stopped" if self.paused["timer"] else "the timer is NOT stopped",
                 (f"publish run {self.dispatched} was dispatched and its outcome is not confirmed" if self.dispatched
                  else "no publish was dispatched")]
        undone = [name for name, done in (("pausing the variable", self.paused["variable"]), ("stopping the timer", self.paused["timer"]),
                                          ("the publish", self.dispatched is not None)) if not done]
        lines = ["state: " + "; ".join(facts)]
        if undone:
            lines.append("NOT done: " + ", ".join(undone))
        if self.paused["variable"] or self.paused["timer"]:
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


def main(argv: list[str], env: dict[str, str], runner: Runner = subprocess_runner) -> int:
    args = build_parser().parse_args(argv)
    log = None
    rollback = None
    try:
        log = Log(state_log_path(env), args.tag)
        log.base.update({"run": uuid.uuid4().hex, "direction": ""})
        log.event("start", step="start", outcome="ok", error="", dry_run=args.dry_run, forward=args.forward,
                  keep_train=args.keep_train)
        rollback = Rollback(args, env, runner, log)
        if not TAG.match(args.tag):
            raise Refusal("tag", f"{args.tag} is not a release tag (vX.Y.Z)")
        rollback.run()
        problem = rollback.cleanup()
        if problem:
            raise StepFailure("cleanup", problem)
        log.event("finish", step="done", outcome="ok", error="")
        return 0
    except StepFailure as failure:
        return fail(log, rollback, failure.step, str(failure), "refused" if isinstance(failure, Refusal) else "failed")
    except Failure as failure:
        sys.stderr.write(f"rollback-beta: {failure}\n")
        return 1
    finally:
        if rollback is not None:
            leftover = rollback.cleanup()
            if leftover:
                sys.stderr.write(f"rollback-beta: cleanup: {leftover}\n")
        if log is not None:
            log.close()


def fail(log: Log | None, rollback: Rollback | None, step: str, message: str, outcome: str) -> int:
    redact = rollback.redact if rollback is not None else (lambda text: text)
    sys.stderr.write(f"rollback-beta: {step}: {redact(message)}\n")
    if rollback is not None:
        for line in rollback.state_report():
            sys.stderr.write(redact(line) + "\n")
    if log is not None:
        try:
            log.event("finish", step=step, outcome=outcome, error=redact(message))
        except Failure as log_failure:
            sys.stderr.write(f"rollback-beta: {log_failure}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
