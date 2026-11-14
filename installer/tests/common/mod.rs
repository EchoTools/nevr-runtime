//! Shared helpers for the integration tests: the repository's release-verification fixtures, and throwaway minisign keys
//! (made with the `minisign` CLI, as `tools/tests/test_verifying_a_release_doc.py` does) to sign manifests the fixtures
//! do not carry. A missing `minisign` fails the test; a gate that skips itself checks nothing.
#![allow(dead_code)]

use nevr_install::manifest::Pins;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Answers each request from `routes`; a path in `cut` announces its full length and sends only that many bytes.
/// Returns the base URL of a server on a free local port.
pub fn serve(routes: HashMap<String, Vec<u8>>, cut: Option<(String, usize)>) -> String {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
    std::thread::spawn(move || {
        for c in l.incoming() {
            let Ok(mut c) = c else { continue };
            let mut buf = [0u8; 4096];
            let k = c.read(&mut buf).unwrap_or(0);
            let req = String::from_utf8_lossy(&buf[..k]).to_string();
            let path = req.split_whitespace().nth(1).unwrap_or("/").to_string();
            match routes.get(&path) {
                Some(body) => {
                    let mut r = format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).into_bytes();
                    let n = match &cut {
                        Some((p, n)) if *p == path => (*n).min(body.len()),
                        _ => body.len(),
                    };
                    r.extend_from_slice(&body[..n]);
                    let _ = c.write_all(&r);
                }
                None => {
                    let _ = c.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                }
            }
        }
    });
    base
}

pub const TEST_PINS: &str = "TEST RWTHthwA6C/csRKFPrZPxjAVZFImiwbgTWoK+P0qiEHQ4mnKdzhbzOqA\n";
pub const TEST_KEY_ID: &str = "c7b61c00e82fdcb1";
pub const NOW: &str = "2026-10-26T12:00:00Z";

pub fn fixture(dir: &str) -> (Vec<u8>, Vec<u8>) {
    let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("../docs/reference/release-verification").join(dir);
    (std::fs::read(base.join("manifest.json")).expect("fixture manifest"), std::fs::read(base.join("manifest.json.sig")).expect("fixture sig"))
}

pub fn test_pins() -> Pins {
    Pins::parse(TEST_PINS).unwrap()
}

pub struct Key {
    pub sec: PathBuf,
    pub b64: String,
}

/// A fresh unencrypted keypair in `dir`.
pub fn make_key(dir: &Path, name: &str) -> Key {
    let (sec, publ) = (dir.join(format!("{name}.key")), dir.join(format!("{name}.pub")));
    let o = Command::new("minisign").args(["-G", "-W", "-f", "-p"]).arg(&publ).arg("-s").arg(&sec).output().expect("minisign is installed");
    assert!(o.status.success(), "minisign -G: {}", String::from_utf8_lossy(&o.stderr));
    let b64 = std::fs::read_to_string(&publ).unwrap().lines().nth(1).unwrap().to_string();
    Key { sec, b64 }
}

/// Signs `bytes` in the hashed mode (`ED`), or the legacy mode when `legacy`.
pub fn sign(dir: &Path, key: &Key, bytes: &[u8], legacy: bool) -> Vec<u8> {
    let (m, x) = (dir.join("to-sign.json"), dir.join("to-sign.json.sig"));
    std::fs::write(&m, bytes).unwrap();
    let mut c = Command::new("minisign");
    c.arg("-S").arg("-s").arg(&key.sec).arg("-x").arg(&x).arg("-m").arg(&m).args(["-t", "nevr-runtime test"]);
    if legacy {
        c.arg("-l");
    }
    let o = c.output().expect("minisign is installed");
    assert!(o.status.success(), "minisign -S: {}", String::from_utf8_lossy(&o.stderr));
    std::fs::read(&x).unwrap()
}

/// The file-order key id of a public key line.
pub fn key_id(b64: &str) -> String {
    use base64::{engine::general_purpose::STANDARD, Engine};
    let raw = STANDARD.decode(b64).unwrap();
    raw[2..10].iter().map(|b| format!("{b:02x}")).collect()
}

/// A minimal valid manifest, with `extra` members merged over it.
pub fn manifest_json(version: u64, extra: serde_json::Value) -> Vec<u8> {
    let mut m = serde_json::json!({
        "schema_version": 1, "product": "nevr-runtime", "manifest_version": version,
        "expires": "2026-11-07T18:00:00Z", "channels": {"beta": "1.0.0"}, "releases": {"1.0.0": {"artifacts": []}}
    });
    for (k, v) in extra.as_object().unwrap() {
        m[k] = v.clone();
    }
    serde_json::to_vec(&m).unwrap()
}
