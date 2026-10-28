//! `read_package` returns exactly the bytes the manifest names: the zip on disk may have been swapped after
//! `verify_zip` looked at it, and a member may differ from the `contents` entry that is signed.

use nevr_install::manifest::{Artifact, Content};
use nevr_install::session::read_package;
use nevr_install::util::sha256_hex;
use std::io::Write;

const HOST: &[u8] = b"host NEVR-BUILD 1.0.0";
const RT: &[u8] = b"runtime NEVR-BUILD 1.0.0";

fn zip_of(members: &[(&str, &[u8])]) -> Vec<u8> {
    let mut buf = std::io::Cursor::new(Vec::new());
    let mut z = zip::ZipWriter::new(&mut buf);
    for (n, b) in members {
        z.start_file(*n, zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(b).unwrap();
    }
    z.finish().unwrap();
    buf.into_inner()
}

fn art(zip: &[u8], listed: &[(&str, &[u8])]) -> Artifact {
    Artifact {
        kind: "windows-zip".into(),
        platform: Some("windows-x86_64".into()),
        path: "v1.0.0/nevr-runtime-v1.0.0-windows.zip".into(),
        size: zip.len() as u64,
        sha256: sha256_hex(zip),
        android: None,
        game_data: None,
        contents: listed.iter().map(|(n, b)| Content { path: n.to_string(), sha256: sha256_hex(b), size: b.len() as u64 }).collect(),
    }
}

#[test]
fn the_package_is_the_bytes_the_manifest_names() {
    let d = tempfile::tempdir().unwrap();
    let members: [(&str, &[u8]); 2] = [("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT)];
    let zip = zip_of(&members);
    std::fs::write(d.path().join("a.zip"), &zip).unwrap();
    let pkg = read_package(&d.path().join("a.zip"), &art(&zip, &members), "1.0.0").unwrap();
    assert_eq!((pkg.host.as_slice(), pkg.runtime.as_slice(), pkg.version.as_str()), (HOST, RT, "1.0.0"));
}

#[test]
fn a_zip_swapped_after_it_was_verified_is_refused() {
    let d = tempfile::tempdir().unwrap();
    let members: [(&str, &[u8]); 2] = [("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT)];
    let zip = zip_of(&members);
    let a = art(&zip, &members);
    // Between the check and the read another process replaces the file with a valid zip holding other bytes.
    let swapped = zip_of(&[("BugSplat64.dll", b"evil host"), ("nevr-runtime.dll", b"evil runtime")]);
    std::fs::write(d.path().join("a.zip"), &swapped).unwrap();
    let e = read_package(&d.path().join("a.zip"), &a, "1.0.0").unwrap_err();
    assert!(e.to_string().contains("does not match the manifest"), "{e}");
}

#[test]
fn a_member_that_differs_from_its_signed_entry_is_refused_even_when_the_zip_hash_matches() {
    let d = tempfile::tempdir().unwrap();
    let zip = zip_of(&[("BugSplat64.dll", b"evil host"), ("nevr-runtime.dll", RT)]);
    // The zip's own hash was recorded from this file, but `contents` names the real host.
    let a = art(&zip, &[("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT)]);
    std::fs::write(d.path().join("a.zip"), &zip).unwrap();
    let e = read_package(&d.path().join("a.zip"), &a, "1.0.0").unwrap_err();
    assert!(e.to_string().contains("BugSplat64.dll in the zip does not match"), "{e}");
}

#[test]
fn a_swapped_zip_with_the_same_members_but_other_bytes_is_refused_by_the_whole_zip_hash() {
    let d = tempfile::tempdir().unwrap();
    let members: [(&str, &[u8]); 2] = [("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT)];
    let zip = zip_of(&members);
    let a = art(&zip, &members);
    // Every member still matches `contents`, but the file is not the zip the manifest hashed (an extra entry).
    let swapped = zip_of(&[("BugSplat64.dll", HOST), ("nevr-runtime.dll", RT), ("extra.txt", b"x")]);
    std::fs::write(d.path().join("a.zip"), &swapped).unwrap();
    let e = read_package(&d.path().join("a.zip"), &a, "1.0.0").unwrap_err();
    assert!(e.to_string().contains("does not match the manifest's size and sha256 when it is read"), "{e}");
}
