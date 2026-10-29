//! `AdbDevice` against a fake `adb` on PATH: which device it picks and how it names the ones it refuses, the argument
//! vector of every call it makes, and the time limit on a call. The tests in this file share the process environment
//! (PATH), so each holds `BENCH` for its whole length.
#![cfg(unix)]

use nevr_install::device::{AdbDevice, Device};
use std::ffi::OsString;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

static BENCH: Mutex<()> = Mutex::new(());

/// A directory holding a fake `adb`, put first on PATH while the value lives. What the fake does is set by files in
/// the directory: `devices` is printed for `adb devices` (with `devices.rc` as its exit code); every other call is
/// appended to `calls` as its arguments joined by `|`; `reply.<verb>` is printed and `rc.<verb>` is the exit code
/// for the call whose verb (the word after `-s <serial>`) is `<verb>`; `sleep.<verb>` seconds pass before it answers.
struct Bench {
    dir: tempfile::TempDir,
    old_path: Option<OsString>,
    _guard: MutexGuard<'static, ()>,
}

impl Bench {
    fn new(devices: &str) -> Bench {
        let guard = BENCH.lock().unwrap_or_else(|p| p.into_inner());
        let dir = tempfile::tempdir().unwrap();
        let adb = dir.path().join("adb");
        std::fs::write(
            &adb,
            r#"#!/bin/bash
here=$(dirname "$0")
if [ "$1" = devices ]; then cat "$here/devices"; exit "$(cat "$here/devices.rc" 2>/dev/null || echo 0)"; fi
[ "$1" = -s ] && shift 2
verb=$1
(IFS='|'; echo "$*" >> "$here/calls")
[ -e "$here/sleep.$verb" ] && sleep "$(cat "$here/sleep.$verb")"
[ -e "$here/reply.$verb" ] && cat "$here/reply.$verb"
exit "$(cat "$here/rc.$verb" 2>/dev/null || echo 0)"
"#,
        )
        .unwrap();
        std::fs::set_permissions(&adb, std::fs::Permissions::from_mode(0o755)).unwrap();
        std::fs::write(dir.path().join("devices"), devices).unwrap();
        let old_path = std::env::var_os("PATH");
        let mut paths = vec![dir.path().to_path_buf()];
        paths.extend(std::env::split_paths(&old_path.clone().unwrap_or_default()));
        std::env::set_var("PATH", std::env::join_paths(paths).unwrap());
        Bench { dir, old_path, _guard: guard }
    }

    fn set(&self, name: &str, text: &str) {
        std::fs::write(self.dir.path().join(name), text).unwrap();
    }

    fn calls(&self) -> Vec<String> {
        std::fs::read_to_string(self.dir.path().join("calls")).unwrap_or_default().lines().map(String::from).collect()
    }

    fn pick(&self, serial: Option<&str>) -> Result<AdbDevice, String> {
        AdbDevice::pick(serial)
    }
}

impl Drop for Bench {
    fn drop(&mut self) {
        match &self.old_path {
            Some(p) => std::env::set_var("PATH", p),
            None => std::env::remove_var("PATH"),
        }
    }
}

const ONE: &str = "List of devices attached\nQUEST123\tdevice\n";

fn err_of(r: Result<AdbDevice, String>) -> String {
    match r {
        Ok(_) => panic!("a device was picked"),
        Err(e) => e,
    }
}

#[test]
fn one_authorized_device_is_picked() {
    let b = Bench::new(ONE);
    let d = b.pick(None).unwrap();
    assert_eq!(d.fingerprint().len(), 12, "a fingerprint, not the serial");
    assert!(!d.fingerprint().contains("QUEST"));
}

#[test]
fn adb_daemon_lines_are_not_counted_as_devices() {
    let b = Bench::new("* daemon not running; starting now at tcp:5037\n* daemon started successfully\nList of devices attached\nQUEST123\tdevice\n");
    assert!(b.pick(None).is_ok());
}

#[test]
fn no_device_is_named() {
    let b = Bench::new("List of devices attached\n\n");
    assert_eq!(err_of(b.pick(None)), "no adb device attached");
    // Only the daemon's own lines: still no device.
    b.set("devices", "* daemon started successfully\nList of devices attached\n");
    assert_eq!(err_of(b.pick(None)), "no adb device attached");
}

#[test]
fn two_real_devices_are_refused_and_the_message_counts_them() {
    let b = Bench::new("List of devices attached\nQUEST123\tdevice\nOTHER456\tdevice\n");
    let e = err_of(b.pick(None));
    assert!(e.starts_with("2 adb devices attached"), "{e}");
    // --serial narrows the choice to the one that was asked for.
    assert!(b.pick(Some("OTHER456")).is_ok());
    assert_eq!(err_of(b.pick(Some("NOSUCH"))), "no adb device attached");
}

#[test]
fn an_unauthorized_device_is_named_with_what_to_do() {
    let b = Bench::new("List of devices attached\nQUEST123\tunauthorized\n");
    let e = err_of(b.pick(None));
    assert!(e.contains("unauthorized") && e.contains("USB debugging"), "{e}");
}

#[test]
fn a_device_without_permissions_is_named_not_hidden_behind_a_one_word_state() {
    let b = Bench::new("List of devices attached\nQUEST123\tno permissions (missing udev rules? user is in the plugdev group); see [x]\n");
    let e = err_of(b.pick(None));
    assert!(e.contains("no permissions"), "{e}");
    assert!(e.contains("missing udev rules"), "{e}");
    assert!(!e.contains("state 'no'"), "{e}");
}

#[test]
fn any_other_state_is_named() {
    let b = Bench::new("List of devices attached\nQUEST123\toffline\n");
    assert_eq!(err_of(b.pick(None)), "the device is in state 'offline', need 'device'");
}

#[test]
fn adb_devices_that_fails_is_an_error_with_its_output() {
    let b = Bench::new("error: cannot connect to daemon\n");
    b.set("devices.rc", "1");
    let e = err_of(b.pick(None));
    assert!(e.starts_with("'adb devices' failed") && e.contains("cannot connect to daemon"), "{e}");
}

#[test]
fn every_call_is_made_against_the_chosen_serial_with_the_arguments_the_install_needs() {
    let b = Bench::new(ONE);
    let d = b.pick(None).unwrap();
    d.shell("pm path com.readyatdawn.r15", 30).unwrap();
    d.install(Path::new("/w/r15.apk"), true).unwrap();
    d.install(Path::new("/w/r15.apk"), false).unwrap();
    d.uninstall("com.readyatdawn.r15").unwrap();
    d.push(Path::new("/w/data.zip"), "/data/local/tmp/_nevr_data.zip").unwrap();
    assert_eq!(
        b.calls(),
        [
            "shell|pm path com.readyatdawn.r15",
            "install|-r|-g|/w/r15.apk",
            "install|-g|/w/r15.apk",
            "uninstall|com.readyatdawn.r15",
            "push|/w/data.zip|/data/local/tmp/_nevr_data.zip",
        ]
    );
}

#[test]
fn the_serial_is_passed_to_adb_so_a_second_device_is_never_touched() {
    let b = Bench::new("List of devices attached\nQUEST123\tdevice\nOTHER456\tdevice\n");
    // The fake logs after the `-s <serial>` pair; it logs the serial itself in `serials`.
    let log: PathBuf = b.dir.path().join("serials");
    b.set(
        "adb",
        &format!(
            "#!/bin/bash\nif [ \"$1\" = devices ]; then cat \"{d}/devices\"; exit 0; fi\necho \"$1 $2\" >> \"{l}\"\n",
            d = b.dir.path().display(),
            l = log.display()
        ),
    );
    std::fs::set_permissions(b.dir.path().join("adb"), std::fs::Permissions::from_mode(0o755)).unwrap();
    let d = b.pick(Some("OTHER456")).unwrap();
    d.shell("true", 5).unwrap();
    assert_eq!(std::fs::read_to_string(log).unwrap(), "-s OTHER456\n");
}

#[test]
fn a_call_that_exceeds_its_limit_is_an_error_and_is_not_reported_as_an_answer() {
    let b = Bench::new(ONE);
    let d = b.pick(None).unwrap();
    b.set("sleep.shell", "20");
    b.set("reply.shell", "package:/x\n");
    let started = Instant::now();
    let e = d.shell("pm path com.readyatdawn.r15", 1).unwrap_err();
    assert!(e.contains("timed out after 1 s"), "{e}");
    assert!(started.elapsed() < Duration::from_secs(10), "the call was not cut off at its limit: {:?}", started.elapsed());
}

#[test]
fn a_nonzero_exit_and_the_output_of_a_call_are_returned_to_the_caller() {
    let b = Bench::new(ONE);
    let d = b.pick(None).unwrap();
    b.set("reply.push", "adb: error: failed to copy\n");
    b.set("rc.push", "1");
    let o = d.push(Path::new("/w/data.zip"), "/data/local/tmp/_nevr_data.zip").unwrap();
    assert_eq!(o.code, 1);
    assert!(o.text.contains("failed to copy"), "{}", o.text);
}

#[test]
fn the_adb_server_version_mismatch_line_is_not_a_device() {
    let b = Bench::new("adb server version (41) doesn't match this client (39); killing...\n* daemon started successfully\nList of devices attached\nQUEST123\tdevice\n");
    assert!(b.pick(None).is_ok(), "{:?}", b.pick(None).err());
}
