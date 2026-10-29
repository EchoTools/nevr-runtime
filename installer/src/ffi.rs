//! The C ABI (`include/nevr_install.h`). Opaque session handle, UTF-8 strings, no callbacks, every function returns a
//! `NevrStatus`; the text of the last failure is `NevrLastError`. A Rust panic never crosses the boundary.

use crate::error::Error;
use crate::manifest::{self, Pins};
use crate::quest::{self, DataRoots, Installed, Observed};
use crate::session::{Config, Session};
use serde_json::{json, Value};
use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;

pub const NEVR_OK: i32 = 0;
pub const NEVR_FAILED: i32 = 1;
pub const NEVR_NEEDS_USER: i32 = 2;
pub const NEVR_BAD_ARGUMENT: i32 = 10;
pub const NEVR_PANIC: i32 = 11;

pub struct NevrSession {
    inner: Session,
    last_error: CString,
}

fn cstr<'a>(p: *const c_char) -> Result<&'a str, String> {
    if p.is_null() {
        return Err("a required string argument is null".into());
    }
    // SAFETY: the caller promises a NUL-terminated string that outlives the call (header contract).
    unsafe { CStr::from_ptr(p) }.to_str().map_err(|_| "a string argument is not UTF-8".to_string())
}

fn opt_cstr<'a>(p: *const c_char) -> Result<Option<&'a str>, String> {
    if p.is_null() {
        Ok(None)
    } else {
        cstr(p).map(Some)
    }
}

fn set_err(s: &mut NevrSession, msg: String) {
    s.last_error = CString::new(msg.replace('\0', " ")).expect("no NUL left");
}

fn out_string(v: &Value, out: *mut *mut c_char) -> Result<(), String> {
    if out.is_null() {
        return Err("the output pointer is null".into());
    }
    let s = CString::new(v.to_string()).map_err(|_| "the result holds a NUL".to_string())?;
    // SAFETY: `out` is non-null and writable (header contract); the string is freed with NevrStringFree.
    unsafe { *out = s.into_raw() };
    Ok(())
}

fn config_from_json(text: &str) -> Result<Config, String> {
    let v: Value = serde_json::from_str(text).map_err(|e| format!("config is not JSON: {e}"))?;
    let get = |k: &str| v[k].as_str().map(str::to_string);
    let platform = get("platform").ok_or("config has no platform (\"pc\" or \"quest\")")?;
    if platform != "pc" && platform != "quest" {
        return Err(format!("platform {platform} is not pc or quest"));
    }
    // "nevr_root" is the self-update's form (ADR 0024): state, log and downloads live under the nevr root, and "root" is not needed.
    let nevr_root = get("nevr_root").map(PathBuf::from);
    let root = match (&nevr_root, get("root")) {
        (_, Some(r)) => PathBuf::from(r),
        (Some(n), None) => n.clone(),
        (None, None) => return Err("config has no root (the installer's own directory)".into()),
    };
    let mut c = Config::new(&platform, &root);
    if let Some(n) = &nevr_root {
        (c.state_dir, c.log_path, c.work_dir) = crate::stage::paths(n);
    }
    if let Some(b) = get("base") {
        c.base = b;
    }
    if let Some(ch) = get("channel") {
        c.channel = ch;
    }
    if let Some(n) = get("now") {
        c.now = Some(n);
    }
    if let Some(p) = get("pins") {
        c.pins = Pins::parse(&p).map_err(|e| e.to_string())?;
    }
    if let Some(n) = v["min_manifest_version"].as_u64() {
        c.min_manifest_version = n.max(manifest::MIN_MANIFEST_VERSION);
    }
    if let Some(p) = get("log") {
        c.log_path = PathBuf::from(p);
    }
    if let Some(p) = get("work") {
        c.work_dir = PathBuf::from(p);
    }
    Ok(c)
}

fn status_of(e: &Error) -> i32 {
    let _ = e;
    NEVR_FAILED
}

/// Runs `f` on the session; a panic or an error becomes a status and the session's last error.
fn with_session(s: *mut NevrSession, f: impl FnOnce(&mut Session) -> Result<i32, Error>) -> i32 {
    if s.is_null() {
        return NEVR_BAD_ARGUMENT;
    }
    // SAFETY: a non-null session pointer came from NevrInstallOpen and is not used from two threads at once (header).
    let sess = unsafe { &mut *s };
    match catch_unwind(AssertUnwindSafe(|| f(&mut sess.inner))) {
        Ok(Ok(code)) => code,
        Ok(Err(e)) => {
            let _ = sess.inner.log.error("ffi_error", e.step, json!({"reason": e.to_string()}));
            set_err(sess, e.to_string());
            status_of(&e)
        }
        Err(_) => {
            set_err(sess, "internal error (panic)".into());
            NEVR_PANIC
        }
    }
}

/// # Safety
/// `config_json` is a NUL-terminated UTF-8 string and `out` points at writable storage for one pointer.
#[no_mangle]
pub unsafe extern "C" fn NevrInstallOpen(config_json: *const c_char, out: *mut *mut NevrSession) -> i32 {
    if out.is_null() {
        return NEVR_BAD_ARGUMENT;
    }
    let r = catch_unwind(AssertUnwindSafe(|| -> Result<NevrSession, String> {
        let cfg = config_from_json(cstr(config_json)?)?;
        let s = Session::open(cfg, "library").map_err(|e| e.to_string())?;
        Ok(NevrSession { inner: s, last_error: CString::default() })
    }));
    match r {
        Ok(Ok(s)) => {
            *out = Box::into_raw(Box::new(s));
            NEVR_OK
        }
        Ok(Err(_)) | Err(_) => {
            *out = std::ptr::null_mut();
            NEVR_FAILED
        }
    }
}

/// Text of the last failure of `s`, valid until the next call on `s`. Never null for a non-null session.
///
/// # Safety
/// `s` is a session from `NevrInstallOpen`.
#[no_mangle]
pub unsafe extern "C" fn NevrLastError(s: *const NevrSession) -> *const c_char {
    if s.is_null() {
        return c"".as_ptr();
    }
    (*s).last_error.as_ptr()
}

/// Steps 1 to 6 over the base URL.
///
/// # Safety
/// `s` is a session from `NevrInstallOpen`.
#[no_mangle]
pub unsafe extern "C" fn NevrManifestFetch(s: *mut NevrSession) -> i32 {
    with_session(s, |s| s.manifest().map(|_| NEVR_OK))
}

/// Steps 2 to 6 on bytes the caller already holds.
///
/// # Safety
/// `manifest` and `sig` point at `manifest_len` and `sig_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn NevrManifestVerify(s: *mut NevrSession, manifest: *const u8, manifest_len: usize, sig: *const u8, sig_len: usize) -> i32 {
    if manifest.is_null() || sig.is_null() {
        return NEVR_BAD_ARGUMENT;
    }
    let (m, g) = (std::slice::from_raw_parts(manifest, manifest_len).to_vec(), std::slice::from_raw_parts(sig, sig_len).to_vec());
    with_session(s, |s| {
        // Held from the read of the state to the commit (#1353), as the installer's own runs hold it.
        let _lock = manifest::lock_state(&s.cfg.state_dir)?;
        let state = manifest::read_state(&s.cfg.state_dir)?;
        let now = match &s.cfg.now {
            Some(n) => n.clone(),
            None => crate::util::now_utc().map_err(|e| Error::new(5, e))?,
        };
        let v = manifest::verify(&m, &g, &s.cfg.pins, &state, &now, s.cfg.min_manifest_version)?;
        manifest::commit(&s.cfg.state_dir, &v)?;
        s.log.info("manifest_check", 6, json!({"ok": true, "key_id": v.key_id, "manifest_version": v.version}))?;
        Ok(NEVR_OK)
    })
}

/// Step 7: the release a channel names, as JSON `{"version": "...", "artifacts": [...]}` (free with `NevrStringFree`).
///
/// # Safety
/// `channel` is null or a NUL-terminated string; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn NevrReleasePick(s: *mut NevrSession, channel: *const c_char, out: *mut *mut c_char) -> i32 {
    let channel = match opt_cstr(channel) {
        Ok(c) => c.map(str::to_string),
        Err(_) => return NEVR_BAD_ARGUMENT,
    };
    with_session(s, |s| {
        if let Some(c) = channel {
            s.cfg.channel = c;
        }
        let (m, v) = s.release(None)?;
        let arts: Vec<Value> = m["releases"][v.as_str()]["artifacts"].as_array().cloned().unwrap_or_default().iter().map(|x| json!({"kind": x["kind"], "platform": x["platform"], "path": x["path"], "size": x["size"], "sha256": x["sha256"]})).collect();
        out_string(&json!({"version": v, "artifacts": arts}), out).map_err(|e| Error::new(0, e))?;
        Ok(NEVR_OK)
    })
}

/// Step 8 for one artifact: fetch to `out_path`, check size and sha256; the file is deleted on failure.
///
/// # Safety
/// All string arguments are NUL-terminated UTF-8; `platform` may be null.
#[no_mangle]
pub unsafe extern "C" fn NevrArtifactFetch(s: *mut NevrSession, version: *const c_char, kind: *const c_char, platform: *const c_char, out_path: *const c_char) -> i32 {
    let (version, kind, platform, out_path) = match (cstr(version), cstr(kind), opt_cstr(platform), cstr(out_path)) {
        (Ok(a), Ok(b), Ok(c), Ok(d)) => (a.to_string(), b.to_string(), c.map(str::to_string), d.to_string()),
        _ => return NEVR_BAD_ARGUMENT,
    };
    with_session(s, |s| {
        let m = manifest::stored(&s.cfg.state_dir)?;
        let art = manifest::find(&m, &version, &kind, platform.as_deref())?;
        let url = format!("{}/{}", s.cfg.base, art.path);
        let f = crate::artifact::download(&url, &art, std::path::Path::new(&out_path))?;
        s.log.info("artifact_fetch", 8, json!({"kind": kind, "url": crate::util::redact_url(&url), "size": art.size, "bytes": f.bytes, "sha256": f.sha256, "ok": true, "resumed_from": f.resumed_from}))?;
        Ok(NEVR_OK)
    })
}

/// Every member of a zip artifact against `contents` (`allow_dirs` is nonzero for game data).
///
/// # Safety
/// All string arguments are NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn NevrArtifactVerifyZip(s: *mut NevrSession, version: *const c_char, kind: *const c_char, zip_path: *const c_char, allow_dirs: i32) -> i32 {
    let (version, kind, zip_path) = match (cstr(version), cstr(kind), cstr(zip_path)) {
        (Ok(a), Ok(b), Ok(c)) => (a.to_string(), b.to_string(), c.to_string()),
        _ => return NEVR_BAD_ARGUMENT,
    };
    with_session(s, |s| {
        let m = manifest::stored(&s.cfg.state_dir)?;
        let art = manifest::find(&m, &version, &kind, None)?;
        let n = crate::artifact::verify_zip(std::path::Path::new(&zip_path), &art, allow_dirs != 0)?;
        s.log.info("verify_installed", 8, json!({"ok": true, "what": "zip-members", "expected": art.contents.len(), "observed": n}))?;
        Ok(NEVR_OK)
    })
}

/// The Quest plan as JSON from what the caller observed on the headset:
/// `{"installed": {"version_code": N, "signers": ["<sha256>", ...] | null} | null, "data": {"legacy": bool, "media": bool} | null,
///   "consent_uninstall": bool}`.
///
/// # Safety
/// String arguments are NUL-terminated UTF-8 and `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn NevrPlanBuild(s: *mut NevrSession, version: *const c_char, observed_json: *const c_char, out: *mut *mut c_char) -> i32 {
    let (version, observed) = match (cstr(version), cstr(observed_json)) {
        (Ok(a), Ok(b)) => (a.to_string(), b.to_string()),
        _ => return NEVR_BAD_ARGUMENT,
    };
    with_session(s, |s| {
        let o: Value = serde_json::from_str(&observed).map_err(|e| Error::new(9, format!("observed_json is not JSON: {e}")))?;
        let installed = match &o["installed"] {
            Value::Null => None,
            i => Some(Installed {
                version_code: i["version_code"].as_u64().ok_or_else(|| Error::new(9, "installed.version_code is not a whole number"))?,
                signers: match &i["signers"] {
                    Value::Null => None,
                    l => Some(l.as_array().ok_or_else(|| Error::new(9, "installed.signers is not a list"))?.iter().map(|x| x.as_str().map(str::to_string)).collect::<Option<Vec<_>>>().ok_or_else(|| Error::new(9, "installed.signers holds a non-string"))?),
                },
            }),
        };
        let data = match &o["data"] {
            Value::Null => None,
            d => Some(DataRoots { legacy: d["legacy"].as_bool().unwrap_or(false), media: d["media"].as_bool().unwrap_or(false) }),
        };
        // Optional, so a caller written before the check keeps working: both keys absent means not observed.
        let free = match (o.get("free_kb_sdcard"), o.get("free_kb_tmp")) {
            (None, None) => None,
            (a, b) => Some(quest::FreeSpace { sdcard_kb: a.and_then(Value::as_u64), tmp_kb: b.and_then(Value::as_u64) }),
        };
        let m = manifest::stored(&s.cfg.state_dir)?;
        let plan = quest::plan(&m, &version, &Observed { installed, data, free }, o["consent_uninstall"].as_bool().unwrap_or(false));
        s.log.info("plan", 9, json!({"steps": plan.steps.iter().map(|x| x.name()).collect::<Vec<_>>(), "decision": plan.decision, "reasons": plan.reasons}))?;
        out_string(&plan.to_json(), out).map_err(|e| Error::new(0, e))?;
        Ok(NEVR_OK)
    })
}

/// Self-update step (ADR 0024): stages the channel's release into `<root_dir>/update/staging/<version>` unless it is the running
/// one. `current_json` is `{"version": "...", "runtime_sha256": "...", "quarantine": [{"version", "runtime_sha256"}]}`; the result
/// is `{"action": "none"|"staged"|"blocked", "version", "direction", "reason", "files": [{path, size, sha256}]}`. Needs a verified
/// manifest (`NevrManifestFetch`); the caller ends the run with `NevrRunEnd`.
///
/// # Safety
/// String arguments are NUL-terminated UTF-8 and `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn NevrPcStage(s: *mut NevrSession, root_dir: *const c_char, current_json: *const c_char, out: *mut *mut c_char) -> i32 {
    let (root_dir, current) = match (cstr(root_dir), opt_cstr(current_json)) {
        (Ok(a), Ok(b)) => (a.to_string(), b.map(str::to_string)),
        _ => return NEVR_BAD_ARGUMENT,
    };
    with_session(s, |s| {
        let cur = match current {
            Some(t) => crate::stage::Current::parse(&t)?,
            None => crate::stage::Current::default(),
        };
        let (_reason, report) = s.pc_stage_body(None, std::path::Path::new(&root_dir), &cur)?;
        out_string(&report, out).map_err(|e| Error::new(0, e))?;
        Ok(NEVR_OK)
    })
}

/// Ends the run: writes `run_end`. `state` is one of installed, unchanged, restored, needs-user.
///
/// # Safety
/// String arguments are NUL-terminated UTF-8; `reason` may be null.
#[no_mangle]
pub unsafe extern "C" fn NevrRunEnd(s: *mut NevrSession, state: *const c_char, reason: *const c_char) -> i32 {
    let (state, reason) = match (cstr(state), opt_cstr(reason)) {
        (Ok(a), Ok(b)) => (a.to_string(), b.unwrap_or("").to_string()),
        _ => return NEVR_BAD_ARGUMENT,
    };
    let state: &'static str = match state.as_str() {
        "installed" => "installed",
        "unchanged" => "unchanged",
        "restored" => "restored",
        "needs-user" => "needs-user",
        _ => return NEVR_BAD_ARGUMENT,
    };
    with_session(s, |s| s.end(state, &reason).map(|_| NEVR_OK))
}

/// # Safety
/// `p` came from this library or is null.
#[no_mangle]
pub unsafe extern "C" fn NevrStringFree(p: *mut c_char) {
    if !p.is_null() {
        drop(CString::from_raw(p));
    }
}

/// # Safety
/// `s` came from `NevrInstallOpen` and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn NevrClose(s: *mut NevrSession) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

/// The library's version as a static string.
#[no_mangle]
pub extern "C" fn NevrCoreVersion() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr().cast()
}
