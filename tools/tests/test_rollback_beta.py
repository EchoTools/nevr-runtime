"""`tools/rollback_beta.py` (#1513): point the beta channel of the signed manifest at an earlier listed release with one
command. The script runs as a subprocess against fakes that record what they were called with: `gh` and the
release signer are scripts in a directory the test controls, and the public host is a server on 127.0.0.1 that serves
the manifest file the fakes change. Nothing here reaches GitHub, systemd or a real host."""

import contextlib
import fcntl
import hashlib
import http.server
import io
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "rollback_beta.py"
sys.path.insert(0, str(ROOT / "tools"))

import rollback_beta  # noqa: E402 - the tool itself, for the tests that run it in this process

SECRETS = {
    "R2_ENDPOINT": "https://sentinel-endpoint.invalid",
    "R2_BUCKET": "sentinel-bucket",
    "R2_ACCESS_KEY_ID": "sentinel-access-key-id",
    "R2_SECRET_ACCESS_KEY": "sentinel-secret-access-key",
    "GH_TOKEN": "sentinel-gh-token",
}
KINDS = (("windows-zip", "windows-x86_64", "windows.zip"), ("quest-apk", "android-arm64", "quest.apk"),
         ("game-data", "android-arm64", "gamedata.zip"))
JOBS = {"guard": "Only this repo, only main", "copy": "Copy the release files to R2",
        "check": "Check the release and build the manifest", "sign": "Sign manifest.json",
        "record": "Commit the manifest record", "upload": "Publish to R2", "outcome": "A run that published nothing is red"}

# One module behind two shims (gh, the signer) and the build tools. Each call is appended to calls.jsonl
# with the credentials that reached it; config.json says what to break; state files hold the variable, the units and the
# manifest the host serves.
FAKES = r'''
import hashlib, json, os, pathlib, shutil, sys

D = pathlib.Path(os.environ["FAKE_DIR"])
CFG = json.loads((D / "config.json").read_text())


def record(who, argv):
    leak = {k: k in os.environ for k in ("R2_SECRET_ACCESS_KEY", "R2_ACCESS_KEY_ID", "GH_TOKEN")}
    with (D / "calls.jsonl").open("a") as f:
        f.write(json.dumps({"who": who, "argv": argv, "cwd": os.getcwd(), "leak": leak,
                            "prog": os.path.basename(sys.argv[0])}) + "\n")


def manifest():
    return json.loads((D / "manifest.json").read_text())


def save(m):
    (D / "manifest.json").write_text(json.dumps(m, indent=1))


def maybe_fail(key):
    if key in CFG.get("noise", {}):
        sys.stdout.write(CFG["noise"][key] + "\n")
    if key in CFG.get("rc", {}):
        sys.stderr.write(CFG.get("leak_stderr", "") + "\nfake failure of " + key + "\n")
        sys.exit(CFG["rc"][key])


def jobs(inputs):
    names = {"guard": "Only this repo, only main", "copy": "Copy the release files to R2",
             "check": "Check the release and build the manifest", "sign": "Sign manifest.json",
             "record": "Commit the manifest record", "upload": "Publish to R2", "outcome": "A run that published nothing is red"}
    dry = inputs.get("dry_run") == "true"
    out = {k: "success" for k in names}
    if dry:
        out["record"] = "skipped"
    out.update(CFG.get("jobs", {}))
    return [{"name": names[k], "conclusion": v} for k, v in out.items() if k not in CFG.get("omit_jobs", [])]


def dry_signed(inputs):
    """The manifest a dry run signs: the live one with beta on the tag, one version up, and what the config bends."""
    m = manifest()
    version = inputs["tag"][1:]
    mode = CFG.get("dry_signed", "ok")
    m["channels"]["beta"] = "5.16.1" if mode == "wrong_beta" else version
    m["manifest_version"] += 1
    rel = m["releases"]
    if mode == "extra_release":
        rel["9.9.9"] = rel["5.16.1"]
    if mode == "changed_other":
        rel[[v for v in rel if v != version][0]]["artifacts"][0]["sha256"] = "0" * 64
    if mode == "changed_artifact":
        rel[version]["artifacts"][0]["sha256"] = "0" * 64
    if mode == "other_channel":
        m["channels"]["stable"] = "5.16.1"
    if mode == "notes_differ":
        rel[version]["notes"] = ["released", "hotfix"]
    if mode == "published_differs":
        rel[version]["published"] = "2026-10-30T00:00:00Z"
    if mode == "field_differs":
        rel[version]["commit"] = "b" * 40
    return m


def gh(a):
    key = " ".join(a[:2])
    record("gh", a)
    maybe_fail(key)
    vars_path = D / "vars.json"
    if key == "variable set":
        if not CFG.get("variable_stuck"):
            vars_path.write_text(json.dumps({a[2]: a[a.index("--body") + 1]}))
        if CFG.get("change_target_on_pause"):
            m = manifest()
            m["releases"]["5.17.0"]["artifacts"][1]["sha256"] = "1" * 64
            save(m)
        if CFG.get("move_beta_on_pause"):
            m = manifest()
            m["releases"]["5.19.0"] = m["releases"]["5.18.0"]
            m["channels"]["beta"] = "5.19.0"
            save(m)
    elif key == "variable list":
        v = json.loads(vars_path.read_text())
        print(CFG["variable_list_raw"] if "variable_list_raw" in CFG else json.dumps([{"name": k, "value": x} for k, x in v.items()]))
    elif key == "run list":
        print(CFG["signer_runs_raw"] if "signer_runs_raw" in CFG else json.dumps(CFG.get("signer_runs", [])))
    elif key == "run download":
        dest = pathlib.Path(a[a.index("-D") + 1])
        dest.mkdir(parents=True)
        inputs = json.loads((D / "dispatch.json").read_text())
        if not CFG.get("dry_no_file"):
            (dest / "manifest.json").write_text("not json" if CFG.get("dry_garbage") else json.dumps(dry_signed(inputs)))
    elif key == "workflow run":
        inputs = dict(a[i + 1].split("=", 1) for i, x in enumerate(a) if x == "-f")
        (D / "dispatch.json").write_text(json.dumps(inputs))
        if inputs.get("dry_run") != "true" or CFG.get("publish_on_dry_run"):
            m = manifest()
            mode = CFG.get("publish", "ok")
            version = inputs["tag"][1:]
            if mode in ("ok", "drop_artifact"):
                m["channels"]["beta"] = version
            if mode == "wrong_beta":
                m["channels"]["beta"] = "5.16.1"
            if mode == "drop_artifact":
                arts = m["releases"][version]["artifacts"]
                m["releases"][version]["artifacts"] = [x for x in arts if x["kind"] != "quest-apk"]
            if mode == "changed_bytes":
                m["channels"]["beta"] = version
                m["releases"][version]["artifacts"][0]["sha256"] = "0" * 64
            if mode != "none":
                m["manifest_version"] += 1
            save(m)
        print("queued" if CFG.get("no_url") else "https://github.example/thesprockee/release-signer/actions/runs/4242")
    elif key == "run watch":
        print("watching")
    elif key == "run view":
        print(json.dumps({"jobs": jobs(json.loads((D / "dispatch.json").read_text()))}))


def signer(a):
    record("signer", a)
    maybe_fail(a[0])
    m = manifest()
    if a[0] == "verify-manifest":
        print("signature ok")
        digest = hashlib.sha256((D / "manifest.json").read_bytes()).hexdigest()
        print(json.dumps({"channels": m["channels"], "manifest_version": m["manifest_version"], "sha256": digest}))
    elif a[0] == "listed-claims":
        version = a[a.index("--tag") + 1][1:]
        if "claims" in CFG:
            print(json.dumps(CFG["claims"]))
        else:
            arts = m["releases"].get(version, {}).get("artifacts", [])
            print(json.dumps([{"name": x["name"], "sha256": x["sha256"], "size": x["size"]} for x in arts
                              if x["kind"] not in ("windows-zip", "quest-library")]))


def build_tool(who, a):
    record(who, a)
    maybe_fail(who)
    if who == "git" and "archive" in a:
        pathlib.Path(a[a.index("-o") + 1]).write_text("tar")
    elif who == "git" and "rev-parse" in a:
        print("d89eb9d" + "0" * 33)
    elif who == "go":
        shutil.copy(os.environ["FAKE_SIGNER_SHIM"], a[a.index("-o") + 1])


def main(who):
    a = sys.argv[1:]
    {"gh": gh, "signer": signer}.get(who, lambda argv: build_tool(who, argv))(a)
'''


class Host(http.server.BaseHTTPRequestHandler):
    """Serves the manifest file the fakes change."""

    directory = pathlib.Path(".")
    mode = "plain"

    def log_message(self, *args) -> None:
        pass

    def do_GET(self) -> None:
        if self.mode == "error500":
            self.send_response(500)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = (self.directory / "manifest.json").read_bytes()
        if self.mode == "tampered":
            body = body.replace(b"manifest_version", b"manifest_versioN")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def artifacts(version: str) -> list[dict]:
    out = []
    for kind, platform, suffix in KINDS:
        name = f"nevr-runtime-v{version}-{suffix}"
        out.append({"name": name, "kind": kind, "platform": platform, "path": f"v{version}/{name}",
                    "sha256": hashlib.sha256(name.encode()).hexdigest(), "size": 1000 + len(out)})
    return out


def manifest(beta: str) -> dict:
    return {"schema_version": 1, "product": "nevr-runtime", "manifest_version": 7, "channels": {"beta": beta},
            "releases": {v: {"version": v, "tag": f"v{v}", "commit": "a" * 40, "published": "2026-10-01T00:00:00Z",
                             "notes": ["released", "tested"], "artifacts": artifacts(v)} for v in ("5.16.1", "5.17.0", "5.18.0")}}


class RollbackCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = pathlib.Path(self.tmp.name)
        self.fake = self.base / "fake"
        self.bin = self.base / "bin"
        self.signer_dir = self.base / "signer"
        for d in (self.fake, self.bin, self.signer_dir):
            d.mkdir()
        (self.bin / "fakes.py").write_text(FAKES)
        for name in ("gh", "release-signer", "git", "go", "tar"):
            who = "signer" if name == "release-signer" else ("gh" if name.startswith("gh") else name)
            path = self.bin / name
            path.write_text(f'#!{sys.executable}\nimport sys\nsys.path.insert(0, "{self.bin}")\nimport fakes\nfakes.main("{who}")\n')
            path.chmod(0o755)
        self.set_manifest(manifest("5.18.0"))
        self.config: dict = {}
        self.write_config()
        (self.fake / "vars.json").write_text(json.dumps({"RELEASE_TRAIN": "true"}))
        self.timer_work = self.base / "timer-work"
        self.timer_work.mkdir()
        Host.directory, Host.mode = self.fake, "plain"
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Host)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.base_url = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.work = self.base / "work"
        self.log = self.base / "state" / "nevr-runtime" / "rollback-beta.jsonl"
        self.env = {**SECRETS, "PATH": f"{self.bin}:/usr/bin:/bin", "HOME": str(self.base), "GH_BIN": str(self.bin / "gh"),
                    "RELEASE_PUBLIC_BASE": self.base_url, "RELEASE_SIGNER_BIN": str(self.bin / "release-signer"),
                    "RELEASE_SIGNER_DIR": str(self.signer_dir), "XDG_STATE_HOME": str(self.base / "state"),
                    "FAKE_DIR": str(self.fake), "NEVR_WORK_ROOT": str(self.work), "FAKE_SIGNER_SHIM": str(self.bin / "release-signer"),
                    "QUEST_RELEASE_SIGNER_REPO": str(self.base / "signer-repo"),
                    "QUEST_RELEASE_WORK_ROOT": str(self.timer_work)}

    def reset_world(self) -> None:
        """The state a test starts from, for the subtests that run the tool more than once."""
        self.set_manifest(manifest("5.18.0"))
        (self.fake / "vars.json").write_text(json.dumps({"RELEASE_TRAIN": "true"}))
        for leftover in ("calls.jsonl", "dispatch.json"):
            (self.fake / leftover).unlink(missing_ok=True)
        self.log.unlink(missing_ok=True)

    @contextlib.contextmanager
    def timer_lock_held(self):  # noqa: ANN201 - a run of the Quest release timer holds <work>/timer.lock
        """Hold the timer's lock, as a run that does work does, for the length of the block."""
        fd = os.open(self.timer_work / "timer.lock", os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield
        finally:
            os.close(fd)

    def run_inproc(self, *args: str, env: dict | None = None, on_call=None) -> tuple[int, str, str]:  # noqa: ANN001
        """The tool in this process, its commands going through the same fakes. `on_call(argv)` may answer a command itself
        (return a CompletedProcess), or raise; every command's argv and timeout are kept in self.seen."""
        self.seen: list[tuple[list[str], float | None]] = []

        def runner(argv, cwd=None, env=None, timeout=None):  # noqa: ANN001, ANN202 - the tool's runner shape
            self.seen.append((argv, timeout))
            answer = on_call(argv) if on_call else None
            return answer if answer is not None else rollback_beta.subprocess_runner(argv, cwd=cwd, env=env, timeout=timeout)

        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = rollback_beta.main(list(args), env or self.env, runner)
        return code, out.getvalue(), err.getvalue()

    def timeout_of(self, *words: str) -> float | None:
        for argv, timeout in self.seen:
            if any(argv[i:i + len(words)] == list(words) for i in range(len(argv))):
                return timeout
        self.fail(f"no command with {words}: {[a for a, _ in self.seen]}")

    def write_config(self) -> None:
        (self.fake / "config.json").write_text(json.dumps(self.config))

    def configure(self, **config: object) -> None:
        self.config.update(config)
        self.write_config()

    def set_manifest(self, m: dict) -> None:
        (self.fake / "manifest.json").write_text(json.dumps(m, indent=1))

    def run_tool(self, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(TOOL), *args], env=env or self.env, capture_output=True, text=True,
                              timeout=120, cwd=str(self.base))

    def calls(self) -> list[dict]:
        path = self.fake / "calls.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def gh_calls(self, *words: str) -> list[list[str]]:
        return [c["argv"] for c in self.calls() if c["who"] == "gh" and c["argv"][:len(words)] == list(words)]

    def changes(self) -> list[dict]:
        """Every call that changes the train or the manifest: a variable set or a dispatch."""
        return [c for c in self.calls() if c["who"] == "gh" and c["argv"][:2] in (["variable", "set"], ["workflow", "run"])]

    def records(self) -> list[dict]:
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def dispatch_inputs(self) -> dict:
        calls = self.gh_calls("workflow", "run")
        self.assertEqual(len(calls), 1, calls)
        argv = calls[0]
        return dict(argv[i + 1].split("=", 1) for i, a in enumerate(argv) if a == "-f")

    def assertRefused(self, proc: subprocess.CompletedProcess, *words: str) -> None:
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("rollback-beta:", proc.stderr)
        self.assertNotIn("Traceback", proc.stderr)
        for word in words:
            self.assertIn(word, proc.stderr)
        self.assertEqual(self.changes(), [], "a refusal changed something")

    def everything_said(self, proc: subprocess.CompletedProcess) -> str:
        return proc.stdout + proc.stderr + (self.log.read_text() if self.log.exists() else "")


class Refusals(RollbackCase):
    def test_a_tag_the_manifest_does_not_list_is_refused(self) -> None:
        self.assertRefused(self.run_tool("v9.9.9", "--dry-run"), "v9.9.9", "not listed")

    def test_the_tag_that_already_is_beta_is_refused(self) -> None:
        self.assertRefused(self.run_tool("v5.18.0", "--dry-run"), "already beta")

    def test_a_higher_tag_needs_forward(self) -> None:
        self.set_manifest(manifest("5.16.1"))
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "--forward")

    def test_forward_refuses_a_tag_that_is_not_higher(self) -> None:
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run", "--forward"), "not higher")

    def test_a_tag_that_is_not_a_release_tag_is_refused(self) -> None:
        self.assertRefused(self.run_tool("5.17.0", "--dry-run"), "vX.Y.Z")

    def test_a_manifest_with_no_beta_is_refused(self) -> None:
        m = manifest("5.18.0")
        m["channels"] = {}
        self.set_manifest(m)
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "no beta")

    def test_live_without_yes_is_refused_before_anything_runs(self) -> None:
        proc = self.run_tool("v5.17.0", "--live")
        self.assertRefused(proc, "--yes")
        self.assertEqual(self.calls(), [], "a refused live run still called a command")

    def test_neither_dry_run_nor_live_is_a_usage_error(self) -> None:
        proc = self.run_tool("v5.17.0")
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("usage: rollback_beta.py", proc.stderr)
        self.assertEqual(self.calls(), [])

    def test_dry_run_and_live_together_are_a_usage_error(self) -> None:
        proc = self.run_tool("v5.17.0", "--dry-run", "--live", "--yes")
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("not allowed with", proc.stderr)
        self.assertEqual(self.calls(), [])

    def test_forward_on_the_way_back_to_a_lower_tag_is_refused_live_too(self) -> None:
        self.assertRefused(self.run_tool("v5.17.0", "--live", "--yes", "--forward"), "not higher")

    def test_a_missing_public_base_is_a_refusal_that_names_it(self) -> None:
        env = {k: v for k, v in self.env.items() if k != "RELEASE_PUBLIC_BASE"}
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run", env=env), "RELEASE_PUBLIC_BASE")
        self.assertEqual(self.calls(), [])

    def test_no_signer_and_no_signer_repo_is_a_refusal_that_names_the_variable(self) -> None:
        env = {k: v for k, v in self.env.items() if k not in ("RELEASE_SIGNER_BIN", "RELEASE_SIGNER_DIR", "QUEST_RELEASE_SIGNER_REPO")}
        proc = self.run_tool("v5.17.0", "--dry-run", env=env)
        self.assertRefused(proc, "QUEST_RELEASE_SIGNER_REPO")

    def test_a_signer_binary_without_its_directory_is_a_refusal_that_names_it(self) -> None:
        env = {k: v for k, v in self.env.items() if k != "RELEASE_SIGNER_DIR"}
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run", env=env), "RELEASE_SIGNER_DIR")

    def test_a_log_that_cannot_be_written_fails_before_any_command(self) -> None:
        blocker = self.base / "not-a-directory"
        blocker.write_text("a file where the state folder should go")
        proc = self.run_tool("v5.17.0", "--dry-run", env={**self.env, "XDG_STATE_HOME": str(blocker)})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("cannot write the log", proc.stderr)
        self.assertEqual(self.calls(), [], "a command ran with no log to record it")

    def test_an_empty_claim_list_is_refused(self) -> None:
        self.configure(claims=[])
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "listed-claims", "no artifacts")

    def test_claims_that_are_not_a_json_array_fail_the_run(self) -> None:
        self.configure(claims={"name": "not a list"})
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("claims: listed-claims did not print a JSON array", proc.stderr)
        self.assertEqual(self.changes(), [])

    def test_a_claim_without_a_name_and_a_sha256_fails_the_run(self) -> None:
        self.configure(claims=[{"name": "nevr-runtime-v5.17.0-quest.apk"}])
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("without a name and a sha256", proc.stderr)
        self.assertEqual(self.changes(), [])

    def test_a_tag_with_a_trailing_newline_is_not_a_release_tag(self) -> None:
        self.assertRefused(self.run_tool("v5.17.0\n", "--dry-run"), "release tag")

    def test_claims_without_the_quest_apk_are_refused_before_anything_changes(self) -> None:
        self.configure(claims=[{"name": "nevr-runtime-v5.17.0-gamedata.zip", "sha256": hashlib.sha256(b"nevr-runtime-v5.17.0-gamedata.zip").hexdigest(), "size": 1}])
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertRefused(proc, "claims", "missing ['nevr-runtime-v5.17.0-quest.apk']")
        self.assertEqual(self.gh_calls("variable"), [])

    def test_a_claim_for_an_artifact_github_holds_is_refused(self) -> None:
        zip_name = "nevr-runtime-v5.17.0-windows.zip"
        claims = [{"name": a["name"], "sha256": a["sha256"], "size": a["size"]} for a in artifacts("5.17.0")]
        self.configure(claims=claims)
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "claims", f"not expected ['{zip_name}']")

    def test_a_claim_listed_twice_is_refused(self) -> None:
        claims = [{"name": a["name"], "sha256": a["sha256"], "size": a["size"]} for a in artifacts("5.17.0")
                  if a["kind"] != "windows-zip"]
        self.configure(claims=claims + claims[:1])
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "claims", "not expected")

    def test_every_artifact_kind_but_the_two_github_holds_must_be_claimed(self) -> None:
        m = manifest("5.18.0")
        pack = {"name": "nevr-runtime-v5.17.0-pack.zip", "kind": "content-pack", "platform": "android-arm64",
                "sha256": hashlib.sha256(b"pack").hexdigest(), "size": 5}
        library = {"name": "nevr-runtime-v5.17.0-quest-lib.so", "kind": "quest-library", "platform": "android-arm64",
                   "sha256": hashlib.sha256(b"lib").hexdigest(), "size": 6}
        m["releases"]["5.17.0"]["artifacts"] += [pack, library]
        self.set_manifest(m)
        claimed = [{"name": a["name"], "sha256": a["sha256"], "size": a["size"]} for a in m["releases"]["5.17.0"]["artifacts"]
                   if a["kind"] in ("quest-apk", "game-data")]
        self.configure(claims=claimed)
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "claims", "nevr-runtime-v5.17.0-pack.zip")
        self.configure(claims=claimed + [{"name": pack["name"], "sha256": pack["sha256"], "size": 5}])
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_a_claim_sha256_with_a_trailing_newline_is_malformed(self) -> None:
        self.configure(claims=[{"name": "nevr-runtime-v5.17.0-quest.apk", "sha256": "a" * 64 + "\n"}])
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("without a name and a sha256", proc.stderr)

    def test_a_claim_the_listed_release_does_not_carry_is_refused(self) -> None:
        self.configure(claims=[{"name": "nevr-runtime-v5.17.0-quest.apk", "sha256": "f" * 64, "size": 1}])
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "nevr-runtime-v5.17.0-quest.apk", "not what the manifest lists")

    def test_a_manifest_the_host_serves_that_is_not_the_one_verified_is_refused(self) -> None:
        Host.mode = "tampered"
        self.assertRefused(self.run_tool("v5.17.0", "--dry-run"), "sha256")

    def test_a_signer_that_fails_says_so_without_its_stdout(self) -> None:
        self.configure(rc={"verify-manifest": 1}, noise={"verify-manifest": "REMOTE-CONTENT-MARKER"})
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertRefused(proc, "verify-manifest")
        self.assertNotIn("REMOTE-CONTENT-MARKER", self.everything_said(proc))


class DryRun(RollbackCase):
    def test_a_dry_run_going_back_dispatches_with_dry_run_and_override_and_changes_nothing(self) -> None:
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        inputs = self.dispatch_inputs()
        want_claims = [{"name": a["name"], "sha256": a["sha256"], "size": a["size"]} for a in artifacts("5.17.0")
                       if a["kind"] in ("quest-apk", "game-data")]
        self.assertEqual({k: v for k, v in inputs.items() if k != "extra_artifacts"},
                         {"action": "publish", "tag": "v5.17.0", "channel": "beta", "override": "true", "dry_run": "true"})
        self.assertEqual(json.loads(inputs["extra_artifacts"]), want_claims)
        dispatch = self.gh_calls("workflow", "run")[0]
        self.assertEqual(dispatch[:6], ["workflow", "run", "sign-manifest.yml", "-R", "thesprockee/release-signer", "--ref"])
        self.assertEqual(dispatch[6], "main")
        self.assertEqual(self.gh_calls("variable", "set"), [])
        self.assertEqual(json.loads((self.fake / "vars.json").read_text()), {"RELEASE_TRAIN": "true"})

    def test_a_dry_run_prints_the_plan_the_run_and_the_result(self) -> None:
        proc = self.run_tool("v5.17.0", "--dry-run")
        out = proc.stdout
        self.assertIn("v5.18.0 -> v5.17.0", out)
        self.assertIn("back", out)
        self.assertIn("manifest_version 7", out)
        for a in artifacts("5.17.0"):
            self.assertIn(f"{a['name']} kind={a['kind']} size={a['size']}", out)
        self.assertIn("not touched (dry run)", out)
        self.assertIn("run 4242", out)
        self.assertRegex(out, r"elapsed \d+ s")
        self.assertIn("dry run ok", out)

    def test_a_dry_run_forward_carries_no_override(self) -> None:
        self.set_manifest(manifest("5.17.0"))
        proc = self.run_tool("v5.18.0", "--dry-run", "--forward")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.dispatch_inputs()["override"], "false")
        self.assertIn("forward", proc.stdout)

    def test_a_dry_run_whose_record_job_ran_is_a_failure(self) -> None:
        self.configure(jobs={"record": "success"})
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("check_jobs", proc.stderr)
        self.assertIn("Commit the manifest record", proc.stderr)

    def test_a_dry_run_whose_upload_job_was_skipped_is_a_failure(self) -> None:
        self.configure(jobs={"upload": "skipped"})
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Publish to R2 is skipped", proc.stderr)

    def test_a_dry_run_that_moved_beta_is_a_failure(self) -> None:
        self.configure(publish_on_dry_run=True)
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("verify_unchanged", proc.stderr)


class LiveRun(RollbackCase):
    def position(self, who: str, *words: str) -> int:
        for i, c in enumerate(self.calls()):
            if c["who"] == who and c["argv"][:len(words)] == list(words):
                return i
        self.fail(f"no {who} call starting {words}: {self.calls()}")

    def test_a_live_rollback_pauses_the_train_before_it_dispatches(self) -> None:
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        paused = self.position("gh", "variable", "set")
        self.assertEqual(self.gh_calls("variable", "set"), [["variable", "set", "RELEASE_TRAIN", "--body", "false", "-R", "EchoTools/nevr-runtime"]])
        dispatched = self.position("gh", "workflow", "run")
        watched = self.position("gh", "run", "watch")
        viewed = self.position("gh", "run", "view")
        last_verify = max(i for i, c in enumerate(self.calls()) if c["who"] == "signer" and c["argv"][0] == "verify-manifest")
        self.assertLess(paused, dispatched)
        self.assertLess(dispatched, watched)
        self.assertLess(watched, viewed)
        self.assertLess(viewed, last_verify)
        inputs = self.dispatch_inputs()
        self.assertEqual((inputs["dry_run"], inputs["override"], inputs["tag"], inputs["channel"]), ("false", "true", "v5.17.0", "beta"))
        self.assertEqual(json.loads((self.fake / "manifest.json").read_text())["channels"]["beta"], "5.17.0")
        self.assertEqual(json.loads((self.fake / "vars.json").read_text()), {"RELEASE_TRAIN": "false"})

    def test_a_live_rollback_never_resumes_the_train_and_says_how(self) -> None:
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.gh_calls("variable", "set")[-1][3:5], ["--body", "false"])
        out = proc.stdout
        self.assertIn("v5.17.0", out)
        self.assertIn("manifest_version 8", out)
        self.assertRegex(out, r"elapsed \d+ s")
        self.assertIn("variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime", out)
        self.assertIn("start again whatever schedules tools/quest_release_timer.py", out)
        self.assertIn("STILL PAUSED", out)
        # The fix ships through the train; the command that re-targets the bad release is labelled for a false alarm only.
        self.assertIn("cannot target a tag the manifest does not list", out)
        self.assertIn("to ship the fix, resume the train", out)
        self.assertIn("forward move that needs no override", out)
        false_alarm = [line for line in out.splitlines() if "rollback_beta.py v5.18.0" in line]
        self.assertEqual(len(false_alarm), 1, out)
        self.assertIn("false alarm only", false_alarm[0])
        self.assertIn("python3 tools/rollback_beta.py v5.18.0 --live --forward --yes", false_alarm[0])
        self.assertLess(out.index("to ship the fix, resume the train"), out.index("false alarm only"))

    def test_keep_train_touches_no_variable(self) -> None:
        proc = self.run_tool("v5.17.0", "--live", "--yes", "--keep-train")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.gh_calls("variable"), [])
        self.assertIn("not touched (--keep-train)", proc.stdout)
        self.assertNotIn("STILL PAUSED", proc.stdout)
        self.assertEqual(json.loads((self.fake / "manifest.json").read_text())["channels"]["beta"], "5.17.0")

    def test_a_failed_variable_set_stops_before_the_dispatch(self) -> None:
        self.configure(rc={"variable set": 1})
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pause_variable", proc.stderr)
        self.assertIn("NOT done", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_variable_that_did_not_change_is_a_failure_before_the_dispatch(self) -> None:
        self.configure(variable_stuck=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pause_variable", proc.stderr)
        self.assertIn("RELEASE_TRAIN", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_quest_release_in_progress_stops_before_the_dispatch(self) -> None:
        for extra in ((), ("--keep-train",)):
            with self.subTest(extra=extra), self.timer_lock_held():
                self.reset_world()
                proc = self.run_tool("v5.17.0", "--live", "--yes", *extra)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("check_timer", proc.stderr)
                self.assertIn("holds its lock", proc.stderr)
                self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_no_quest_release_run_holding_the_lock_lets_the_run_through(self) -> None:
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(self.gh_calls("workflow", "run")), 1)

    def test_a_sign_manifest_run_in_progress_stops_before_the_dispatch(self) -> None:
        for status in ("queued", "in_progress", "waiting", "requested", "pending", "something_new"):
            with self.subTest(status=status):
                self.reset_world()
                self.configure(signer_runs=[{"databaseId": 99, "status": "completed"}, {"databaseId": 77, "status": status}])
                proc = self.run_tool("v5.17.0", "--live", "--yes")
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("check_signer_runs", proc.stderr)
                self.assertIn(f"77 ({status})", proc.stderr)
                self.assertNotIn("99", proc.stderr.replace("sign-manifest", ""))
                self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_only_completed_signer_runs_let_the_run_through(self) -> None:
        self.configure(signer_runs=[{"databaseId": 99, "status": "completed"}, {"databaseId": 98, "status": "completed"}])
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        listed = self.gh_calls("run", "list")
        self.assertEqual(listed[0][:7], ["run", "list", "--workflow", "sign-manifest.yml", "-R", "thesprockee/release-signer", "--json"])

    def test_an_unreadable_signer_run_list_stops_before_the_dispatch(self) -> None:
        for raw in ("null", "[{\"databaseId\": 1}]", "not json"):
            with self.subTest(raw=raw):
                self.reset_world()
                self.configure(signer_runs_raw=raw)
                proc = self.run_tool("v5.17.0", "--live", "--yes")
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("check_signer_runs", proc.stderr)
                self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_the_guards_run_after_the_pause_and_before_the_dispatch(self) -> None:
        self.run_tool("v5.17.0", "--live", "--yes")
        paused = self.position("gh", "variable", "set")
        listed = self.position("gh", "run", "list")
        dispatched = self.position("gh", "workflow", "run")
        self.assertLess(paused, listed)
        self.assertLess(listed, dispatched)

    def test_keep_train_still_refuses_while_a_quest_release_runs(self) -> None:
        with self.timer_lock_held():
            proc = self.run_tool("v5.17.0", "--live", "--yes", "--keep-train")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("check_timer", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_beta_that_moved_while_pausing_stops_before_the_dispatch(self) -> None:
        self.configure(move_beta_on_pause=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("recheck", proc.stderr)
        self.assertIn("5.19.0", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_target_whose_bytes_changed_while_pausing_stops_before_the_dispatch(self) -> None:
        self.configure(change_target_on_pause=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("recheck", proc.stderr)
        self.assertIn("not what the manifest lists", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_failed_watch_fails_the_run_and_names_the_run(self) -> None:
        self.configure(rc={"run watch": 1})
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("watch", proc.stderr)
        self.assertIn("4242", proc.stderr)
        self.assertIn("RELEASE_TRAIN is false", proc.stderr)

    def test_a_dispatch_that_prints_no_run_url_fails_the_run(self) -> None:
        self.configure(no_url=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("gh exited 0 but printed no run URL: a publish MAY have been dispatched: list the runs on the signer repo "
                      "before dispatching again", proc.stderr)
        self.assertNotIn("no publish was dispatched", proc.stderr)
        self.assertIn("RELEASE_TRAIN is false", proc.stderr)
        self.assertEqual(self.gh_calls("run", "watch"), [])

    def test_a_failed_dispatch_fails_the_run(self) -> None:
        self.configure(rc={"workflow run": 1})
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("dispatch", proc.stderr)

    def test_a_green_run_with_a_skipped_publishing_job_is_a_failure(self) -> None:
        self.configure(jobs={"record": "skipped"})
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("check_jobs", proc.stderr)
        self.assertIn("Commit the manifest record is skipped", proc.stderr)

    def test_a_missing_publishing_job_is_a_failure(self) -> None:
        self.configure(omit_jobs=["upload"])
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Publish to R2 is missing", proc.stderr)

    def test_a_beta_other_than_the_target_after_the_publish_is_a_failure(self) -> None:
        self.configure(publish="wrong_beta")
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("verify_live", proc.stderr)
        self.assertIn("5.16.1", proc.stderr)

    def test_a_beta_that_never_moved_is_a_failure(self) -> None:
        self.configure(publish="none")
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("verify_live", proc.stderr)

    def test_an_artifact_that_dropped_out_of_the_target_is_a_failure(self) -> None:
        self.configure(publish="drop_artifact")
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("verify_live", proc.stderr)
        self.assertIn("nevr-runtime-v5.17.0-quest.apk", proc.stderr)

    def test_an_artifact_whose_bytes_changed_is_a_failure(self) -> None:
        self.configure(publish="changed_bytes")
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("verify_live", proc.stderr)
        self.assertIn("nevr-runtime-v5.17.0-windows.zip", proc.stderr)

    def test_rolling_forward_carries_no_override_and_prints_the_way_back(self) -> None:
        self.set_manifest(manifest("5.17.0"))
        proc = self.run_tool("v5.18.0", "--live", "--yes", "--forward")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.dispatch_inputs()["override"], "false")
        self.assertEqual(json.loads((self.fake / "manifest.json").read_text())["channels"]["beta"], "5.18.0")
        undo = [line for line in proc.stdout.splitlines() if "rollback_beta.py v5.17.0" in line]
        self.assertEqual(len(undo), 1, proc.stdout)
        self.assertIn("python3 tools/rollback_beta.py v5.17.0 --live --yes", undo[0])
        self.assertNotIn("--forward", undo[0])


class DryRunRehearsal(RollbackCase):
    """A dry run reads what a live run would meet, and compares the manifest it signed with the live one."""

    def test_a_dry_run_says_what_a_live_run_would_meet_and_changes_nothing(self) -> None:
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        out = proc.stdout
        self.assertIn("plan: RELEASE_TRAIN is 'true'", out)
        self.assertIn("plan: a run of the Quest release timer does not hold its lock", out)
        self.assertIn("runs in progress on thesprockee/release-signer: none", out)
        self.assertIn("a live run would not be refused", out)
        self.assertEqual(self.changes()[0]["argv"][:2], ["workflow", "run"])
        self.assertEqual(len(self.changes()), 1)

    def test_a_dry_run_says_a_live_run_would_be_refused_while_a_quest_release_runs(self) -> None:
        with self.timer_lock_held():
            proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("a live run would be refused now: a Quest release timer run holds its lock", proc.stdout)

    def test_a_dry_run_says_a_live_run_would_be_refused_while_a_signer_run_is_under_way(self) -> None:
        self.configure(signer_runs=[{"databaseId": 77, "status": "in_progress"}])
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("a live run would be refused now:", proc.stdout)
        self.assertIn("77 (in_progress)", proc.stdout)

    def test_a_dry_run_says_what_it_could_not_read_and_still_runs(self) -> None:
        self.configure(variable_list_raw="null", signer_runs_raw="null")
        (self.timer_work / "timer.lock").symlink_to(self.base / "nowhere")  # a lock that cannot be opened
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for what in ("the variable could not be read", "the Quest release timer's lock could not be read",
                     "the signer runs could not be read"):
            self.assertIn(what, proc.stdout)
        self.assertEqual(len(self.gh_calls("workflow", "run")), 1)

    def test_the_signed_manifest_of_the_run_is_downloaded_by_name_and_compared(self) -> None:
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        download = self.gh_calls("run", "download")
        self.assertEqual(len(download), 1)
        self.assertEqual(download[0][:7], ["run", "download", "4242", "-R", "thesprockee/release-signer", "-n", "manifest-signed"])
        self.assertEqual(download[0][7], "-D")
        self.assertIn("dry run manifest: beta is v5.17.0, the 3 releases and the other channels are unchanged, 3 artifacts",
                      proc.stdout)
        self.assertFalse(pathlib.Path(download[0][8]).exists(), "the downloaded manifest was left behind")

    def test_a_signed_manifest_that_is_not_the_rollback_fails_the_dry_run(self) -> None:
        for mode, words in (("wrong_beta", "has beta '5.16.1'"), ("extra_release", "set of releases"),
                            ("changed_other", "entries of other releases: 5.16.1"), ("changed_artifact", "other artifacts"),
                            ("other_channel", "channel other than beta"), ("dry_no_file", "no readable manifest.json"),
                            ("dry_garbage", "no readable manifest.json")):
            with self.subTest(mode=mode):
                self.reset_world()
                self.configure(**({mode: True} if mode.startswith("dry_") else {"dry_signed": mode}))
                proc = self.run_tool("v5.17.0", "--dry-run")
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("check_dry_manifest", proc.stderr)
                self.assertIn(words, proc.stderr)
                self.config.pop(mode, None)
                self.config.pop("dry_signed", None)
                self.write_config()

    def test_a_difference_in_the_notes_is_reported_and_does_not_fail(self) -> None:
        self.configure(dry_signed="notes_differ")
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("note: the notes of v5.17.0 differ (lines only in the live manifest: 1, only in the dry run's: 1)", proc.stdout)

    def test_the_published_time_is_ignored_and_another_field_is_reported(self) -> None:
        self.configure(dry_signed="published_differs")
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("note:", proc.stdout)
        self.reset_world()
        self.configure(dry_signed="field_differs")
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("note: the dry run's entry for v5.17.0 differs from the live one in commit", proc.stdout)


class FailurePaths(RollbackCase):
    """Every way out of a run reports what is paused and what was dispatched."""

    def test_a_crash_after_the_dispatch_reports_the_state_and_the_run(self) -> None:
        def boom(argv):  # noqa: ANN001, ANN202
            if argv[1:3] == ["run", "view"]:
                raise RuntimeError("boom " + SECRETS["GH_TOKEN"])

        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", on_call=boom)
        self.assertNotEqual(code, 0)
        self.assertIn("crashed: RuntimeError: boom <redacted>", err)
        self.assertNotIn(SECRETS["GH_TOKEN"], err)
        self.assertIn("publish run 4242 was dispatched (its outcome is not confirmed)", err)
        self.assertIn("RELEASE_TRAIN is false", err)
        self.assertIn("variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime", err)
        last = self.records()[-1]
        self.assertEqual((last["event"], last["outcome"], last["step"]), ("finish", "crashed", "check_jobs"))

    def test_ctrl_c_during_the_watch_reports_the_state_and_exits_130(self) -> None:
        def interrupt(argv):  # noqa: ANN001, ANN202
            if argv[1:3] == ["run", "watch"]:
                raise KeyboardInterrupt

        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", on_call=interrupt)
        self.assertEqual(code, 130)
        self.assertIn("interrupted (Ctrl-C)", err)
        self.assertIn("publish run 4242 was dispatched", err)
        self.assertIn("RELEASE_TRAIN is false", err)
        last = self.records()[-1]
        self.assertEqual((last["event"], last["outcome"], last["step"]), ("finish", "interrupted", "watch"))

    def test_ctrl_c_during_the_dispatch_call_may_have_dispatched(self) -> None:
        def interrupt(argv):  # noqa: ANN001, ANN202
            if argv[1:3] == ["workflow", "run"]:
                raise KeyboardInterrupt

        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", on_call=interrupt)
        self.assertEqual(code, 130)
        self.assertIn("a publish MAY have been dispatched (gh left no run id): list the runs on the signer repo before "
                      "dispatching again", err)
        self.assertNotIn("no publish was dispatched", err)
        self.assertEqual(self.records()[-1]["step"], "dispatch")

    def test_a_crash_during_the_dispatch_call_may_have_dispatched(self) -> None:
        def boom(argv):  # noqa: ANN001, ANN202
            if argv[1:3] == ["workflow", "run"]:
                raise RuntimeError("boom")

        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", on_call=boom)
        self.assertEqual(code, 1)
        self.assertIn("crashed: RuntimeError: boom", err)
        self.assertIn("a publish MAY have been dispatched", err)
        self.assertNotIn("no publish was dispatched", err)

    def test_ctrl_c_before_the_dispatch_still_says_no_publish_was_dispatched(self) -> None:
        def interrupt(argv):  # noqa: ANN001, ANN202
            if argv[1:3] == ["variable", "set"]:
                raise KeyboardInterrupt

        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", on_call=interrupt)
        self.assertEqual(code, 130)
        self.assertIn("no publish was dispatched", err)
        self.assertNotIn("MAY have been dispatched", err)

    def test_a_crash_once_the_run_id_is_known_names_the_run_and_not_a_maybe(self) -> None:
        def boom(argv):  # noqa: ANN001, ANN202
            if argv[1:3] == ["run", "watch"]:
                raise RuntimeError("boom")

        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", on_call=boom)
        self.assertIn("publish run 4242 was dispatched (its outcome is not confirmed)", err)
        self.assertNotIn("MAY have been dispatched", err)

    def test_a_log_that_fails_after_the_dispatch_still_shows_the_run_and_the_state(self) -> None:
        class Failing(rollback_beta.Log):
            def event(self, event: str, **fields: object) -> None:
                if fields.get("step") == "dispatch" and fields.get("outcome") == "ok":
                    raise rollback_beta.Failure("cannot write the log: disk full (test)")
                super().event(event, **fields)

        with mock.patch.object(rollback_beta, "Log", Failing):
            code, out, err = self.run_inproc("v5.17.0", "--live", "--yes")
        self.assertNotEqual(code, 0)
        self.assertIn("run 4242 dispatched", out)
        self.assertIn("publish run 4242 was dispatched", err)
        self.assertIn("RELEASE_TRAIN is false", err)
        self.assertIn("cannot write the log", err)

    def test_a_log_that_fails_every_record_still_gets_the_report_out_without_raising(self) -> None:
        class Dead(rollback_beta.Log):
            def event(self, event: str, **fields: object) -> None:
                if event != "start":
                    raise rollback_beta.Failure("cannot write the log: gone (test)")
                super().event(event, **fields)

        with mock.patch.object(rollback_beta, "Log", Dead):
            code, out, err = self.run_inproc("v5.17.0", "--live", "--yes")
        self.assertNotEqual(code, 0)
        self.assertEqual(err.count("cannot write the log: gone (test)"), 2, err)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_log_that_cannot_be_closed_does_not_raise_and_fails_a_good_run(self) -> None:
        class Closing(rollback_beta.Log):
            def close(self) -> None:
                super().close()
                raise OSError(5, "Input/output error")

        with mock.patch.object(rollback_beta, "Log", Closing):
            code, out, err = self.run_inproc("v5.17.0", "--dry-run")
        self.assertEqual(code, 1)
        self.assertIn("cannot close the log", err)
        self.assertIn("dry run ok", out)

    def test_a_dispatch_that_timed_out_may_have_been_accepted(self) -> None:
        def timed_out(argv):  # noqa: ANN001, ANN202
            if argv[1:3] == ["workflow", "run"]:
                return subprocess.CompletedProcess(argv, 124, "", "gh ran longer than 600 s and was stopped")

        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", on_call=timed_out)
        self.assertNotEqual(code, 0)
        self.assertIn("A publish MAY have been dispatched: list the runs on the signer repo before dispatching again", err)
        self.assertIn("a publish MAY have been dispatched (gh left no run id)", err)
        self.assertNotIn("no publish was dispatched", err)

    def test_a_cleanup_that_fails_after_a_verified_rollback_says_so_and_exits_zero(self) -> None:
        env = {k: v for k, v in self.env.items() if k not in ("RELEASE_SIGNER_BIN", "RELEASE_SIGNER_DIR")}
        with mock.patch.object(rollback_beta.shutil, "rmtree", side_effect=OSError(13, "Permission denied")):
            code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", env=env)
        self.assertEqual(code, 0, err)
        self.assertIn("rolled back and verified; cleanup of ", err)
        self.assertIn(" failed: Permission denied", err)
        self.assertNotIn("not confirmed", err + out)
        self.assertNotIn("undo", err)
        self.assertIn("cleanup_failed", [r["event"] for r in self.records()])
        self.assertEqual(self.records()[-1]["outcome"], "ok")

    def test_keep_train_does_not_hide_a_dispatch_from_the_state_report(self) -> None:
        self.configure(rc={"run watch": 1})
        proc = self.run_tool("v5.17.0", "--live", "--yes", "--keep-train")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("publish run 4242 was dispatched (its outcome is not confirmed)", proc.stderr)
        self.assertNotIn("RELEASE_TRAIN", proc.stderr)

    def test_a_dry_run_that_fails_after_the_dispatch_says_the_run_publishes_nothing(self) -> None:
        self.configure(jobs={"upload": "failure"})
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("publish run 4242 was dispatched (a dry run: it publishes nothing)", proc.stderr)

    def test_a_null_variable_list_is_a_step_failure_not_a_traceback(self) -> None:
        self.configure(variable_list_raw="null")
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pause_variable: the variable list is unreadable", proc.stderr)
        self.assertNotIn("Traceback", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])


class Runbook(unittest.TestCase):
    """The commands in RELEASING.md's rollback section are the ones an operator pastes: they have to run."""

    @staticmethod
    def section() -> str:
        text = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
        return text.split("## Emergency: roll beta back", 1)[1].split("\n## ", 1)[0]

    def blocks(self) -> str:
        return "\n".join(re.findall(r"```sh\n(.*?)```", self.section(), re.S))

    def test_the_base_line_names_the_public_host_document(self) -> None:
        lines = [line for line in self.blocks().splitlines() if line.startswith("export RELEASE_PUBLIC_BASE=")]
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("docs/reference/verifying-a-release.md", lines[0])
        self.assertTrue((ROOT / "docs" / "reference" / "verifying-a-release.md").is_file())

    def test_the_commands_name_the_account_through_gh_bin_and_never_hardcode_one(self) -> None:
        blocks = self.blocks()
        self.assertNotIn("gh-spritz", blocks)
        self.assertNotIn("systemctl", blocks)
        self.assertEqual(blocks.count("$GH_BIN variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime"), 2)

    def test_the_prose_says_what_an_operator_has_to_know_before_pasting(self) -> None:
        prose = self.section()
        self.assertIn("refuses loudly if it is empty", prose)
        self.assertIn("this tool does not stop the timer", prose)
        self.assertIn("When `GH_BIN` is unset the tool uses `gh`", prose)

class Timeouts(RollbackCase):
    def test_every_command_has_a_timeout_and_the_slow_ones_a_longer_one(self) -> None:
        env = {k: v for k, v in self.env.items() if k not in ("RELEASE_SIGNER_BIN", "RELEASE_SIGNER_DIR")}
        code, out, err = self.run_inproc("v5.17.0", "--live", "--yes", env=env)
        self.assertEqual(code, 0, err)
        self.assertEqual([argv for argv, timeout in self.seen if not timeout], [])
        self.assertEqual(self.timeout_of("build", "-o"), rollback_beta.BUILD_TIMEOUT)
        self.assertEqual(self.timeout_of("fetch", "origin"), rollback_beta.DEFAULT_TIMEOUT)
        self.assertEqual(self.timeout_of("workflow", "run"), rollback_beta.DEFAULT_TIMEOUT)
        self.assertEqual(self.timeout_of("run", "watch"), 1800)
        self.assertEqual((rollback_beta.DEFAULT_TIMEOUT, rollback_beta.BUILD_TIMEOUT), (600, 1800))

    def test_the_watch_waits_longer_for_a_target_that_lists_pc_game_data(self) -> None:
        m = manifest("5.18.0")
        m["releases"]["5.17.0"]["artifacts"].append({"name": "echovr-data-win10-0123456789ab.zip", "kind": "game-data",
                                                     "platform": "windows-x86_64", "sha256": hashlib.sha256(b"pc").hexdigest(),
                                                     "size": 5 << 30})
        self.set_manifest(m)
        code, out, err = self.run_inproc("v5.17.0", "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.timeout_of("run", "watch"), 7200)

    def test_quest_game_data_alone_does_not_lengthen_the_watch(self) -> None:
        code, out, err = self.run_inproc("v5.17.0", "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.timeout_of("run", "watch"), 1800)


class Defaults(RollbackCase):
    def test_gh_defaults_to_gh_and_the_resume_commands_name_it(self) -> None:
        env = {k: v for k, v in self.env.items() if k != "GH_BIN"}
        proc = self.run_tool("v5.17.0", "--live", "--yes", env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        progs = {c["prog"] for c in self.calls() if c["who"] == "gh"}
        self.assertEqual(progs, {"gh"})
        self.assertIn("gh variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime", proc.stdout)

    def test_an_error_that_carries_the_host_is_redacted_in_the_output_and_the_log(self) -> None:
        Host.mode = "error500"
        proc = self.run_tool("v5.17.0", "--dry-run")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("answered 500", proc.stderr)
        said = self.everything_said(proc)
        self.assertNotIn(self.base_url, said)
        self.assertNotIn("127.0.0.1", said)
        self.assertIn("<redacted>", self.records()[-1]["error"])

    def test_a_bare_host_name_in_an_error_is_redacted(self) -> None:
        self.configure(rc={"variable set": 1}, leak_stderr="connect to 127.0.0.1 port refused")
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        said = self.everything_said(proc)
        self.assertNotIn("127.0.0.1", said)
        self.assertIn("connect to <redacted> port refused", proc.stderr)


class Ordering(RollbackCase):
    def test_versions_compare_as_numbers_not_as_text(self) -> None:
        m = manifest("5.10.0")
        m["releases"] = {v: {"version": v, "tag": f"v{v}", "artifacts": artifacts(v)} for v in ("5.9.0", "5.10.0")}
        self.set_manifest(m)
        proc = self.run_tool("v5.9.0", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.dispatch_inputs()["override"], "true")
        self.assertIn("back", proc.stdout)


class Records(RollbackCase):
    def test_a_credential_or_the_host_in_a_command_error_never_reaches_the_output_or_the_log(self) -> None:
        self.configure(rc={"variable set": 1}, leak_stderr=f"saw {SECRETS['GH_TOKEN']} and {SECRETS['R2_SECRET_ACCESS_KEY']} "
                                                           f"at {self.base_url}/nevr-runtime/manifest.json")
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        said = self.everything_said(proc)
        self.assertIn("<redacted>", said)
        for value in (SECRETS["GH_TOKEN"], SECRETS["R2_SECRET_ACCESS_KEY"], self.base_url, "127.0.0.1"):
            self.assertNotIn(value, said)

    def test_every_step_leaves_one_structured_record_and_none_holds_a_secret(self) -> None:
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        records = self.records()
        steps = [r["step"] for r in records if r["event"] == "step" and r["outcome"] == "ok"]
        self.assertEqual(steps, ["read_live", "check_target", "claims", "plan", "pause_variable", "check_timer",
                                 "check_signer_runs", "recheck", "dispatch", "watch", "check_jobs", "verify_live"])
        self.assertEqual(len({r["run"] for r in records}), 1)
        for r in records:
            for key in ("ts", "run", "event", "step", "tag", "direction", "outcome", "error"):
                self.assertIn(key, r, r)
        self.assertEqual({r["tag"] for r in records}, {"v5.17.0"})
        self.assertEqual(records[-1]["event"], "finish")
        self.assertEqual(records[-1]["outcome"], "ok")
        self.assertEqual({r["direction"] for r in records if r["step"] in ("claims", "plan", "dispatch", "verify_live")}, {"back"})
        said = self.everything_said(proc)
        for name, value in SECRETS.items():
            self.assertNotIn(value, said, name)
        self.assertNotIn(self.base_url, said)
        self.assertNotIn("127.0.0.1", said)
        for c in self.calls():
            if c["who"] != "gh":
                self.assertFalse(c["leak"]["GH_TOKEN"], c)
            self.assertFalse(c["leak"]["R2_SECRET_ACCESS_KEY"] or c["leak"]["R2_ACCESS_KEY_ID"], c)

    def test_a_failed_step_is_recorded_with_its_error(self) -> None:
        self.configure(jobs={"record": "failure"})
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        records = self.records()
        failed = [r for r in records if r["outcome"] == "failed"]
        self.assertEqual([r["step"] for r in failed], ["check_jobs"])
        self.assertIn("Commit the manifest record is failure", failed[0]["error"])
        self.assertEqual(records[-1]["event"], "finish")
        self.assertEqual(records[-1]["outcome"], "failed")

    def test_a_refusal_is_recorded_as_refused(self) -> None:
        self.run_tool("v5.18.0", "--dry-run")
        last = self.records()[-1]
        self.assertEqual((last["event"], last["outcome"], last["step"]), ("finish", "refused", "check_target"))


class SignerSnapshot(RollbackCase):
    def test_without_a_signer_binary_the_tool_builds_one_from_a_snapshot_and_removes_it(self) -> None:
        env = {k: v for k, v in self.env.items() if k not in ("RELEASE_SIGNER_BIN", "RELEASE_SIGNER_DIR")}
        proc = self.run_tool("v5.17.0", "--dry-run", env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        built = [c for c in self.calls() if c["who"] in ("git", "tar", "go")]
        self.assertEqual([(c["who"], c["argv"][0] if c["who"] != "git" else c["argv"][2]) for c in built],
                         [("git", "fetch"), ("git", "rev-parse"), ("git", "archive"), ("tar", "-xf"), ("go", "build")])
        go = built[-1]
        self.assertTrue(go["cwd"].startswith(str(self.work / "rollback-beta")), go["cwd"])
        signer_calls = [c for c in self.calls() if c["who"] == "signer"]
        self.assertTrue(signer_calls)
        self.assertTrue(all(c["cwd"] == go["cwd"] for c in signer_calls), "the signer ran outside its snapshot")
        left = [p.name for p in (self.work / "rollback-beta").iterdir() if p.name != "README.md"]
        self.assertEqual(left, [], "the snapshot was left behind")

    def test_a_failed_snapshot_build_fails_the_run_and_still_removes_the_scratch(self) -> None:
        env = {k: v for k, v in self.env.items() if k not in ("RELEASE_SIGNER_BIN", "RELEASE_SIGNER_DIR")}
        self.configure(rc={"go": 1})
        proc = self.run_tool("v5.17.0", "--dry-run", env=env)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("build_signer", proc.stderr)
        left = [p.name for p in (self.work / "rollback-beta").iterdir() if p.name != "README.md"]
        self.assertEqual(left, [])


class Runner(unittest.TestCase):
    def test_a_command_that_outlives_its_timeout_is_a_failure_with_a_named_code(self) -> None:
        result = rollback_beta.subprocess_runner(["sleep", "30"], timeout=1)
        self.assertEqual(result.returncode, 124)
        self.assertIn("1 s", result.stderr)

    def test_a_program_that_cannot_run_is_exit_127(self) -> None:
        self.assertEqual(rollback_beta.subprocess_runner(["/nonexistent/program"]).returncode, 127)


if __name__ == "__main__":
    unittest.main()
