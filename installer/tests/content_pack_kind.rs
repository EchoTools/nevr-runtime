//! The `content-pack` artifact kind (docs/reference/verifying-a-release.md, step 8): a valid entry, one test per refusal,
//! `find_content_pack`, and a regression for the `game-data` entry. Every test calls the public API with serde_json
//! values; the entries are built here by hand.

use nevr_install::error::Error;
use nevr_install::manifest::{self, Artifact};
use nevr_install::util::sha256_hex;
use serde_json::{json, Value};

/// 64 lower-case hex digits of one character: `h('1')` sorts before `h('2')`.
fn h(c: char) -> String {
    c.to_string().repeat(64)
}

/// A content-pack entry that passes every check, for `id`. Its name and path carry the first 12 hex of its own sha256.
fn pack_entry(id: &str) -> Value {
    let sha = sha256_hex(format!("pack bytes for {id}").as_bytes());
    let name = format!("{id}-{}.zip", &sha[..12]);
    json!({
        "name": name,
        "kind": "content-pack",
        "platform": "windows-x86_64",
        "path": format!("packs/{name}"),
        "sha256": sha,
        "size": 4096,
        "contents": [{"path": "_data/5932408047/rad15/a", "sha256": h('a'), "size": 10}],
        "content_pack": {
            "id": id,
            "version": "1.0.0",
            "requires_stock_manifest_sha256": [h('1')],
            "stock_data_version": "5932408047",
            "plugins": ["combat-mod"],
        }
    })
}

fn release(entries: Vec<Value>) -> Value {
    json!({"releases": {"1.0.0": {"artifacts": entries}}})
}

/// Parses one content-pack entry as the only artifact of release 1.0.0.
fn parse(e: Value) -> nevr_install::error::Result<Artifact> {
    manifest::find(&release(vec![e]), "1.0.0", "content-pack", None)
}

/// The refusal of `e`: it must fail at step 8, and the message is returned for the caller to check exactly.
fn refused(e: Value) -> String {
    let err: Error = parse(e).expect_err("the entry should be refused");
    assert_eq!(err.step, 8, "{}", err.msg);
    err.msg
}

#[test]
fn a_valid_content_pack_parses() {
    let a = parse(pack_entry("combat")).unwrap();
    assert_eq!(a.kind, "content-pack");
    assert_eq!(a.platform.as_deref(), Some("windows-x86_64"));
    assert!(a.path.starts_with("packs/combat-") && a.path.ends_with(".zip"), "{}", a.path);
    assert_eq!(a.contents.len(), 1);
    let cp = a.content_pack.expect("a content-pack artifact carries its content_pack");
    assert_eq!(cp.id, "combat");
    assert_eq!(cp.version, "1.0.0");
    assert_eq!(cp.requires_stock_manifest_sha256, [h('1')]);
    assert_eq!(cp.stock_data_version, "5932408047");
    assert_eq!(cp.plugins, ["combat-mod"]);
}

#[test]
fn a_content_pack_may_name_two_stock_manifests_and_no_plugin() {
    let mut e = pack_entry("combat");
    e["content_pack"]["requires_stock_manifest_sha256"] = json!([h('1'), h('2')]);
    e["content_pack"]["plugins"] = json!([]);
    let cp = parse(e).unwrap().content_pack.unwrap();
    assert_eq!(cp.requires_stock_manifest_sha256, [h('1'), h('2')]);
    assert!(cp.plugins.is_empty());
}

#[test]
fn a_path_not_under_packs_is_refused_before_anything_else() {
    let name = pack_entry("combat")["name"].as_str().unwrap().to_string();
    for bad in [name.clone(), format!("data/{name}"), format!("packs/../{name}"), format!("v1.0.0/nevr-runtime-v1.0.0-{name}")] {
        let mut e = pack_entry("combat");
        e["path"] = json!(bad);
        assert_eq!(refused(e), "path in the manifest is not packs/<name>.zip", "{bad}");
    }
}

#[test]
fn a_name_whose_sha12_does_not_match_the_hash_is_refused() {
    let mut e = pack_entry("combat");
    e["name"] = json!("combat-000000000000.zip");
    e["path"] = json!("packs/combat-000000000000.zip");
    let msg = refused(e);
    assert!(msg.starts_with("name in the manifest is not combat-"), "{msg}");
}

#[test]
fn a_name_field_that_differs_from_the_path_is_refused() {
    let mut e = pack_entry("combat");
    e["name"] = json!("combat-000000000000.zip");
    let msg = refused(e);
    assert!(msg.starts_with("name in the manifest is not combat-"), "{msg}");
}

#[test]
fn an_id_that_does_not_match_the_name_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["id"] = json!("other");
    let msg = refused(e);
    assert!(msg.starts_with("name in the manifest is not other-"), "{msg}");
}

#[test]
fn the_reserved_id_echovr_is_refused() {
    assert_eq!(refused(pack_entry("echovr")), "content_pack.id echovr is reserved");
}

#[test]
fn an_id_outside_the_pattern_is_refused() {
    for bad in ["a", "1combat", "Combat", "co-mbat", "x".repeat(33).as_str()] {
        let mut e = pack_entry("combat");
        e["content_pack"]["id"] = json!(bad);
        assert_eq!(refused(e), "content_pack.id is not a-z then 1 to 31 of a-z 0-9", "{bad}");
    }
}

#[test]
fn a_bad_version_is_refused() {
    for bad in ["1.0", "1.0.0.0", "v1.0.0", "1.0.0-beta", "1..0"] {
        let mut e = pack_entry("combat");
        e["content_pack"]["version"] = json!(bad);
        assert_eq!(refused(e), "content_pack.version is not X.Y.Z", "{bad}");
    }
}

#[test]
fn a_requires_list_that_is_empty_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["requires_stock_manifest_sha256"] = json!([]);
    assert_eq!(refused(e), "content_pack.requires_stock_manifest_sha256 holds 1 or 2 hashes");
}

#[test]
fn a_requires_list_of_three_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["requires_stock_manifest_sha256"] = json!([h('1'), h('2'), h('3')]);
    assert_eq!(refused(e), "content_pack.requires_stock_manifest_sha256 holds 1 or 2 hashes");
}

#[test]
fn an_unsorted_requires_list_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["requires_stock_manifest_sha256"] = json!([h('2'), h('1')]);
    assert_eq!(refused(e), "content_pack.requires_stock_manifest_sha256 is not sorted ascending");
}

#[test]
fn a_duplicated_requires_list_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["requires_stock_manifest_sha256"] = json!([h('1'), h('1')]);
    assert_eq!(refused(e), "content_pack.requires_stock_manifest_sha256 lists a hash twice");
}

#[test]
fn an_uppercase_requires_hash_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["requires_stock_manifest_sha256"] = json!(["A".repeat(64)]);
    assert_eq!(refused(e), "content_pack.requires_stock_manifest_sha256 holds a value that is not 64 lower-case hex digits");
}

#[test]
fn a_short_requires_hash_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["requires_stock_manifest_sha256"] = json!(["1".repeat(63)]);
    assert_eq!(refused(e), "content_pack.requires_stock_manifest_sha256 holds a value that is not 64 lower-case hex digits");
}

#[test]
fn a_bad_stock_data_version_is_refused() {
    for bad in [json!(""), json!("12a"), json!("9".repeat(21)), json!(5932408047u64)] {
        let mut e = pack_entry("combat");
        e["content_pack"]["stock_data_version"] = bad.clone();
        assert_eq!(refused(e), "content_pack.stock_data_version is not 1 to 20 digits", "{bad}");
    }
}

#[test]
fn a_duplicate_plugin_name_is_refused() {
    let mut e = pack_entry("combat");
    e["content_pack"]["plugins"] = json!(["combat-mod", "combat-mod"]);
    assert_eq!(refused(e), "content_pack.plugins lists a name twice");
}

#[test]
fn a_bad_plugin_name_is_refused() {
    for bad in ["has space", "a.b", "a/b", ""] {
        let mut e = pack_entry("combat");
        e["content_pack"]["plugins"] = json!([bad]);
        assert_eq!(refused(e), "content_pack.plugins holds a name that is not 1 to 64 of A-Z a-z 0-9 _ -", "{bad:?}");
    }
}

#[test]
fn a_platform_other_than_windows_x86_64_is_refused() {
    let mut e = pack_entry("combat");
    e["platform"] = json!("android-arm64");
    assert_eq!(refused(e), "a content-pack artifact's platform is not windows-x86_64");
}

#[test]
fn a_missing_content_pack_object_is_refused() {
    let mut e = pack_entry("combat");
    e.as_object_mut().unwrap().remove("content_pack");
    assert_eq!(refused(e), "a content-pack artifact has no content_pack object");
    let mut e = pack_entry("combat");
    e["content_pack"] = json!("combat");
    assert_eq!(refused(e), "a content-pack artifact has no content_pack object");
}

#[test]
fn an_empty_contents_list_is_refused() {
    let mut e = pack_entry("combat");
    e["contents"] = json!([]);
    assert_eq!(refused(e), "a content-pack artifact lists no contents");
}

#[test]
fn a_contents_path_with_dot_dot_is_refused() {
    let mut e = pack_entry("combat");
    e["contents"] = json!([{"path": "_data/../escape", "sha256": h('a'), "size": 1}]);
    let msg = refused(e);
    assert!(msg.contains("contents path"), "{msg}");
}

#[test]
fn a_contents_path_outside_the_strict_set_is_refused() {
    // `+` is printable ASCII, so the general relative-path check accepts it; the content-pack check does not.
    let mut e = pack_entry("combat");
    e["contents"] = json!([{"path": "_data/a+b", "sha256": h('a'), "size": 1}]);
    assert_eq!(refused(e), "a content-pack contents path is not made of a-z A-Z 0-9 . _ - segments: _data/a+b");
}

#[test]
fn two_content_packs_of_one_id_in_a_release_fail_find_content_pack() {
    let m = release(vec![pack_entry("combat"), {
        let mut b = pack_entry("combat");
        b["contents"][0]["size"] = json!(11);
        b
    }]);
    let err = manifest::find_content_pack(&m, "1.0.0", "combat").expect_err("two packs of one id");
    assert_eq!(err.step, 8);
    assert_eq!(err.msg, "release 1.0.0 has no content-pack artifact for id combat, or lists it twice");
}

#[test]
fn find_content_pack_picks_the_pack_of_the_named_id() {
    let m = release(vec![pack_entry("combat"), pack_entry("otherpack")]);
    let a = manifest::find_content_pack(&m, "1.0.0", "otherpack").unwrap();
    assert_eq!(a.content_pack.unwrap().id, "otherpack");
    let missing = manifest::find_content_pack(&m, "1.0.0", "nothere").unwrap_err();
    assert_eq!(missing.step, 8);
    assert_eq!(missing.msg, "release 1.0.0 has no content-pack artifact for id nothere, or lists it twice");
}

#[test]
fn a_game_data_entry_still_parses() {
    let m = release(vec![json!({
        "kind": "game-data",
        "platform": "android-arm64",
        "path": "data/echovr-data-aaaaaaaaaaaa.zip",
        "size": 20,
        "sha256": h('b'),
        "game_data": {"data_version": "5932408047", "echo_build": "5932408047"},
    })]);
    let a = manifest::find(&m, "1.0.0", "game-data", Some("android-arm64")).unwrap();
    assert_eq!(a.path, "data/echovr-data-aaaaaaaaaaaa.zip");
    assert_eq!(a.game_data.unwrap().data_version, "5932408047");
    assert!(a.content_pack.is_none());
}
