//! `pc stage` (ADR 0024, section 3): steps 1 to 8 for the channel's `windows-zip`, then the runtime and the plugin members
//! are extracted into `<root>/update/staging/<version>`. Nothing is published unless every check passed; the folder is built
//! as `<version>.partial-<pid>` and renamed when complete. The host swaps it in at the next launch (ADR 0024, section 4).

use crate::artifact;
use crate::error::{Error, Result};
use crate::manifest::{self, Artifact, Content};
use crate::session::{Ended, Session};
use crate::util::semver_ok;
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

pub const RUNTIME: &str = "nevr-runtime.dll";
/// A `<version>.partial-*` folder untouched for this long belongs to a stage that died.
const PARTIAL_STALE: Duration = Duration::from_secs(600);

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

/// `nevr-runtime.dll`, or `plugins/<name>.dll` with a plain name: the only members a stage extracts.
pub fn staged_member(path: &str) -> bool {
    if path == RUNTIME {
        return true;
    }
    path.strip_prefix("plugins/").is_some_and(|n| {
        n.len() > 4 && n.len() <= 68 && n.ends_with(".dll") && n.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
    })
}

fn triple(v: &str) -> Option<(u64, u64, u64)> {
    if !semver_ok(v) {
        return None;
    }
    let mut it = v.split('.').map(|p| p.parse::<u64>().unwrap_or(0));
    Some((it.next()?, it.next()?, it.next()?))
}

/// How the target relates to what runs now.
pub fn direction(current: &str, target: &str) -> &'static str {
    match (triple(current), triple(target)) {
        (Some(c), Some(t)) if t > c => "upgrade",
        (Some(c), Some(t)) if t < c => "downgrade",
        (Some(_), Some(_)) => "replace",
        _ => "install",
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

fn members_intact(dir: &Path, wanted: &[&Content]) -> bool {
    wanted.iter().all(|c| match crate::fetch::sha256_file(&dir.join(&c.path)) {
        Ok((n, h)) => n == c.size && h == c.sha256,
        Err(_) => false,
    })
}

impl Session {
    /// Stages the release the channel names, unless it is the running one or a failed one. Returns the run's end and the
    /// report `{"action": "none"|"staged"|"blocked", "version", "direction", "files": [...], "reason"}`.
    pub fn pc_stage(&self, version: Option<&str>, root: &Path, cur: &Current) -> Result<(Ended, Value)> {
        let (reason, report) = self.pc_stage_body(version, root, cur)?;
        Ok((self.end("unchanged", &reason)?, report))
    }

    /// `pc_stage` without the `run_end` record, for a caller (the C ABI) that ends the run itself. Returns the end reason.
    pub fn pc_stage_body(&self, version: Option<&str>, root: &Path, cur: &Current) -> Result<(String, Value)> {
        let (m, v) = self.release(version)?;
        let dir = direction(&cur.version, &v);
        let none = |action: &str, reason: &str| -> Result<(String, Value)> {
            self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": action, "reason": reason, "direction": dir}))?;
            let report = json!({"action": action, "version": v, "direction": dir, "reason": reason, "files": []});
            Ok((format!("{action}: {reason}"), report))
        };
        let zips = m["releases"][v.as_str()]["artifacts"].as_array().map(|a| a.iter().filter(|x| x["kind"] == "windows-zip").count()).unwrap_or(0);
        if zips == 0 {
            return none("blocked", "no_windows_zip");
        }
        let art = manifest::find(&m, &v, "windows-zip", None)?;
        let Some(runtime) = art.contents.iter().find(|c| c.path == RUNTIME) else { return none("blocked", "no_runtime_member") };
        if cur.quarantine.iter().any(|(qv, qs)| *qv == v && *qs == runtime.sha256) {
            return none("none", "quarantined");
        }
        if cur.version == v && cur.runtime_sha256 == runtime.sha256 {
            return none("none", "current");
        }
        let wanted: Vec<&Content> = art.contents.iter().filter(|c| staged_member(&c.path)).collect();
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
        if published.as_deref() == Some(v.as_str()) && target.is_dir() && members_intact(&target, &wanted) {
            self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": "staged", "reason": "already_staged", "direction": dir, "files": wanted.len()}))?;
            return Ok(("staged: already_staged".to_string(), report("already_staged")));
        }
        let _ = fs::remove_dir_all(&target);

        let zip_path = self.cfg.work_dir.join(art.path.rsplit('/').next().unwrap_or("windows.zip"));
        fs::create_dir_all(&self.cfg.work_dir).map_err(|e| Error::new(8, format!("{}: {e}", self.cfg.work_dir.display())))?;
        let url = format!("{}/{}", self.cfg.base, art.path);
        let fetched = match artifact::download(&url, &art, &zip_path) {
            Ok(f) => f,
            Err(e) => {
                self.log.error("artifact_fetch", 8, json!({"kind": "windows-zip", "url": url, "size": art.size, "ok": false, "reason": e.msg}))?;
                self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                return Err(e);
            }
        };
        self.log.info("artifact_fetch", 8, json!({"kind": "windows-zip", "url": url, "size": art.size, "bytes": fetched.bytes, "sha256": fetched.sha256, "ok": true, "resumed_from": fetched.resumed_from}))?;
        let staged = (|| -> Result<()> {
            artifact::verify_zip(&zip_path, &art, false)?;
            let partial = stage_root.join(format!("{v}.partial-{}", std::process::id()));
            let _ = fs::remove_dir_all(&partial);
            fs::create_dir_all(&partial).map_err(|e| Error::new(8, format!("the staging folder {}: {e}", partial.display())))?;
            let subset = Artifact { contents: wanted.iter().map(|c| (*c).clone()).collect(), ..art.clone() };
            let written = artifact::extract(&zip_path, &subset, &partial).inspect_err(|_| {
                let _ = fs::remove_dir_all(&partial);
            })?;
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

/// The folders `pc stage` uses under a nevr root.
pub fn paths(root: &Path) -> (PathBuf, PathBuf, PathBuf) {
    (root.join("update").join("manifest"), root.join("logs").join("nevr-update.jsonl"), root.join("update").join("work"))
}
