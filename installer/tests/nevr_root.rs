//! The nevr root a PC run acts on is the folder the host loads from (#1353). Any other root is refused unless the run is a
//! dev or test run (`--dev-nevr-root`), and the log says why in either case. The canonical folder comes from the config
//! here, so the accepted case runs on every platform; the CLI reads it from %LOCALAPPDATA% on Windows.

use nevr_install::session::{Config, Session};
use serde_json::Value;
use std::path::{Path, PathBuf};

/// The canonical folder (`<tmp>/nevr/echovr`) and a second folder that is not it.
fn folders(d: &Path) -> (PathBuf, PathBuf) {
    (d.join("nevr").join("echovr"), d.join("elsewhere"))
}

fn open(d: &Path, canonical: &Path, dev: bool) -> Session {
    let mut cfg = Config::new("pc", &d.join("root"));
    cfg.canonical_nevr_root = Some(canonical.to_path_buf());
    cfg.dev_nevr_root = dev;
    Session::open(cfg, "test").unwrap()
}

/// The `nevr_root` records of the run, oldest first.
fn nevr_root_records(d: &Path) -> Vec<Value> {
    std::fs::read_to_string(d.join("root/logs/nevr-install.jsonl"))
        .unwrap()
        .lines()
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|r| r["event"] == "nevr_root")
        .collect()
}

#[test]
fn the_canonical_root_is_accepted_and_the_acceptance_is_logged() {
    let d = tempfile::tempdir().unwrap();
    let (canonical, _) = folders(d.path());
    let s = open(d.path(), &canonical, false);
    s.check_nevr_root(&canonical).unwrap();
    s.check_nevr_root(&canonical.join(".")).unwrap();
    let recs = nevr_root_records(d.path());
    assert_eq!(recs.len(), 2, "{recs:?}");
    assert_eq!((recs[0]["level"].as_str(), recs[0]["ok"].as_bool(), recs[0]["canonical"].as_bool()), (Some("INFO"), Some(true), Some(true)));
}

#[test]
fn another_root_is_refused_and_the_log_names_the_reason() {
    let d = tempfile::tempdir().unwrap();
    let (canonical, other) = folders(d.path());
    let s = open(d.path(), &canonical, false);
    let err = s.check_nevr_root(&other).unwrap_err();
    assert!(err.msg.contains("--dev-nevr-root") && err.msg.contains(&canonical.display().to_string()), "{}", err.msg);
    let recs = nevr_root_records(d.path());
    let last = recs.last().expect("the refusal is logged");
    assert_eq!((last["level"].as_str(), last["ok"].as_bool()), (Some("ERROR"), Some(false)), "{last}");
    assert!(last["reason"].as_str().unwrap_or("").contains("the folder the game loads nEVR from"), "{last}");
}

#[test]
fn a_dev_run_may_name_another_root_and_its_log_says_so() {
    let d = tempfile::tempdir().unwrap();
    let (canonical, other) = folders(d.path());
    let s = open(d.path(), &canonical, true);
    s.check_nevr_root(&other).unwrap();
    let recs = nevr_root_records(d.path());
    let last = recs.last().expect("the dev run is logged");
    assert_eq!((last["level"].as_str(), last["ok"].as_bool(), last["dev"].as_bool()), (Some("WARN"), Some(true), Some(true)), "{last}");
    assert_eq!(last["canonical"], false);
}

#[test]
fn without_a_canonical_folder_every_root_is_refused_outside_a_dev_run() {
    let d = tempfile::tempdir().unwrap();
    let (_, other) = folders(d.path());
    let mut cfg = Config::new("pc", &d.path().join("root"));
    cfg.canonical_nevr_root = None;
    let s = Session::open(cfg, "test").unwrap();
    let err = s.check_nevr_root(&other).unwrap_err();
    assert!(err.msg.contains("--dev-nevr-root"), "{}", err.msg);
}

/// The C ABI's stage (`NevrPcStage`) gets the root rule of the CLI's `pc stage` (#1383): refused on a root the host does not load
/// from, unless the config says `dev_nevr_root`. Both calls stop before the manifest is needed, so the dev call's log is the proof
/// that the check passed.
#[test]
fn the_c_stage_refuses_another_root_unless_the_config_says_dev() {
    use nevr_install::ffi::{NevrClose, NevrInstallOpen, NevrLastError, NevrPcStage, NevrSession, NEVR_OK};
    use std::ffi::{c_char, CStr, CString};
    let d = tempfile::tempdir().unwrap();
    let other = d.path().join("elsewhere");
    let root = CString::new(other.to_str().unwrap()).unwrap();
    let stage_log = other.join("logs/nevr-update.jsonl");
    let open = |dev: bool| {
        let cfg = CString::new(serde_json::json!({"platform": "pc", "nevr_root": other, "dev_nevr_root": dev}).to_string()).unwrap();
        let mut sess: *mut NevrSession = std::ptr::null_mut();
        assert_eq!(unsafe { NevrInstallOpen(cfg.as_ptr(), &mut sess) }, NEVR_OK);
        sess
    };
    let stage = |sess: *mut NevrSession| {
        let mut out: *mut c_char = std::ptr::null_mut();
        let code = unsafe { NevrPcStage(sess, root.as_ptr(), std::ptr::null(), &mut out) };
        let msg = unsafe { CStr::from_ptr(NevrLastError(sess)) }.to_string_lossy().into_owned();
        (code, msg)
    };

    let refused = open(false);
    let (code, msg) = stage(refused);
    unsafe { NevrClose(refused) };
    assert_ne!(code, NEVR_OK, "{msg}");
    assert!(msg.contains("--dev-nevr-root"), "{msg}");
    assert!(!other.join("update").exists(), "a refused stage makes no folder under the root");
    let refusal: Value = std::fs::read_to_string(&stage_log).unwrap().lines().map(|l| serde_json::from_str::<Value>(l).unwrap()).rfind(|r| r["event"] == "nevr_root").expect("the refusal is logged");
    assert_eq!((refusal["level"].as_str(), refusal["ok"].as_bool()), (Some("ERROR"), Some(false)), "{refusal}");

    let dev = open(true);
    let (_, msg) = stage(dev);
    unsafe { NevrClose(dev) };
    assert!(!msg.contains("--dev-nevr-root"), "a dev config passes the root check: {msg}");
    let passed: Value = std::fs::read_to_string(&stage_log).unwrap().lines().map(|l| serde_json::from_str::<Value>(l).unwrap()).rfind(|r| r["event"] == "nevr_root").expect("the dev run is logged");
    assert_eq!((passed["level"].as_str(), passed["ok"].as_bool(), passed["dev"].as_bool()), (Some("WARN"), Some(true), Some(true)), "{passed}");
}

#[test]
fn a_root_that_differs_only_in_case_is_the_canonical_one_only_where_the_platform_ignores_case() {
    let d = tempfile::tempdir().unwrap();
    let canonical = d.path().join("Nevr").join("EchoVR");
    let spelled = d.path().join("nevr").join("echovr");
    let s = open(d.path(), &canonical, false);
    let got = s.check_nevr_root(&spelled);
    assert_eq!(got.is_ok(), cfg!(windows), "{:?}", got.err().map(|e| e.msg));
}
