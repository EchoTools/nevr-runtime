//! `nevr-install pc uninstall` through the CLI binary (#1497): the restore, the record, the exit codes, the one stdout line,
//! the log and the stdin rule. Each test builds its own game folder and nevr root in a temporary directory, runs the binary
//! on them, and reads what is left. Nothing here reads the network, a real install, or the caller's stdin.

use nevr_install::pc::{Record, RecordFields};
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

const GAME: &[u8] = b"the game's own BugSplat64.dll";
const HOST: &[u8] = b"nevr host NEVR-BUILD 9.9.9";
const RUNTIME: &[u8] = b"nevr runtime NEVR-BUILD 9.9.9";

fn sha(b: &[u8]) -> String {
    nevr_install::util::sha256_hex(b)
}

/// The game check (`game_running`) looks at every process on the machine, so one test's fake `echovr.exe` would stop another
/// test's run. Every test holds this gate for its whole length, so the runs are one at a time.
static GATE: std::sync::Mutex<()> = std::sync::Mutex::new(());

struct Fixture {
    _gate: std::sync::MutexGuard<'static, ()>,
    _tmp: tempfile::TempDir,
    game: PathBuf,
    root: PathBuf,
    core: PathBuf,
    log: PathBuf,
}

struct Run {
    exit: i32,
    stdout: String,
    stderr: String,
}

impl Run {
    fn json(&self) -> Value {
        serde_json::from_str(self.stdout.trim()).unwrap_or_else(|e| panic!("stdout is not one JSON object ({e}): {:?}", self.stdout))
    }
}

impl Fixture {
    fn new() -> Fixture {
        let gate = GATE.lock().unwrap_or_else(|e| e.into_inner());
        let tmp = tempfile::tempdir().unwrap();
        let f = Fixture {
            _gate: gate,
            game: tmp.path().join("game"),
            root: tmp.path().join("nevr"),
            core: tmp.path().join("core"),
            log: tmp.path().join("nevr-install.jsonl"),
            _tmp: tmp,
        };
        fs::create_dir_all(&f.game).unwrap();
        fs::write(f.game.join("echovr.exe"), b"exe").unwrap();
        f
    }

    /// The install `pc install` leaves: the game's own file kept aside in the root, our host beside echovr.exe, the runtime
    /// in the root and the record in the root's installer folder (ADR 0012).
    fn installed(&self) -> PathBuf {
        let backup = self.root.join("installer/aside/BugSplat64.dll.original-20261030-120000");
        fs::create_dir_all(backup.parent().unwrap()).unwrap();
        fs::create_dir_all(self.root.join("plugins")).unwrap();
        fs::write(&backup, GAME).unwrap();
        fs::write(self.game.join("BugSplat64.dll"), HOST).unwrap();
        fs::write(self.root.join("nevr-runtime.dll"), RUNTIME).unwrap();
        let backup_s = backup.display().to_string();
        self.write_record(&RecordFields {
            backup: &backup_s,
            runtime_root: &self.root.display().to_string(),
            game_dir: &self.game.display().to_string(),
            release: "1.0.0",
            installed: &sha(HOST),
            runtime: &sha(RUNTIME),
            ..Default::default()
        });
        backup
    }

    fn write_record(&self, f: &RecordFields) {
        fs::create_dir_all(self.root.join("installer")).unwrap();
        fs::write(self.root.join("installer/nevr-install.txt"), Record::render(f)).unwrap();
    }

    fn args(&self) -> Vec<String> {
        ["pc", "uninstall", "--dir"]
            .iter()
            .map(|s| s.to_string())
            .chain([self.game.display().to_string(), "--nevr-root".into(), self.root.display().to_string(), "--dev-nevr-root".into()])
            .chain(["--root".into(), self.core.display().to_string(), "--log".into(), self.log.display().to_string()])
            .collect()
    }

    fn run(&self) -> Run {
        self.run_with(&[])
    }

    fn run_with(&self, extra: &[String]) -> Run {
        let out = Command::new(env!("CARGO_BIN_EXE_nevr-install")).args(self.args()).args(extra).stdin(Stdio::null()).output().unwrap();
        Run { exit: out.status.code().unwrap_or(-1), stdout: String::from_utf8_lossy(&out.stdout).into_owned(), stderr: String::from_utf8_lossy(&out.stderr).into_owned() }
    }

    fn log_records(&self) -> Vec<Value> {
        fs::read_to_string(&self.log).unwrap_or_default().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
    }
}

/// Every file under `dir`, by path, with its bytes: the before and after of a run.
fn snapshot(dir: &Path) -> Vec<(String, Vec<u8>)> {
    fn walk(base: &Path, dir: &Path, out: &mut Vec<(String, Vec<u8>)>) {
        let Ok(rd) = fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(base, &p, out);
            } else {
                out.push((p.strip_prefix(base).unwrap().display().to_string(), fs::read(&p).unwrap()));
            }
        }
    }
    let mut out = Vec::new();
    walk(dir, dir, &mut out);
    out.sort();
    out
}

fn names(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = fs::read_dir(dir).map(|rd| rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
    v.sort();
    v
}

#[test]
fn uninstall_restores_the_recorded_original_and_sets_the_runtime_aside() {
    let f = Fixture::new();
    f.installed();
    let r = f.run();
    assert_eq!(r.exit, 0, "stdout={} stderr={}", r.stdout, r.stderr);
    assert_eq!(fs::read(f.game.join("BugSplat64.dll")).unwrap(), GAME, "the game's own file is back, byte for byte");
    assert!(!f.root.join("nevr-runtime.dll").exists(), "the runtime leaves the root's load path");
    let aside = names(&f.root.join("installer/aside"));
    assert_eq!(aside.iter().filter(|n| n.starts_with("nevr-runtime.dll.uninstalled-")).count(), 1, "{aside:?}");
    assert_eq!(aside.iter().filter(|n| n.starts_with("nevr-install.txt.undone-")).count(), 1, "the record is set aside, not deleted: {aside:?}");
    assert!(!f.root.join("installer/nevr-install.txt").exists());
    assert_eq!(names(&f.game), vec!["BugSplat64.dll", "echovr.exe"], "nothing of ours beside echovr.exe but the host");
}

#[test]
fn stdout_is_one_json_line_in_the_plan_and_install_shape() {
    let f = Fixture::new();
    f.installed();
    let r = f.run();
    assert_eq!(r.stdout.lines().count(), 1, "exactly one line on stdout: {:?}", r.stdout);
    let v = r.json();
    let mut keys: Vec<&str> = v.as_object().unwrap().keys().map(String::as_str).collect();
    keys.sort();
    assert_eq!(keys, vec!["exit", "failed", "log", "plan", "reason", "run_id", "state"]);
    assert_eq!(v["state"], "uninstalled");
    assert_eq!(v["exit"], 0);
    let mut pk: Vec<&str> = v["plan"].as_object().unwrap().keys().map(String::as_str).collect();
    pk.sort();
    assert_eq!(pk, vec!["action", "decision", "outcome", "reasons", "stale_record"]);
}

#[test]
fn uninstall_reads_nothing_from_stdin() {
    let f = Fixture::new();
    f.installed();
    // The pipe is held open and never written or closed: a read of stdin would block until the deadline.
    let mut child: Child = Command::new(env!("CARGO_BIN_EXE_nevr-install")).args(f.args()).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().unwrap();
    let _held = child.stdin.take();
    let deadline = Instant::now() + Duration::from_secs(60);
    let status = loop {
        if let Some(s) = child.try_wait().unwrap() {
            break s;
        }
        assert!(Instant::now() < deadline, "pc uninstall is waiting on stdin");
        std::thread::sleep(Duration::from_millis(50));
    };
    assert_eq!(status.code(), Some(0));
}

#[test]
fn with_no_install_record_the_run_exits_0_unchanged_and_changes_nothing() {
    let f = Fixture::new();
    fs::write(f.game.join("BugSplat64.dll"), HOST).unwrap();
    let before = snapshot(&f.game);
    let r = f.run();
    assert_eq!(r.exit, 0, "stdout={} stderr={}", r.stdout, r.stderr);
    let v = r.json();
    assert_eq!((v["state"].as_str(), v["exit"].as_i64()), (Some("unchanged"), Some(0)));
    assert_eq!(snapshot(&f.game), before);
    assert!(!f.root.exists() || snapshot(&f.root).is_empty(), "no root file is written");
}

#[test]
fn an_unknown_original_exits_2_changes_nothing_and_carries_the_verify_files_advice() {
    let f = Fixture::new();
    f.installed();
    f.write_record(&RecordFields {
        runtime_root: &f.root.display().to_string(),
        game_dir: &f.game.display().to_string(),
        release: "1.0.0",
        installed: &sha(HOST),
        runtime: &sha(RUNTIME),
        original_unknown: true,
        ..Default::default()
    });
    let (game_before, root_before) = (snapshot(&f.game), snapshot(&f.root));
    let r = f.run();
    assert_eq!(r.exit, 2, "stdout={} stderr={}", r.stdout, r.stderr);
    let v = r.json();
    assert_eq!(v["state"], "needs-user");
    let reason = v["reason"].as_str().unwrap();
    assert!(reason.contains("verify the game's files on the platform (Steam, Oculus)"), "{reason}");
    assert!(reason.contains("618e34d3bfde69846c0202d42951366bff5e03cd2885b6825f55e09fb2cf160b"), "{reason}");
    assert_eq!(snapshot(&f.game), game_before);
    assert_eq!(snapshot(&f.root), root_before);
}

#[test]
fn a_record_another_installer_owns_is_left_alone_with_exit_2() {
    let f = Fixture::new();
    f.installed();
    let text = fs::read_to_string(f.root.join("installer/nevr-install.txt")).unwrap().replace("owner=nevr-install", "owner=launcher");
    fs::write(f.root.join("installer/nevr-install.txt"), text).unwrap();
    let before = snapshot(&f.game);
    let r = f.run();
    assert_eq!(r.exit, 2, "stdout={} stderr={}", r.stdout, r.stderr);
    assert_eq!(r.json()["state"], "needs-user");
    assert_eq!(snapshot(&f.game), before);
    assert!(f.root.join("nevr-runtime.dll").exists());
}

#[test]
fn a_backup_that_is_a_nevr_runtime_is_refused_with_exit_2() {
    let f = Fixture::new();
    let backup = f.installed();
    fs::write(&backup, RUNTIME).unwrap();
    let before = snapshot(&f.game);
    let r = f.run();
    assert_eq!(r.exit, 2, "stdout={} stderr={}", r.stdout, r.stderr);
    assert!(r.json()["reason"].as_str().unwrap().contains("is a NEVR runtime, not the game's own file"));
    assert_eq!(snapshot(&f.game), before);
}

#[test]
fn a_game_host_that_is_not_ours_is_refused_with_exit_2() {
    let f = Fixture::new();
    f.installed();
    fs::write(f.game.join("BugSplat64.dll"), b"someone else's host").unwrap();
    let r = f.run();
    assert_eq!(r.exit, 2, "stdout={} stderr={}", r.stdout, r.stderr);
    assert!(r.json()["reason"].as_str().unwrap().contains("changed since"));
    assert_eq!(fs::read(f.game.join("BugSplat64.dll")).unwrap(), b"someone else's host");
}

#[test]
fn a_running_game_is_refused_with_exit_2_and_nothing_changes() {
    let f = Fixture::new();
    f.installed();
    // A process whose command name is echovr.exe (the name /proc reports for the game) keeps the run from proceeding.
    fs::remove_file(f.game.join("echovr.exe")).unwrap();
    std::os::unix::fs::symlink("/bin/sleep", f.game.join("echovr.exe")).unwrap();
    let mut game = Command::new(f.game.join("echovr.exe")).arg("60").spawn().unwrap();
    let before = snapshot(&f.game);
    let r = f.run();
    let _ = game.kill();
    let _ = game.wait();
    assert_eq!(r.exit, 2, "stdout={} stderr={}", r.stdout, r.stderr);
    assert!(r.json()["reason"].as_str().unwrap().contains("echovr.exe is running"));
    assert_eq!(snapshot(&f.game), before);
}

#[test]
fn the_run_writes_jsonl_records_for_each_restore_and_its_end() {
    let f = Fixture::new();
    f.installed();
    let r = f.run();
    assert_eq!(r.exit, 0);
    let recs = f.log_records();
    let events: Vec<&str> = recs.iter().filter_map(|v| v["event"].as_str()).collect();
    for want in ["run_start", "file_restore", "run_end"] {
        assert!(events.contains(&want), "missing {want}: {events:?}");
    }
    let end = recs.last().unwrap();
    assert_eq!((end["event"].as_str(), end["state"].as_str()), (Some("run_end"), Some("uninstalled")));
    let restores = recs.iter().filter(|v| v["event"] == "file_restore").count();
    assert!(restores >= 3, "the host, the runtime set-aside and the record each get a line: {restores}");
}

#[test]
fn a_legacy_dbgcore_goes_back_beside_echovr_exe() {
    let f = Fixture::new();
    f.installed();
    let legacy = f.root.join("installer/aside/dbgcore.dll.legacy-20261029-100002");
    fs::write(&legacy, b"the legacy dbgcore.dll").unwrap();
    f.write_record(&RecordFields {
        backup: &f.root.join("installer/aside/BugSplat64.dll.original-20261030-120000").display().to_string(),
        legacy: &legacy.display().to_string(),
        runtime_root: &f.root.display().to_string(),
        game_dir: &f.game.display().to_string(),
        release: "1.0.0",
        installed: &sha(HOST),
        runtime: &sha(RUNTIME),
        ..Default::default()
    });
    let r = f.run();
    assert_eq!(r.exit, 0, "stdout={} stderr={}", r.stdout, r.stderr);
    assert_eq!(fs::read(f.game.join("dbgcore.dll")).unwrap(), b"the legacy dbgcore.dll");
    assert!(!legacy.exists(), "moved back, not copied");
}

#[test]
fn an_earlier_runtime_and_a_runtime_moved_out_of_the_game_folder_go_back_to_their_names() {
    let f = Fixture::new();
    f.installed();
    let aside = f.root.join("installer/aside");
    let earlier = aside.join("nevr-runtime.dll.original-20261029-100000");
    fs::write(&earlier, b"the runtime a player had before install").unwrap();
    let moved = aside.join("nevr-runtime.dll.legacy-20261029-100001");
    fs::write(&moved, b"a runtime moved out of the game folder NEVR-BUILD 0.9.0").unwrap();
    f.write_record(&RecordFields {
        backup: &aside.join("BugSplat64.dll.original-20261030-120000").display().to_string(),
        runtime_aside: &earlier.display().to_string(),
        runtime_legacy: &moved.display().to_string(),
        runtime_root: &f.root.display().to_string(),
        game_dir: &f.game.display().to_string(),
        release: "1.0.0",
        installed: &sha(HOST),
        runtime: &sha(RUNTIME),
        ..Default::default()
    });
    let r = f.run();
    assert_eq!(r.exit, 0, "stdout={} stderr={}", r.stdout, r.stderr);
    assert_eq!(fs::read(f.root.join("nevr-runtime.dll")).unwrap(), b"the runtime a player had before install");
    assert_eq!(fs::read(f.game.join("nevr-runtime.dll")).unwrap(), b"a runtime moved out of the game folder NEVR-BUILD 0.9.0");
    assert!(names(&aside).iter().all(|n| !n.starts_with("nevr-runtime.dll.original-") && !n.starts_with("nevr-runtime.dll.legacy-")), "both went back");
}

#[test]
fn a_runtime_in_the_root_that_is_not_a_nevr_runtime_is_left_alone() {
    let f = Fixture::new();
    f.installed();
    fs::write(f.root.join("nevr-runtime.dll"), b"a runtime that is not ours").unwrap();
    let r = f.run();
    assert_eq!(r.exit, 0, "stdout={} stderr={}", r.stdout, r.stderr);
    assert_eq!(fs::read(f.root.join("nevr-runtime.dll")).unwrap(), b"a runtime that is not ours");
    assert_eq!(r.json()["state"], "uninstalled");
}

#[test]
fn a_set_aside_file_the_record_names_that_is_gone_or_a_dbgcore_that_is_back_is_refused() {
    let f = Fixture::new();
    f.installed();
    let aside = f.root.join("installer/aside");
    let rec = |legacy: &str, runtime_aside: &str| {
        f.write_record(&RecordFields {
            backup: &aside.join("BugSplat64.dll.original-20261030-120000").display().to_string(),
            legacy,
            runtime_aside,
            runtime_root: &f.root.display().to_string(),
            game_dir: &f.game.display().to_string(),
            release: "1.0.0",
            installed: &sha(HOST),
            runtime: &sha(RUNTIME),
            ..Default::default()
        })
    };
    let gone = aside.join("dbgcore.dll.legacy-20261029-100009").display().to_string();
    rec(&gone, "");
    let r = f.run();
    assert_eq!(r.exit, 2, "a gone legacy dbgcore: stdout={} stderr={}", r.stdout, r.stderr);
    assert!(r.json()["reason"].as_str().unwrap().contains("the set-aside dbgcore.dll is gone"));

    let gone_runtime = aside.join("nevr-runtime.dll.original-20261029-100009").display().to_string();
    rec("", &gone_runtime);
    let r = f.run();
    assert_eq!(r.exit, 2, "a gone runtime aside: stdout={} stderr={}", r.stdout, r.stderr);
    assert!(r.json()["reason"].as_str().unwrap().contains("the set-aside nevr-runtime.dll is gone"));

    let legacy = aside.join("dbgcore.dll.legacy-20261029-100010");
    fs::write(&legacy, b"legacy").unwrap();
    rec(&legacy.display().to_string(), "");
    fs::write(f.game.join("dbgcore.dll"), b"a dbgcore that came back").unwrap();
    let before = snapshot(&f.game);
    let r = f.run();
    assert_eq!(r.exit, 2, "a dbgcore beside the game: stdout={} stderr={}", r.stdout, r.stderr);
    assert!(r.json()["reason"].as_str().unwrap().contains("a dbgcore.dll exists again"));
    assert_eq!(snapshot(&f.game), before);
}

#[test]
fn a_game_folder_that_refuses_the_write_ends_unchanged_with_exit_1() {
    use std::os::unix::fs::PermissionsExt;
    let f = Fixture::new();
    f.installed();
    let before_root = snapshot(&f.root);
    fs::set_permissions(&f.game, fs::Permissions::from_mode(0o555)).unwrap();
    let r = f.run();
    fs::set_permissions(&f.game, fs::Permissions::from_mode(0o755)).unwrap();
    assert_eq!(r.exit, 1, "stdout={} stderr={}", r.stdout, r.stderr);
    let v = r.json();
    assert_eq!((v["state"].as_str(), v["failed"].as_bool()), (Some("unchanged"), Some(true)), "{v}");
    assert_eq!(fs::read(f.game.join("BugSplat64.dll")).unwrap(), HOST, "the host was never replaced");
    assert_eq!(snapshot(&f.root), before_root, "the root was not touched");
}

#[test]
fn a_root_the_host_does_not_load_from_is_refused_without_the_dev_flag() {
    let f = Fixture::new();
    f.installed();
    let args: Vec<String> = f.args().into_iter().filter(|a| a != "--dev-nevr-root").collect();
    let out = Command::new(env!("CARGO_BIN_EXE_nevr-install")).args(&args).stdin(Stdio::null()).output().unwrap();
    assert_eq!(out.status.code(), Some(2), "stdout={}", String::from_utf8_lossy(&out.stdout));
    let v: Value = serde_json::from_slice(&out.stdout).unwrap();
    // On Windows the reason names the canonical folder; on this platform it says there is none. Both refuse, and both name the flag.
    let reason = v["reason"].as_str().unwrap();
    assert!(reason.contains("is refused") || reason.contains("is not the folder the game loads"), "{v}");
    assert!(reason.contains("--dev-nevr-root"), "{v}");
    assert!(f.root.join("nevr-runtime.dll").exists(), "nothing was undone");
}

#[test]
fn a_game_folder_the_launcher_lists_is_refused_with_exit_2_and_nothing_changes() {
    let mut f = Fixture::new();
    let lroot = f._tmp.path().join("lroot");
    f.game = lroot.join("ready-at-dawn-echo-arena").join("bin").join("win10");
    fs::create_dir_all(&f.game).unwrap();
    fs::write(f.game.join("echovr.exe"), b"exe").unwrap();
    f.installed();
    let launcher = f._tmp.path().join("launcher");
    fs::create_dir_all(&launcher).unwrap();
    fs::write(launcher.join("launcher.json"), format!(r#"{{"versions":[{{"id":"release-1","root":"{}"}}]}}"#, lroot.display())).unwrap();
    let before = snapshot(&f.game);
    let r = f.run_with(&["--launcher-dir".into(), launcher.display().to_string()]);
    assert_eq!(r.exit, 2, "stdout={} stderr={}", r.stdout, r.stderr);
    assert!(r.json()["reason"].as_str().unwrap().contains("Echo VR Launcher manages this install"));
    assert_eq!(snapshot(&f.game), before);
}
