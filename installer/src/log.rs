//! The run log (ADR 0023, section 5): one JSON object per line, appended, never truncated, tagged `[NEVR.INSTALL]`.
//! A write that fails is an error the caller must act on; a run that cannot open its log changes nothing.

use crate::error::{Error, Result};
use crate::util::{hex, now_utc};
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Instant;

pub const TAG: &str = "[NEVR.INSTALL]";

pub struct Log {
    file: Mutex<File>,
    path: PathBuf,
    run_id: String,
    platform: String,
    started: Instant,
}

impl Log {
    /// Opens (creating, appending) the log file. Failure to open is a failure of the run (step 0).
    pub fn open(path: &Path, platform: &str) -> Result<Log> {
        if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
            fs::create_dir_all(dir).map_err(|e| Error::new(0, format!("cannot create the log directory {}: {e}", dir.display())))?;
        }
        let file = OpenOptions::new().create(true).append(true).open(path).map_err(|e| Error::new(0, format!("cannot open the log {}: {e}", path.display())))?;
        let seed = format!("{:?}{}{}", std::time::SystemTime::now(), std::process::id(), path.display());
        let run_id = hex(&Sha256::digest(seed.as_bytes())[..6]);
        Ok(Log { file: Mutex::new(file), path: path.to_path_buf(), run_id, platform: platform.to_string(), started: Instant::now() })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn run_id(&self) -> &str {
        &self.run_id
    }

    pub fn elapsed_ms(&self) -> u64 {
        self.started.elapsed().as_millis() as u64
    }

    /// One record. `fields` is merged into the record; a key that names a standard field is refused so a caller cannot
    /// overwrite `run_id`, `ts` or `tag`.
    pub fn event(&self, level: &str, event: &str, step: u32, fields: Value) -> Result<()> {
        let mut rec = Map::new();
        rec.insert("tag".into(), json!(TAG));
        rec.insert("ts".into(), json!(now_utc().map_err(|e| Error::new(0, e))?));
        rec.insert("level".into(), json!(level));
        rec.insert("run_id".into(), json!(self.run_id));
        rec.insert("event".into(), json!(event));
        rec.insert("step".into(), json!(step));
        rec.insert("platform".into(), json!(self.platform));
        rec.insert("elapsed_ms".into(), json!(self.elapsed_ms()));
        if let Value::Object(extra) = fields {
            for (k, v) in extra {
                if rec.contains_key(&k) {
                    return Err(Error::new(0, format!("log field {k} is reserved")));
                }
                rec.insert(k, v);
            }
        }
        let mut line = serde_json::to_vec(&Value::Object(rec)).expect("a record serializes");
        line.push(b'\n');
        let mut f = self.file.lock().map_err(|_| Error::new(0, "the log lock is poisoned"))?;
        f.write_all(&line).and_then(|_| f.flush()).map_err(|e| Error::new(0, format!("cannot write the log {}: {e}", self.path.display())))
    }

    pub fn info(&self, event: &str, step: u32, fields: Value) -> Result<()> {
        self.event("INFO", event, step, fields)
    }

    pub fn error(&self, event: &str, step: u32, fields: Value) -> Result<()> {
        self.event("ERROR", event, step, fields)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn records_are_json_lines_with_the_tag_and_never_truncate() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("logs").join("install.jsonl");
        {
            let l = Log::open(&p, "quest").unwrap();
            l.info("run_start", 0, json!({"channel": "beta"})).unwrap();
        }
        {
            let l = Log::open(&p, "quest").unwrap();
            l.error("run_end", 9, json!({"state": "unchanged"})).unwrap();
        }
        let text = fs::read_to_string(&p).unwrap();
        let recs: Vec<Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        assert_eq!(recs.len(), 2);
        assert_eq!(recs[0]["tag"], TAG);
        assert_eq!(recs[0]["event"], "run_start");
        assert_eq!(recs[0]["channel"], "beta");
        assert_eq!(recs[1]["state"], "unchanged");
        assert_ne!(recs[0]["run_id"], recs[1]["run_id"]);
        assert!(!text.contains('\u{1b}'));
    }

    #[test]
    fn an_unopenable_log_is_an_error() {
        let dir = tempfile::tempdir().unwrap();
        let blocker = dir.path().join("file");
        fs::write(&blocker, b"x").unwrap();
        // A path below a regular file cannot be created.
        let e = Log::open(&blocker.join("sub").join("log"), "pc").err().unwrap();
        assert_eq!(e.step, 0);
        // A directory in place of the file cannot be opened for append.
        let d = dir.path().join("asdir");
        fs::create_dir(&d).unwrap();
        assert!(Log::open(&d, "pc").is_err());
    }

    #[test]
    fn a_reserved_field_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let l = Log::open(&dir.path().join("l"), "pc").unwrap();
        assert!(l.info("x", 1, json!({"run_id": "forged"})).is_err());
    }

    #[test]
    fn a_failed_write_is_reported() {
        // /dev/full accepts the open and refuses every write with ENOSPC.
        if Path::new("/dev/full").exists() {
            let l = Log::open(Path::new("/dev/full"), "pc").unwrap();
            let e = l.info("x", 1, json!({})).unwrap_err();
            assert_eq!(e.step, 0);
            assert!(e.msg.contains("cannot write the log"), "{}", e.msg);
        }
    }
}
