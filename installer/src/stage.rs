//! `pc stage` (ADR 0024, section 3): steps 1 to 8 for the artifact a profile names, then the members the profile lists are
//! written into `<root>/update/staging/<version>`. Nothing is published unless every check passed; the folder is built
//! as `<version>.partial-<pid>` and renamed when complete. The host swaps it in at the next launch (ADR 0024, section 4).
//!
//! A `Profile` says which artifact kind to take and which members to stage; it is the one place the platforms differ:
//! - the PC profile (`PC_PROFILE`) takes the channel's `windows-zip` and stages `nevr-runtime.dll` and `plugins/*.dll`
//!   from it, each checked against the zip's `contents`; the Authenticode signature of each file is its anchor, so no
//!   proof is kept;
//! - the Quest profile (`QUEST_PROFILE`) takes the channel's `quest-runtime`, a single `.so` with no zip, checks it
//!   against the artifact's own `size` and `sha256`, and keeps the stored manifest pair that listed it in
//!   `<version>/proof/`: an ELF carries no signature, so the signed manifest is the proof a later check runs step 2 on.

use crate::artifact;
use crate::error::{Error, Result};
use crate::manifest::{self, Artifact, Content};
use crate::session::{Ended, Session};
use crate::util::{rel_path_ok, semver_cmp};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

pub const RUNTIME: &str = "nevr-runtime.dll";
/// The PC profile (ADR 0024 section 3): what `NevrPcStage` stages.
pub const PC_PROFILE: &str = r#"{"kind":"windows-zip","members":["nevr-runtime.dll","plugins/*.dll"]}"#;
/// The Quest profile: the runtime library, one file (`kQuestRuntimeMember` in `src/core/update/names.h`).
pub const QUEST_PROFILE: &str = r#"{"kind":"quest-runtime","members":["libnevr_runtime.so"]}"#;
/// A `<version>.partial-*` folder untouched for this long belongs to a stage that died.
const PARTIAL_STALE: Duration = Duration::from_secs(600);
/// The most members a profile lists.
const MAX_MEMBERS: usize = 16;
/// The longest name a wildcard member matches (`<64 characters>.dll`, the host's plugin rule in `src/core/update/names.h`).
const MAX_WILDCARD_NAME: usize = 68;
/// Where a single-file artifact's proof pair is kept beside it, and the two files of it.
const PROOF_DIR: &str = "proof";
const PROOF_FILES: [&str; 2] = ["manifest.json", "manifest.json.sig"];

/// How the artifact of a kind is shaped.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Form {
    /// A zip: the listed members are extracted, each checked against the artifact's `contents`.
    Zip,
    /// One file: the artifact itself, checked against its own `size` and `sha256`; no `contents` are read.
    File,
}

/// The artifact kinds the core stages and the shape of each. A profile that names any other kind is refused.
const KINDS: [(&str, Form); 2] = [("windows-zip", Form::Zip), ("quest-runtime", Form::File)];

/// One entry of a profile's `members`.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Member {
    /// This path, exactly.
    Exact(String),
    /// `<dir>/*<suffix>`: a plain name with that suffix directly under `dir` (`plugins/*.dll`).
    Glob { dir: String, suffix: String },
}

fn plain_name_bytes(s: &str) -> bool {
    s.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
}

impl Member {
    fn parse(spec: &str, form: Form) -> std::result::Result<Member, String> {
        if !spec.contains('*') {
            if !rel_path_ok(spec) {
                return Err(format!("member {spec:?} is not a safe relative path"));
            }
            if form == Form::File && !plain_name_bytes(spec) {
                return Err(format!("member {spec:?} of a single-file kind is not a plain file name"));
            }
            return Ok(Member::Exact(spec.to_string()));
        }
        let glob = spec.split_once('/').filter(|(dir, _)| rel_path_ok(dir) && plain_name_bytes(dir)).and_then(|(dir, rest)| {
            let suffix = rest.strip_prefix('*')?;
            let ext = suffix.strip_prefix('.')?;
            (!ext.is_empty() && ext.bytes().all(|b| b.is_ascii_alphanumeric())).then(|| Member::Glob { dir: dir.to_string(), suffix: suffix.to_string() })
        });
        match glob {
            Some(g) if form == Form::Zip => Ok(g),
            Some(_) => Err(format!("member {spec:?}: a single-file kind lists one plain file name")),
            None => Err(format!("member {spec:?} is not a path or <dir>/*.<ext>")),
        }
    }

    fn takes(&self, path: &str) -> bool {
        match self {
            Member::Exact(p) => p == path,
            Member::Glob { dir, suffix } => path.strip_prefix(dir.as_str()).and_then(|r| r.strip_prefix('/')).is_some_and(|n| {
                n.len() > suffix.len() && n.len() <= MAX_WILDCARD_NAME && n.ends_with(suffix.as_str()) && plain_name_bytes(n)
            }),
        }
    }
}

/// Which artifact kind a stage takes and which members it stages: `{"kind": "...", "members": ["...", ...]}`. The first member
/// is exact and names the runtime: the file the release is identified by (the current, quarantine and discard checks compare
/// its sha256). A single-file kind lists exactly that one member.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Profile {
    pub kind: String,
    pub form: Form,
    members: Vec<Member>,
}

impl Profile {
    pub fn parse(text: &str) -> Result<Profile> {
        let bad = |m: String| Error::new(9, format!("stage profile: {m}"));
        let v: Value = serde_json::from_str(text).map_err(|e| bad(format!("not JSON: {e}")))?;
        let obj = v.as_object().ok_or_else(|| bad("not an object".into()))?;
        if let Some(k) = obj.keys().find(|k| !matches!(k.as_str(), "kind" | "members")) {
            return Err(bad(format!("unknown key {k:?}")));
        }
        let kind = obj.get("kind").and_then(Value::as_str).ok_or_else(|| bad("has no kind".into()))?;
        let Some((_, form)) = KINDS.iter().find(|(k, _)| *k == kind) else {
            let known: Vec<&str> = KINDS.iter().map(|(k, _)| *k).collect();
            return Err(bad(format!("kind {kind:?} is not one the core stages ({})", known.join(", "))));
        };
        let list = obj.get("members").and_then(Value::as_array).ok_or_else(|| bad("has no members list".into()))?;
        if list.is_empty() || list.len() > MAX_MEMBERS {
            return Err(bad(format!("lists {} members, wants 1 to {MAX_MEMBERS}", list.len())));
        }
        let mut members: Vec<Member> = Vec::new();
        for item in list {
            let spec = item.as_str().ok_or_else(|| bad("a member is not a string".into()))?;
            let m = Member::parse(spec, *form).map_err(bad)?;
            if members.contains(&m) {
                return Err(bad(format!("lists member {spec:?} twice")));
            }
            members.push(m);
        }
        if !matches!(members[0], Member::Exact(_)) {
            return Err(bad("the first member names the runtime and is one exact path".into()));
        }
        if *form == Form::File && members.len() != 1 {
            return Err(bad("a single-file kind lists exactly one member".into()));
        }
        Ok(Profile { kind: kind.to_string(), form: *form, members })
    }

    /// The PC profile.
    pub fn pc() -> Profile {
        Profile::parse(PC_PROFILE).expect("the PC profile parses")
    }

    /// The runtime member's path.
    pub fn runtime(&self) -> &str {
        match &self.members[0] {
            Member::Exact(p) => p,
            Member::Glob { .. } => unreachable!("parse makes the first member exact"),
        }
    }

    /// Whether the profile stages the zip member `path`.
    pub fn takes(&self, path: &str) -> bool {
        self.members.iter().any(|m| m.takes(path))
    }
}

/// What the caller knows about the running release: its version, the sha256 of its runtime, and the releases that failed.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Current {
    pub version: String,
    pub runtime_sha256: String,
    pub quarantine: Vec<(String, String)>,
}

impl Current {
    pub fn parse(text: &str) -> Result<Current> {
        let v: Value = serde_json::from_str(text).map_err(|e| Error::new(0, format!("current_json is not JSON: {e}")))?;
        let s = |x: &Value| x.as_str().unwrap_or("").to_string();
        let quarantine = v["quarantine"].as_array().map(|a| a.iter().map(|q| (s(&q["version"]), s(&q["runtime_sha256"]))).collect()).unwrap_or_default();
        Ok(Current { version: s(&v["version"]), runtime_sha256: s(&v["runtime_sha256"]), quarantine })
    }
}

/// `nevr-runtime.dll`, or `plugins/<name>.dll` with a plain name: the only members the PC profile stages.
pub fn staged_member(path: &str) -> bool {
    Profile::pc().takes(path)
}

/// How the target relates to what runs now. A version that `semver_cmp` cannot read (not `X.Y.Z`, or a component past
/// u64::MAX) gives `install`: it is never compared as a smaller number (#2062).
pub fn direction(current: &str, target: &str) -> &'static str {
    match semver_cmp(target, current) {
        Some(std::cmp::Ordering::Greater) => "upgrade",
        Some(std::cmp::Ordering::Less) => "downgrade",
        Some(std::cmp::Ordering::Equal) => "replace",
        None => "install",
    }
}

fn clean_staging(dir: &Path, keep_version: Option<&str>, log: &crate::log::Log) -> Result<()> {
    let Ok(rd) = fs::read_dir(dir) else { return Ok(()) };
    let now = SystemTime::now();
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        let p = e.path();
        let stale_partial = name.contains(".partial-")
            && e.metadata().and_then(|m| m.modified()).ok().and_then(|t| now.duration_since(t).ok()).is_some_and(|age| age >= PARTIAL_STALE);
        let unpublished = !name.contains(".partial-") && Some(name.as_str()) != keep_version;
        if stale_partial || unpublished {
            let _ = fs::remove_dir_all(&p);
            log.info("stage_cleanup", 8, json!({"removed": name, "reason": if stale_partial { "stale_partial" } else { "unpublished" }}))?;
        }
    }
    Ok(())
}

fn members_intact(dir: &Path, wanted: &[Content]) -> bool {
    wanted.iter().all(|c| match crate::fetch::sha256_file(&dir.join(&c.path)) {
        Ok((n, h)) => n == c.size && h == c.sha256,
        Err(_) => false,
    })
}

/// The stored manifest pair a single-file artifact keeps as its proof, as bytes: manifest, then signature. It must be the pair
/// that lists exactly `art` for `version`: the stage chose the artifact from the stored manifest earlier, and another run
/// may have stored a newer pair since (the state lock is held per read, not across the stage).
fn proof_pair(state_dir: &Path, version: &str, art: &Artifact) -> Result<(Vec<u8>, Vec<u8>)> {
    let (m, manifest_bytes, sig) = manifest::stored_pair(state_dir)?;
    let listed = manifest::find(&m, version, &art.kind, art.platform.as_deref())?;
    if listed != *art {
        return Err(Error::new(8, "the stored manifest changed while the release was staged: it no longer lists the file that was fetched"));
    }
    Ok((manifest_bytes, sig))
}

fn write_proof(dir: &Path, pair: &(Vec<u8>, Vec<u8>)) -> Result<()> {
    let proof = dir.join(PROOF_DIR);
    fs::create_dir_all(&proof).map_err(|e| Error::new(9, format!("{}: {e}", proof.display())))?;
    for (name, bytes) in PROOF_FILES.iter().zip([&pair.0, &pair.1]) {
        crate::util::write_atomic(&proof.join(name), bytes).map_err(|e| Error::new(9, format!("{}: {e}", proof.join(name).display())))?;
    }
    Ok(())
}

fn proof_intact(dir: &Path, pair: &(Vec<u8>, Vec<u8>)) -> bool {
    PROOF_FILES.iter().zip([&pair.0, &pair.1]).all(|(name, bytes)| fs::read(dir.join(PROOF_DIR).join(name)).is_ok_and(|b| b == **bytes))
}

impl Session {
    /// Records that the runtime refused the staged release `version` whose runtime has `runtime_sha256` (its Authenticode check failed after
    /// the stage verified every byte against the manifest): the next stage answers `none: discarded` for that release and fetches nothing.
    /// The record is keyed by version and runtime hash, so a rebuilt release under the same version is staged again.
    pub fn pc_discard(&self, root: &Path, version: &str, runtime_sha256: &str, reason: &str) -> Result<(Ended, Value)> {
        if !version_name(version) || !sha256_name(runtime_sha256) || !reason_word(reason) {
            return Err(Error::new(9, "discard needs a version, a lowercase sha256 and a reason word"));
        }
        let mut all = read_discards(root);
        if !all.iter().any(|(v, h)| v == version && h == runtime_sha256) {
            all.push((version.to_string(), runtime_sha256.to_string()));
        }
        let doc = json!({"discards": all.iter().map(|(v, h)| json!({"version": v, "runtime_sha256": h})).collect::<Vec<_>>()});
        let path = discards_path(root);
        let io = |e: std::io::Error| Error::new(8, format!("{}: {e}", path.display()));
        if let Some(dir) = path.parent() {
            fs::create_dir_all(dir).map_err(io)?;
        }
        crate::util::write_atomic(&path, doc.to_string().as_bytes()).map_err(io)?;
        self.log.info("stage_discard", 8, json!({"version": version, "runtime_sha256": runtime_sha256, "reason": reason, "recorded": all.len()}))?;
        let report = json!({"action": "discarded", "version": version, "reason": reason});
        Ok((self.end("unchanged", &format!("discarded {version}: {reason}"))?, report))
    }

    /// Stages the release the channel names, unless it is the running one or a failed one. Returns the run's end and the
    /// report `{"action": "none"|"staged"|"blocked", "version", "direction", "files": [...], "reason"}`.
    pub fn pc_stage(&self, version: Option<&str>, root: &Path, cur: &Current) -> Result<(Ended, Value)> {
        let (reason, report) = self.pc_stage_body(version, root, cur)?;
        Ok((self.end("unchanged", &reason)?, report))
    }

    /// `pc_stage` without the `run_end` record, for a caller (the C ABI) that ends the run itself. Returns the end reason.
    pub fn pc_stage_body(&self, version: Option<&str>, root: &Path, cur: &Current) -> Result<(String, Value)> {
        self.stage_body(&Profile::pc(), version, root, cur)
    }

    /// The stage for any profile, without the `run_end` record (`NevrStage`). Returns the end reason and the report.
    pub fn stage_body(&self, profile: &Profile, version: Option<&str>, root: &Path, cur: &Current) -> Result<(String, Value)> {
        let (m, v) = self.release(version)?;
        let dir = direction(&cur.version, &v);
        let kind = profile.kind.as_str();
        let none = |action: &str, reason: &str| -> Result<(String, Value)> {
            self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": action, "reason": reason, "direction": dir}))?;
            let report = json!({"action": action, "version": v, "direction": dir, "reason": reason, "files": []});
            Ok((format!("{action}: {reason}"), report))
        };
        let listed = m["releases"][v.as_str()]["artifacts"].as_array().map(|a| a.iter().filter(|x| x["kind"] == kind).count()).unwrap_or(0);
        if listed == 0 {
            return none("blocked", &format!("no_{}", kind.replace('-', "_")));
        }
        let art = manifest::find(&m, &v, kind, None)?;
        let wanted: Vec<Content> = match profile.form {
            Form::Zip => art.contents.iter().filter(|c| profile.takes(&c.path)).cloned().collect(),
            Form::File => vec![Content { path: profile.runtime().to_string(), sha256: art.sha256.clone(), size: art.size }],
        };
        let Some(runtime_sha256) = wanted.iter().find(|c| c.path == profile.runtime()).map(|c| c.sha256.clone()) else { return none("blocked", "no_runtime_member") };
        if cur.quarantine.iter().any(|(qv, qs)| *qv == v && *qs == runtime_sha256) {
            return none("none", "quarantined");
        }
        if read_discards(root).iter().any(|(dv, ds)| *dv == v && *ds == runtime_sha256) {
            return none("none", "discarded");
        }
        if cur.version == v && cur.runtime_sha256 == runtime_sha256 {
            return none("none", "current");
        }
        // A single-file artifact keeps the pair that listed it (see the module comment); a zip's anchor is the signature in each file.
        let proof = match profile.form {
            Form::File => match proof_pair(&self.cfg.state_dir, &v, &art) {
                Ok(p) => Some(p),
                Err(e) => {
                    self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                    return Err(e);
                }
            },
            Form::Zip => None,
        };
        let stage_root = root.join("update").join("staging");
        if let Err(e) = fs::create_dir_all(&stage_root) {
            let msg = format!("the staging folder {}: {e}", stage_root.display());
            self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": msg, "direction": dir}))?;
            return Err(Error::new(8, msg));
        }
        let published = fs::read_to_string(root.join("update").join("staged.json")).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()).and_then(|j| j["version"].as_str().map(str::to_string));
        clean_staging(&stage_root, published.as_deref(), &self.log)?;
        let target = stage_root.join(&v);
        let report = |reason: &str| json!({"action": "staged", "version": v, "direction": dir, "reason": reason,
            "files": wanted.iter().map(|c| json!({"path": c.path, "size": c.size, "sha256": c.sha256})).collect::<Vec<_>>()});
        if published.as_deref() == Some(v.as_str()) && target.is_dir() && members_intact(&target, &wanted) && proof.as_ref().is_none_or(|p| proof_intact(&target, p)) {
            self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": "staged", "reason": "already_staged", "direction": dir, "files": wanted.len()}))?;
            return Ok(("staged: already_staged".to_string(), report("already_staged")));
        }
        let _ = fs::remove_dir_all(&target);

        let zip_path = self.cfg.work_dir.join(art.path.rsplit('/').next().unwrap_or("artifact"));
        fs::create_dir_all(&self.cfg.work_dir).map_err(|e| Error::new(8, format!("{}: {e}", self.cfg.work_dir.display())))?;
        let url = format!("{}/{}", self.cfg.base, art.path);
        let fetched = match artifact::download(&url, &art, &zip_path) {
            Ok(f) => f,
            Err(e) => {
                self.log.error("artifact_fetch", 8, json!({"kind": kind, "url": url, "size": art.size, "ok": false, "reason": e.msg}))?;
                self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                return Err(e);
            }
        };
        self.log.info("artifact_fetch", 8, json!({"kind": kind, "url": url, "size": art.size, "bytes": fetched.bytes, "sha256": fetched.sha256, "ok": true, "resumed_from": fetched.resumed_from}))?;
        let staged = (|| -> Result<()> {
            if profile.form == Form::Zip {
                artifact::verify_zip(&zip_path, &art, false)?;
            }
            let partial = stage_root.join(format!("{v}.partial-{}", std::process::id()));
            let _ = fs::remove_dir_all(&partial);
            fs::create_dir_all(&partial).map_err(|e| Error::new(8, format!("the staging folder {}: {e}", partial.display())))?;
            let removing = |_: &Error| {
                let _ = fs::remove_dir_all(&partial);
            };
            let written = match profile.form {
                Form::Zip => artifact::extract(&zip_path, &Artifact { contents: wanted.clone(), ..art.clone() }, &partial),
                Form::File => artifact::place_file(&zip_path, &art, profile.runtime(), &partial).map(|p| vec![p]),
            }
            .inspect_err(removing)?;
            if let Some(pair) = &proof {
                write_proof(&partial, pair).inspect_err(removing)?;
            }
            for (c, p) in wanted.iter().zip(written.iter()) {
                self.log.info("stage_member", 8, json!({"path": c.path, "size": c.size, "sha256": c.sha256, "ok": true, "file": p.file_name().map(|n| n.to_string_lossy().to_string())}))?;
            }
            fs::rename(&partial, &target).map_err(|e| {
                let _ = fs::remove_dir_all(&partial);
                Error::new(8, format!("the staging folder {}: {e}", target.display()))
            })?;
            Ok(())
        })();
        let _ = fs::remove_file(&zip_path);
        match staged {
            Ok(()) => {
                self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": "staged", "direction": dir, "files": wanted.len()}))?;
                Ok((format!("staged {v} ({dir})"), report("staged")))
            }
            Err(e) => {
                self.log.error("stage_member", 8, json!({"ok": false, "reason": e.msg}))?;
                self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                Err(e)
            }
        }
    }
}

/// Where the stage records the releases the runtime refused after staging them (`NevrPcDiscard`): in the core's own state folder, so the
/// next stage does not fetch them again. A file the core cannot read is an empty record: a stage is never blocked by damage.
pub fn discards_path(root: &Path) -> PathBuf {
    root.join("update").join("manifest").join("discards.json")
}

/// The releases recorded in `discards.json`, as (version, runtime sha256).
pub fn read_discards(root: &Path) -> Vec<(String, String)> {
    let Ok(text) = fs::read_to_string(discards_path(root)) else { return Vec::new() };
    let Ok(v) = serde_json::from_str::<Value>(&text) else { return Vec::new() };
    v["discards"]
        .as_array()
        .map(|a| a.iter().filter_map(|e| Some((e["version"].as_str()?.to_string(), e["runtime_sha256"].as_str()?.to_string()))).collect())
        .unwrap_or_default()
}

fn version_name(v: &str) -> bool {
    !v.is_empty() && v.len() <= 64 && v.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'+' | b'-')) && !v.starts_with('.') && !v.ends_with('.')
}

fn sha256_name(h: &str) -> bool {
    h.len() == 64 && h.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn reason_word(r: &str) -> bool {
    !r.is_empty() && r.len() <= 64 && r.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

/// The folders `pc stage` uses under a nevr root.
pub fn paths(root: &Path) -> (PathBuf, PathBuf, PathBuf) {
    (root.join("update").join("manifest"), root.join("logs").join("nevr-update.jsonl"), root.join("update").join("work"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::sha256_hex;

    const SHA_A: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const SHA_B: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    fn manifest_listing(sha: &str) -> Vec<u8> {
        json!({"releases": {"1.0.0": {"artifacts": [{"kind": "quest-runtime", "platform": "android-arm64", "path": "v1.0.0/nevr-runtime-v1.0.0-quest-runtime.so", "size": 5, "sha256": sha}]}}})
            .to_string()
            .into_bytes()
    }

    fn store(dir: &Path, manifest: &[u8], sig: &[u8]) {
        fs::write(dir.join("manifest.json"), manifest).unwrap();
        fs::write(dir.join("manifest.json.sig"), sig).unwrap();
        fs::write(dir.join("state.json"), json!({"last_manifest_sha256": sha256_hex(manifest)}).to_string()).unwrap();
    }

    fn artifact_listed_by(manifest: &[u8]) -> Artifact {
        manifest::find(&serde_json::from_slice(manifest).unwrap(), "1.0.0", "quest-runtime", None).unwrap()
    }

    #[test]
    fn the_proof_is_the_stored_pair_that_lists_the_file_that_was_fetched() {
        let d = tempfile::tempdir().unwrap();
        let (a, b) = (manifest_listing(SHA_A), manifest_listing(SHA_B));
        let fetched = artifact_listed_by(&a);
        store(d.path(), &a, b"signature a");
        assert_eq!(proof_pair(d.path(), "1.0.0", &fetched).unwrap(), (a.clone(), b"signature a".to_vec()));
        // Another run stored a newer pair, between the choice of the artifact and the copy: that pair lists other bytes.
        store(d.path(), &b, b"signature b");
        let e = proof_pair(d.path(), "1.0.0", &fetched).unwrap_err();
        assert!(e.msg.contains("changed while the release was staged"), "{e}");
    }

    #[test]
    fn the_proof_needs_the_stored_signature() {
        let d = tempfile::tempdir().unwrap();
        let a = manifest_listing(SHA_A);
        store(d.path(), &a, b"x");
        fs::remove_file(d.path().join("manifest.json.sig")).unwrap();
        assert!(proof_pair(d.path(), "1.0.0", &artifact_listed_by(&a)).unwrap_err().msg.contains("no stored signature"));
    }

    #[test]
    fn the_pc_profile_stages_the_runtime_and_plain_plugin_names_only() {
        for (path, want) in [
            ("nevr-runtime.dll", true),
            ("plugins/ok-1_a.dll", true),
            ("plugins/.dll", false),
            ("plugins/notes.txt", false),
            ("plugins/sub/x.dll", false),
            ("plugins/x.dll/", false),
            ("Plugins/x.dll", false),
            ("BugSplat64.dll", false),
            ("x/plugins/x.dll", false),
        ] {
            assert_eq!(staged_member(path), want, "{path}");
        }
        assert!(staged_member(&format!("plugins/{}.dll", "a".repeat(64))));
        assert!(!staged_member(&format!("plugins/{}.dll", "a".repeat(65))));
    }

    #[test]
    fn the_quest_profile_names_one_file_and_the_pc_profile_names_the_runtime_first() {
        let q = Profile::parse(QUEST_PROFILE).unwrap();
        assert_eq!((q.kind.as_str(), q.form, q.runtime()), ("quest-runtime", Form::File, "libnevr_runtime.so"));
        assert!(q.takes("libnevr_runtime.so") && !q.takes("other.so"));
        let p = Profile::pc();
        assert_eq!((p.kind.as_str(), p.form, p.runtime()), ("windows-zip", Form::Zip, "nevr-runtime.dll"));
    }
}
