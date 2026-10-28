//! The headset as the Quest plan sees it: a handful of operations, so the plan and its execution are tested against a
//! fake and run against `adb`. `AdbDevice` runs the host's `adb` with a time limit on every call.

use std::io::Read;
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Output {
    pub code: i32,
    pub text: String,
}

pub trait Device {
    /// A stable fingerprint of the device identity for the log (never the serial itself).
    fn fingerprint(&self) -> String;
    fn shell(&self, cmd: &str, timeout_secs: u64) -> Result<Output, String>;
    fn install(&self, apk: &Path, replace: bool) -> Result<Output, String>;
    fn uninstall(&self, package: &str) -> Result<Output, String>;
    fn push(&self, local: &Path, remote: &str) -> Result<Output, String>;
}

/// Runs `program args...` and returns its exit code and combined stdout and stderr, killing it after `timeout`.
pub fn run_with_timeout(program: &str, args: &[&str], timeout: Duration) -> Result<Output, String> {
    let mut child = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("cannot run {program}: {e}"))?;
    let mut out = child.stdout.take().expect("piped");
    let mut err = child.stderr.take().expect("piped");
    let t_out = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = out.read_to_string(&mut s);
        s
    });
    let t_err = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = err.read_to_string(&mut s);
        s
    });
    let started = Instant::now();
    let status = loop {
        match child.try_wait().map_err(|e| format!("{program}: {e}"))? {
            Some(s) => break s,
            None if started.elapsed() > timeout => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("{program} {} timed out after {} s", args.join(" "), timeout.as_secs()));
            }
            None => std::thread::sleep(Duration::from_millis(50)),
        }
    };
    let mut text = t_out.join().unwrap_or_default();
    text.push_str(&t_err.join().unwrap_or_default());
    Ok(Output { code: status.code().unwrap_or(-1), text })
}

pub struct AdbDevice {
    serial: String,
}

impl AdbDevice {
    /// Exactly one authorized device, or an error that says which condition failed.
    pub fn pick(serial: Option<&str>) -> Result<AdbDevice, String> {
        let out = run_with_timeout("adb", &["devices"], Duration::from_secs(30))?;
        if out.code != 0 {
            return Err(format!("'adb devices' failed: {}", out.text.trim()));
        }
        let rows: Vec<(String, String)> = out
            .text
            .lines()
            .filter(|l| !l.starts_with('*') && !l.starts_with("List"))
            .filter_map(|l| {
                let mut p = l.split_whitespace();
                Some((p.next()?.to_string(), p.collect::<Vec<_>>().join(" ")))
            })
            .collect();
        let rows: Vec<_> = match serial {
            Some(s) => rows.into_iter().filter(|r| r.0 == s).collect(),
            None => rows,
        };
        match rows.len() {
            0 => Err("no adb device attached".into()),
            1 => {
                let (s, state) = &rows[0];
                match state.as_str() {
                    "device" => Ok(AdbDevice { serial: s.clone() }),
                    "unauthorized" => Err("the device is unauthorized: accept the USB debugging prompt in the headset and retry".into()),
                    other => Err(format!("the device is in state '{other}', need 'device'")),
                }
            }
            n => Err(format!("{n} adb devices attached, need exactly one (pass --serial)")),
        }
    }

    fn adb(&self, args: &[&str], secs: u64) -> Result<Output, String> {
        let mut all = vec!["-s", self.serial.as_str()];
        all.extend_from_slice(args);
        run_with_timeout("adb", &all, Duration::from_secs(secs))
    }
}

impl Device for AdbDevice {
    fn fingerprint(&self) -> String {
        crate::util::sha256_hex(self.serial.as_bytes())[..12].to_string()
    }
    fn shell(&self, cmd: &str, timeout_secs: u64) -> Result<Output, String> {
        self.adb(&["shell", cmd], timeout_secs)
    }
    fn install(&self, apk: &Path, replace: bool) -> Result<Output, String> {
        let p = apk.to_string_lossy();
        if replace {
            self.adb(&["install", "-r", "-g", &p], 600)
        } else {
            self.adb(&["install", "-g", &p], 600)
        }
    }
    fn uninstall(&self, package: &str) -> Result<Output, String> {
        self.adb(&["uninstall", package], 120)
    }
    fn push(&self, local: &Path, remote: &str) -> Result<Output, String> {
        self.adb(&["push", &local.to_string_lossy(), remote], 1800)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_command_that_overruns_is_killed_and_reported() {
        let e = run_with_timeout("sleep", &["5"], Duration::from_millis(200)).unwrap_err();
        assert!(e.contains("timed out"), "{e}");
    }

    #[test]
    fn output_and_exit_code_are_returned() {
        let o = run_with_timeout("sh", &["-c", "echo out; echo err >&2; exit 3"], Duration::from_secs(5)).unwrap();
        assert_eq!(o.code, 3);
        assert!(o.text.contains("out") && o.text.contains("err"));
    }
}
