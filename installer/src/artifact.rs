//! Step 8 for one artifact: download capped at `size`, exact size and sha256 from the file on disk, every zip member
//! against `contents`, and extraction of only the listed paths. A file that fails a check is deleted; a download that was
//! only interrupted keeps its `.part` file so the next run resumes it.

use crate::error::{Error, Result};
use crate::fetch::{fetch_capped, sha256_file, Failure};
use crate::manifest::Artifact;
use crate::util::hex;
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Fetched {
    pub bytes: u64,
    pub sha256: String,
    pub resumed_from: u64,
}

fn part_path(out: &Path) -> PathBuf {
    let mut p = out.as_os_str().to_owned();
    p.push(".part");
    PathBuf::from(p)
}

/// Downloads `url` to `out`, checking the artifact's size and sha256. On success `out` exists and is complete.
pub fn download(url: &str, art: &Artifact, out: &Path) -> Result<Fetched> {
    let part = part_path(out);
    let _ = fs::remove_file(out);
    let mut start = fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
    if start > art.size {
        let _ = fs::remove_file(&part);
        start = 0;
    }
    let mut resumed_from = start;
    loop {
        let mut file = if start == 0 {
            File::create(&part)
        } else {
            OpenOptions::new().append(true).open(&part)
        }
        .map_err(|e| Error::new(8, format!("{}: {e}", part.display())))?;
        match fetch_capped(url, start, art.size, &mut file) {
            Ok(_) => break,
            Err(Failure::Refused(_)) if start > 0 => {
                // The server did not honour the range: start again from zero, once.
                drop(file);
                let _ = fs::remove_file(&part);
                start = 0;
                resumed_from = 0;
            }
            Err(Failure::Refused(m)) => {
                drop(file);
                let _ = fs::remove_file(&part);
                return Err(Error::new(8, m));
            }
            Err(Failure::Interrupted(m)) => return Err(Error::new(8, format!("{m} (the partial download is kept for a resume)"))),
        }
    }
    let (size, sha) = sha256_file(&part).map_err(|e| Error::new(8, format!("{}: {e}", part.display())))?;
    if size != art.size {
        let _ = fs::remove_file(&part);
        return Err(Error::new(8, format!("size is {size}, the manifest says {}", art.size)));
    }
    if sha != art.sha256 {
        let _ = fs::remove_file(&part);
        return Err(Error::new(8, "sha256 differs from the manifest"));
    }
    fs::rename(&part, out).map_err(|e| Error::new(8, format!("{}: {e}", out.display())))?;
    Ok(Fetched { bytes: size, sha256: sha, resumed_from })
}

/// Every member of the zip against `contents`: nothing unlisted, nothing listed missing, nothing twice, only regular
/// files (directory entries only when `allow_dirs`), exact size and sha256. Extract nothing before this passes.
pub fn verify_zip(zip_path: &Path, art: &Artifact, allow_dirs: bool) -> Result<usize> {
    let fail = |m: String| Error::new(8, m);
    let file = File::open(zip_path).map_err(|e| fail(format!("{}: {e}", zip_path.display())))?;
    let mut z = zip::ZipArchive::new(file).map_err(|e| fail(format!("{} is not a zip: {e}", zip_path.display())))?;
    let mut seen: Vec<String> = Vec::new();
    for i in 0..z.len() {
        let mut entry = z.by_index(i).map_err(|e| fail(format!("zip entry {i}: {e}")))?;
        let name = entry.name().map(|n| n.to_string()).map_err(|e| fail(format!("zip entry {i}: {e}")))?;
        if seen.contains(&name) {
            return Err(fail("an entry appears twice in the zip".into()));
        }
        seen.push(name.clone());
        if entry.is_dir() {
            if allow_dirs {
                continue;
            }
            return Err(fail(format!("{name} is not a regular file")));
        }
        let mode = entry.unix_mode().map(|m| m & 0o170000).unwrap_or(0);
        if mode != 0 && mode != 0o100000 {
            return Err(fail(format!("{name} is not a regular file")));
        }
        let want = art.contents.iter().find(|c| c.path == name).ok_or_else(|| fail(format!("the zip holds {name}, which the manifest does not list")))?;
        let mut hasher = Sha256::new();
        let (mut n, mut buf) = (0u64, vec![0u8; 65536]);
        loop {
            let k = entry.by_ref().take(want.size + 1 - n.min(want.size + 1)).read(&mut buf).map_err(|e| fail(format!("{name}: {e}")))?;
            if k == 0 {
                break;
            }
            hasher.update(&buf[..k]);
            n += k as u64;
            if n > want.size {
                return Err(fail(format!("{name} has a different size than the manifest says")));
            }
        }
        if n != want.size {
            return Err(fail(format!("{name} has a different size than the manifest says")));
        }
        if hex(&hasher.finalize()) != want.sha256 {
            return Err(fail(format!("{name} has a different sha256 than the manifest says")));
        }
    }
    let mut absent: Vec<&str> = art.contents.iter().map(|c| c.path.as_str()).filter(|p| !seen.iter().any(|s| s == p)).collect();
    absent.sort_unstable();
    if !absent.is_empty() {
        return Err(fail(format!("the manifest lists {}, which the zip does not hold", absent.join(", "))));
    }
    Ok(seen.len())
}

/// Writes the listed files under `root`, each by a temporary file renamed into place, hashing as it writes. Call only
/// after `verify_zip` passed. Returns the paths written.
pub fn extract(zip_path: &Path, art: &Artifact, root: &Path) -> Result<Vec<PathBuf>> {
    let fail = |m: String| Error::new(9, m);
    let file = File::open(zip_path).map_err(|e| fail(format!("{}: {e}", zip_path.display())))?;
    let mut z = zip::ZipArchive::new(file).map_err(|e| fail(format!("{}: {e}", zip_path.display())))?;
    let mut written = Vec::new();
    for c in &art.contents {
        let mut entry = z.by_name(&c.path).map_err(|e| fail(format!("{}: {e}", c.path)))?;
        let dest = root.join(&c.path);
        if let Some(dir) = dest.parent() {
            fs::create_dir_all(dir).map_err(|e| fail(format!("{}: {e}", dir.display())))?;
        }
        // A temporary file of this writer's own, never a fixed name another run may also use (#1353). A failure removes it.
        let (tmp, mut out) = crate::util::create_unique_temp(&dest).map_err(|e| fail(format!("{}: {e}", dest.display())))?;
        let streamed = stream_entry(&mut entry, c, &mut out, &tmp);
        drop(out);
        let (n, digest) = match streamed {
            Ok(done) => done,
            Err(e) => {
                let _ = fs::remove_file(&tmp);
                return Err(e);
            }
        };
        if n != c.size || digest != c.sha256 {
            let _ = fs::remove_file(&tmp);
            return Err(fail(format!("{} changed between the check and the extraction", c.path)));
        }
        if let Err(e) = fs::rename(&tmp, &dest) {
            let _ = fs::remove_file(&tmp);
            return Err(fail(format!("{}: {e}", dest.display())));
        }
        written.push(dest);
    }
    Ok(written)
}

/// Copies one zip entry into `out` (the temporary file `tmp` names, for the messages) and returns the bytes copied and
/// their sha256 in hex. At most `c.size + 1` bytes are read, so a longer entry is seen as a mismatch.
fn stream_entry<R: Read>(entry: &mut R, c: &crate::manifest::Content, out: &mut File, tmp: &Path) -> Result<(u64, String)> {
    let fail = |m: String| Error::new(9, m);
    let mut hasher = Sha256::new();
    let (mut n, mut buf) = (0u64, vec![0u8; 65536]);
    let mut reader = entry.by_ref().take(c.size + 1);
    loop {
        let k = reader.read(&mut buf).map_err(|e| fail(format!("{}: {e}", c.path)))?;
        if k == 0 {
            break;
        }
        hasher.update(&buf[..k]);
        n += k as u64;
        out.write_all(&buf[..k]).map_err(|e| fail(format!("{}: {e}", tmp.display())))?;
    }
    out.sync_all().map_err(|e| fail(format!("{}: {e}", tmp.display())))?;
    Ok((n, hex(&hasher.finalize())))
}
