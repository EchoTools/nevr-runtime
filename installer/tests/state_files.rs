//! The files a run writes and reads in a state directory another run may share (#1353): a run holds the state lock from
//! its read of state.json to its commit of the pair, a temporary file belongs to the writer that made it, and the stored
//! version floor never goes down.

mod common;
use common::*;
use nevr_install::artifact;
use nevr_install::manifest::{self, Artifact, Content, Pins, State};
use nevr_install::session::{Config, Session};
use nevr_install::util::{self, sha256_hex};
use std::ffi::OsString;
use std::io::Write;
use std::sync::mpsc;
use std::time::Duration;

fn zip_of(entries: &[(&str, &[u8])]) -> Vec<u8> {
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        for (name, bytes) in entries {
            z.start_file(*name, zip::write::SimpleFileOptions::default()).unwrap();
            z.write_all(bytes).unwrap();
        }
        z.finish().unwrap();
    }
    buf.into_inner()
}

/// The same zip with the entries stored, not deflated, so a damaged byte is damaged data (the zip's CRC check then fails).
fn zip_stored(entries: &[(&str, &[u8])]) -> Vec<u8> {
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        let opts = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored);
        for (name, bytes) in entries {
            z.start_file(*name, opts).unwrap();
            z.write_all(bytes).unwrap();
        }
        z.finish().unwrap();
    }
    buf.into_inner()
}

fn names_in(dir: &std::path::Path) -> Vec<OsString> {
    let mut v: Vec<OsString> = std::fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name()).collect();
    v.sort();
    v
}

fn zip_artifact(entries: &[(&str, &[u8])]) -> Artifact {
    Artifact {
        kind: "windows-zip".into(),
        platform: None,
        path: "v1.0.0/nevr-runtime-v1.0.0-windows.zip".into(),
        size: 0,
        sha256: String::new(),
        android: None,
        game_data: None,
        contents: entries.iter().map(|(p, b)| Content { path: p.to_string(), sha256: sha256_hex(b), size: b.len() as u64 }).collect(),
    }
}

#[test]
fn write_atomic_never_touches_another_writers_temp_file() {
    let d = tempfile::tempdir().unwrap();
    let target = d.path().join("state.json");
    std::fs::write(d.path().join("state.json.new"), b"another writer's temp").unwrap();
    util::write_atomic(&target, b"mine").unwrap();
    assert_eq!(std::fs::read(&target).unwrap(), b"mine");
    assert_eq!(std::fs::read(d.path().join("state.json.new")).unwrap(), b"another writer's temp");
}

#[test]
fn a_failed_write_leaves_no_temp_file_behind() {
    let d = tempfile::tempdir().unwrap();
    let target = d.path().join("state.json");
    // A directory where the file goes: the rename of the finished temp file fails.
    std::fs::create_dir(&target).unwrap();
    assert!(util::write_atomic(&target, b"x").is_err());
    let left: Vec<OsString> = std::fs::read_dir(d.path()).unwrap().map(|e| e.unwrap().file_name()).collect();
    assert_eq!(left, vec![OsString::from("state.json")], "a failed write left a temp file: {left:?}");
}

#[test]
fn extract_never_touches_another_writers_temp_file() {
    let d = tempfile::tempdir().unwrap();
    let entries: &[(&str, &[u8])] = &[("x/a.bin", b"AAA")];
    let zip = d.path().join("a.zip");
    std::fs::write(&zip, zip_of(entries)).unwrap();
    let root = d.path().join("root");
    std::fs::create_dir_all(root.join("x")).unwrap();
    std::fs::write(root.join("x/a.bin.new"), b"another writer's temp").unwrap();
    artifact::extract(&zip, &zip_artifact(entries), &root).unwrap();
    assert_eq!(std::fs::read(root.join("x/a.bin")).unwrap(), b"AAA");
    assert_eq!(std::fs::read(root.join("x/a.bin.new")).unwrap(), b"another writer's temp");
}

#[test]
fn a_damaged_entry_leaves_no_temp_file_behind() {
    let d = tempfile::tempdir().unwrap();
    let entries: &[(&str, &[u8])] = &[("x/a.bin", b"AAAAAAAA")];
    let mut bytes = zip_stored(entries);
    // The stored bytes are damaged; the central directory still holds the entry's CRC, so reading the entry fails.
    let at = bytes.windows(8).position(|w| w == b"AAAAAAAA").unwrap();
    bytes[at] = b'B';
    let zip = d.path().join("a.zip");
    std::fs::write(&zip, bytes).unwrap();
    let root = d.path().join("root");
    assert!(artifact::extract(&zip, &zip_artifact(entries), &root).is_err(), "a damaged entry was extracted");
    assert_eq!(names_in(&root.join("x")), Vec::<OsString>::new(), "a failed extraction left a file behind");
}

#[test]
fn a_mismatched_entry_leaves_no_temp_file_behind() {
    let d = tempfile::tempdir().unwrap();
    let entries: &[(&str, &[u8])] = &[("x/a.bin", b"AAA")];
    let zip = d.path().join("a.zip");
    std::fs::write(&zip, zip_stored(entries)).unwrap();
    let mut art = zip_artifact(entries);
    art.contents[0].sha256 = "0".repeat(64);
    let root = d.path().join("root");
    assert!(artifact::extract(&zip, &art, &root).is_err());
    assert_eq!(names_in(&root.join("x")), Vec::<OsString>::new(), "a mismatched entry left a temp file behind");
}

#[test]
fn a_rename_that_fails_leaves_no_temp_file_behind() {
    let d = tempfile::tempdir().unwrap();
    let entries: &[(&str, &[u8])] = &[("x/a.bin", b"AAA")];
    let zip = d.path().join("a.zip");
    std::fs::write(&zip, zip_stored(entries)).unwrap();
    let root = d.path().join("root");
    // A folder where the file goes: the rename of the finished temp file fails.
    std::fs::create_dir_all(root.join("x/a.bin")).unwrap();
    assert!(artifact::extract(&zip, &zip_artifact(entries), &root).is_err());
    assert_eq!(names_in(&root.join("x")), vec![OsString::from("a.bin")], "a failed rename left a temp file behind");
}

#[test]
fn stored_reads_the_pair_under_the_state_lock() {
    let d = tempfile::tempdir().unwrap();
    let state = d.path().join("state");
    std::fs::create_dir_all(&state).unwrap();
    let holder = std::fs::OpenOptions::new().create(true).write(true).truncate(false).open(state.join("state.lock")).unwrap();
    holder.lock().unwrap();
    let (tx, rx) = mpsc::channel();
    let dir = state.clone();
    std::thread::spawn(move || {
        let r = manifest::stored(&dir).map(|_| ()).map_err(|e| e.msg);
        let _ = tx.send(r);
    });
    assert!(rx.recv_timeout(Duration::from_millis(800)).is_err(), "stored() read the pair while another run held the state lock");
    drop(holder);
    // Nothing is stored in this folder: the error is the missing pair, reported after the lock was taken.
    assert!(rx.recv_timeout(Duration::from_secs(60)).unwrap().is_err());
}

#[test]
fn commit_never_lowers_the_stored_version_floor() {
    let (m, s) = fixture("good");
    let v = manifest::verify(&m, &s, &test_pins(), &State::default(), NOW, 1).unwrap();
    assert_eq!(v.version, 1);
    let d = tempfile::tempdir().unwrap();
    // Another run committed version 9 after this run read the state, and revoked a key.
    let stored = serde_json::json!({"highest_manifest_version": 9, "revoked_keys": ["aaaa1111bbbb2222"], "last_manifest_sha256": "0".repeat(64)});
    std::fs::write(d.path().join("state.json"), stored.to_string()).unwrap();
    manifest::commit(d.path(), &v).unwrap();
    let after = manifest::read_state(d.path()).unwrap();
    assert_eq!(after.highest, 9, "a commit of version 1 lowered the stored floor of 9");
    assert_eq!(after.last_sha256, "0".repeat(64), "a commit of version 1 replaced the stored pair's digest");
    assert_eq!(after.revoked, vec!["aaaa1111bbbb2222".to_string()], "a commit dropped a stored revocation");
    assert!(!d.path().join("manifest.json").exists(), "an older manifest replaced the stored pair");
}

#[test]
fn a_run_waits_for_the_state_lock_before_it_reads_the_state() {
    let d = tempfile::tempdir().unwrap();
    let k = make_key(d.path(), "k");
    let pair = d.path().join("pair");
    std::fs::create_dir_all(&pair).unwrap();
    let m = manifest_json(5, serde_json::json!({}));
    std::fs::write(pair.join("manifest.json"), &m).unwrap();
    std::fs::write(pair.join("manifest.json.sig"), sign(d.path(), &k, &m, false)).unwrap();
    let root = d.path().join("root");
    let state = root.join("state");
    std::fs::create_dir_all(&state).unwrap();
    // Another run holds the state lock: the lock file the installer takes in the state directory.
    let holder = std::fs::OpenOptions::new().create(true).write(true).truncate(false).open(state.join("state.lock")).unwrap();
    holder.lock().unwrap();
    let mut cfg = Config::new("pc", &root);
    cfg.pins = Pins::parse(&format!("T {}\n", k.b64)).unwrap();
    cfg.now = Some(NOW.into());
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let r = Session::open(cfg, "test").and_then(|s| s.manifest_from_dir(&pair));
        let _ = tx.send(r.map(|v| v.version).map_err(|e| e.to_string()));
    });
    assert!(rx.recv_timeout(Duration::from_millis(800)).is_err(), "the run finished while another run held the state lock");
    drop(holder);
    assert_eq!(rx.recv_timeout(Duration::from_secs(60)).unwrap(), Ok(5));
}
