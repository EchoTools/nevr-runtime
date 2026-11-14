//! Small checks shared by the manifest, artifact and plan code. Each is the Rust reference of
//! `docs/reference/verifying-a-release.md`, kept in one place so a rule changes once.

use sha2::{Digest, Sha256};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    hex(&Sha256::digest(bytes))
}

pub fn is_sha256_hex(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// Exactly YYYY-MM-DDTHH:MM:SSZ, and a real calendar time. Nothing else, so no trailing newline either.
pub fn utc_ok(s: &str) -> bool {
    let b = s.as_bytes();
    let digits = |r: std::ops::Range<usize>| b.len() == 20 && b[r].iter().all(u8::is_ascii_digit);
    if !(digits(0..4)
        && b[4] == b'-'
        && digits(5..7)
        && b[7] == b'-'
        && digits(8..10)
        && b[10] == b'T'
        && digits(11..13)
        && b[13] == b':'
        && digits(14..16)
        && b[16] == b':'
        && digits(17..19)
        && b[19] == b'Z')
    {
        return false;
    }
    let n = |r: std::ops::Range<usize>| s[r].parse::<u32>().unwrap_or(99);
    let (y, mo, d, h, mi, se) = (n(0..4), n(5..7), n(8..10), n(11..13), n(14..16), n(17..19));
    let leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    let days = match mo {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if leap => 29,
        2 => 28,
        _ => 0,
    };
    (1..=days).contains(&d) && h < 24 && mi < 60 && se < 60
}

/// The system clock as YYYY-MM-DDTHH:MM:SSZ. A clock before 1970 is an error, never a guess.
pub fn now_utc() -> Result<String, String> {
    let secs = SystemTime::now().duration_since(UNIX_EPOCH).map_err(|e| format!("the clock reads before 1970: {e}"))?.as_secs();
    Ok(utc_from_unix(secs))
}

/// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
pub fn utc_from_unix(secs: u64) -> String {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!("{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z", rem / 3600, (rem % 3600) / 60, rem % 60)
}

pub fn semver_ok(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    parts.len() == 3
        && parts.iter().all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()) && (*p == "0" || !p.starts_with('0')))
}

/// Two `X.Y.Z` versions compared as numbers (`2.10.0` is above `2.9.0`); `None` when either is not `X.Y.Z`, or has a
/// component that does not fit a u64 (it is not dropped: that would compare the rest as a shorter version).
pub fn semver_cmp(a: &str, b: &str) -> Option<std::cmp::Ordering> {
    let parts = |v: &str| -> Option<Vec<u64>> { semver_ok(v).then(|| v.split('.').map(|p| p.parse::<u64>().ok()).collect::<Option<Vec<u64>>>()).flatten() };
    Some(parts(a)?.cmp(&parts(b)?))
}

/// `v<version>/nevr-runtime-v<version>-<name>`, the name made of a-z 0-9 . - only: one version directory, one file.
pub fn path_ok(version: &str, path: &str) -> bool {
    semver_ok(version)
        && path
            .strip_prefix(&format!("v{version}/nevr-runtime-v{version}-"))
            .is_some_and(|name| !name.is_empty() && name.bytes().all(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'.' | b'-')))
}

/// `data/<name>.zip` with the name a-z 0-9 . - only: the shape release-signer gives a `game-data` artifact, which is
/// shared by every release and so has no version directory.
pub fn data_path_ok(path: &str) -> bool {
    path.strip_prefix("data/")
        .is_some_and(|name| name.ends_with(".zip") && name.len() > 4 && name.bytes().all(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'.' | b'-')))
}

/// `packs/<name>.zip` with the name a-z 0-9 . - only: the shape a `content-pack` artifact has (a CDN file, no version
/// directory). The name's own rule (`<id>-<sha12>.zip`) is checked by the manifest, which knows the id and the hash.
pub fn pack_path_ok(path: &str) -> bool {
    path.strip_prefix("packs/")
        .is_some_and(|name| name.ends_with(".zip") && name.len() > 4 && name.bytes().all(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'.' | b'-')))
}

/// A content pack id: `^[a-z][a-z0-9]{1,31}$`.
pub fn pack_id_ok(id: &str) -> bool {
    let b = id.as_bytes();
    (2..=32).contains(&b.len()) && b[0].is_ascii_lowercase() && b[1..].iter().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit())
}

/// A content pack version: `^[0-9]+\.[0-9]+\.[0-9]+$`. Unlike `semver_ok`, a leading zero is not refused here.
pub fn pack_version_ok(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    parts.len() == 3 && parts.iter().all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()))
}

/// A content pack's `stock_data_version`: `^[0-9]{1,20}$`.
pub fn stock_data_version_ok(v: &str) -> bool {
    (1..=20).contains(&v.len()) && v.bytes().all(|b| b.is_ascii_digit())
}

/// A content pack plugin name: `^[A-Za-z0-9_-]{1,64}$`.
pub fn plugin_name_ok(name: &str) -> bool {
    (1..=64).contains(&name.len()) && name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// A `content-pack` contents path: segments of `A-Za-z0-9._-` joined by `/`, with no empty, `.` or `..` segment. Stricter
/// than `rel_path_ok`, which allows any printable ASCII.
pub fn pack_member_path_ok(p: &str) -> bool {
    !p.is_empty()
        && p.split('/').all(|s| !s.is_empty() && s != "." && s != ".." && s.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-')))
}

/// A relative path made of segments of printable ASCII with no empty, `.` or `..` segment, no backslash and no drive
/// colon, so it cannot leave the directory it is joined to.
pub fn rel_path_ok(p: &str) -> bool {
    !p.is_empty()
        && !p.starts_with('/')
        && p.split('/').all(|s| !s.is_empty() && s != "." && s != "..")
        && p.bytes().all(|b| (0x21..0x7f).contains(&b) && b != b'\\' && b != b':')
}

/// A temporary file beside `path`, created exclusively under a name only this writer holds: the process id and a counter,
/// retried while the name is taken. Two runs writing the same path never share a temporary file (#1353).
pub fn create_unique_temp(path: &Path) -> std::io::Result<(PathBuf, fs::File)> {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let pid = std::process::id();
    let mut last = None;
    for _ in 0..1024 {
        let n = NEXT.fetch_add(1, Ordering::Relaxed);
        let tmp = temp_name(path, pid, n);
        match fs::OpenOptions::new().write(true).create_new(true).open(&tmp) {
            Ok(f) => return Ok((tmp, f)),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => last = Some(e),
            Err(e) => return Err(e),
        }
    }
    Err(last.unwrap_or_else(|| std::io::Error::other("no free temporary name")))
}

/// The temporary name for `path` that process `pid` tries as its attempt `n`: distinct across processes and attempts.
fn temp_name(path: &Path, pid: u32, n: u64) -> PathBuf {
    let mut name = path.as_os_str().to_owned();
    name.push(format!(".{pid}-{n}.new"));
    PathBuf::from(name)
}

/// A file replaced whole: the bytes go to a temporary file of this writer's own (`create_unique_temp`), synced, renamed over
/// `path`, and then the folder is synced, so a reader sees the old file or the new one and never half of it, and the rename
/// itself survives a power cut. A failed write removes its temporary file.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let (tmp, mut f) = create_unique_temp(path)?;
    let written = f.write_all(bytes).and_then(|()| f.sync_all());
    drop(f);
    let done = written.and_then(|()| fs::rename(&tmp, path)).and_then(|()| sync_parent_dir(path));
    if done.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    done
}

/// Like `write_atomic`, with the temporary at the literal name `<path><suffix>` (`.new` for a stage's member, `.tmp` for a
/// manifest or the ledger). The temporary is created exclusively (`create_new`, which on unix is `O_CREAT|O_EXCL` and so
/// never follows a link at that name): an existing file or link there is an error, never written through. The caller
/// lists or removes that name beforehand, so an existing one is a leftover, not another writer's. A failed write removes
/// the temporary by its name.
pub fn write_with_literal_tmp(path: &Path, suffix: &str, bytes: &[u8]) -> std::io::Result<()> {
    let mut tmp_name = path.as_os_str().to_owned();
    tmp_name.push(suffix);
    let tmp = PathBuf::from(tmp_name);
    let mut f = fs::OpenOptions::new().write(true).create_new(true).open(&tmp)?;
    let written = f.write_all(bytes).and_then(|()| f.sync_all());
    drop(f);
    let done = written.and_then(|()| fs::rename(&tmp, path)).and_then(|()| sync_parent_dir(path));
    if done.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    done
}

/// `write_with_literal_tmp` with the `.new` temporary of a stage's member.
pub fn write_literal_new(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    write_with_literal_tmp(path, ".new", bytes)
}

/// Syncs the folder that holds `path`, so a rename into it is on disk (unix: a folder can be opened and synced).
#[cfg(unix)]
fn sync_parent_dir(path: &Path) -> std::io::Result<()> {
    let dir = path.parent().filter(|d| !d.as_os_str().is_empty()).unwrap_or(Path::new("."));
    fs::File::open(dir)?.sync_all()
}

/// Windows: no folder sync is made. std opens a folder with no flush, and a folder cannot be opened for
/// `FlushFileBuffers` without a raw call this crate does not make. The file is synced before the rename; what a power cut
/// does to a rename on NTFS is [UNVERIFIED].
#[cfg(not(unix))]
fn sync_parent_dir(_path: &Path) -> std::io::Result<()> {
    Ok(())
}

/// True for a symbolic link, and on Windows for a junction or any other reparse point: nothing here moves through one.
pub fn is_link(m: &fs::Metadata) -> bool {
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

/// What `remove_owned` / `remove_entry` did with one entry. The words are those of the C++ guard (`src/core/owned_remove.h`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Removed,
    Absent,
    NotEmpty,
    RefusedName,
    RefusedLink,
    RefusedOutsideRoot,
    Failed,
}

impl Outcome {
    pub fn word(self) -> &'static str {
        match self {
            Outcome::Removed => "removed",
            Outcome::Absent => "absent",
            Outcome::NotEmpty => "not_empty",
            Outcome::RefusedName => "refused_name",
            Outcome::RefusedLink => "refused_link",
            Outcome::RefusedOutsideRoot => "refused_outside_root",
            Outcome::Failed => "failed",
        }
    }
}

/// The entry grammar of the C++ guard (`src/core/owned_remove_rules.h`, `ValidEntry`), ported exactly: non-empty, not
/// absolute, no byte below 0x20 and no DEL (an embedded NUL included), no '\\' and no ':' anywhere, and every
/// '/'-separated component non-empty and not "." or "..". A trailing '/' makes an empty last component, so it is refused.
pub fn valid_entry(entry: &str) -> bool {
    if entry.is_empty() || entry.starts_with('/') {
        return false;
    }
    if entry.bytes().any(|b| b < 0x20 || b == 0x7f || b == b'\\' || b == b':') {
        return false;
    }
    entry.split('/').all(|p| !p.is_empty() && p != "." && p != "..")
}

fn same_path(a: &Path, b: &Path) -> bool {
    #[cfg(windows)]
    {
        a.as_os_str().to_string_lossy().eq_ignore_ascii_case(&b.as_os_str().to_string_lossy())
    }
    #[cfg(not(windows))]
    {
        a == b
    }
}

/// Removes the one entry `entry` (its grammar checked by `valid_entry`) under `root`, given the canonical form of `root`
/// (`None` when it could not be taken). Walks each parent component with `symlink_metadata`: a link is refused; the
/// canonical parent must then be the canonical root followed by those components, else the entry is outside the root.
/// The last component is removed as itself: a link is unlinked and its target untouched, a folder only when empty.
///
/// Residual race: std has no `openat`, so a parent can be swapped for a link between the walk and the removal. The
/// window is the time between two system calls, and it is the one the C++ POSIX guard closes with `openat`; this
/// function is for the installer's own records, which a second party does not write.
fn remove_within(root: &Path, canon_root: Option<&Path>, entry: &str) -> Outcome {
    if !valid_entry(entry) {
        return Outcome::RefusedName;
    }
    let Some(canon_root) = canon_root else {
        // No root to take a real path of: an absent root holds no entry; anything else is a failure to look.
        return match fs::symlink_metadata(root) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Outcome::Absent,
            _ => Outcome::Failed,
        };
    };
    let mut parts: Vec<&str> = entry.split('/').collect();
    let last = parts.pop().unwrap_or_default();
    let mut parent = root.to_path_buf();
    for part in &parts {
        parent.push(part);
        match fs::symlink_metadata(&parent) {
            Ok(m) if is_link(&m) => return Outcome::RefusedLink,
            Ok(m) if m.is_dir() => {}
            // A file where a folder should be: the entry cannot be inside it.
            Ok(_) => return Outcome::Absent,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Outcome::Absent,
            Err(_) => return Outcome::Failed,
        }
    }
    let expected = parts.iter().fold(canon_root.to_path_buf(), |p, s| p.join(s));
    match fs::canonicalize(&parent) {
        Ok(actual) if same_path(&actual, &expected) => {}
        Ok(_) => return Outcome::RefusedOutsideRoot,
        Err(_) => return Outcome::Failed,
    }
    let target = parent.join(last);
    let meta = match fs::symlink_metadata(&target) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Outcome::Absent,
        Err(_) => return Outcome::Failed,
    };
    if is_link(&meta) {
        // A folder link is a folder to Windows and a file to unix: try the folder form first, so neither follows it.
        return match fs::remove_dir(&target).or_else(|_| fs::remove_file(&target)) {
            Ok(()) => Outcome::Removed,
            Err(_) => Outcome::Failed,
        };
    }
    if meta.is_dir() {
        return match fs::remove_dir(&target) {
            Ok(()) => Outcome::Removed,
            Err(e) if e.kind() == std::io::ErrorKind::DirectoryNotEmpty => Outcome::NotEmpty,
            Err(_) => Outcome::Failed,
        };
    }
    match fs::remove_file(&target) {
        Ok(()) => Outcome::Removed,
        Err(_) => Outcome::Failed,
    }
}

/// Removes each of `entries` under `root`, in the order given, one at a time, by name: the caller's record of what it made.
/// Never recurses; never lists `root`. Every entry is tried and gets one result. A folder is removed only when empty.
pub fn remove_owned(root: &Path, entries: &[String]) -> Vec<(String, Outcome)> {
    let canon = fs::canonicalize(root).ok();
    entries.iter().map(|e| (e.clone(), remove_within(root, canon.as_deref(), e))).collect()
}

/// Removes the one entry `entry` under `root` by the rules of `remove_owned`.
pub fn remove_entry(root: &Path, entry: &str) -> Outcome {
    remove_within(root, fs::canonicalize(root).ok().as_deref(), entry)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utc_accepts_only_the_exact_form() {
        assert!(utc_ok("2026-10-26T12:00:00Z"));
        for bad in ["", "yesterday", "2026-10-26T12:00:00", "2026-10-26T12:00:00Z\n", "2026-02-30T12:00:00Z", "2026-13-01T00:00:00Z", "2026-10-26T24:00:00Z"] {
            assert!(!utc_ok(bad), "{bad:?}");
        }
        assert!(utc_ok("2028-02-29T00:00:00Z"));
        assert!(!utc_ok("2026-02-29T00:00:00Z"));
    }

    #[test]
    fn unix_to_utc() {
        assert_eq!(utc_from_unix(0), "1970-01-01T00:00:00Z");
        assert_eq!(utc_from_unix(1_782_648_000), "2026-06-28T12:00:00Z");
        assert!(utc_ok(&utc_from_unix(1_793_000_000)));
    }

    #[test]
    fn artifact_paths() {
        assert!(path_ok("5.5.1", "v5.5.1/nevr-runtime-v5.5.1-windows.zip"));
        for bad in ["v5.5.1/nevr-runtime-v5.5.1-", "v5.5.2/nevr-runtime-v5.5.1-x", "v5.5.1/../nevr-runtime-v5.5.1-x", "v5.5.1/nevr-runtime-v5.5.1-A", "v5.5.1/nevr-runtime-v5.5.1-x\n", "v5.5.1/nevr-runtime-v5.5.1-a/b"] {
            assert!(!path_ok("5.5.1", bad), "{bad:?}");
        }
        assert!(!path_ok("05.5.1", "v05.5.1/nevr-runtime-v05.5.1-x"));
        assert!(data_path_ok("data/echovr-data-fc2eedeacc50.zip"));
        assert!(!data_path_ok("data/../x.zip"));
        assert!(!data_path_ok("data/x.zip/y"));
        assert!(!data_path_ok("data/.zip"));
    }

    #[test]
    fn content_pack_shapes() {
        assert!(pack_path_ok("packs/combat-9f86d081884c.zip"));
        for bad in ["combat-9f86d081884c.zip", "data/combat-9f86d081884c.zip", "packs/", "packs/.zip", "packs/x.zip/y", "packs/../x.zip", "packs/a/b.zip", "packs/A.zip", "packs/x.zip\n", "packs/x.tar"] {
            assert!(!pack_path_ok(bad), "{bad:?}");
        }
        assert!(pack_id_ok("ab") && pack_id_ok("combat2") && pack_id_ok(&format!("a{}", "9".repeat(31))));
        for bad in ["", "a", "1ab", "Combat", "co-mbat", "co_mbat", &format!("a{}", "9".repeat(32))] {
            assert!(!pack_id_ok(bad), "{bad:?}");
        }
        assert!(pack_version_ok("1.0.0") && pack_version_ok("05.10.200"));
        for bad in ["1.0", "1.0.0.0", "1..0", "1.0.x", "v1.0.0", "1.0.0\n", "", "-1.0.0"] {
            assert!(!pack_version_ok(bad), "{bad:?}");
        }
        assert!(stock_data_version_ok("5932408047") && stock_data_version_ok(&"9".repeat(20)));
        assert!(!stock_data_version_ok("") && !stock_data_version_ok(&"9".repeat(21)) && !stock_data_version_ok("12a"));
        assert!(plugin_name_ok("combat-mod") && plugin_name_ok("A_b-9") && plugin_name_ok(&"x".repeat(64)));
        for bad in ["", "a b", "a.b", "a/b", &"x".repeat(65)] {
            assert!(!plugin_name_ok(bad), "{bad:?}");
        }
        assert!(pack_member_path_ok("_data/5932408047/rad15/android/manifests/2b47aab238f60515"));
        assert!(pack_member_path_ok("a...b/.hidden"));
        for bad in ["", "/a", "a/", "a//b", "./a", "a/./b", "a/../b", "../a", "a\\b", "a b", "a:b", "a/\u{e9}"] {
            assert!(!pack_member_path_ok(bad), "{bad:?}");
        }
    }

    #[test]
    fn relative_paths_cannot_escape() {
        assert!(rel_path_ok("_data/5932408047/rad15/android/manifests/2b47aab238f60515"));
        for bad in ["", "/etc/passwd", "a/../b", "a//b", "./a", "a\\b", "C:/x", "a b", "a/"] {
            assert!(!rel_path_ok(bad), "{bad:?}");
        }
    }

    #[test]
    fn semver_cmp_refuses_an_overflowing_component() {
        // A component that does not fit a u64 is unreadable: it must not be dropped, which compares the rest as a
        // shorter version (`[0, 0]` sorts below `[5, 17, 0]`).
        assert_eq!(semver_cmp("99999999999999999999.0.0", "5.17.0"), None);
        assert_eq!(semver_cmp("18446744073709551616.0.0", "1.0.0"), None);
        assert_eq!(semver_cmp("1.0.99999999999999999999", "1.0.0"), None);
    }

    #[test]
    fn two_writers_never_get_the_same_temporary_name() {
        let p = Path::new("/x/state.json");
        assert_ne!(temp_name(p, 1, 0), temp_name(p, 2, 0), "two processes share a name");
        assert_ne!(temp_name(p, 1, 0), temp_name(p, 1, 1), "two attempts share a name");
        let d = tempfile::tempdir().unwrap();
        let target = d.path().join("state.json");
        let (a, _fa) = create_unique_temp(&target).unwrap();
        let (b, _fb) = create_unique_temp(&target).unwrap();
        assert_ne!(a, b);
        assert_ne!(a, d.path().join("state.json.new"));
        assert!(a.starts_with(d.path()) && a.file_name().unwrap().to_string_lossy().ends_with(".new"));
    }

    #[test]
    fn a_taken_temporary_name_is_skipped_never_opened() {
        let d = tempfile::tempdir().unwrap();
        let target = d.path().join("state.json");
        let pid = std::process::id();
        // Names this process hands out next, all taken by another writer's file (the counter is process-wide, so the span
        // is wide enough for every test that runs before this one).
        for n in 0..512 {
            std::fs::write(d.path().join(format!("state.json.{pid}-{n}.new")), b"someone else's").unwrap();
        }
        let (tmp, _f) = create_unique_temp(&target).unwrap();
        assert_eq!(std::fs::metadata(&tmp).unwrap().len(), 0, "the writer opened a file another writer holds: {}", tmp.display());
    }

    #[test]
    fn the_entry_grammar_is_the_guards() {
        for bad in ["..", "/abs", "a\\b", "C:x", "", "a//b", "a/./b", "a/", "..\0/x", "a/..", "x\u{1}y", "x\u{7f}", "a:b"] {
            assert!(!valid_entry(bad), "{bad:?} must be refused");
        }
        for ok in ["a", "plugins/x.dll", "a b", "a/b.c", "a/.hidden", "\u{e9}"] {
            assert!(valid_entry(ok), "{ok:?} must be accepted");
        }
    }

    #[test]
    fn refused_outside_root_is_reached_when_the_real_parent_is_not_where_the_root_says() {
        // On Linux the walk alone (no link on the path) cannot make the canonical parent differ from the root joined with
        // its components, except by a race that a test cannot make. So this drives `remove_within` with a canonical root
        // that names another folder: the real parent `a` is then not `<other>/a`, and nothing may be removed.
        let root = tempfile::tempdir().unwrap();
        let other = tempfile::tempdir().unwrap();
        fs::create_dir_all(root.path().join("a")).unwrap();
        fs::write(root.path().join("a").join("b"), b"kept").unwrap();
        let canon_other = fs::canonicalize(other.path()).unwrap();
        assert_eq!(remove_within(root.path(), Some(&canon_other), "a/b"), Outcome::RefusedOutsideRoot);
        assert_eq!(fs::read(root.path().join("a").join("b")).unwrap(), b"kept", "nothing is removed");
        // The same entry with the true root removes it.
        let canon_root = fs::canonicalize(root.path()).unwrap();
        assert_eq!(remove_within(root.path(), Some(&canon_root), "a/b"), Outcome::Removed);
    }

    #[test]
    fn an_absent_root_holds_nothing_to_remove() {
        let parent = tempfile::tempdir().unwrap();
        let missing = parent.path().join("never-made");
        assert_eq!(remove_entry(&missing, "x"), Outcome::Absent);
        assert_eq!(remove_owned(&missing, &["x".to_string()])[0].1, Outcome::Absent);
        assert_eq!(remove_entry(&missing, "..").word(), "refused_name", "the name is checked first");
    }

    #[test]
    fn a_bad_name_is_refused_and_nothing_under_the_root_is_touched() {
        let root = tempfile::tempdir().unwrap();
        let canary = tempfile::tempdir().unwrap();
        fs::write(canary.path().join("keep"), b"canary").unwrap();
        for bad in ["..", "/abs", "a\\b", "C:x", "", "a//b", "a/./b", "a/", "..\0/x"] {
            assert_eq!(remove_entry(root.path(), bad), Outcome::RefusedName, "{bad:?}");
        }
        assert_eq!(fs::read(canary.path().join("keep")).unwrap(), b"canary");
    }

    #[test]
    fn the_normal_path_removes_each_named_entry_and_no_more() {
        let root = tempfile::tempdir().unwrap();
        let canary = tempfile::tempdir().unwrap();
        fs::write(canary.path().join("keep"), b"canary").unwrap();
        fs::create_dir_all(root.path().join("d").join("sub")).unwrap();
        fs::write(root.path().join("d").join("f.dll"), b"f").unwrap();
        fs::create_dir_all(root.path().join("empty")).unwrap();
        fs::write(root.path().join("stranger"), b"s").unwrap();
        let entries: Vec<String> = ["d/f.dll", "d/sub", "d", "empty", "missing"].iter().map(|s| s.to_string()).collect();
        let got = remove_owned(root.path(), &entries);
        assert_eq!(got.iter().map(|(_, o)| *o).collect::<Vec<_>>(), [Outcome::Removed, Outcome::Removed, Outcome::Removed, Outcome::Removed, Outcome::Absent]);
        assert!(!root.path().join("d").exists() && !root.path().join("empty").exists());
        assert_eq!(fs::read(root.path().join("stranger")).unwrap(), b"s", "a name not in the list is kept");
        assert_eq!(remove_entry(root.path(), "stranger"), Outcome::Removed);
        assert_eq!(fs::read(canary.path().join("keep")).unwrap(), b"canary");
    }

    #[test]
    fn a_folder_with_an_unlisted_file_is_not_empty_and_a_link_is_unlinked_not_followed() {
        let root = tempfile::tempdir().unwrap();
        let canary = tempfile::tempdir().unwrap();
        fs::write(canary.path().join("keep"), b"canary").unwrap();
        fs::create_dir_all(root.path().join("d")).unwrap();
        fs::write(root.path().join("d").join("unlisted"), b"u").unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(canary.path(), root.path().join("l")).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_dir(canary.path(), root.path().join("l")).unwrap();
        assert_eq!(remove_entry(root.path(), "d"), Outcome::NotEmpty);
        assert!(root.path().join("d").join("unlisted").is_file());
        assert_eq!(remove_entry(root.path(), "l"), Outcome::Removed);
        assert!(fs::symlink_metadata(root.path().join("l")).is_err(), "the link itself is gone");
        assert_eq!(fs::read(canary.path().join("keep")).unwrap(), b"canary", "the link's target is intact");
    }
}
