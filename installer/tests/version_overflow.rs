//! A version component past u64::MAX is unreadable: the installer refuses it, and never reads it as a smaller number
//! (#2062, the shape #1236 and #2056 fixed for `semver_cmp`).

use nevr_install::manifest;
use nevr_install::stage;
use serde_json::json;

const OVERFLOWING: &str = "99999999999999999999.0.0";
const READABLE: &str = "5.17.0";
const UNREADABLE_RESULT: &str = "install";
const SHA256_OF_NOTHING: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

fn game_data_artifact() -> serde_json::Value {
    json!({
        "kind": "game-data",
        "path": "data/game.zip",
        "size": 1,
        "sha256": SHA256_OF_NOTHING,
        "platform": "android-arm64",
        "game_data": {"data_version": "1", "echo_build": ""}
    })
}

#[test]
fn direction_refuses_an_overflowing_current_release() {
    // Read as 0.0.0 this was an upgrade to 5.17.0; the installer must not compare it at all.
    assert_eq!(stage::direction(OVERFLOWING, READABLE), UNREADABLE_RESULT);
}

#[test]
fn direction_refuses_an_overflowing_target_release() {
    // Read as 0.0.0 this was a downgrade from 5.17.0.
    assert_eq!(stage::direction(READABLE, OVERFLOWING), UNREADABLE_RESULT);
}

#[test]
fn direction_still_orders_readable_releases() {
    assert_eq!(stage::direction("5.17.0", "5.18.0"), "upgrade");
    assert_eq!(stage::direction("5.18.0", "5.17.0"), "downgrade");
    assert_eq!(stage::direction("5.17.0", "5.17.0"), "replace");
}

#[test]
fn latest_game_data_refuses_a_manifest_that_lists_an_overflowing_release() {
    let m = json!({
        "releases": {
            OVERFLOWING: {"artifacts": [game_data_artifact()]},
            READABLE: {"artifacts": [game_data_artifact()]}
        }
    });
    // The overflowing release used to sort as 0.0.0, so 5.17.0 was returned as the newest. It must be refused.
    assert!(manifest::latest_game_data(&m).is_none());
}

#[test]
fn latest_game_data_picks_the_newest_readable_release() {
    let m = json!({
        "releases": {
            "5.9.0": {"artifacts": [game_data_artifact()]},
            "5.17.0": {"artifacts": [game_data_artifact()]}
        }
    });
    let (version, _) = manifest::latest_game_data(&m).expect("two readable releases");
    assert_eq!(version, "5.17.0");
}
