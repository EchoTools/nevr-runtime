//! The PC uninstall as a library (#1497): the plan, the restore and the rollback that `pc uninstall` runs, against a temporary
//! game folder and nevr root. The restore logic is the install's (`pc::put`, `Undo`, `rollback`), so a failed uninstall puts
//! every file back the way a failed install does.

use nevr_install::log::Log;
use nevr_install::pc::{self, Layout, Outcome, Record, RecordFields, Seam};
use std::fs;
use std::path::Path;

const GAME: &[u8] = b"the game's own BugSplat64.dll";
const HOST: &[u8] = b"nevr host NEVR-BUILD 9.9.9";
const RUNTIME: &[u8] = b"nevr runtime NEVR-BUILD 9.9.9";

struct World {
    game: tempfile::TempDir,
    root: tempfile::TempDir,
    scratch: tempfile::TempDir,
}

impl World {
    fn new() -> World {
        let w = World { game: tempfile::tempdir().unwrap(), root: tempfile::tempdir().unwrap(), scratch: tempfile::tempdir().unwrap() };
        fs::write(w.game.path().join("echovr.exe"), b"exe").unwrap();
        fs::write(w.game.path().join(pc::HOST), HOST).unwrap();
        fs::create_dir_all(w.root.path().join("installer/aside")).unwrap();
        let backup = w.root.path().join("installer/aside/BugSplat64.dll.original-20261030-120000");
        fs::write(&backup, GAME).unwrap();
        fs::write(w.root.path().join(pc::RUNTIME), RUNTIME).unwrap();
        let backup_s = backup.display().to_string();
        let record = Record::render(&RecordFields {
            backup: &backup_s,
            runtime_root: &w.root.path().display().to_string(),
            game_dir: &w.game.path().display().to_string(),
            release: "1.0.0",
            installed: &nevr_install::util::sha256_hex(HOST),
            runtime: &nevr_install::util::sha256_hex(RUNTIME),
            ..Default::default()
        });
        fs::write(w.root.path().join("installer").join(pc::STATE_FILE), record).unwrap();
        w
    }
    fn layout(&self) -> Layout {
        Layout::new(self.game.path(), self.root.path())
    }
    fn log(&self) -> Log {
        Log::open(&self.scratch.path().join("log.jsonl"), "pc").unwrap()
    }
}

fn listing(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    v.sort();
    v
}

#[test]
fn the_plan_refuses_an_unknown_original_with_the_verify_files_advice() {
    let w = World::new();
    let rec = Record::render(&RecordFields {
        runtime_root: &w.root.path().display().to_string(),
        game_dir: &w.game.path().display().to_string(),
        release: "1.0.0",
        installed: &nevr_install::util::sha256_hex(HOST),
        runtime: &nevr_install::util::sha256_hex(RUNTIME),
        original_unknown: true,
        ..Default::default()
    });
    fs::write(w.root.path().join("installer").join(pc::STATE_FILE), rec).unwrap();
    let obs = pc::observe(&w.layout(), false).unwrap();
    let plan = pc::plan_uninstall(&obs, &[]);
    assert_eq!(plan.outcome, Outcome::NeedsUser);
    assert_eq!(plan.reasons, vec![pc::recovery_without_original()], "the reason is pc.rs's own recovery text");
}

#[test]
fn a_host_the_package_sums_name_is_one_of_ours_and_is_restored_over() {
    let w = World::new();
    let hand_copied = b"a newer host copied in by hand";
    fs::write(w.game.path().join(pc::HOST), hand_copied).unwrap();
    let obs = pc::observe(&w.layout(), false).unwrap();
    assert_eq!(pc::plan_uninstall(&obs, &[]).outcome, Outcome::NeedsUser, "not ours without the sums");
    let sums = format!("{}  BugSplat64.dll\n{}  nevr-install.exe\n", nevr_install::util::sha256_hex(hand_copied), "0".repeat(64));
    let known = pc::host_hashes_in_sums(&sums);
    assert_eq!(known, vec![nevr_install::util::sha256_hex(hand_copied)], "only the BugSplat64.dll line counts");
    let plan = pc::plan_uninstall(&obs, &known);
    assert_eq!(plan.outcome, Outcome::Proceed, "{:?}", plan.reasons);
}

#[test]
fn a_failed_uninstall_puts_every_file_back() {
    let w = World::new();
    let obs = pc::observe(&w.layout(), false).unwrap();
    let plan = pc::plan_uninstall(&obs, &[]);
    assert_eq!(plan.outcome, Outcome::Proceed, "{:?}", plan.reasons);
    let seam = Seam { fail_step: Some("uninstall_restored"), ..Default::default() };
    let end = pc::execute_uninstall_with(&w.layout(), &obs, &w.log(), &seam).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), HOST, "the host is back as it was");
    assert_eq!(fs::read(w.root.path().join(pc::RUNTIME)).unwrap(), RUNTIME, "the runtime is back where it was");
    assert!(w.root.path().join("installer").join(pc::STATE_FILE).is_file(), "the record is back where it was");
    assert_eq!(listing(w.game.path()), vec!["BugSplat64.dll", "echovr.exe"]);
}

#[test]
fn a_failure_after_the_record_moved_puts_the_record_back_where_it_was() {
    let w = World::new();
    let record_path = w.root.path().join("installer").join(pc::STATE_FILE);
    let before = fs::read(&record_path).unwrap();
    let obs = pc::observe(&w.layout(), false).unwrap();
    let seam = Seam { fail_step: Some("uninstall_record_moved"), ..Default::default() };
    let end = pc::execute_uninstall_with(&w.layout(), &obs, &w.log(), &seam).unwrap();
    assert_eq!(end.state, "restored", "{}", end.reason);
    assert_eq!(fs::read(&record_path).unwrap(), before, "the record is back in the root's installer folder, unchanged");
    assert!(listing(&w.root.path().join("installer/aside")).iter().all(|n| !n.starts_with("nevr-install.txt.undone-")), "no undone copy is left");
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), HOST, "the host is back as it was");
}

#[test]
fn a_clean_uninstall_restores_the_original_and_ends_uninstalled() {
    let w = World::new();
    let obs = pc::observe(&w.layout(), false).unwrap();
    let plan = pc::plan_uninstall(&obs, &[]);
    assert_eq!(plan.outcome, Outcome::Proceed, "{:?}", plan.reasons);
    let end = pc::execute_uninstall_with(&w.layout(), &obs, &w.log(), &Seam::default()).unwrap();
    assert_eq!((end.state, end.reason.as_str()), ("uninstalled", ""));
    assert_eq!(fs::read(w.game.path().join(pc::HOST)).unwrap(), GAME);
    assert!(!w.root.path().join(pc::RUNTIME).exists());
}
