//! `install-nevr-runtime pc stage` (ADR 0024, section 3): the real binary, one call each, against a local server that serves a
//! manifest signed by a throwaway key. The runtime and the plugin members of the channel's `windows-zip` are staged under
//! `<root>/update/staging/<version>`; nothing is published after a failed check.

mod common;
use common::*;
use nevr_install::util::sha256_hex;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::Command;

const HOST: &[u8] = b"host BugSplat64 NEVR-BUILD 1.0.0";
const RT: &[u8] = b"runtime NEVR-BUILD 1.0.0 with some more bytes in it";
const README: &[u8] = b"read me";
const PLUGIN: &[u8] = b"a plugin dll";

type Members<'a> = Vec<(&'a str, &'a [u8])>;

/// Answers each request from `routes`; a path in `cut` announces its full length and sends only that many bytes.
fn serve(routes: HashMap<String, Vec<u8>>, cut: Option<(String, usize)>) -> String {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
    std::thread::spawn(move || {
        for c in l.incoming() {
            let Ok(mut c) = c else { continue };
            let mut buf = [0u8; 4096];
            let k = c.read(&mut buf).unwrap_or(0);
            let req = String::from_utf8_lossy(&buf[..k]).to_string();
            let path = req.split_whitespace().nth(1).unwrap_or("/").to_string();
            match routes.get(&path) {
                Some(body) => {
                    let mut r = format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).into_bytes();
                    let n = match &cut {
                        Some((p, n)) if *p == path => (*n).min(body.len()),
                        _ => body.len(),
                    };
                    r.extend_from_slice(&body[..n]);
                    let _ = c.write_all(&r);
                }
                None => {
                    let _ = c.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                }
            }
        }
    });
    base
}

fn zip_of(members: &Members) -> Vec<u8> {
    let mut buf = std::io::Cursor::new(Vec::new());
    let mut z = zip::ZipWriter::new(&mut buf);
    for (n, b) in members {
        z.start_file(*n, zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(b).unwrap();
    }
    z.finish().unwrap();
    buf.into_inner()
}

struct Opts<'a> {
    version: &'a str,
    manifest_version: u64,
    members: Members<'a>,
    /// What `contents` lists; defaults to every member with its real bytes.
    listed: Option<Members<'a>>,
    zip_sha_wrong: bool,
    cut: Option<usize>,
    with_zip_artifact: bool,
}

impl<'a> Opts<'a> {
    fn new() -> Opts<'a> {
        Opts {
            version: "1.0.0",
            manifest_version: 5,
            members: vec![("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT), ("README.txt", README)],
            listed: None,
            zip_sha_wrong: false,
            cut: None,
            with_zip_artifact: true,
        }
    }
}

struct World {
    keys: tempfile::TempDir,
    key: Key,
    pins: PathBuf,
    root: tempfile::TempDir,
}

fn world() -> World {
    let keys = tempfile::tempdir().unwrap();
    let key = make_key(keys.path(), "k");
    let pins = keys.path().join("pins.txt");
    std::fs::write(&pins, format!("T {}\n", key.b64)).unwrap();
    World { keys, key, pins, root: tempfile::tempdir().unwrap() }
}

/// Serves one signed manifest with one release, and returns its base URL.
fn publish(w: &World, o: &Opts) -> String {
    let zip = zip_of(&o.members);
    let listed = o.listed.clone().unwrap_or_else(|| o.members.clone());
    let contents: Vec<Value> = listed.iter().map(|(n, b)| json!({"path": n, "sha256": sha256_hex(b), "size": b.len()})).collect();
    let zip_path = format!("v{0}/nevr-runtime-v{0}-windows.zip", o.version);
    let art = if o.with_zip_artifact {
        json!({"kind": "windows-zip", "platform": "windows-x86_64", "path": zip_path, "size": zip.len(),
               "sha256": if o.zip_sha_wrong { "0".repeat(64) } else { sha256_hex(&zip) }, "contents": contents})
    } else {
        json!({"kind": "quest-apk", "platform": "android-arm64", "path": "v1/x.apk", "size": 10, "sha256": "2".repeat(64),
               "android": {"package": "com.readyatdawn.r15", "version_code": 5, "signer_cert_sha256": "a".repeat(64)}})
    };
    let m = manifest_json(o.manifest_version, json!({"channels": {"beta": o.version}, "releases": {o.version: {"artifacts": [art]}}}));
    let sig = sign(w.keys.path(), &w.key, &m, false);
    let routes = HashMap::from([
        ("/manifest.json".to_string(), m),
        ("/manifest.json.sig".to_string(), sig),
        (format!("/{zip_path}"), zip),
    ]);
    serve(routes, o.cut.map(|n| (format!("/{zip_path}"), n)))
}

fn run(w: &World, base: &str, current: &Value, extra: &[&str]) -> (i32, Value, String) {
    run_at(w, base, current, extra, NOW)
}

fn run_at(w: &World, base: &str, current: &Value, extra: &[&str], now: &str) -> (i32, Value, String) {
    let o = Command::new(env!("CARGO_BIN_EXE_install-nevr-runtime"))
        .args(["pc", "stage", "--root", w.root.path().to_str().unwrap(), "--base", base, "--pins", w.pins.to_str().unwrap(), "--now", now])
        .args(["--current-json", &current.to_string()])
        .args(extra)
        .output()
        .unwrap();
    let out: Value = serde_json::from_slice(&o.stdout).unwrap_or_else(|e| panic!("stdout is not JSON ({e}): {}", String::from_utf8_lossy(&o.stdout)));
    (o.status.code().unwrap(), out, String::from_utf8_lossy(&o.stderr).to_string())
}

fn current(version: &str, runtime: &[u8]) -> Value {
    json!({"version": version, "runtime_sha256": sha256_hex(runtime), "quarantine": []})
}

fn log(w: &World) -> Vec<Value> {
    std::fs::read_to_string(w.root.path().join("logs/nevr-update.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn events(w: &World) -> Vec<String> {
    log(w).iter().map(|r| r["event"].as_str().unwrap().to_string()).collect()
}

fn staging(w: &World) -> PathBuf {
    w.root.path().join("update/staging")
}

fn staging_is_empty(w: &World) -> bool {
    !staging(w).exists() || std::fs::read_dir(staging(w)).unwrap().next().is_none()
}

#[test]
fn the_channels_release_is_staged_with_the_runtime_and_nothing_else() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let (code, out, err) = run(&w, &base, &current("0.9.0", b"old runtime"), &[]);
    assert_eq!(code, 0, "{out} {err}");
    let r = &out["plan"];
    assert_eq!((r["action"].as_str(), r["version"].as_str(), r["direction"].as_str()), (Some("staged"), Some("1.0.0"), Some("upgrade")), "{out}");
    assert_eq!(r["files"], json!([{"path": "nevr-runtime.dll", "size": RT.len(), "sha256": sha256_hex(RT)}]));
    let dir = staging(&w).join("1.0.0");
    assert_eq!(std::fs::read(dir.join("nevr-runtime.dll")).unwrap(), RT);
    let work: Vec<_> = std::fs::read_dir(w.root.path().join("update/work")).map(|d| d.map(|e| e.unwrap().file_name()).collect()).unwrap_or_default();
    assert!(work.is_empty(), "the downloaded zip is removed once staged: {work:?}");
    assert!(!dir.join("BugSplat64.dll").exists(), "the host is never staged");
    assert!(!dir.join("README.txt").exists(), "only the runtime and plugin members are extracted");
    let names: Vec<String> = std::fs::read_dir(staging(&w)).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().to_string()).collect();
    assert_eq!(names, vec!["1.0.0".to_string()], "no partial folder is left");
    assert!(w.root.path().join("update/manifest/manifest.json").exists(), "the updater keeps its own manifest state");
    let ev = events(&w);
    for want in ["run_start", "manifest_check", "release_selected", "artifact_fetch", "stage_member", "stage_end", "run_end"] {
        assert!(ev.contains(&want.to_string()), "missing {want} in {ev:?}");
    }
    let member = log(&w).into_iter().find(|r| r["event"] == "stage_member").unwrap();
    assert_eq!((member["path"].as_str(), member["ok"].as_bool()), (Some("nevr-runtime.dll"), Some(true)));
    assert_eq!(log(&w).last().unwrap()["event"], "run_end");
}

#[test]
fn plugin_members_are_staged_with_the_runtime() {
    let w = world();
    let mut o = Opts::new();
    o.members.push(("plugins/example.dll", PLUGIN));
    let base = publish(&w, &o);
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"x"), &[]);
    assert_eq!(code, 0, "{out}");
    let paths: Vec<&str> = out["plan"]["files"].as_array().unwrap().iter().map(|f| f["path"].as_str().unwrap()).collect();
    assert_eq!(paths, vec!["nevr-runtime.dll", "plugins/example.dll"]);
    assert_eq!(std::fs::read(staging(&w).join("1.0.0/plugins/example.dll")).unwrap(), PLUGIN);
}

#[test]
fn a_release_equal_to_the_running_one_fetches_nothing() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run(&w, &base, &current("1.0.0", RT), &[]);
    assert_eq!(code, 0, "{out}");
    assert_eq!((out["plan"]["action"].as_str(), out["plan"]["reason"].as_str()), (Some("none"), Some("current")));
    assert!(!events(&w).contains(&"artifact_fetch".to_string()), "no download for the release already running");
    assert!(!staging(&w).exists());
}

#[test]
fn the_same_version_with_other_bytes_is_replaced() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run(&w, &base, &current("1.0.0", b"a rebuilt runtime"), &[]);
    assert_eq!(code, 0, "{out}");
    assert_eq!((out["plan"]["action"].as_str(), out["plan"]["direction"].as_str()), (Some("staged"), Some("replace")));
}

#[test]
fn a_release_the_channel_withdrew_to_is_followed_downward_and_the_log_says_so() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run(&w, &base, &current("2.0.0", b"newer"), &[]);
    assert_eq!(code, 0, "{out}");
    assert_eq!((out["plan"]["action"].as_str(), out["plan"]["direction"].as_str()), (Some("staged"), Some("downgrade")));
    let end = log(&w).into_iter().find(|r| r["event"] == "stage_end").unwrap();
    assert_eq!(end["direction"], "downgrade");
}

#[test]
fn a_quarantined_release_is_not_staged_again() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let mut cur = current("0.9.0", b"old");
    cur["quarantine"] = json!([{"version": "1.0.0", "runtime_sha256": sha256_hex(RT)}]);
    let (code, out, _) = run(&w, &base, &cur, &[]);
    assert_eq!(code, 0, "{out}");
    assert_eq!((out["plan"]["action"].as_str(), out["plan"]["reason"].as_str()), (Some("none"), Some("quarantined")));
    assert!(!events(&w).contains(&"artifact_fetch".to_string()));
    assert!(!staging(&w).exists());
}

#[test]
fn a_quarantine_entry_for_other_bytes_does_not_hold_back_a_rebuilt_release() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let mut cur = current("0.9.0", b"old");
    cur["quarantine"] = json!([{"version": "1.0.0", "runtime_sha256": sha256_hex(b"other bytes")}]);
    let (code, out, _) = run(&w, &base, &cur, &[]);
    assert_eq!((code, out["plan"]["action"].as_str()), (0, Some("staged")), "{out}");
}

#[test]
fn a_download_cut_off_at_any_size_publishes_nothing_and_the_next_run_completes() {
    let zip_len = zip_of(&Opts::new().members).len();
    for cut in [0, 1, zip_len - 1] {
        let w = world();
        let mut o = Opts::new();
        o.cut = Some(cut);
        let base = publish(&w, &o);
        let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
        assert_eq!(code, 1, "cut at {cut}: {out}");
        assert_eq!(out["state"], "unchanged");
        assert!(staging_is_empty(&w), "cut at {cut}: nothing is staged");
        let fetch = log(&w).into_iter().find(|r| r["event"] == "artifact_fetch").unwrap_or_else(|| panic!("cut at {cut}: no artifact_fetch record: {:?}", events(&w)));
        assert_eq!(fetch["ok"], false, "cut at {cut}: {fetch}");
        assert!(fetch["reason"].as_str().unwrap().len() > 3);
        // The next launch, with a healthy server, completes.
        let base2 = publish(&w, &Opts::new());
        let (code, out, _) = run(&w, &base2, &current("0.9.0", b"old"), &[]);
        assert_eq!((code, out["plan"]["action"].as_str()), (0, Some("staged")), "after cut at {cut}: {out}");
    }
}

#[test]
fn a_wrong_zip_hash_is_refused_and_nothing_is_kept() {
    let w = world();
    let mut o = Opts::new();
    o.zip_sha_wrong = true;
    let base = publish(&w, &o);
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("sha256"), "{out}");
    assert!(staging_is_empty(&w));
    assert!(!w.root.path().join("update/work/nevr-runtime-v1.0.0-windows.zip").exists());
    let fetch = log(&w).into_iter().find(|r| r["event"] == "artifact_fetch").unwrap();
    assert_eq!(fetch["ok"], false);
}

#[test]
fn a_member_whose_bytes_differ_from_contents_is_refused() {
    let w = world();
    let mut o = Opts::new();
    o.listed = Some(vec![("BugSplat64.dll", HOST), ("nevr-runtime.dll", b"not the runtime in the zip"), ("README.txt", README)]);
    let base = publish(&w, &o);
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("nevr-runtime.dll"), "{out}");
    assert!(staging_is_empty(&w));
}

#[test]
fn a_member_the_manifest_does_not_list_is_refused() {
    let w = world();
    let mut o = Opts::new();
    o.listed = Some(vec![("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT)]);
    let base = publish(&w, &o);
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("README.txt"), "{out}");
    assert!(staging_is_empty(&w));
}

#[test]
fn a_member_name_that_leaves_the_folder_is_refused_and_writes_nothing_outside() {
    for evil in ["plugins/../escape.dll", "../escape.dll", "plugins\\x.dll", "C:evil.dll", "/abs.dll"] {
        let w = world();
        let mut o = Opts::new();
        o.members.push((evil, b"evil"));
        let base = publish(&w, &o);
        let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
        assert_eq!(code, 1, "{evil}: {out}");
        assert!(out["reason"].as_str().unwrap().contains("safe relative path"), "{evil}: {out}");
        assert!(staging_is_empty(&w), "{evil}");
        assert!(!w.root.path().join("escape.dll").exists() && !w.root.path().join("update/escape.dll").exists(), "{evil}");
    }
}

#[test]
fn a_manifest_older_than_the_stored_floor_is_refused() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 0, "{out}");
    let mut older = Opts::new();
    older.manifest_version = 4;
    let base2 = publish(&w, &older);
    let (code, out, _) = run(&w, &base2, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().starts_with("FAIL step 4"), "{out}");
}

#[test]
fn an_expired_manifest_is_refused() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run_at(&w, &base, &current("0.9.0", b"old"), &[], "2026-12-01T00:00:00Z");
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().starts_with("FAIL step 5"), "{out}");
    assert!(staging_is_empty(&w));
}

#[test]
fn an_unreachable_server_changes_nothing_and_says_so() {
    let w = world();
    let (code, out, _) = run(&w, "http://127.0.0.1:1", &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().starts_with("FAIL step 1"), "{out}");
    assert!(staging_is_empty(&w));
    let last = log(&w).pop().unwrap();
    assert_eq!((last["event"].as_str(), last["failed"].as_bool()), (Some("run_end"), Some(true)));
}

#[test]
fn a_release_without_a_windows_zip_is_blocked_not_failed() {
    let w = world();
    let mut o = Opts::new();
    o.with_zip_artifact = false;
    let base = publish(&w, &o);
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 0, "{out}");
    assert_eq!((out["plan"]["action"].as_str(), out["plan"]["reason"].as_str()), (Some("blocked"), Some("no_windows_zip")));
    assert!(staging_is_empty(&w));
}

#[test]
fn a_windows_zip_without_a_runtime_is_blocked() {
    let w = world();
    let mut o = Opts::new();
    o.members = vec![("BugSplat64.dll", HOST), ("README.txt", README)];
    let base = publish(&w, &o);
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 0, "{out}");
    assert_eq!((out["plan"]["action"].as_str(), out["plan"]["reason"].as_str()), (Some("blocked"), Some("no_runtime_member")));
    assert!(!events(&w).contains(&"artifact_fetch".to_string()), "nothing to stage, nothing fetched");
}

#[test]
fn only_plain_plugin_names_are_staged() {
    let w = world();
    let long = format!("plugins/{}.dll", "a".repeat(65));
    let mut o = Opts::new();
    o.members = vec![("nevr-runtime.dll", RT), ("plugins/ok-1_a.dll", PLUGIN), ("plugins/notes.txt", PLUGIN), ("plugins/sub/x.dll", PLUGIN), ("plugins/.dll", PLUGIN), (long.as_str(), PLUGIN)];
    let base = publish(&w, &o);
    let (code, out, err) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 0, "{out} {err}");
    let paths: Vec<&str> = out["plan"]["files"].as_array().unwrap().iter().map(|f| f["path"].as_str().unwrap()).collect();
    assert_eq!(paths, vec!["nevr-runtime.dll", "plugins/ok-1_a.dll"], "{out}");
}

#[test]
fn a_staging_folder_that_cannot_be_made_fails_with_nothing_published() {
    let w = world();
    std::fs::create_dir_all(w.root.path().join("update")).unwrap();
    std::fs::write(w.root.path().join("update/staging"), b"a file where the folder goes").unwrap();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 1, "{out}");
    assert!(out["reason"].as_str().unwrap().contains("staging"), "{out}");
    let end = log(&w).into_iter().find(|r| r["event"] == "stage_end").unwrap();
    assert_eq!(end["ok"], false);
}

#[test]
fn old_partial_folders_and_unpublished_staging_are_removed_and_a_fresh_partial_is_kept() {
    let w = world();
    let s = staging(&w);
    for d in ["9.9.9.partial-1", "0.5.0", "7.7.7.partial-2"] {
        std::fs::create_dir_all(s.join(d)).unwrap();
        std::fs::write(s.join(d).join("f"), b"x").unwrap();
    }
    let old = std::time::SystemTime::now() - std::time::Duration::from_secs(3600);
    std::fs::File::open(s.join("9.9.9.partial-1")).unwrap().set_modified(old).unwrap();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 0, "{out}");
    assert!(!s.join("9.9.9.partial-1").exists(), "a partial folder untouched for an hour is removed");
    assert!(!s.join("0.5.0").exists(), "a staging folder no staged.json points at is removed");
    assert!(s.join("7.7.7.partial-2").exists(), "a fresh partial folder may belong to a stage that is running");
    assert!(s.join("1.0.0/nevr-runtime.dll").exists());
}

#[test]
fn a_release_already_staged_and_intact_is_not_fetched_again_and_a_damaged_one_is() {
    let w = world();
    let base = publish(&w, &Opts::new());
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!(code, 0, "{out}");
    // The runtime publishes staged.json after its own checks.
    std::fs::write(w.root.path().join("update/staged.json"), json!({"version": "1.0.0"}).to_string()).unwrap();
    let before = events(&w).iter().filter(|e| *e == "artifact_fetch").count();
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!((code, out["plan"]["action"].as_str(), out["plan"]["reason"].as_str()), (0, Some("staged"), Some("already_staged")), "{out}");
    assert_eq!(events(&w).iter().filter(|e| *e == "artifact_fetch").count(), before, "no second download");
    // A damaged staged file is fetched and staged again.
    std::fs::write(staging(&w).join("1.0.0/nevr-runtime.dll"), b"damaged").unwrap();
    let (code, out, _) = run(&w, &base, &current("0.9.0", b"old"), &[]);
    assert_eq!((code, out["plan"]["action"].as_str()), (0, Some("staged")), "{out}");
    assert_eq!(std::fs::read(staging(&w).join("1.0.0/nevr-runtime.dll")).unwrap(), RT);
    assert_eq!(events(&w).iter().filter(|e| *e == "artifact_fetch").count(), before + 1);
}

#[test]
fn stage_needs_a_root_and_writes_nothing_without_one() {
    let o = Command::new(env!("CARGO_BIN_EXE_install-nevr-runtime")).args(["pc", "stage", "--current-json", "{}"]).output().unwrap();
    assert_eq!(o.status.code(), Some(1));
}

#[allow(dead_code)]
fn _unused(_: &Path) {}

#[test]
fn a_c_program_stages_through_nevr_pc_stage_and_ends_the_run_once() {
    // The static library the C program links (a `cargo test` run builds only the test profile's rlib).
    let manifest = format!("{}/Cargo.toml", env!("CARGO_MANIFEST_DIR"));
    let b = Command::new(env!("CARGO")).args(["build", "--lib", "--manifest-path", &manifest]).output().unwrap();
    assert!(b.status.success(), "{}", String::from_utf8_lossy(&b.stderr));
    let exe = std::env::current_exe().unwrap();
    let lib = exe.parent().unwrap().parent().unwrap().join("libnevr_install.a");
    assert!(lib.exists(), "{}", lib.display());

    let w = world();
    let base = publish(&w, &Opts::new());
    let bin = w.keys.path().join("stage_check");
    let cc = Command::new("cc")
        .arg(concat!(env!("CARGO_MANIFEST_DIR"), "/tests/c/stage_check.c"))
        .arg("-I").arg(concat!(env!("CARGO_MANIFEST_DIR"), "/include"))
        .arg(&lib).args(["-lpthread", "-ldl", "-lm", "-Wall", "-Werror", "-o"]).arg(&bin)
        .output().expect("cc is installed");
    assert!(cc.status.success(), "{}", String::from_utf8_lossy(&cc.stderr));

    let cfg = json!({"platform": "pc", "nevr_root": w.root.path(), "base": base, "pins": format!("T {}\n", w.key.b64), "now": NOW}).to_string();
    let o = Command::new(&bin).arg(&cfg).arg(w.root.path()).arg(current("0.9.0", b"old runtime").to_string()).output().unwrap();
    let (out, err) = (String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr));
    assert!(o.status.success(), "stdout: {out}\nstderr: {err}");
    let report: Value = serde_json::from_str(out.lines().find_map(|l| l.strip_prefix("stage=")).unwrap()).unwrap();
    assert_eq!((report["action"].as_str(), report["version"].as_str()), (Some("staged"), Some("1.0.0")), "{report}");
    assert_eq!(std::fs::read(staging(&w).join("1.0.0/nevr-runtime.dll")).unwrap(), RT);
    let ev = events(&w);
    assert_eq!(ev.iter().filter(|e| *e == "run_end").count(), 1, "the caller ends the run, the stage does not: {ev:?}");
    assert!(ev.contains(&"stage_end".to_string()) && ev.contains(&"stage_member".to_string()), "{ev:?}");
}
