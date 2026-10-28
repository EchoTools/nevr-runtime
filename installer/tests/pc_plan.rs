//! The PC plan and its execution against a temporary game folder and a temporary nevr root (ADR 0023 section 2, ADR 0012).
//! The only file the plan writes beside `echovr.exe` is `BugSplat64.dll`; the runtime, the record and every set-aside file
//! go to the root. A failure puts every file back, and says so only when it did.

use nevr_install::log::Log;
use nevr_install::pc::{self, Action, Layout, Outcome, Package, Record, Seam};
use std::fs;
use std::path::{Path, PathBuf};

const GAME: &[u8] = b"the game's own BugSplat64.dll";
const HOST_A: &[u8] = b"host A NEVR-BUILD 1.0.0";
const HOST_B: &[u8] = b"host B NEVR-BUILD 2.0.0";
const RT_A: &[u8] = b"runtime A";
const RT_B: &[u8] = b"runtime B";

fn pkg(host: &[u8], rt: &[u8]) -> Package {
    pkg_v("1.0.0", host, rt)
}

fn pkg_v(version: &str, host: &[u8], rt: &[u8]) -> Package {
    Package { host: host.to_vec(), runtime: rt.to_vec(), version: version.to_string() }
}

struct World {
    game: tempfile::TempDir,
    root: tempfile::TempDir,
    scratch: tempfile::TempDir,
}

impl World {
    fn layout(&self) -> Layout {
        Layout::new(self.game.path(), self.root.path())
    }
    fn log(&self) -> Log {
        Log::open(&self.scratch.path().join("log.jsonl"), "pc").unwrap()
    }
    fn record(&self) -> Record {
        Record::parse(&fs::read_to_string(self.layout().record()).unwrap())
    }
}

fn world() -> World {
    let w = World { game: tempfile::tempdir().unwrap(), root: tempfile::tempdir().unwrap(), scratch: tempfile::tempdir().unwrap() };
    fs::write(w.game.path().join("echovr.exe"), b"exe").unwrap();
    fs::write(w.game.path().join(pc::HOST), GAME).unwrap();
    w
}

fn plan_of(w: &World, p: &Package, allow_downgrade: bool) -> (pc::Observed, pc::Plan) {
    let obs = pc::observe(&w.layout(), false).unwrap();
    let plan = pc::plan(&obs, p, allow_downgrade);
    (obs, plan)
}

/// Plans and executes; returns (plan, end).
fn run(w: &World, p: &Package) -> (pc::Plan, pc::End) {
    run_with(w, p, &Seam::default())
}

fn run_with(w: &World, p: &Package, seam: &Seam) -> (pc::Plan, pc::End) {
    let (obs, plan) = plan_of(w, p, false);
    assert_eq!(plan.outcome, Outcome::Proceed, "{:?}", plan.reasons);
    let end = pc::execute_with(&w.layout(), &plan, &obs, p, &w.log(), seam).unwrap();
    (plan, end)
}

fn listing(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    v.sort();
    v
}

fn aside(w: &World) -> PathBuf {
    w.layout().aside_dir()
}

fn aside_named(w: &World, prefix: &str) -> PathBuf {
    let l = listing(&aside(w));
    let n = l.iter().find(|n| n.starts_with(prefix)).unwrap_or_else(|| panic!("no {prefix}* in the aside folder: {l:?}"));
    aside(w).join(n)
}

#[test]
fn the_only_file_written_beside_the_game_is_the_host_and_everything_else_goes_to_the_root() {
    let w = world();
    let (plan, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!(plan.action, Some(Action::First));
    assert_eq!((end.state, end.reason.as_str()), ("installed", ""));
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"], "no runtime, record, backup or temp file beside echovr.exe");
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), HOST_A);
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), RT_A);
    assert!(w.layout().record().is_file());
}

#[test]
fn first_install_sets_the_original_aside_in_the_root_and_records_for_uninstall() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let rec = w.record();
    for k in ["backup", "legacy", "runtime_aside", "runtime_legacy", "runtime_root", "game_dir", "release", "installed_sha256", "runtime_sha256"] {
        assert!(rec.get(k).is_some(), "missing {k}");
    }
    let backup = Path::new(rec.get("backup").unwrap());
    assert_eq!(fs::read(backup).unwrap(), GAME);
    assert!(backup.starts_with(aside(&w)) && backup.file_name().unwrap().to_string_lossy().starts_with("BugSplat64.dll.original-"), "{backup:?}");
    assert_eq!(rec.get("installed_sha256").unwrap(), nevr_install::util::sha256_hex(HOST_A));
    assert_eq!(rec.get("release"), Some("1.0.0"));
    assert_eq!(rec.get("runtime_root"), Some(w.root.path().to_str().unwrap()));
    assert_eq!(rec.get("game_dir"), Some(w.game.path().to_str().unwrap()));
}

#[test]
fn legacy_files_beside_the_game_are_moved_into_the_root_never_deleted() {
    let w = world();
    fs::write(w.game.path().join("dbgcore.dll"), b"legacy").unwrap();
    fs::write(w.game.path().join(pc::RUNTIME), b"an older runtime NEVR-BUILD 0.9.0").unwrap();
    let (_, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!(end.state, "installed", "{}", end.reason);
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"]);
    assert_eq!(fs::read(aside_named(&w, "dbgcore.dll.legacy-")).unwrap(), b"legacy");
    assert_eq!(fs::read(aside_named(&w, "nevr-runtime.dll.legacy-")).unwrap(), b"an older runtime NEVR-BUILD 0.9.0");
    let rec = w.record();
    assert!(rec.get("legacy").unwrap().contains("dbgcore.dll.legacy-"));
    assert!(rec.get("runtime_legacy").unwrap().contains("nevr-runtime.dll.legacy-"));
}

#[test]
fn a_runtime_beside_the_game_that_is_not_ours_is_left_alone() {
    let w = world();
    fs::write(w.game.path().join(pc::RUNTIME), b"somebody else's file").unwrap();
    let (_, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!(end.state, "installed");
    assert_eq!(fs::read(w.game.path().join(pc::RUNTIME)).unwrap(), b"somebody else's file");
}

#[test]
fn a_stranger_already_in_the_root_is_set_aside_not_overwritten() {
    let w = world();
    fs::write(w.root.path().join(pc::RUNTIME), b"stranger").unwrap();
    let (_, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!(end.state, "installed");
    assert_eq!(fs::read(aside_named(&w, "nevr-runtime.dll.original-")).unwrap(), b"stranger");
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), RT_A);
}

#[test]
fn an_update_replaces_a_host_this_install_put_there_and_keeps_the_first_original() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let first = w.record();
    let (plan, end) = run(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert_eq!(plan.action, Some(Action::Update));
    assert_eq!(end.state, "installed");
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), HOST_B);
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), RT_B);
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"]);
    let rec = w.record();
    assert_eq!(rec.get("backup"), first.get("backup"), "the oldest original stays");
    assert_eq!(rec.get("previous_sha256").unwrap(), nevr_install::util::sha256_hex(HOST_A));
    assert_eq!(rec.get("release"), Some("2.0.0"));
}

#[test]
fn the_same_package_twice_is_unchanged() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let (_, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    assert_eq!(plan.outcome, Outcome::Unchanged);
}

#[test]
fn a_stranger_host_with_no_record_is_not_set_aside_as_the_games_own() {
    let w = world();
    fs::write(w.game.path().join(pc::HOST), b"x NEVR-BUILD 0.1").unwrap();
    let (_, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    assert_eq!(plan.outcome, Outcome::NeedsUser);
    assert!(plan.reasons[0].contains("NEVR runtime"), "{:?}", plan.reasons);
}

#[test]
fn an_unrecorded_host_when_a_record_exists_needs_the_player() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    fs::write(w.game.path().join(pc::HOST), b"someone else's host").unwrap();
    let (_, plan) = plan_of(&w, &pkg_v("2.0.0", HOST_B, RT_B), false);
    assert_eq!(plan.outcome, Outcome::NeedsUser);
    assert!(plan.reasons[0].contains("neither one"), "{:?}", plan.reasons);
}

#[test]
fn verify_files_restoring_the_games_file_makes_the_record_stale_and_it_is_set_aside_in_the_root() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    fs::write(w.game.path().join(pc::HOST), GAME).unwrap();
    let (plan, end) = run(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert!(plan.stale_record);
    assert_eq!(plan.action, Some(Action::First));
    assert_eq!(end.state, "installed");
    assert!(fs::read_to_string(aside_named(&w, "nevr-install.txt.stale-")).unwrap().contains("installed_sha256"));
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"]);
}

#[test]
fn a_record_left_in_the_game_folder_by_an_earlier_installer_is_read_and_moved_into_the_root() {
    let w = world();
    // The layout install.ps1 wrote before the root existed: the record and the set-aside file beside the game.
    let backup = w.game.path().join("BugSplat64.dll.original-20260101-000000");
    fs::write(&backup, GAME).unwrap();
    fs::write(w.game.path().join(pc::HOST), HOST_A).unwrap();
    let text = Record::render(&pc::RecordFields {
        backup: &backup.display().to_string(),
        installed: &nevr_install::util::sha256_hex(HOST_A),
        runtime: &nevr_install::util::sha256_hex(RT_A),
        ..Default::default()
    });
    fs::write(w.game.path().join(pc::STATE_FILE), text).unwrap();
    let (plan, end) = run(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert_eq!((plan.action, end.state), (Some(Action::Update), "installed"), "{}", end.reason);
    assert!(w.layout().record().is_file());
    assert!(!w.game.path().join(pc::STATE_FILE).exists(), "the old record is moved, not copied");
    assert!(aside_named(&w, "nevr-install.txt.legacy-").is_file());
    assert_eq!(w.record().get("backup"), Some(backup.display().to_string().as_str()), "the first original stays where it was put");
}

#[test]
fn a_running_game_missing_exe_or_missing_host_stops_before_any_change() {
    let w = world();
    let obs = pc::observe(&w.layout(), true).unwrap();
    assert_eq!(pc::plan(&obs, &pkg(HOST_A, RT_A), false).outcome, Outcome::NeedsUser);
    fs::remove_file(w.game.path().join("echovr.exe")).unwrap();
    assert_eq!(plan_of(&w, &pkg(HOST_A, RT_A), false).1.outcome, Outcome::NeedsUser);
    let w2 = world();
    fs::remove_file(w2.game.path().join(pc::HOST)).unwrap();
    assert_eq!(plan_of(&w2, &pkg(HOST_A, RT_A), false).1.outcome, Outcome::NeedsUser);
}

#[test]
fn an_installed_release_newer_than_the_offered_one_is_refused_unless_allowed() {
    let w = world();
    run(&w, &pkg_v("2.1.0", HOST_B, RT_B));
    let older = pkg_v("2.0.9", HOST_A, RT_A);
    let (_, plan) = plan_of(&w, &older, false);
    assert_eq!(plan.outcome, Outcome::NeedsUser);
    assert!(plan.reasons[0].contains("2.1.0") && plan.reasons[0].contains("2.0.9") && plan.reasons[0].contains("--allow-downgrade"), "{:?}", plan.reasons);
    let (_, allowed) = plan_of(&w, &older, true);
    assert_eq!((allowed.outcome, allowed.action), (Outcome::Proceed, Some(Action::Update)));
    // A release that is newer, or the same version with other bytes, is not a downgrade.
    assert_eq!(plan_of(&w, &pkg_v("2.1.1", HOST_A, RT_A), false).1.outcome, Outcome::Proceed);
    assert_eq!(plan_of(&w, &pkg_v("2.1.0", HOST_A, RT_A), false).1.outcome, Outcome::Proceed);
    // Versions compare as numbers, not as text.
    assert_eq!(plan_of(&w, &pkg_v("2.10.0", HOST_A, RT_A), false).1.outcome, Outcome::Proceed);
}

#[test]
fn a_record_without_a_release_key_does_not_block_an_install() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let text = fs::read_to_string(w.layout().record()).unwrap().lines().filter(|l| !l.starts_with("release=")).collect::<Vec<_>>().join("\n") + "\n";
    fs::write(w.layout().record(), text).unwrap();
    assert_eq!(plan_of(&w, &pkg_v("0.0.1", HOST_B, RT_B), false).1.outcome, Outcome::Proceed);
}

#[test]
fn a_write_that_fails_after_the_runtime_returns_every_file_to_its_prior_bytes() {
    use std::os::unix::fs::PermissionsExt;
    let w = world();
    fs::write(w.root.path().join(pc::RUNTIME), b"stranger").unwrap();
    // The game folder refuses new files, so the host's temp file cannot be made; the runtime was written before it.
    fs::set_permissions(w.game.path(), fs::Permissions::from_mode(0o555)).unwrap();
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    let end = pc::execute(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log()).unwrap();
    fs::set_permissions(w.game.path(), fs::Permissions::from_mode(0o755)).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), GAME);
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), b"stranger", "the runtime that was in the root is back");
    assert!(!w.layout().record().exists(), "no record for an install that did not happen");
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"], "no temp file left behind");
    assert!(listing(&aside(&w)).iter().any(|n| n.starts_with("nevr-runtime.dll.failed-")), "the failed copy is kept");
}

#[test]
fn a_failure_after_the_files_changed_is_not_reported_as_unchanged() {
    // The record cannot be written because a directory is where its file goes: the error comes after the host, the
    // runtime and the legacy files were all changed, and every one of them is put back.
    let w = world();
    fs::write(w.game.path().join("dbgcore.dll"), b"legacy").unwrap();
    fs::write(w.game.path().join(pc::RUNTIME), b"an older runtime NEVR-BUILD 0.9.0").unwrap();
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    fs::create_dir_all(w.layout().record()).unwrap();
    let end = pc::execute(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log()).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), GAME);
    assert_eq!(fs::read(w.game.path().join("dbgcore.dll")).unwrap(), b"legacy");
    assert_eq!(fs::read(w.game.path().join(pc::RUNTIME)).unwrap(), b"an older runtime NEVR-BUILD 0.9.0");
    assert!(!w.root.path().join(pc::RUNTIME).exists());
}

#[test]
fn a_restore_that_cannot_be_done_is_reported_with_the_file_and_is_never_called_restored() {
    let w = world();
    fs::write(w.game.path().join(pc::RUNTIME), b"an older runtime NEVR-BUILD 0.9.0").unwrap();
    let seam = Seam { fail_step: Some("record_written"), fail_undo: Some("legacy_runtime") };
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    let end = pc::execute_with(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log(), &seam).unwrap();
    assert_eq!(end.state, "needs-user", "{}", end.reason);
    assert!(end.reason.contains("could not be put back") && end.reason.contains("nevr-runtime.dll"), "{}", end.reason);
    // The other files were still put back, and the moved file is where the player can find it.
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), GAME);
    assert_eq!(fs::read(aside_named(&w, "nevr-runtime.dll.legacy-")).unwrap(), b"an older runtime NEVR-BUILD 0.9.0");
    let log = fs::read_to_string(w.scratch.path().join("log.jsonl")).unwrap();
    assert!(log.contains("\"event\":\"restore_failed\""), "{log}");
}

#[test]
fn the_final_read_back_is_part_of_the_install_and_a_failure_there_restores() {
    let w = world();
    let seam = Seam { fail_step: Some("verify_installed"), fail_undo: None };
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    let end = pc::execute_with(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log(), &seam).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), GAME);
    assert!(!w.layout().record().exists());
}

#[test]
fn an_update_that_fails_puts_both_prior_files_back() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let seam = Seam { fail_step: Some("host_written"), fail_undo: None };
    let (obs, plan) = plan_of(&w, &pkg_v("2.0.0", HOST_B, RT_B), false);
    let end = pc::execute_with(&w.layout(), &plan, &obs, &pkg_v("2.0.0", HOST_B, RT_B), &w.log(), &seam).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), HOST_A);
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), RT_A);
    assert_eq!(w.record().get("release"), Some("1.0.0"), "the record still describes what is installed");
}

#[test]
fn the_install_writes_a_file_write_record_per_change() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let text = fs::read_to_string(w.scratch.path().join("log.jsonl")).unwrap();
    let writes: Vec<serde_json::Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).filter(|r: &serde_json::Value| r["event"] == "file_write").collect();
    assert!(writes.len() >= 4, "{text}");
    assert!(writes.iter().any(|w| w["sha256"] == nevr_install::util::sha256_hex(HOST_A)));
    assert!(writes.iter().any(|w| w["sha256"] == nevr_install::util::sha256_hex(RT_A)));
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
