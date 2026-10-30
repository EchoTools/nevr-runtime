"""`tools/rollback_beta.py` (#1513): point the beta channel of the signed manifest at an earlier listed release with one
command. The script runs as a subprocess against fakes that record what they were called with: `gh`, `systemctl` and the
release signer are scripts in a directory the test controls, and the public host is a server on 127.0.0.1 that serves
the manifest file the fakes change. Nothing here reaches GitHub, systemd or a real host."""

import hashlib
import http.server
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "rollback_beta.py"

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

# One module behind three shims (gh, systemctl, the signer) and the build tools. Each call is appended to calls.jsonl
# with the credentials that reached it; config.json says what to break; state files hold the variable, the units and the
# manifest the host serves.
FAKES = r'''
import hashlib, json, os, pathlib, shutil, sys

D = pathlib.Path(os.environ["FAKE_DIR"])
CFG = json.loads((D / "config.json").read_text())


def record(who, argv):
    leak = {k: k in os.environ for k in ("R2_SECRET_ACCESS_KEY", "R2_ACCESS_KEY_ID", "GH_TOKEN")}
    with (D / "calls.jsonl").open("a") as f:
        f.write(json.dumps({"who": who, "argv": argv, "cwd": os.getcwd(), "leak": leak}) + "\n")


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


def gh(a):
    key = " ".join(a[:2])
    record("gh", a)
    maybe_fail(key)
    vars_path = D / "vars.json"
    if key == "variable set":
        if not CFG.get("variable_stuck"):
            vars_path.write_text(json.dumps({a[2]: a[a.index("--body") + 1]}))
        if CFG.get("move_beta_on_pause"):
            m = manifest()
            m["releases"]["5.19.0"] = m["releases"]["5.18.0"]
            m["channels"]["beta"] = "5.19.0"
            save(m)
    elif key == "variable list":
        v = json.loads(vars_path.read_text())
        print(json.dumps([{"name": k, "value": x} for k, x in v.items()]))
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


def systemctl(a):
    record("systemctl", a)
    verb, unit = a[1], a[-1]
    maybe_fail(verb + ":" + unit)
    units = json.loads((D / "units.json").read_text())
    if verb == "stop":
        units[unit] = "active" if CFG.get("stop_ineffective") else "inactive"
    elif verb == "start":
        units[unit] = "active"
    elif verb == "is-active":
        state = units.get(unit, "inactive")
        if CFG.get("service_active") and unit.endswith(".service"):
            state = "active"
        print(state)
        sys.exit(0 if state == "active" else 3)
    (D / "units.json").write_text(json.dumps(units))


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
                              if x["kind"] in ("quest-apk", "game-data")]))


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
    {"gh": gh, "systemctl": systemctl, "signer": signer}.get(who, lambda argv: build_tool(who, argv))(a)
'''


class Host(http.server.BaseHTTPRequestHandler):
    """Serves the manifest file the fakes change."""

    directory = pathlib.Path(".")
    mode = "plain"

    def log_message(self, *args) -> None:
        pass

    def do_GET(self) -> None:
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
            "releases": {v: {"version": v, "tag": f"v{v}", "artifacts": artifacts(v)} for v in ("5.16.1", "5.17.0", "5.18.0")}}


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
        for name in ("gh", "systemctl", "release-signer", "git", "go", "tar"):
            who = "signer" if name == "release-signer" else name
            path = self.bin / name
            path.write_text(f'#!{sys.executable}\nimport sys\nsys.path.insert(0, "{self.bin}")\nimport fakes\nfakes.main("{who}")\n')
            path.chmod(0o755)
        self.set_manifest(manifest("5.18.0"))
        self.config: dict = {}
        self.write_config()
        (self.fake / "vars.json").write_text(json.dumps({"RELEASE_TRAIN": "true"}))
        (self.fake / "units.json").write_text(json.dumps({"nevr-quest-release.timer": "active",
                                                          "nevr-quest-release.service": "inactive"}))
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
                    "QUEST_RELEASE_SIGNER_REPO": str(self.base / "signer-repo")}

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
        """Every call that changes the train or the manifest: a variable set, a systemctl stop or start, a dispatch."""
        return [c for c in self.calls() if (c["who"] == "gh" and c["argv"][:2] in (["variable", "set"], ["workflow", "run"]))
                or (c["who"] == "systemctl" and c["argv"][1] in ("stop", "start"))]

    def records(self) -> list[dict]:
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def dispatch_inputs(self) -> dict:
        calls = self.gh_calls("workflow", "run")
        self.assertEqual(len(calls), 1, calls)
        argv = calls[0]
        return dict(argv[i + 1].split("=", 1) for i, a in enumerate(argv) if a == "-f")

    def assertRefused(self, proc: subprocess.CompletedProcess, *words: str) -> None:
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
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
        self.assertEqual([c for c in self.calls() if c["who"] == "systemctl"], [])
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
        stopped = self.position("systemctl", "--user", "stop")
        self.assertEqual([c["argv"] for c in self.calls() if c["who"] == "systemctl" and c["argv"][1] == "stop"],
                         [["--user", "stop", "nevr-quest-release.timer"]])
        dispatched = self.position("gh", "workflow", "run")
        watched = self.position("gh", "run", "watch")
        viewed = self.position("gh", "run", "view")
        last_verify = max(i for i, c in enumerate(self.calls()) if c["who"] == "signer" and c["argv"][0] == "verify-manifest")
        self.assertLess(paused, stopped)
        self.assertLess(stopped, dispatched)
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
        starts = [c for c in self.calls() if c["who"] == "systemctl" and c["argv"][1] == "start"]
        self.assertEqual(starts, [])
        out = proc.stdout
        self.assertIn("v5.17.0", out)
        self.assertIn("manifest_version 8", out)
        self.assertRegex(out, r"elapsed \d+ s")
        self.assertIn("python3 tools/rollback_beta.py v5.18.0 --live --forward --yes", out)
        self.assertIn("variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime", out)
        self.assertIn("systemctl --user start nevr-quest-release.timer", out)
        self.assertIn("STILL PAUSED", out)

    def test_keep_train_touches_neither_the_variable_nor_the_timer(self) -> None:
        proc = self.run_tool("v5.17.0", "--live", "--yes", "--keep-train")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.gh_calls("variable"), [])
        self.assertEqual([c for c in self.calls() if c["who"] == "systemctl"], [])
        self.assertIn("not touched (--keep-train)", proc.stdout)
        self.assertNotIn("STILL PAUSED", proc.stdout)
        self.assertEqual(json.loads((self.fake / "manifest.json").read_text())["channels"]["beta"], "5.17.0")

    def test_a_failed_variable_set_stops_before_the_timer_and_the_dispatch(self) -> None:
        self.configure(rc={"variable set": 1})
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pause_variable", proc.stderr)
        self.assertIn("NOT done", proc.stderr)
        self.assertEqual([c for c in self.calls() if c["who"] == "systemctl"], [])
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_variable_that_did_not_change_is_a_failure_before_the_dispatch(self) -> None:
        self.configure(variable_stuck=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pause_variable", proc.stderr)
        self.assertIn("RELEASE_TRAIN", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_failed_timer_stop_stops_before_the_dispatch_and_says_the_variable_is_paused(self) -> None:
        self.configure(rc={"stop:nevr-quest-release.timer": 1})
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pause_timer", proc.stderr)
        self.assertIn("RELEASE_TRAIN is false", proc.stderr)
        self.assertIn("timer is NOT stopped", proc.stderr)
        self.assertIn("variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_timer_that_is_still_active_after_the_stop_is_a_failure(self) -> None:
        self.configure(stop_ineffective=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pause_timer", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_quest_timer_run_in_progress_stops_before_the_dispatch(self) -> None:
        self.configure(service_active=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("nevr-quest-release.service", proc.stderr)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

    def test_a_beta_that_moved_while_pausing_stops_before_the_dispatch(self) -> None:
        self.configure(move_beta_on_pause=True)
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("recheck", proc.stderr)
        self.assertIn("5.19.0", proc.stderr)
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
        self.assertIn("no run URL", proc.stderr)
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

    def test_the_watch_has_a_timeout(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        import contextlib  # noqa: PLC0415
        import io  # noqa: PLC0415

        import rollback_beta  # noqa: PLC0415

        timeouts: dict[str, object] = {}

        def runner(argv, cwd=None, env=None, timeout=None):  # noqa: ANN001, ANN202 - the tool's runner shape
            timeouts[" ".join(argv[1:3])] = timeout
            return rollback_beta.subprocess_runner(argv, cwd=cwd, env=env, timeout=timeout)

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(rollback_beta.main(["v5.17.0", "--dry-run"], self.env, runner), 0)
        self.assertEqual(timeouts["run watch"], rollback_beta.WATCH_TIMEOUT)
        self.assertLessEqual(rollback_beta.WATCH_TIMEOUT, 3600)


    def test_every_step_leaves_one_structured_record_and_none_holds_a_secret(self) -> None:
        proc = self.run_tool("v5.17.0", "--live", "--yes")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        records = self.records()
        steps = [r["step"] for r in records if r["event"] == "step" and r["outcome"] == "ok"]
        self.assertEqual(steps, ["read_live", "check_target", "claims", "plan", "pause_variable", "pause_timer", "recheck",
                                 "dispatch", "watch", "check_jobs", "verify_live"])
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
        sys.path.insert(0, str(ROOT / "tools"))
        import rollback_beta  # noqa: PLC0415 - imported here so a missing tool fails the test, not the collection

        result = rollback_beta.subprocess_runner(["sleep", "30"], timeout=1)
        self.assertEqual(result.returncode, 124)
        self.assertIn("1 s", result.stderr)

    def test_a_program_that_cannot_run_is_exit_127(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        import rollback_beta  # noqa: PLC0415

        self.assertEqual(rollback_beta.subprocess_runner(["/nonexistent/program"]).returncode, 127)


if __name__ == "__main__":
    unittest.main()
