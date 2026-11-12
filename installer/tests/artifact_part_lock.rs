//! #1384: a download holds an exclusive lock on its `.part` file, so a second download to the same output refuses
//! with a message that names the file, and leaves the partial bytes alone.

use nevr_install::artifact;
use nevr_install::manifest::Artifact;
use nevr_install::util::sha256_hex;
use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::net::TcpListener;

const BODY: &[u8] = b"hello artifact";

/// Serves one connection with a complete 200 response for `BODY`; returns the base URL.
fn serve_once() -> String {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
    std::thread::spawn(move || {
        if let Ok((mut c, _)) = l.accept() {
            let mut buf = [0u8; 4096];
            let _ = c.read(&mut buf);
            let head = format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", BODY.len());
            let _ = c.write_all(head.as_bytes());
            let _ = c.write_all(BODY);
        }
    });
    base
}

fn art() -> Artifact {
    Artifact {
        kind: "x".into(),
        platform: None,
        path: "p".into(),
        size: BODY.len() as u64,
        sha256: sha256_hex(BODY),
        android: None,
        game_data: None,
        content_pack: None,
        contents: vec![],
    }
}

fn lock_part(part: &std::path::Path) -> File {
    let f = OpenOptions::new().read(true).write(true).create(true).truncate(false).open(part).unwrap();
    f.try_lock().expect("the test takes the lock first");
    f
}

#[test]
fn a_second_download_to_a_locked_part_file_is_refused_and_names_the_file() {
    let d = tempfile::tempdir().unwrap();
    let out = d.path().join("a.zip");
    let part = d.path().join("a.zip.part");
    std::fs::write(&part, b"partial").unwrap();
    let _holder = lock_part(&part);

    let base = serve_once();
    let err = artifact::download(&format!("{base}/a.zip"), &art(), &out).expect_err("the second writer must refuse");
    let msg = err.to_string();
    assert!(msg.contains("a.zip.part"), "the refusal names the partial file: {msg}");
    assert!(msg.contains("locked by another download"), "the refusal says why: {msg}");
    assert_eq!(std::fs::read(&part).unwrap(), b"partial", "the holder's partial bytes are left alone");
    assert!(!out.exists(), "nothing is written to the output");
}

#[test]
fn the_lock_is_released_when_the_holder_lets_go_and_the_download_completes() {
    let d = tempfile::tempdir().unwrap();
    let out = d.path().join("a.zip");
    let part = d.path().join("a.zip.part");
    let holder = lock_part(&part);
    drop(holder);

    let base = serve_once();
    let fetched = artifact::download(&format!("{base}/a.zip"), &art(), &out).expect("the lock is free");
    assert_eq!(fetched.bytes, BODY.len() as u64);
    let mut got = Vec::new();
    File::open(&out).unwrap().read_to_end(&mut got).unwrap();
    assert_eq!(got, BODY);
    assert!(!part.exists(), "the completed download leaves no .part file");
}
