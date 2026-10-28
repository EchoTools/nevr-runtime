//! The PC plan and its execution against a temporary game folder (ADR 0023 section 2). The folder is the only thing
//! written; the state file keeps the keys tools/package-release/uninstall.ps1 reads.

use nevr_install::log::Log;
use nevr_install::pc::{self, Action, Outcome, Package, Record};
use std::fs;
use std::path::Path;

const GAME: &[u8] = b"the game's own BugSplat64.dll";
const HOST_A: &[u8] = b"host A NEVR-BUILD 1.0.0";
const HOST_B: &[u8] = b"host B NEVR-BUILD 2.0.0";
const RT_A: &[u8] = b"runtime A";
const RT_B: &[u8] = b"runtime B";

fn pkg(host: &[u8], rt: &[u8]) -> Package {
    Package { host: host.to_vec(), runtime: rt.to_vec() }
}

fn folder() -> tempfile::TempDir {
    let d = tempfile::tempdir().unwrap();
    fs::write(d.path().join("echovr.exe"), b"exe").unwrap();
    fs::write(d.path().join(pc::HOST), GAME).unwrap();
    d
}

fn log(d: &Path) -> Log {
    Log::open(&d.join("log.jsonl"), "pc").unwrap()
}

/// Plans and executes; returns (plan, end).
fn run(dir: &Path, p: &Package, scratch: &Path) -> (pc::Plan, pc::End) {
    let obs = pc::observe(dir, false).unwrap();
    let plan = pc::plan(&obs, p);
    assert_eq!(plan.outcome, Outcome::Proceed, "{:?}", plan.reasons);
    let end = pc::execute(dir, &plan, &obs, p, &log(scratch)).unwrap();
    (plan, end)
}

fn listing(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    v.sort();
    v
}

#[test]
fn first_install_sets_the_original_aside_writes_the_runtime_and_records_for_uninstall() {
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    let (plan, end) = run(d.path(), &pkg(HOST_A, RT_A), s.path());
    assert_eq!(plan.action, Some(Action::First));
    assert_eq!((end.state, end.reason.as_str()), ("installed", ""));
    assert_eq!(fs::read(d.path().join(pc::HOST)).unwrap(), HOST_A);
    assert_eq!(fs::read(d.path().join(pc::RUNTIME)).unwrap(), RT_A);
    let rec = Record::parse(&fs::read_to_string(d.path().join(pc::STATE_FILE)).unwrap());
    // The keys uninstall.ps1 reads.
    for k in ["backup", "legacy", "runtime_aside", "installed_sha256", "runtime_sha256"] {
        assert!(rec.get(k).is_some(), "missing {k}");
    }
    assert_eq!(fs::read(rec.get("backup").unwrap()).unwrap(), GAME);
    assert!(rec.get("backup").unwrap().contains("BugSplat64.dll.original-"));
    assert_eq!(rec.get("installed_sha256").unwrap(), nevr_install::util::sha256_hex(HOST_A));
}

#[test]
fn a_legacy_dbgcore_and_an_earlier_runtime_are_set_aside_never_deleted() {
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    fs::write(d.path().join("dbgcore.dll"), b"legacy").unwrap();
    fs::write(d.path().join(pc::RUNTIME), b"stranger").unwrap();
    let (_, end) = run(d.path(), &pkg(HOST_A, RT_A), s.path());
    assert_eq!(end.state, "installed");
    let l = listing(d.path());
    assert!(!l.contains(&"dbgcore.dll".to_string()));
    let legacy = l.iter().find(|n| n.starts_with("dbgcore.dll.legacy-")).expect("legacy set aside");
    assert_eq!(fs::read(d.path().join(legacy)).unwrap(), b"legacy");
    let aside = l.iter().find(|n| n.starts_with("nevr-runtime.dll.original-")).expect("runtime set aside");
    assert_eq!(fs::read(d.path().join(aside)).unwrap(), b"stranger");
}

#[test]
fn an_update_replaces_a_host_this_install_put_there_and_keeps_the_first_original() {
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    run(d.path(), &pkg(HOST_A, RT_A), s.path());
    let first = Record::parse(&fs::read_to_string(d.path().join(pc::STATE_FILE)).unwrap());
    let (plan, end) = run(d.path(), &pkg(HOST_B, RT_B), s.path());
    assert_eq!(plan.action, Some(Action::Update));
    assert_eq!(end.state, "installed");
    assert_eq!(fs::read(d.path().join(pc::HOST)).unwrap(), HOST_B);
    assert_eq!(fs::read(d.path().join(pc::RUNTIME)).unwrap(), RT_B);
    let rec = Record::parse(&fs::read_to_string(d.path().join(pc::STATE_FILE)).unwrap());
    assert_eq!(rec.get("backup"), first.get("backup"), "the oldest original stays");
    assert_eq!(rec.get("previous_sha256").unwrap(), nevr_install::util::sha256_hex(HOST_A));
}

#[test]
fn the_same_package_twice_is_unchanged() {
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    run(d.path(), &pkg(HOST_A, RT_A), s.path());
    let plan = pc::plan(&pc::observe(d.path(), false).unwrap(), &pkg(HOST_A, RT_A));
    assert_eq!(plan.outcome, Outcome::Unchanged);
}

#[test]
fn a_stranger_host_with_no_record_is_not_set_aside_as_the_games_own() {
    let d = folder();
    fs::write(d.path().join(pc::HOST), b"x NEVR-BUILD 0.1").unwrap();
    let plan = pc::plan(&pc::observe(d.path(), false).unwrap(), &pkg(HOST_A, RT_A));
    assert_eq!(plan.outcome, Outcome::NeedsUser);
    assert!(plan.reasons[0].contains("NEVR runtime"), "{:?}", plan.reasons);
}

#[test]
fn an_unrecorded_host_when_a_record_exists_needs_the_player() {
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    run(d.path(), &pkg(HOST_A, RT_A), s.path());
    fs::write(d.path().join(pc::HOST), b"someone else's host").unwrap();
    let plan = pc::plan(&pc::observe(d.path(), false).unwrap(), &pkg(HOST_B, RT_B));
    assert_eq!(plan.outcome, Outcome::NeedsUser);
    assert!(plan.reasons[0].contains("neither one"), "{:?}", plan.reasons);
}

#[test]
fn verify_files_restoring_the_games_file_makes_the_record_stale_and_it_is_set_aside_not_deleted() {
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    run(d.path(), &pkg(HOST_A, RT_A), s.path());
    fs::write(d.path().join(pc::HOST), GAME).unwrap();
    let (plan, end) = run(d.path(), &pkg(HOST_B, RT_B), s.path());
    assert!(plan.stale_record);
    assert_eq!(plan.action, Some(Action::First));
    assert_eq!(end.state, "installed");
    assert!(listing(d.path()).iter().any(|n| n.starts_with("nevr-install.txt.stale-")));
}

#[test]
fn a_running_game_missing_exe_or_missing_host_stops_before_any_change() {
    let d = folder();
    let obs = pc::observe(d.path(), true).unwrap();
    assert_eq!(pc::plan(&obs, &pkg(HOST_A, RT_A)).outcome, Outcome::NeedsUser);
    fs::remove_file(d.path().join("echovr.exe")).unwrap();
    assert_eq!(pc::plan(&pc::observe(d.path(), false).unwrap(), &pkg(HOST_A, RT_A)).outcome, Outcome::NeedsUser);
    let d2 = folder();
    fs::remove_file(d2.path().join(pc::HOST)).unwrap();
    assert_eq!(pc::plan(&pc::observe(d2.path(), false).unwrap(), &pkg(HOST_A, RT_A)).outcome, Outcome::NeedsUser);
}

#[test]
fn a_write_that_fails_after_the_runtime_returns_the_folder_to_its_prior_bytes() {
    use std::os::unix::fs::PermissionsExt;
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    let mut perm = fs::metadata(d.path().join(pc::HOST)).unwrap().permissions();
    perm.set_mode(0o444);
    fs::set_permissions(d.path().join(pc::HOST), perm).unwrap();
    let before_host = fs::read(d.path().join(pc::HOST)).unwrap();
    let obs = pc::observe(d.path(), false).unwrap();
    let plan = pc::plan(&obs, &pkg(HOST_A, RT_A));
    let end = pc::execute(d.path(), &plan, &obs, &pkg(HOST_A, RT_A), &log(s.path())).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(d.path().join(pc::HOST)).unwrap(), before_host);
    assert!(!d.path().join(pc::RUNTIME).exists(), "the runtime written before the host is moved out of the way");
    assert!(!d.path().join(pc::STATE_FILE).exists(), "no record for an install that did not happen");
}

#[test]
fn the_install_writes_a_file_write_record_per_change() {
    let (d, s) = (folder(), tempfile::tempdir().unwrap());
    run(d.path(), &pkg(HOST_A, RT_A), s.path());
    let text = fs::read_to_string(s.path().join("log.jsonl")).unwrap();
    let writes: Vec<serde_json::Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).filter(|r: &serde_json::Value| r["event"] == "file_write").collect();
    assert!(writes.len() >= 3, "{text}");
    assert!(writes.iter().any(|w| w["sha256"] == nevr_install::util::sha256_hex(HOST_A)));
}

#[test]
fn launch_witness_reads_the_hosts_own_records() {
    let (h, r) = ("hh", "rr");
    let good = format!("{{\"event\":\"versions\",\"host_sha256\":\"{h}\",\"runtime_sha256\":\"{r}\"}}\n{{\"event\":\"runtime_load\",\"verify\":\"accepted\"}}\n");
    assert!(pc::launch_witness(&good, h, r).is_ok());
    assert!(pc::launch_witness("", h, r).is_err());
    assert!(pc::launch_witness(&good, "other", r).unwrap_err().contains("other files"));
    let refused = good.replace("accepted", "refused");
    assert!(pc::launch_witness(&refused, h, r).unwrap_err().contains("not accepted"));
    let stopped = format!("{good}{{\"event\":\"game_stop\"}}\n");
    assert!(pc::launch_witness(&stopped, h, r).unwrap_err().contains("stopped"));
}
