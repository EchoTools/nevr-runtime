//! The Quest plan: one test per branch of the diagram in ADR 0023 section 3, and the execution over a fake headset.

use nevr_install::quest::{self, DataRoots, Installed, Observed, Outcome, Step};
use serde_json::{json, Value};

const SIGNER: &str = "bc4d88e4c523f2516de3c32be63084a4fb0fe8f368d773f71b3d4bdf45b8517a";
const OTHER: &str = "1111111111111111111111111111111111111111111111111111111111111111";
const H: &str = "2222222222222222222222222222222222222222222222222222222222222222";

fn release(with_data: bool, signer: Value, code: u64) -> Value {
    let mut arts = vec![json!({"kind": "quest-apk", "platform": "android-arm64", "path": "v1.0.0/nevr-runtime-v1.0.0-quest.apk", "size": 10, "sha256": H,
        "android": {"package": "com.readyatdawn.r15", "version_code": code, "signer_cert_sha256": signer}})];
    if with_data {
        arts.push(json!({"kind": "game-data", "platform": "android-arm64", "path": "data/echovr-data-aaaaaaaaaaaa.zip", "size": 20, "sha256": H,
            "game_data": {"data_version": "1", "echo_build": "2"}, "contents": [{"path": "_data/1/rad15/android/manifests/aa", "sha256": H, "size": 3}]}));
    }
    json!({"releases": {"1.0.0": {"artifacts": arts}}})
}

fn obs(code: Option<u64>, signers: Option<Vec<&str>>, data: Option<(bool, bool)>) -> Observed {
    Observed {
        installed: code.map(|c| Installed { version_code: c, signers: signers.map(|s| s.into_iter().map(String::from).collect()) }),
        data: data.map(|(legacy, media)| DataRoots { legacy, media }),
        free: None,
    }
}

fn names(p: &quest::Plan) -> Vec<&'static str> {
    p.steps.iter().map(Step::name).collect()
}

#[test]
fn absent_installs_the_apk_and_the_data() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(None, None, Some((false, false))), false);
    assert_eq!(p.outcome, Outcome::Proceed);
    assert_eq!(p.decision, "install");
    assert_eq!(names(&p), ["fetch-apk", "check-apk", "install-apk", "fetch-data", "extract-data", "verify-installed"]);
}

#[test]
fn same_signer_lower_version_updates_in_place() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(Some(4), Some(vec![SIGNER]), Some((false, true))), false);
    assert_eq!((p.outcome, p.decision.as_str()), (Outcome::Proceed, "update"));
    assert_eq!(names(&p), ["fetch-apk", "check-apk", "install-apk-replace", "verify-installed"]);
}

#[test]
fn same_signer_same_version_with_data_present_is_unchanged() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(Some(5), Some(vec![SIGNER]), Some((false, true))), false);
    assert_eq!((p.outcome, p.decision.as_str()), (Outcome::Unchanged, "unchanged"));
    assert!(p.steps.is_empty());
}

#[test]
fn same_signer_higher_version_is_never_a_downgrade() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(Some(6), Some(vec![SIGNER]), Some((false, true))), false);
    assert_eq!((p.outcome, p.decision.as_str()), (Outcome::NeedsUser, "downgrade-refused"));
    assert!(p.steps.is_empty());
}

#[test]
fn a_different_signer_without_consent_needs_the_player_and_lists_what_an_uninstall_deletes() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(Some(4), Some(vec![OTHER]), Some((true, false))), false);
    assert_eq!((p.outcome, p.decision.as_str()), (Outcome::NeedsUser, "signer-mismatch"));
    assert!(!names(&p).contains(&"uninstall"));
    assert!(p.consequences.iter().any(|c| c.contains("/data/data/com.readyatdawn.r15")));
    assert!(p.consequences.iter().any(|c| c.contains("OBB")));
}

#[test]
fn a_different_signer_with_consent_uninstalls_first() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(Some(4), Some(vec![OTHER]), Some((true, false))), true);
    assert_eq!(p.outcome, Outcome::Proceed);
    assert_eq!(names(&p), ["fetch-apk", "check-apk", "uninstall", "install-apk", "verify-installed"]);
}

#[test]
fn no_signer_in_the_manifest_is_a_refusal() {
    for signer in [Value::Null, json!([])] {
        let p = quest::plan(&release(true, signer, 5), "1.0.0", &obs(None, None, None), true);
        assert_eq!(p.outcome, Outcome::Unchanged);
        assert!(p.reasons[0].contains("signer_cert_sha256"), "{:?}", p.reasons);
        assert!(p.steps.is_empty());
    }
}

#[test]
fn an_unreadable_signer_never_plans_an_uninstall_and_says_so() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(Some(4), None, Some((false, true))), true);
    assert_eq!(p.signer, "unknown-until-install");
    assert!(!names(&p).contains(&"uninstall"));
    assert_eq!(names(&p), ["fetch-apk", "check-apk", "install-apk-replace", "verify-installed"]);
}

#[test]
fn data_in_the_legacy_root_counts_as_present() {
    let p = quest::plan(&release(true, json!(SIGNER), 5), "1.0.0", &obs(Some(5), Some(vec![SIGNER]), Some((true, false))), false);
    assert_eq!(p.outcome, Outcome::Unchanged);
    assert!(p.game_data.contains(quest::LEGACY_ROOT), "{}", p.game_data);
}

#[test]
fn a_release_without_game_data_and_no_data_on_the_device_needs_the_player_and_says_none_in_release() {
    let p = quest::plan(&release(false, json!(SIGNER), 5), "1.0.0", &obs(None, None, Some((false, false))), false);
    assert_eq!(p.outcome, Outcome::NeedsUser);
    assert!(p.game_data.contains("game-data: none in release 1.0.0"), "{}", p.game_data);
    assert!(p.steps.is_empty(), "nothing is installed from a release that cannot be completed");
}

#[test]
fn a_release_without_game_data_is_fine_when_the_device_already_has_it() {
    let p = quest::plan(&release(false, json!(SIGNER), 5), "1.0.0", &obs(None, None, Some((true, false))), false);
    assert_eq!(p.outcome, Outcome::Proceed);
    assert_eq!(names(&p), ["fetch-apk", "check-apk", "install-apk", "verify-installed"]);
}

#[test]
fn a_wrong_package_is_refused() {
    let mut r = release(true, json!(SIGNER), 5);
    r["releases"]["1.0.0"]["artifacts"][0]["android"]["package"] = json!("com.example.other");
    let p = quest::plan(&r, "1.0.0", &obs(None, None, None), true);
    assert_eq!(p.outcome, Outcome::Unchanged);
    assert!(p.reasons[0].contains("com.example.other"));
}

#[test]
fn badging_and_certificate_output_is_parsed() {
    let b = "package: name='com.readyatdawn.r15' versionCode='51601999' versionName='5.16.1' platformBuildVersionName='10'\nsdkVersion:'29'\n";
    assert_eq!(quest::parse_badging(b), Some(("com.readyatdawn.r15".to_string(), 51601999)));
    assert_eq!(quest::parse_badging("nothing"), None);
    let c = format!("Signer #1 certificate DN: CN=x\nSigner #1 certificate SHA-256 digest: {SIGNER}\nSigner #1 certificate SHA-256 digest: {SIGNER}\nSigner #1 certificate SHA-256 digest: nothex\n");
    assert_eq!(quest::parse_certs(&c), [SIGNER]);
}

#[test]
fn the_newest_listed_game_data_is_found_across_releases() {
    let art = |code: u64, data: bool| {
        let mut r = release(data, json!(SIGNER), code);
        r["releases"]["1.0.0"].take()
    };
    let m = json!({"releases": {"5.10.0": art(1, true), "5.16.1": art(2, false), "5.9.0": art(3, true), "junk": art(4, true)}});
    let (v, a) = nevr_install::manifest::latest_game_data(&m).unwrap();
    assert_eq!(v, "5.10.0", "5.16.1 lists none; 5.10.0 sorts above 5.9.0 by number, not by text");
    assert_eq!(a.kind, "game-data");
    assert!(nevr_install::manifest::latest_game_data(&json!({"releases": {"1.0.0": art(1, false)}})).is_none());
}

fn with_free(sdcard_kb: Option<u64>, tmp_kb: Option<u64>) -> Observed {
    Observed { free: Some(quest::FreeSpace { sdcard_kb, tmp_kb }), ..obs(None, None, Some((false, false))) }
}

#[test]
fn the_game_data_needs_exactly_2_2_gib_free_in_both_places() {
    let m = release(true, json!(SIGNER), 5);
    let ok = quest::plan(&m, "1.0.0", &with_free(Some(quest::MIN_FREE_KB), Some(quest::MIN_FREE_KB)), false);
    assert_eq!((ok.outcome, ok.decision.as_str()), (Outcome::Proceed, "install"), "{:?}", ok.reasons);
    assert_eq!(quest::MIN_FREE_KB, 2_306_867);
    for (s, t, place) in [(quest::MIN_FREE_KB - 1, quest::MIN_FREE_KB, "/sdcard"), (quest::MIN_FREE_KB, quest::MIN_FREE_KB - 1, "/data/local/tmp")] {
        let p = quest::plan(&m, "1.0.0", &with_free(Some(s), Some(t)), false);
        assert_eq!((p.outcome, p.decision.as_str()), (Outcome::Unchanged, "refused"), "{place}");
        assert!(p.reasons[0].starts_with(place) && p.reasons[0].contains("need 2306867 KiB"), "{:?}", p.reasons);
        assert!(p.steps.is_empty());
    }
}

#[test]
fn free_space_that_cannot_be_read_is_a_refusal_and_no_device_means_no_check() {
    let m = release(true, json!(SIGNER), 5);
    let p = quest::plan(&m, "1.0.0", &with_free(Some(quest::MIN_FREE_KB), None), false);
    assert_eq!(p.decision, "refused");
    assert!(p.reasons[0].contains("/data/local/tmp could not be read") || p.reasons[0].contains("free space on /data/local/tmp could not be read"), "{:?}", p.reasons);
    let none = quest::plan(&m, "1.0.0", &obs(None, None, Some((false, false))), false);
    assert_eq!(none.outcome, Outcome::Proceed, "free space not observed: no check");
}

#[test]
fn free_space_is_not_needed_when_the_data_is_already_there() {
    let m = release(true, json!(SIGNER), 5);
    let o = Observed { free: Some(quest::FreeSpace { sdcard_kb: Some(10), tmp_kb: Some(10) }), ..obs(Some(4), Some(vec![SIGNER]), Some((false, true))) };
    let p = quest::plan(&m, "1.0.0", &o, false);
    assert_eq!((p.outcome, p.decision.as_str()), (Outcome::Proceed, "update"));
}
