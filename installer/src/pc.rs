//! The PC plan (ADR 0023, section 2): the semantics of `tools/package-release/install.ps1`, made data and tested.
//! The game's own `BugSplat64.dll` is set aside, never deleted; `nevr-runtime.dll` is written before the host that loads
//! it; both are read back; any failure puts every file back; `nevr-install.txt` keeps the keys `uninstall.ps1` reads.

use crate::error::{Error, Result};
use crate::log::Log;
use crate::util::{now_utc, sha256_hex};
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const HOST: &str = "BugSplat64.dll";
pub const RUNTIME: &str = "nevr-runtime.dll";
pub const STATE_FILE: &str = "nevr-install.txt";

/// The files the plan installs, taken from the verified zip.
#[derive(Debug, Clone)]
pub struct Package {
    pub host: Vec<u8>,
    pub runtime: Vec<u8>,
}

impl Package {
    pub fn host_sha(&self) -> String {
        sha256_hex(&self.host)
    }
    pub fn runtime_sha(&self) -> String {
        sha256_hex(&self.runtime)
    }
}

/// `nevr-install.txt`: `key=value` lines, UTF-8 without a BOM.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Record {
    pub values: BTreeMap<String, String>,
}

impl Record {
    pub fn parse(text: &str) -> Record {
        let mut values = BTreeMap::new();
        for line in text.lines() {
            if let Some((k, v)) = line.split_once('=') {
                if !k.is_empty() && k.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_') {
                    values.insert(k.to_string(), v.to_string());
                }
            }
        }
        Record { values }
    }

    pub fn get(&self, k: &str) -> Option<&str> {
        self.values.get(k).map(String::as_str)
    }

    /// Every host hash this folder's installs put there: the recorded one and the earlier ones.
    pub fn known(&self) -> Vec<String> {
        let mut k = vec![self.get("installed_sha256").unwrap_or("").to_string()];
        if let Some(p) = self.get("previous_sha256").filter(|p| !p.is_empty()) {
            k.extend(p.split(',').map(str::to_string));
        }
        k
    }

    pub fn render(backup: &str, legacy: &str, previous: Option<&str>, runtime_aside: &str, installed: &str, runtime: &str) -> String {
        let mut lines = vec![
            format!("backup={backup}"),
            format!("legacy={legacy}"),
            format!("runtime_aside={runtime_aside}"),
            format!("installed_sha256={installed}"),
            format!("runtime_sha256={runtime}"),
        ];
        if let Some(p) = previous {
            lines.push(format!("previous_sha256={p}"));
        }
        lines.join("\n") + "\n"
    }
}

/// A NEVR runtime carries the `NEVR-BUILD ` identity literal; one built before it carries the CDN user agent.
pub fn is_nevr_runtime(bytes: &[u8]) -> bool {
    let has = |needle: &[u8]| bytes.windows(needle.len()).any(|w| w == needle);
    has(b"NEVR-BUILD ") || has(b"nevr-runtime/1.0")
}

#[derive(Debug, Clone, Default)]
pub struct Observed {
    pub has_exe: bool,
    pub game_running: bool,
    pub target_sha: Option<String>,
    pub target_is_nevr: bool,
    pub record: Option<Record>,
    pub backup_exists: bool,
    pub backup_is_nevr: bool,
    pub backup_sha: Option<String>,
    pub runtime_sha: Option<String>,
    pub dbgcore_present: bool,
}

pub fn observe(dir: &Path, game_running: bool) -> Result<Observed> {
    let io = |what: &Path, e: std::io::Error| Error::new(9, format!("{}: {e}", what.display()));
    let read_opt = |p: &Path| -> Result<Option<Vec<u8>>> {
        match fs::read(p) {
            Ok(b) => Ok(Some(b)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(io(p, e)),
        }
    };
    let target = read_opt(&dir.join(HOST))?;
    let runtime = read_opt(&dir.join(RUNTIME))?;
    let record = read_opt(&dir.join(STATE_FILE))?.map(|b| Record::parse(&String::from_utf8_lossy(&b)));
    let backup = match record.as_ref().and_then(|r| r.get("backup")).filter(|b| !b.is_empty()) {
        Some(b) => read_opt(Path::new(b))?,
        None => None,
    };
    Ok(Observed {
        has_exe: dir.join("echovr.exe").is_file(),
        game_running,
        target_is_nevr: target.as_deref().is_some_and(is_nevr_runtime),
        target_sha: target.as_deref().map(sha256_hex),
        record,
        backup_exists: backup.is_some(),
        backup_is_nevr: backup.as_deref().is_some_and(is_nevr_runtime),
        backup_sha: backup.as_deref().map(sha256_hex),
        runtime_sha: runtime.as_deref().map(sha256_hex),
        dbgcore_present: dir.join("dbgcore.dll").exists(),
    })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    /// Set the game's own file aside and install.
    First,
    /// A NEVR host this folder's install put there is replaced; the first original stays.
    Update,
    /// This package's host was copied in by hand: record it and bring the runtime beside it.
    RecordOnly,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Proceed,
    Unchanged,
    NeedsUser,
}

#[derive(Debug, Clone)]
pub struct Plan {
    pub outcome: Outcome,
    pub action: Option<Action>,
    pub decision: String,
    pub reasons: Vec<String>,
    /// A record that no longer describes the folder (Verify Files restored the game's own file) is set aside first.
    pub stale_record: bool,
}

impl Plan {
    pub fn to_json(&self) -> Value {
        json!({
            "outcome": match self.outcome { Outcome::Proceed => "proceed", Outcome::Unchanged => "unchanged", Outcome::NeedsUser => "needs-user" },
            "decision": self.decision,
            "reasons": self.reasons,
            "action": self.action.map(|a| format!("{a:?}")),
            "stale_record": self.stale_record,
        })
    }
}

pub fn plan(obs: &Observed, pkg: &Package) -> Plan {
    let ours = pkg.host_sha();
    let ours_rt = pkg.runtime_sha();
    let p = |outcome, action, decision: &str, reason: Option<String>, stale: bool| Plan {
        outcome,
        action,
        decision: decision.into(),
        reasons: reason.into_iter().collect(),
        stale_record: stale,
    };
    let needs = |why: &str| p(Outcome::NeedsUser, None, "needs-user", Some(why.to_string()), false);
    if !obs.has_exe {
        return needs("no echovr.exe in the folder");
    }
    if obs.game_running {
        return needs("echovr.exe is running; close the game first");
    }
    let Some(orig) = obs.target_sha.as_deref() else {
        return needs("no BugSplat64.dll in the folder to set aside");
    };
    let mut record = obs.record.clone();
    let mut stale = false;
    if let Some(r) = &record {
        let known = r.known();
        let recorded_backup_is_orig = obs.backup_exists && obs.backup_sha.as_deref() == Some(orig);
        if orig != ours && !known.iter().any(|k| k == orig) && recorded_backup_is_orig && !obs.target_is_nevr {
            stale = true;
            record = None;
        }
    }
    match record {
        Some(r) => {
            for key in ["backup", "legacy", "installed_sha256"] {
                if r.get(key).is_none() {
                    return needs(&format!("nevr-install.txt has no {key}"));
                }
            }
            if !obs.backup_exists {
                return needs("the backup named in nevr-install.txt is gone; nothing was changed");
            }
            if obs.dbgcore_present {
                return needs("a dbgcore.dll exists again; run uninstall.ps1 first");
            }
            if obs.backup_is_nevr {
                return needs("the recorded backup is a NEVR runtime, not the game's own file; restore the game's BugSplat64.dll (Verify Files), delete nevr-install.txt, then run again");
            }
            if orig == ours {
                let runtime_current = obs.runtime_sha.as_deref() == Some(ours_rt.as_str());
                if r.get("installed_sha256") == Some(ours.as_str()) && runtime_current {
                    return p(Outcome::Unchanged, None, "unchanged", Some("the installed BugSplat64.dll and nevr-runtime.dll are already this package".into()), false);
                }
                return p(Outcome::Proceed, Some(Action::RecordOnly), "record", None, false);
            }
            if !r.known().iter().any(|k| k == orig) {
                return needs(if obs.target_is_nevr {
                    "BugSplat64.dll is a NEVR runtime this installer did not record; run uninstall.ps1 first, then install again"
                } else {
                    "BugSplat64.dll is neither one this installer put there nor a NEVR runtime; restore the game's own file (Verify Files), delete nevr-install.txt, then run again"
                });
            }
            p(Outcome::Proceed, Some(Action::Update), "update", None, false)
        }
        None => {
            if orig == ours {
                return p(Outcome::Unchanged, None, "unchanged", Some("the installed BugSplat64.dll is already this package".into()), stale);
            }
            if obs.target_is_nevr {
                return needs("the installed BugSplat64.dll is a NEVR runtime, not the game's own file, and nevr-install.txt is missing; restore the game's original (Verify Files), then run again");
            }
            p(Outcome::Proceed, Some(Action::First), "install", None, stale)
        }
    }
}

fn stamp() -> String {
    let t = now_utc().unwrap_or_else(|_| "19700101T000000Z".into());
    t.replace(['-', ':'], "").replace('T', "-").trim_end_matches('Z').to_string()
}

fn verify_file(path: &Path, sha: &str) -> std::result::Result<(), String> {
    let b = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if sha256_hex(&b) != sha {
        return Err(format!("{} does not match the package after writing", path.display()));
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct End {
    pub state: &'static str,
    pub reason: String,
}

/// Executes a `Proceed` plan in `dir` with the verified package. Every change is logged as a `file_write`.
pub fn execute(dir: &Path, plan: &Plan, obs: &Observed, pkg: &Package, log: &Log) -> Result<End> {
    let action = plan.action.expect("a Proceed plan has an action");
    let target = dir.join(HOST);
    let runtime_target = dir.join(RUNTIME);
    let state_path = dir.join(STATE_FILE);
    let ours = pkg.host_sha();
    let ours_rt = pkg.runtime_sha();
    let orig = obs.target_sha.clone().expect("observed");
    let st = stamp();
    let write_log = |path: &Path, sha: &str, replaced: &str, aside: &str| log.info("file_write", 9, json!({"path": path.display().to_string(), "sha256": sha, "replaced_sha256": replaced, "set_aside": aside}));

    if plan.stale_record {
        let aside = dir.join(format!("{STATE_FILE}.stale-{st}"));
        if aside.exists() {
            return Ok(End { state: "unchanged", reason: format!("name exists: {}", aside.display()) });
        }
        fs::rename(&state_path, &aside).map_err(|e| Error::new(9, format!("{}: {e}", aside.display())))?;
        write_log(&state_path, "", "", &aside.display().to_string())?;
    }

    let record = if plan.stale_record { None } else { obs.record.clone() };
    match action {
        Action::RecordOnly => {
            let r = record.expect("record");
            if obs.runtime_sha.as_deref() != Some(ours_rt.as_str()) {
                fs::write(&runtime_target, &pkg.runtime).map_err(|e| Error::new(9, format!("{}: {e}", runtime_target.display())))?;
                write_log(&runtime_target, &ours_rt, obs.runtime_sha.as_deref().unwrap_or(""), "")?;
                if let Err(why) = verify_file(&runtime_target, &ours_rt) {
                    return Ok(End { state: "needs-user", reason: why });
                }
            }
            let previous: Vec<String> = r.known().into_iter().filter(|k| *k != ours && !k.is_empty()).collect::<std::collections::BTreeSet<_>>().into_iter().collect();
            let text = Record::render(r.get("backup").unwrap_or(""), r.get("legacy").unwrap_or(""), Some(&previous.join(",")), r.get("runtime_aside").unwrap_or(""), &ours, &ours_rt);
            fs::write(&state_path, text).map_err(|e| Error::new(9, format!("{}: {e}", state_path.display())))?;
            Ok(End { state: "installed", reason: String::new() })
        }
        Action::Update => {
            let r = record.expect("record");
            let prior = fs::read(&target).map_err(|e| Error::new(9, format!("{}: {e}", target.display())))?;
            let prior_runtime = fs::read(&runtime_target).ok();
            let attempt = (|| -> std::result::Result<(), String> {
                fs::write(&runtime_target, &pkg.runtime).map_err(|e| e.to_string())?;
                write_log(&runtime_target, &ours_rt, obs.runtime_sha.as_deref().unwrap_or(""), "").map_err(|e| e.to_string())?;
                verify_file(&runtime_target, &ours_rt)?;
                fs::write(&target, &pkg.host).map_err(|e| e.to_string())?;
                write_log(&target, &ours, &orig, "").map_err(|e| e.to_string())?;
                verify_file(&target, &ours)
            })();
            if let Err(why) = attempt {
                let _ = fs::write(&target, &prior);
                match prior_runtime {
                    Some(b) => {
                        let _ = fs::write(&runtime_target, b);
                    }
                    None => {
                        let _ = fs::rename(&runtime_target, dir.join(format!("{RUNTIME}.failed-{st}")));
                    }
                }
                return Ok(End { state: "restored", reason: why });
            }
            let previous: Vec<String> = r.known().into_iter().filter(|k| *k != ours && !k.is_empty()).collect::<std::collections::BTreeSet<_>>().into_iter().collect();
            let text = Record::render(r.get("backup").unwrap_or(""), r.get("legacy").unwrap_or(""), Some(&previous.join(",")), r.get("runtime_aside").unwrap_or(""), &ours, &ours_rt);
            fs::write(&state_path, text).map_err(|e| Error::new(9, format!("{}: {e}", state_path.display())))?;
            Ok(End { state: "installed", reason: String::new() })
        }
        Action::First => {
            let backup = dir.join(format!("{HOST}.original-{st}"));
            if backup.exists() {
                return Ok(End { state: "unchanged", reason: format!("backup name exists: {}", backup.display()) });
            }
            fs::copy(&target, &backup).map_err(|e| Error::new(9, format!("{}: {e}", backup.display())))?;
            if let Err(why) = verify_file(&backup, &orig) {
                return Ok(End { state: "unchanged", reason: format!("{why} (delete the bad copy by hand)") });
            }
            write_log(&backup, &orig, "", &target.display().to_string())?;
            let mut legacy_aside: Option<PathBuf> = None;
            if obs.dbgcore_present {
                let aside = dir.join(format!("dbgcore.dll.legacy-{}", &st[..8]));
                if aside.exists() {
                    return Ok(End { state: "unchanged", reason: format!("name exists: {}", aside.display()) });
                }
                fs::rename(dir.join("dbgcore.dll"), &aside).map_err(|e| Error::new(9, format!("{}: {e}", aside.display())))?;
                write_log(&dir.join("dbgcore.dll"), "", "", &aside.display().to_string())?;
                legacy_aside = Some(aside);
            }
            let mut runtime_aside: Option<PathBuf> = None;
            if obs.runtime_sha.is_some() {
                let aside = dir.join(format!("{RUNTIME}.original-{st}"));
                if aside.exists() {
                    return Ok(End { state: "unchanged", reason: format!("name exists: {}", aside.display()) });
                }
                fs::rename(&runtime_target, &aside).map_err(|e| Error::new(9, format!("{}: {e}", aside.display())))?;
                write_log(&runtime_target, "", obs.runtime_sha.as_deref().unwrap_or(""), &aside.display().to_string())?;
                runtime_aside = Some(aside);
            }
            let attempt = (|| -> std::result::Result<(), String> {
                // The runtime first: until the host is replaced nothing loads it.
                fs::write(&runtime_target, &pkg.runtime).map_err(|e| e.to_string())?;
                write_log(&runtime_target, &ours_rt, "", "").map_err(|e| e.to_string())?;
                verify_file(&runtime_target, &ours_rt)?;
                fs::write(&target, &pkg.host).map_err(|e| e.to_string())?;
                write_log(&target, &ours, &orig, &backup.display().to_string()).map_err(|e| e.to_string())?;
                verify_file(&target, &ours)
            })();
            if let Err(why) = attempt {
                let _ = fs::copy(&backup, &target);
                if runtime_target.exists() {
                    let _ = fs::rename(&runtime_target, dir.join(format!("{RUNTIME}.failed-{st}")));
                }
                if let Some(a) = &runtime_aside {
                    let _ = fs::rename(a, &runtime_target);
                }
                if let Some(a) = &legacy_aside {
                    let _ = fs::rename(a, dir.join("dbgcore.dll"));
                }
                return Ok(End { state: "restored", reason: why });
            }
            let text = Record::render(
                &backup.display().to_string(),
                &legacy_aside.map(|p| p.display().to_string()).unwrap_or_default(),
                None,
                &runtime_aside.map(|p| p.display().to_string()).unwrap_or_default(),
                &ours,
                &ours_rt,
            );
            fs::write(&state_path, text).map_err(|e| Error::new(9, format!("{}: {e}", state_path.display())))?;
            Ok(End { state: "installed", reason: String::new() })
        }
    }
}

/// Step 10: both files read back against the manifest.
pub fn verify_installed(dir: &Path, pkg: &Package, log: &Log) -> Result<()> {
    let mut observed = serde_json::Map::new();
    let mut ok = true;
    for (name, want) in [(HOST, pkg.host_sha()), (RUNTIME, pkg.runtime_sha())] {
        let got = fs::read(dir.join(name)).map(|b| sha256_hex(&b)).unwrap_or_default();
        ok &= got == want;
        observed.insert(name.into(), json!({"expected": want, "observed": got}));
    }
    log.event(if ok { "INFO" } else { "ERROR" }, "verify_installed", 10, json!({"ok": ok, "files": observed}))?;
    if ok {
        Ok(())
    } else {
        Err(Error::new(10, "an installed file does not match the manifest"))
    }
}

/// Step 11 on PC: the host's own record of the last start (`nevr-bootstrap.jsonl`). Returns the witness line when the
/// last `versions` record names the installed hashes and the last `runtime_load` was accepted.
pub fn launch_witness(bootstrap_jsonl: &str, host_sha: &str, runtime_sha: &str) -> std::result::Result<String, String> {
    let recs: Vec<Value> = bootstrap_jsonl.lines().filter_map(|l| serde_json::from_str(l).ok()).collect();
    let versions = recs.iter().rev().find(|r| r["event"] == "versions").ok_or("no versions record: the game has not started with this host")?;
    if versions["host_sha256"] != host_sha || versions["runtime_sha256"] != runtime_sha {
        return Err(format!("the last start ran other files: host {} runtime {}", versions["host_sha256"], versions["runtime_sha256"]));
    }
    let load = recs.iter().rev().find(|r| r["event"] == "runtime_load").ok_or("no runtime_load record")?;
    if load["verify"] != "accepted" {
        return Err(format!("runtime_load verify is {}, not accepted", load["verify"]));
    }
    if recs.iter().rev().take_while(|r| r["event"] != "versions").any(|r| r["event"] == "game_stop") {
        return Err("the host stopped the game after loading".into());
    }
    Ok(load.to_string())
}
