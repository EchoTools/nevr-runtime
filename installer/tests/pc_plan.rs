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
fn the_install_makes_the_roots_plugins_folder_empty_and_touches_nothing_in_it() {
    let w = world();
    let (_, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!(end.state, "installed");
    assert!(w.layout().plugins().is_dir(), "the folder the runtime loads plugins from exists after an install");
    assert!(listing(&w.layout().plugins()).is_empty(), "and holds nothing: the install ships no plugin");
    fs::write(w.layout().plugins().join("mine.dll"), b"a plugin the player put there").unwrap();
    let (_, end) = run(&w, &pkg(HOST_B, RT_B));
    assert_eq!(end.state, "installed");
    assert_eq!(listing(&w.layout().plugins()), vec!["mine.dll"], "an update leaves what is in the folder where it is");
    assert_eq!(fs::read(w.layout().plugins().join("mine.dll")).unwrap(), b"a plugin the player put there");
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"], "no plugins folder beside echovr.exe");
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
    assert_eq!(rec.get("runtime_aside"), Some(""), "an install that set no runtime aside records the empty name");
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
    assert_eq!(w.record().get("runtime_aside"), Some(aside_named(&w, "nevr-runtime.dll.original-").to_str().unwrap()), "uninstall finds the file by this name");
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

const OTHER_NEVR_HOST: &[u8] = b"x NEVR-BUILD 0.1";

/// A NEVR host in the game folder and no record anywhere (the root was lost): the install adopts it.
fn lost_root_world() -> World {
    let w = world();
    fs::write(w.game.path().join(pc::HOST), OTHER_NEVR_HOST).unwrap();
    w
}

#[test]
fn a_nevr_host_with_no_record_is_adopted_not_set_aside_as_the_games_own_and_not_refused() {
    let w = lost_root_world();
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    assert_eq!((plan.outcome, plan.action), (Outcome::Proceed, Some(Action::Adopt)), "{:?}", plan.reasons);
    let end = pc::execute(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log()).unwrap();
    assert_eq!((end.state, end.reason.as_str()), ("installed", ""));
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), HOST_A);
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), RT_A);
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"]);
    assert!(!listing(&aside(&w)).iter().any(|n| n.starts_with("BugSplat64.dll.original-")), "a NEVR host is never kept as the game's own file");
    let rec = w.record();
    assert_eq!(rec.get("original"), Some("unknown"));
    assert_eq!(rec.get("backup"), Some(""));
    assert_eq!(rec.get("installed_sha256").unwrap(), nevr_install::util::sha256_hex(HOST_A));
    assert_eq!(rec.get("previous_sha256").unwrap(), nevr_install::util::sha256_hex(OTHER_NEVR_HOST), "the host that was there is a known one");
}

#[test]
fn a_lost_root_with_this_packages_host_already_in_place_records_it_and_brings_the_runtime() {
    let w = world();
    fs::write(w.game.path().join(pc::HOST), HOST_A).unwrap();
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    assert_eq!((plan.outcome, plan.action), (Outcome::Proceed, Some(Action::Adopt)), "{:?}", plan.reasons);
    let end = pc::execute(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log()).unwrap();
    assert_eq!(end.state, "installed", "{}", end.reason);
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), RT_A);
    let rec = w.record();
    assert_eq!(rec.get("original"), Some("unknown"));
    assert_eq!(rec.get("previous_sha256"), Some(""), "the host was already this package's");
    assert_eq!(plan_of(&w, &pkg(HOST_A, RT_A), false).1.outcome, Outcome::Unchanged, "the record is back, so the next run changes nothing");
}

#[test]
fn this_packages_own_host_is_adopted_even_when_it_carries_no_build_stamp() {
    // Recognition by the stamp is for other builds; the package's own bytes are known without it.
    let w = world();
    let plain: &[u8] = b"a host with no identity literal";
    fs::write(w.game.path().join(pc::HOST), plain).unwrap();
    let (plan, end) = run(&w, &pkg(plain, RT_A));
    assert_eq!((plan.action, end.state), (Some(Action::Adopt), "installed"), "{}", end.reason);
    assert_eq!(w.record().get("original"), Some("unknown"));
    assert!(!listing(&aside(&w)).iter().any(|n| n.starts_with("BugSplat64.dll.original-")));
}

#[test]
fn an_adopted_install_updates_without_a_backup_and_keeps_the_original_unknown() {
    let w = lost_root_world();
    run(&w, &pkg(HOST_A, RT_A));
    let (plan, end) = run(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert_eq!((plan.action, end.state), (Some(Action::Update), "installed"), "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), HOST_B);
    let rec = w.record();
    assert_eq!(rec.get("original"), Some("unknown"));
    assert_eq!(rec.get("backup"), Some(""));
    let previous = rec.get("previous_sha256").unwrap();
    assert!(previous.contains(&nevr_install::util::sha256_hex(OTHER_NEVR_HOST)) && previous.contains(&nevr_install::util::sha256_hex(HOST_A)), "{previous}");
}

#[test]
fn the_games_own_file_restored_over_an_adopted_install_is_set_aside_and_becomes_the_known_original() {
    let w = lost_root_world();
    run(&w, &pkg(HOST_A, RT_A));
    fs::write(w.game.path().join(pc::HOST), GAME).unwrap();
    let (plan, end) = run(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert!(plan.stale_record);
    assert_eq!((plan.action, end.state), (Some(Action::First), "installed"), "{}", end.reason);
    let rec = w.record();
    assert_eq!(rec.get("original"), None, "the original is known again");
    assert_eq!(fs::read(rec.get("backup").unwrap()).unwrap(), GAME);
}

#[test]
fn an_adopt_that_fails_puts_the_host_and_runtime_back_and_writes_no_record() {
    let w = lost_root_world();
    fs::write(w.root.path().join(pc::RUNTIME), b"stranger").unwrap();
    let seam = Seam { fail_step: Some("record_final"), fail_undo: None };
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    assert_eq!(plan.action, Some(Action::Adopt));
    let end = pc::execute_with(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log(), &seam).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), OTHER_NEVR_HOST);
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), b"stranger");
    assert!(!w.layout().record().exists());
}

#[test]
fn a_legacy_dbgcore_beside_an_adopted_host_is_moved_into_the_root() {
    let w = lost_root_world();
    fs::write(w.game.path().join("dbgcore.dll"), b"legacy").unwrap();
    let (plan, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!((plan.action, end.state), (Some(Action::Adopt), "installed"), "{}", end.reason);
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"]);
    assert_eq!(fs::read(aside_named(&w, "dbgcore.dll.legacy-")).unwrap(), b"legacy");
    assert!(w.record().get("legacy").unwrap().contains("dbgcore.dll.legacy-"));
}

#[test]
fn the_pinned_vanilla_hash_and_the_recovery_text_without_an_original() {
    assert_eq!(pc::VANILLA_HOST_SHA256, "618e34d3bfde69846c0202d42951366bff5e03cd2885b6825f55e09fb2cf160b");
    let text = pc::recovery_without_original();
    for want in [pc::VANILLA_HOST_SHA256, "verify", "reinstall", "doesn't restore it"] {
        assert!(text.to_lowercase().contains(&want.to_lowercase()), "{want} missing from: {text}");
    }
    // The plan tells the player the same thing when it adopts, so a lost original is known before it matters.
    let w = lost_root_world();
    let (_, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    assert!(plan.reasons.iter().any(|r| r.contains(pc::VANILLA_HOST_SHA256)), "{:?}", plan.reasons);
}

#[test]
fn the_log_says_whether_the_original_host_is_the_pinned_one() {
    let read = |w: &World| -> Vec<serde_json::Value> {
        fs::read_to_string(w.scratch.path().join("log.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).filter(|r: &serde_json::Value| r["event"] == "host_original").collect()
    };
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let rec = read(&w);
    assert_eq!(rec.len(), 1);
    assert_eq!(rec[0]["state"], "saved");
    assert_eq!(rec[0]["matches_pinned"], false, "the test's stand-in is not the pinned file");
    assert_eq!(rec[0]["sha256"], nevr_install::util::sha256_hex(GAME));
    let w = lost_root_world();
    run(&w, &pkg(HOST_A, RT_A));
    let rec = read(&w);
    assert_eq!((rec.len(), rec[0]["state"].as_str()), (1, Some("unknown")));
    assert_eq!(rec[0]["pinned_sha256"], pc::VANILLA_HOST_SHA256);
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
    run(&w, &pkg_v("2.10.0", HOST_B, RT_B));
    assert_eq!(plan_of(&w, &pkg_v("2.9.0", HOST_A, RT_A), false).1.outcome, Outcome::NeedsUser, "2.10.0 is newer than 2.9.0");
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

#[test]
fn an_error_before_anything_changed_ends_unchanged_not_restored() {
    let w = world();
    // A file where the installer folder goes: the aside folder cannot be made, and nothing has been touched yet.
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    fs::write(w.root.path().join("installer"), b"in the way").unwrap();
    let end = pc::execute(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log()).unwrap();
    assert_eq!(end.state, "unchanged", "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), GAME);
    assert!(!w.root.path().join(pc::RUNTIME).exists());
}

#[test]
fn the_same_files_under_a_new_release_number_update_the_record() {
    let w = world();
    run(&w, &pkg_v("1.0.0", HOST_A, RT_A));
    let (plan, end) = run(&w, &pkg_v("1.0.1", HOST_A, RT_A));
    assert_eq!((plan.action, end.state), (Some(Action::RecordOnly), "installed"));
    assert_eq!(w.record().get("release"), Some("1.0.1"));
}

#[test]
fn a_record_beside_the_game_for_the_current_package_is_moved_into_the_root() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    let text = fs::read_to_string(w.layout().record()).unwrap();
    fs::remove_file(w.layout().record()).unwrap();
    fs::write(w.game.path().join(pc::STATE_FILE), text).unwrap();
    let (plan, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!((plan.action, end.state), (Some(Action::RecordOnly), "installed"), "{}", end.reason);
    assert!(w.layout().record().is_file() && !w.game.path().join(pc::STATE_FILE).exists());
}

#[test]
fn an_earlier_runtime_that_appears_beside_the_game_after_an_install_is_moved_too() {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    fs::write(w.game.path().join(pc::RUNTIME), b"an older runtime NEVR-BUILD 0.9.0").unwrap();
    let (plan, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!((plan.action, end.state), (Some(Action::RecordOnly), "installed"));
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"]);
}

#[test]
fn a_failure_after_the_record_was_written_takes_the_record_back_too() {
    let w = world();
    // First install: the record did not exist, so it must not exist afterwards.
    let seam = Seam { fail_step: Some("record_final"), fail_undo: None };
    let (obs, plan) = plan_of(&w, &pkg(HOST_A, RT_A), false);
    let end = pc::execute_with(&w.layout(), &plan, &obs, &pkg(HOST_A, RT_A), &w.log(), &seam).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert!(!w.layout().record().exists());
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), GAME);
    // An update: the record that described the earlier install comes back byte for byte.
    run(&w, &pkg(HOST_A, RT_A));
    let before = fs::read(w.layout().record()).unwrap();
    let (obs, plan) = plan_of(&w, &pkg_v("2.0.0", HOST_B, RT_B), false);
    let end = pc::execute_with(&w.layout(), &plan, &obs, &pkg_v("2.0.0", HOST_B, RT_B), &w.log(), &seam).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(w.layout().record()).unwrap(), before);
}

// The refusals the shell installers pinned before the core replaced them (#1149): each one is a plan that asks the player
// and changes nothing.

const A_RUNTIME_MARKER: &[u8] = b"MZ\0NEVR-BUILD 5.0.0 aaaaaaaa";
const PRE_MARKER_RUNTIME: &[u8] = b"MZ\0nevr-runtime/1.0\0aaaaaaaa";

fn installed_a() -> World {
    let w = world();
    run(&w, &pkg(HOST_A, RT_A));
    w
}

fn backup_of(w: &World) -> PathBuf {
    PathBuf::from(w.record().get("backup").unwrap())
}

/// The game folder and the record, to compare a refusal against.
fn snapshot(w: &World) -> (Vec<(String, Vec<u8>)>, Vec<u8>) {
    let files = listing(w.game.path()).into_iter().map(|n| (n.clone(), fs::read(w.game.path().join(&n)).unwrap())).collect();
    (files, fs::read(w.layout().record()).unwrap())
}

fn needs_user(w: &World, p: &Package) -> String {
    let (_, plan) = plan_of(w, p, false);
    assert_eq!(plan.outcome, Outcome::NeedsUser, "{plan:?}");
    plan.reasons.join("; ")
}

#[test]
fn a_backup_that_carries_the_runtime_marker_is_refused_for_an_update_and_for_recording_a_hand_copy() {
    for payload in [A_RUNTIME_MARKER, PRE_MARKER_RUNTIME] {
        let w = installed_a();
        fs::write(backup_of(&w), payload).unwrap();
        let before = snapshot(&w);
        let why = needs_user(&w, &pkg_v("2.0.0", HOST_B, RT_B));
        assert!(why.contains("NEVR runtime") && why.contains("Verify Files"), "{why}");
        // This package's own host copied in by hand would only be recorded; the poisoned backup refuses that too.
        fs::write(w.game.path().join(pc::HOST), HOST_B).unwrap();
        let why = needs_user(&w, &pkg_v("2.0.0", HOST_B, RT_B));
        assert!(why.contains("NEVR runtime"), "{why}");
        fs::write(w.game.path().join(pc::HOST), HOST_A).unwrap();
        assert_eq!(snapshot(&w), before, "a refusal changes nothing");
    }
}

#[test]
fn a_backup_that_only_resembles_the_marker_is_the_games_own_file() {
    // The marker is "NEVR-BUILD " (trailing space, upper case) or the CDN user agent; these are neither.
    for payload in [&b"MZ\0NEVR-BUILD\0aaaaaaaa"[..], &b"MZ\0nevr-build 5.0.0 aaaaaaaa"[..]] {
        let w = installed_a();
        fs::write(backup_of(&w), payload).unwrap();
        let (plan, end) = run(&w, &pkg_v("2.0.0", HOST_B, RT_B));
        assert_eq!((plan.action, end.state), (Some(Action::Update), "installed"), "{}", end.reason);
        assert_eq!(fs::read(backup_of(&w)).unwrap(), payload, "the backup is kept as it was");
        // A first install over a host with those bytes sets it aside instead of refusing it as a runtime.
        let fresh = world();
        fs::write(fresh.game.path().join(pc::HOST), payload).unwrap();
        let (plan, end) = run(&fresh, &pkg(HOST_A, RT_A));
        assert_eq!((plan.action, end.state), (Some(Action::First), "installed"), "{}", end.reason);
        assert_eq!(fs::read(backup_of(&fresh)).unwrap(), payload);
    }
}

#[test]
fn a_host_with_the_cdn_user_agent_and_no_record_is_adopted_never_saved_as_the_original() {
    let w = world();
    fs::write(w.game.path().join(pc::HOST), PRE_MARKER_RUNTIME).unwrap();
    let (plan, end) = run(&w, &pkg(HOST_A, RT_A));
    assert_eq!((plan.action, end.state), (Some(Action::Adopt), "installed"), "{}", end.reason);
    assert!(listing(&aside(&w)).iter().all(|n| !n.starts_with("BugSplat64.dll.original-")), "{:?}", listing(&aside(&w)));
    assert_eq!(w.record().get("original"), Some("unknown"));
    assert_eq!(w.record().get("previous_sha256").unwrap(), nevr_install::util::sha256_hex(PRE_MARKER_RUNTIME));
}

#[test]
fn a_deleted_backup_refuses_an_update_and_a_hand_copy_and_changes_nothing() {
    let w = installed_a();
    fs::remove_file(backup_of(&w)).unwrap();
    let before = snapshot(&w);
    let why = needs_user(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert!(why.contains("is gone"), "{why}");
    fs::write(w.game.path().join(pc::HOST), HOST_B).unwrap();
    let why = needs_user(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert!(why.contains("is gone"), "{why}");
    fs::write(w.game.path().join(pc::HOST), HOST_A).unwrap();
    assert_eq!(snapshot(&w), before);
}

#[test]
fn a_dbgcore_that_is_back_refuses_a_hand_copy_and_changes_nothing() {
    let w = installed_a();
    fs::write(w.game.path().join(pc::HOST), HOST_B).unwrap();
    fs::write(w.game.path().join("dbgcore.dll"), b"legacy dbgcore").unwrap();
    let before = snapshot(&w);
    let why = needs_user(&w, &pkg_v("2.0.0", HOST_B, RT_B));
    assert!(why.contains("a dbgcore.dll exists again"), "{why}");
    assert_eq!(snapshot(&w), before);
}

#[test]
fn a_folder_with_a_non_ascii_name_round_trips_through_the_record() {
    let parent = tempfile::tempdir().unwrap();
    let game = parent.path().join("Zoë").join("bin").join("win10");
    fs::create_dir_all(&game).unwrap();
    fs::write(game.join("echovr.exe"), b"exe").unwrap();
    fs::write(game.join(pc::HOST), GAME).unwrap();
    // The backup is kept under the root, so the root's name is non-ASCII too: the record has to carry it intact.
    let root = parent.path().join("Zoë root");
    let scratch = tempfile::tempdir().unwrap();
    let layout = Layout::new(&game, &root);
    let package = pkg(HOST_A, RT_A);
    let obs = pc::observe(&layout, false).unwrap();
    let plan = pc::plan(&obs, &package, false);
    let log = Log::open(&scratch.path().join("log.jsonl"), "pc").unwrap();
    let end = pc::execute(&layout, &plan, &obs, &package, &log).unwrap();
    assert_eq!(end.state, "installed", "{}", end.reason);
    let raw = fs::read(layout.record()).unwrap();
    assert!(!raw.starts_with(b"\xef\xbb\xbf"), "the record is written without a BOM");
    assert!(String::from_utf8(raw).unwrap().contains("Zoë"), "the folder name is kept as UTF-8");
    let again = pc::observe(&layout, false).unwrap();
    assert!(again.backup_exists, "the backup path in the record resolves");
    assert_eq!(again.record.unwrap().get("game_dir"), Some(game.to_str().unwrap()));
}
