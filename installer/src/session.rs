//! A run: the configuration, the log, and the commands the CLI and the C ABI share. Every command writes its records
//! through `Log` and ends with exactly one `run_end`.

use crate::artifact;
use crate::device::Device;
use crate::error::{Error, Result};
use crate::fetch::{fetch_capped, Failure, DEFAULT_BASE};
use crate::log::Log;
use crate::manifest::{self, Artifact, Pins, Verified, MAX_MANIFEST_BYTES, MAX_SIG_BYTES, MIN_MANIFEST_VERSION};
use crate::{pc, quest};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};

pub const CORE_VERSION: &str = env!("CARGO_PKG_VERSION");

#[derive(Debug, Clone)]
pub struct Config {
    pub base: String,
    pub state_dir: PathBuf,
    pub log_path: PathBuf,
    pub work_dir: PathBuf,
    pub platform: String,
    pub channel: String,
    pub pins: Pins,
    pub min_manifest_version: u64,
    /// Overrides the clock (tests); `None` reads the system clock.
    pub now: Option<String>,
    /// Seconds between the two retries of a failed signature check (the pair may have been caught between two writes).
    pub retry_delay_secs: u64,
}

impl Config {
    pub fn new(platform: &str, root: &Path) -> Config {
        Config {
            base: DEFAULT_BASE.into(),
            state_dir: root.join("state"),
            log_path: root.join("logs").join("nevr-install.jsonl"),
            work_dir: root.join("work"),
            platform: platform.into(),
            channel: "beta".into(),
            pins: Pins::builtin(),
            min_manifest_version: MIN_MANIFEST_VERSION,
            now: None,
            retry_delay_secs: 3,
        }
    }
}

/// How a run ended: one of the four states of ADR 0023.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Ended {
    pub state: &'static str,
    pub reason: String,
    /// The run stopped on a check or an error, as opposed to finding nothing to do.
    pub failed: bool,
}

impl Ended {
    /// 0 installed, or unchanged with nothing wrong; 1 a check or step failed with nothing changed; 2 needs the
    /// player; 3 restored after a failure.
    pub fn exit_code(&self) -> i32 {
        match self.state {
            "needs-user" => 2,
            "restored" => 3,
            _ if self.failed => 1,
            _ => 0,
        }
    }
}

pub struct Session {
    pub cfg: Config,
    pub log: Log,
}

fn clock(cfg: &Config) -> Result<String> {
    match &cfg.now {
        Some(n) => Ok(n.clone()),
        None => crate::util::now_utc().map_err(|e| Error::new(5, e)),
    }
}

impl Session {
    /// Opens the log (refusing the run when it cannot) and writes `run_start`.
    pub fn open(cfg: Config, mode: &str) -> Result<Session> {
        let log = Log::open(&cfg.log_path, &cfg.platform)?;
        log.info("run_start", 0, json!({"channel": cfg.channel, "manifest_base": cfg.base, "core_version": CORE_VERSION, "min_manifest_version": cfg.min_manifest_version, "mode": mode, "pins": cfg.pins.ids()}))?;
        Ok(Session { cfg, log })
    }

    /// Writes `run_end`. A failure to write it is returned: the run is not over until its end is on record.
    pub fn end(&self, state: &'static str, reason: &str) -> Result<Ended> {
        self.end_with(state, reason, false)
    }

    pub fn end_with(&self, state: &'static str, reason: &str, failed: bool) -> Result<Ended> {
        self.log.event(if failed || state == "restored" || state == "needs-user" { "ERROR" } else { "INFO" }, "run_end", 0, json!({"state": state, "reason": reason, "failed": failed}))?;
        Ok(Ended { state, reason: reason.to_string(), failed })
    }

    /// An error ends the run as `unchanged` (nothing was written before the failing check) and says why.
    pub fn fail(&self, e: &Error) -> Ended {
        let _ = self.log.error("manifest_check", e.step, json!({"ok": false, "reason": e.to_string()}));
        match self.end_with("unchanged", &e.to_string(), true) {
            Ok(end) => end,
            Err(le) => Ended { state: "unchanged", reason: format!("{e}; and the log could not record it: {le}"), failed: true },
        }
    }

    fn get(&self, name: &str, max: u64) -> std::result::Result<Vec<u8>, Failure> {
        let url = format!("{}/{name}", self.cfg.base);
        let mut body = Vec::new();
        fetch_capped(&url, 0, max, &mut body)?;
        let _ = self.log.info("manifest_fetch", 1, json!({"url": url, "bytes": body.len(), "status": 200}));
        Ok(body)
    }

    /// Steps 1 to 6: fetch the pair, verify it, store it. A signature failure fetches the pair again, twice.
    pub fn manifest(&self) -> Result<Verified> {
        let state = manifest::read_state(&self.cfg.state_dir)?;
        let now = clock(&self.cfg)?;
        let mut last: Option<Error> = None;
        for attempt in 0..3 {
            if attempt > 0 {
                std::thread::sleep(std::time::Duration::from_secs(self.cfg.retry_delay_secs));
            }
            let fetch = |name: &str, max: u64| self.get(name, max).map_err(|f| Error::new(1, match f { Failure::Interrupted(m) | Failure::Refused(m) => m }));
            let bytes = fetch("manifest.json", MAX_MANIFEST_BYTES)?;
            let sig = fetch("manifest.json.sig", MAX_SIG_BYTES)?;
            match manifest::verify(&bytes, &sig, &self.cfg.pins, &state, &now, self.cfg.min_manifest_version) {
                Ok(v) => {
                    manifest::commit(&self.cfg.state_dir, &v)?;
                    self.log.info("manifest_check", 6, json!({"ok": true, "key_id": v.key_id, "manifest_version": v.version, "stored_highest": state.highest}))?;
                    return Ok(v);
                }
                Err(e) if e.step == 2 => last = Some(e),
                Err(e) => return Err(e),
            }
        }
        Err(last.expect("three attempts ran"))
    }

    /// Reads the manifest from a directory instead of the network (an offline mirror, or a test).
    pub fn manifest_from_dir(&self, dir: &Path) -> Result<Verified> {
        let state = manifest::read_state(&self.cfg.state_dir)?;
        let read = |n: &str| std::fs::read(dir.join(n)).map_err(|e| Error::new(1, format!("{n}: {e}")));
        let v = manifest::verify(&read("manifest.json")?, &read("manifest.json.sig")?, &self.cfg.pins, &state, &clock(&self.cfg)?, self.cfg.min_manifest_version)?;
        manifest::commit(&self.cfg.state_dir, &v)?;
        self.log.info("manifest_check", 6, json!({"ok": true, "key_id": v.key_id, "manifest_version": v.version, "stored_highest": state.highest}))?;
        Ok(v)
    }

    /// Step 7 on the stored manifest (step 8's read-back rule applies to everything after this).
    pub fn release(&self, version: Option<&str>) -> Result<(Value, String)> {
        let m = manifest::stored(&self.cfg.state_dir)?;
        let v = match version {
            Some(v) => {
                if m["releases"][v].is_null() {
                    return Err(Error::new(7, format!("release {v} is not in releases")));
                }
                v.to_string()
            }
            None => manifest::pick(&m, &self.cfg.channel)?,
        };
        let arts: Vec<Value> = m["releases"][v.as_str()]["artifacts"]
            .as_array()
            .map(|a| a.iter().map(|x| json!({"kind": x["kind"], "path": x["path"], "size": x["size"], "sha256": x["sha256"]})).collect())
            .unwrap_or_default();
        self.log.info("release_selected", 7, json!({"channel": self.cfg.channel, "version": v, "artifacts": arts}))?;
        Ok((m, v))
    }

    /// The Quest command: plan, and with `install` execute. `dev` is `None` for a plan with no headset.
    pub fn quest(&self, version: Option<&str>, dev: Option<&dyn Device>, install: bool, consent_uninstall: bool) -> Result<(Ended, Value)> {
        let (m, v) = self.release(version)?;
        // The chosen release's own game data; failing that, the newest listed one, so a device that already holds
        // complete data is recognised even when this release lists none.
        let data_for_observation = manifest::find(&m, &v, "game-data", Some("android-arm64")).ok().or_else(|| manifest::latest_game_data(&m).map(|x| x.1));
        let observed = match dev {
            Some(d) => quest::observe(d, data_for_observation.as_ref(), &self.log)?,
            None => quest::Observed::default(),
        };
        let plan = quest::plan(&m, &v, &observed, consent_uninstall);
        self.log.info("plan", 9, json!({"steps": plan.steps.iter().map(|s| s.name()).collect::<Vec<_>>(), "installed": observed.installed.as_ref().map(|i| json!({"version_code": i.version_code, "signers": i.signers})), "decision": plan.decision, "reasons": plan.reasons}))?;
        let report = plan.to_json();
        let ended = match plan.outcome {
            quest::Outcome::Unchanged => self.end("unchanged", &plan.reasons.join("; "))?,
            quest::Outcome::NeedsUser => self.end("needs-user", &plan.reasons.join("; "))?,
            quest::Outcome::Proceed if !install => self.end("unchanged", "plan only: nothing was changed")?,
            quest::Outcome::Proceed => {
                let dev = dev.ok_or_else(|| Error::new(9, "install needs a device"))?;
                let opts = quest::RunOpts { work_dir: self.cfg.work_dir.clone(), base: &self.cfg.base, consent_uninstall };
                let end = quest::execute(dev, &m, &v, &plan, &opts, &self.log)?;
                if end.state == "installed" {
                    self.write_record(&v, &m, &plan.apk, &plan.data)?;
                }
                self.end_with(end.state, &end.reason, end.state == "unchanged")?
            }
        };
        Ok((ended, report))
    }

    /// The install record: written last, by a temporary file and a rename.
    fn write_record(&self, version: &str, m: &Value, a: &Option<Artifact>, b: &Option<Artifact>) -> Result<()> {
        let state = manifest::read_state(&self.cfg.state_dir)?;
        let arts: Vec<Value> = [a, b].iter().filter_map(|x| x.as_ref()).map(|x| json!({"kind": x.kind, "path": x.path, "sha256": x.sha256})).collect();
        let rec = json!({
            "release": version,
            "manifest_version": m["manifest_version"],
            "last_manifest_sha256": state.last_sha256,
            "artifacts": arts,
            "layout": if self.cfg.platform == "quest" { quest::MEDIA_ROOT } else { "beside echovr.exe" },
            "time": crate::util::now_utc().unwrap_or_default(),
        });
        crate::util::write_atomic(&self.cfg.state_dir.join("install-record.json"), &serde_json::to_vec_pretty(&rec).expect("record serializes")).map_err(|e| Error::new(9, format!("install-record.json: {e}")))
    }

    /// The PC command: verify the zip, plan against `dir`, and with `install` execute.
    pub fn pc(&self, version: Option<&str>, dir: &Path, install: bool, game_running: bool) -> Result<(Ended, Value)> {
        let (m, v) = self.release(version)?;
        let art = manifest::find(&m, &v, "windows-zip", None)?;
        let zip_path = self.cfg.work_dir.join(art.path.rsplit('/').next().unwrap_or("windows.zip"));
        std::fs::create_dir_all(&self.cfg.work_dir).map_err(|e| Error::new(8, format!("{}: {e}", self.cfg.work_dir.display())))?;
        let url = format!("{}/{}", self.cfg.base, art.path);
        let f = artifact::download(&url, &art, &zip_path)?;
        self.log.info("artifact_fetch", 8, json!({"kind": "windows-zip", "url": url, "size": art.size, "bytes": f.bytes, "sha256": f.sha256, "ok": true, "resumed_from": f.resumed_from}))?;
        let n = artifact::verify_zip(&zip_path, &art, false)?;
        self.log.info("verify_installed", 8, json!({"ok": true, "what": "zip-members", "expected": art.contents.len(), "observed": n}))?;
        let pkg = read_package(&zip_path, &art)?;
        let observed = pc::observe(dir, game_running)?;
        let plan = pc::plan(&observed, &pkg);
        self.log.info("plan", 9, json!({"decision": plan.decision, "reasons": plan.reasons, "action": plan.action.map(|a| format!("{a:?}")), "installed": {"host_sha256": observed.target_sha, "runtime_sha256": observed.runtime_sha}}))?;
        let report = plan.to_json();
        let ended = match plan.outcome {
            pc::Outcome::Unchanged => self.end("unchanged", &plan.reasons.join("; "))?,
            pc::Outcome::NeedsUser => self.end("needs-user", &plan.reasons.join("; "))?,
            pc::Outcome::Proceed if !install => self.end("unchanged", "plan only: nothing was changed")?,
            pc::Outcome::Proceed => {
                let end = pc::execute(dir, &plan, &observed, &pkg, &self.log)?;
                if end.state == "installed" {
                    pc::verify_installed(dir, &pkg, &self.log)?;
                    self.write_record(&v, &m, &Some(art.clone()), &None)?;
                }
                self.end_with(end.state, &end.reason, end.state == "unchanged")?
            }
        };
        Ok((ended, report))
    }
}

/// The two files the PC plan installs, read from a zip whose members already passed `verify_zip`.
pub fn read_package(zip_path: &Path, art: &Artifact) -> Result<pc::Package> {
    use std::io::Read;
    let file = std::fs::File::open(zip_path).map_err(|e| Error::new(9, format!("{}: {e}", zip_path.display())))?;
    let mut z = zip::ZipArchive::new(file).map_err(|e| Error::new(9, e.to_string()))?;
    let mut take = |name: &str| -> Result<Vec<u8>> {
        let c = art.contents.iter().find(|c| c.path == name).ok_or_else(|| Error::new(9, format!("the windows-zip lists no {name}")))?;
        let mut e = z.by_name(name).map_err(|e| Error::new(9, format!("{name}: {e}")))?;
        let mut b = Vec::with_capacity(c.size as usize);
        e.by_ref().take(c.size + 1).read_to_end(&mut b).map_err(|e| Error::new(9, format!("{name}: {e}")))?;
        Ok(b)
    };
    Ok(pc::Package { host: take(pc::HOST)?, runtime: take(pc::RUNTIME)? })
}
