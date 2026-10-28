//! The C ABI from a C program (not Rust): the header compiles, the static library links, every call behaves.

mod common;
use common::*;
use serde_json::json;
use std::process::Command;

#[test]
fn a_c_program_drives_open_verify_pick_plan_end() {
    // Build the static library the C program links (a `cargo test` run builds only the test profile's rlib).
    let manifest = format!("{}/Cargo.toml", env!("CARGO_MANIFEST_DIR"));
    let b = Command::new(env!("CARGO")).args(["build", "--lib", "--manifest-path", &manifest]).output().unwrap();
    assert!(b.status.success(), "{}", String::from_utf8_lossy(&b.stderr));
    let exe = std::env::current_exe().unwrap();
    let debug = exe.parent().unwrap().parent().unwrap();
    let lib = debug.join("libnevr_install.a");
    assert!(lib.exists(), "{}", lib.display());

    let d = tempfile::tempdir().unwrap();
    let k = make_key(d.path(), "k");
    let m = manifest_json(
        5,
        json!({"releases": {"1.0.0": {"artifacts": [{"kind": "quest-apk", "platform": "android-arm64", "path": "v1.0.0/nevr-runtime-v1.0.0-quest.apk",
            "size": 10, "sha256": "2".repeat(64), "android": {"package": "com.readyatdawn.r15", "version_code": 5, "signer_cert_sha256": "a".repeat(64)}}]}}}),
    );
    let sig = sign(d.path(), &k, &m, false);
    std::fs::write(d.path().join("m.json"), &m).unwrap();
    std::fs::write(d.path().join("m.sig"), &sig).unwrap();
    let cfg = json!({"platform": "quest", "root": d.path().join("root"), "pins": format!("T {}\n", k.b64), "now": NOW}).to_string();

    let bin = d.path().join("abi_check");
    let cc = Command::new("cc")
        .arg(concat!(env!("CARGO_MANIFEST_DIR"), "/tests/c/abi_check.c"))
        .arg("-I").arg(concat!(env!("CARGO_MANIFEST_DIR"), "/include"))
        .arg(&lib).args(["-lpthread", "-ldl", "-lm", "-Wall", "-Werror", "-o"]).arg(&bin)
        .output().expect("cc is installed");
    assert!(cc.status.success(), "{}", String::from_utf8_lossy(&cc.stderr));

    let o = Command::new(&bin).arg(&cfg).arg(d.path().join("m.json")).arg(d.path().join("m.sig")).output().unwrap();
    let (out, err) = (String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr));
    assert!(o.status.success(), "stdout: {out}\nstderr: {err}");
    assert!(out.contains("release=") && out.contains("\"version\":\"1.0.0\""), "{out}");
    // Data in the legacy root plus a current-enough release: the plan proceeds with the APK steps only.
    assert!(out.contains("\"decision\":\"install\""), "{out}");

    let log = std::fs::read_to_string(d.path().join("root/logs/nevr-install.jsonl")).unwrap();
    let ev: Vec<serde_json::Value> = log.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
    assert!(ev.iter().any(|r| r["event"] == "run_start"));
    assert!(ev.iter().any(|r| r["event"] == "ffi_error"), "the tampered manifest was logged");
    let last = ev.last().unwrap();
    assert_eq!((last["event"].as_str(), last["state"].as_str(), last["reason"].as_str()), (Some("run_end"), Some("unchanged"), Some("abi check")));
}
