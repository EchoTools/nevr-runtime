//! `NevrStage` (ADR 0024 section 3, Quest self-update design piece core-stage-profile): the stage takes the artifact kind and
//! the members from a profile. The Quest profile stages a single-file `quest-runtime` artifact (no zip) and keeps the stored
//! manifest pair beside it in `proof/`; the PC profile does what `NevrPcStage` does and writes no `proof/`.
//! The real C ABI functions, against a local server that serves a manifest signed by a throwaway key.

mod common;
use common::*;
use nevr_install::ffi::{NevrClose, NevrInstallOpen, NevrLastError, NevrManifestFetch, NevrPcDiscard, NevrPcStage, NevrSession, NevrStage, NevrStringFree, NEVR_BAD_ARGUMENT, NEVR_FAILED, NEVR_OK};
use nevr_install::util::sha256_hex;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::ffi::{c_char, CStr, CString};
use std::io::Write;
use std::path::{Path, PathBuf};

const QUEST_PROFILE: &str = r#"{"kind":"quest-runtime","members":["libnevr_runtime.so"]}"#;
const PC_PROFILE: &str = r#"{"kind":"windows-zip","members":["nevr-runtime.dll","plugins/*.dll"]}"#;
const MEMBER: &str = "libnevr_runtime.so";
/// The most members a profile may list; one more is refused.
const MAX_MEMBERS: usize = 16;
const SO: &[u8] = b"\x7fELF a runtime shared object with some bytes in it";
const SO_PATH: &str = "v1.0.0/nevr-runtime-v1.0.0-quest-runtime.so";
const ZIP_PATH: &str = "v1.0.0/nevr-runtime-v1.0.0-windows.zip";
const HOST: &[u8] = b"host BugSplat64 NEVR-BUILD 1.0.0";
const DLL: &[u8] = b"runtime NEVR-BUILD 1.0.0 with some more bytes in it";
const PLUGIN: &[u8] = b"a plugin dll";

struct World {
    keys: tempfile::TempDir,
    key: Key,
    root: tempfile::TempDir,
    manifest: Vec<u8>,
    sig: Vec<u8>,
}

fn world() -> World {
    let keys = tempfile::tempdir().unwrap();
    let key = make_key(keys.path(), "k");
    World { keys, key, root: tempfile::tempdir().unwrap(), manifest: Vec::new(), sig: Vec::new() }
}

fn quest_artifact(listed: &[u8]) -> Value {
    json!({"kind": "quest-runtime", "platform": "android-arm64", "path": SO_PATH, "size": listed.len(), "sha256": sha256_hex(listed)})
}

fn zip_artifact() -> (Value, Vec<u8>) {
    let members: [(&str, &[u8]); 3] = [("BugSplat64.dll", HOST), ("nevr-runtime.dll", DLL), ("plugins/example.dll", PLUGIN)];
    let mut buf = std::io::Cursor::new(Vec::new());
    let mut z = zip::ZipWriter::new(&mut buf);
    for (n, b) in members {
        z.start_file(n, zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(b).unwrap();
    }
    z.finish().unwrap();
    let zip = buf.into_inner();
    let contents: Vec<Value> = members.iter().map(|(n, b)| json!({"path": n, "sha256": sha256_hex(b), "size": b.len()})).collect();
    (json!({"kind": "windows-zip", "platform": "windows-x86_64", "path": ZIP_PATH, "size": zip.len(), "sha256": sha256_hex(&zip), "contents": contents}), zip)
}

/// Serves a signed manifest whose release 1.0.0 lists `artifacts`; `files` are the bodies served at their paths.
fn publish(w: &mut World, artifacts: Vec<Value>, files: Vec<(&str, Vec<u8>)>, cut: Option<(&str, usize)>) -> String {
    w.manifest = manifest_json(5, json!({"channels": {"beta": "1.0.0"}, "releases": {"1.0.0": {"artifacts": artifacts}}}));
    w.sig = sign(w.keys.path(), &w.key, &w.manifest, false);
    let mut routes = HashMap::from([("/manifest.json".to_string(), w.manifest.clone()), ("/manifest.json.sig".to_string(), w.sig.clone())]);
    for (p, b) in files {
        routes.insert(format!("/{p}"), b);
    }
    serve(routes, cut.map(|(p, n)| (format!("/{p}"), n)))
}

fn publish_quest(w: &mut World, listed: &[u8], served: &[u8], cut: Option<usize>) -> String {
    publish(w, vec![quest_artifact(listed)], vec![(SO_PATH, served.to_vec())], cut.map(|n| (SO_PATH, n)))
}

fn open(w: &World, base: &str, platform: &str, root: &Path, dev_root: bool) -> *mut NevrSession {
    let cfg = json!({"platform": platform, "nevr_root": root, "dev_nevr_root": dev_root, "base": base, "pins": format!("T {}\n", w.key.b64), "now": NOW}).to_string();
    let c = CString::new(cfg).unwrap();
    let mut s: *mut NevrSession = std::ptr::null_mut();
    assert_eq!(unsafe { NevrInstallOpen(c.as_ptr(), &mut s) }, NEVR_OK);
    s
}

fn last_error(s: *mut NevrSession) -> String {
    unsafe { CStr::from_ptr(NevrLastError(s)) }.to_string_lossy().to_string()
}

/// One call of `NevrStage` on a fresh session whose manifest was fetched first; returns the status, the report and the last error.
fn stage_at(w: &World, base: &str, platform: &str, root: &Path, dev_root: bool, profile: Option<&str>, current: Option<&Value>) -> (i32, Value, String) {
    let s = open(w, base, platform, root, dev_root);
    assert_eq!(unsafe { NevrManifestFetch(s) }, NEVR_OK, "{}", last_error(s));
    let r = call(s, root, profile, current);
    unsafe { NevrClose(s) };
    r
}

fn call(s: *mut NevrSession, root: &Path, profile: Option<&str>, current: Option<&Value>) -> (i32, Value, String) {
    let (root_c, cur_c) = (CString::new(root.to_str().unwrap()).unwrap(), current.map(|c| CString::new(c.to_string()).unwrap()));
    let prof_c = profile.map(|p| CString::new(p).unwrap());
    let mut out: *mut c_char = std::ptr::null_mut();
    let rc = unsafe { NevrStage(s, root_c.as_ptr(), prof_c.as_ref().map_or(std::ptr::null(), |p| p.as_ptr()), cur_c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()), &mut out) };
    finish(s, rc, out)
}

fn pc_call(s: *mut NevrSession, root: &Path, current: &Value) -> (i32, Value, String) {
    let (root_c, cur_c) = (CString::new(root.to_str().unwrap()).unwrap(), CString::new(current.to_string()).unwrap());
    let mut out: *mut c_char = std::ptr::null_mut();
    let rc = unsafe { NevrPcStage(s, root_c.as_ptr(), cur_c.as_ptr(), &mut out) };
    finish(s, rc, out)
}

fn finish(s: *mut NevrSession, rc: i32, out: *mut c_char) -> (i32, Value, String) {
    let report = if out.is_null() {
        Value::Null
    } else {
        let v = serde_json::from_str(&unsafe { CStr::from_ptr(out) }.to_string_lossy()).unwrap();
        unsafe { NevrStringFree(out) };
        v
    };
    (rc, report, last_error(s))
}

fn quest(w: &World, base: &str, profile: &str, current: &Value) -> (i32, Value, String) {
    stage_at(w, base, "quest", w.root.path(), false, Some(profile), Some(current))
}

fn current(version: &str, runtime: &[u8]) -> Value {
    json!({"version": version, "runtime_sha256": sha256_hex(runtime), "quarantine": []})
}

fn log(root: &Path) -> Vec<Value> {
    std::fs::read_to_string(root.join("logs/nevr-update.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn count(root: &Path, event: &str) -> usize {
    log(root).iter().filter(|r| r["event"] == event).count()
}

fn staging(root: &Path) -> PathBuf {
    root.join("update/staging")
}

fn staging_is_empty(root: &Path) -> bool {
    !staging(root).exists() || std::fs::read_dir(staging(root)).unwrap().next().is_none()
}

fn tree(dir: &Path) -> Vec<String> {
    fn walk(base: &Path, dir: &Path, out: &mut Vec<String>) {
        for e in std::fs::read_dir(dir).unwrap() {
            let p = e.unwrap().path();
            let rel = p.strip_prefix(base).unwrap().to_string_lossy().to_string();
            out.push(rel);
            if p.is_dir() {
                walk(base, &p, out);
            }
        }
    }
    let mut out = Vec::new();
    walk(dir, dir, &mut out);
    out.sort();
    out
}

#[test]
fn a_quest_runtime_is_staged_as_one_file_with_the_stored_manifest_pair_beside_it() {
    let mut w = world();
    let base = publish_quest(&mut w, SO, SO, None);
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &current("0.9.0", b"old"));
    assert_eq!(rc, NEVR_OK, "{err}");
    assert_eq!((r["action"].as_str(), r["version"].as_str(), r["direction"].as_str()), (Some("staged"), Some("1.0.0"), Some("upgrade")), "{r}");
    assert_eq!(r["files"], json!([{"path": MEMBER, "size": SO.len(), "sha256": sha256_hex(SO)}]));
    let dir = staging(w.root.path()).join("1.0.0");
    assert_eq!(std::fs::read(dir.join(MEMBER)).unwrap(), SO);
    assert_eq!(std::fs::read(dir.join("proof/manifest.json")).unwrap(), w.manifest, "the pair that passed step 2 is kept beside the file");
    assert_eq!(std::fs::read(dir.join("proof/manifest.json.sig")).unwrap(), w.sig);
    assert_eq!(tree(&staging(w.root.path())), vec!["1.0.0", "1.0.0/libnevr_runtime.so", "1.0.0/proof", "1.0.0/proof/manifest.json", "1.0.0/proof/manifest.json.sig"], "no partial folder, no other file");
    let work: Vec<_> = std::fs::read_dir(w.root.path().join("update/work")).map(|d| d.map(|e| e.unwrap().file_name()).collect()).unwrap_or_default();
    assert!(work.is_empty(), "the download is removed once staged: {work:?}");
    let fetch = log(w.root.path()).into_iter().find(|r| r["event"] == "artifact_fetch").unwrap();
    assert_eq!((fetch["kind"].as_str(), fetch["ok"].as_bool()), (Some("quest-runtime"), Some(true)), "{fetch}");
    let member = log(w.root.path()).into_iter().find(|r| r["event"] == "stage_member").unwrap();
    assert_eq!((member["path"].as_str(), member["ok"].as_bool()), (Some(MEMBER), Some(true)), "{member}");
}

#[test]
fn a_flipped_byte_in_the_served_file_is_refused_and_nothing_is_staged() {
    let mut w = world();
    let mut flipped = SO.to_vec();
    flipped[10] ^= 0x01;
    let base = publish_quest(&mut w, SO, &flipped, None);
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &current("0.9.0", b"old"));
    assert_eq!((rc, &r), (NEVR_FAILED, &Value::Null), "{err}");
    assert!(err.contains("sha256"), "{err}");
    assert!(staging_is_empty(w.root.path()));
    assert!(!w.root.path().join("update/work/nevr-runtime-v1.0.0-quest-runtime.so").exists());
    let fetch = log(w.root.path()).into_iter().find(|r| r["event"] == "artifact_fetch").unwrap();
    assert_eq!((fetch["kind"].as_str(), fetch["ok"].as_bool()), (Some("quest-runtime"), Some(false)), "{fetch}");
}

#[test]
fn a_body_over_the_manifests_size_is_refused_and_nothing_is_staged() {
    let mut w = world();
    let mut longer = SO.to_vec();
    longer.extend_from_slice(b"trailing bytes the manifest does not list");
    let base = publish_quest(&mut w, SO, &longer, None);
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &current("0.9.0", b"old"));
    assert_eq!((rc, &r), (NEVR_FAILED, &Value::Null), "{err}");
    assert!(staging_is_empty(w.root.path()));
    assert_eq!(count(w.root.path(), "artifact_fetch"), 1);
    assert!(!w.root.path().join("update/work/nevr-runtime-v1.0.0-quest-runtime.so").exists());
}

#[test]
fn a_body_cut_short_stages_nothing_and_the_next_run_completes() {
    for cut in [0, 1, SO.len() - 1] {
        let mut w = world();
        let base = publish_quest(&mut w, SO, SO, Some(cut));
        let (rc, _, err) = quest(&w, &base, QUEST_PROFILE, &current("0.9.0", b"old"));
        assert_eq!(rc, NEVR_FAILED, "cut at {cut}: {err}");
        assert!(staging_is_empty(w.root.path()), "cut at {cut}");
        let base2 = publish_quest(&mut w, SO, SO, None);
        let (rc, r, err) = quest(&w, &base2, QUEST_PROFILE, &current("0.9.0", b"old"));
        assert_eq!((rc, r["action"].as_str()), (NEVR_OK, Some("staged")), "after cut at {cut}: {err}");
    }
}

#[test]
fn a_kind_the_core_does_not_stage_is_refused_before_anything_is_fetched() {
    for kind in ["plugin", "content-pack", "quest-apk", "quest-library", "game-data", "", "Windows-Zip"] {
        let mut w = world();
        let base = publish_quest(&mut w, SO, SO, None);
        let profile = json!({"kind": kind, "members": [MEMBER]}).to_string();
        let (rc, r, err) = quest(&w, &base, &profile, &current("0.9.0", b"old"));
        assert_eq!((rc, &r), (NEVR_FAILED, &Value::Null), "kind {kind:?}: {err}");
        assert!(err.contains("kind"), "kind {kind:?}: {err}");
        assert_eq!(count(w.root.path(), "artifact_fetch"), 0, "kind {kind:?}: nothing is fetched for a kind the core does not stage");
        assert!(!w.root.path().join("update/staging").exists(), "kind {kind:?}");
    }
}

#[test]
fn a_release_that_lists_no_artifact_of_the_kind_is_blocked_not_failed() {
    let mut w = world();
    let (zip_art, zip) = zip_artifact();
    let base = publish(&mut w, vec![zip_art], vec![(ZIP_PATH, zip)], None);
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &current("0.9.0", b"old"));
    assert_eq!(rc, NEVR_OK, "{err}");
    assert_eq!((r["action"].as_str(), r["reason"].as_str(), r["files"].clone()), (Some("blocked"), Some("no_quest_runtime"), json!([])), "{r}");
    assert_eq!(count(w.root.path(), "artifact_fetch"), 0);
    assert!(staging_is_empty(w.root.path()));
}

#[test]
fn a_profile_that_is_not_well_formed_is_refused_and_writes_nothing() {
    let bad = [
        "not json".to_string(),
        "[]".to_string(),
        json!({"members": [MEMBER]}).to_string(),
        json!({"kind": "quest-runtime"}).to_string(),
        json!({"kind": "quest-runtime", "members": []}).to_string(),
        json!({"kind": "quest-runtime", "members": [MEMBER, "other.so"]}).to_string(),
        json!({"kind": "quest-runtime", "members": ["../libnevr_runtime.so"]}).to_string(),
        json!({"kind": "quest-runtime", "members": ["lib/libnevr_runtime.so"]}).to_string(),
        json!({"kind": "quest-runtime", "members": ["*.so"]}).to_string(),
        json!({"kind": "quest-runtime", "members": [7]}).to_string(),
        json!({"kind": "quest-runtime", "members": [MEMBER], "extra": true}).to_string(),
        json!({"kind": "windows-zip", "members": ["plugins/*.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "plugins/*/x.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "../*.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "./*.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "*/x.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "plugins/*dll"]}).to_string(),
        json!({"kind": "quest-runtime", "members": ["libnevr runtime.so"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "nevr-runtime.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "../evil.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "/evil.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "a//b.dll"]}).to_string(),
        json!({"kind": "windows-zip", "members": (0..=MAX_MEMBERS).map(|i| format!("m{i}.dll")).collect::<Vec<_>>()}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "plugins/*."]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "plugins/*.d-ll"]}).to_string(),
        json!({"kind": "windows-zip", "members": ["nevr-runtime.dll", "plugins/*.tar.gz"]}).to_string(),
        json!({"kind": "quest-runtime", "members": [MEMBER, "plugins/*.so"]}).to_string(),
    ];
    let wildcard_for_a_single_file = json!({"kind": "quest-runtime", "members": [MEMBER, "plugins/*.so"]}).to_string();
    for profile in bad {
        let mut w = world();
        let base = publish_quest(&mut w, SO, SO, None);
        let (rc, r, err) = quest(&w, &base, &profile, &current("0.9.0", b"old"));
        assert_eq!((rc, &r), (NEVR_FAILED, &Value::Null), "{profile}: {err}");
        assert!(err.contains("profile"), "{profile}: {err}");
        if profile == wildcard_for_a_single_file {
            assert!(err.contains("single-file kind lists one plain file name"), "{profile}: {err}");
        }
        assert_eq!(count(w.root.path(), "artifact_fetch"), 0, "{profile}");
        assert!(!w.root.path().join("update/staging").exists(), "{profile}");
    }
}

#[test]
fn nevr_stage_needs_a_session_a_root_and_a_profile() {
    let mut w = world();
    let base = publish_quest(&mut w, SO, SO, None);
    let (root, prof, cur) = (CString::new(w.root.path().to_str().unwrap()).unwrap(), CString::new(QUEST_PROFILE).unwrap(), CString::new("{}").unwrap());
    let mut out: *mut c_char = std::ptr::null_mut();
    assert_eq!(unsafe { NevrStage(std::ptr::null_mut(), root.as_ptr(), prof.as_ptr(), cur.as_ptr(), &mut out) }, NEVR_BAD_ARGUMENT);
    let s = open(&w, &base, "quest", w.root.path(), false);
    assert_eq!(unsafe { NevrStage(s, std::ptr::null(), prof.as_ptr(), cur.as_ptr(), &mut out) }, NEVR_BAD_ARGUMENT);
    assert_eq!(unsafe { NevrStage(s, root.as_ptr(), std::ptr::null(), cur.as_ptr(), &mut out) }, NEVR_BAD_ARGUMENT, "a stage with no profile has no kind to take");
    assert!(out.is_null());
    unsafe { NevrClose(s) };
}

#[test]
fn the_running_quest_runtime_and_a_quarantined_or_discarded_one_fetch_nothing() {
    let mut w = world();
    let base = publish_quest(&mut w, SO, SO, None);
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &current("1.0.0", SO));
    assert_eq!((rc, r["action"].as_str(), r["reason"].as_str()), (NEVR_OK, Some("none"), Some("current")), "{err}");
    let mut cur = current("0.9.0", b"old");
    cur["quarantine"] = json!([{"version": "1.0.0", "runtime_sha256": sha256_hex(SO)}]);
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &cur);
    assert_eq!((rc, r["action"].as_str(), r["reason"].as_str()), (NEVR_OK, Some("none"), Some("quarantined")), "{err}");
    let (v, h, why, root) = (CString::new("1.0.0").unwrap(), CString::new(sha256_hex(SO)).unwrap(), CString::new("verify_failed").unwrap(), CString::new(w.root.path().to_str().unwrap()).unwrap());
    let s = open(&w, &base, "quest", w.root.path(), false);
    assert_eq!(unsafe { NevrPcDiscard(s, root.as_ptr(), v.as_ptr(), h.as_ptr(), why.as_ptr()) }, NEVR_OK, "{}", last_error(s));
    unsafe { NevrClose(s) };
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &current("0.9.0", b"old"));
    assert_eq!((rc, r["action"].as_str(), r["reason"].as_str()), (NEVR_OK, Some("none"), Some("discarded")), "{err}");
    assert_eq!(count(w.root.path(), "artifact_fetch"), 0);
    assert!(!staging(w.root.path()).exists());
}

#[test]
fn a_quest_runtime_already_staged_with_its_proof_is_not_fetched_again_and_a_damaged_one_is() {
    let mut w = world();
    let base = publish_quest(&mut w, SO, SO, None);
    let cur = current("0.9.0", b"old");
    let (rc, _, err) = quest(&w, &base, QUEST_PROFILE, &cur);
    assert_eq!(rc, NEVR_OK, "{err}");
    // The runtime publishes staged.json after its own checks.
    std::fs::write(w.root.path().join("update/staged.json"), json!({"version": "1.0.0"}).to_string()).unwrap();
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &cur);
    assert_eq!((rc, r["action"].as_str(), r["reason"].as_str()), (NEVR_OK, Some("staged"), Some("already_staged")), "{err}");
    assert_eq!(count(w.root.path(), "artifact_fetch"), 1, "no second download");
    // A changed proof file is not the stored pair: the release is fetched and staged again.
    let sig = staging(w.root.path()).join("1.0.0/proof/manifest.json.sig");
    std::fs::write(&sig, b"not the signature").unwrap();
    let (rc, r, err) = quest(&w, &base, QUEST_PROFILE, &cur);
    assert_eq!((rc, r["action"].as_str(), r["reason"].as_str()), (NEVR_OK, Some("staged"), Some("staged")), "{err}");
    assert_eq!(std::fs::read(&sig).unwrap(), w.sig);
    assert_eq!(count(w.root.path(), "artifact_fetch"), 2);
    // So is a missing proof folder, and so is a damaged runtime.
    std::fs::remove_dir_all(staging(w.root.path()).join("1.0.0/proof")).unwrap();
    std::fs::write(w.root.path().join("update/staged.json"), json!({"version": "1.0.0"}).to_string()).unwrap();
    let (rc, _, err) = quest(&w, &base, QUEST_PROFILE, &cur);
    assert_eq!(rc, NEVR_OK, "{err}");
    assert_eq!(count(w.root.path(), "artifact_fetch"), 3);
    assert!(staging(w.root.path()).join("1.0.0/proof/manifest.json").exists());
    std::fs::write(w.root.path().join("update/staged.json"), json!({"version": "1.0.0"}).to_string()).unwrap();
    std::fs::write(staging(w.root.path()).join("1.0.0").join(MEMBER), b"damaged").unwrap();
    let (rc, _, err) = quest(&w, &base, QUEST_PROFILE, &cur);
    assert_eq!(rc, NEVR_OK, "{err}");
    assert_eq!(std::fs::read(staging(w.root.path()).join("1.0.0").join(MEMBER)).unwrap(), SO);
    assert_eq!(count(w.root.path(), "artifact_fetch"), 4);
}

#[test]
fn the_pc_profile_stages_what_nevr_pc_stage_stages_and_keeps_no_proof() {
    let mut w = world();
    let (zip_art, zip) = zip_artifact();
    let base = publish(&mut w, vec![zip_art], vec![(ZIP_PATH, zip)], None);
    let cur = current("0.9.0", b"old");
    let (root_a, root_b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let sa = open(&w, &base, "pc", root_a.path(), true);
    assert_eq!(unsafe { NevrManifestFetch(sa) }, NEVR_OK);
    let (rc_a, report_a, err) = pc_call(sa, root_a.path(), &cur);
    assert_eq!(rc_a, NEVR_OK, "{err}");
    unsafe { NevrClose(sa) };
    let (rc_b, report_b, err) = stage_at(&w, &base, "pc", root_b.path(), true, Some(PC_PROFILE), Some(&cur));
    assert_eq!(rc_b, NEVR_OK, "{err}");
    assert_eq!(report_a, report_b, "the PC profile is what NevrPcStage uses");
    let paths: Vec<&str> = report_b["files"].as_array().unwrap().iter().map(|f| f["path"].as_str().unwrap()).collect();
    assert_eq!(paths, vec!["nevr-runtime.dll", "plugins/example.dll"], "{report_b}");
    assert_eq!(tree(&staging(root_a.path())), tree(&staging(root_b.path())));
    assert!(!tree(&staging(root_b.path())).iter().any(|p| p.contains("proof")), "a zip stage keeps no proof pair: the Authenticode signature is its anchor");
}

#[test]
fn a_pc_session_is_held_to_the_root_the_host_loads_from_and_a_quest_session_to_an_absolute_one() {
    let mut w = world();
    let base = publish_quest(&mut w, SO, SO, None);
    let (rc, r, err) = stage_at(&w, &base, "pc", w.root.path(), false, Some(QUEST_PROFILE), Some(&current("0.9.0", b"old")));
    assert_eq!((rc, &r), (NEVR_FAILED, &Value::Null), "{err}");
    assert!(err.contains("--dev-nevr-root"), "{err}");
    assert!(!w.root.path().join("update/staging").exists());
    assert_eq!(count(w.root.path(), "artifact_fetch"), 0);
    // A Quest session takes the app's private folder as the caller computed it; a relative one is not that.
    let s = open(&w, &base, "quest", w.root.path(), false);
    assert_eq!(unsafe { NevrManifestFetch(s) }, NEVR_OK);
    let (rc, r, err) = call(s, Path::new("update-root"), Some(QUEST_PROFILE), None);
    assert_eq!((rc, &r), (NEVR_FAILED, &Value::Null), "{err}");
    assert!(err.contains("absolute"), "{err}");
    unsafe { NevrClose(s) };
}
