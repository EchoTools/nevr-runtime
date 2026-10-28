//! `nevr-install`: the CLI over the core (ADR 0023). One JSON object on stdout per run; the details are in the log.
//! Exit codes: 0 installed or nothing to do, 1 a check or step failed (nothing changed), 2 needs the player, 3 restored.

use nevr_install::device::AdbDevice;
use nevr_install::manifest::Pins;
use nevr_install::session::{Config, Ended, Session};
use serde_json::{json, Value};
use std::path::PathBuf;
use std::process::ExitCode;

const USAGE: &str = "usage: nevr-install <command> [options]
  manifest verify [--from-dir DIR]          steps 1 to 7: fetch (or read DIR), verify, store, pick the channel's release
  quest plan|install [--no-device] [--serial S] [--version V] [--yes-uninstall]
  pc plan|install --dir <folder with echovr.exe> [--nevr-root DIR] [--allow-downgrade] [--version V]
  pc launch-check --dir <folder> [--nevr-root DIR] [--bootstrap-log FILE] [--version V]
options: --root DIR  --base URL  --channel NAME  --log FILE  --state-dir DIR  --work-dir DIR  --pins FILE  --now UTC
exit codes: 0 installed or nothing to do, 1 failed with nothing changed, 2 needs the player, 3 restored";

fn default_root() -> PathBuf {
    if let Ok(r) = std::env::var("NEVR_INSTALL_ROOT") {
        return PathBuf::from(r);
    }
    if cfg!(windows) {
        if let Ok(p) = std::env::var("LOCALAPPDATA") {
            return PathBuf::from(p).join("nevr").join("installer");
        }
    }
    if let Ok(x) = std::env::var("XDG_STATE_HOME") {
        return PathBuf::from(x).join("nevr-install");
    }
    PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into())).join(".local").join("state").join("nevr-install")
}

struct Args {
    rest: Vec<String>,
    opts: Vec<(String, Option<String>)>,
}

impl Args {
    fn parse(argv: Vec<String>) -> Args {
        let flags = ["--no-device", "--yes-uninstall", "--allow-downgrade"];
        let (mut rest, mut opts) = (vec![], vec![]);
        let mut it = argv.into_iter();
        while let Some(a) = it.next() {
            if a.starts_with("--") {
                if flags.contains(&a.as_str()) {
                    opts.push((a, None));
                } else {
                    let v = it.next();
                    opts.push((a, v));
                }
            } else {
                rest.push(a);
            }
        }
        Args { rest, opts }
    }
    fn val(&self, k: &str) -> Option<String> {
        self.opts.iter().find(|o| o.0 == k).and_then(|o| o.1.clone())
    }
    fn flag(&self, k: &str) -> bool {
        self.opts.iter().any(|o| o.0 == k)
    }
}

fn finish(sess: Option<&Session>, ended: Ended, plan: Option<Value>) -> ExitCode {
    let code = ended.exit_code();
    let out = json!({"state": ended.state, "reason": ended.reason, "exit": code, "failed": ended.failed, "log": sess.map(|s| s.log.path().display().to_string()), "run_id": sess.map(|s| s.log.run_id().to_string()), "plan": plan});
    println!("{out}");
    eprintln!("[{}] {}", ended.state, ended.reason);
    ExitCode::from(code as u8)
}

fn game_running() -> bool {
    if cfg!(windows) {
        return nevr_install::device::run_with_timeout("tasklist", &["/FI", "IMAGENAME eq echovr.exe", "/NH"], std::time::Duration::from_secs(20)).map(|o| o.text.to_lowercase().contains("echovr.exe")).unwrap_or(true);
    }
    std::fs::read_dir("/proc").map(|d| d.flatten().any(|e| std::fs::read_to_string(e.path().join("comm")).map(|c| c.trim() == "echovr.exe").unwrap_or(false))).unwrap_or(false)
}

fn main() -> ExitCode {
    let args = Args::parse(std::env::args().skip(1).collect());
    let words: Vec<&str> = args.rest.iter().map(String::as_str).collect();
    let (platform, cmd) = match words.as_slice() {
        ["manifest", "verify"] => ("pc", "manifest"),
        ["quest", c @ ("plan" | "install")] => ("quest", *c),
        ["pc", c @ ("plan" | "install" | "launch-check")] => ("pc", *c),
        _ => {
            eprintln!("{USAGE}");
            return ExitCode::from(1);
        }
    };
    let root = args.val("--root").map(PathBuf::from).unwrap_or_else(default_root);
    let mut cfg = Config::new(platform, &root);
    // The nevr root (ADR 0012): `--nevr-root`, else %LOCALAPPDATA%\nevr\echovr on Windows, else unknown.
    cfg.nevr_root = args.val("--nevr-root").map(PathBuf::from).or_else(|| {
        if cfg!(windows) {
            std::env::var("LOCALAPPDATA").ok().filter(|p| !p.is_empty()).map(|p| PathBuf::from(p).join("nevr").join("echovr"))
        } else {
            None
        }
    });
    cfg.allow_downgrade = args.flag("--allow-downgrade");
    if let Some(v) = args.val("--base") {
        cfg.base = v;
    }
    if let Some(v) = args.val("--channel") {
        cfg.channel = v;
    }
    if let Some(v) = args.val("--log") {
        cfg.log_path = PathBuf::from(v);
    }
    if let Some(v) = args.val("--state-dir") {
        cfg.state_dir = PathBuf::from(v);
    }
    if let Some(v) = args.val("--work-dir") {
        cfg.work_dir = PathBuf::from(v);
    }
    if let Some(v) = args.val("--now") {
        cfg.now = Some(v);
    }
    if let Some(v) = args.val("--pins") {
        match std::fs::read_to_string(&v).map_err(|e| e.to_string()).and_then(|t| Pins::parse(&t).map_err(|e| e.to_string())) {
            Ok(p) => cfg.pins = p,
            Err(e) => {
                eprintln!("FAIL step 2: --pins {v}: {e}");
                return ExitCode::from(1);
            }
        }
    }
    let sess = match Session::open(cfg, &format!("cli:{platform} {cmd}")) {
        Ok(s) => s,
        Err(e) => {
            // The run cannot be recorded, so it changes nothing.
            eprintln!("{e}");
            println!("{}", json!({"state": "unchanged", "reason": e.to_string(), "exit": 1, "failed": true, "log": null}));
            return ExitCode::from(1);
        }
    };
    let version = args.val("--version");
    let result = (|| -> nevr_install::error::Result<(Ended, Option<Value>)> {
        match (platform, cmd) {
            (_, "manifest") => {
                match args.val("--from-dir") {
                    Some(d) => sess.manifest_from_dir(std::path::Path::new(&d))?,
                    None => sess.manifest()?,
                };
                let (_, v) = sess.release(version.as_deref())?;
                Ok((sess.end("unchanged", &format!("manifest verified; channel {} -> {v}", sess.cfg.channel))?, None))
            }
            ("quest", c) => {
                if args.val("--from-dir").is_some() {
                    sess.manifest_from_dir(std::path::Path::new(&args.val("--from-dir").unwrap()))?;
                } else {
                    sess.manifest()?;
                }
                let dev = if args.flag("--no-device") {
                    None
                } else {
                    Some(AdbDevice::pick(args.val("--serial").as_deref()).map_err(|e| nevr_install::error::Error::new(9, e))?)
                };
                let (end, plan) = sess.quest(version.as_deref(), dev.as_ref().map(|d| d as &dyn nevr_install::device::Device), c == "install", args.flag("--yes-uninstall"))?;
                Ok((end, Some(plan)))
            }
            ("pc", "launch-check") => {
                let dir = PathBuf::from(args.val("--dir").ok_or_else(|| nevr_install::error::Error::new(11, "--dir is required"))?);
                sess.manifest()?;
                let (m, v) = sess.release(version.as_deref())?;
                let art = nevr_install::manifest::find(&m, &v, "windows-zip", None)?;
                let sha = |n: &str| art.contents.iter().find(|c| c.path == n).map(|c| c.sha256.clone()).unwrap_or_default();
                // The host writes its log under the nevr root (ADR 0012); the game folder is the fallback for an older host.
                let log_path = args.val("--bootstrap-log").map(PathBuf::from).unwrap_or_else(|| sess.cfg.nevr_root.as_ref().map(|r| r.join("logs")).unwrap_or_else(|| dir.join("logs")).join("nevr-bootstrap.jsonl"));
                let text = std::fs::read_to_string(&log_path).map_err(|e| nevr_install::error::Error::new(11, format!("{}: {e}", log_path.display())))?;
                match nevr_install::pc::launch_witness(&text, &sha("BugSplat64.dll"), &sha("nevr-runtime.dll")) {
                    Ok(w) => {
                        sess.log.info("launch_check", 11, json!({"ok": true, "witness": w, "file": log_path.display().to_string()}))?;
                        Ok((sess.end("unchanged", "launch check passed")?, None))
                    }
                    Err(why) => {
                        sess.log.error("launch_check", 11, json!({"ok": false, "reason": why, "file": log_path.display().to_string()}))?;
                        Ok((sess.end_with("needs-user", &why, false)?, None))
                    }
                }
            }
            ("pc", c) => {
                let dir = PathBuf::from(args.val("--dir").ok_or_else(|| nevr_install::error::Error::new(9, "--dir is required"))?);
                if let Some(d) = args.val("--from-dir") {
                    sess.manifest_from_dir(std::path::Path::new(&d))?;
                } else {
                    sess.manifest()?;
                }
                let (end, plan) = sess.pc(version.as_deref(), &dir, c == "install", game_running())?;
                Ok((end, Some(plan)))
            }
            _ => unreachable!("the command table above is exhaustive"),
        }
    })();
    match result {
        Ok((end, plan)) => finish(Some(&sess), end, plan),
        Err(e) => finish(Some(&sess), sess.fail(&e), None),
    }
}
