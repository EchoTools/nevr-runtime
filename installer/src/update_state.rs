//! The installer's part of self-update (ADR 0024, section 7): after `pc install` the installer's release is the baseline.
//! `update\state.json` records the channel and that release, and what an earlier self-update left behind (a journal, a
//! pending or staged release, the two generations the host keeps, the crash marker) is cleared, so nothing older competes
//! with the installed release and a reinstall after a failed update does not leave the next start stopping on
//! `no_previous`. The quarantine list stays. `uninstall` never comes here: it leaves `update\` as it finds it.
//!
//! The work runs under `update\lock`, opened the way the host opens it (share mode 0, delete on close, retried every
//! 100 ms for up to 30 s, `update_swap.h`), so it never runs beside a host's swap. Every decision is one `[NEVR.INSTALL]`
//! record: `update_lock`, one `update_clear` for each thing it looked at, and `update_state`.
//!
//! What is read first and written last: the state is read before anything is removed, so a `state.json` that cannot be
//! read stops the run with `update\` as it was; `state.json` is written after the removals. The journal goes first among
//! the removals: a `swap.json` left beside a new baseline would be undone by the next start.

use crate::log::Log;
use crate::util::write_atomic;
use serde_json::{json, Map, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub const DIR: &str = "update";
pub const STATE: &str = "state.json";
pub const LOCK: &str = "lock";
/// How long the host waits for `update\lock`, and how often it tries (`kUpdateLockTimeoutMs`, `kUpdateLockStepMs`).
pub const LOCK_TIMEOUT: Duration = Duration::from_secs(30);
pub const LOCK_STEP: Duration = Duration::from_millis(100);
/// The host reads no JSON file of `update\` over this size or deeper than `MAX_DEPTH` (`update_names.h`): such a file is
/// damaged to it, so it is damaged here.
pub const MAX_JSON_BYTES: u64 = 64 * 1024;
pub const MAX_DEPTH: usize = 16;
/// What a baseline removes, in this order, each a file or a folder of `update\`. Journal first.
pub const CLEARED: [&str; 7] = ["swap.json", "pending.json", "crashed", "staged.json", "staging", "previous", "failed"];

/// The release the installer put in place.
#[derive(Debug, Clone)]
pub struct Baseline<'a> {
    pub channel: &'a str,
    pub version: &'a str,
    pub runtime_sha256: &'a str,
}

/// Why the update folder could not be reset. `reason` is the token in the log line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Stop {
    pub reason: &'static str,
    pub detail: String,
}

impl std::fmt::Display for Stop {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.reason, self.detail)
    }
}

/// How `state.json` was found.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Before {
    Absent,
    Read,
    /// Present and damaged; replaced by one built from the baseline (the quarantine in it is lost).
    Rebuilt(String),
}

/// True for a symbolic link, and on Windows for a junction or any other reparse point: the host refuses to move through them.
fn is_link(m: &fs::Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        m.file_attributes() & 0x400 != 0 // FILE_ATTRIBUTE_REPARSE_POINT
    }
    #[cfg(not(windows))]
    {
        m.file_type().is_symlink()
    }
}

/// `update\lock`, held until it is dropped.
pub struct UpdateLock {
    _file: fs::File,
    pub waited: Duration,
}

#[cfg(windows)]
fn open_lock(path: &Path) -> std::io::Result<Option<fs::File>> {
    use std::os::windows::fs::OpenOptionsExt;
    // Share mode 0 and delete on close: the host's open (`UpdateLock::Acquire`, update_swap.cpp). Any failure is "not yet"
    // (a sharing violation is another holder; access denied is a lock file another process has delete-pending), so the
    // caller retries until its timeout, as the host does.
    fs::OpenOptions::new().create(true).read(true).write(true).share_mode(0).custom_flags(0x0400_0000).open(path).map(Some)
}

#[cfg(not(windows))]
fn open_lock(path: &Path) -> std::io::Result<Option<fs::File>> {
    let f = fs::OpenOptions::new().create(true).write(true).truncate(false).open(path)?;
    match f.try_lock() {
        Ok(()) => Ok(Some(f)),
        Err(fs::TryLockError::WouldBlock) => Ok(None),
        Err(fs::TryLockError::Error(e)) => Err(e),
    }
}

/// Takes `<update>\lock`, trying every `step` until `timeout`. Returns `Stop` (`lock_timeout`, or `lock_failed` with the
/// error) when it cannot.
pub fn take_lock(update: &Path, timeout: Duration, step: Duration) -> Result<UpdateLock, Stop> {
    let path = update.join(LOCK);
    let start = Instant::now();
    let mut last: Option<std::io::Error>;
    loop {
        // Every failure to open is retried until the timeout, like the host (update_swap.cpp, `UpdateLock::Acquire`): a
        // lock file that is delete-pending answers access denied, not a sharing violation.
        match open_lock(&path) {
            Ok(Some(file)) => return Ok(UpdateLock { _file: file, waited: start.elapsed() }),
            Ok(None) => last = None,
            Err(e) => last = Some(e),
        }
        if start.elapsed() >= timeout {
            let why = match &last {
                Some(e) => format!("the open failed: {e}"),
                None => "it is held by another process".to_string(),
            };
            return Err(Stop { reason: "lock_timeout", detail: format!("{} could not be taken: {why} (waited {} ms)", path.display(), start.elapsed().as_millis()) });
        }
        std::thread::sleep(step);
    }
}

// The host's own name rules (src/runtime/host/update_names.cpp: IsName, IsDeviceName, IsVersionName, IsPluginName,
// ParseMember), mirrored because C++ is out of reach of this crate. A name is 1 to 64 characters from its set, never starts
// or ends with '.', and is not a Windows device name (CON PRN AUX NUL CONIN$ CONOUT$ COM0-9 LPT0-9, any case, compared on
// the part before the first '.'). The tests below pin this copy to the host's table.
fn is_device_name(name: &str) -> bool {
    let base = name.split('.').next().unwrap_or("").to_ascii_uppercase();
    ["CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"].contains(&base.as_str())
        || (base.len() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base.as_bytes()[3].is_ascii_digit())
}

fn is_name(name: &str, allowed: impl Fn(u8) -> bool) -> bool {
    !name.is_empty() && name.len() <= 64 && !name.starts_with('.') && !name.ends_with('.') && name.bytes().all(allowed) && !is_device_name(name)
}

/// update_names.cpp `IsVersionName`: [0-9A-Za-z.+-]{1,64}.
pub fn is_version_name(v: &str) -> bool {
    is_name(v, |c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'+' | b'-'))
}

/// update_names.cpp `ParseMember`: `nevr-runtime.dll`, or `plugins/<name>.dll` with `<name>` [A-Za-z0-9._-]{1,64}.
pub fn is_member(path: &str) -> bool {
    if path == crate::stage::RUNTIME {
        return true;
    }
    path.strip_prefix("plugins/").and_then(|r| r.strip_suffix(".dll")).is_some_and(|n| is_name(n, |c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'-')))
}

fn depth(v: &Value) -> usize {
    match v {
        Value::Array(a) => 1 + a.iter().map(depth).max().unwrap_or(0),
        Value::Object(o) => 1 + o.values().map(depth).max().unwrap_or(0),
        _ => 0,
    }
}

/// What `state.json` has to be for the host to use it (`update_swap.cpp`, the check before "owned"): an object; `owned` a
/// list of `{path, sha256}` with a member path; `discards` an object of counts; `quarantine` a list. `Err` says what is wrong.
fn damage(v: &Value) -> Option<String> {
    let Some(o) = v.as_object() else { return Some("not an object".into()) };
    if depth(v) > MAX_DEPTH {
        return Some(format!("nested deeper than {MAX_DEPTH}"));
    }
    if let Some(owned) = o.get("owned") {
        let Some(list) = owned.as_array() else { return Some("owned is not a list".into()) };
        for e in list {
            let ok = e["path"].as_str().is_some_and(is_member) && e["sha256"].as_str().is_some_and(crate::util::is_sha256_hex);
            if !ok {
                return Some("owned has an entry that is not a member path with a sha256".into());
            }
        }
    }
    if let Some(d) = o.get("discards") {
        // The host: every key IsVersionName, every value an unsigned number (update_swap.cpp, `ValidState`).
        if !d.as_object().is_some_and(|m| m.iter().all(|(k, c)| is_version_name(k) && c.is_u64())) {
            return Some("discards is not an object of version names and counts".into());
        }
    }
    if o.get("quarantine").is_some_and(|q| !q.is_array()) {
        return Some("quarantine is not a list".into());
    }
    None
}

/// Reads `<update>\state.json`: absent, read, or damaged (`Rebuilt`). A file that is there and cannot be read is a `Stop`,
/// never a damaged one: nothing may be written over what another program holds.
fn read_state(path: &Path) -> Result<(Map<String, Value>, Before), Stop> {
    let meta = match fs::symlink_metadata(path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok((Map::new(), Before::Absent)),
        Err(e) => return Err(Stop { reason: "state_unreadable", detail: format!("{}: {e}", path.display()) }),
    };
    if is_link(&meta) {
        return Err(Stop { reason: "reparse_point", detail: format!("{} is a link", path.display()) });
    }
    if meta.len() > MAX_JSON_BYTES {
        return Ok((Map::new(), Before::Rebuilt(format!("larger than {MAX_JSON_BYTES} bytes"))));
    }
    let bytes = fs::read(path).map_err(|e| Stop { reason: "state_unreadable", detail: format!("{}: {e}", path.display()) })?;
    match serde_json::from_slice::<Value>(&bytes) {
        Err(e) => Ok((Map::new(), Before::Rebuilt(format!("does not parse: {e}")))),
        Ok(v) => match damage(&v) {
            Some(why) => Ok((Map::new(), Before::Rebuilt(why))),
            None => match v {
                Value::Object(m) => Ok((m, Before::Read)),
                _ => unreachable!("damage() refuses what is not an object"),
            },
        },
    }
}

/// The state after the install: this release and channel; `owned` is the runtime at its new hash and every plugin the
/// earlier state listed (the installer does not touch `plugins\`, so what self-update placed there stays owned); the
/// discard counts and the quarantine as they were.
fn next_state(before: &Map<String, Value>, b: &Baseline) -> Value {
    let mut owned = vec![json!({"path": crate::stage::RUNTIME, "sha256": b.runtime_sha256})];
    if let Some(list) = before.get("owned").and_then(Value::as_array) {
        owned.extend(list.iter().filter(|e| e["path"] != crate::stage::RUNTIME).cloned());
    }
    let mut s = Map::new();
    s.insert("channel".into(), json!(b.channel));
    s.insert("current".into(), json!({"version": b.version, "runtime_sha256": b.runtime_sha256}));
    s.insert("owned".into(), Value::Array(owned));
    for key in ["discards", "quarantine"] {
        if let Some(v) = before.get(key) {
            s.insert(key.into(), v.clone());
        }
    }
    Value::Object(s)
}

/// Removes one thing of `update\`: a link is removed and never followed, a folder whole, a file. Returns the outcome word.
fn clear_one(p: &Path) -> Result<&'static str, std::io::Error> {
    let meta = match fs::symlink_metadata(p) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok("absent"),
        Err(e) => return Err(e),
    };
    if is_link(&meta) {
        // A folder link is a folder to Windows and a file to unix; try the folder form first, so neither follows it.
        fs::remove_dir(p).or_else(|_| fs::remove_file(p))?;
        return Ok("link_removed");
    }
    if meta.is_dir() {
        fs::remove_dir_all(p)?;
    } else {
        fs::remove_file(p)?;
    }
    Ok("removed")
}

/// Records `release` as the baseline under `<root>\update` (ADR 0024, section 7). Takes the lock with `timeout`; on any
/// `Err` the install itself stands and nothing past the failing step was changed. Every step writes its line; a line that
/// cannot be written is an error too (a decision with no record is a defect).
pub fn reset(root: &Path, b: &Baseline, log: &Log, timeout: Duration) -> Result<(), Stop> {
    let stop = |s: Stop| -> Stop {
        let _ = log.error("update_state", 9, json!({"ok": false, "reason": s.reason, "detail": s.detail, "channel": b.channel, "version": b.version}));
        s
    };
    let lost = |e: crate::error::Error| Stop { reason: "log_failed", detail: e.to_string() };
    let update: PathBuf = root.join(DIR);
    match fs::symlink_metadata(&update) {
        Ok(m) if is_link(&m) => return Err(stop(Stop { reason: "reparse_point", detail: format!("{} is a link; nothing in it was changed", update.display()) })),
        Ok(m) if !m.is_dir() => return Err(stop(Stop { reason: "not_a_folder", detail: format!("{} is a file", update.display()) })),
        Ok(_) => {}
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir_all(&update).map_err(|e| stop(Stop { reason: "create_failed", detail: format!("{}: {e}", update.display()) }))?;
        }
        Err(e) => return Err(stop(Stop { reason: "create_failed", detail: format!("{}: {e}", update.display()) })),
    }

    let lock = match take_lock(&update, timeout, LOCK_STEP) {
        Ok(l) => l,
        Err(s) => {
            let _ = log.error("update_lock", 9, json!({"ok": false, "reason": s.reason, "detail": s.detail, "waited_ms": timeout.as_millis() as u64}));
            return Err(stop(s));
        }
    };
    log.info("update_lock", 9, json!({"ok": true, "path": update.join(LOCK).display().to_string(), "wait_ms": lock.waited.as_millis() as u64})).map_err(lost)?;

    let (before_state, before) = read_state(&update.join(STATE)).map_err(stop)?;
    for name in CLEARED {
        let p = update.join(name);
        match clear_one(&p) {
            Ok(outcome) => log.info("update_clear", 9, json!({"name": name, "path": p.display().to_string(), "outcome": outcome})).map_err(lost)?,
            Err(e) => {
                let _ = log.error("update_clear", 9, json!({"name": name, "path": p.display().to_string(), "outcome": "failed", "reason": e.to_string()}));
                return Err(stop(Stop { reason: "clear_failed", detail: format!("{}: {e}", p.display()) }));
            }
        }
    }
    let state = next_state(&before_state, b);
    let bytes = serde_json::to_vec_pretty(&state).expect("a JSON value serializes");
    write_atomic(&update.join(STATE), &bytes).map_err(|e| stop(Stop { reason: "state_write_failed", detail: format!("{}: {e}", update.join(STATE).display()) }))?;
    let (state_before, rebuilt_reason) = match &before {
        Before::Absent => ("absent", None),
        Before::Read => ("read", None),
        Before::Rebuilt(why) => ("rebuilt", Some(why.clone())),
    };
    log.info(
        "update_state",
        9,
        json!({"ok": true, "channel": b.channel, "version": b.version, "runtime_sha256": b.runtime_sha256, "state_before": state_before, "rebuilt_reason": rebuilt_reason,
               "owned": state["owned"].as_array().map(Vec::len), "quarantine": state["quarantine"].as_array().map(Vec::len).unwrap_or(0), "path": update.join(STATE).display().to_string(),
               "lock_wait_ms": lock.waited.as_millis() as u64}),
    )
    .map_err(lost)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_lock_another_handle_holds_times_out_with_its_reason_and_frees_when_released() {
        let dir = tempfile::tempdir().unwrap();
        let held = take_lock(dir.path(), Duration::from_millis(200), Duration::from_millis(20)).unwrap();
        let again = take_lock(dir.path(), Duration::from_millis(300), Duration::from_millis(20));
        let stop = again.err().expect("a second holder is refused");
        assert_eq!(stop.reason, "lock_timeout", "{stop}");
        drop(held);
        assert!(take_lock(dir.path(), Duration::from_millis(300), Duration::from_millis(20)).is_ok(), "released, it can be taken again");
    }

    #[test]
    fn an_open_that_fails_is_retried_until_the_timeout_not_only_a_sharing_violation() {
        // A folder where the lock file goes: every open fails (not a sharing violation), as a delete-pending file's open does.
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir(dir.path().join(LOCK)).unwrap();
        let stop = take_lock(dir.path(), Duration::from_millis(250), Duration::from_millis(20)).err().expect("it never opens");
        assert_eq!(stop.reason, "lock_timeout", "{stop}");
        assert!(stop.detail.contains("the open failed") && stop.detail.contains("waited"), "the last error and the wait are named: {stop}");
        // The cause clears while the install waits: the lock is then taken.
        let path = dir.path().join(LOCK);
        let clearer = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(300));
            fs::remove_dir(path).unwrap();
        });
        let got = take_lock(dir.path(), Duration::from_secs(5), Duration::from_millis(20));
        clearer.join().unwrap();
        assert!(got.is_ok_and(|l| l.waited >= Duration::from_millis(250)), "retried past the early failures");
    }

    #[test]
    fn the_name_rules_are_the_hosts() {
        // The host's cases (update_names.cpp): what it takes and what it refuses.
        for ok in ["1.2.3", "5.16.1-dev.4+abc", "a", &"a".repeat(64), "CONSOLE", "COM10", "COM"] {
            assert!(is_version_name(ok), "{ok}");
        }
        for bad in ["", ".", "..", ".1", "1.", "1/2", "1 2", "1_2", &"a".repeat(65), "CON", "con", "Nul.1", "aux", "COM1", "lpt9.x", "CONIN$", "CONOUT$.a", "PRN"] {
            assert!(!is_version_name(bad), "{bad}");
        }
        for ok in ["nevr-runtime.dll", "plugins/a.dll", "plugins/a_b-c.d.dll", "plugins/CONSOLE.dll", "plugins/COM10.dll"] {
            assert!(is_member(ok), "{ok}");
        }
        for bad in ["", "plugins/.dll", "plugins/.x.dll", "plugins/x..dll", "plugins/CON.dll", "plugins/nul.dll", "plugins/COM1.dll", "plugins/LPT0.x.dll", "plugins/CONIN$.dll", "plugins/a/b.dll", "plugins/../x.dll", "other/a.dll", "plugins/a.exe", "Nevr-Runtime.dll"] {
            assert!(!is_member(bad), "{bad}");
        }
        let sha = "a".repeat(64);
        for bad in [json!({"discards": {"CON": 1}}), json!({"discards": {"..": 1}}), json!({"discards": {"a b": 1}}), json!({"owned": [{"path": "plugins/CON.dll", "sha256": sha}]}), json!({"owned": [{"path": "plugins/.x.dll", "sha256": sha}]})] {
            assert!(damage(&bad).is_some(), "the installer takes what the host refuses: {bad}");
        }
    }

    #[test]
    fn a_state_the_host_would_not_use_is_damaged() {
        let sha = "a".repeat(64);
        assert_eq!(damage(&json!({"owned": [{"path": "nevr-runtime.dll", "sha256": sha}], "discards": {"1.0.0": 1}, "quarantine": []})), None);
        for bad in [json!([]), json!({"owned": {}}), json!({"owned": [{"path": "../x.dll", "sha256": sha}]}), json!({"owned": [{"path": "nevr-runtime.dll", "sha256": "zz"}]}), json!({"discards": {"1.0.0": "two"}}), json!({"quarantine": {}})] {
            assert!(damage(&bad).is_some(), "{bad}");
        }
        let mut deep = json!(1);
        for _ in 0..MAX_DEPTH + 1 {
            deep = json!({"a": deep});
        }
        assert!(damage(&deep).unwrap().contains("deeper"));
    }
}
