//! The PC plan (ADR 0023, section 2): the semantics of `tools/package-release/install.ps1`, made data and tested, with the
//! layout of ADR 0012. The only file written beside `echovr.exe` is the host, `BugSplat64.dll`; the runtime, the record and
//! every set-aside file go to the nevr root. The game's own `BugSplat64.dll` is copied aside, never deleted; the runtime is
//! written before the host that loads it; every file is read back; any failure puts every changed file back, and the run
//! says `restored` only when that worked.

use crate::error::{Error, Result};
use crate::log::Log;
use crate::util::{now_utc, semver_cmp, sha256_hex};
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

pub const HOST: &str = "BugSplat64.dll";
pub const RUNTIME: &str = "nevr-runtime.dll";
pub const STATE_FILE: &str = "nevr-install.txt";

/// Where the plan reads and writes: `game` is the folder with `echovr.exe`, `root` the nevr root.
#[derive(Debug, Clone)]
pub struct Layout {
    pub game: PathBuf,
    pub root: PathBuf,
}

impl Layout {
    pub fn new(game: impl Into<PathBuf>, root: impl Into<PathBuf>) -> Layout {
        Layout { game: game.into(), root: root.into() }
    }
    /// The host, the one file the plan writes in the game folder.
    pub fn host(&self) -> PathBuf {
        self.game.join(HOST)
    }
    pub fn runtime(&self) -> PathBuf {
        self.root.join(RUNTIME)
    }
    /// An earlier layout put the runtime beside the game.
    pub fn legacy_runtime(&self) -> PathBuf {
        self.game.join(RUNTIME)
    }
    pub fn legacy_dbgcore(&self) -> PathBuf {
        self.game.join("dbgcore.dll")
    }
    /// An earlier installer kept its record beside the game.
    pub fn legacy_record(&self) -> PathBuf {
        self.game.join(STATE_FILE)
    }
    pub fn record(&self) -> PathBuf {
        self.root.join("installer").join(STATE_FILE)
    }
    /// Every file the plan sets aside lands here.
    pub fn aside_dir(&self) -> PathBuf {
        self.root.join("installer").join("aside")
    }
}

/// The files the plan installs, taken from the verified zip, and the release they belong to.
#[derive(Debug, Clone)]
pub struct Package {
    pub host: Vec<u8>,
    pub runtime: Vec<u8>,
    pub version: String,
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

/// What a record says; every key `uninstall` reads.
#[derive(Debug, Default)]
pub struct RecordFields<'a> {
    pub backup: &'a str,
    pub legacy: &'a str,
    pub runtime_aside: &'a str,
    pub runtime_legacy: &'a str,
    pub runtime_root: &'a str,
    pub game_dir: &'a str,
    pub release: &'a str,
    pub installed: &'a str,
    pub runtime: &'a str,
    pub previous: Option<&'a str>,
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

    pub fn render(f: &RecordFields) -> String {
        let mut lines = vec![
            format!("backup={}", f.backup),
            format!("legacy={}", f.legacy),
            format!("runtime_aside={}", f.runtime_aside),
            format!("runtime_legacy={}", f.runtime_legacy),
            format!("runtime_root={}", f.runtime_root),
            format!("game_dir={}", f.game_dir),
            format!("release={}", f.release),
            format!("installed_sha256={}", f.installed),
            format!("runtime_sha256={}", f.runtime),
        ];
        if let Some(p) = f.previous {
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
    /// The record, from the root or, failing that, from beside the game (an earlier installer put it there).
    pub record: Option<Record>,
    pub record_path: Option<PathBuf>,
    pub record_in_root: bool,
    pub backup_exists: bool,
    pub backup_is_nevr: bool,
    pub backup_sha: Option<String>,
    /// The runtime in the root.
    pub runtime_sha: Option<String>,
    pub dbgcore_present: bool,
    /// A runtime beside the game that carries the NEVR identity: the earlier layout, moved into the root's aside folder.
    /// A file of that name that is not ours is left alone.
    pub legacy_runtime_is_nevr: bool,
}

pub fn observe(l: &Layout, game_running: bool) -> Result<Observed> {
    let io = |what: &Path, e: std::io::Error| Error::new(9, format!("{}: {e}", what.display()));
    let read_opt = |p: &Path| -> Result<Option<Vec<u8>>> {
        match fs::read(p) {
            Ok(b) => Ok(Some(b)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(io(p, e)),
        }
    };
    let target = read_opt(&l.host())?;
    let runtime = read_opt(&l.runtime())?;
    let legacy_runtime = read_opt(&l.legacy_runtime())?;
    let (record_bytes, record_path) = match read_opt(&l.record())? {
        Some(b) => (Some(b), Some(l.record())),
        None => match read_opt(&l.legacy_record())? {
            Some(b) => (Some(b), Some(l.legacy_record())),
            None => (None, None),
        },
    };
    let record = record_bytes.map(|b| Record::parse(&String::from_utf8_lossy(&b)));
    let backup = match record.as_ref().and_then(|r| r.get("backup")).filter(|b| !b.is_empty()) {
        Some(b) => read_opt(Path::new(b))?,
        None => None,
    };
    Ok(Observed {
        has_exe: l.game.join("echovr.exe").is_file(),
        game_running,
        target_is_nevr: target.as_deref().is_some_and(is_nevr_runtime),
        target_sha: target.as_deref().map(sha256_hex),
        record,
        record_in_root: record_path.as_deref() == Some(l.record().as_path()),
        record_path,
        backup_exists: backup.is_some(),
        backup_is_nevr: backup.as_deref().is_some_and(is_nevr_runtime),
        backup_sha: backup.as_deref().map(sha256_hex),
        runtime_sha: runtime.as_deref().map(sha256_hex),
        dbgcore_present: l.legacy_dbgcore().exists(),
        legacy_runtime_is_nevr: legacy_runtime.as_deref().is_some_and(is_nevr_runtime),
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

pub fn plan(obs: &Observed, pkg: &Package, allow_downgrade: bool) -> Plan {
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
            if let Some(installed) = r.get("release").filter(|_| !allow_downgrade) {
                if semver_cmp(installed, &pkg.version) == Some(std::cmp::Ordering::Greater) {
                    return needs(&format!("release {installed} is installed and newer than the offered {}; pass --allow-downgrade to install the older one", pkg.version));
                }
            }
            if orig == ours {
                let runtime_current = obs.runtime_sha.as_deref() == Some(ours_rt.as_str());
                if r.get("installed_sha256") == Some(ours.as_str()) && runtime_current && r.get("release") == Some(pkg.version.as_str()) && obs.record_in_root && !obs.legacy_runtime_is_nevr {
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

/// `dir/name`, or `dir/name-2`, `-3`... when that exists: a set-aside file is never overwritten and a second install in
/// the same second does not refuse.
fn fresh(dir: &Path, name: &str) -> PathBuf {
    let first = dir.join(name);
    if !first.exists() {
        return first;
    }
    (2..).map(|n| dir.join(format!("{name}-{n}"))).find(|p| !p.exists()).expect("an unused name exists")
}

fn verify_file(path: &Path, sha: &str) -> std::result::Result<(), String> {
    let b = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if sha256_hex(&b) != sha {
        return Err(format!("{} does not match the package after writing", path.display()));
    }
    Ok(())
}

/// A temporary file in the same folder, renamed over `path`: a reader sees the old file or the new one, never half of
/// either. The temporary is removed when the write or the rename fails.
fn put(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let tmp = path.with_file_name(format!("{name}.nevr-new-{}", std::process::id()));
    let r = (|| {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
        drop(f);
        fs::rename(&tmp, path)
    })();
    if r.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    r
}

/// Copies `src` to `dst`, reads the copy back, and only then removes `src`; returns the bytes for an undo. Across volumes
/// a rename is a copy, so this is the move.
fn move_out(src: &Path, dst: &Path) -> std::result::Result<Vec<u8>, String> {
    let b = fs::read(src).map_err(|e| format!("{}: {e}", src.display()))?;
    put(dst, &b).map_err(|e| format!("{}: {e}", dst.display()))?;
    verify_file(dst, &sha256_hex(&b))?;
    fs::remove_file(src).map_err(|e| format!("{}: {e}", src.display()))?;
    Ok(b)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct End {
    pub state: &'static str,
    pub reason: String,
}

/// Test hooks: make the named step, or the named undo, fail as an I/O error would. Steps: `runtime_written`,
/// `host_written`, `legacy_moved`, `verify_installed`, `record_written` (before the record is written), `record_final` (after). Undos: `legacy_runtime`, `legacy_dbgcore`, `host`,
/// `runtime`.
#[derive(Debug, Default, Clone)]
pub struct Seam {
    pub fail_step: Option<&'static str>,
    pub fail_undo: Option<&'static str>,
}

impl Seam {
    fn step(&self, name: &str) -> std::result::Result<(), String> {
        if self.fail_step == Some(name) {
            Err(format!("injected failure at {name}"))
        } else {
            Ok(())
        }
    }
}

/// One change to take back.
enum Undo {
    /// Write `bytes` back to `path`.
    Restore { what: &'static str, path: PathBuf, bytes: Vec<u8> },
    /// Move `from` back to `to`.
    Move { what: &'static str, from: PathBuf, to: PathBuf },
    /// Delete a file this run created.
    Remove { what: &'static str, path: PathBuf },
    /// Keep a failed install's file: move `path` into the aside folder under `name`.
    SetAside { what: &'static str, path: PathBuf, dir: PathBuf, name: String },
}

impl Undo {
    fn what(&self) -> &'static str {
        match self {
            Undo::Restore { what, .. } | Undo::Move { what, .. } | Undo::SetAside { what, .. } | Undo::Remove { what, .. } => what,
        }
    }
    fn run(&self) -> std::io::Result<()> {
        match self {
            Undo::Restore { path, bytes, .. } => put(path, bytes),
            Undo::Move { from, to, .. } => fs::rename(from, to),
            Undo::SetAside { path, dir, name, .. } => fs::rename(path, fresh(dir, name)),
            Undo::Remove { path, .. } => fs::remove_file(path),
        }
    }
    fn target(&self) -> &Path {
        match self {
            Undo::Restore { path, .. } => path,
            Undo::Move { to, .. } => to,
            Undo::SetAside { path, .. } | Undo::Remove { path, .. } => path,
        }
    }
}

/// Takes every change back, newest first, and returns what could not be taken back.
fn rollback(undo: Vec<Undo>, seam: &Seam, log: &Log) -> Vec<String> {
    let mut failed = Vec::new();
    for u in undo.into_iter().rev() {
        let r = if seam.fail_undo == Some(u.what()) { Err(std::io::Error::other(format!("injected failure at undo {}", u.what()))) } else { u.run() };
        match r {
            Ok(()) => {
                let _ = log.info("file_restore", 9, json!({"path": u.target().display().to_string(), "what": u.what(), "ok": true}));
            }
            Err(e) => {
                let _ = log.error("restore_failed", 9, json!({"path": u.target().display().to_string(), "what": u.what(), "reason": e.to_string()}));
                failed.push(format!("{} ({}: {e})", u.target().display(), u.what()));
            }
        }
    }
    failed
}

/// Executes a `Proceed` plan with the verified package. Every change is logged as a `file_write`.
pub fn execute(l: &Layout, plan: &Plan, obs: &Observed, pkg: &Package, log: &Log) -> Result<End> {
    execute_with(l, plan, obs, pkg, log, &Seam::default())
}

pub fn execute_with(l: &Layout, plan: &Plan, obs: &Observed, pkg: &Package, log: &Log, seam: &Seam) -> Result<End> {
    let mut undo: Vec<Undo> = Vec::new();
    match apply(l, plan, obs, pkg, log, seam, &mut undo) {
        Ok(()) => Ok(End { state: "installed", reason: String::new() }),
        Err(why) if undo.is_empty() => Ok(End { state: "unchanged", reason: why }),
        Err(why) => {
            log.error("install_failed", 9, json!({"reason": why, "changes_to_take_back": undo.len()}))?;
            let failed = rollback(undo, seam, log);
            if failed.is_empty() {
                Ok(End { state: "restored", reason: why })
            } else {
                Ok(End { state: "needs-user", reason: format!("{why}; these files could not be put back: {}; the set-aside copies are in {}", failed.join("; "), l.aside_dir().display()) })
            }
        }
    }
}

/// The steps. Each change pushes its undo first-hand, so an error at any point leaves `undo` holding exactly what changed.
fn apply(l: &Layout, plan: &Plan, obs: &Observed, pkg: &Package, log: &Log, seam: &Seam, undo: &mut Vec<Undo>) -> std::result::Result<(), String> {
    let action = plan.action.expect("a Proceed plan has an action");
    let (host, runtime) = (l.host(), l.runtime());
    let (ours, ours_rt) = (pkg.host_sha(), pkg.runtime_sha());
    let orig = obs.target_sha.clone().expect("observed");
    let st = stamp();
    let aside = l.aside_dir();
    let write_log = |path: &Path, sha: &str, replaced: &str, aside: &str| log.info("file_write", 9, json!({"path": path.display().to_string(), "sha256": sha, "replaced_sha256": replaced, "set_aside": aside})).map_err(|e| e.to_string());
    let io = |p: &Path, e: std::io::Error| format!("{}: {e}", p.display());

    fs::create_dir_all(&aside).map_err(|e| io(&aside, e))?;
    let host_bytes = fs::read(&host).map_err(|e| io(&host, e))?;
    if sha256_hex(&host_bytes) != orig {
        return Err(format!("{} changed after it was read for the plan", host.display()));
    }

    // A record that no longer describes the folder is set aside first.
    let mut old_record_text: Option<Vec<u8>> = None;
    if plan.stale_record {
        if let Some(rp) = &obs.record_path {
            let to = fresh(&aside, &format!("{STATE_FILE}.stale-{st}"));
            let b = move_out(rp, &to)?;
            undo.push(Undo::Restore { what: "stale_record", path: rp.clone(), bytes: b });
            write_log(rp, "", "", &to.display().to_string())?;
        }
    } else if let Some(rp) = obs.record_path.as_ref().filter(|p| **p == l.legacy_record()) {
        old_record_text = fs::read(rp).ok();
    }
    let record = if plan.stale_record { None } else { obs.record.clone() };

    let mut backup = record.as_ref().and_then(|r| r.get("backup")).unwrap_or("").to_string();
    let mut legacy = record.as_ref().and_then(|r| r.get("legacy")).unwrap_or("").to_string();
    let mut runtime_aside = record.as_ref().and_then(|r| r.get("runtime_aside")).unwrap_or("").to_string();
    let mut runtime_legacy = record.as_ref().and_then(|r| r.get("runtime_legacy")).unwrap_or("").to_string();

    if action == Action::First {
        // The game's own file, copied before anything replaces it.
        let b = fresh(&aside, &format!("{HOST}.original-{st}"));
        put(&b, &host_bytes).map_err(|e| io(&b, e))?;
        verify_file(&b, &orig).map_err(|e| format!("{e} (delete the bad copy by hand)"))?;
        write_log(&b, &orig, "", &host.display().to_string())?;
        backup = b.display().to_string();
        if obs.dbgcore_present {
            let to = fresh(&aside, &format!("dbgcore.dll.legacy-{}", &st[..8]));
            let bytes = move_out(&l.legacy_dbgcore(), &to)?;
            undo.push(Undo::Restore { what: "legacy_dbgcore", path: l.legacy_dbgcore(), bytes });
            write_log(&l.legacy_dbgcore(), "", "", &to.display().to_string())?;
            legacy = to.display().to_string();
        }
    }

    // The runtime, into the root. A different file already there is set aside first; it is put back on failure.
    let prior_runtime = fs::read(&runtime).ok();
    if action != Action::RecordOnly || obs.runtime_sha.as_deref() != Some(ours_rt.as_str()) {
        fs::create_dir_all(&l.root).map_err(|e| io(&l.root, e))?;
        if prior_runtime.is_some() && action == Action::First {
            let to = fresh(&aside, &format!("{RUNTIME}.original-{st}"));
            fs::rename(&runtime, &to).map_err(|e| io(&to, e))?;
            undo.push(Undo::Move { what: "runtime_aside", from: to.clone(), to: runtime.clone() });
            write_log(&runtime, "", obs.runtime_sha.as_deref().unwrap_or(""), &to.display().to_string())?;
            runtime_aside = to.display().to_string();
        }
        // The runtime first: until the host is replaced nothing loads it.
        put(&runtime, &pkg.runtime).map_err(|e| io(&runtime, e))?;
        match &prior_runtime {
            Some(prior) if action != Action::First => undo.push(Undo::Restore { what: "runtime", path: runtime.clone(), bytes: prior.clone() }),
            _ => undo.push(Undo::SetAside { what: "runtime", path: runtime.clone(), dir: aside.clone(), name: format!("{RUNTIME}.failed-{st}") }),
        }
        write_log(&runtime, &ours_rt, obs.runtime_sha.as_deref().unwrap_or(""), "")?;
        verify_file(&runtime, &ours_rt)?;
        seam.step("runtime_written")?;
    }

    if action != Action::RecordOnly {
        put(&host, &pkg.host).map_err(|e| io(&host, e))?;
        undo.push(Undo::Restore { what: "host", path: host.clone(), bytes: host_bytes });
        write_log(&host, &ours, &orig, if action == Action::First { &backup } else { "" })?;
        verify_file(&host, &ours)?;
        seam.step("host_written")?;
    }

    // The earlier layout's runtime beside the game goes last, so a failure above leaves it where it was.
    if obs.legacy_runtime_is_nevr {
        let to = fresh(&aside, &format!("{RUNTIME}.legacy-{st}"));
        let bytes = move_out(&l.legacy_runtime(), &to)?;
        undo.push(Undo::Restore { what: "legacy_runtime", path: l.legacy_runtime(), bytes });
        write_log(&l.legacy_runtime(), "", "", &to.display().to_string())?;
        runtime_legacy = to.display().to_string();
        seam.step("legacy_moved")?;
    }

    // The record an earlier installer left beside the game moves into the root with the rest.
    if let (Some(text), Some(rp)) = (old_record_text, obs.record_path.as_ref()) {
        let to = fresh(&aside, &format!("{STATE_FILE}.legacy-{st}"));
        move_out(rp, &to)?;
        undo.push(Undo::Restore { what: "legacy_record", path: rp.clone(), bytes: text });
        write_log(rp, "", "", &to.display().to_string())?;
    }

    verify_installed(l, pkg, log, seam)?;

    let previous: Vec<String> = record
        .as_ref()
        .map(|r| r.known())
        .unwrap_or_default()
        .into_iter()
        .filter(|k| *k != ours && !k.is_empty())
        .collect::<std::collections::BTreeSet<_>>()
        .into_iter()
        .collect();
    let previous = previous.join(",");
    let text = Record::render(&RecordFields {
        backup: &backup,
        legacy: &legacy,
        runtime_aside: &runtime_aside,
        runtime_legacy: &runtime_legacy,
        runtime_root: &l.root.display().to_string(),
        game_dir: &l.game.display().to_string(),
        release: &pkg.version,
        installed: &ours,
        runtime: &ours_rt,
        previous: if action == Action::First { None } else { Some(&previous) },
    });
    seam.step("record_written")?;
    let rec = l.record();
    fs::create_dir_all(rec.parent().expect("the record has a folder")).map_err(|e| io(&l.root, e))?;
    let prior_record = if obs.record_in_root { fs::read(&rec).ok() } else { None };
    put(&rec, text.as_bytes()).map_err(|e| io(&rec, e))?;
    undo.push(match prior_record {
        Some(bytes) => Undo::Restore { what: "record", path: rec.clone(), bytes },
        None => Undo::Remove { what: "record", path: rec.clone() },
    });
    write_log(&rec, &sha256_hex(text.as_bytes()), "", "")?;
    seam.step("record_final")?;
    Ok(())
}

/// Step 10: both files read back against the manifest's hashes (the package was checked against them when it was read).
pub fn verify_installed(l: &Layout, pkg: &Package, log: &Log, seam: &Seam) -> std::result::Result<(), String> {
    let mut observed = serde_json::Map::new();
    let mut ok = true;
    for (name, path, want) in [(HOST, l.host(), pkg.host_sha()), (RUNTIME, l.runtime(), pkg.runtime_sha())] {
        let got = fs::read(&path).map(|b| sha256_hex(&b)).unwrap_or_default();
        ok &= got == want;
        observed.insert(name.into(), json!({"expected": want, "observed": got}));
    }
    ok &= seam.step("verify_installed").is_ok();
    log.event(if ok { "INFO" } else { "ERROR" }, "verify_installed", 10, json!({"ok": ok, "files": observed})).map_err(|e| e.to_string())?;
    if ok {
        Ok(())
    } else {
        Err("an installed file does not match the manifest".into())
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
