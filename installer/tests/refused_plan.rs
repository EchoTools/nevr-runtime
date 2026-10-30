//! A refused Quest plan is a failed request whether it only plans or installs (#1353): its exit code says so.

mod common;
use common::*;
use nevr_install::manifest::Pins;
use nevr_install::session::{Config, Session};

#[test]
fn a_refused_quest_plan_exits_non_zero_as_an_install_does() {
    let d = tempfile::tempdir().unwrap();
    let k = make_key(d.path(), "k");
    // A quest-apk entry with no signer_cert_sha256: the plan refuses, because there is nothing to check the signer against.
    let m = manifest_json(
        5,
        serde_json::json!({"releases": {"1.0.0": {"artifacts": [{"kind": "quest-apk", "platform": "android-arm64",
            "path": "v1.0.0/nevr-runtime-v1.0.0-quest.apk", "size": 10, "sha256": "2".repeat(64),
            "android": {"package": "com.readyatdawn.r15", "version_code": 5}}]}}}),
    );
    let pair = d.path().join("pair");
    std::fs::create_dir_all(&pair).unwrap();
    std::fs::write(pair.join("manifest.json"), &m).unwrap();
    std::fs::write(pair.join("manifest.json.sig"), sign(d.path(), &k, &m, false)).unwrap();
    let mut cfg = Config::new("quest", &d.path().join("root"));
    cfg.pins = Pins::parse(&format!("T {}\n", k.b64)).unwrap();
    cfg.now = Some(NOW.into());
    let s = Session::open(cfg, "test").unwrap();
    s.manifest_from_dir(&pair).unwrap();
    let (ended, report) = s.quest(Some("1.0.0"), None, false, false, None).unwrap();
    assert_eq!(report["decision"], "refused", "{report}");
    assert_eq!(ended.state, "unchanged");
    assert_eq!(ended.exit_code(), 1, "a refused plan exited 0 (reason: {})", ended.reason);
}
