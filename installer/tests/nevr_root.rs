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

#[test]
fn a_root_that_differs_only_in_case_is_the_canonical_one_only_where_the_platform_ignores_case() {
    let d = tempfile::tempdir().unwrap();
    let canonical = d.path().join("Nevr").join("EchoVR");
    let spelled = d.path().join("nevr").join("echovr");
    let s = open(d.path(), &canonical, false);
    let got = s.check_nevr_root(&spelled);
    assert_eq!(got.is_ok(), cfg!(windows), "{:?}", got.err().map(|e| e.msg));
}
