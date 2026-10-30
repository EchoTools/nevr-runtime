//! Quest execution over a fake headset: what is installed, uninstalled and pushed, and in what order.

use nevr_install::device::{Device, Output};
use nevr_install::log::Log;
use nevr_install::manifest::{Android, Artifact, Content};
use nevr_install::quest::{self, Outcome, Plan, RunOpts, Step};
use nevr_install::util::sha256_hex;
use std::cell::RefCell;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::Path;

struct Fake {
    calls: RefCell<Vec<String>>,
    install_results: RefCell<Vec<String>>,
    shell_replies: Vec<(&'static str, String)>,
    /// Exit code of a shell command that contains the needle (default 0).
    shell_codes: Vec<(&'static str, i32)>,
    /// A shell command that contains the needle fails to run at all (a time limit, adb gone): the call returns `Err`.
    shell_errs: Vec<(&'static str, String)>,
    /// Every shell command with the time limit it was given.
    shell_cmds: RefCell<Vec<(String, u64)>>,
    /// The push fails with this answer (exit code 1) instead of succeeding.
    push_fails: Option<&'static str>,
}

impl Fake {
    fn new(install_results: &[&str]) -> Fake {
        Fake {
            calls: RefCell::new(vec![]),
            install_results: RefCell::new(install_results.iter().map(|s| s.to_string()).collect()),
            shell_replies: vec![],
            shell_codes: vec![],
            shell_errs: vec![],
            shell_cmds: RefCell::new(vec![]),
            push_fails: None,
        }
    }
    fn calls(&self) -> Vec<String> {
        self.calls.borrow().clone()
    }
    /// The shell commands run, in order, as `(command, time limit in seconds)`.
    fn shell_cmds(&self) -> Vec<(String, u64)> {
        self.shell_cmds.borrow().clone()
    }
}

fn out(text: &str) -> Output {
    Output { code: if text.contains("Success") || !text.contains("Failure") { 0 } else { 1 }, text: text.to_string() }
}

impl Device for Fake {
    fn fingerprint(&self) -> String {
        "fake".into()
    }
    fn shell(&self, cmd: &str, t: u64) -> Result<Output, String> {
        self.calls.borrow_mut().push(format!("shell {}", cmd.split_whitespace().next().unwrap_or("")));
        self.shell_cmds.borrow_mut().push((cmd.to_string(), t));
        if let Some((_, e)) = self.shell_errs.iter().find(|(n, _)| cmd.contains(n)) {
            return Err(e.clone());
        }
        let code = self.shell_codes.iter().find(|(n, _)| cmd.contains(n)).map(|x| x.1).unwrap_or(0);
        let text = self.shell_replies.iter().find(|(n, _)| cmd.contains(n)).map(|x| x.1.clone()).unwrap_or_default();
        Ok(Output { code, text })
    }
    fn install(&self, _apk: &Path, replace: bool) -> Result<Output, String> {
        self.calls.borrow_mut().push(format!("install replace={replace}"));
        let r = self.install_results.borrow_mut().remove(0);
        Ok(out(&r))
    }
    fn uninstall(&self, package: &str) -> Result<Output, String> {
        self.calls.borrow_mut().push(format!("uninstall {package}"));
        Ok(out("Success"))
    }
    fn push(&self, _l: &Path, _r: &str) -> Result<Output, String> {
        self.calls.borrow_mut().push("push".into());
        match self.push_fails {
            Some(why) => Ok(Output { code: 1, text: why.to_string() }),
            None => Ok(out("1 file pushed")),
        }
    }
}

fn serve_one(body: Vec<u8>) -> String {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
    std::thread::spawn(move || {
        for c in l.incoming().take(4) {
            let Ok(mut c) = c else { continue };
            let mut b = [0u8; 2048];
            let _ = c.read(&mut b);
            let mut r = format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).into_bytes();
            r.extend_from_slice(&body);
            let _ = c.write_all(&r);
        }
    });
    base
}

fn apk_art(bytes: &[u8]) -> Artifact {
    Artifact {
        kind: "quest-apk".into(), platform: Some("android-arm64".into()), path: "v1.0.0/nevr-runtime-v1.0.0-quest.apk".into(),
        size: bytes.len() as u64, sha256: sha256_hex(bytes),
        android: Some(Android { package: quest::PACKAGE.into(), version_code: 5, signer_cert_sha256: vec!["a".repeat(64)] }),
        game_data: None, content_pack: None, contents: vec![],
    }
}

fn plan_with(steps: Vec<Step>, apk: Option<Artifact>, data: Option<Artifact>) -> Plan {
    Plan { outcome: Outcome::Proceed, decision: "test".into(), reasons: vec![], steps, apk, data, consequences: vec![], game_data: String::new(), signer: String::new(), dev_artifact: None }
}

fn opts<'a>(d: &Path, base: &'a str, consent: bool) -> RunOpts<'a> {
    RunOpts { work_dir: d.join("work"), base, consent_uninstall: consent }
}

fn records(p: &Path) -> Vec<serde_json::Value> {
    std::fs::read_to_string(p).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

#[test]
fn a_downloaded_apk_that_aapt2_cannot_read_is_never_installed() {
    let d = tempfile::tempdir().unwrap();
    let apk = b"not an apk".to_vec();
    let base = serve_one(apk.clone());
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let plan = plan_with(vec![Step::FetchApk, Step::CheckApk, Step::InstallApk { replace: false }], Some(apk_art(&apk)), None);
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, true), &log).unwrap();
    assert_eq!(end.state, "unchanged");
    assert!(!dev.calls().iter().any(|c| c.starts_with("install") || c.starts_with("uninstall")), "{:?}", dev.calls());
    let r = records(log.path());
    assert!(r.iter().any(|x| x["event"] == "step_end" && x["step_id"] == "check-apk" && x["ok"] == false));
}

#[test]
fn update_incompatible_without_consent_stops_before_any_uninstall() {
    let d = tempfile::tempdir().unwrap();
    let apk = b"apk bytes".to_vec();
    let base = serve_one(apk.clone());
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&["Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match]"]);
    let plan = plan_with(vec![Step::FetchApk, Step::InstallApk { replace: true }], Some(apk_art(&apk)), None);
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, false), &log).unwrap();
    assert_eq!(end.state, "unchanged");
    assert!(end.reason.contains("INSTALL_FAILED_UPDATE_INCOMPATIBLE"), "{}", end.reason);
    assert!(!dev.calls().iter().any(|c| c.starts_with("uninstall")), "{:?}", dev.calls());
    let r = records(log.path());
    assert!(r.iter().any(|x| x["event"] == "consent" && x["given"] == false));
}

#[test]
fn update_incompatible_with_consent_uninstalls_then_installs_and_logs_the_consent() {
    let d = tempfile::tempdir().unwrap();
    let apk = b"apk bytes".to_vec();
    let base = serve_one(apk.clone());
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&["Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]", "Success"]);
    let plan = plan_with(vec![Step::FetchApk, Step::InstallApk { replace: true }], Some(apk_art(&apk)), None);
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, true), &log).unwrap();
    assert_eq!(end.state, "installed", "{}", end.reason);
    let c = dev.calls();
    let order: Vec<&str> = c.iter().map(String::as_str).collect();
    assert_eq!(order, ["install replace=true", "uninstall com.readyatdawn.r15", "install replace=false"]);
    assert!(records(log.path()).iter().any(|x| x["event"] == "consent" && x["given"] == true));
}

#[test]
fn a_failed_install_after_the_uninstall_says_needs_user_because_the_package_changed() {
    let d = tempfile::tempdir().unwrap();
    let apk = b"apk bytes".to_vec();
    let base = serve_one(apk.clone());
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&["Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]", "Failure [INSTALL_FAILED_NO_MATCHING_ABIS]"]);
    let plan = plan_with(vec![Step::FetchApk, Step::InstallApk { replace: true }], Some(apk_art(&apk)), None);
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, true), &log).unwrap();
    assert_eq!(end.state, "needs-user");
    assert!(end.reason.contains("NO_MATCHING_ABIS"), "{}", end.reason);
}

#[test]
fn game_data_is_downloaded_verified_pushed_extracted_and_the_temp_zip_removed() {
    let d = tempfile::tempdir().unwrap();
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        z.start_file("_data/1/rad15/android/manifests/aa", zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(b"AAA").unwrap();
        z.finish().unwrap();
    }
    let zip_bytes = buf.into_inner();
    let base = serve_one(zip_bytes.clone());
    let data = Artifact {
        kind: "game-data".into(), platform: Some("android-arm64".into()), path: "data/echovr-data-aaaaaaaaaaaa.zip".into(),
        size: zip_bytes.len() as u64, sha256: sha256_hex(&zip_bytes), android: None, game_data: None, content_pack: None,
        contents: vec![Content { path: "_data/1/rad15/android/manifests/aa".into(), sha256: sha256_hex(b"AAA"), size: 3 }],
    };
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let plan = plan_with(vec![Step::FetchData, Step::ExtractData], None, Some(data.clone()));
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, false), &log).unwrap();
    assert_eq!(end.state, "installed", "{}", end.reason);
    let c = dev.calls();
    assert_eq!(c, ["push", "shell mkdir", "shell rm"], "push, extract under the media root, remove the temp zip");

    // A zip whose bytes do not match the manifest is never pushed.
    let base = serve_one(b"corrupt".to_vec());
    let dev = Fake::new(&[]);
    let log2 = Log::open(&d.path().join("l2.jsonl"), "quest").unwrap();
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, false), &log2).unwrap();
    assert_eq!(end.state, "unchanged");
    assert!(dev.calls().is_empty(), "{:?}", dev.calls());
}

#[test]
fn roots_hold_checks_every_path_at_its_size() {
    let data = Artifact {
        kind: "game-data".into(), platform: None, path: "data/x.zip".into(), size: 1, sha256: "0".repeat(64), android: None, game_data: None, content_pack: None,
        contents: vec![Content { path: "a/f".into(), sha256: "0".repeat(64), size: 3 }, Content { path: "a/g".into(), sha256: "0".repeat(64), size: 5 }],
    };
    let mut dev = Fake::new(&[]);
    dev.shell_replies = vec![("stat", format!("{r}/a/f 3\n{r}/a/g 5\n", r = quest::LEGACY_ROOT))];
    assert!(quest::roots_hold(&dev, quest::LEGACY_ROOT, &data).unwrap());
    assert!(!quest::roots_hold(&dev, quest::MEDIA_ROOT, &data).unwrap(), "paths under another root do not count");
    dev.shell_replies = vec![("stat", format!("{r}/a/f 3\n{r}/a/g 4\n", r = quest::LEGACY_ROOT))];
    assert!(!quest::roots_hold(&dev, quest::LEGACY_ROOT, &data).unwrap(), "a wrong size does not count");
    dev.shell_replies = vec![];
    assert!(!quest::roots_hold(&dev, quest::LEGACY_ROOT, &data).unwrap(), "nothing listed is nothing present");
}

#[test]
fn a_local_apk_that_changed_after_it_was_checked_is_never_installed() {
    let d = tempfile::tempdir().unwrap();
    let checked = b"the bytes the checks read".to_vec();
    let file = d.path().join("r15.apk");
    std::fs::write(&file, b"other bytes written afterwards").unwrap();
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let mut plan = plan_with(vec![Step::FetchApk, Step::CheckApk, Step::InstallApk { replace: false }], Some(apk_art(&checked)), None);
    plan.dev_artifact = Some(file);
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), "http://127.0.0.1:1", true), &log).unwrap();
    assert_eq!(end.state, "unchanged");
    assert!(end.reason.contains("changed after"), "{}", end.reason);
    assert!(!dev.calls().iter().any(|c| c.starts_with("install") || c.starts_with("uninstall")), "{:?}", dev.calls());
    let r = records(log.path());
    assert!(r.iter().filter(|x| x["event"] == "step_end").all(|x| x["trust"] == "unverified dev artifact"), "{r:?}");
}

// ---- what the headset is asked, and how long it has to answer ---------------------------------------------------------

const REMOTE_ZIP: &str = "/data/local/tmp/_nevr_data.zip";

/// A one-file game-data zip, its manifest entry, and a server that serves the zip.
fn game_data() -> (Artifact, String) {
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        z.start_file("_data/1/rad15/android/manifests/aa", zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(b"AAA").unwrap();
        z.finish().unwrap();
    }
    let zip_bytes = buf.into_inner();
    let base = serve_one(zip_bytes.clone());
    let data = Artifact {
        kind: "game-data".into(), platform: Some("android-arm64".into()), path: "data/echovr-data-aaaaaaaaaaaa.zip".into(),
        size: zip_bytes.len() as u64, sha256: sha256_hex(&zip_bytes), android: None, game_data: None, content_pack: None,
        contents: vec![Content { path: "_data/1/rad15/android/manifests/aa".into(), sha256: sha256_hex(b"AAA"), size: 3 }],
    };
    (data, base)
}

fn extract_plan(data: Artifact) -> Plan {
    plan_with(vec![Step::FetchData, Step::ExtractData], None, Some(data))
}

#[test]
fn the_extract_runs_unzip_under_the_media_root_then_removes_the_temp_zip_each_under_a_time_limit() {
    let d = tempfile::tempdir().unwrap();
    let (data, base) = game_data();
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &extract_plan(data), &opts(d.path(), &base, false), &log).unwrap();
    assert_eq!(end.state, "installed", "{}", end.reason);
    let media = quest::MEDIA_ROOT;
    assert_eq!(
        dev.shell_cmds(),
        [
            (format!("mkdir -p '{media}' && cd '{media}' && unzip -o '{REMOTE_ZIP}'"), 900),
            (format!("rm -f '{REMOTE_ZIP}'"), 60),
        ]
    );
}

#[test]
fn the_temp_zip_is_removed_when_the_unzip_fails_and_when_it_cannot_run_at_all() {
    let (data, base) = game_data();
    // The unzip exits non-zero.
    let d = tempfile::tempdir().unwrap();
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let mut dev = Fake::new(&[]);
    dev.shell_codes = vec![("unzip", 9)];
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &extract_plan(data.clone()), &opts(d.path(), &base, false), &log).unwrap();
    assert_eq!(end.state, "needs-user", "files may be partly extracted: the player is told");
    assert!(end.reason.contains("unzip on the headset failed"), "{}", end.reason);
    assert!(dev.shell_cmds().iter().any(|(c, _)| c == &format!("rm -f '{REMOTE_ZIP}'")), "{:?}", dev.shell_cmds());
    // The unzip hits its time limit (the call returns an error, not an answer).
    let d2 = tempfile::tempdir().unwrap();
    let log2 = Log::open(&d2.path().join("l.jsonl"), "quest").unwrap();
    let mut dev = Fake::new(&[]);
    dev.shell_errs = vec![("unzip", "adb shell timed out after 900 s".into())];
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &extract_plan(data), &opts(d2.path(), &base, false), &log2).unwrap();
    assert_eq!(end.state, "needs-user");
    assert!(end.reason.contains("timed out after 900 s"), "{}", end.reason);
    assert!(dev.shell_cmds().iter().any(|(c, _)| c == &format!("rm -f '{REMOTE_ZIP}'")), "{:?}", dev.shell_cmds());
}

#[test]
fn a_download_that_fails_is_an_error_that_touches_nothing_on_the_headset() {
    let d = tempfile::tempdir().unwrap();
    let apk = b"apk bytes".to_vec();
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let plan = plan_with(vec![Step::FetchApk, Step::CheckApk, Step::InstallApk { replace: false }], Some(apk_art(&apk)), None);
    // Nothing listens on port 1.
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), "http://127.0.0.1:1", true), &log).unwrap();
    assert_eq!(end.state, "unchanged");
    assert!(end.reason.contains("could not fetch"), "{}", end.reason);
    assert!(dev.calls().is_empty(), "{:?}", dev.calls());
    assert!(records(log.path()).iter().any(|x| x["event"] == "step_end" && x["step_id"] == "fetch-apk" && x["ok"] == false));
}

#[test]
fn an_apk_whose_bytes_are_not_the_manifests_is_rejected_removed_and_never_installed() {
    let d = tempfile::tempdir().unwrap();
    let listed = b"apk bytes".to_vec();
    let base = serve_one(b"APK BYTES".to_vec()); // same length, different content
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let plan = plan_with(vec![Step::FetchApk, Step::CheckApk, Step::InstallApk { replace: false }], Some(apk_art(&listed)), None);
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, true), &log).unwrap();
    assert_eq!(end.state, "unchanged");
    assert_eq!(end.reason, "FAIL step 8: sha256 differs from the manifest");
    assert!(dev.calls().is_empty(), "{:?}", dev.calls());
    let left: Vec<_> = std::fs::read_dir(d.path().join("work")).unwrap().flatten().map(|e| e.file_name()).collect();
    assert!(left.is_empty(), "the rejected download is deleted: {left:?}");
}

fn quiet_log(d: &Path) -> Log {
    Log::open(&d.join("l.jsonl"), "quest").unwrap()
}

const PM: &str = "pm path com.readyatdawn.r15";

#[test]
fn a_package_check_that_fails_is_an_error_with_the_exit_code_and_output_not_not_installed() {
    let d = tempfile::tempdir().unwrap();
    let mut dev = Fake::new(&[]);
    dev.shell_codes = vec![(PM, 7)];
    dev.shell_replies = vec![(PM, "Error: boom\n".into())];
    let e = quest::observe(&dev, None, &quiet_log(d.path())).unwrap_err();
    assert_eq!((e.step, e.msg.as_str()), (9, "package check failed (exit 7): Error: boom"));
}

#[test]
fn a_package_check_that_times_out_is_an_error_not_not_installed() {
    let d = tempfile::tempdir().unwrap();
    let mut dev = Fake::new(&[]);
    dev.shell_errs = vec![(PM, "adb -s Q shell pm path timed out after 30 s".into())];
    let e = quest::observe(&dev, None, &quiet_log(d.path())).unwrap_err();
    assert_eq!(e.step, 9);
    assert!(e.msg.contains("timed out after 30 s"), "{}", e.msg);
}

#[test]
fn a_headset_without_the_game_observes_no_package_and_one_with_it_observes_its_version() {
    let d = tempfile::tempdir().unwrap();
    // `pm path` exits 1 with no output when the package is absent.
    let mut dev = Fake::new(&[]);
    dev.shell_codes = vec![(PM, 1)];
    let seen = quest::observe(&dev, None, &quiet_log(d.path())).unwrap();
    assert_eq!(seen.installed, None);
    assert_eq!(seen.data, None, "no game-data artifact to compare against: presence is not observed");
    // Installed: the version comes from dumpsys, and the signer is unknown over adb.
    let mut dev = Fake::new(&[]);
    dev.shell_replies = vec![(PM, "package:/data/app/x/base.apk\n".into()), ("dumpsys package", "    versionCode=51602006 minSdk=29 targetSdk=29\n".into())];
    let seen = quest::observe(&dev, None, &quiet_log(d.path())).unwrap();
    assert_eq!(seen.installed, Some(quest::Installed { version_code: 51602006, signers: None }));
}

#[test]
fn the_observation_reads_free_space_and_gives_every_headset_call_a_time_limit() {
    let d = tempfile::tempdir().unwrap();
    let (data, _) = game_data();
    let mut dev = Fake::new(&[]);
    dev.shell_codes = vec![(PM, 1)];
    dev.shell_replies = vec![("df -Pk /sdcard", "Filesystem 1K-blocks Used Available Use% Mounted\nx 9 8 2306867 1% /sdcard\n".into())];
    let log = quiet_log(d.path());
    quest::observe(&dev, Some(&data), &log).unwrap();
    let cmds = dev.shell_cmds();
    let limit = |prefix: &str| -> Vec<u64> { cmds.iter().filter(|(c, _)| c.starts_with(prefix)).map(|x| x.1).collect() };
    assert_eq!(limit("pm path"), [30]);
    assert_eq!(limit("getprop"), [30, 30]);
    assert_eq!(limit("df -Pk /sdcard"), [30]);
    assert_eq!(limit("df -Pk /data/local/tmp"), [30]);
    assert_eq!(limit("for f in"), [120, 120], "the media root, then the legacy root");
    assert_eq!(cmds.len(), 7, "{cmds:?}");
    let device = records(log.path()).into_iter().find(|r| r["event"] == "device").expect("device record");
    assert_eq!(device["free_kb_sdcard"], 2306867, "{device}");
    assert_eq!(device["serial_fp"], "fake");
}

#[test]
fn a_push_that_fails_leaves_no_temp_zip_on_the_headset() {
    let (data, base) = game_data();
    let d = tempfile::tempdir().unwrap();
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let mut dev = Fake::new(&[]);
    dev.push_fails = Some("adb: error: failed to copy");
    let end = quest::execute(&dev, &serde_json::json!({}), "1.0.0", &extract_plan(data), &opts(d.path(), &base, false), &log).unwrap();
    assert_eq!(end.state, "unchanged", "{}", end.reason);
    assert!(end.reason.contains("adb push failed"), "{}", end.reason);
    assert!(dev.shell_cmds().iter().any(|(c, _)| c == &format!("rm -f '{REMOTE_ZIP}'")), "a partial copy of the zip stays: {:?}", dev.shell_cmds());
    assert!(!dev.shell_cmds().iter().any(|(c, _)| c.contains("unzip")), "nothing is unpacked after a failed push");
}

/// A `file://` base whose folder name carries a query (#1353): `bytes` sit at `rel` under that folder.
fn file_base_with_query(d: &Path, rel: &str, bytes: &[u8]) -> String {
    let folder = d.join("base?token=tok-9f3ab1");
    let file = folder.join(rel);
    std::fs::create_dir_all(file.parent().unwrap()).unwrap();
    std::fs::write(&file, bytes).unwrap();
    format!("file://{}", folder.display())
}

#[test]
fn an_apk_fetched_from_a_base_with_a_query_is_logged_without_it() {
    let d = tempfile::tempdir().unwrap();
    let apk = b"quest-apk-bytes".to_vec();
    let art = apk_art(&apk);
    let base = file_base_with_query(d.path(), &art.path, &apk);
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let plan = plan_with(vec![Step::FetchApk], Some(art), None);
    quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, true), &log).unwrap();
    let text = std::fs::read_to_string(d.path().join("l.jsonl")).unwrap();
    let fetched = records(&d.path().join("l.jsonl")).into_iter().find(|r| r["event"] == "artifact_fetch" && r["kind"] == "quest-apk").expect("the APK fetch is logged");
    assert_eq!(fetched["ok"], true, "the success line is the one under test: {fetched}");
    assert!(!text.contains("tok-9f3ab1"), "the base's query was written down: {text}");
}

#[test]
fn game_data_fetched_from_a_base_with_a_query_is_logged_without_it() {
    let d = tempfile::tempdir().unwrap();
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        z.start_file("_data/1/rad15/android/manifests/aa", zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(b"AAA").unwrap();
        z.finish().unwrap();
    }
    let zip_bytes = buf.into_inner();
    let data = Artifact {
        kind: "game-data".into(), platform: Some("android-arm64".into()), path: "data/echovr-data-aaaaaaaaaaaa.zip".into(),
        size: zip_bytes.len() as u64, sha256: sha256_hex(&zip_bytes), android: None, game_data: None, content_pack: None,
        contents: vec![Content { path: "_data/1/rad15/android/manifests/aa".into(), sha256: sha256_hex(b"AAA"), size: 3 }],
    };
    let base = file_base_with_query(d.path(), &data.path, &zip_bytes);
    let log = Log::open(&d.path().join("l.jsonl"), "quest").unwrap();
    let dev = Fake::new(&[]);
    let plan = plan_with(vec![Step::FetchData], None, Some(data));
    quest::execute(&dev, &serde_json::json!({}), "1.0.0", &plan, &opts(d.path(), &base, false), &log).unwrap();
    let text = std::fs::read_to_string(d.path().join("l.jsonl")).unwrap();
    let fetched = records(&d.path().join("l.jsonl")).into_iter().find(|r| r["event"] == "artifact_fetch" && r["kind"] == "game-data").expect("the game-data fetch is logged");
    assert_eq!(fetched["ok"], true, "the success line is the one under test: {fetched}");
    assert!(!text.contains("tok-9f3ab1"), "the base's query was written down: {text}");
}
