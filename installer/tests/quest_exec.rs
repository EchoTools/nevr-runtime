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
}

impl Fake {
    fn new(install_results: &[&str]) -> Fake {
        Fake { calls: RefCell::new(vec![]), install_results: RefCell::new(install_results.iter().map(|s| s.to_string()).collect()), shell_replies: vec![] }
    }
    fn calls(&self) -> Vec<String> {
        self.calls.borrow().clone()
    }
}

fn out(text: &str) -> Output {
    Output { code: if text.contains("Success") || !text.contains("Failure") { 0 } else { 1 }, text: text.to_string() }
}

impl Device for Fake {
    fn fingerprint(&self) -> String {
        "fake".into()
    }
    fn shell(&self, cmd: &str, _t: u64) -> Result<Output, String> {
        self.calls.borrow_mut().push(format!("shell {}", cmd.split_whitespace().next().unwrap_or("")));
        for (needle, reply) in &self.shell_replies {
            if cmd.contains(needle) {
                return Ok(Output { code: 0, text: reply.clone() });
            }
        }
        Ok(Output { code: 0, text: String::new() })
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
        Ok(out("1 file pushed"))
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
        game_data: None, contents: vec![],
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
        size: zip_bytes.len() as u64, sha256: sha256_hex(&zip_bytes), android: None, game_data: None,
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
        kind: "game-data".into(), platform: None, path: "data/x.zip".into(), size: 1, sha256: "0".repeat(64), android: None, game_data: None,
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
