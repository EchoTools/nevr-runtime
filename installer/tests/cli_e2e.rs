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
    let m = manifest_json(
        manifest_version,
        json!({"channels": {"beta": version}, "releases": {version: {"artifacts": [{
            "kind": "windows-zip", "platform": "windows-x86_64", "path": format!("v{version}/nevr-runtime-v{version}-windows.zip"),
            "size": zip_bytes.len(), "sha256": sha256_hex(&zip_bytes),
            "contents": [
                {"path": "BugSplat64.dll", "sha256": sha256_hex(HOST), "size": HOST.len()},
                {"path": "nevr-runtime.dll", "sha256": sha256_hex(RT), "size": RT.len()}]}]}}}),
    );
    let mut sig = sign(keys.path(), &k, &m, false);
    if tamper_sig {
        let mut m2 = m.clone();
        m2[10] ^= 1; // serve different bytes than were signed
        let base = serve_forever(HashMap::from([("/manifest.json".into(), m2), ("/manifest.json.sig".into(), sig.clone())]));
        sig.clear();
        return finish(base, &k, keys);
    }
    let base = serve_forever(HashMap::from([
        ("/manifest.json".into(), m),
        ("/manifest.json.sig".into(), sig),
        (format!("/v{version}/nevr-runtime-v{version}-windows.zip"), zip_bytes),
    ]));
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
    let o = Command::new(env!("CARGO_BIN_EXE_nevr-install"))
        .args(args)
        .args(["--root", dirs.root.path().to_str().unwrap(), "--nevr-root", dirs.nevr.path().to_str().unwrap(), "--base", &server.base, "--pins", server.pins.to_str().unwrap(), "--now", NOW])
        .output()
        .unwrap();
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
    // The test release has no quest-apk, so the plan refuses and says why.
    assert_eq!(code, 0, "{out}");
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
