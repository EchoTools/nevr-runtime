//! The real `nevr-install` binary, one call each, against a local server that serves a manifest signed by a throwaway
//! key. Asserts the exit code, the files, and that the log holds the records a failed install must explain itself with.

mod common;
use common::*;
use nevr_install::util::sha256_hex;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::Path;
use std::process::Command;

const HOST: &[u8] = b"new host NEVR-BUILD 9.9.9";
const RT: &[u8] = b"new runtime";
const GAME: &[u8] = b"the game's BugSplat64.dll";

fn serve_forever(routes: HashMap<String, Vec<u8>>) -> String {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
    std::thread::spawn(move || {
        for c in l.incoming() {
            let Ok(mut c) = c else { continue };
            let mut buf = [0u8; 4096];
            let k = c.read(&mut buf).unwrap_or(0);
            let req = String::from_utf8_lossy(&buf[..k]).to_string();
            let path = req.split_whitespace().nth(1).unwrap_or("/").to_string();
            let resp = match routes.get(&path) {
                Some(body) => {
                    let mut r = format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).into_bytes();
                    r.extend_from_slice(body);
                    r
                }
                None => b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".to_vec(),
            };
            let _ = c.write_all(&resp);
        }
    });
    base
}

struct World {
    base: String,
    pins: std::path::PathBuf,
    root: tempfile::TempDir,
    /// The nevr root (ADR 0012): the runtime, the record and the set-aside files.
    nevr: tempfile::TempDir,
    game: tempfile::TempDir,
    _keys: tempfile::TempDir,
}

fn world(tamper_sig: bool) -> World {
    world_of("1.0.0", 5, tamper_sig)
}

/// A server for release `version` at manifest version `manifest_version`, with its own key.
fn world_of(version: &str, manifest_version: u64, tamper_sig: bool) -> World {
    world_full(version, manifest_version, tamper_sig, true, false)
}

/// `serve_zip` false leaves the Windows package off the server (a run that asks for it fails); `game_data` adds a
/// `game-data` artifact to the release (never served: a test that needs the bytes does not use it).
fn world_with(tamper_sig: bool, serve_zip: bool, game_data: bool) -> World {
    world_full("1.0.0", 5, tamper_sig, serve_zip, game_data)
}

fn world_full(version: &str, manifest_version: u64, tamper_sig: bool, serve_zip: bool, game_data: bool) -> World {
    let keys = tempfile::tempdir().unwrap();
    let k = make_key(keys.path(), "k");
    let zip_bytes = {
        let mut buf = std::io::Cursor::new(Vec::new());
        let mut z = zip::ZipWriter::new(&mut buf);
        for (n, b) in [("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT)] {
            z.start_file(n, zip::write::SimpleFileOptions::default()).unwrap();
            z.write_all(b).unwrap();
        }
        z.finish().unwrap();
        buf.into_inner()
    };
    let mut artifacts = vec![json!({
        "kind": "windows-zip", "platform": "windows-x86_64", "path": format!("v{version}/nevr-runtime-v{version}-windows.zip"),
        "size": zip_bytes.len(), "sha256": sha256_hex(&zip_bytes),
        "contents": [
            {"path": "BugSplat64.dll", "sha256": sha256_hex(HOST), "size": HOST.len()},
            {"path": "nevr-runtime.dll", "sha256": sha256_hex(RT), "size": RT.len()}]})];
    if game_data {
        artifacts.push(json!({"kind": "game-data", "platform": "android-arm64", "path": "data/echovr-data-aaaaaaaaaaaa.zip", "size": 20,
            "sha256": sha256_hex(b"data"), "game_data": {"data_version": "1", "echo_build": "2"},
            "contents": [{"path": "_data/x", "sha256": sha256_hex(b"abc"), "size": 3}]}));
    }
    let m = manifest_json(manifest_version, json!({"channels": {"beta": version}, "releases": {version: {"artifacts": artifacts}}}));
    let mut sig = sign(keys.path(), &k, &m, false);
    if tamper_sig {
        let mut m2 = m.clone();
        m2[10] ^= 1; // serve different bytes than were signed
        let base = serve_forever(HashMap::from([("/manifest.json".into(), m2), ("/manifest.json.sig".into(), sig.clone())]));
        sig.clear();
        return finish(base, &k, keys);
    }
    let mut routes = HashMap::from([("/manifest.json".to_string(), m), ("/manifest.json.sig".to_string(), sig)]);
    if serve_zip {
        routes.insert(format!("/v{version}/nevr-runtime-v{version}-windows.zip"), zip_bytes);
    }
    let base = serve_forever(routes);
    finish(base, &k, keys)
}

fn finish(base: String, k: &Key, keys: tempfile::TempDir) -> World {
    let pins = keys.path().join("pins.txt");
    std::fs::write(&pins, format!("T {}\n", k.b64)).unwrap();
    let game = tempfile::tempdir().unwrap();
    std::fs::write(game.path().join("echovr.exe"), b"exe").unwrap();
    std::fs::write(game.path().join("BugSplat64.dll"), GAME).unwrap();
    World { base, pins, root: tempfile::tempdir().unwrap(), nevr: tempfile::tempdir().unwrap(), game, _keys: keys }
}

fn run(w: &World, args: &[&str]) -> (i32, Value, String) {
    run_in(w, w, args)
}

/// Runs against `server`'s manifest, in `dirs`' installer root, nevr root and game folder.
fn run_in(server: &World, dirs: &World, args: &[&str]) -> (i32, Value, String) {
    run_cmd(server, dirs, args, &server.base, &[])
}

/// `base` replaces the server's address (a dead port for an offline run); `envs` are added to the child's environment.
fn run_with(w: &World, args: &[&str], base: &str, envs: &[(&str, String)]) -> (i32, Value, String) {
    run_cmd(w, w, args, base, envs)
}

fn run_cmd(server: &World, dirs: &World, args: &[&str], base: &str, envs: &[(&str, String)]) -> (i32, Value, String) {
    let mut cmd = Command::new(env!("CARGO_BIN_EXE_nevr-install"));
    // The fixture's nevr root is a temporary folder, not the canonical one: a dev run says so (#1353).
    cmd.args(args).args(["--root", dirs.root.path().to_str().unwrap(), "--nevr-root", dirs.nevr.path().to_str().unwrap(), "--dev-nevr-root", "--base", base, "--pins", server.pins.to_str().unwrap(), "--now", NOW]);
    for (k, v) in envs {
        cmd.env(k, v);
    }
    let o = cmd.output().unwrap();
    let out: Value = serde_json::from_slice(&o.stdout).unwrap_or_else(|e| panic!("stdout is not JSON ({e}): {}", String::from_utf8_lossy(&o.stdout)));
    (o.status.code().unwrap(), out, String::from_utf8_lossy(&o.stderr).to_string())
}

fn records(w: &World) -> Vec<Value> {
    std::fs::read_to_string(w.root.path().join("logs/nevr-install.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn events_of(w: &World) -> Vec<String> {
    events(w)
}

fn events(w: &World) -> Vec<String> {
    records(w).iter().map(|r| r["event"].as_str().unwrap().to_string()).collect()
}

#[test]
fn pc_install_end_to_end_logs_every_step_and_installs_the_manifests_bytes() {
    let w = world(false);
    let dir = w.game.path().to_str().unwrap();
    let (code, out, err) = run(&w, &["pc", "install", "--dir", dir]);
    assert_eq!(code, 0, "{out} {err}");
    assert_eq!(out["state"], "installed");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), HOST);
    assert_eq!(std::fs::read(w.nevr.path().join("nevr-runtime.dll")).unwrap(), RT);
    assert!(!w.game.path().join("nevr-runtime.dll").exists(), "the runtime is not written beside the game");
    let mut beside: Vec<String> = std::fs::read_dir(w.game.path()).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    beside.sort();
    assert_eq!(beside, vec!["BugSplat64.dll", "echovr.exe"], "the host is the only file the install writes beside echovr.exe");
    let ev = events(&w);
    for want in ["run_start", "manifest_fetch", "manifest_check", "release_selected", "artifact_fetch", "plan", "file_write", "verify_installed", "run_end"] {
        assert!(ev.contains(&want.to_string()), "missing {want} in {ev:?}");
    }
    assert_eq!(ev.last().unwrap(), "run_end");
    assert!(records(&w).iter().all(|r| r["tag"] == "[NEVR.INSTALL]" && r["run_id"] == records(&w)[0]["run_id"]));
    assert!(w.root.path().join("state/install-record.json").exists(), "the install record is written last");
    // A second identical run changes nothing and says so.
    let (code, out, _) = run(&w, &["pc", "install", "--dir", dir]);
    assert_eq!((code, out["state"].as_str()), (0, Some("unchanged")));
}

#[test]
fn pc_plan_changes_nothing() {
    let w = world(false);
    let (code, out, _) = run(&w, &["pc", "plan", "--dir", w.game.path().to_str().unwrap()]);
    assert_eq!(code, 0);
    assert_eq!(out["plan"]["decision"], "install");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
    assert!(!w.game.path().join("nevr-runtime.dll").exists());
    assert!(!w.nevr.path().join("nevr-runtime.dll").exists());
    assert!(!w.root.path().join("state/install-record.json").exists());
}

#[test]
fn without_a_nevr_root_the_install_asks_for_one_and_changes_nothing() {
    let w = world(false);
    let dir = w.game.path().to_str().unwrap();
    for extra in [vec![], vec!["--nevr-root", "relative/root"]] {
        let o = Command::new(env!("CARGO_BIN_EXE_nevr-install"))
            .args(["pc", "install", "--dir", dir, "--root", w.root.path().to_str().unwrap(), "--base", &w.base, "--pins", w.pins.to_str().unwrap(), "--now", NOW])
            .args(&extra)
            .env_remove("LOCALAPPDATA")
            .output()
            .unwrap();
        let out: Value = serde_json::from_slice(&o.stdout).unwrap();
        assert_eq!((o.status.code(), out["state"].as_str()), (Some(2), Some("needs-user")), "{extra:?} {out}");
        assert!(out["reason"].as_str().unwrap().contains("nevr root") || out["reason"].as_str().unwrap().contains("--nevr-root"), "{out}");
        assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
    }
    assert!(!events_of(&w).contains(&"artifact_fetch".to_string()), "nothing is downloaded before the root is known");
}

#[test]
fn an_older_release_over_a_newer_one_is_refused_unless_allowed() {
    let newer = world_of("2.0.0", 6, false);
    let dir = newer.game.path().to_str().unwrap().to_string();
    let (code, out, err) = run(&newer, &["pc", "install", "--dir", &dir]);
    assert_eq!(code, 0, "{out} {err}");
    // The channel now names 1.0.0, served by another manifest (a higher manifest version, so the floor does not stop it).
    let older = world_of("1.0.0", 7, false);
    let (code, out, _) = run_in(&older, &newer, &["pc", "install", "--dir", &dir]);
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    assert!(out["reason"].as_str().unwrap().contains("2.0.0") && out["reason"].as_str().unwrap().contains("--allow-downgrade"), "{out}");
    assert_eq!(std::fs::read(newer.nevr.path().join("nevr-runtime.dll")).unwrap(), RT);
    let (code, out, err) = run_in(&older, &newer, &["pc", "install", "--dir", &dir, "--allow-downgrade"]);
    assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{out} {err}");
}

#[test]
fn an_install_record_that_cannot_be_written_is_reported_and_the_installed_files_stay_installed() {
    let w = world(false);
    // A directory where the record's file goes: the files are installed and verified, only the informational record fails.
    std::fs::create_dir_all(w.root.path().join("state/install-record.json")).unwrap();
    let (code, out, _) = run(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap()]);
    assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{out}");
    assert!(out["reason"].as_str().unwrap().contains("install-record.json was not written"), "{out}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), HOST);
    let bad = records(&w).into_iter().find(|r| r["event"] == "install_record").expect("the failure is on record");
    assert_eq!(bad["ok"], false);
}

#[test]
fn a_manifest_that_does_not_verify_changes_nothing_and_run_end_says_why() {
    let w = world(true);
    let (code, out, err) = run(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap()]);
    assert_eq!(code, 1, "{out} {err}");
    assert_eq!(out["state"], "unchanged");
    assert!(out["reason"].as_str().unwrap().starts_with("FAIL step 2"), "{out}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
    let last = records(&w).pop().unwrap();
    assert_eq!((last["event"].as_str(), last["state"].as_str(), last["failed"].as_bool()), (Some("run_end"), Some("unchanged"), Some(true)));
    assert!(!w.root.path().join("state/manifest.json").exists(), "nothing is stored for an unverified manifest");
}

#[test]
fn a_log_that_cannot_be_opened_stops_the_run_before_anything_changes() {
    let w = world(false);
    let blocker = w.root.path().join("blocker");
    std::fs::write(&blocker, b"x").unwrap();
    let bad = blocker.join("sub").join("log.jsonl");
    let (code, out, _) = run(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--log", bad.to_str().unwrap()]);
    assert_eq!(code, 1);
    assert!(out["reason"].as_str().unwrap().contains("cannot"), "{out}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
    assert!(!w.game.path().join("nevr-runtime.dll").exists());
}

#[test]
fn manifest_verify_stores_the_pair_and_state() {
    let w = world(false);
    let (code, out, _) = run(&w, &["manifest", "verify"]);
    assert_eq!(code, 0, "{out}");
    for f in ["manifest.json", "manifest.json.sig", "state.json"] {
        assert!(w.root.path().join("state").join(f).exists(), "{f}");
    }
    assert!(Path::new(out["log"].as_str().unwrap()).exists());
}

#[test]
fn quest_plan_without_a_device_reports_the_release_and_installs_nothing() {
    let w = world(false);
    let (code, out, _) = run(&w, &["quest", "plan", "--no-device"]);
    // The test release has no quest-apk, so the plan refuses and says why. A refused plan is a failed request, so it exits 1
    // (#1353); this assertion used to expect 0, which was the defect.
    assert_eq!(code, 1, "{out}");
    assert_eq!(out["state"], "unchanged");
    assert!(out["plan"]["reasons"][0].as_str().unwrap().contains("quest-apk"), "{out}");
}

#[test]
fn usage_error_exits_1() {
    let o = Command::new(env!("CARGO_BIN_EXE_nevr-install")).arg("nonsense").output().unwrap();
    assert_eq!(o.status.code(), Some(1));
}

#[test]
fn launch_check_reads_the_hosts_log_in_the_nevr_root() {
    let w = world(false);
    let dir = w.game.path().to_str().unwrap();
    let (code, ..) = run(&w, &["pc", "install", "--dir", dir]);
    assert_eq!(code, 0);
    // No witness yet: the host has not started the game.
    std::fs::create_dir_all(w.nevr.path().join("logs")).unwrap();
    std::fs::write(w.nevr.path().join("logs/nevr-bootstrap.jsonl"), "").unwrap();
    let (code, out, _) = run(&w, &["pc", "launch-check", "--dir", dir]);
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    let line = |v: Value| v.to_string() + "\n";
    let text = line(json!({"event": "versions", "host_sha256": sha256_hex(HOST), "runtime_sha256": sha256_hex(RT)})) + &line(json!({"event": "runtime_load", "verify": "accepted"}));
    std::fs::write(w.nevr.path().join("logs/nevr-bootstrap.jsonl"), text).unwrap();
    let (code, out, _) = run(&w, &["pc", "launch-check", "--dir", dir]);
    assert_eq!((code, out["state"].as_str()), (0, Some("unchanged")), "{out}");
}

#[test]
fn an_install_that_cannot_start_changing_anything_exits_1_unchanged() {
    let w = world(false);
    // A file where the aside folder goes: the first thing the install does is make that folder.
    std::fs::create_dir_all(w.nevr.path().join("installer")).unwrap();
    std::fs::write(w.nevr.path().join("installer/aside"), b"in the way").unwrap();
    let (code, out, _) = run(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap()]);
    assert_eq!((code, out["state"].as_str(), out["failed"].as_bool()), (1, Some("unchanged"), Some(true)), "{out}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
    assert!(!w.nevr.path().join("nevr-runtime.dll").exists());
}

// A log that stops accepting writes part-way through a run (a disk that fills, a size limit): the shape is a per-file size
// limit (`ulimit -f`, SIGXFSZ ignored so the write fails with EFBIG instead of killing the process). The log file is padded
// so the limit falls just before the record named by `target`; every record before it is written and that one and the rest
// are lost. A probe world runs the same scenario with a healthy log to find where the record sits.

fn setup_failing_host_write(w: &World) {
    use std::os::unix::fs::PermissionsExt;
    // The game folder refuses the host's temporary file, so the host write fails after the runtime was written.
    std::fs::set_permissions(w.game.path(), std::fs::Permissions::from_mode(0o555)).unwrap();
}

fn reopen_game_folder(w: &World) {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(w.game.path(), std::fs::Permissions::from_mode(0o755)).unwrap();
}

/// Bytes of log written before the first record named `target` in a healthy run of the scenario.
fn log_bytes_before(target: &str, scenario: fn(&World)) -> usize {
    let probe = world(false);
    scenario(&probe);
    let _ = run(&probe, &["pc", "install", "--dir", probe.game.path().to_str().unwrap()]);
    reopen_game_folder(&probe);
    let text = std::fs::read_to_string(probe.root.path().join("logs/nevr-install.jsonl")).unwrap();
    let mut before = 0;
    for line in text.lines() {
        let v: Value = serde_json::from_str(line).unwrap();
        if v["event"] == target {
            return before;
        }
        before += line.len() + 1;
    }
    panic!("the probe run never wrote {target}: {text}");
}

fn run_with_log_cut_before(w: &World, target: &str, scenario: fn(&World)) -> (i32, Value, String) {
    let before = log_bytes_before(target, scenario);
    let kb = (before + 100).div_ceil(1024);
    let pad = kb * 1024 - before - 60;
    std::fs::create_dir_all(w.root.path().join("logs")).unwrap();
    std::fs::write(w.root.path().join("logs/nevr-install.jsonl"), format!("{}\n", "#".repeat(pad - 1))).unwrap();
    scenario(w);
    let o = Command::new("bash")
        .args(["-c", "trap '' XFSZ; ulimit -f \"$0\"; exec \"$@\"", &kb.to_string(), env!("CARGO_BIN_EXE_nevr-install")])
        .args(["pc", "install", "--dir", w.game.path().to_str().unwrap()])
        .args(["--root", w.root.path().to_str().unwrap(), "--nevr-root", w.nevr.path().to_str().unwrap(), "--dev-nevr-root", "--base", &w.base,"--pins", w.pins.to_str().unwrap(), "--now", NOW])
        .output()
        .unwrap();
    reopen_game_folder(w);
    let out: Value = serde_json::from_slice(&o.stdout).unwrap_or_else(|e| panic!("stdout is not JSON ({e}): {}", String::from_utf8_lossy(&o.stdout)));
    (o.status.code().unwrap(), out, String::from_utf8_lossy(&o.stderr).to_string())
}

#[test]
fn a_log_that_fails_while_the_rollback_is_due_does_not_stop_the_rollback() {
    let w = world(false);
    let (code, out, _) = run_with_log_cut_before(&w, "install_failed", setup_failing_host_write);
    assert_eq!((code, out["state"].as_str()), (3, Some("restored")), "{out}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
    assert!(!w.nevr.path().join("nevr-runtime.dll").exists(), "the runtime written before the host is taken back");
    assert!(!w.nevr.path().join("installer/nevr-install.txt").exists());
    let reason = out["reason"].as_str().unwrap();
    // The record of the failure and the record of the one file put back were both lost.
    assert!(reason.contains("; 2 log record(s) could not be written during the rollback: FAIL step 0: cannot write the log"), "the lost records are said: {reason}");
}

#[test]
fn a_log_that_fails_after_the_install_does_not_turn_it_into_unchanged() {
    let w = world(false);
    let (code, out, _) = run_with_log_cut_before(&w, "run_end", |_| {});
    assert_eq!((code, out["state"].as_str(), out["failed"].as_bool()), (0, Some("installed"), Some(false)), "{out}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), HOST);
    assert_eq!(std::fs::read(w.nevr.path().join("nevr-runtime.dll")).unwrap(), RT);
    assert!(out["reason"].as_str().unwrap().contains("the log could not record the end"), "{out}");
}

#[test]
fn a_log_that_fails_at_the_install_record_failure_still_ends_installed() {
    // Both the informational record and the log fail after the files are installed.
    fn block_record(w: &World) {
        std::fs::create_dir_all(w.root.path().join("state/install-record.json")).unwrap();
    }
    let w = world(false);
    let (code, out, _) = run_with_log_cut_before(&w, "install_record", block_record);
    assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{out}");
    let reason = out["reason"].as_str().unwrap();
    assert!(reason.contains("install-record.json was not written") && reason.contains("the log could not record"), "{reason}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), HOST);
}

// ---- the package folder (install.ps1 passes the folder it sits in) --------------------------------------------------

fn package_dir(host: &[u8], runtime: Option<&[u8]>) -> tempfile::TempDir {
    let d = tempfile::tempdir().unwrap();
    std::fs::write(d.path().join("BugSplat64.dll"), host).unwrap();
    if let Some(r) = runtime {
        std::fs::write(d.path().join("nevr-runtime.dll"), r).unwrap();
    }
    d
}

fn record(w: &World, event: &str) -> Option<Value> {
    records(w).into_iter().find(|r| r["event"] == event)
}

#[test]
fn a_package_folder_that_matches_the_manifest_installs_without_downloading() {
    let w = world_with(false, false, false); // the zip is not served: a download would fail the run
    let pkg = package_dir(HOST, Some(RT));
    let (code, out, err) = run(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--package-dir", pkg.path().to_str().unwrap()]);
    assert_eq!(code, 0, "{out} {err}");
    assert_eq!(out["state"], "installed");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), HOST);
    assert_eq!(std::fs::read(w.nevr.path().join("nevr-runtime.dll")).unwrap(), RT);
    let local = record(&w, "package_local").expect("package_local record");
    assert_eq!(local["ok"], true, "{local}");
    assert!(record(&w, "artifact_fetch").is_none(), "nothing was downloaded");
}

#[test]
fn a_package_dll_that_differs_from_the_manifest_is_not_installed_the_manifests_package_is_downloaded() {
    let w = world(false);
    let pkg = package_dir(b"tampered host", Some(RT));
    let (code, out, err) = run(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--package-dir", pkg.path().to_str().unwrap()]);
    assert_eq!(code, 0, "{out} {err}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), HOST, "the manifest's bytes, not the folder's");
    let local = record(&w, "package_local").expect("package_local record");
    assert_eq!(local["ok"], false);
    assert!(local["reason"].as_str().unwrap().contains("BugSplat64.dll"), "{local}");
    assert!(record(&w, "artifact_fetch").is_some());
}

#[test]
fn a_package_folder_missing_a_dll_downloads_the_manifests_package() {
    let w = world(false);
    let pkg = package_dir(HOST, None);
    let (code, out, _) = run(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--package-dir", pkg.path().to_str().unwrap()]);
    assert_eq!(code, 0, "{out}");
    assert_eq!(std::fs::read(w.nevr.path().join("nevr-runtime.dll")).unwrap(), RT);
    assert_eq!(record(&w, "package_local").unwrap()["ok"], false);
}

#[test]
fn an_unreachable_server_uses_the_manifest_stored_by_an_earlier_run_and_still_verifies_it() {
    let w = world_with(false, false, false);
    let (code, _, _) = run(&w, &["manifest", "verify"]);
    assert_eq!(code, 0);
    let pkg = package_dir(HOST, Some(RT));
    let dead = "http://127.0.0.1:1";
    let (code, out, err) = run_with(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--package-dir", pkg.path().to_str().unwrap()], dead, &[]);
    assert_eq!(code, 0, "{out} {err}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), HOST);
    let checks: Vec<Value> = records(&w).into_iter().filter(|r| r["event"] == "manifest_check").collect();
    assert_eq!(checks.last().unwrap()["ok"], true);
    assert_eq!(checks.last().unwrap()["source"], "stored", "{checks:?}");
}

#[test]
fn an_unreachable_server_with_no_stored_manifest_changes_nothing() {
    let w = world_with(false, false, false);
    let pkg = package_dir(HOST, Some(RT));
    let (code, out, _) = run_with(&w, &["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--package-dir", pkg.path().to_str().unwrap()], "http://127.0.0.1:1", &[]);
    assert_eq!(code, 1, "{out}");
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
}

#[test]
fn a_stored_manifest_that_is_stale_is_not_used_offline() {
    let w = world_with(false, false, false);
    assert_eq!(run(&w, &["manifest", "verify"]).0, 0);
    let pkg = package_dir(HOST, Some(RT));
    let o = Command::new(env!("CARGO_BIN_EXE_nevr-install"))
        .args(["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--package-dir", pkg.path().to_str().unwrap()])
        .args(["--root", w.root.path().to_str().unwrap(), "--nevr-root", w.nevr.path().to_str().unwrap(), "--dev-nevr-root", "--base", "http://127.0.0.1:1","--pins", w.pins.to_str().unwrap(), "--now", "2026-12-01T00:00:00Z"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1), "{}", String::from_utf8_lossy(&o.stdout));
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
}

// ---- a local tester APK (--artifact-dir) ------------------------------------------------------------------------------

const DEV_SIGNER: &str = "3333333333333333333333333333333333333333333333333333333333333333";

/// Fake adb (stateful: a marker file says the package got installed), aapt2 and apksigner on a PATH directory.
fn fake_tools(pkg_name: &str, code: u32, signer: &str) -> tempfile::TempDir {
    let d = tempfile::tempdir().unwrap();
    let put = |name: &str, body: String| {
        let p = d.path().join(name);
        std::fs::write(&p, body).unwrap();
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755)).unwrap();
    };
    put(
        "adb",
        format!(
            r#"#!/bin/bash
if [ "$1" = devices ]; then printf 'List of devices attached
Q1	device
'; exit 0; fi
[ "$1" = -s ] && shift 2
echo "$*" >> "$FAKE_STATE/adb.log"
case "$1" in
  install) echo Success; touch "$FAKE_STATE/installed"; exit 0 ;;
  uninstall) echo Success; exit 0 ;;
  shell)
    case "$2" in
      "pm path"*) if [ -e "$FAKE_STATE/installed" ]; then echo package:/x; exit 0; fi; exit 1 ;;
      "dumpsys package"*) echo "    versionCode={code} minSdk=29"; exit 0 ;;
      "df -Pk"*) printf 'Filesystem 1K-blocks Used Available Use%% Mounted
x 1 1 9000000 1%% /
'; exit 0 ;;
      "for f in"*) echo "/sdcard/Android/media/com.readyatdawn.r15/files/_data/x 3"; exit 0 ;;
      *) exit 0 ;;
    esac ;;
esac
exit 0
"#
        ),
    );
    put("aapt2", format!("#!/bin/bash\necho \"package: name='{pkg_name}' versionCode='{code}' versionName='x'\"\n"));
    put("apksigner", format!("#!/bin/bash\necho 'Signer #1 certificate SHA-256 digest: {signer}'\n"));
    d
}

fn dev_dir(apk: &[u8], sidecar: Option<&str>) -> tempfile::TempDir {
    let d = tempfile::tempdir().unwrap();
    std::fs::write(d.path().join("r15.apk"), apk).unwrap();
    if let Some(s) = sidecar {
        std::fs::write(d.path().join("r15.apk.sha256"), format!("{s}  r15.apk\n")).unwrap();
    }
    d
}

fn run_dev(w: &World, tools: &tempfile::TempDir, state: &tempfile::TempDir, dir: &tempfile::TempDir, extra: &[&str]) -> (i32, Value, String) {
    let path = format!("{}:{}", tools.path().display(), std::env::var("PATH").unwrap());
    let mut args = vec!["quest", "install", "--artifact-dir", dir.path().to_str().unwrap()];
    args.extend_from_slice(extra);
    run_with(w, &args, &w.base, &[("PATH", path), ("FAKE_STATE", state.path().display().to_string())])
}

#[test]
fn a_local_apk_is_installed_checked_and_every_step_is_logged_as_an_unverified_dev_artifact() {
    let w = world_with(false, true, true); // the release has game-data but no quest-apk
    let (tools, state) = (fake_tools("com.readyatdawn.r15", 7, DEV_SIGNER), tempfile::tempdir().unwrap());
    let dir = dev_dir(b"apk-bytes", Some(&sha256_hex(b"apk-bytes")));
    let (code, out, err) = run_dev(&w, &tools, &state, &dir, &[]);
    assert_eq!(code, 0, "{out} {err}");
    assert_eq!(out["state"], "installed");
    let calls = std::fs::read_to_string(state.path().join("adb.log")).unwrap();
    assert!(calls.contains(&format!("install -g {}", dir.path().join("r15.apk").display())), "{calls}");
    let dev = record(&w, "dev_artifact").expect("dev_artifact record");
    assert_eq!((dev["package"].as_str(), dev["version_code"].as_u64(), dev["sha256"].as_str()), (Some("com.readyatdawn.r15"), Some(7), Some(sha256_hex(b"apk-bytes").as_str())), "{dev}");
    assert_eq!(dev["trust"], "unverified dev artifact");
    for r in records(&w).iter().filter(|r| r["event"] == "step_start" || r["event"] == "step_end" || r["event"] == "plan") {
        assert_eq!(r["trust"], "unverified dev artifact", "{r}");
    }
    assert!(records(&w).iter().filter(|r| r["event"] == "step_start").count() >= 3);
    assert!(record(&w, "artifact_fetch").is_none(), "the local file is used, not downloaded");
    let rec: Value = serde_json::from_slice(&std::fs::read(w.root.path().join("state/install-record.json")).unwrap()).unwrap();
    assert_eq!(rec["dev_artifact"], true, "the install record says the APK was a local one: {rec}");
}

#[test]
fn a_local_apk_whose_bytes_do_not_match_its_sidecar_is_never_installed() {
    let w = world_with(false, true, true);
    let (tools, state) = (fake_tools("com.readyatdawn.r15", 7, DEV_SIGNER), tempfile::tempdir().unwrap());
    let dir = dev_dir(b"apk-bytes", Some(&sha256_hex(b"other bytes")));
    let (code, out, _) = run_dev(&w, &tools, &state, &dir, &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("sha256"), "{out}");
    assert!(!state.path().join("adb.log").exists(), "no device call was made");
}

#[test]
fn a_local_apk_without_a_sidecar_is_refused() {
    let w = world_with(false, true, true);
    let (tools, state) = (fake_tools("com.readyatdawn.r15", 7, DEV_SIGNER), tempfile::tempdir().unwrap());
    let dir = dev_dir(b"apk-bytes", None);
    let (code, out, _) = run_dev(&w, &tools, &state, &dir, &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("sidecar"), "{out}");
}

#[test]
fn a_local_apk_for_another_package_is_refused() {
    let w = world_with(false, true, true);
    let (tools, state) = (fake_tools("com.example.other", 7, DEV_SIGNER), tempfile::tempdir().unwrap());
    let dir = dev_dir(b"apk-bytes", Some(&sha256_hex(b"apk-bytes")));
    let (code, out, _) = run_dev(&w, &tools, &state, &dir, &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("com.example.other"), "{out}");
    assert!(!state.path().join("adb.log").exists());
}

#[test]
fn a_local_apk_signed_with_no_certificate_the_tool_can_read_is_refused() {
    let w = world_with(false, true, true);
    let (tools, state) = (fake_tools("com.readyatdawn.r15", 7, "not-a-digest"), tempfile::tempdir().unwrap());
    let dir = dev_dir(b"apk-bytes", Some(&sha256_hex(b"apk-bytes")));
    let (code, out, _) = run_dev(&w, &tools, &state, &dir, &[]);
    assert_eq!(code, 1, "{out}");
    assert!(!state.path().join("adb.log").exists());
}

#[test]
fn an_artifact_dir_with_two_apks_is_refused() {
    let w = world_with(false, true, true);
    let (tools, state) = (fake_tools("com.readyatdawn.r15", 7, DEV_SIGNER), tempfile::tempdir().unwrap());
    let dir = dev_dir(b"apk-bytes", Some(&sha256_hex(b"apk-bytes")));
    std::fs::write(dir.path().join("second.apk"), b"x").unwrap();
    let (code, out, _) = run_dev(&w, &tools, &state, &dir, &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("exactly one"), "{out}");
}

#[test]
fn a_run_without_an_artifact_dir_logs_no_dev_trust_field() {
    // The release has no quest-apk, so the run is refused; none of its records may claim a dev artifact.
    let w = world(false);
    let (code, _, _) = run(&w, &["quest", "install", "--no-device"]);
    assert_eq!(code, 1);
    assert!(records(&w).iter().all(|r| r.get("trust").is_none()));
}

#[test]
fn a_sha256sums_line_next_to_the_apk_is_its_expected_hash() {
    let w = world_with(false, true, true);
    let tools = fake_tools("com.readyatdawn.r15", 7, DEV_SIGNER);
    let sha = sha256_hex(b"apk-bytes");
    // The plain form, and the `*name` form of binary mode (each against a headset that has nothing installed).
    for line in [format!("{sha}  r15.apk\n"), format!("{sha} *r15.apk\n")] {
        let state = tempfile::tempdir().unwrap();
        let dir = dev_dir(b"apk-bytes", None);
        std::fs::write(dir.path().join("SHA256SUMS"), &line).unwrap();
        let (code, out, err) = run_dev(&w, &tools, &state, &dir, &[]);
        assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{line:?}: {out} {err}");
    }
    // A line for another file, or two lines for this one, is not an expectation.
    for sums in [format!("{sha}  other.apk\n"), format!("{sha}  r15.apk\n{sha}  r15.apk\n")] {
        let state = tempfile::tempdir().unwrap();
        let dir = dev_dir(b"apk-bytes", None);
        std::fs::write(dir.path().join("SHA256SUMS"), &sums).unwrap();
        let (code, out, _) = run_dev(&w, &tools, &state, &dir, &[]);
        assert_eq!(code, 1, "{sums:?}: {out}");
        assert!(out["reason"].as_str().unwrap().contains("no sidecar"), "{out}");
    }
}

// ---- a Quest install from the signed release, against a fake headset ---------------------------------------------------

const QUEST_APK: &[u8] = b"quest-apk-bytes";
const QUEST_DATA_FILE: &[u8] = b"abc";

/// A release with a `quest-apk` (versionCode 7, signed by `DEV_SIGNER`) and a one-file `game-data`, both served.
fn quest_world() -> World {
    let keys = tempfile::tempdir().unwrap();
    let k = make_key(keys.path(), "k");
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        z.start_file("_data/x", zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(QUEST_DATA_FILE).unwrap();
        z.finish().unwrap();
    }
    let data_zip = buf.into_inner();
    let artifacts = vec![
        json!({"kind": "quest-apk", "platform": "android-arm64", "path": "v1.0.0/nevr-runtime-v1.0.0-quest.apk", "size": QUEST_APK.len(), "sha256": sha256_hex(QUEST_APK),
            "android": {"package": "com.readyatdawn.r15", "version_code": 7, "signer_cert_sha256": [DEV_SIGNER]}}),
        json!({"kind": "game-data", "platform": "android-arm64", "path": "data/echovr-data-aaaaaaaaaaaa.zip", "size": data_zip.len(), "sha256": sha256_hex(&data_zip),
            "game_data": {"data_version": "1", "echo_build": "2"},
            "contents": [{"path": "_data/x", "sha256": sha256_hex(QUEST_DATA_FILE), "size": QUEST_DATA_FILE.len()}]}),
    ];
    let m = manifest_json(5, json!({"channels": {"beta": "1.0.0"}, "releases": {"1.0.0": {"artifacts": artifacts}}}));
    let sig = sign(keys.path(), &k, &m, false);
    let routes = HashMap::from([
        ("/manifest.json".to_string(), m),
        ("/manifest.json.sig".to_string(), sig),
        ("/v1.0.0/nevr-runtime-v1.0.0-quest.apk".to_string(), QUEST_APK.to_vec()),
        ("/data/echovr-data-aaaaaaaaaaaa.zip".to_string(), data_zip),
    ]);
    finish(serve_forever(routes), &k, keys)
}

/// A fake `adb` that behaves like a headset: `FAKE_STATE/installed` and `version` say what is installed (`install` makes it
/// versionCode 7), `extracted` says the game data is there (`unzip` makes it so). Every call after `-s <serial>` is
/// logged to `adb.log`, and the target of its stdin to `stdin.log`. `FAKE_DEVICES`, `FAKE_PM_RC`, `FAKE_PM_OUT` and
/// `FAKE_PUSH_RC` change what it answers. aapt2 and apksigner report the release's own package, versionCode and signer.
fn fake_headset() -> tempfile::TempDir {
    let d = fake_tools("com.readyatdawn.r15", 7, DEV_SIGNER);
    let adb = d.path().join("adb");
    std::fs::write(
        &adb,
        r#"#!/bin/bash
S="$FAKE_STATE"
echo "stdin=$(readlink /proc/self/fd/0)" >> "$S/stdin.log"
if [ "$1" = devices ]; then printf 'List of devices attached\n%b' "${FAKE_DEVICES-Q1\tdevice\n}"; exit 0; fi
[ "$1" = -s ] && shift 2
echo "$*" >> "$S/adb.log"
case "$1" in
  install) echo Success; echo 7 > "$S/version"; touch "$S/installed"; exit 0 ;;
  uninstall) echo Success; exit 0 ;;
  push) exit "${FAKE_PUSH_RC:-0}" ;;
  shell)
    case "$2" in
      "pm path"*)
        if [ -n "${FAKE_PM_RC:-}" ]; then echo "${FAKE_PM_OUT:-}"; exit "$FAKE_PM_RC"; fi
        if [ -e "$S/installed" ]; then echo package:/x; exit 0; fi
        exit 1 ;;
      "dumpsys package"*) echo "    versionCode=$(cat "$S/version") minSdk=29"; exit 0 ;;
      "getprop"*) echo fake; exit 0 ;;
      "df -Pk"*) printf 'Filesystem 1K-blocks Used Available Use%% Mounted\nx 1 1 9000000 1%% /\n'; exit 0 ;;
      "for f in"*) [ -e "$S/extracted" ] && echo "/sdcard/Android/media/com.readyatdawn.r15/files/_data/x 3"; exit 0 ;;
      "mkdir -p"*) touch "$S/extracted"; exit 0 ;;
      *) exit 0 ;;
    esac ;;
esac
exit 0
"#,
    )
    .unwrap();
    d
}

fn run_quest(w: &World, tools: &tempfile::TempDir, state: &tempfile::TempDir, args: &[&str], envs: &[(&str, &str)]) -> (i32, Value, String) {
    let path = format!("{}:{}", tools.path().display(), std::env::var("PATH").unwrap());
    let mut all = vec![("PATH", path), ("FAKE_STATE", state.path().display().to_string())];
    all.extend(envs.iter().map(|(k, v)| (*k, v.to_string())));
    run_with(w, args, &w.base, &all)
}

fn adb_calls(state: &tempfile::TempDir) -> Vec<String> {
    std::fs::read_to_string(state.path().join("adb.log")).unwrap_or_default().lines().map(String::from).collect()
}

/// `calls` holds one entry starting with each of `wanted`, in that order (other calls may lie between them).
fn assert_in_order(calls: &[String], wanted: &[&str]) {
    let mut at = 0;
    for w in wanted {
        match calls[at..].iter().position(|c| c.starts_with(w)) {
            Some(i) => at += i + 1,
            None => panic!("{w:?} is missing or out of order in {calls:#?}"),
        }
    }
}

#[test]
fn a_quest_install_over_adb_runs_the_whole_sequence_in_order_and_removes_the_temp_zip() {
    let (w, tools, state) = (quest_world(), fake_headset(), tempfile::tempdir().unwrap());
    // An older build of the game is installed and the game data is absent.
    std::fs::write(state.path().join("installed"), b"").unwrap();
    std::fs::write(state.path().join("version"), b"6\n").unwrap();
    let (code, out, err) = run_quest(&w, &tools, &state, &["quest", "install"], &[]);
    assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{out} {err}");
    let calls = adb_calls(&state);
    let apk = w.root.path().join("work/nevr-runtime-v1.0.0-quest.apk");
    let zip = w.root.path().join("work/echovr-data-aaaaaaaaaaaa.zip");
    let media = "/sdcard/Android/media/com.readyatdawn.r15/files";
    assert_in_order(
        &calls,
        &[
            "shell pm path com.readyatdawn.r15",
            "shell df -Pk /sdcard",
            "shell df -Pk /data/local/tmp",
            &format!("install -r -g {}", apk.display()),
            &format!("push {} /data/local/tmp/_nevr_data.zip", zip.display()),
            &format!("shell mkdir -p '{media}' && cd '{media}' && unzip -o '/data/local/tmp/_nevr_data.zip'"),
            "shell rm -f '/data/local/tmp/_nevr_data.zip'",
            "shell pm path com.readyatdawn.r15", // the read-back after the install
        ],
    );
    assert!(!calls.iter().any(|c| c.starts_with("uninstall")), "{calls:?}");
    assert!(w.root.path().join("state/install-record.json").exists());
    // A second run finds the package current and the data present, and touches nothing.
    std::fs::remove_file(state.path().join("adb.log")).unwrap();
    let (code, out, _) = run_quest(&w, &tools, &state, &["quest", "install"], &[]);
    assert_eq!((code, out["state"].as_str()), (0, Some("unchanged")), "{out}");
    assert!(!adb_calls(&state).iter().any(|c| c.starts_with("install") || c.starts_with("push") || c.starts_with("uninstall")));
}

#[test]
fn a_quest_install_on_a_headset_without_the_game_installs_instead_of_aborting() {
    let (w, tools, state) = (quest_world(), fake_headset(), tempfile::tempdir().unwrap());
    let (code, out, err) = run_quest(&w, &tools, &state, &["quest", "install"], &[]);
    assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{out} {err}");
    let apk = w.root.path().join("work/nevr-runtime-v1.0.0-quest.apk");
    assert!(adb_calls(&state).contains(&format!("install -g {}", apk.display())), "{:?}", adb_calls(&state));
}

#[test]
fn a_failing_package_check_stops_the_run_before_anything_is_installed() {
    let (w, tools, state) = (quest_world(), fake_headset(), tempfile::tempdir().unwrap());
    let (code, out, _) = run_quest(&w, &tools, &state, &["quest", "install"], &[("FAKE_PM_RC", "7"), ("FAKE_PM_OUT", "Error: boom")]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("package check failed (exit 7): Error: boom"), "{out}");
    assert!(!adb_calls(&state).iter().any(|c| c.starts_with("install") || c.starts_with("uninstall") || c.starts_with("push")), "{:?}", adb_calls(&state));
    assert!(!w.root.path().join("state/install-record.json").exists());
}

#[test]
fn two_attached_devices_stop_the_run_before_any_call_to_either() {
    let (w, tools, state) = (quest_world(), fake_headset(), tempfile::tempdir().unwrap());
    let (code, out, _) = run_quest(&w, &tools, &state, &["quest", "install"], &[("FAKE_DEVICES", "Q1\\tdevice\\nQ2\\tdevice\\n")]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("2 adb devices attached"), "{out}");
    assert_eq!(adb_calls(&state), Vec::<String>::new());
    assert_eq!(out["state"], "unchanged");
}

#[test]
fn a_run_that_changes_the_package_but_fails_to_push_says_needs_user() {
    let (w, tools, state) = (quest_world(), fake_headset(), tempfile::tempdir().unwrap());
    let (code, out, _) = run_quest(&w, &tools, &state, &["quest", "install"], &[("FAKE_PUSH_RC", "1")]);
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    assert!(adb_calls(&state).iter().any(|c| c.starts_with("install")), "the package was changed before the push");
    assert!(out["reason"].as_str().unwrap().contains("adb push failed"), "{out}");
    assert!(!w.root.path().join("state/install-record.json").exists(), "no install record for a run that did not finish");
}

#[test]
fn adb_is_never_given_the_terminal_or_the_callers_stdin() {
    let (w, tools, state) = (quest_world(), fake_headset(), tempfile::tempdir().unwrap());
    let path = format!("{}:{}", tools.path().display(), std::env::var("PATH").unwrap());
    let mut child = Command::new(env!("CARGO_BIN_EXE_nevr-install"))
        .args(["quest", "install", "--root", w.root.path().to_str().unwrap(), "--base", &w.base, "--pins", w.pins.to_str().unwrap(), "--now", NOW])
        .env("PATH", path)
        .env("FAKE_STATE", state.path())
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    // The caller's stdin is a pipe with data in it: an adb that inherited it would read it.
    child.stdin.as_mut().unwrap().write_all(b"typed at the terminal\n").unwrap();
    let o = child.wait_with_output().unwrap();
    assert_eq!(o.status.code(), Some(0), "{} {}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr));
    let seen: Vec<String> = std::fs::read_to_string(state.path().join("stdin.log")).unwrap().lines().map(String::from).collect();
    assert!(seen.len() >= 10, "adb devices, the observations, install, push, extract and the read-back: {seen:?}");
    assert!(seen.iter().all(|s| s == "stdin=/dev/null"), "{seen:?}");
}

// ---- #1285: a game folder an Echo VR Launcher manages is not installed over -------------------------------------------

/// A host as the launcher-era release shipped it: the identity stamp and the CDN agent string inside an MZ blob.
const LAUNCHER_ERA_HOST: &[u8] = b"MZ\0\0 NEVR-BUILD 5.4.3 bb87761815ca2ac574d60626ccb139d18c3681e5 \0 nevr-runtime/1.0 \0";

/// `<launcher library>/<id>/ready-at-dawn-echo-arena/bin/win10`, holding the launcher-era host and an echovr.exe.
struct LauncherInstall {
    library: tempfile::TempDir,
    data: tempfile::TempDir,
    game: std::path::PathBuf,
}

fn launcher_install() -> LauncherInstall {
    let library = tempfile::tempdir().unwrap();
    let game = library.path().join("pc-latest").join("ready-at-dawn-echo-arena").join("bin").join("win10");
    std::fs::create_dir_all(&game).unwrap();
    std::fs::write(game.join("echovr.exe"), b"exe").unwrap();
    std::fs::write(game.join("BugSplat64.dll"), LAUNCHER_ERA_HOST).unwrap();
    LauncherInstall { library, data: tempfile::tempdir().unwrap(), game }
}

impl LauncherInstall {
    /// launcher.json as the launcher writes it, listing the install at `root`.
    fn list(&self, root: &str) {
        let text = json!({"schema": 3, "library": self.library.path().to_str().unwrap(), "versions": [{"id": "pc-latest", "name": "PC", "root": root, "external": false}]});
        std::fs::write(self.data.path().join("launcher.json"), serde_json::to_vec_pretty(&text).unwrap()).unwrap();
    }

    fn listed_root(&self) -> String {
        self.library.path().join("pc-latest").to_str().unwrap().to_string()
    }

    fn install(&self, w: &World) -> (i32, Value, String) {
        run(w, &["pc", "install", "--dir", self.game.to_str().unwrap(), "--launcher-dir", self.data.path().to_str().unwrap()])
    }
}

fn tree_of(dir: &Path) -> Vec<(String, Vec<u8>)> {
    let mut out = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).unwrap() {
            let p = e.unwrap().path();
            if p.is_dir() {
                stack.push(p);
            } else {
                out.push((p.strip_prefix(dir).unwrap().to_string_lossy().into_owned(), std::fs::read(&p).unwrap()));
            }
        }
    }
    out.sort();
    out
}

fn assert_launcher_refused(w: &World, l: &LauncherInstall, before: &[(String, Vec<u8>)], out: &Value, code: i32) {
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    let why = out["reason"].as_str().unwrap();
    assert!(why.contains("Echo VR Launcher") && why.contains("5.4.3") && why.contains("through the launcher"), "{why}");
    assert_eq!(tree_of(&l.game), before, "the launcher's game folder is untouched");
    assert!(tree_of(w.nevr.path()).is_empty(), "nothing is written to the nevr root");
    let ev = events(w);
    assert!(!ev.contains(&"artifact_fetch".to_string()), "no package is downloaded for a refusal: {ev:?}");
}

#[test]
fn a_game_folder_the_launcher_lists_is_refused_naming_the_launcher_and_the_version_and_nothing_changes() {
    let w = world(false);
    let l = launcher_install();
    l.list(&l.listed_root());
    let before = tree_of(&l.game);
    let (code, out, err) = l.install(&w);
    assert_launcher_refused(&w, &l, &before, &out, code);
    assert!(records(&w).iter().any(|r| r["event"] == "plan" && r["decision"] == "needs-user"), "{err}");
}

#[test]
fn the_launchers_own_spelling_of_the_folder_still_matches() {
    let w = world(false);
    for spelling in ["with a trailing separator", "with a byte order mark", "a root written with backslashes"] {
        let l = launcher_install();
        let root = l.listed_root();
        let listed = if spelling.contains("backslashes") { root.replace('/', "\\") } else { format!("{root}/") };
        l.list(&listed);
        if spelling.contains("byte order mark") {
            let text = std::fs::read(l.data.path().join("launcher.json")).unwrap();
            std::fs::write(l.data.path().join("launcher.json"), [&b"\xEF\xBB\xBF"[..], &text].concat()).unwrap();
        }
        let before = tree_of(&l.game);
        let (code, out, _) = l.install(&w);
        assert!(code == 2, "{spelling}: {out}");
        assert_launcher_refused(&w, &l, &before, &out, code);
    }
}

#[cfg(unix)]
#[test]
fn a_launcher_root_that_is_a_link_to_the_game_folder_matches() {
    let w = world(false);
    let l = launcher_install();
    let alias = l.library.path().join("alias");
    std::os::unix::fs::symlink(l.library.path().join("pc-latest"), &alias).unwrap();
    l.list(alias.to_str().unwrap());
    let before = tree_of(&l.game);
    let (code, out, _) = l.install(&w);
    assert_launcher_refused(&w, &l, &before, &out, code);
}

#[test]
fn a_launcher_that_lists_another_folder_or_none_does_not_stop_an_install_that_adopts_the_old_host() {
    let w = world(false);
    let l = launcher_install();
    l.list(l.library.path().join("some-other-version").to_str().unwrap());
    let (code, out, err) = l.install(&w);
    assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{out} {err}");
    assert_eq!(out["plan"]["action"], "Adopt");
    assert_eq!(std::fs::read(l.game.join("BugSplat64.dll")).unwrap(), HOST);
    // No launcher.json at all is the same: the launcher is not there.
    let w2 = world(false);
    let l2 = launcher_install();
    let (code, out, err) = l2.install(&w2);
    assert_eq!((code, out["state"].as_str()), (0, Some("installed")), "{out} {err}");
}

#[test]
fn a_launcher_json_that_cannot_be_read_stops_the_run_and_says_so() {
    let w = world(false);
    let l = launcher_install();
    std::fs::write(l.data.path().join("launcher.json"), b"{ not json").unwrap();
    let before = tree_of(&l.game);
    let (code, out, _) = l.install(&w);
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    assert!(out["reason"].as_str().unwrap().contains("cannot tell whether a launcher manages"), "{out}");
    assert_eq!(tree_of(&l.game), before);
    assert!(tree_of(w.nevr.path()).is_empty());
}

#[test]
fn the_record_names_its_owner_and_a_record_owned_by_another_installer_is_refused() {
    let w = world(false);
    let dir = w.game.path().to_str().unwrap();
    let (code, out, err) = run(&w, &["pc", "install", "--dir", dir]);
    assert_eq!(code, 0, "{out} {err}");
    let record = w.nevr.path().join("installer").join("nevr-install.txt");
    let text = std::fs::read_to_string(&record).unwrap();
    assert!(text.lines().any(|l| l == "owner=nevr-install"), "{text}");
    std::fs::write(&record, text.replace("owner=nevr-install", "owner=launcher")).unwrap();
    let before = tree_of(w.game.path());
    let (code, out, _) = run(&w, &["pc", "install", "--dir", dir, "--version", "1.0.0"]);
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    let why = out["reason"].as_str().unwrap();
    assert!(why.contains("owned by launcher") && why.contains("its own installer"), "{why}");
    assert_eq!(tree_of(w.game.path()), before);
}

#[test]
fn the_2017_builds_win7_folder_is_matched_too() {
    let w = world(false);
    let l = launcher_install();
    let win7 = l.library.path().join("pc-latest").join("ready-at-dawn-echo-arena").join("bin").join("win7");
    std::fs::create_dir_all(&win7).unwrap();
    std::fs::write(win7.join("echovr.exe"), b"exe").unwrap();
    std::fs::write(win7.join("BugSplat64.dll"), LAUNCHER_ERA_HOST).unwrap();
    l.list(&l.listed_root());
    let before = tree_of(&win7);
    let (code, out, _) = run(&w, &["pc", "install", "--dir", win7.to_str().unwrap(), "--launcher-dir", l.data.path().to_str().unwrap()]);
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    assert_eq!(tree_of(&win7), before);
}

#[test]
fn a_launcher_json_that_is_there_but_unreadable_is_not_taken_for_no_launcher() {
    let w = world(false);
    let l = launcher_install();
    std::fs::create_dir(l.data.path().join("launcher.json")).unwrap(); // reading it fails with something other than NotFound
    let before = tree_of(&l.game);
    let (code, out, _) = l.install(&w);
    assert_eq!((code, out["state"].as_str()), (2, Some("needs-user")), "{out}");
    assert!(out["reason"].as_str().unwrap().contains("cannot tell whether a launcher manages"), "{out}");
    assert_eq!(tree_of(&l.game), before);
}

#[test]
fn a_base_url_with_credentials_is_never_written_to_the_log_or_the_output() {
    // A closed loopback port: the fetch fails at connect, and the failure text is what the run writes down.
    let w = world(false);
    let base = "https://user:s3cret-token@127.0.0.1:1/nevr-runtime";
    let (code, out, err) = run_with(&w, &["manifest", "verify"], base, &[]);
    assert_eq!(code, 1, "{out}");
    let log = std::fs::read_to_string(w.root.path().join("logs/nevr-install.jsonl")).unwrap();
    for text in [out.to_string(), err, log] {
        assert!(!text.contains("s3cret-token") && !text.contains("user:"), "the credentials were written down: {text}");
    }
}

#[test]
fn manifest_verify_waits_for_the_state_lock_before_it_reads_the_state() {
    let w = world(false);
    let state = w.root.path().join("state");
    std::fs::create_dir_all(&state).unwrap();
    // Another run holds the state lock: the lock file the installer takes in the state directory.
    let holder = std::fs::OpenOptions::new().create(true).write(true).truncate(false).open(state.join("state.lock")).unwrap();
    holder.lock().unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_nevr-install"))
        .args(["manifest", "verify", "--root", w.root.path().to_str().unwrap(), "--nevr-root", w.nevr.path().to_str().unwrap(), "--base", &w.base, "--pins", w.pins.to_str().unwrap(), "--now", NOW])
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    // The run writes its start, then takes the lock, and only then fetches. While the lock is held it must fetch nothing: a
    // run that does not wait writes its fetch record within moments, so a three-second window tells the two apart.
    let log = w.root.path().join("logs/nevr-install.jsonl");
    let fetched = || std::fs::read_to_string(&log).unwrap_or_default().contains("\"manifest_fetch\"");
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
    while !fetched() && std::time::Instant::now() < deadline {
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
    assert!(!fetched(), "the run fetched the manifest while another run held the state lock");
    assert!(child.try_wait().unwrap().is_none(), "the run finished while another run held the state lock");
    drop(holder);
    assert_eq!(child.wait().unwrap().code(), Some(0));
    assert!(fetched(), "the run did not fetch once it had the lock");
}

#[test]
fn a_nevr_root_other_than_the_canonical_one_is_refused_unless_a_dev_run_names_it() {
    let w = world(false);
    let out = Command::new(env!("CARGO_BIN_EXE_nevr-install"))
        .args(["pc", "install", "--dir", w.game.path().to_str().unwrap(), "--root", w.root.path().to_str().unwrap(), "--nevr-root", w.nevr.path().to_str().unwrap(), "--base", &w.base, "--pins", w.pins.to_str().unwrap(), "--now", NOW])
        .env_remove("LOCALAPPDATA")
        .output()
        .unwrap();
    let v: Value = serde_json::from_slice(&out.stdout).unwrap_or_else(|e| panic!("stdout is not JSON ({e}): {}", String::from_utf8_lossy(&out.stdout)));
    assert_eq!(out.status.code(), Some(2), "{v}");
    assert!(v["reason"].as_str().unwrap_or("").contains("--dev-nevr-root"), "{v}");
    // Nothing is installed into the game folder: the host is still the game's own file.
    assert_eq!(std::fs::read(w.game.path().join("BugSplat64.dll")).unwrap(), GAME);
    assert!(!w.nevr.path().join("nevr-runtime.dll").exists());
}

#[test]
fn launch_check_refuses_a_root_the_host_does_not_load_from_before_it_reads_a_log() {
    let w = world(false);
    let out = Command::new(env!("CARGO_BIN_EXE_nevr-install"))
        .args(["pc", "launch-check", "--dir", w.game.path().to_str().unwrap(), "--root", w.root.path().to_str().unwrap(), "--nevr-root", w.nevr.path().to_str().unwrap(), "--base", &w.base, "--pins", w.pins.to_str().unwrap(), "--now", NOW])
        .env_remove("LOCALAPPDATA")
        .output()
        .unwrap();
    let v: Value = serde_json::from_slice(&out.stdout).unwrap_or_else(|e| panic!("stdout is not JSON ({e}): {}", String::from_utf8_lossy(&out.stdout)));
    assert_eq!((out.status.code(), v["state"].as_str()), (Some(2), Some("needs-user")), "{v}");
    assert!(v["reason"].as_str().unwrap_or("").contains("--dev-nevr-root"), "{v}");
}

#[test]
fn a_stage_download_that_fails_logs_the_base_without_its_credentials() {
    // The updater's state folder is <nevr root>/update/manifest (ADR 0024). An earlier run stores the pair there, so the
    // staging run can use it while the base is unreachable.
    let w = world(false);
    let nevr = w.nevr.path();
    let state = nevr.join("update").join("manifest");
    let pins = w.pins.to_str().unwrap();
    let bin = env!("CARGO_BIN_EXE_nevr-install");
    let stored = Command::new(bin)
        .args(["manifest", "verify", "--root", w.root.path().to_str().unwrap(), "--state-dir", state.to_str().unwrap(), "--base", &w.base, "--pins", pins, "--now", NOW])
        .output()
        .unwrap();
    assert_eq!(stored.status.code(), Some(0), "{}", String::from_utf8_lossy(&stored.stdout));
    let base = "https://user:s3cret-token@127.0.0.1:1/nevr-runtime";
    let o = Command::new(bin)
        .args(["pc", "stage", "--root", nevr.to_str().unwrap(), "--state-dir", state.to_str().unwrap(), "--base", base, "--pins", pins, "--now", NOW])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1), "{}", String::from_utf8_lossy(&o.stdout));
    let log = std::fs::read_to_string(nevr.join("logs/nevr-update.jsonl")).unwrap();
    assert!(!log.contains("s3cret-token") && !log.contains("user:"), "the credentials were written down: {log}");
    let fetch: Value = log.lines().map(|l| serde_json::from_str::<Value>(l).unwrap()).find(|r| r["event"] == "artifact_fetch").expect("the failed download is logged");
    assert_eq!(fetch["ok"], false, "{fetch}");
    assert!(fetch["url"].as_str().unwrap_or("").starts_with("https://127.0.0.1:1/nevr-runtime/v"), "{fetch}");
}
