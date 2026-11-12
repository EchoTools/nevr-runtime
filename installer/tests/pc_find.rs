//! The search for an Echo VR install (#1300): `pc::find_installs` over injected drive roots and search roots, and the
//! `pc find` command run as the real binary. Nothing here reads the registry or the real drives: the drives are temporary
//! folders, so the test runs the same on every platform.

use nevr_install::pc::{find_installs, EXE_NAME};
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const LAYOUT_STORE: &[&str] = &["Software", "Software", "ready-at-dawn-echo-arena", "bin", "win10"];
const LAYOUT_SOFTWARE: &[&str] = &["Software", "ready-at-dawn-echo-arena", "bin", "win10"];
const LAYOUT_BARE: &[&str] = &["ready-at-dawn-echo-arena", "bin", "win10"];

/// Puts an `echovr.exe` at `base` joined with `parts`, and returns the folder that holds it.
fn place(base: &Path, parts: &[&str]) -> PathBuf {
    let folder = parts.iter().fold(base.to_path_buf(), |p, part| p.join(part));
    fs::create_dir_all(&folder).unwrap();
    fs::write(folder.join(EXE_NAME), b"echovr").unwrap();
    folder
}

fn sorted(mut v: Vec<PathBuf>) -> Vec<PathBuf> {
    v.sort();
    v
}

#[test]
fn the_drive_root_and_the_named_folders_are_searched_with_the_bare_layout() {
    let drive = tempfile::tempdir().unwrap();
    let d = drive.path();
    let expected = vec![
        place(d, LAYOUT_BARE),
        place(&d.join("EchoVR"), LAYOUT_BARE),
        place(&d.join("Echo VR"), LAYOUT_BARE),
        place(&d.join("Games").join("EchoVR"), LAYOUT_BARE),
    ];
    let found = find_installs(&[d.to_path_buf()], &[]);
    assert_eq!(sorted(found), sorted(expected));
}

#[test]
fn an_install_in_c_echovr_is_found_on_any_drive_letter_not_only_c_d_and_e() {
    // The field report (#1300): C:\EchoVR\ready-at-dawn-echo-arena\bin\win10. The drive is a folder here.
    let drives: Vec<tempfile::TempDir> = (0..3).map(|_| tempfile::tempdir().unwrap()).collect();
    let roots: Vec<PathBuf> = drives.iter().map(|t| t.path().to_path_buf()).collect();
    let expected: Vec<PathBuf> = roots.iter().map(|r| place(&r.join("EchoVR"), LAYOUT_BARE)).collect();
    let found = find_installs(&roots, &[]);
    assert_eq!(found, expected, "one install per drive, in drive order");
}

#[test]
fn each_of_the_three_layouts_is_searched_under_a_drive_root() {
    let drive = tempfile::tempdir().unwrap();
    let d = drive.path();
    let expected = vec![place(d, LAYOUT_STORE), place(d, LAYOUT_SOFTWARE), place(d, LAYOUT_BARE)];
    let found = find_installs(&[d.to_path_buf()], &[]);
    assert_eq!(sorted(found), sorted(expected));
}

#[test]
fn the_oculus_and_meta_horizon_roots_under_program_files_and_the_drive_root_are_searched() {
    let drive = tempfile::tempdir().unwrap();
    let d = drive.path();
    let expected = vec![
        place(&d.join("Program Files").join("Meta Horizon"), LAYOUT_SOFTWARE),
        place(&d.join("Program Files").join("Oculus"), LAYOUT_SOFTWARE),
        place(&d.join("Program Files (x86)").join("Oculus"), LAYOUT_STORE),
        place(&d.join("Oculus"), LAYOUT_BARE),
        place(&d.join("Meta Horizon"), LAYOUT_BARE),
    ];
    let found = find_installs(&[d.to_path_buf()], &[]);
    assert_eq!(sorted(found), sorted(expected));
}

#[test]
fn a_search_root_from_the_oculus_registry_is_searched_with_the_three_layouts() {
    let lib = tempfile::tempdir().unwrap();
    let expected = vec![place(lib.path(), LAYOUT_SOFTWARE)];
    let found = find_installs(&[], &[lib.path().to_path_buf()]);
    assert_eq!(found, expected);
}

#[test]
fn a_folder_reached_twice_is_reported_once() {
    let drive = tempfile::tempdir().unwrap();
    let d = drive.path();
    let install = place(&d.join("EchoVR"), LAYOUT_BARE);
    // Named as a drive folder and as a search root, and the same root twice.
    let found = find_installs(&[d.to_path_buf()], &[d.join("EchoVR"), d.join("EchoVR")]);
    assert_eq!(found, vec![install]);
}

#[test]
fn a_folder_spelled_in_another_case_is_reported_once() {
    // NTFS is case-insensitive, so `echovr` and `EchoVR` name one folder there. This test makes one folder: on a
    // case-sensitive file system the second spelling is absent, and on NTFS it is the same folder. Either way one is reported.
    let drive = tempfile::tempdir().unwrap();
    let d = drive.path();
    place(&d.join("EchoVR"), LAYOUT_BARE);
    let found = find_installs(&[], &[d.join("EchoVR"), d.join("echovr")]);
    assert_eq!(found.len(), 1, "found: {found:?}");
}

#[test]
fn a_miss_is_an_empty_list() {
    let drive = tempfile::tempdir().unwrap();
    assert!(find_installs(&[drive.path().to_path_buf()], &[]).is_empty());
}

/// The real binary: one JSON array on standard output, and the run in the install log beside it.
#[test]
fn pc_find_prints_one_json_array_and_logs_the_run() {
    let work = tempfile::tempdir().unwrap();
    let lib = tempfile::tempdir().unwrap();
    let install = place(lib.path(), LAYOUT_BARE);
    let out = Command::new(env!("CARGO_BIN_EXE_install-nevr-runtime"))
        .args(["pc", "find", "--root"])
        .arg(work.path())
        .arg("--search-root")
        .arg(lib.path())
        .output()
        .unwrap();
    assert_eq!(out.status.code(), Some(0), "stderr: {}", String::from_utf8_lossy(&out.stderr));
    let stdout = String::from_utf8(out.stdout).unwrap();
    let listed: Value = serde_json::from_str(stdout.trim()).expect("stdout is one JSON value");
    assert_eq!(listed, serde_json::json!([install.display().to_string()]));

    let log = fs::read_to_string(work.path().join("logs").join("nevr-install.jsonl")).unwrap();
    let find: Vec<Value> = log
        .lines()
        .map(|l| serde_json::from_str::<Value>(l).expect("each log line is JSON"))
        .filter(|r| r["event"] == "find")
        .collect();
    assert_eq!(find.len(), 1, "one find record: {log}");
    assert_eq!(find[0]["found"], 1);
}

#[test]
fn pc_find_with_no_install_prints_an_empty_array_and_exits_zero() {
    let work = tempfile::tempdir().unwrap();
    let lib = tempfile::tempdir().unwrap();
    let out = Command::new(env!("CARGO_BIN_EXE_install-nevr-runtime"))
        .args(["pc", "find", "--root"])
        .arg(work.path())
        .arg("--search-root")
        .arg(lib.path())
        .output()
        .unwrap();
    assert_eq!(out.status.code(), Some(0));
    assert_eq!(String::from_utf8(out.stdout).unwrap().trim(), "[]");
}
