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

/// A file replaced whole: the bytes go to a temporary file of this writer's own (`create_unique_temp`), renamed over
/// `path`, so a reader sees the old file or the new one and never half of it. A failed write removes its temporary file.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let (tmp, mut f) = create_unique_temp(path)?;
    let written = f.write_all(bytes).and_then(|()| f.sync_all());
    drop(f);
    let done = written.and_then(|()| fs::rename(&tmp, path));
    if done.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    done
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
}
