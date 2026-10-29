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
    /// The nevr root (ADR 0012): where the PC plan puts the runtime, the record and every set-aside file. `None` when the
    /// player's profile folder is unknown; a PC install then asks for `--nevr-root` and changes nothing.
    pub nevr_root: Option<PathBuf>,
    /// Install an older release over a newer one (a PC install refuses by default).
    pub allow_downgrade: bool,
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
            nevr_root: None,
            allow_downgrade: false,
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

    /// The end of a run whose files have changed: the state is the files', so a failed `run_end` write is added to the
    /// reason instead of replacing the state.
    fn end_after_change(&self, state: &'static str, reason: &str, failed: bool) -> Ended {
        match self.end_with(state, reason, failed) {
            Ok(end) => end,
            Err(le) => Ended { state, reason: format!("{reason}; and the log could not record the end: {le}"), failed },
        }
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
        if let Err(f) = fetch_capped(&url, 0, max, &mut body) {
            let (Failure::Interrupted(m) | Failure::Refused(m)) = &f;
            let _ = self.log.event("WARN", "manifest_fetch", 1, json!({"url": url, "ok": false, "reason": m}));
            return Err(f);
        }
        let _ = self.log.info("manifest_fetch", 1, json!({"url": url, "bytes": body.len(), "status": 200}));
        Ok(body)
    }

    /// Steps 1 to 6: fetch the pair, verify it, store it. A signature failure fetches the pair again, twice. When the
    /// pair cannot be fetched at all (step 1), the pair an earlier run stored is verified again, against the pins and
    /// the clock, and used if it still passes (`manifest_check source=stored`); a stored pair that is stale, damaged
    /// or absent fails the run with the fetch error.
    pub fn manifest(&self) -> Result<Verified> {
        match self.manifest_online() {
            Err(e) if e.step == 1 => self.manifest_stored(&e),
            r => r,
        }
    }

    fn manifest_stored(&self, fetch_error: &Error) -> Result<Verified> {
        let again = |why: String| Error::new(1, format!("{}; the manifest stored by an earlier run cannot be used: {why}", fetch_error.msg));
        let dir = &self.cfg.state_dir;
        let state = manifest::read_state(dir).map_err(|e| again(e.msg))?;
        let read = |n: &str| std::fs::read(dir.join(n)).map_err(|e| again(format!("{n}: {e}")));
        let v = manifest::verify(&read("manifest.json")?, &read("manifest.json.sig")?, &self.cfg.pins, &state, &clock(&self.cfg)?, self.cfg.min_manifest_version).map_err(|e| again(e.to_string()))?;
        if v.sha256 != state.last_sha256 {
            return Err(again("it is not the pair state.json names".into()));
        }
        self.log.info("manifest_check", 6, json!({"ok": true, "source": "stored", "key_id": v.key_id, "manifest_version": v.version, "stored_highest": state.highest}))?;
        Ok(v)
    }

    fn manifest_online(&self) -> Result<Verified> {
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
                    self.log.info("manifest_check", 6, json!({"ok": true, "source": "network", "key_id": v.key_id, "manifest_version": v.version, "stored_highest": state.highest}))?;
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
        self.log.info("manifest_check", 6, json!({"ok": true, "source": "dir", "key_id": v.key_id, "manifest_version": v.version, "stored_highest": state.highest}))?;
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
    ///
    /// `dev_apk` is a local tester APK (`--artifact-dir`, ADR 0023 section 3a): it is checked on its own terms, takes the
    /// place of the manifest's `quest-apk`, and marks every record the run writes `trust: "unverified dev artifact"`.
    pub fn quest(&self, version: Option<&str>, dev: Option<&dyn Device>, install: bool, consent_uninstall: bool, dev_apk: Option<&quest::DevApk>) -> Result<(Ended, Value)> {
        let (m, v) = self.release(version)?;
        if let Some(d) = dev_apk {
            self.log.event("WARN", "dev_artifact", 8, quest::with_trust(json!({"kind": "quest-apk", "path": d.path.display().to_string(), "size": d.size, "sha256": d.sha256, "package": d.android.package, "version_code": d.android.version_code, "signers": d.android.signer_cert_sha256, "ok": true}), true))?;
        }
        // The chosen release's own game data; failing that, the newest listed one, so a device that already holds
        // complete data is recognised even when this release lists none.
        let data_for_observation = manifest::find(&m, &v, "game-data", Some("android-arm64")).ok().or_else(|| manifest::latest_game_data(&m).map(|x| x.1));
        let observed = match dev {
            Some(d) => quest::observe(d, data_for_observation.as_ref(), &self.log)?,
            None => quest::Observed::default(),
        };
        let plan = quest::plan_with(&m, &v, &observed, consent_uninstall, dev_apk);
        self.log.info("plan", 9, quest::with_trust(json!({"steps": plan.steps.iter().map(|s| s.name()).collect::<Vec<_>>(), "installed": observed.installed.as_ref().map(|i| json!({"version_code": i.version_code, "signers": i.signers})), "decision": plan.decision, "reasons": plan.reasons}), dev_apk.is_some()))?;
        let report = plan.to_json();
        let ended = match plan.outcome {
            // A refused plan under `install` is a failed request: nothing was installed, and a caller must not read exit 0.
            quest::Outcome::Unchanged => self.end_with("unchanged", &plan.reasons.join("; "), install && plan.decision == "refused")?,
            quest::Outcome::NeedsUser => self.end("needs-user", &plan.reasons.join("; "))?,
            quest::Outcome::Proceed if !install => self.end("unchanged", "plan only: nothing was changed")?,
            quest::Outcome::Proceed => {
                let dev = dev.ok_or_else(|| Error::new(9, "install needs a device"))?;
                let opts = quest::RunOpts { work_dir: self.cfg.work_dir.clone(), base: &self.cfg.base, consent_uninstall };
                let end = quest::execute(dev, &m, &v, &plan, &opts, &self.log)?;
                if end.state == "installed" {
                    self.write_record(&v, &m, &plan.apk, &plan.data, dev_apk.is_some())?;
                }
                self.end_with(end.state, &end.reason, end.state == "unchanged")?
            }
        };
        Ok((ended, report))
    }

    /// The install record: written last, by a temporary file and a rename.
    fn write_record(&self, version: &str, m: &Value, a: &Option<Artifact>, b: &Option<Artifact>, dev_artifact: bool) -> Result<()> {
        let state = manifest::read_state(&self.cfg.state_dir)?;
        let arts: Vec<Value> = [a, b].iter().filter_map(|x| x.as_ref()).map(|x| json!({"kind": x.kind, "path": x.path, "sha256": x.sha256})).collect();
        let rec = json!({
            "release": version,
            "manifest_version": m["manifest_version"],
            "last_manifest_sha256": state.last_sha256,
            "artifacts": arts,
            "dev_artifact": dev_artifact,
            "layout": if self.cfg.platform == "quest" { quest::MEDIA_ROOT } else { "host in the game folder, everything else in the nevr root" },
            "time": crate::util::now_utc().unwrap_or_default(),
        });
        crate::util::write_atomic(&self.cfg.state_dir.join("install-record.json"), &serde_json::to_vec_pretty(&rec).expect("record serializes")).map_err(|e| Error::new(9, format!("install-record.json: {e}")))
    }

    /// The PC command: verify the zip, plan against `dir` (the game folder) and the nevr root, and with `install` execute.
    ///
    /// `package_dir` is the folder an installer script ships in (the Windows zip, unpacked): when its two DLLs are
    /// exactly the manifest's entries for the release (size and sha256), they are installed and nothing is downloaded;
    /// when either is missing or differs, the package the manifest names is downloaded as usual.
    pub fn pc(&self, version: Option<&str>, dir: &Path, install: bool, game_running: bool, package_dir: Option<&Path>) -> Result<(Ended, Value)> {
        let (m, v) = self.release(version)?;
        let art = manifest::find(&m, &v, "windows-zip", None)?;
        // The root is settled before anything is fetched or changed.
        let root = match &self.cfg.nevr_root {
            Some(r) if r.is_absolute() => r.clone(),
            Some(r) => return self.pc_stop(&format!("--nevr-root {} is not an absolute path", r.display())),
            None => return self.pc_stop("the nevr root is unknown (LOCALAPPDATA is not set); pass --nevr-root"),
        };
        let layout = pc::Layout::new(dir, root);
        let local = match package_dir {
            Some(d) => self.local_package(d, &art, &v)?,
            None => None,
        };
        let pkg = match local {
            Some(p) => p,
            None => {
                let zip_path = self.cfg.work_dir.join(art.path.rsplit('/').next().unwrap_or("windows.zip"));
                std::fs::create_dir_all(&self.cfg.work_dir).map_err(|e| Error::new(8, format!("{}: {e}", self.cfg.work_dir.display())))?;
                let url = format!("{}/{}", self.cfg.base, art.path);
                let f = artifact::download(&url, &art, &zip_path)?;
                self.log.info("artifact_fetch", 8, json!({"kind": "windows-zip", "url": url, "size": art.size, "bytes": f.bytes, "sha256": f.sha256, "ok": true, "resumed_from": f.resumed_from}))?;
                let n = artifact::verify_zip(&zip_path, &art, false)?;
                self.log.info("verify_installed", 8, json!({"ok": true, "what": "zip-members", "expected": art.contents.len(), "observed": n}))?;
                // The bytes that are planned and installed are the bytes just checked against the manifest, not a second read.
                read_package(&zip_path, &art, &v)?
            }
        };
        let observed = pc::observe(&layout, game_running)?;
        let plan = pc::plan(&observed, &pkg, self.cfg.allow_downgrade);
        self.log.info("plan", 9, json!({"decision": plan.decision, "reasons": plan.reasons, "action": plan.action.map(|a| format!("{a:?}")), "installed": {"host_sha256": observed.target_sha, "runtime_sha256": observed.runtime_sha}, "nevr_root": layout.root.display().to_string()}))?;
        let report = plan.to_json();
        let ended = match plan.outcome {
            pc::Outcome::Unchanged => self.end("unchanged", &plan.reasons.join("; "))?,
            pc::Outcome::NeedsUser => self.end("needs-user", &plan.reasons.join("; "))?,
            pc::Outcome::Proceed if !install => self.end("unchanged", "plan only: nothing was changed")?,
            pc::Outcome::Proceed => {
                let end = pc::execute(&layout, &plan, &observed, &pkg, &self.log)?;
                // From here the folders have changed (or been put back): a log that cannot be written is reported in the
                // end, never allowed to turn the run into `unchanged`.
                if end.state == "installed" {
                    // The files are installed and read back; the installer's own record of it is informational, so a
                    // failure to write it is reported, not turned into a failed install.
                    match self.write_record(&v, &m, &Some(art.clone()), &None, false) {
                        Ok(()) => self.end_after_change(end.state, &end.reason, false),
                        Err(e) => {
                            let lost = self.log.error("install_record", 9, json!({"ok": false, "reason": e.to_string()})).err();
                            let reason = format!("installed, but install-record.json was not written: {e}");
                            self.end_after_change("installed", &match lost { Some(l) => format!("{reason}; and the log could not record it: {l}"), None => reason }, false)
                        }
                    }
                } else {
                    self.end_after_change(end.state, &end.reason, end.state == "unchanged")
                }
            }
        };
        Ok((ended, report))
    }

    /// A PC run that stops before it fetches or changes anything: the player has to give the root.
    fn pc_stop(&self, why: &str) -> Result<(Ended, Value)> {
        self.log.error("plan", 9, json!({"decision": "needs-user", "reasons": [why]}))?;
        let report = json!({"outcome": "needs-user", "decision": "needs-user", "reasons": [why], "action": null, "stale_record": false});
        Ok((self.end("needs-user", why)?, report))
    }
}

impl Session {
    /// The package folder's two DLLs against the manifest's entries (`package_local`). `None` says why in the log and
    /// sends the run to the download; the bytes returned were read once, hashed, and are the ones the plan installs.
    fn local_package(&self, dir: &Path, art: &Artifact, version: &str) -> Result<Option<pc::Package>> {
        let miss = |why: String| -> Result<Option<pc::Package>> {
            self.log.event("WARN", "package_local", 8, json!({"dir": dir.display().to_string(), "ok": false, "reason": why}))?;
            Ok(None)
        };
        let mut files = Vec::new();
        for name in [pc::HOST, pc::RUNTIME] {
            let Some(c) = art.contents.iter().find(|c| c.path == name) else {
                return miss(format!("the manifest lists no {name}"));
            };
            let bytes = match std::fs::read(dir.join(name)) {
                Ok(b) => b,
                Err(e) => return miss(format!("{name}: {e}")),
            };
            let sha = crate::util::sha256_hex(&bytes);
            if bytes.len() as u64 != c.size || sha != c.sha256 {
                return miss(format!("{name} is {} bytes with sha256 {sha}, the manifest lists {} bytes with sha256 {}", bytes.len(), c.size, c.sha256));
            }
            files.push(bytes);
        }
        let runtime = files.pop().expect("two files");
        let host = files.pop().expect("two files");
        self.log.info("package_local", 8, json!({"dir": dir.display().to_string(), "ok": true, "files": [pc::HOST, pc::RUNTIME]}))?;
        Ok(Some(pc::Package { host, runtime, version: version.to_string() }))
    }
}

/// The two files the PC plan installs, read from the zip. The zip is read into memory once and hashed whole against the
/// manifest, then each member is hashed against `contents`: what is returned is exactly what the manifest names, even if
/// the file on disk was swapped after `verify_zip` looked at it.
pub fn read_package(zip_path: &Path, art: &Artifact, version: &str) -> Result<pc::Package> {
    use std::io::Read;
    let bytes = std::fs::read(zip_path).map_err(|e| Error::new(9, format!("{}: {e}", zip_path.display())))?;
    if bytes.len() as u64 != art.size || crate::util::sha256_hex(&bytes) != art.sha256 {
        return Err(Error::new(8, format!("{} does not match the manifest's size and sha256 when it is read for the install", zip_path.display())));
    }
    let mut z = zip::ZipArchive::new(std::io::Cursor::new(bytes)).map_err(|e| Error::new(9, e.to_string()))?;
    let mut take = |name: &str| -> Result<Vec<u8>> {
        let c = art.contents.iter().find(|c| c.path == name).ok_or_else(|| Error::new(9, format!("the windows-zip lists no {name}")))?;
        let mut e = z.by_name(name).map_err(|e| Error::new(9, format!("{name}: {e}")))?;
        let mut b = Vec::with_capacity(c.size as usize);
        e.by_ref().take(c.size + 1).read_to_end(&mut b).map_err(|e| Error::new(9, format!("{name}: {e}")))?;
        if b.len() as u64 != c.size || crate::util::sha256_hex(&b) != c.sha256 {
            return Err(Error::new(8, format!("{name} in the zip does not match the manifest's size and sha256")));
        }
        Ok(b)
    };
    Ok(pc::Package { host: take(pc::HOST)?, runtime: take(pc::RUNTIME)?, version: version.to_string() })
}
