//! Steps 2 to 8 of the method: the signature, the schema, the version floor, expiry, the stored state, the channel and
//! the shape of an artifact entry. This is the Rust reference of `docs/reference/verifying-a-release.md`, library-shaped:
//! it returns `Error { step, msg }` where the reference prints `FAIL step N: msg`, and reads each input once.

use crate::error::{Error, Result};
use crate::util::{data_path_ok, hex, is_sha256_hex, path_ok, rel_path_ok, semver_ok, sha256_hex, utc_ok, write_atomic};
use base64::{engine::general_purpose::STANDARD, Engine};
use minisign_verify::{PublicKey, Signature};
use serde_json::{json, Value};
use std::fs;
use std::io::ErrorKind;
use std::path::Path;

/// The `manifest_version` that was current when this build was made. Nothing at run time can lower it.
pub const MIN_MANIFEST_VERSION: u64 = 4;
/// Step 1 limits.
pub const MAX_MANIFEST_BYTES: u64 = 1 << 20;
pub const MAX_SIG_BYTES: u64 = 1024;

const BUILTIN_PINS: &str = include_str!("../pinned-keys.txt");

/// Pinned public keys. A manifest is accepted when a pinned, non-revoked key signed it.
#[derive(Debug, Clone)]
pub struct Pins {
    keys: Vec<(String, String)>, // (key id in file order, base64 key)
}

impl Pins {
    /// K1, K2 and KR.
    pub fn builtin() -> Pins {
        Pins::parse(BUILTIN_PINS).expect("the built-in pins parse")
    }

    /// `LABEL BASE64KEY` lines; blank lines and `#` lines are skipped.
    pub fn parse(text: &str) -> Result<Pins> {
        let mut keys = Vec::new();
        for line in text.lines().filter(|l| !l.trim().is_empty() && !l.starts_with('#')) {
            let b64 = line.split_whitespace().nth(1).ok_or_else(|| Error::new(2, format!("pinned key line has no key: {line}")))?;
            let raw = STANDARD.decode(b64).map_err(|e| Error::new(2, format!("pinned key is not base64: {e}")))?;
            if raw.len() != 42 {
                return Err(Error::new(2, "pinned key is not 42 bytes"));
            }
            keys.push((hex(&raw[2..10]), b64.to_string()));
        }
        if keys.is_empty() {
            return Err(Error::new(2, "no pinned keys"));
        }
        Ok(Pins { keys })
    }

    pub fn ids(&self) -> Vec<String> {
        self.keys.iter().map(|k| k.0.clone()).collect()
    }
}

/// What step 6 keeps between runs.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct State {
    pub highest: u64,
    pub revoked: Vec<String>,
    pub last_sha256: String,
}

/// state.json: a missing file or directory is a first run; any other read problem, or a file that does not hold what
/// it should, is a failure at step 6. An unreadable state must not turn into an empty one.
pub fn read_state(dir: &Path) -> Result<State> {
    let path = dir.join("state.json");
    let v: Value = match fs::read(&path) {
        Ok(b) => serde_json::from_slice(&b).map_err(|e| Error::new(6, format!("{}: {e}", path.display())))?,
        Err(e) if e.kind() == ErrorKind::NotFound => return Ok(State::default()),
        Err(e) => return Err(Error::new(6, format!("{}: {e}", path.display()))),
    };
    let bad = || Error::new(6, format!("{} is not a state file", path.display()));
    let obj = v.as_object().ok_or_else(bad)?;
    let highest = match obj.get("highest_manifest_version") {
        None => 0,
        Some(h) => h.as_u64().ok_or_else(bad)?,
    };
    let last_sha256 = match obj.get("last_manifest_sha256") {
        None => String::new(),
        Some(s) => s.as_str().ok_or_else(bad)?.to_string(),
    };
    let revoked = match obj.get("revoked_keys") {
        None => Vec::new(),
        Some(r) => r.as_array().ok_or_else(bad)?.iter().map(|k| k.as_str().map(str::to_string).ok_or_else(bad)).collect::<Result<_>>()?,
    };
    Ok(State { highest, revoked, last_sha256 })
}

/// The lock a run holds from its read of the stored state to the end of its commit (`commit`), so two runs that share a
/// state directory never interleave a read with another run's write. Released when dropped. A second run waits for it.
#[derive(Debug)]
pub struct StateLock(#[allow(dead_code)] fs::File); // the open file is the lock: it is held, never read, until the drop

/// Takes the exclusive state lock (`state.lock` in `dir`), blocking until the run holding it releases it. Never take it
/// twice in one process: the second call would wait on the first. Callers take it, `commit` does not.
pub fn lock_state(dir: &Path) -> Result<StateLock> {
    let fail = |e: std::io::Error| Error::new(6, format!("{}: {e}", dir.join("state.lock").display()));
    fs::create_dir_all(dir).map_err(fail)?;
    let f = fs::OpenOptions::new().create(true).write(true).truncate(false).open(dir.join("state.lock")).map_err(fail)?;
    f.lock().map_err(fail)?;
    Ok(StateLock(f))
}

/// The result of steps 2 to 5 (and the state step 6 will store). Nothing is written until `commit`.
#[derive(Debug, Clone)]
pub struct Verified {
    pub bytes: Vec<u8>,
    pub sig_text: String,
    pub value: Value,
    pub key_id: String,
    pub version: u64,
    pub sha256: String,
    pub new_state: State,
}

/// Steps 2 to 5 and the in-memory half of step 6, on bytes already read once. `now` is UTC as YYYY-MM-DDTHH:MM:SSZ.
pub fn verify(bytes: &[u8], sig: &[u8], pins: &Pins, state: &State, now: &str, min_version: u64) -> Result<Verified> {
    if !utc_ok(now) {
        return Err(Error::new(5, "the current time is not UTC written as YYYY-MM-DDTHH:MM:SSZ"));
    }
    if bytes.len() as u64 > MAX_MANIFEST_BYTES || sig.len() as u64 > MAX_SIG_BYTES {
        return Err(Error::new(1, "manifest.json is over 1 MiB or manifest.json.sig is over 1 KiB"));
    }
    // Step 2: before any JSON parser sees the manifest.
    let sig_text = String::from_utf8(sig.to_vec()).map_err(|_| Error::new(2, "signature file is not UTF-8"))?;
    let blob = sig_text.lines().nth(1).and_then(|l| STANDARD.decode(l).ok()).unwrap_or_default();
    if blob.len() != 74 {
        return Err(Error::new(2, "signature blob is not 74 bytes"));
    }
    if &blob[..2] != b"ED" {
        return Err(Error::new(2, "signature algorithm is not ED (hashed)"));
    }
    let id = hex(&blob[2..10]);
    if state.revoked.contains(&id) {
        return Err(Error::new(2, format!("signing key {id} is revoked")));
    }
    let key_b64 = pins.keys.iter().find(|k| k.0 == id).map(|k| k.1.as_str()).ok_or_else(|| Error::new(2, format!("signing key {id} is not pinned")))?;
    let key = PublicKey::from_base64(key_b64).map_err(|e| Error::new(2, format!("pinned key: {e}")))?;
    let signature = Signature::decode(&sig_text).map_err(|e| Error::new(2, format!("signature file: {e}")))?;
    key.verify(bytes, &signature, false).map_err(|e| Error::new(2, format!("signature does not verify: {e}")))?;

    // Step 3: the same bytes, now parsed.
    let m: Value = serde_json::from_slice(bytes).map_err(|e| Error::new(3, format!("manifest.json: {e}")))?;
    if m["schema_version"] != 1 || m["product"] != "nevr-runtime" {
        return Err(Error::new(3, "schema_version is not 1 or product is not nevr-runtime"));
    }
    let version = m["manifest_version"].as_u64().ok_or_else(|| Error::new(3, "manifest_version is not a whole number"))?;

    // Step 4: the stored highest and the compiled minimum. The equal-version test applies only when a manifest was
    // stored at that version.
    let have = state.highest.max(min_version);
    let sha = sha256_hex(bytes);
    if version < have {
        return Err(Error::new(4, format!("manifest version went down: got {version}, have {have}")));
    }
    if version == state.highest && state.highest > 0 && state.last_sha256 != sha {
        return Err(Error::new(4, "same manifest_version, different bytes"));
    }

    // Step 5: whole-second UTC strings with a Z suffix compare as times.
    let expires = m["expires"].as_str().filter(|e| utc_ok(e)).ok_or_else(|| Error::new(5, "expires is not UTC written as YYYY-MM-DDTHH:MM:SSZ"))?;
    if now >= expires {
        return Err(Error::new(5, format!("update information is stale since {expires}; check the clock or try later")));
    }

    // Step 6 (in memory): a revocation list that is present has to be a list of strings, never skipped.
    let listed: Vec<String> = match m.get("revoked_keys") {
        None | Some(Value::Null) => Vec::new(),
        Some(r) => r
            .as_array()
            .and_then(|a| a.iter().map(|k| k.as_str().map(str::to_string)).collect::<Option<Vec<_>>>())
            .ok_or_else(|| Error::new(6, "revoked_keys in the manifest is not a list of strings"))?,
    };
    let mut revoked = state.revoked.clone();
    for k in listed {
        if !revoked.contains(&k) {
            revoked.push(k);
        }
    }
    revoked.sort();
    Ok(Verified {
        bytes: bytes.to_vec(),
        sig_text,
        value: m,
        key_id: id,
        version,
        sha256: sha.clone(),
        new_state: State { highest: version, revoked, last_sha256: sha },
    })
}

/// Step 6: the manifest and its signature, then the state last, each a temporary file renamed over the old one. The caller
/// holds the state lock (`lock_state`) from its read of the state to this call. The stored version floor is re-read here
/// and only ever raised: a verified manifest older than the stored one leaves the stored pair as it is, so the floor and
/// the pair on disk always agree (the next run's step 4 would refuse a pair below its own floor).
pub fn commit(dir: &Path, v: &Verified) -> Result<()> {
    let fail = |what: &str, e: std::io::Error| Error::new(6, format!("{what}: {e}"));
    fs::create_dir_all(dir).map_err(|e| fail(&dir.display().to_string(), e))?;
    let stored = read_state(dir)?;
    let mut revoked = stored.revoked.clone();
    for k in &v.new_state.revoked {
        if !revoked.contains(k) {
            revoked.push(k.clone());
        }
    }
    revoked.sort();
    let (highest, last_sha256) = if v.version >= stored.highest {
        write_atomic(&dir.join("manifest.json"), &v.bytes).map_err(|e| fail("manifest.json", e))?;
        write_atomic(&dir.join("manifest.json.sig"), v.sig_text.as_bytes()).map_err(|e| fail("manifest.json.sig", e))?;
        (v.version, v.new_state.last_sha256.clone())
    } else {
        (stored.highest, stored.last_sha256.clone())
    };
    let state = json!({
        "highest_manifest_version": highest,
        "revoked_keys": revoked,
        "last_manifest_sha256": last_sha256,
    });
    write_atomic(&dir.join("state.json"), &serde_json::to_vec_pretty(&state).expect("state serializes")).map_err(|e| fail("state.json", e))
}

/// Step 8, first half: the manifest that passed step 2, read back from the state directory. It has to be the one
/// `state.json` names. No caller passes a manifest path. The pair is read under the state lock, so a run committing it
/// at the same moment cannot make this read a half-written pair.
pub fn stored(dir: &Path) -> Result<Value> {
    let _lock = lock_state(dir).map_err(|e| Error::new(8, e.msg))?;
    let state = read_state(dir).map_err(|e| Error::new(8, e.msg))?;
    let bytes = fs::read(dir.join("manifest.json")).map_err(|e| Error::new(8, format!("no verified manifest in {}: {e}", dir.display())))?;
    if state.last_sha256.is_empty() || sha256_hex(&bytes) != state.last_sha256 {
        return Err(Error::new(8, "the stored manifest is not the one that passed step 2"));
    }
    serde_json::from_slice(&bytes).map_err(|e| Error::new(8, format!("stored manifest: {e}")))
}

/// Step 7: the version a channel names, which must be a key of `releases`.
pub fn pick(m: &Value, channel: &str) -> Result<String> {
    let release = m["channels"][channel].as_str().ok_or_else(|| Error::new(7, format!("channel {channel} has no release")))?;
    if !semver_ok(release) {
        return Err(Error::new(7, format!("channel {channel} names a version that is not X.Y.Z")));
    }
    if m["releases"][release].is_null() {
        return Err(Error::new(7, format!("channel {channel} names {release}, which is not in releases")));
    }
    Ok(release.to_string())
}

/// `android` object of a `quest-apk` artifact.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Android {
    pub package: String,
    pub version_code: u64,
    /// Empty when the manifest carries none.
    pub signer_cert_sha256: Vec<String>,
}

/// One file of a zip artifact.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Content {
    pub path: String,
    pub sha256: String,
    pub size: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GameData {
    pub data_version: String,
    pub echo_build: String,
}

/// One artifact entry with every field checked for shape (step 8).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Artifact {
    pub kind: String,
    pub platform: Option<String>,
    pub path: String,
    pub size: u64,
    pub sha256: String,
    pub android: Option<Android>,
    pub game_data: Option<GameData>,
    pub contents: Vec<Content>,
}

/// The one artifact of `kind` (and `platform`, when given) in `version`. Zero or two is a failure naming the release.
pub fn find(m: &Value, version: &str, kind: &str, platform: Option<&str>) -> Result<Artifact> {
    if !semver_ok(version) {
        return Err(Error::new(8, format!("{version} is not X.Y.Z")));
    }
    let wanted: Vec<&Value> = m["releases"][version]["artifacts"]
        .as_array()
        .map(|a| a.iter().filter(|x| x["kind"] == kind && platform.is_none_or(|p| x["platform"] == p)).collect())
        .unwrap_or_default();
    if wanted.len() != 1 {
        return Err(Error::new(8, format!("release {version} has no {kind} artifact, or lists it twice")));
    }
    parse_artifact(version, wanted[0])
}

/// The `game-data` artifact of the newest release that lists one, with that release's version. Used to decide whether
/// the data on a device is complete when the chosen release lists none (one copy of the data serves every release).
pub fn latest_game_data(m: &Value) -> Option<(String, Artifact)> {
    let key = |v: &str| -> Vec<u64> { v.split('.').filter_map(|p| p.parse().ok()).collect() };
    let mut versions: Vec<&String> = m["releases"].as_object()?.keys().filter(|v| semver_ok(v)).collect();
    versions.sort_by_key(|v| key(v));
    versions.into_iter().rev().find_map(|v| find(m, v, "game-data", Some("android-arm64")).ok().map(|a| (v.clone(), a)))
}

/// Whether `version` lists an artifact of `kind` (and `platform`), without checking its shape.
pub fn has(m: &Value, version: &str, kind: &str, platform: Option<&str>) -> bool {
    m["releases"][version]["artifacts"]
        .as_array()
        .is_some_and(|a| a.iter().any(|x| x["kind"] == kind && platform.is_none_or(|p| x["platform"] == p)))
}

fn parse_artifact(version: &str, e: &Value) -> Result<Artifact> {
    let kind = e["kind"].as_str().unwrap_or("").to_string();
    let path = e["path"].as_str().unwrap_or("");
    let shape_ok = if kind == "game-data" { data_path_ok(path) } else { path_ok(version, path) };
    if !shape_ok {
        let want = if kind == "game-data" { "data/<name>.zip" } else { "v<version>/nevr-runtime-v<version>-<name>" };
        return Err(Error::new(8, format!("path in the manifest is not {want}")));
    }
    let size = e["size"].as_u64().ok_or_else(|| Error::new(8, "size in the manifest is not a whole number"))?;
    let sha256 = e["sha256"].as_str().filter(|s| is_sha256_hex(s)).ok_or_else(|| Error::new(8, "sha256 in the manifest is not 64 hex digits"))?.to_string();
    let android = match e.get("android") {
        None | Some(Value::Null) => None,
        Some(a) => {
            let package = a["package"].as_str().ok_or_else(|| Error::new(8, "the android object has no package"))?.to_string();
            let version_code = a["version_code"].as_u64().ok_or_else(|| Error::new(8, "the android object has no version_code"))?;
            let signer_cert_sha256 = match &a["signer_cert_sha256"] {
                Value::Null => Vec::new(),
                Value::String(s) => vec![s.clone()],
                Value::Array(list) => list.iter().map(|s| s.as_str().map(str::to_string)).collect::<Option<Vec<_>>>().ok_or_else(|| Error::new(8, "signer_cert_sha256 holds a value that is not a string"))?,
                _ => return Err(Error::new(8, "signer_cert_sha256 is not a string or a list of strings")),
            };
            if signer_cert_sha256.iter().any(|s| !is_sha256_hex(s)) {
                return Err(Error::new(8, "signer_cert_sha256 is not 64 lower-case hex digits"));
            }
            Some(Android { package, version_code, signer_cert_sha256 })
        }
    };
    let game_data = match e.get("game_data") {
        None | Some(Value::Null) => None,
        Some(g) => Some(GameData {
            data_version: g["data_version"].as_str().ok_or_else(|| Error::new(8, "game_data has no data_version"))?.to_string(),
            echo_build: g["echo_build"].as_str().unwrap_or("").to_string(),
        }),
    };
    let mut contents = Vec::new();
    if let Some(list) = e.get("contents").filter(|c| !c.is_null()) {
        for c in list.as_array().ok_or_else(|| Error::new(8, "contents is not a list"))? {
            let cpath = c["path"].as_str().filter(|p| rel_path_ok(p)).ok_or_else(|| Error::new(8, "a contents path is not a safe relative path"))?;
            let csize = c["size"].as_u64().ok_or_else(|| Error::new(8, format!("{cpath}: size is not a whole number")))?;
            let csha = c["sha256"].as_str().filter(|s| is_sha256_hex(s)).ok_or_else(|| Error::new(8, format!("{cpath}: sha256 is not 64 hex digits")))?;
            if contents.iter().any(|x: &Content| x.path == cpath) {
                return Err(Error::new(8, "the manifest lists an entry twice"));
            }
            contents.push(Content { path: cpath.to_string(), sha256: csha.to_string(), size: csize });
        }
    }
    Ok(Artifact {
        kind,
        platform: e["platform"].as_str().map(str::to_string),
        path: path.to_string(),
        size,
        sha256,
        android,
        game_data,
        contents,
    })
}
