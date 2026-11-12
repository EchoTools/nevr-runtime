#!/usr/bin/env python3
"""The Quest half of a release, run by hand by the maintainer who holds the Quest signing key, after the release is published
(RELEASING.md, "The Quest half of a release"; #1210). Usage: quest_release_timer.py --tag vX.Y.Z | --tag vX.Y.Z --reset-failures

The store APK, the Quest signing key and the R2 credential stay with the maintainer who runs this, so no workflow builds the Quest APK.
Given the published release's tag, this tool finishes the release:

  checkout         the checkout this runs from is clean and HEAD is the freshly fetched origin/main, or it refuses
  tags             every local tag is on origin at the same commit (the release preflight's rule), or it refuses naming up to
                   ten tags with their short ids, counting the rest, and printing the quoted remove and restore commands (with
                   the full object id) for each tag it lists
  token            the GitHub token of the gh in use
  build_signer     the release signer, built from a snapshot of the main of the checkout QUEST_RELEASE_SIGNER_REPO names
  check_listed     the live manifest already lists the tag: no release build is made and the run goes on to the variant;
                   else the game data it carries
  worktree         a clean worktree at the tag, set up for a build
  build_slot       waits while two or more ninja processes run
  build_apk        `just release-quest-apk` in the shared build slice
  publish_apk      tools/publish_quest_apk.py: R2, fetched back, and the manifest claim
  arena_*          the arena APK (the store APK with the loader, no combat), listed as the kind quest-apk-arena in the same
                   manifest publish: arena_gate (the signer takes `--kind`, else `arena_skipped`), arena_build, arena_publish.
                   A failure from the gate on blocks the dispatch (`part` arena, `reason` arena_blocks_dispatch)
  dispatch, watch  the beta manifest publish, live, in the release-signer repository
  check_jobs       the jobs that publish all succeeded (a green run with a skipped job is a failure)
  verify_live      `verify-manifest --live` lists the tag on beta, and the manifest it verified lists the carried game data
  variant_*        the unlisted variant APK, below
  cleanup          the worktree and the signer snapshot, by exact path

The variant. Every release also gets an unlisted second APK, nevr-runtime-<tag>-q15t-quest.apk, in the same run, built
from the release's own loader asset and stamped with the variant word `q15t` (docs/adr/0040 rule 1), which turns the combat area
on (docs/adr/0046 rule 5), and published beside the main APK. It carries no plugin library: every combat module is the
sentinel's own. `just release-quest-variant-apk` repacks the store game in the tag's worktree.
The repack needs an Android NDK (ANDROID_NDK_HOME) and a vcpkg checkout (VCPKG_ROOT) because the variant_configure step runs
`just configure-android` (justfile: `configure-android: _build-guard _require-ndk`), whose arm64-android ports of
src/quest/vcpkg.json hold the nlohmann/json headers the repack's libr15 patcher is built with (the `json_trees` list in
`android-repack-apk`); it also needs config/public-defaults.env in the checkout the timer runs from. A run that lacks one is
refused before any step, with a line that names it. The variant steps run after the main release is complete (published, listed and
verified live) and never before:

  variant_exists     (a listed tag only) the host's variant pair: 404 for the APK is absent (it is built and published); an
                     APK that is there (200, not HTML) is listed only when its `.sha256` is there too, is the one line
                     `<sha256>  <variant name>` and states the sha256 of the APK as the host serves it (`variant_listed`,
                     nothing built); a missing, malformed or disagreeing sidecar, and any other answer, is a failure that
                     names the files and rebuilds nothing, because an upload never replaces an object
  variant_configure  `just configure-android` in the tag's worktree (the Quest build tree and its vcpkg ports), in the same
                     slice as the main build; for a tag that was already listed the run makes the worktree and the PC configure
                     itself first
  variant_build      `just release-quest-variant-apk` in the same worktree. Its output goes to the 0600 file
                     <work>/variant-<tag>-build.log, kept when the step fails and removed when the variant is done. After it
                     the run writes no per-library record: the APK carries no plugin library
  variant_publish    tools/publish_quest_apk.py --variant: R2 and the public host only, no signer, no manifest claim

A failure of a variant step carries the step, the program and its exit code, never a tail of the program's output;
variant_build's failure names the 0600 file.

A failing variant step is recorded as `failed` with its step name, exits 1 and
counts toward the same three tries, but the main release is by then complete and nothing of it is undone. `done` is written
only when the variant is done as well, so the next run (the tag is listed by then) builds only the variant. Every
`failed` and `refused` record carries `part`: "main" until the main release is complete (`main_complete` is written then, once,
and a run whose tag has it starts as "variant"), "variant"
after that; the record that the timer gave up on a tag says which part it was. An exception no step expected is recorded as a
`failed` of the step `unexpected` (its class and where, never its text), what the run made is removed, and the run exits 1.

A tag whose history has `main_complete` takes the variant-only path whatever beta lists (`variant_only`: no signer, no
check_listed, no dispatch), so a beta that moved never makes the main release run again. A run holds
a non-blocking lock on <work>/timer.lock; one that finds it held records `busy` and exits 0 having done nothing.

Every step appends a JSON line to $XDG_STATE_HOME/nevr-runtime/quest-release-timer.jsonl; a log that cannot be written is
a failure, and that log is the record of each run. A failed step exits 1 and names itself, with the tail of the failing command's stderr and of its stdout (not the stdout of curl, gh or the signer, which is remote content). A refusal
(`refused`: a dirty or stale checkout, a tag origin does not have) exits 1 and is not counted. A tag that has failed three times is not retried, and says so in a `skipped_after_failures` record
(with the last failed step and its part) before the run exits 0. `--tag vX.Y.Z --reset-failures` records `failures_reset` for that tag and does
nothing else; only the failures after the newest one count toward the three. The R2 credential goes only to publish_apk and
variant_publish, and the GitHub token only to publish_apk and the dispatch: every other command gets an environment without them. The environment is
the R2 credential (R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY), NEVR_RCLONE_REMOTE, RELEASE_PUBLIC_BASE,
NEVR_QUEST_REFERENCE_APK (the reference build `just release-quest-apk` repacks, #1994), NEVR_STORE_APK (the store APK the variant is
repacked from, a file), ANDROID_NDK_HOME and VCPKG_ROOT (the Quest
build); a missing one, an ANDROID_NDK_HOME that is not an NDK r26d (its source.properties says another revision), a VCPKG_ROOT that is not a vcpkg checkout, and a checkout without
config/public-defaults.env are refusals that name it. Nothing written is masked.
"""

import argparse
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

# QUEST_RELEASE_SIGNER_REPO is the path of a local clone of the release-signer repository (build_signer builds from its main).
# NEVR_QUEST_REFERENCE_APK is the reference build the release APK is repacked from (`just release-quest-apk`, #1994).
# NEVR_STORE_APK is the store APK the variant is repacked from (`just release-quest-variant-apk`); it is read after the main release
# is published, so a run without it is refused before that, as for the build inputs below.
# ANDROID_NDK_HOME and VCPKG_ROOT are the inputs of the variant's configure step (`just configure-android`, which the repack's libr15
# patcher build reads its nlohmann/json headers from): a run without them is refused before the main release is published, not after.
REQUIRED_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "RELEASE_PUBLIC_BASE",
                "NEVR_RCLONE_REMOTE", "QUEST_RELEASE_SIGNER_REPO", "NEVR_QUEST_REFERENCE_APK", "NEVR_STORE_APK", "ANDROID_NDK_HOME", "VCPKG_ROOT")
SECRET_ENV = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "NEVR_RCLONE_REMOTE")
# What a set ANDROID_NDK_HOME / VCPKG_ROOT must hold for the Quest build to find it (src/quest/CMakePresets.json, the justfile's recipes).
NDK_REVISION = re.compile(r"26\.3\.[0-9]+")  # r26d's Pkg.Revision in the NDK's source.properties (the revision is 26.3.x, never r26a-c)
NDK_FILES = ("build/cmake/android.toolchain.cmake", "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm")
VCPKG_FILE = "scripts/buildsystems/vcpkg.cmake"
PUBLIC_DEFAULTS = "config/public-defaults.env"  # in the checkout the timer runs from: `just worktree-setup` copies it into the tag's worktree
VARIANT = "q15t"
# The arena APK (#2298): the store APK with the release's loader and no combat, listed in the manifest as the kind quest-apk-arena. It goes
# into the same manifest publish as the main APK, so a failure of its steps before the dispatch blocks the dispatch (a tag is published once).
ARENA = "arena"
ARENA_KIND = "quest-apk-arena"
ARENA_BLOCKS = "arena_blocks_dispatch"
SIGNER_KIND_FLAG = re.compile(r"^[ \t]+--?kind\b", re.MULTILINE)  # in `put-quest-apk -h`; Go's flag package prints one dash
MANIFEST_PATH = "nevr-runtime/manifest.json"
USER_AGENT = "nevr-runtime-quest-release-timer"  # the release host answers 403 to the default agent
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")  # always fullmatch: `$` would pass a trailing newline
RUN_ID = re.compile(r"/actions/runs/([0-9]+)")
SIGNER_SLUG = "thesprockee/release-signer"
PUBLISHING_JOBS = ("Commit the manifest record", "Publish to R2", "A run that published nothing is red")
MAX_FAILURES = 3
GAME_DATA_PLATFORM = "android-arm64"  # the Quest half of a release carries the game-data entry of this platform only
SLOT_WAITS = 60
SLOT_WAIT_SECONDS = 60
TAIL = 2000
APK_LIMIT = 4 << 30  # the most the variant APK check reads from the host
SIDECAR_LIMIT = 4096
SIDECAR_LINE = re.compile(r"([0-9a-f]{64})  (\S+)\n?")  # what `sha256sum` writes, one line
READ_CHUNK = 1 << 20
SIGNER_BINARY = "release-signer"  # what build_signer builds; its `verify-manifest --live` prints the live manifest on stdout
# Programs whose stdout is remote content, never a log field (docs/standards/logging.md, Rule 4).
REMOTE_OUTPUT_PROGRAMS = frozenset({"curl", "gh", SIGNER_BINARY})
MAX_TAGS_LISTED = 10
LOCAL_TAGS_FORMAT = "--format=%(refname:strip=2)\t%(objectname)\t%(*objectname)"  # release_preflight.local_tags' format

Runner = Callable[..., subprocess.CompletedProcess]


class StepFailure(Failure):
    def __init__(self, step: str, message: str, reason: str = "") -> None:
        super().__init__(message)
        self.step = step
        self.reason = reason  # a token for the person cutting the release, when the step has one (the arena steps)


class Refusal(StepFailure):
    """A precondition that is not met (a dirty checkout, a checkout not at origin/main): logged as `refused`, never counted."""


def repo_path(env: dict[str, str]) -> pathlib.Path:
    """The checkout the timer runs from (QUEST_RELEASE_REPO, else the clone this script sits in)."""
    return pathlib.Path(env.get("QUEST_RELEASE_REPO") or pathlib.Path(__file__).resolve().parents[1])


def ndk_revision(ndk: pathlib.Path) -> str | None:
    """The `Pkg.Revision` of an NDK directory's source.properties, None when the file or the key is absent or unreadable."""
    try:
        text = (ndk / "source.properties").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    found = re.search(r"^Pkg\.Revision[ \t]*=[ \t]*(\S+)[ \t]*$", text, re.MULTILINE)
    return found.group(1) if found else None


def build_input_problems(env: dict[str, str]) -> list[tuple[str, str]]:
    """What the Quest build cannot find, as (name, sentence) pairs: an ANDROID_NDK_HOME that is not an NDK r26d (its source.properties says another revision), a VCPKG_ROOT that is not
    a vcpkg checkout, a NEVR_STORE_APK that is not a file, and a checkout without config/public-defaults.env. An unset variable is not reported here (REQUIRED_ENV
    names it). The sentence names what is missing relative to the variable, never the variable's value."""
    problems = []
    ndk = env.get("ANDROID_NDK_HOME")
    if ndk:
        lacking = [name for name in NDK_FILES if not (pathlib.Path(ndk) / name).is_file()]
        revision = ndk_revision(pathlib.Path(ndk))
        if revision is None:
            lacking.append("source.properties with a Pkg.Revision")
        if lacking:
            problems.append(("ANDROID_NDK_HOME", f"ANDROID_NDK_HOME is not an Android NDK r26d directory (it lacks {', '.join(lacking)})"))
        elif not NDK_REVISION.fullmatch(revision):
            problems.append(("ANDROID_NDK_HOME", f"ANDROID_NDK_HOME is not an Android NDK r26d directory (its source.properties says "
                                                 f"Pkg.Revision {revision}, r26d is 26.3.x)"))
    vcpkg = env.get("VCPKG_ROOT")
    if vcpkg and not (pathlib.Path(vcpkg) / VCPKG_FILE).is_file():
        problems.append(("VCPKG_ROOT", f"VCPKG_ROOT is not a vcpkg checkout (it lacks {VCPKG_FILE})"))
    store = env.get("NEVR_STORE_APK")
    if store and not pathlib.Path(store).is_file():
        problems.append(("NEVR_STORE_APK", "NEVR_STORE_APK is not a file (the variant is repacked from the store APK)"))
    if not (repo_path(env) / PUBLIC_DEFAULTS).is_file():
        problems.append((PUBLIC_DEFAULTS, f"{PUBLIC_DEFAULTS} is missing from the checkout the timer runs from "
                                          "(copy config/public-defaults.env.example and fill in the four public values)"))
    return problems


def work_root(env: dict[str, str]) -> pathlib.Path:
    """The scratch directory of the timer (QUEST_RELEASE_WORK_ROOT, else beside the log under $XDG_STATE_HOME); its lock lives in it."""
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


class Timer:
    def __init__(self, env: dict[str, str], runner: Runner, sleep: Callable[[float], None], log: Log) -> None:
        self.env, self.runner, self.sleep, self.log = env, runner, sleep, log
        self.r2 = {name: env[name] for name in SECRET_ENV if env.get(name)}
        self.token = ""
        self.gh = env.get("GH_BIN", "gh")
        self.repo = repo_path(env)
        self.signer_repo = pathlib.Path(env["QUEST_RELEASE_SIGNER_REPO"])  # REQUIRED_ENV: main() refuses a run without it
        self.work = work_root(env)
        # What every command gets. The R2 credential and the GitHub token are added by the one or two steps that use them.
        self.child = {k: v for k, v in env.items() if k not in SECRET_ENV and k != "GH_TOKEN"}
        self.part = "main"  # what a failure now belongs to: the main release, or the variant that follows it
        self.main_done = False  # the tag's history has `main_complete`: only the variant is left, whatever beta lists

    def cmd(self, step: str, argv: list[str], cwd: pathlib.Path | str | None = None, extra: dict | None = None,
            ok_codes: tuple[int, ...] = (0,), quiet: bool = False,
            output_file: pathlib.Path | None = None) -> subprocess.CompletedProcess:
        """Run a command. A failure carries the program, its exit code and the tails of its output, unless `quiet`
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
        """The command's whole output, in a file that is 0600 from the moment it is opened (a run replaces it)."""
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
                handle.write(f"exit {result.returncode}\n--- stderr ---\n{result.stderr or ''}\n"
                             f"--- stdout ---\n{result.stdout or ''}\n")
        except OSError as error:
            raise StepFailure(step, f"cannot write the output file: {error.strerror}") from error

    def tails(self, program: str, result: subprocess.CompletedProcess) -> str:
        """` stderr: <tail> | stdout: <tail>`, an empty stream left out, and no stdout of a program in
        REMOTE_OUTPUT_PROGRAMS (or the gh binary in use)."""
        name = pathlib.Path(program).name
        remote = name in REMOTE_OUTPUT_PROGRAMS or name == pathlib.Path(self.gh).name
        parts = []
        for label, text in (("stderr", result.stderr), ("stdout", "" if remote else result.stdout)):
            tail = (text or "")[-TAIL:].strip()
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
        """The checkout this runs from is the code that ships: clean, and exactly the freshly fetched origin/main. A clean
        checkout that is behind origin/main (HEAD is an ancestor of it) is advanced to it by a fast-forward-only merge, the
        old and new sha logged (#1945). A dirty checkout, one that has diverged, or one ahead of origin/main is refused."""
        step = "checkout"
        repo = str(self.repo)
        self.cmd(step, ["git", "-C", repo, "fetch", "--quiet", "origin", "main"])
        dirty = self.cmd(step, ["git", "-C", repo, "status", "--porcelain"]).stdout.strip()
        if dirty:
            raise Refusal(step, f"the checkout the timer runs from is not clean: {dirty.splitlines()[0]}")
        head = self.cmd(step, ["git", "-C", repo, "rev-parse", "HEAD"]).stdout.strip()
        main = self.cmd(step, ["git", "-C", repo, "rev-parse", "origin/main"]).stdout.strip()
        if head != main:
            behind = self.cmd(step, ["git", "-C", repo, "merge-base", "--is-ancestor", "HEAD", "origin/main"],
                              ok_codes=(0, 1)).returncode == 0
            if not behind:
                raise Refusal(step, f"the checkout the timer runs from is at {head[:12]}, not the fetched origin/main {main[:12]}")
            self.cmd(step, ["git", "-C", repo, "merge", "--ff-only", "--quiet", "origin/main"])
            self.log.event("checkout_fast_forward", step=step, from_sha=head, to_sha=main)
            head = self.cmd(step, ["git", "-C", repo, "rev-parse", "HEAD"]).stdout.strip()
            if head != main:
                raise Refusal(step, f"the fast-forward left the checkout at {head[:12]}, not the fetched origin/main {main[:12]}")
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

    def release(self, tag: str) -> None:
        version = tag[1:]
        signer_dir = self.work / f"release-signer-{tag}"
        tarball = self.work / f"release-signer-{tag}.tar"
        extra_file = self.work / f"extra-{tag}.json"
        worktree = self.work / tag
        arena_file = self.work / f"extra-arena-{tag}.json"
        made = {"worktree": False}
        try:
            self._release_steps(tag, version, worktree, signer_dir, tarball, extra_file, made, arena_file)
        except Exception:  # a StepFailure, or anything a step did not expect: what this run made is removed either way
            self.cleanup(worktree if made["worktree"] else None, signer_dir, tarball, extra_file, raising=False, arena_file=arena_file)
            raise
        self.cleanup(worktree if made["worktree"] else None, signer_dir, tarball, extra_file, raising=True, arena_file=arena_file)

    def _release_steps(self, tag: str, version: str, worktree: pathlib.Path, signer_dir: pathlib.Path,
                       tarball: pathlib.Path, extra_file: pathlib.Path, made: dict, arena_file: pathlib.Path) -> None:
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

        publish_env = {**self.r2, "GH_TOKEN": self.token, "RELEASE_SIGNER_BIN": str(binary),
                       "RELEASE_SIGNER_DIR": str(signer_dir), "GH_BIN": self.gh}
        self.cmd("publish_apk", ["python3", "tools/publish_quest_apk.py", tag, "--extra-out", str(extra_file)],
                 worktree, publish_env)
        claims = self.read_claims("publish_apk", extra_file)
        self.ok("publish_apk")

        arena_claims = self.arena_steps(tag, worktree, build_env, binary, signer_dir, publish_env, arena_file)
        claim = json.dumps([*claims, *arena_claims], separators=(",", ":"))

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
        for wanted in arena_claims:
            self.check_arena_listed(self.live_manifest(after, "verify_live"), version, wanted)
        self.ok("verify_live", beta=listed, game_data=carried["name"], arena=[c["name"] for c in arena_claims])
        self.mark_main_complete(tag)
        self.finish_variant(tag, worktree, made, build_env)

    @staticmethod
    def read_claims(step: str, path: pathlib.Path) -> list[dict]:
        """The signer's claim file: a JSON array of extra_artifacts entries."""
        try:
            claims = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise StepFailure(step, f"the manifest claim {path.name} is unreadable: {error}") from error
        if not isinstance(claims, list) or not all(isinstance(c, dict) for c in claims):
            raise StepFailure(step, f"the manifest claim {path.name} is not a list of claims")
        return claims

    def signer_lists_arena(self, binary: pathlib.Path, signer_dir: pathlib.Path) -> bool:
        """Whether the signer built from its main takes `--kind` on put-quest-apk (its usage text says so). The signer's usage text is
        read here and never logged."""
        step = "arena_gate"
        result = self.cmd(step, [str(binary), "put-quest-apk", "-h"], signer_dir, ok_codes=(0, 1, 2))
        return bool(SIGNER_KIND_FLAG.search(f"{result.stderr or ''}\n{result.stdout or ''}"))

    def arena_steps(self, tag: str, worktree: pathlib.Path, build_env: dict, binary: pathlib.Path, signer_dir: pathlib.Path,
                    publish_env: dict, arena_file: pathlib.Path) -> list[dict]:
        """The arena APK, built and put up after the main APK; its claim joins the main one in the one manifest publish. Returns the
        claims (none while the signer lacks `--kind`: recorded as `arena_skipped`, and the run is today's flow). From the gate on, a
        failure is recorded with part `arena` and the reason `arena_blocks_dispatch`: nothing is dispatched, and a run after the fix
        goes on (a tag is published once, so publishing without arena would lose it for good)."""
        try:
            lists_arena = self.signer_lists_arena(binary, signer_dir)
        except StepFailure as failure:
            raise StepFailure(failure.step, f"{failure}; the manifest was not published, so the release is not listed until a later run",
                              ARENA_BLOCKS) from failure
        if not lists_arena:
            self.log.event("arena_skipped", reason="signer_lacks_kind", kind=ARENA_KIND)
            return []
        self.ok("arena_gate", kind=ARENA_KIND)
        self.part = ARENA
        slice_command = ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G"]
        try:
            self.cmd("arena_build", [*slice_command, "just", "release-quest-arena-apk", tag], worktree, build_env, quiet=True)
            self.ok("arena_build", apk=f"nevr-runtime-{tag}-{ARENA}-quest.apk")
            self.cmd("arena_publish", ["python3", "tools/publish_quest_apk.py", tag, "--listed-variant", ARENA, "--extra-out",
                                       str(arena_file)], worktree, publish_env)
            claims = self.read_claims("arena_publish", arena_file)
        except StepFailure as failure:
            raise StepFailure(failure.step, f"{failure}; the manifest was not published, so the release is not listed until a "
                                            "later run (a tag is published once, and publishing without arena would lose it)",
                              ARENA_BLOCKS) from failure
        finally:
            self.part = "main"
        wanted = [c for c in claims if c.get("name") == f"nevr-runtime-{tag}-{ARENA}-quest.apk"]
        if len(wanted) != 1:
            raise StepFailure("arena_publish", f"the signer's claim lists {len(wanted)} entries for the arena APK, not one", ARENA_BLOCKS)
        self.ok("arena_publish", apk=wanted[0]["name"])
        return wanted

    @staticmethod
    def check_arena_listed(manifest: dict, version: str, wanted: dict) -> None:
        """The release the manifest lists carries the arena artifact as the kind quest-apk-arena, with the claimed name, size and sha256."""
        step = "verify_live"
        found = [a for a in manifest.get("releases", {}).get(version, {}).get("artifacts", []) if a.get("name") == wanted.get("name")]
        if len(found) != 1:
            raise StepFailure(step, f"the {version} release lists {len(found)} entries named {wanted.get('name')}, not one")
        have = found[0]
        if (have.get("kind"), have.get("size"), have.get("sha256")) != (ARENA_KIND, wanted.get("size"), wanted.get("sha256")):
            raise StepFailure(step, f"{wanted.get('name')} is listed as kind {have.get('kind')}, size {have.get('size')}, "
                                    f"sha256 {have.get('sha256')}; the claim was kind {ARENA_KIND}, size {wanted.get('size')}, "
                                    f"sha256 {wanted.get('sha256')}")

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

    @staticmethod
    def variant_apk_name(tag: str) -> str:
        return f"nevr-runtime-{tag}-{VARIANT}-quest.apk"

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
        repair = "; nothing is rebuilt or replaced, see RELEASING.md (The Quest half of a release, the variant pair) for the repair"
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
        if build_env is None:
            listed = self.variant_exists(tag)
            self.ok("variant_exists", listed=listed, apk=self.variant_apk_name(tag))
            if listed:
                self.log.event("variant_listed", apk=self.variant_apk_name(tag))
                self.forget_build_log(tag)
                return
            self.make_worktree(tag, worktree, made)
            build_env = self.prepare_build(worktree)
        slice_command = ["systemd-run", "--user", "--scope", "--slice=nevr-builds.slice", "-p", "MemoryMax=8G"]
        # The Quest build tree and its vcpkg ports, in the tag's worktree; the recipe configures again and finds them in place.
        self.cmd("variant_configure", [*slice_command, "just", "configure-android"], worktree, build_env)
        self.ok("variant_configure")
        # The recipe repacks the store game with the variant word; no plugin library is built or handed in.
        self.cmd("variant_build", [*slice_command, "just", "release-quest-variant-apk", tag], worktree, build_env,
                 output_file=self.variant_build_log(tag))
        self.ok("variant_build", apk=self.variant_apk_name(tag))
        # Only the R2 credential: the variant has no signer and no manifest claim. The publish tool's own log has its reasons.
        self.cmd("variant_publish", ["python3", "tools/publish_quest_apk.py", tag, "--variant", VARIANT], worktree, dict(self.r2),
                 quiet=True)
        self.ok("variant_publish", apk=self.variant_apk_name(tag))
        self.forget_build_log(tag)

    def cleanup(self, worktree: pathlib.Path | None, signer_dir: pathlib.Path, tarball: pathlib.Path,
                extra_file: pathlib.Path, raising: bool, arena_file: pathlib.Path | None = None) -> None:
        """Remove exactly what this run made. A failed removal is logged either way; it fails the run only when the
        release itself succeeded, so it never hides the failure that came first."""
        problems = []
        if worktree is not None:
            result = self.runner(["just", "remove-worktree", str(worktree)], cwd=str(self.repo), env=self.child)
            if result.returncode != 0:
                problems.append(f"remove-worktree exited {result.returncode}: {(result.stderr or '')[-TAIL:]}")
        try:
            if signer_dir.exists():
                shutil.rmtree(signer_dir)
            tarball.unlink(missing_ok=True)
            extra_file.unlink(missing_ok=True)
            if arena_file is not None:
                arena_file.unlink(missing_ok=True)
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
    parser.add_argument("--tag", required=True, help="the published release to finish, vX.Y.Z")
    parser.add_argument("--reset-failures", action="store_true",
                        help="with --tag: record that the failures of that tag so far no longer count toward the three tries")
    args = parser.parse_args(argv)
    path = state_log_path(env)
    if not TAG.fullmatch(args.tag):
        sys.stderr.write(f"quest-release-timer: {args.tag} is not a release tag (vX.Y.Z)\n")
        return 1
    log = None
    timer = None
    lock = None
    try:
        log = Log(path, args.tag)
        log.event("start")
        if args.reset_failures:
            cleared = len(counted_failures([e for e in read_events(path) if e.get("tag") == args.tag]))
            log.event("failures_reset", cleared=cleared)
            return 0
        missing = [name for name in REQUIRED_ENV if not env.get(name)]
        invalid = build_input_problems(env)
        if missing or invalid:
            parts = ([f"set {', '.join(missing)}"] if missing else []) + [sentence for _, sentence in invalid]
            log.event("refused", missing=[*missing, *(name for name, _ in invalid)], part="main")
            raise Failure(f"refused: {'; '.join(parts)}")
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
        timer.token = token
        log.event("step_ok", step="token")
        tag = args.tag
        log.base["tag"] = tag
        history = [e for e in read_events(path) if e.get("tag") == tag]
        timer.main_done = any(e.get("event") == "main_complete" for e in history)
        timer.part = "variant" if timer.main_done else "main"
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
    except Refusal as refusal:
        sys.stderr.write(f"quest-release-timer: {refusal.step}: {refusal}\n")
        if log is not None:
            _record(log, "refused", refusal.step, str(refusal), part_of(timer), refusal.reason)
        return 1
    except StepFailure as failure:
        sys.stderr.write(f"quest-release-timer: {failure.step}: {failure}\n")
        if log is not None:
            _record(log, "failed", failure.step, str(failure), ARENA if failure.reason == ARENA_BLOCKS else part_of(timer), failure.reason)
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
        return 1
    finally:
        if lock is not None:
            os.close(lock)  # the lock is released with the descriptor, however the run ended
        if log is not None:
            log.close()


def take_lock(work: pathlib.Path) -> int | None:
    """A non-blocking exclusive lock on <work>/timer.lock for the life of the process, so two hand `--tag` runs never work
    at once: both would publish the same tag. None when it is held."""
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
    # An older log may hold `failed` records with part "draft": they do not count toward the three.
    return [e for e in history_since_reset(history) if e.get("event") == "failed" and e.get("part") != "draft"]


def _record(log: Log, event: str, step: str, error: str, part: str = "main", reason: str = "") -> None:
    try:
        log.event(event, step=step, error=error, part=part, **({"reason": reason} if reason else {}))
    except Failure as log_failure:
        sys.stderr.write(f"quest-release-timer: {log_failure}\n")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
