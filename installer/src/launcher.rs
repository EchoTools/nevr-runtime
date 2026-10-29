//! Echo VR Launcher owns some installs (#1285). It keeps what it manages in `launcher.json` in its data folder
//! (`%LOCALAPPDATA%\EchoVR_Launcher` on Windows): each `versions[]` entry names an install root, the folder that holds
//! `ready-at-dawn-echo-arena`, and the game's folder is `<root>/ready-at-dawn-echo-arena/bin/win10` (`bin/win7` for the
//! 2017 builds). A PC install into such a folder would put a second copy of nEVR beside the launcher's, so it is refused
//! and the player is told to update through the launcher.
//!
//! The file is read, never written. A file that is there but cannot be read is a refusal of its own: the check cannot
//! say the launcher is absent. A file that is not there means no launcher.

use serde_json::Value;
use std::path::{Path, PathBuf};

/// What the launcher is called in a message to the player.
pub const NAME: &str = "Echo VR Launcher";
const ARENA_DIR: &str = "ready-at-dawn-echo-arena";
const BIN_DIRS: [&str; 2] = ["bin/win10", "bin/win7"];

#[derive(Debug, PartialEq, Eq)]
pub enum Owner {
    /// No launcher lists the folder (or there is no launcher data).
    Nobody,
    /// The launcher lists the folder; `id` is its entry's id.
    Launcher { id: String },
    /// `launcher.json` exists and cannot be read; why.
    Unknown(String),
}

/// Whether the launcher whose data folder is `launcher_dir` manages the game folder `game_dir`.
pub fn owner_of(launcher_dir: Option<&Path>, game_dir: &Path) -> Owner {
    let Some(data) = launcher_dir else { return Owner::Nobody };
    let file = data.join("launcher.json");
    let text = match std::fs::read(&file) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Owner::Nobody,
        Err(e) => return Owner::Unknown(format!("{}: {e}", file.display())),
    };
    // The launcher reads a file saved with a byte order mark, so this does too.
    let text = text.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(&text);
    let state: Value = match serde_json::from_slice(text) {
        Ok(v) => v,
        Err(e) => return Owner::Unknown(format!("{}: {e}", file.display())),
    };
    let Some(versions) = state.get("versions").and_then(Value::as_array) else {
        return Owner::Unknown(format!("{}: no versions list", file.display()));
    };
    let game = Forms::of(game_dir);
    for v in versions {
        let (Some(id), Some(root)) = (v.get("id").and_then(Value::as_str), v.get("root").and_then(Value::as_str)) else { continue };
        if root.trim().is_empty() {
            continue;
        }
        for bin in BIN_DIRS {
            let theirs = PathBuf::from(format!("{}/{ARENA_DIR}/{bin}", plain(root)));
            if game.matches(&Forms::of(&theirs)) {
                return Owner::Launcher { id: id.to_string() };
            }
        }
    }
    Owner::Nobody
}

/// A folder as written, and as the file system resolves it (through links), each with `/` separators and no trailing one.
struct Forms {
    written: String,
    resolved: Option<String>,
}

impl Forms {
    fn of(p: &Path) -> Forms {
        Forms { written: plain(&p.to_string_lossy()), resolved: std::fs::canonicalize(p).ok().map(|c| plain(&c.to_string_lossy())) }
    }

    fn matches(&self, other: &Forms) -> bool {
        let same = |a: &str, b: &str| if cfg!(windows) { a.eq_ignore_ascii_case(b) } else { a == b };
        same(&self.written, &other.written)
            || matches!((&self.resolved, &other.resolved), (Some(a), Some(b)) if same(a, b))
            || matches!(&self.resolved, Some(a) if same(a, &other.written))
            || matches!(&other.resolved, Some(b) if same(&self.written, b))
    }
}

fn plain(s: &str) -> String {
    let s = s.trim().replace('\\', "/");
    let s = s.strip_prefix("//?/").unwrap_or(&s);
    s.trim_end_matches('/').to_string()
}

/// The version in a NEVR host's identity literal (`NEVR-BUILD <version> <commit>`), if it has one.
pub fn host_version(bytes: &[u8]) -> Option<String> {
    let needle = b"NEVR-BUILD ";
    let at = bytes.windows(needle.len()).position(|w| w == needle)? + needle.len();
    let version: Vec<u8> = bytes[at..].iter().copied().take_while(|b| b.is_ascii_graphic()).take(64).collect();
    if version.is_empty() { None } else { Some(String::from_utf8_lossy(&version).into_owned()) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_version_is_the_word_after_the_literal() {
        assert_eq!(host_version(b"MZ\0 NEVR-BUILD 5.4.3 bb87761815ca\0").as_deref(), Some("5.4.3"));
        assert_eq!(host_version(b"NEVR-BUILD 5.18.0+3 abc").as_deref(), Some("5.18.0+3"));
        assert_eq!(host_version(b"no literal, nevr-runtime/1.0"), None);
        assert_eq!(host_version(b"NEVR-BUILD \0"), None);
    }

    #[test]
    fn a_path_is_compared_with_slashes_and_no_trailing_one() {
        assert_eq!(plain("C:\\Users\\a\\Echo\\"), "C:/Users/a/Echo");
        assert_eq!(plain("\\\\?\\C:\\x"), "C:/x");
    }
}
