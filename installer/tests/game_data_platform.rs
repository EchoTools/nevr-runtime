//! A release lists one `game-data` artifact per platform (`android-arm64` and `windows-x86_64`). `NevrArtifactVerifyZip`
//! takes the platform like `NevrArtifactFetch` does, and refuses a `game-data` call that names none: the kind lists one
//! entry per platform, so "the" game-data artifact is not defined. Every other kind keeps its rule.

mod common;
use common::*;
use nevr_install::ffi::{NevrArtifactVerifyZip, NevrClose, NevrInstallOpen, NevrLastError, NevrManifestVerify, NevrSession, NEVR_FAILED, NEVR_OK};
use nevr_install::manifest;
use nevr_install::util::sha256_hex;
use serde_json::{json, Value};
use std::ffi::{CStr, CString};
use std::io::Write;
use std::path::Path;

const ANDROID_NAME: &str = "echovr-data-aaaaaaaaaaaa.zip";
const PC_NAME: &str = "echovr-data-win10-0123456789ab.zip";
const ANDROID_MEMBER: (&str, &[u8]) = ("rad15/android/manifests/m1", b"android manifest");
const PC_MEMBER: (&str, &[u8]) = ("_data/5932408047/rad15/win10/manifests/m1", b"pc manifest");

fn zip_of(path: &Path, member: (&str, &[u8])) {
    let mut z = zip::ZipWriter::new(std::fs::File::create(path).unwrap());
    z.start_file(member.0, zip::write::SimpleFileOptions::default()).unwrap();
    z.write_all(member.1).unwrap();
    z.finish().unwrap();
}

fn data_entry(name: &str, platform: &str, member: (&str, &[u8])) -> Value {
    json!({"kind": "game-data", "platform": platform, "path": format!("data/{name}"), "size": 100, "sha256": "3".repeat(64),
        "game_data": {"data_version": "5932408047", "echo_build": ""},
        "contents": [{"path": member.0, "size": member.1.len(), "sha256": sha256_hex(member.1)}]})
}

fn windows_entry(version: &str, name: &str) -> Value {
    json!({"kind": "windows-zip", "platform": "windows-x86_64", "path": format!("v{version}/nevr-runtime-v{version}-{name}"), "size": 100, "sha256": "4".repeat(64),
        "contents": [{"path": ANDROID_MEMBER.0, "size": ANDROID_MEMBER.1.len(), "sha256": sha256_hex(ANDROID_MEMBER.1)}]})
}

/// The signed manifest, as the bytes and the signature: release 1.0.0 lists the PC game data first, the Android one
/// second, and two Windows zips; release 1.0.1 lists one Windows zip.
fn signed_manifest(dir: &Path) -> (String, Vec<u8>, Vec<u8>) {
    let k = make_key(dir, "k");
    let one_windows = windows_entry("1.0.1", "windows.zip");
    let m = manifest_json(
        5,
        json!({"channels": {"beta": "1.0.1"}, "releases": {
            "1.0.0": {"artifacts": [data_entry(PC_NAME, "windows-x86_64", PC_MEMBER), data_entry(ANDROID_NAME, "android-arm64", ANDROID_MEMBER),
                windows_entry("1.0.0", "one.zip"), windows_entry("1.0.0", "two.zip")]},
            "1.0.1": {"artifacts": [one_windows]}}}),
    );
    let sig = sign(dir, &k, &m, false);
    (format!("T {}\n", k.b64), m, sig)
}

struct Fixture {
    dir: tempfile::TempDir,
    sess: *mut NevrSession,
}

impl Fixture {
    fn new() -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let (pins, m, sig) = signed_manifest(dir.path());
        zip_of(&dir.path().join("android.zip"), ANDROID_MEMBER);
        zip_of(&dir.path().join("pc.zip"), PC_MEMBER);
        let cfg = CString::new(json!({"platform": "pc", "root": dir.path().join("root"), "pins": pins, "now": NOW}).to_string()).unwrap();
        let mut sess: *mut NevrSession = std::ptr::null_mut();
        // SAFETY: `cfg` is NUL-terminated, `sess` is writable, the manifest and signature slices outlive the call.
        unsafe {
            assert_eq!(NevrInstallOpen(cfg.as_ptr(), &mut sess), NEVR_OK);
            assert_eq!(NevrManifestVerify(sess, m.as_ptr(), m.len(), sig.as_ptr(), sig.len()), NEVR_OK, "{}", last_error(sess));
        }
        Fixture { dir, sess }
    }

    /// `NevrArtifactVerifyZip` over `zip` in the fixture directory; `platform` None passes a null pointer.
    fn verify(&self, version: &str, kind: &str, platform: Option<&str>, zip: &str, allow_dirs: i32) -> (i32, String) {
        let (version, kind, zip) = (CString::new(version).unwrap(), CString::new(kind).unwrap(), CString::new(self.dir.path().join(zip).to_str().unwrap()).unwrap());
        let platform = platform.map(|p| CString::new(p).unwrap());
        let ptr = platform.as_ref().map_or(std::ptr::null(), |p| p.as_ptr());
        // SAFETY: every string is NUL-terminated, `platform` is null or NUL-terminated, the session is open on this thread.
        let rc = unsafe { NevrArtifactVerifyZip(self.sess, version.as_ptr(), kind.as_ptr(), ptr, zip.as_ptr(), allow_dirs) };
        (rc, last_error(self.sess))
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        // SAFETY: the session was opened by `new` and is closed once.
        unsafe { NevrClose(self.sess) };
    }
}

fn last_error(sess: *mut NevrSession) -> String {
    // SAFETY: the session is open; the returned string is NUL-terminated and valid until the next call.
    unsafe { CStr::from_ptr(NevrLastError(sess)).to_string_lossy().into_owned() }
}

#[test]
fn android_platform_selects_the_android_game_data_though_the_pc_one_is_listed_first() {
    let f = Fixture::new();
    let (rc, err) = f.verify("1.0.0", "game-data", Some("android-arm64"), "android.zip", 1);
    assert_eq!((rc, err.as_str()), (NEVR_OK, ""), "the Android zip is held to the Android entry");
    let (rc, err) = f.verify("1.0.0", "game-data", Some("android-arm64"), "pc.zip", 1);
    assert_eq!(rc, NEVR_FAILED, "the PC zip is not the Android entry's contents");
    assert!(err.contains("which the manifest does not list"), "{err}");
}

#[test]
fn windows_platform_selects_the_pc_game_data() {
    let f = Fixture::new();
    let (rc, err) = f.verify("1.0.0", "game-data", Some("windows-x86_64"), "pc.zip", 1);
    assert_eq!((rc, err.as_str()), (NEVR_OK, ""), "the PC zip is held to the PC entry");
    let (rc, err) = f.verify("1.0.0", "game-data", Some("windows-x86_64"), "android.zip", 1);
    assert_eq!(rc, NEVR_FAILED, "the Android zip is not the PC entry's contents");
    assert!(err.contains("which the manifest does not list"), "{err}");
}

#[test]
fn game_data_without_a_platform_is_refused_with_the_reason() {
    let f = Fixture::new();
    let (rc, err) = f.verify("1.0.0", "game-data", None, "android.zip", 1);
    assert_eq!(rc, NEVR_FAILED);
    assert!(err.contains("one game-data artifact per platform") && err.contains("android-arm64") && err.contains("windows-x86_64"), "{err}");
    assert!(!err.contains("lists it twice"), "{err}");
}

#[test]
fn another_kind_keeps_its_rule_with_and_without_a_platform() {
    let f = Fixture::new();
    // One entry of the kind: a null platform selects it, as before.
    let (rc, err) = f.verify("1.0.1", "windows-zip", None, "android.zip", 0);
    assert_eq!((rc, err.as_str()), (NEVR_OK, ""));
    // Two entries of the kind: a null platform still says it lists the kind twice.
    let (rc, err) = f.verify("1.0.0", "windows-zip", None, "android.zip", 0);
    assert_eq!(rc, NEVR_FAILED);
    assert!(err.contains("lists it twice"), "{err}");
}

#[test]
fn the_pc_artifact_name_passes_the_path_shape_check() {
    let m = json!({"releases": {"1.0.0": {"artifacts": [data_entry(PC_NAME, "windows-x86_64", PC_MEMBER), data_entry(ANDROID_NAME, "android-arm64", ANDROID_MEMBER)]}}});
    let pc = manifest::find(&m, "1.0.0", "game-data", Some("windows-x86_64")).unwrap();
    assert_eq!(pc.path, format!("data/{PC_NAME}"));
    assert_eq!(pc.contents[0].path, PC_MEMBER.0);
    let android = manifest::find(&m, "1.0.0", "game-data", Some("android-arm64")).unwrap();
    assert_eq!(android.path, format!("data/{ANDROID_NAME}"));
    assert_eq!(manifest::find(&m, "1.0.0", "game-data", None).unwrap_err().step, 8, "two entries and no platform select nothing");
}
