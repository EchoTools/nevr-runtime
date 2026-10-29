//! The C ABI's manifest verify takes the state lock the CLI takes, so an embedder's run and an installer run never
//! interleave their reads and writes of one state directory (#1353).

mod common;
use common::*;
use nevr_install::ffi::{NevrClose, NevrInstallOpen, NevrManifestVerify, NevrSession, NEVR_OK};
use serde_json::json;
use std::ffi::CString;
use std::sync::mpsc;
use std::time::Duration;

#[test]
fn manifest_verify_over_the_c_abi_waits_for_the_state_lock() {
    let d = tempfile::tempdir().unwrap();
    let k = make_key(d.path(), "k");
    let m: Vec<u8> = manifest_json(5, json!({})).into();
    let sig: Vec<u8> = sign(d.path(), &k, &m, false).into();
    let root = d.path().join("root");
    let state = root.join("state");
    std::fs::create_dir_all(&state).unwrap();
    // Another run holds the state lock.
    let holder = std::fs::OpenOptions::new().create(true).write(true).truncate(false).open(state.join("state.lock")).unwrap();
    holder.lock().unwrap();
    let cfg = CString::new(json!({"platform": "pc", "root": root, "pins": format!("T {}\n", k.b64), "now": NOW}).to_string()).unwrap();
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let mut s: *mut NevrSession = std::ptr::null_mut();
        // SAFETY: `cfg` is NUL-terminated and `s` is writable; the session is opened, used and closed on this thread.
        let rc = unsafe {
            assert_eq!(NevrInstallOpen(cfg.as_ptr(), &mut s), NEVR_OK);
            let rc = NevrManifestVerify(s, m.as_ptr(), m.len(), sig.as_ptr(), sig.len());
            NevrClose(s);
            rc
        };
        let _ = tx.send(rc);
    });
    assert!(rx.recv_timeout(Duration::from_millis(800)).is_err(), "the C ABI verify finished while another run held the state lock");
    drop(holder);
    assert_eq!(rx.recv_timeout(Duration::from_secs(60)).unwrap(), NEVR_OK);
}
