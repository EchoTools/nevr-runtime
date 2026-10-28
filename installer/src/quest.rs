//! The Quest plan (ADR 0023, section 3): what to do to a headset, as data, from the release and what was observed; and
//! its execution over a `Device`. The plan never uninstalls without consent and never claims a signer it could not read.

use crate::artifact;
use crate::device::{run_with_timeout, Device};
use crate::error::{Error, Result};
use crate::log::Log;
use crate::manifest::{self, Android, Artifact};
use serde_json::{json, Value};
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::time::Duration;

pub const PACKAGE: &str = "com.readyatdawn.r15";
/// Where game data goes (ADR 0013: no storage permission needed; where the community installers already put it).
pub const MEDIA_ROOT: &str = "/sdcard/Android/media/com.readyatdawn.r15/files";
/// Where this tree's earlier scripts wrote it.
pub const LEGACY_ROOT: &str = "/sdcard/readyatdawn";
const REMOTE_ZIP: &str = "/data/local/tmp/_nevr_data.zip";
const PLATFORM: &str = "android-arm64";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Installed {
    pub version_code: u64,
    /// SHA-256 digests of the signing certificates; `None` when the backend cannot read them (adb).
    pub signers: Option<Vec<String>>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DataRoots {
    pub legacy: bool,
    pub media: bool,
}

#[derive(Debug, Clone, Default)]
pub struct Observed {
    pub installed: Option<Installed>,
    /// `None`: not observed (no device).
    pub data: Option<DataRoots>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Proceed,
    Unchanged,
    NeedsUser,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Step {
    FetchApk,
    CheckApk,
    Uninstall,
    InstallApk { replace: bool },
    FetchData,
    ExtractData,
    VerifyInstalled,
}

impl Step {
    pub fn name(&self) -> &'static str {
        match self {
            Step::FetchApk => "fetch-apk",
            Step::CheckApk => "check-apk",
            Step::Uninstall => "uninstall",
            Step::InstallApk { replace: true } => "install-apk-replace",
            Step::InstallApk { replace: false } => "install-apk",
            Step::FetchData => "fetch-data",
            Step::ExtractData => "extract-data",
            Step::VerifyInstalled => "verify-installed",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Plan {
    pub outcome: Outcome,
    pub decision: String,
    pub reasons: Vec<String>,
    pub steps: Vec<Step>,
    pub apk: Option<Artifact>,
    pub data: Option<Artifact>,
    /// What an uninstall deletes, listed when one is part of or asked for by the plan.
    pub consequences: Vec<String>,
    pub game_data: String,
    pub signer: String,
}

impl Plan {
    pub fn to_json(&self) -> Value {
        json!({
            "outcome": match self.outcome { Outcome::Proceed => "proceed", Outcome::Unchanged => "unchanged", Outcome::NeedsUser => "needs-user" },
            "decision": self.decision,
            "reasons": self.reasons,
            "steps": self.steps.iter().map(Step::name).collect::<Vec<_>>(),
            "apk": self.apk.as_ref().map(|a| json!({"path": a.path, "size": a.size, "sha256": a.sha256, "version_code": a.android.as_ref().map(|x| x.version_code)})),
            "game_data_artifact": self.data.as_ref().map(|a| json!({"path": a.path, "size": a.size, "sha256": a.sha256})),
            "uninstall_deletes": self.consequences,
            "game_data": self.game_data,
            "signer": self.signer,
        })
    }
}

fn consequences() -> Vec<String> {
    vec![
        format!("/data/data/{PACKAGE} (the app's private data)"),
        format!("/sdcard/Android/data/{PACKAGE} (settings and saves kept by the game)"),
        format!("the OBB of {PACKAGE}"),
        format!("{MEDIA_ROOT} may go with it; whether it survives was not measured (#965)"),
    ]
}

/// The Quest plan. `consent_uninstall` is the player's explicit agreement to remove a differently signed install.
pub fn plan(m: &Value, version: &str, obs: &Observed, consent_uninstall: bool) -> Plan {
    let mut p = Plan {
        outcome: Outcome::Proceed,
        decision: String::new(),
        reasons: vec![],
        steps: vec![],
        apk: None,
        data: None,
        consequences: vec![],
        game_data: String::new(),
        signer: String::new(),
    };
    let refuse = |mut p: Plan, why: String| {
        p.outcome = Outcome::Unchanged;
        p.decision = "refused".into();
        p.reasons.push(why);
        p
    };
    let apk = match manifest::find(m, version, "quest-apk", Some(PLATFORM)) {
        Ok(a) => a,
        Err(e) => return refuse(p, e.msg),
    };
    let android = match &apk.android {
        Some(a) if a.package == PACKAGE => a.clone(),
        Some(a) => return refuse(p, format!("the quest-apk is for package {}, not {PACKAGE}", a.package)),
        None => return refuse(p, "the quest-apk entry has no android object".into()),
    };
    if android.signer_cert_sha256.is_empty() {
        return refuse(p, "the quest-apk entry has no signer_cert_sha256: nothing to check the signer against".into());
    }
    p.apk = Some(apk);
    p.signer = match &obs.installed {
        None => "no package installed".into(),
        Some(Installed { signers: Some(s), .. }) => format!("installed signer(s) {}", s.join(",")),
        Some(Installed { signers: None, .. }) => "unknown-until-install".into(),
    };

    // The APK.
    let mut needs_user = false;
    let mut apk_steps: Vec<Step> = vec![];
    match &obs.installed {
        None => {
            p.decision = "install".into();
            apk_steps = vec![Step::FetchApk, Step::CheckApk, Step::InstallApk { replace: false }];
        }
        Some(inst) => {
            let want: BTreeSet<&String> = android.signer_cert_sha256.iter().collect();
            let same_signer = match &inst.signers {
                Some(s) => Some(s.iter().collect::<BTreeSet<_>>() == want),
                None => None,
            };
            if same_signer == Some(false) {
                p.consequences = consequences();
                if consent_uninstall {
                    p.decision = "reinstall-after-uninstall".into();
                    apk_steps = vec![Step::FetchApk, Step::CheckApk, Step::Uninstall, Step::InstallApk { replace: false }];
                } else {
                    p.decision = "signer-mismatch".into();
                    p.reasons.push("the installed package is signed differently and cannot be updated in place; removing it deletes the items listed in uninstall_deletes. Re-run with consent to allow it".into());
                    needs_user = true;
                }
            } else if inst.version_code > android.version_code {
                p.decision = "downgrade-refused".into();
                p.reasons.push(format!("installed versionCode {} is above the release's {}; Android cannot downgrade", inst.version_code, android.version_code));
                needs_user = true;
            } else if inst.version_code == android.version_code {
                p.decision = "apk-current".into();
            } else {
                p.decision = "update".into();
                apk_steps = vec![Step::FetchApk, Step::CheckApk, Step::InstallApk { replace: true }];
            }
        }
    }

    // The game data: present in either root counts as present.
    let data_present = obs.data.map(|d| d.legacy || d.media);
    p.game_data = match obs.data {
        None => "presence not observed".into(),
        Some(d) if d.legacy || d.media => format!("present in {}", [(d.media, MEDIA_ROOT), (d.legacy, LEGACY_ROOT)].iter().filter(|x| x.0).map(|x| x.1).collect::<Vec<_>>().join(" and ")),
        Some(_) => "absent".into(),
    };
    let mut data_steps: Vec<Step> = vec![];
    if data_present != Some(true) {
        match manifest::find(m, version, "game-data", Some(PLATFORM)) {
            Ok(a) => {
                p.game_data = format!("{}; the release lists {} ({} bytes), extracted under {MEDIA_ROOT}", p.game_data, a.path, a.size);
                p.data = Some(a);
                data_steps = vec![Step::FetchData, Step::ExtractData];
            }
            Err(_) => {
                p.game_data = format!("{}; game-data: none in release {version}", p.game_data);
                p.reasons.push(format!("the game data is not on the device and release {version} lists no game-data artifact"));
                needs_user = true;
            }
        }
    }

    if needs_user {
        p.outcome = Outcome::NeedsUser;
        return p;
    }
    p.steps = apk_steps;
    p.steps.extend(data_steps);
    if p.steps.is_empty() {
        p.outcome = Outcome::Unchanged;
        p.decision = "unchanged".into();
        p.reasons.push("the package is current and the game data is present".into());
    } else {
        p.steps.push(Step::VerifyInstalled);
    }
    p
}

/// `package: name='x' versionCode='N' ...` from `aapt2 dump badging`.
pub fn parse_badging(text: &str) -> Option<(String, u64)> {
    let line = text.lines().find(|l| l.starts_with("package: "))?;
    let field = |key: &str| -> Option<String> {
        let tag = format!("{key}='");
        let start = line.find(&tag)? + tag.len();
        Some(line[start..].split('\'').next()?.to_string())
    };
    Some((field("name")?, field("versionCode")?.parse().ok()?))
}

/// The distinct `certificate SHA-256 digest:` values of `apksigner verify --print-certs`.
pub fn parse_certs(text: &str) -> Vec<String> {
    let mut v: Vec<String> = text
        .lines()
        .filter_map(|l| l.split("certificate SHA-256 digest: ").nth(1))
        .map(|d| d.trim().to_string())
        .filter(|d| crate::util::is_sha256_hex(d))
        .collect();
    v.sort();
    v.dedup();
    v
}

/// The APK itself against the manifest's `android` object, before install (host-side `aapt2` and `apksigner`).
pub fn check_apk(apk: &Path, want: &Android) -> Result<Vec<String>> {
    let p = apk.to_string_lossy();
    let badging = run_with_timeout("aapt2", &["dump", "badging", &p], Duration::from_secs(120)).map_err(|e| Error::new(8, e))?;
    if badging.code != 0 {
        return Err(Error::new(8, format!("aapt2 could not read {p}")));
    }
    let (pkg, code) = parse_badging(&badging.text).ok_or_else(|| Error::new(8, "aapt2 printed no package line"))?;
    if pkg != want.package {
        return Err(Error::new(8, format!("the APK package is '{pkg}', the manifest says '{}'", want.package)));
    }
    if code != want.version_code {
        return Err(Error::new(8, format!("the APK versionCode is '{code}', the manifest says '{}'", want.version_code)));
    }
    let certs = run_with_timeout("apksigner", &["verify", "--print-certs", &p], Duration::from_secs(120)).map_err(|e| Error::new(8, e))?;
    if certs.code != 0 {
        return Err(Error::new(8, format!("apksigner does not accept {p}")));
    }
    let digests = parse_certs(&certs.text);
    if digests.is_empty() {
        return Err(Error::new(8, "apksigner printed no signing certificate"));
    }
    if let Some(d) = digests.iter().find(|d| !want.signer_cert_sha256.contains(d)) {
        return Err(Error::new(8, format!("the APK is signed by {d}, which the manifest does not list")));
    }
    Ok(digests)
}

fn sh_quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// Reads the headset: the package, the free space, and which roots hold every `contents` path of `data` at its size.
pub fn observe(dev: &dyn Device, data: Option<&Artifact>, log: &Log) -> Result<Observed> {
    let io = |e: String| Error::new(9, e);
    let pm = dev.shell(&format!("pm path {PACKAGE}"), 30).map_err(io)?;
    let installed = if pm.code == 0 && pm.text.contains("package:") {
        let d = dev.shell(&format!("dumpsys package {PACKAGE}"), 30).map_err(io)?;
        let code = d.text.lines().find_map(|l| l.trim().strip_prefix("versionCode=")).and_then(|r| r.split_whitespace().next()).and_then(|n| n.parse::<u64>().ok());
        Some(Installed { version_code: code.ok_or_else(|| Error::new(9, "dumpsys printed no versionCode for the installed package"))?, signers: None })
    } else if pm.code == 0 || pm.text.trim().is_empty() {
        None
    } else {
        return Err(Error::new(9, format!("package check failed (exit {}): {}", pm.code, pm.text.trim())));
    };
    let mut facts = json!({"serial_fp": dev.fingerprint(), "package": installed.as_ref().map(|i| json!({"version_code": i.version_code, "signers": "unknown-until-install"}))});
    for (key, prop) in [("model", "ro.product.model"), ("android_sdk", "ro.build.version.sdk")] {
        if let Ok(o) = dev.shell(&format!("getprop {prop}"), 30) {
            facts[key] = json!(o.text.trim());
        }
    }
    for (key, path) in [("free_kb_sdcard", "/sdcard"), ("free_kb_tmp", "/data/local/tmp")] {
        let o = dev.shell(&format!("df -Pk {path}"), 30).map_err(io)?;
        facts[key] = json!(o.text.lines().nth(1).and_then(|l| l.split_whitespace().nth(3)).and_then(|n| n.parse::<u64>().ok()));
    }
    let roots = match data {
        Some(a) => Some(DataRoots { media: roots_hold(dev, MEDIA_ROOT, a)?, legacy: roots_hold(dev, LEGACY_ROOT, a)? }),
        None => None,
    };
    facts["data_present"] = json!(roots.map(|r| json!({"media": r.media, "legacy": r.legacy})));
    log.info("device", 9, facts)?;
    Ok(Observed { installed, data: roots })
}

/// Whether every `contents` path of `data` exists under `root` with the listed size.
pub fn roots_hold(dev: &dyn Device, root: &str, data: &Artifact) -> Result<bool> {
    if data.contents.is_empty() {
        return Ok(false);
    }
    let list: Vec<String> = data.contents.iter().map(|c| sh_quote(&format!("{root}/{}", c.path))).collect();
    let o = dev.shell(&format!("for f in {}; do stat -c '%n %s' \"$f\" 2>/dev/null; done", list.join(" ")), 120).map_err(|e| Error::new(9, e))?;
    let found: Vec<(&str, u64)> = o.text.lines().filter_map(|l| l.rsplit_once(' ')).filter_map(|(n, s)| Some((n, s.trim().parse().ok()?))).collect();
    Ok(data.contents.iter().all(|c| found.iter().any(|(n, s)| *n == format!("{root}/{}", c.path) && *s == c.size)))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct End {
    pub state: &'static str,
    pub reason: String,
}

pub struct RunOpts<'a> {
    pub work_dir: PathBuf,
    pub base: &'a str,
    pub consent_uninstall: bool,
}

fn step_log(log: &Log, step: &Step, ok: bool, reason: &str, started: u64) -> Result<()> {
    log.event(if ok { "INFO" } else { "ERROR" }, "step_end", 9, json!({"step_id": step.name(), "ok": ok, "reason": reason, "elapsed_ms": log.elapsed_ms() - started}))
}

/// Runs a `Proceed` plan against the headset. Every step is logged; a failure after nothing was changed is `unchanged`,
/// a failure after the package changed is `needs-user` with the reason (the next run starts from the plan again).
pub fn execute(dev: &dyn Device, m: &Value, version: &str, plan: &Plan, opts: &RunOpts, log: &Log) -> Result<End> {
    let mut apk_path: Option<PathBuf> = None;
    let mut data_zip: Option<PathBuf> = None;
    let mut changed = false;
    let fail = |changed: bool, why: String| -> Result<End> { Ok(End { state: if changed { "needs-user" } else { "unchanged" }, reason: why }) };
    std::fs::create_dir_all(&opts.work_dir).map_err(|e| Error::new(9, format!("{}: {e}", opts.work_dir.display())))?;
    for step in &plan.steps {
        let t0 = log.elapsed_ms();
        log.info("step_start", 9, json!({"step_id": step.name()}))?;
        let res: std::result::Result<(), String> = (|| match step {
            Step::FetchApk => {
                let a = plan.apk.as_ref().expect("apk in plan");
                let out = opts.work_dir.join(a.path.rsplit('/').next().unwrap_or("quest.apk"));
                let url = format!("{}/{}", opts.base, a.path);
                let f = artifact::download(&url, a, &out).map_err(|e| e.to_string())?;
                log.info("artifact_fetch", 8, json!({"kind": "quest-apk", "url": url, "size": a.size, "bytes": f.bytes, "sha256": f.sha256, "ok": true, "resumed_from": f.resumed_from})).map_err(|e| e.to_string())?;
                apk_path = Some(out);
                Ok(())
            }
            Step::CheckApk => {
                let a = plan.apk.as_ref().and_then(|a| a.android.as_ref()).expect("android in plan");
                let digests = check_apk(apk_path.as_ref().expect("fetched"), a).map_err(|e| e.to_string())?;
                log.info("verify_installed", 8, json!({"ok": true, "what": "apk", "expected": {"package": a.package, "version_code": a.version_code}, "observed": {"signers": digests}})).map_err(|e| e.to_string())?;
                Ok(())
            }
            Step::Uninstall => {
                log.info("consent", 9, json!({"what": "uninstall", "given": opts.consent_uninstall})).map_err(|e| e.to_string())?;
                let o = dev.uninstall(PACKAGE)?;
                changed = true;
                if o.code != 0 || !o.text.contains("Success") {
                    return Err(format!("uninstall of {PACKAGE} failed: {}", o.text.trim()));
                }
                Ok(())
            }
            Step::InstallApk { replace } => {
                let apk = apk_path.as_ref().expect("fetched");
                let o = dev.install(apk, *replace)?;
                if o.text.contains("INSTALL_FAILED_UPDATE_INCOMPATIBLE") {
                    log.info("consent", 9, json!({"what": "uninstall", "given": opts.consent_uninstall})).map_err(|e| e.to_string())?;
                    if !opts.consent_uninstall {
                        return Err("INSTALL_FAILED_UPDATE_INCOMPATIBLE: the installed package is signed differently; re-run with consent to uninstall it (this deletes the app data listed by the plan)".into());
                    }
                    let u = dev.uninstall(PACKAGE)?;
                    changed = true;
                    if u.code != 0 || !u.text.contains("Success") {
                        return Err(format!("uninstall of {PACKAGE} failed: {}", u.text.trim()));
                    }
                    let again = dev.install(apk, false)?;
                    if again.code != 0 || !again.text.contains("Success") {
                        return Err(format!("install after uninstall failed: {}", again.text.trim()));
                    }
                } else if o.code != 0 || !o.text.contains("Success") {
                    return Err(format!("adb install failed (exit {}): {}", o.code, o.text.trim()));
                }
                changed = true;
                Ok(())
            }
            Step::FetchData => {
                let a = plan.data.as_ref().expect("data in plan");
                let out = opts.work_dir.join(a.path.rsplit('/').next().unwrap_or("data.zip"));
                let url = format!("{}/{}", opts.base, a.path);
                let f = artifact::download(&url, a, &out).map_err(|e| e.to_string())?;
                log.info("artifact_fetch", 8, json!({"kind": "game-data", "url": url, "size": a.size, "bytes": f.bytes, "sha256": f.sha256, "ok": true, "resumed_from": f.resumed_from})).map_err(|e| e.to_string())?;
                artifact::verify_zip(&out, a, true).map_err(|e| e.to_string())?;
                data_zip = Some(out);
                Ok(())
            }
            Step::ExtractData => {
                let zip = data_zip.as_ref().expect("fetched");
                let o = dev.push(zip, REMOTE_ZIP)?;
                if o.code != 0 {
                    return Err(format!("adb push failed: {}", o.text.trim()));
                }
                let x = dev.shell(&format!("mkdir -p {r} && cd {r} && unzip -o {z}", r = sh_quote(MEDIA_ROOT), z = sh_quote(REMOTE_ZIP)), 900);
                let _ = dev.shell(&format!("rm -f {}", sh_quote(REMOTE_ZIP)), 60);
                changed = true;
                let x = x?;
                if x.code != 0 {
                    return Err(format!("unzip on the headset failed (partial files may remain in {MEDIA_ROOT}): {}", x.text.trim()));
                }
                Ok(())
            }
            Step::VerifyInstalled => {
                let after = observe(dev, plan.data.as_ref().or(None), log).map_err(|e| e.to_string())?;
                let apk = plan.apk.as_ref().and_then(|a| a.android.as_ref()).expect("android");
                let inst = after.installed.ok_or("the package is not installed after the install")?;
                if inst.version_code != apk.version_code && plan.steps.iter().any(|s| matches!(s, Step::InstallApk { .. })) {
                    return Err(format!("installed versionCode is {}, the manifest says {}", inst.version_code, apk.version_code));
                }
                if plan.data.is_some() && after.data.map(|d| d.media) != Some(true) {
                    return Err(format!("game data under {MEDIA_ROOT} does not match the manifest's contents"));
                }
                log.info("verify_installed", 10, json!({"ok": true, "expected": {"version_code": apk.version_code}, "observed": {"version_code": inst.version_code, "data_media_root": after.data.map(|d| d.media)}})).map_err(|e| e.to_string())?;
                Ok(())
            }
        })();
        match res {
            Ok(()) => step_log(log, step, true, "", t0)?,
            Err(why) => {
                step_log(log, step, false, &why, t0)?;
                return fail(changed, why);
            }
        }
    }
    let _ = m;
    let _ = version;
    Ok(End { state: "installed", reason: String::new() })
}

/// The first-launch witness (ADR 0013): the `data_check` line that says the game used the data it found.
pub fn launch_witness(log_text: &str) -> Option<&str> {
    log_text.lines().find(|l| l.contains("data_check") && l.contains("use"))
}
