//! Step 1 and step 8 over a real local HTTP server: caps, ranges, redirects, exact size and sha256, every zip member.

use nevr_install::artifact;
use nevr_install::fetch::{fetch_capped, Failure};
use nevr_install::manifest::{Artifact, Content};
use nevr_install::util::sha256_hex;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};

/// Serves `responder(request_text) -> raw response bytes` for `n` connections; returns (url base, requests seen).
fn serve(n: usize, responder: impl Fn(&str) -> Vec<u8> + Send + 'static) -> (String, Arc<Mutex<Vec<String>>>) {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
    let seen = Arc::new(Mutex::new(Vec::new()));
    let s2 = seen.clone();
    std::thread::spawn(move || {
        for _ in 0..n {
            let Ok((mut c, _)) = l.accept() else { return };
            let mut buf = [0u8; 4096];
            let k = c.read(&mut buf).unwrap_or(0);
            let req = String::from_utf8_lossy(&buf[..k]).to_string();
            s2.lock().unwrap().push(req.clone());
            let _ = c.write_all(&responder(&req));
        }
    });
    (base, seen)
}

fn ok(body: &[u8]) -> Vec<u8> {
    let mut r = format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).into_bytes();
    r.extend_from_slice(body);
    r
}

fn art(body: &[u8]) -> Artifact {
    Artifact { kind: "x".into(), platform: None, path: "p".into(), size: body.len() as u64, sha256: sha256_hex(body), android: None, game_data: None, contents: vec![] }
}

#[test]
fn a_body_within_the_cap_is_read() {
    let (base, _) = serve(1, |_| ok(b"hello"));
    let mut sink = Vec::new();
    assert_eq!(fetch_capped(&base, 0, 5, &mut sink).ok(), Some(5));
    assert_eq!(sink, b"hello");
}

#[test]
fn a_body_over_the_cap_is_refused_whether_or_not_a_length_was_announced() {
    let (base, _) = serve(1, |_| ok(b"hello world"));
    let mut sink = Vec::new();
    assert!(matches!(fetch_capped(&base, 0, 5, &mut sink), Err(Failure::Refused(m)) if m.contains("more than 5 bytes")));
    // No Content-Length: the connection close ends the body.
    let (base, _) = serve(1, |_| b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n0123456789".to_vec());
    let mut sink = Vec::new();
    assert!(matches!(fetch_capped(&base, 0, 5, &mut sink), Err(Failure::Refused(_))));
}

#[test]
fn a_redirect_is_not_followed() {
    let (base, seen) = serve(2, |_| b"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".to_vec());
    let mut sink = Vec::new();
    assert!(matches!(fetch_capped(&base, 0, 100, &mut sink), Err(Failure::Refused(m)) if m.contains("302")));
    assert_eq!(seen.lock().unwrap().len(), 1);
}

#[test]
fn a_non_https_url_for_another_machine_is_refused_before_any_connection() {
    let mut sink = Vec::new();
    assert!(matches!(fetch_capped("http://example.invalid/x", 0, 5, &mut sink), Err(Failure::Refused(m)) if m.contains("only https")));
}

#[test]
fn only_the_loopback_host_itself_is_fetched_over_http_not_a_name_that_starts_like_it() {
    for url in [
        "http://127.0.0.1.invalid/x",
        "http://localhost.invalid/x",
        "http://localhost.example.com:80/x",
        "http://127.0.0.10/x",
        "http://127.0.0.1@example.invalid/x",
        "http://localhost@example.invalid/x",
        "http://user@127.0.0.1/x",
        "http://user:pass@localhost:80/x",
        "http://127.0.0.1:80@example.invalid/x",
        "http://[::2]/x",
        "http://127.0.0.1%2eexample.invalid/x",
        "http:// 127.0.0.1/x",
    ] {
        let mut sink = Vec::new();
        assert!(matches!(fetch_capped(url, 0, 5, &mut sink), Err(Failure::Refused(m)) if m.contains("only https")), "{url} must be refused before any connection");
    }
}

#[test]
fn the_loopback_names_are_still_fetched_over_http() {
    let (base, _) = serve(2, |_| ok(b"hello"));
    let port = base.rsplit(':').next().unwrap().to_string();
    for host in ["127.0.0.1", "localhost"] {
        let mut sink = Vec::new();
        assert_eq!(fetch_capped(&format!("http://{host}:{port}/x"), 0, 5, &mut sink).ok(), Some(5), "{host}");
    }
}

#[test]
fn download_checks_exact_size_and_sha256_and_deletes_a_bad_file() {
    let d = tempfile::tempdir().unwrap();
    let body = b"the artifact bytes";
    let (base, _) = serve(1, move |_| ok(body));
    let out = d.path().join("a.bin");
    let f = artifact::download(&format!("{base}/a"), &art(body), &out).unwrap();
    assert_eq!((f.bytes, f.resumed_from), (body.len() as u64, 0));
    assert_eq!(std::fs::read(&out).unwrap(), body);

    // Same size, different bytes: sha256 differs, nothing is left behind.
    let (base, _) = serve(1, |_| ok(b"the artifact BYTES"));
    let out2 = d.path().join("b.bin");
    let e = artifact::download(&format!("{base}/a"), &art(body), &out2).unwrap_err();
    assert_eq!((e.step, e.msg.as_str()), (8, "sha256 differs from the manifest"));
    assert!(!out2.exists() && !d.path().join("b.bin.part").exists());

    // Shorter than the manifest says.
    let (base, _) = serve(1, |_| ok(b"short"));
    let out3 = d.path().join("c.bin");
    let e = artifact::download(&format!("{base}/a"), &art(body), &out3).unwrap_err();
    assert!(e.msg.starts_with("size is 5"), "{}", e.msg);
    assert!(!out3.exists());
}

#[test]
fn an_interrupted_download_resumes_with_a_range_and_the_whole_file_is_hashed() {
    let d = tempfile::tempdir().unwrap();
    let body: Vec<u8> = (0..200u8).collect();
    let out = d.path().join("r.bin");
    std::fs::write(d.path().join("r.bin.part"), &body[..80]).unwrap();
    let rest = body[80..].to_vec();
    let (base, seen) = serve(1, move |_| {
        let mut r = format!("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 80-199/200\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", rest.len()).into_bytes();
        r.extend_from_slice(&rest);
        r
    });
    let f = artifact::download(&format!("{base}/a"), &art(&body), &out).unwrap();
    assert_eq!(f.resumed_from, 80);
    assert_eq!(std::fs::read(&out).unwrap(), body);
    assert!(seen.lock().unwrap()[0].to_lowercase().contains("range: bytes=80-"));
}

#[test]
fn a_server_that_ignores_the_range_makes_the_download_start_again_from_zero() {
    let d = tempfile::tempdir().unwrap();
    let body: Vec<u8> = (0..100u8).collect();
    let out = d.path().join("r.bin");
    std::fs::write(d.path().join("r.bin.part"), &body[..30]).unwrap();
    let b2 = body.clone();
    let (base, _) = serve(2, move |_| ok(&b2));
    let f = artifact::download(&format!("{base}/a"), &art(&body), &out).unwrap();
    assert_eq!(f.resumed_from, 0);
    assert_eq!(std::fs::read(&out).unwrap(), body);
}

fn make_zip(entries: &[(&str, &[u8])]) -> Vec<u8> {
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        for (n, b) in entries {
            z.start_file(*n, zip::write::SimpleFileOptions::default()).unwrap();
            z.write_all(b).unwrap();
        }
        z.finish().unwrap();
    }
    buf.into_inner()
}

fn zip_art(listed: &[(&str, &[u8])]) -> Artifact {
    let mut a = art(b"");
    a.contents = listed.iter().map(|(n, b)| Content { path: n.to_string(), sha256: sha256_hex(b), size: b.len() as u64 }).collect();
    a
}

fn zip_check(zip_entries: &[(&str, &[u8])], listed: &[(&str, &[u8])]) -> nevr_install::error::Result<usize> {
    let d = tempfile::tempdir().unwrap();
    let p = d.path().join("a.zip");
    std::fs::write(&p, make_zip(zip_entries)).unwrap();
    artifact::verify_zip(&p, &zip_art(listed), false)
}

#[test]
fn every_member_must_match_contents() {
    let e: &[(&str, &[u8])] = &[("a.dll", b"AAA"), ("b.txt", b"BB")];
    assert_eq!(zip_check(e, e).unwrap(), 2);
    // An extra entry the manifest does not list.
    let m = zip_check(&[("a.dll", b"AAA"), ("b.txt", b"BB"), ("extra.txt", b"x")], e).unwrap_err().msg;
    assert_eq!(m, "the zip holds extra.txt, which the manifest does not list");
    // A listed file the zip lacks.
    let m = zip_check(&[("a.dll", b"AAA")], e).unwrap_err().msg;
    assert_eq!(m, "the manifest lists b.txt, which the zip does not hold");
    // Different content, same size; different size.
    assert!(zip_check(&[("a.dll", b"AAB"), ("b.txt", b"BB")], e).unwrap_err().msg.contains("different sha256"));
    assert!(zip_check(&[("a.dll", b"AAAA"), ("b.txt", b"BB")], e).unwrap_err().msg.contains("different size"));
    // A path-traversal entry cannot pass: it is not a listed name.
    let m = zip_check(&[("../evil.dll", b"x")], e).unwrap_err().msg;
    assert!(m.contains("does not list"), "{m}");
}

#[test]
fn directory_entries_are_refused_for_a_program_zip_and_allowed_for_game_data() {
    let d = tempfile::tempdir().unwrap();
    let p = d.path().join("a.zip");
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        z.add_directory("sub/", zip::write::SimpleFileOptions::default()).unwrap();
        z.start_file("sub/f", zip::write::SimpleFileOptions::default()).unwrap();
        z.write_all(b"F").unwrap();
        z.finish().unwrap();
    }
    std::fs::write(&p, buf.into_inner()).unwrap();
    let a = zip_art(&[("sub/f", b"F")]);
    assert!(artifact::verify_zip(&p, &a, false).unwrap_err().msg.contains("not a regular file"));
    assert_eq!(artifact::verify_zip(&p, &a, true).unwrap(), 2);
}

#[test]
fn extract_writes_only_listed_files_by_rename_and_nothing_else() {
    let d = tempfile::tempdir().unwrap();
    let p = d.path().join("a.zip");
    let e: &[(&str, &[u8])] = &[("x/a.bin", b"AAA"), ("y.bin", b"Y")];
    std::fs::write(&p, make_zip(e)).unwrap();
    let root = d.path().join("root");
    let written = artifact::extract(&p, &zip_art(e), &root).unwrap();
    assert_eq!(written.len(), 2);
    assert_eq!(std::fs::read(root.join("x/a.bin")).unwrap(), b"AAA");
    assert!(!root.join("x/a.bin.new").exists());
}
