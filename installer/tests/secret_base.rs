//! The success lines of the runs that fetch from a base carrying a query (#1353). The base is a `file://` address whose
//! folder name ends in `?token=...`, so the run reads its release from disk. Every line it writes that names the base
//! must show the address without its query. A failure test cannot reach these success lines, so they are tested here.

mod common;
use common::*;
use nevr_install::ffi::{NevrArtifactFetch, NevrClose, NevrInstallOpen, NevrManifestFetch, NevrSession, NEVR_OK};
use nevr_install::fetch::{fetch_capped, Failure};
use nevr_install::util::sha256_hex;
use serde_json::{json, Value};
use std::ffi::CString;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const HOST: &[u8] = b"new host NEVR-BUILD 9.9.9";
const RT: &[u8] = b"new runtime";
const GAME: &[u8] = b"the game's BugSplat64.dll";
/// The value of the base's query. No output of any run may show it.
const TOKEN: &str = "tok-9f3ab1";
const ZIP_PATH: &str = "v1.0.0/nevr-runtime-v1.0.0-windows.zip";

fn s(p: &Path) -> &str {
    p.to_str().unwrap()
}

/// The release a `file://` base serves: the manifest, its signature and the Windows zip, in a folder named
/// `base?token=<TOKEN>` under `keys`. Returns the base address and the pins file.
fn file_release(keys: &Path) -> (String, PathBuf) {
    let k = make_key(keys, "k");
    let zip_bytes = {
        let mut buf = std::io::Cursor::new(Vec::new());
        {
            let mut z = zip::ZipWriter::new(&mut buf);
            for (n, b) in [("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT)] {
                z.start_file(n, zip::write::SimpleFileOptions::default()).unwrap();
                z.write_all(b).unwrap();
            }
            z.finish().unwrap();
        }
        buf.into_inner()
    };
    let artifacts = vec![json!({
        "kind": "windows-zip", "platform": "windows-x86_64", "path": ZIP_PATH,
        "size": zip_bytes.len(), "sha256": sha256_hex(&zip_bytes),
        "contents": [
            {"path": "BugSplat64.dll", "sha256": sha256_hex(HOST), "size": HOST.len()},
            {"path": "nevr-runtime.dll", "sha256": sha256_hex(RT), "size": RT.len()}]})];
    let m: Vec<u8> = manifest_json(5, json!({"channels": {"beta": "1.0.0"}, "releases": {"1.0.0": {"artifacts": artifacts}}})).into();
    let sig: Vec<u8> = sign(keys, &k, &m, false).into();
    // The fetch reads `<folder>/<name>`, and the folder's name carries the query.
    let folder = keys.join(format!("base?token={TOKEN}"));
    std::fs::create_dir_all(folder.join("v1.0.0")).unwrap();
    std::fs::write(folder.join("manifest.json"), &m).unwrap();
    std::fs::write(folder.join("manifest.json.sig"), &sig).unwrap();
    std::fs::write(folder.join(ZIP_PATH), &zip_bytes).unwrap();
    let pins = keys.join("pins.txt");
    std::fs::write(&pins, format!("T {}\n", k.b64)).unwrap();
    (format!("file://{}", folder.display()), pins)
}

fn cli(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_nevr-install")).args(args).output().unwrap()
}

/// Every JSON line of a log, or a panic that names the missing file.
fn log_lines(path: &Path) -> Vec<Value> {
    let text = std::fs::read_to_string(path).unwrap_or_else(|e| panic!("no log at {}: {e}", path.display()));
    text.lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn record<'a>(lines: &'a [Value], name: &str) -> &'a Value {
    lines.iter().find(|r| r["event"] == name).unwrap_or_else(|| panic!("no {name} record in the log: {lines:?}"))
}

/// Nothing a run writes may show the query's value: its stdout, its stderr and its log text.
fn assert_no_token(texts: &[&str]) {
    for t in texts {
        assert!(!t.contains(TOKEN), "the base's query was written down: {t}");
    }
}

#[test]
fn a_manifest_fetched_from_a_base_with_a_query_is_logged_without_it() {
    let keys = tempfile::tempdir().unwrap();
    let (base, pins) = file_release(keys.path());
    let root = tempfile::tempdir().unwrap();
    let nevr = tempfile::tempdir().unwrap();
    let o = cli(&["manifest", "verify", "--root", s(root.path()), "--nevr-root", s(nevr.path()), "--dev-nevr-root", "--base", &base, "--pins", s(&pins), "--now", NOW]);
    let stdout = String::from_utf8_lossy(&o.stdout).to_string();
    let stderr = String::from_utf8_lossy(&o.stderr).to_string();
    assert_eq!(o.status.code(), Some(0), "{stdout} {stderr}");
    let log_path = root.path().join("logs/nevr-install.jsonl");
    let log = std::fs::read_to_string(&log_path).unwrap();
    let fetched = log_lines(&log_path);
    let m = record(&fetched, "manifest_fetch");
    assert_eq!(m["status"], 200, "the success line is the one under test: {m}");
    let url = m["url"].as_str().unwrap();
    assert!(url.starts_with("file://") && url.ends_with("/base"), "the logged address is not the redacted base: {url}");
    assert_no_token(&[&stdout, &stderr, &log]);
}

#[test]
fn an_artifact_installed_from_a_base_with_a_query_is_logged_without_it() {
    let keys = tempfile::tempdir().unwrap();
    let (base, pins) = file_release(keys.path());
    let game = tempfile::tempdir().unwrap();
    std::fs::write(game.path().join("echovr.exe"), b"exe").unwrap();
    std::fs::write(game.path().join("BugSplat64.dll"), GAME).unwrap();
    let root = tempfile::tempdir().unwrap();
    let nevr = tempfile::tempdir().unwrap();
    let o = cli(&["pc", "install", "--dir", s(game.path()), "--root", s(root.path()), "--nevr-root", s(nevr.path()), "--dev-nevr-root", "--base", &base, "--pins", s(&pins), "--now", NOW]);
    let stdout = String::from_utf8_lossy(&o.stdout).to_string();
    let stderr = String::from_utf8_lossy(&o.stderr).to_string();
    assert_eq!(o.status.code(), Some(0), "{stdout} {stderr}");
    let log = std::fs::read_to_string(root.path().join("logs/nevr-install.jsonl")).unwrap();
    let a = record(&log_lines(&root.path().join("logs/nevr-install.jsonl")), "artifact_fetch").clone();
    assert_eq!(a["ok"], true, "the success line is the one under test: {a}");
    assert_no_token(&[&stdout, &stderr, &log]);
}

#[test]
fn an_artifact_staged_from_a_base_with_a_query_is_logged_without_it() {
    let keys = tempfile::tempdir().unwrap();
    let (base, pins) = file_release(keys.path());
    let root = tempfile::tempdir().unwrap();
    let nevr = tempfile::tempdir().unwrap();
    // The updater's state folder is <nevr root>/update/manifest (ADR 0024): an earlier run stores the pair there.
    let state = nevr.path().join("update").join("manifest");
    let stored = cli(&["manifest", "verify", "--root", s(root.path()), "--state-dir", s(&state), "--base", &base, "--pins", s(&pins), "--now", NOW]);
    assert_eq!(stored.status.code(), Some(0), "{}", String::from_utf8_lossy(&stored.stdout));
    let o = cli(&["pc", "stage", "--root", s(nevr.path()), "--state-dir", s(&state), "--base", &base, "--pins", s(&pins), "--now", NOW]);
    let stdout = String::from_utf8_lossy(&o.stdout).to_string();
    let stderr = String::from_utf8_lossy(&o.stderr).to_string();
    assert_eq!(o.status.code(), Some(0), "{stdout} {stderr}");
    let log = std::fs::read_to_string(nevr.path().join("logs/nevr-update.jsonl")).unwrap();
    let a = record(&log_lines(&nevr.path().join("logs/nevr-update.jsonl")), "artifact_fetch").clone();
    assert_eq!(a["ok"], true, "the success line is the one under test: {a}");
    assert_no_token(&[&stdout, &stderr, &log]);
}

#[test]
fn an_artifact_fetched_over_the_c_abi_from_a_base_with_a_query_is_logged_without_it() {
    let keys = tempfile::tempdir().unwrap();
    let (base, pins) = file_release(keys.path());
    let pins_text = std::fs::read_to_string(&pins).unwrap();
    let root = tempfile::tempdir().unwrap();
    let log = root.path().join("ffi.jsonl");
    let cfg = CString::new(json!({"platform": "pc", "root": s(root.path()), "base": base, "pins": pins_text, "now": NOW, "log": s(&log)}).to_string()).unwrap();
    let out = CString::new(s(&root.path().join("app.zip"))).unwrap();
    let (version, kind, platform) = (CString::new("1.0.0").unwrap(), CString::new("windows-zip").unwrap(), CString::new("windows-x86_64").unwrap());
    let mut sess: *mut NevrSession = std::ptr::null_mut();
    // SAFETY: `cfg` and the other strings are NUL-terminated, `sess` is writable, and the session is used and closed on this thread.
    unsafe {
        assert_eq!(NevrInstallOpen(cfg.as_ptr(), &mut sess), NEVR_OK);
        assert_eq!(NevrManifestFetch(sess), NEVR_OK);
        assert_eq!(NevrArtifactFetch(sess, version.as_ptr(), kind.as_ptr(), platform.as_ptr(), out.as_ptr()), NEVR_OK);
        NevrClose(sess);
    }
    let text = std::fs::read_to_string(&log).unwrap();
    let a = record(&log_lines(&log), "artifact_fetch").clone();
    assert_eq!(a["ok"], true, "the success line is the one under test: {a}");
    assert_no_token(&[&text]);
}

#[test]
fn a_read_that_fails_on_a_file_base_names_the_base_without_its_query() {
    // A folder opens as a file on Linux, and reading it fails: the failure is the one a read of a file base reports.
    let d = tempfile::tempdir().unwrap();
    let folder = d.path().join(format!("file?token={TOKEN}"));
    std::fs::create_dir_all(&folder).unwrap();
    let url = format!("file://{}", folder.display());
    let mut sink: Vec<u8> = Vec::new();
    match fetch_capped(&url, 0, 1024, &mut sink) {
        Err(Failure::Interrupted(m)) => assert!(!m.contains(TOKEN), "the failed read named the query: {m}"),
        Err(_) => panic!("the read did not fail as an interrupted read"),
        Ok(n) => panic!("a folder was read as {n} bytes"),
    }
}
