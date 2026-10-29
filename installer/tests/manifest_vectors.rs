//! Every row of the "Test vectors" table of docs/reference/verifying-a-release.md, steps 1 to 7.

mod common;
use common::*;
use nevr_install::manifest::{self, State};
use nevr_install::util::sha256_hex;

fn ok_with(m: &[u8], s: &[u8], state: &State, now: &str, min: u64) -> nevr_install::error::Result<manifest::Verified> {
    manifest::verify(m, s, &test_pins(), state, now, min)
}

fn step_and(r: nevr_install::error::Result<manifest::Verified>) -> (u32, String) {
    let e = r.err().expect("expected a failure");
    (e.step, e.msg)
}

#[test]
fn good_passes_steps_2_to_5_with_the_test_key() {
    let (m, s) = fixture("good");
    let v = ok_with(&m, &s, &State::default(), NOW, 1).unwrap();
    assert_eq!(v.key_id, TEST_KEY_ID);
    assert_eq!(v.version, 1);
    assert_eq!(v.new_state.highest, 1);
    assert_eq!(v.new_state.last_sha256, sha256_hex(&m));
}

#[test]
fn good_is_refused_with_the_real_pins() {
    let (m, s) = fixture("good");
    let (step, msg) = step_and(manifest::verify(&m, &s, &manifest::Pins::builtin(), &State::default(), NOW, 1));
    assert_eq!(step, 2);
    assert!(msg.contains("is not pinned"), "{msg}");
}

#[test]
fn the_builtin_pins_are_the_three_documented_keys() {
    assert_eq!(manifest::Pins::builtin().ids(), ["c0ee9c8547023d34", "3e72af45f3a77255", "410cace100e5977f"]);
}

#[test]
fn legacy_ed_is_refused() {
    let (m, s) = fixture("legacy-ed");
    let (step, msg) = step_and(ok_with(&m, &s, &State::default(), NOW, 1));
    assert_eq!((step, msg.as_str()), (2, "signature algorithm is not ED (hashed)"));
}

#[test]
fn unpinned_key_is_refused() {
    let (m, s) = fixture("unpinned-key");
    let (step, msg) = step_and(ok_with(&m, &s, &State::default(), NOW, 1));
    assert_eq!(step, 2);
    assert!(msg.contains("is not pinned"), "{msg}");
}

#[test]
fn same_version_other_bytes_is_refused_when_the_stored_hash_differs() {
    let (good, _) = fixture("good");
    let (m, s) = fixture("same-version-other-bytes");
    let state = State { highest: 1, revoked: vec![], last_sha256: sha256_hex(&good) };
    let (step, msg) = step_and(ok_with(&m, &s, &state, NOW, 1));
    assert_eq!((step, msg.as_str()), (4, "same manifest_version, different bytes"));
}

#[test]
fn the_same_bytes_at_the_same_version_pass() {
    let (good, s) = fixture("good");
    let state = State { highest: 1, revoked: vec![], last_sha256: sha256_hex(&good) };
    ok_with(&good, &s, &state, NOW, 1).unwrap();
}

#[test]
fn one_byte_of_the_manifest_changed() {
    let (mut m, s) = fixture("good");
    m[100] = b'X';
    assert_eq!(step_and(ok_with(&m, &s, &State::default(), NOW, 1)).0, 2);
}

#[test]
fn one_byte_of_an_artifact_entry_changed() {
    let (m, s) = fixture("good");
    let t = String::from_utf8(m).unwrap().replacen("\"size\": 4914229", "\"size\": 4914228", 1);
    assert_eq!(step_and(ok_with(t.as_bytes(), &s, &State::default(), NOW, 1)).0, 2);
}

#[test]
fn one_byte_of_the_trusted_comment_changed() {
    let (m, s) = fixture("good");
    let t = String::from_utf8(s).unwrap().replace("manifest_version=1", "manifest_version=2");
    assert_eq!(step_and(ok_with(&m, t.as_bytes(), &State::default(), NOW, 1)).0, 2);
}

#[test]
fn a_lower_manifest_version_than_stored() {
    let (m, s) = fixture("good");
    let state = State { highest: 2, ..State::default() };
    let (step, msg) = step_and(ok_with(&m, &s, &state, NOW, 1));
    assert_eq!((step, msg.as_str()), (4, "manifest version went down: got 1, have 2"));
}

#[test]
fn a_fresh_install_built_after_the_manifest() {
    let (m, s) = fixture("good");
    let (step, msg) = step_and(ok_with(&m, &s, &State::default(), NOW, 2));
    assert_eq!((step, msg.as_str()), (4, "manifest version went down: got 1, have 2"));
}

#[test]
fn an_expired_manifest() {
    let (m, s) = fixture("good");
    let (step, msg) = step_and(ok_with(&m, &s, &State::default(), "2026-11-08T00:00:00Z", 1));
    assert_eq!(step, 5);
    assert!(msg.contains("stale since 2026-11-07T18:00:00Z"), "{msg}");
}

#[test]
fn a_clock_reading_that_is_not_a_time() {
    let (m, s) = fixture("good");
    for bad in ["", "yesterday", "2026-10-26T12:00:00", "2026-10-26T12:00:00Z\n"] {
        let (step, msg) = step_and(ok_with(&m, &s, &State::default(), bad, 1));
        assert_eq!(step, 5, "{bad:?}");
        assert!(msg.contains("the current time is not UTC"), "{msg}");
    }
}

#[test]
fn a_revoked_key() {
    let (m, s) = fixture("good");
    let state = State { revoked: vec![TEST_KEY_ID.into()], ..State::default() };
    let (step, msg) = step_and(ok_with(&m, &s, &state, NOW, 1));
    assert_eq!((step, msg), (2, format!("signing key {TEST_KEY_ID} is revoked")));
}

#[test]
fn a_state_file_that_cannot_be_read() {
    let d = tempfile::tempdir().unwrap();
    std::fs::write(d.path().join("state.json"), b"not json").unwrap();
    assert_eq!(manifest::read_state(d.path()).unwrap_err().step, 6);
    let d2 = tempfile::tempdir().unwrap();
    std::fs::create_dir(d2.path().join("state.json")).unwrap();
    assert_eq!(manifest::read_state(d2.path()).unwrap_err().step, 6);
    let d3 = tempfile::tempdir().unwrap();
    std::fs::write(d3.path().join("state.json"), br#"{"revoked_keys": "abc"}"#).unwrap();
    assert_eq!(manifest::read_state(d3.path()).unwrap_err().step, 6);
    // A missing file is a first run, not an error.
    assert_eq!(manifest::read_state(&d3.path().join("nowhere")).unwrap(), State::default());
}

#[test]
fn oversize_inputs_are_refused_at_step_1() {
    let (m, s) = fixture("good");
    assert_eq!(step_and(ok_with(&vec![b' '; (1 << 20) + 1], &s, &State::default(), NOW, 1)).0, 1);
    assert_eq!(step_and(ok_with(&m, &vec![b' '; 1025], &State::default(), NOW, 1)).0, 1);
}

#[test]
fn a_revocation_list_that_is_not_a_list_of_strings() {
    let d = tempfile::tempdir().unwrap();
    let k = make_key(d.path(), "a");
    let pins = manifest::Pins::parse(&format!("A {}\n", k.b64)).unwrap();
    for bad in [serde_json::json!("abc"), serde_json::json!([1])] {
        let m = manifest_json(1, serde_json::json!({"revoked_keys": bad}));
        let s = sign(d.path(), &k, &m, false);
        let e = manifest::verify(&m, &s, &pins, &State::default(), NOW, 1).unwrap_err();
        assert_eq!(e.step, 6, "{bad}");
        assert!(e.msg.contains("revoked_keys"), "{}", e.msg);
    }
}

#[test]
fn a_member_of_the_wrong_type_is_a_step_failure_never_a_panic() {
    let d = tempfile::tempdir().unwrap();
    let k = make_key(d.path(), "a");
    let pins = manifest::Pins::parse(&format!("A {}\n", k.b64)).unwrap();
    for (extra, step) in [
        (serde_json::json!({"product": 5}), 3),
        (serde_json::json!({"manifest_version": "1"}), 3),
        (serde_json::json!({"schema_version": 2}), 3),
        (serde_json::json!({"expires": 5}), 5),
    ] {
        let m = manifest_json(1, extra.clone());
        let s = sign(d.path(), &k, &m, false);
        let e = manifest::verify(&m, &s, &pins, &State::default(), NOW, 1).unwrap_err();
        assert_eq!(e.step, step, "{extra}");
    }
    // channels and releases of the wrong type fail at step 7.
    for extra in [serde_json::json!({"channels": []}), serde_json::json!({"releases": "x"})] {
        let m = manifest_json(1, extra.clone());
        let s = sign(d.path(), &k, &m, false);
        let v = manifest::verify(&m, &s, &pins, &State::default(), NOW, 1).unwrap();
        assert_eq!(manifest::pick(&v.value, "beta").unwrap_err().step, 7, "{extra}");
    }
}

#[test]
fn a_signature_made_in_legacy_mode_by_a_pinned_key_is_refused() {
    let d = tempfile::tempdir().unwrap();
    let k = make_key(d.path(), "a");
    let pins = manifest::Pins::parse(&format!("A {}\n", k.b64)).unwrap();
    let m = manifest_json(1, serde_json::json!({}));
    let s = sign(d.path(), &k, &m, true);
    let e = manifest::verify(&m, &s, &pins, &State::default(), NOW, 1).unwrap_err();
    assert_eq!((e.step, e.msg.as_str()), (2, "signature algorithm is not ED (hashed)"));
    // And the same key in the hashed mode passes: the refusal is about the mode.
    let s = sign(d.path(), &k, &m, false);
    manifest::verify(&m, &s, &pins, &State::default(), NOW, 1).unwrap();
}

#[test]
fn key_compromise_recovery() {
    let d = tempfile::tempdir().unwrap();
    let (a, b) = (make_key(d.path(), "a"), make_key(d.path(), "b"));
    let pins = manifest::Pins::parse(&format!("A {}\nB {}\n", a.b64, b.b64)).unwrap();
    // A signs manifest 1; accepted, stored.
    let m1 = manifest_json(1, serde_json::json!({}));
    let v1 = manifest::verify(&m1, &sign(d.path(), &a, &m1, false), &pins, &State::default(), NOW, 1).unwrap();
    // B signs a recovery manifest at a higher version that revokes A.
    let m2 = manifest_json(2, serde_json::json!({"revoked_keys": [key_id(&a.b64)]}));
    let v2 = manifest::verify(&m2, &sign(d.path(), &b, &m2, false), &pins, &v1.new_state, NOW, 1).unwrap();
    assert_eq!(v2.new_state.revoked, [key_id(&a.b64)]);
    // One more signed by A is refused at step 2, whatever its version.
    let m3 = manifest_json(9, serde_json::json!({}));
    let e = manifest::verify(&m3, &sign(d.path(), &a, &m3, false), &pins, &v2.new_state, NOW, 1).unwrap_err();
    assert_eq!(e.step, 2);
    assert!(e.msg.contains("is revoked"), "{}", e.msg);
}

#[test]
fn step_6_stores_the_manifest_and_the_state_last_and_step_8_reads_it_back() {
    let (m, s) = fixture("good");
    let v = ok_with(&m, &s, &State::default(), NOW, 1).unwrap();
    let d = tempfile::tempdir().unwrap();
    manifest::commit(d.path(), &v).unwrap();
    assert_eq!(std::fs::read(d.path().join("manifest.json")).unwrap(), m);
    assert_eq!(manifest::read_state(d.path()).unwrap().last_sha256, sha256_hex(&m));
    assert_eq!(manifest::pick(&manifest::stored(d.path()).unwrap(), "beta").unwrap(), "5.5.1");
    // A manifest nobody verified: edited after step 6.
    std::fs::write(d.path().join("manifest.json"), b"{}").unwrap();
    let e = manifest::stored(d.path()).unwrap_err();
    assert_eq!((e.step, e.msg.as_str()), (8, "the stored manifest is not the one that passed step 2"));
}

#[test]
fn step_7_channel_rules() {
    let (m, s) = fixture("good");
    let v = ok_with(&m, &s, &State::default(), NOW, 1).unwrap();
    assert_eq!(manifest::pick(&v.value, "beta").unwrap(), "5.5.1");
    assert_eq!(manifest::pick(&v.value, "stable").unwrap_err().msg, "channel stable has no release");
}

#[test]
fn step_8_entry_shape() {
    let (m, s) = fixture("good");
    let v = ok_with(&m, &s, &State::default(), NOW, 1).unwrap();
    let a = manifest::find(&v.value, "5.5.1", "windows-zip", None).unwrap();
    assert_eq!(a.path, "v5.5.1/nevr-runtime-v5.5.1-windows.zip");
    assert_eq!(manifest::find(&v.value, "5.5.1", "game-data", None).unwrap_err().step, 8);
    assert_eq!(manifest::find(&v.value, "5.5.2", "windows-zip", None).unwrap_err().step, 8);
    // A path that names another release is refused before any request.
    let bad = String::from_utf8(m_bytes(&v)).unwrap().replace("v5.5.1/nevr-runtime-v5.5.1-windows.zip", "v5.5.2/nevr-runtime-v5.5.1-windows.zip");
    let val: serde_json::Value = serde_json::from_str(&bad).unwrap();
    assert_eq!(manifest::find(&val, "5.5.1", "windows-zip", None).unwrap_err().step, 8);
}

fn m_bytes(v: &manifest::Verified) -> Vec<u8> {
    v.bytes.clone()
}
