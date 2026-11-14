//! `pc stage` (ADR 0024, section 3): steps 1 to 8 for the artifact a profile names, then the members the profile lists are
//! written into `<root>/update/staging/<version>`. Nothing is published unless every check passed; the folder is built
//! as `<version>.partial-<pid>` and renamed when complete. The host swaps it in at the next launch (ADR 0024, section 4).
//!
//! A `Profile` says which artifact kind to take and which members to stage; it is the one place the platforms differ:
//! - the PC profile (`PC_PROFILE`) takes the channel's `windows-zip` and stages `nevr-runtime.dll` and `plugins/*.dll`
//!   from it, each checked against the zip's `contents`; the Authenticode signature of each file is its anchor, so no
//!   proof is kept;
//! - the Quest profile (`QUEST_PROFILE`) takes the channel's `quest-runtime`, a single `.so` with no zip, checks it
//!   against the artifact's own `size` and `sha256`, and keeps the stored manifest pair that listed it in
//!   `<version>/proof/`: an ELF carries no signature, so the signed manifest is the proof a later check runs step 2 on.

use crate::artifact;
use crate::error::{Error, Result};
use crate::manifest::{self, Artifact, Content};
use crate::session::{Ended, Session};
use crate::update_state::{is_member, is_version_name, MAX_JSON_BYTES};
use crate::util::{rel_path_ok, semver_cmp, valid_entry};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

pub const RUNTIME: &str = "nevr-runtime.dll";
/// The PC profile (ADR 0024 section 3): what `NevrPcStage` stages.
pub const PC_PROFILE: &str = r#"{"kind":"windows-zip","members":["nevr-runtime.dll","plugins/*.dll"]}"#;
/// The Quest profile: the runtime library, one file (`kQuestRuntimeMember` in `src/core/update/names.h`).
pub const QUEST_PROFILE: &str = r#"{"kind":"quest-runtime","members":["libnevr-runtime.so"]}"#;
/// A `<version>.partial-*` folder untouched for this long belongs to a stage that died.
const PARTIAL_STALE: Duration = Duration::from_secs(600);
/// The most members a profile lists.
const MAX_MEMBERS: usize = 16;
/// The longest name a wildcard member matches (`<64 characters>.dll`, the host's plugin rule in `src/core/update/names.h`).
const MAX_WILDCARD_NAME: usize = 68;
/// Where a single-file artifact's proof pair is kept beside it, and the two files of it.
const PROOF_DIR: &str = "proof";
const PROOF_FILES: [&str; 2] = ["manifest.json", "manifest.json.sig"];

/// How the artifact of a kind is shaped.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Form {
    /// A zip: the listed members are extracted, each checked against the artifact's `contents`.
    Zip,
    /// One file: the artifact itself, checked against its own `size` and `sha256`; no `contents` are read.
    File,
}

/// The artifact kinds the core stages and the shape of each. A profile that names any other kind is refused.
const KINDS: [(&str, Form); 2] = [("windows-zip", Form::Zip), ("quest-runtime", Form::File)];

/// One entry of a profile's `members`.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Member {
    /// This path, exactly.
    Exact(String),
    /// `<dir>/*<suffix>`: a plain name with that suffix directly under `dir` (`plugins/*.dll`).
    Glob { dir: String, suffix: String },
}

fn plain_name_bytes(s: &str) -> bool {
    s.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
}

impl Member {
    fn parse(spec: &str, form: Form) -> std::result::Result<Member, String> {
        if !spec.contains('*') {
            if !rel_path_ok(spec) {
                return Err(format!("member {spec:?} is not a safe relative path"));
            }
            if form == Form::File && !plain_name_bytes(spec) {
                return Err(format!("member {spec:?} of a single-file kind is not a plain file name"));
            }
            return Ok(Member::Exact(spec.to_string()));
        }
        let glob = spec.split_once('/').filter(|(dir, _)| rel_path_ok(dir) && plain_name_bytes(dir)).and_then(|(dir, rest)| {
            let suffix = rest.strip_prefix('*')?;
            let ext = suffix.strip_prefix('.')?;
            (!ext.is_empty() && ext.bytes().all(|b| b.is_ascii_alphanumeric())).then(|| Member::Glob { dir: dir.to_string(), suffix: suffix.to_string() })
        });
        match glob {
            Some(g) if form == Form::Zip => Ok(g),
            Some(_) => Err(format!("member {spec:?}: a single-file kind lists one plain file name")),
            None => Err(format!("member {spec:?} is not a path or <dir>/*.<ext>")),
        }
    }

    fn takes(&self, path: &str) -> bool {
        match self {
            Member::Exact(p) => p == path,
            Member::Glob { dir, suffix } => path.strip_prefix(dir.as_str()).and_then(|r| r.strip_prefix('/')).is_some_and(|n| {
                n.len() > suffix.len() && n.len() <= MAX_WILDCARD_NAME && n.ends_with(suffix.as_str()) && plain_name_bytes(n)
            }),
        }
    }
}

/// Which artifact kind a stage takes and which members it stages: `{"kind": "...", "members": ["...", ...]}`. The first member
/// is exact and names the runtime: the file the release is identified by (the current, quarantine and discard checks compare
/// its sha256). A single-file kind lists exactly that one member.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Profile {
    pub kind: String,
    pub form: Form,
    members: Vec<Member>,
}

impl Profile {
    pub fn parse(text: &str) -> Result<Profile> {
        let bad = |m: String| Error::new(9, format!("stage profile: {m}"));
        let v: Value = serde_json::from_str(text).map_err(|e| bad(format!("not JSON: {e}")))?;
        let obj = v.as_object().ok_or_else(|| bad("not an object".into()))?;
        if let Some(k) = obj.keys().find(|k| !matches!(k.as_str(), "kind" | "members")) {
            return Err(bad(format!("unknown key {k:?}")));
        }
        let kind = obj.get("kind").and_then(Value::as_str).ok_or_else(|| bad("has no kind".into()))?;
        let Some((_, form)) = KINDS.iter().find(|(k, _)| *k == kind) else {
            let known: Vec<&str> = KINDS.iter().map(|(k, _)| *k).collect();
            return Err(bad(format!("kind {kind:?} is not one the core stages ({})", known.join(", "))));
        };
        let list = obj.get("members").and_then(Value::as_array).ok_or_else(|| bad("has no members list".into()))?;
        if list.is_empty() || list.len() > MAX_MEMBERS {
            return Err(bad(format!("lists {} members, wants 1 to {MAX_MEMBERS}", list.len())));
        }
        let mut members: Vec<Member> = Vec::new();
        for item in list {
            let spec = item.as_str().ok_or_else(|| bad("a member is not a string".into()))?;
            let m = Member::parse(spec, *form).map_err(bad)?;
            if members.contains(&m) {
                return Err(bad(format!("lists member {spec:?} twice")));
            }
            members.push(m);
        }
        if !matches!(members[0], Member::Exact(_)) {
            return Err(bad("the first member names the runtime and is one exact path".into()));
        }
        if *form == Form::File && members.len() != 1 {
            return Err(bad("a single-file kind lists exactly one member".into()));
        }
        Ok(Profile { kind: kind.to_string(), form: *form, members })
    }

    /// The PC profile.
    pub fn pc() -> Profile {
        Profile::parse(PC_PROFILE).expect("the PC profile parses")
    }

    /// The runtime member's path.
    pub fn runtime(&self) -> &str {
        match &self.members[0] {
            Member::Exact(p) => p,
            Member::Glob { .. } => unreachable!("parse makes the first member exact"),
        }
    }

    /// Whether the profile stages the zip member `path`.
    pub fn takes(&self, path: &str) -> bool {
        self.members.iter().any(|m| m.takes(path))
    }
}

/// What the caller knows about the running release: its version, the sha256 of its runtime, and the releases that failed.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Current {
    pub version: String,
    pub runtime_sha256: String,
    pub quarantine: Vec<(String, String)>,
}

impl Current {
    pub fn parse(text: &str) -> Result<Current> {
        let v: Value = serde_json::from_str(text).map_err(|e| Error::new(0, format!("current_json is not JSON: {e}")))?;
        let s = |x: &Value| x.as_str().unwrap_or("").to_string();
        let quarantine = v["quarantine"].as_array().map(|a| a.iter().map(|q| (s(&q["version"]), s(&q["runtime_sha256"]))).collect()).unwrap_or_default();
        Ok(Current { version: s(&v["version"]), runtime_sha256: s(&v["runtime_sha256"]), quarantine })
    }
}

/// `nevr-runtime.dll`, or `plugins/<name>.dll` with a plain name: the only members the PC profile stages.
pub fn staged_member(path: &str) -> bool {
    Profile::pc().takes(path)
}

/// How the target relates to what runs now. A version that `semver_cmp` cannot read (not `X.Y.Z`, or a component past
/// u64::MAX) gives `install`: it is never compared as a smaller number (#2062).
pub fn direction(current: &str, target: &str) -> &'static str {
    match semver_cmp(target, current) {
        Some(std::cmp::Ordering::Greater) => "upgrade",
        Some(std::cmp::Ordering::Less) => "downgrade",
        Some(std::cmp::Ordering::Equal) => "replace",
        None => "install",
    }
}

/// The stage's ledger, `update\stage-ledger.json`: the names of the `staging` entries this installer made and has not yet
/// removed (`<version>` or `<version>.partial-<pid>`). Each name has its manifest, `staging\<name>.files`, which lists what
/// the stage put in that folder: each member, its literal `<member>.new` temporary, the parent folders children-first, and
/// the proof files with theirs. Only a name in the ledger is ever removed, and only the entries its manifest lists. A
/// ledger name whose folder is gone is dropped with its manifest. A `.files` with no ledger name is left and logged: only a
/// listing finds it, and a listing deletes nothing. The ledger holds at most `MAX_LEDGER_NAMES` names; a stage past that is
/// refused, not cut. A ledger that cannot be read (a link, not a file, over its size or cap, malformed) is logged, moved aside
/// to `stage-ledger.json.bad` (replacing an earlier one), and a fresh ledger is started: nothing is removed on its strength.
/// The manifest and the ledger are written through the literal `<file>.tmp`, removed by name at the stage's start.
pub const LEDGER: &str = "stage-ledger.json";
const MANIFEST_SUFFIX: &str = ".files";
/// The literal temporary a manifest or the ledger is written through (`<file>.tmp`), and the name a ledger that cannot be
/// read is moved to (`<file>.bad`). Both are by name: the stage's single writer creates and removes them.
const LEDGER_TMP_SUFFIX: &str = ".tmp";
const LEDGER_BAD: &str = ".bad";
/// The most entries one manifest lists, and the most names the ledger holds.
const MAX_ENTRIES: usize = 64;
const MAX_LEDGER_NAMES: usize = 256;
/// The member the Quest profile stages (`kQuestRuntimeMember` in `src/core/update/names.h`).
const QUEST_MEMBER: &str = "libnevr-runtime.so";

fn clean_staging(dir: &Path, keep_version: Option<&str>, log: &crate::log::Log) -> Result<()> {
    clean_staging_at(dir, keep_version, log, SystemTime::now())
}

/// Removes the recorded entries that are stale or unpublished, judged at `now`. Listing the folder only logs: a name with
/// no record is left and logged `stage_cleanup reason=no_manifest`, and nothing that a listing found is removed.
fn clean_staging_at(dir: &Path, keep_version: Option<&str>, log: &crate::log::Log, now: SystemTime) -> Result<()> {
    let Ok(meta) = fs::symlink_metadata(dir) else { return Ok(()) };
    if crate::util::is_link(&meta) || !meta.is_dir() {
        log.error("stage_cleanup", 8, json!({"name": dir.display().to_string(), "outcome": "refused", "reason": "not_a_folder"}))?;
        return Ok(());
    }
    // The leftover temporary of the ledger is this stage's own, by its literal name: removed before anything is written.
    if let Some(update) = dir.parent() {
        remove_literal(update, &format!("{LEDGER}{LEDGER_TMP_SUFFIX}"), log)?;
    }
    let names = load_ledger(dir, log)?;
    for name in &names {
        let folder = fs::symlink_metadata(dir.join(name)).ok();
        let stale_partial = name.contains(".partial-")
            && folder.as_ref().and_then(|m| m.modified().ok()).and_then(|t| now.duration_since(t).ok()).is_some_and(|age| age >= PARTIAL_STALE);
        let unpublished = !name.contains(".partial-") && Some(name.as_str()) != keep_version;
        // A name whose folder is gone is dropped with its manifest, whatever it was for: nothing is left to keep.
        if folder.is_none() || stale_partial || unpublished {
            let reason = if folder.is_none() { "folder_gone" } else if stale_partial { "stale_partial" } else { "unpublished" };
            remove_recorded(dir, name, reason, log)?;
        }
    }
    let Ok(rd) = fs::read_dir(dir) else { return Ok(()) };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        let known = names.iter().any(|n| *n == name || name == format!("{n}{MANIFEST_SUFFIX}"));
        if known || Some(name.as_str()) == keep_version {
            continue;
        }
        log.info("stage_cleanup", 8, json!({"name": name, "outcome": "kept", "reason": "no_manifest"}))?;
    }
    Ok(())
}

/// The names of the `staging` entries the stage recorded: `<version>`, or `<version>.partial-<digits>`, each a version name.
fn staging_name_ok(name: &str) -> bool {
    let base = match name.split_once(".partial-") {
        Some((base, pid)) if !pid.is_empty() && pid.bytes().all(|b| b.is_ascii_digit()) => base,
        Some(_) => return false,
        None => name,
    };
    valid_entry(name) && !name.contains('/') && is_version_name(base)
}

/// A member the core stages: `nevr-runtime.dll`, `plugins/<name>.dll` (`is_member`), or the Quest runtime.
fn staged_member_ok(path: &str) -> bool {
    is_member(path) || path == QUEST_MEMBER
}

/// The two files of the proof folder, as manifest entries (`proof/manifest.json`, `proof/manifest.json.sig`).
fn proof_file_ok(path: &str) -> bool {
    PROOF_FILES.iter().any(|f| path == format!("{PROOF_DIR}/{f}"))
}

/// What a manifest may list (the one predicate for writing and for reading): a member or a proof file, the literal
/// temporary `<member>.new` / `<proof file>.new` a stage writes beside it, a plugin folder, or the proof folder.
fn staged_entry_ok(entry: &str) -> bool {
    if !valid_entry(entry) {
        return false;
    }
    if let Some(core) = entry.strip_suffix(".new") {
        return staged_member_ok(core) || proof_file_ok(core);
    }
    staged_member_ok(entry) || proof_file_ok(entry) || entry == "plugins" || entry == PROOF_DIR
}

/// The entries a staged folder holds, for its manifest: for each wanted path its temporary `<path>.new`, the path, then its
/// parent folders deepest first; then the proof files (each with its `.new`) and the proof folder. Built before anything is
/// extracted, so a removal knows the list in advance and a killed extraction's temporary is listed too.
fn staged_entries(wanted: &[&str], proof: bool) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut push = |e: String| {
        if !out.contains(&e) {
            out.push(e);
        }
    };
    for path in wanted {
        push(format!("{path}.new"));
        push(path.to_string());
        let comps: Vec<&str> = path.split('/').collect();
        for depth in (1..comps.len()).rev() {
            push(comps[..depth].join("/"));
        }
    }
    if proof {
        for f in PROOF_FILES {
            push(format!("{PROOF_DIR}/{f}.new"));
            push(format!("{PROOF_DIR}/{f}"));
        }
        push(PROOF_DIR.to_string());
    }
    out
}

/// The entries of a release's stage, checked before anything is written: the same predicate and cap a manifest is read
/// with, so a set that could not be recorded (a name the grammar refuses, or more than `MAX_ENTRIES`) is refused here, with
/// nothing made.
fn check_recordable(wanted: &[&str], proof: bool) -> Result<Vec<String>> {
    let entries = staged_entries(wanted, proof);
    if entries.len() > MAX_ENTRIES {
        return Err(Error::new(9, format!("the release stages {} entries, a stage records at most {MAX_ENTRIES}: refused, nothing was staged", entries.len())));
    }
    if let Some(bad) = entries.iter().find(|e| !staged_entry_ok(e)) {
        return Err(Error::new(9, format!("the member {bad:?} cannot be recorded (not a member name the core stages): refused, nothing was staged")));
    }
    Ok(entries)
}

fn manifest_path(stage_root: &Path, name: &str) -> PathBuf {
    stage_root.join(format!("{name}{MANIFEST_SUFFIX}"))
}

fn write_manifest(stage_root: &Path, name: &str, entries: &[String]) -> Result<()> {
    // The same predicate and cap `read_manifest` applies: a manifest that could not be read back is never written.
    if entries.len() > MAX_ENTRIES || entries.iter().any(|e| !staged_entry_ok(e)) {
        return Err(Error::new(9, format!("the manifest of {name} would not read back (cap {MAX_ENTRIES}, the member grammar): refused")));
    }
    let path = manifest_path(stage_root, name);
    crate::util::write_with_literal_tmp(&path, LEDGER_TMP_SUFFIX, json!({"entries": entries}).to_string().as_bytes()).map_err(|e| Error::new(8, format!("{}: {e}", path.display())))
}

/// The entries a manifest lists, or None when it is absent, a link, larger than `MAX_JSON_BYTES`, or does not read as one.
fn read_manifest(stage_root: &Path, name: &str) -> Option<Vec<String>> {
    let path = manifest_path(stage_root, name);
    let meta = fs::symlink_metadata(&path).ok()?;
    if crate::util::is_link(&meta) || !meta.is_file() || meta.len() > MAX_JSON_BYTES {
        return None;
    }
    let v: Value = serde_json::from_slice(&fs::read(&path).ok()?).ok()?;
    let list = v["entries"].as_array()?;
    if list.len() > MAX_ENTRIES {
        return None;
    }
    list.iter().map(|e| e.as_str().filter(|s| staged_entry_ok(s)).map(str::to_string)).collect()
}

fn ledger_path(stage_root: &Path) -> PathBuf {
    stage_root.parent().unwrap_or(stage_root).join(LEDGER)
}

/// What the ledger file is: absent, the names it lists, or a reason it cannot be read as a ledger.
enum LedgerFile {
    Absent,
    Names(Vec<String>),
    Bad(String),
}

fn inspect_ledger(stage_root: &Path) -> LedgerFile {
    let path = ledger_path(stage_root);
    let meta = match fs::symlink_metadata(&path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return LedgerFile::Absent,
        Err(e) => return LedgerFile::Bad(format!("cannot be inspected: {e}")),
    };
    if crate::util::is_link(&meta) {
        return LedgerFile::Bad("is a link".into());
    }
    if !meta.is_file() {
        return LedgerFile::Bad("is not a file".into());
    }
    if meta.len() > MAX_JSON_BYTES {
        return LedgerFile::Bad(format!("is larger than {MAX_JSON_BYTES} bytes"));
    }
    let v = match fs::read(&path).map_err(|e| e.to_string()).and_then(|b| serde_json::from_slice::<Value>(&b).map_err(|e| e.to_string())) {
        Ok(v) => v,
        Err(e) => return LedgerFile::Bad(format!("does not parse: {e}")),
    };
    let Some(list) = v["names"].as_array() else { return LedgerFile::Bad("has no names list".into()) };
    if list.len() > MAX_LEDGER_NAMES {
        return LedgerFile::Bad(format!("lists {} names, the cap is {MAX_LEDGER_NAMES}", list.len()));
    }
    let mut names = Vec::new();
    for n in list {
        match n.as_str() {
            Some(name) if staging_name_ok(name) => names.push(name.to_string()),
            _ => return LedgerFile::Bad("lists a name that is not a staging name".into()),
        }
    }
    LedgerFile::Names(names)
}

/// The names the ledger holds, for a caller that has no log. A ledger that is unreadable is an `Err`, and nothing is removed
/// by it; the stage's own start (`load_ledger`) has already moved such a ledger aside.
fn read_ledger(stage_root: &Path) -> Result<Vec<String>> {
    match inspect_ledger(stage_root) {
        LedgerFile::Absent => Ok(Vec::new()),
        LedgerFile::Names(n) => Ok(n),
        LedgerFile::Bad(why) => Err(Error::new(8, format!("{}: {why}: refused, nothing was removed by it", ledger_path(stage_root).display()))),
    }
}

/// The ledger the stage starts from. A ledger that cannot be read as one (a link, not a file, too large, over the cap,
/// malformed) is logged at error with its reason, moved aside to the literal name `stage-ledger.json.bad` (replacing an
/// earlier `.bad`), and replaced by a fresh empty ledger. Nothing is removed on the strength of it: the folders it named are
/// unrecorded from then on (left and logged, or cleared through the stage's own version check).
fn load_ledger(stage_root: &Path, log: &crate::log::Log) -> Result<Vec<String>> {
    match inspect_ledger(stage_root) {
        LedgerFile::Absent => Ok(Vec::new()),
        LedgerFile::Names(n) => Ok(n),
        LedgerFile::Bad(why) => {
            let path = ledger_path(stage_root);
            let bad = PathBuf::from(format!("{}{LEDGER_BAD}", path.display()));
            log.error("stage_ledger", 8, json!({"outcome": "moved_aside", "reason": why, "path": path.display().to_string(), "to": bad.display().to_string()}))?;
            // A rename moves a link itself, never its target; it replaces an earlier `.bad` of the same name. A `.bad` that is
            // not a file (a non-empty folder, say) cannot be replaced by it: that is logged, and the stage goes on with a fresh
            // ledger written over the bad ledger file through the literal temporary. The `.bad` folder is left alone.
            match fs::rename(&path, &bad) {
                Ok(()) => Ok(Vec::new()),
                Err(e) => {
                    log.error("stage_ledger", 8, json!({"outcome": "not_moved_aside", "reason": e.to_string(), "path": path.display().to_string(), "to": bad.display().to_string()}))?;
                    crate::util::write_with_literal_tmp(&path, LEDGER_TMP_SUFFIX, json!({"names": []}).to_string().as_bytes())
                        .map_err(|e| Error::new(8, format!("{}: cannot be replaced by a fresh ledger: {e}", path.display())))?;
                    Ok(Vec::new())
                }
            }
        }
    }
}

/// Removes a leftover `<name>` of this stage's own, by its literal name, and logs what it did. A link at that name is removed
/// as a link (`remove_entry` never follows one).
fn remove_literal(parent: &Path, name: &str, log: &crate::log::Log) -> Result<()> {
    let outcome = crate::util::remove_entry(parent, name);
    if !matches!(outcome, crate::util::Outcome::Removed | crate::util::Outcome::Absent) {
        log.error("stage_cleanup", 8, json!({"name": name, "outcome": outcome.word(), "reason": "leftover_temporary"}))?;
    } else if outcome == crate::util::Outcome::Removed {
        log.info("stage_cleanup", 8, json!({"name": name, "outcome": "removed", "reason": "leftover_temporary"}))?;
    }
    Ok(())
}

fn write_ledger(stage_root: &Path, names: &[String]) -> Result<()> {
    let path = ledger_path(stage_root);
    if names.is_empty() {
        return match fs::remove_file(&path) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(Error::new(8, format!("{}: {e}", path.display()))),
        };
    }
    crate::util::write_with_literal_tmp(&path, LEDGER_TMP_SUFFIX, json!({"names": names}).to_string().as_bytes()).map_err(|e| Error::new(8, format!("{}: {e}", path.display())))
}

fn record_name(stage_root: &Path, name: &str) -> Result<()> {
    let mut names = read_ledger(stage_root)?;
    if !names.iter().any(|n| n == name) {
        if names.len() >= MAX_LEDGER_NAMES {
            return Err(Error::new(8, format!("the stage ledger holds {MAX_LEDGER_NAMES} names: {name} is refused, not recorded, nothing was staged")));
        }
        names.push(name.to_string());
    }
    write_ledger(stage_root, &names)
}

fn forget_name(stage_root: &Path, name: &str) -> Result<()> {
    let names: Vec<String> = read_ledger(stage_root)?.into_iter().filter(|n| n != name).collect();
    write_ledger(stage_root, &names)
}

/// Records, before anything is extracted, the entries `name` will hold: the ledger names it first, then its manifest is
/// written. The ledger's room is checked first, so a refused name leaves nothing behind; a write killed between the two
/// leaves a name with no manifest, which a cleanup forgets and whose manifest temporary it removes by name.
fn begin_staging(stage_root: &Path, name: &str, entries: &[String]) -> Result<()> {
    let names = read_ledger(stage_root)?;
    if !names.iter().any(|n| n == name) && names.len() >= MAX_LEDGER_NAMES {
        return Err(Error::new(8, format!("the stage ledger holds {MAX_LEDGER_NAMES} names: {name} is refused, not recorded, nothing was staged")));
    }
    record_name(stage_root, name)?;
    write_manifest(stage_root, name, entries)
}

/// Removes one recorded `staging` entry: first its folder's listed entries, one by name, then the folder if it is then
/// empty, then its manifest and its ledger name. A folder that still holds an unlisted file keeps its manifest and its
/// ledger name, so the next run tries again. A link named like the entry is removed as a link, never followed. A name whose
/// manifest is unreadable is forgotten and its folder left (logged `kept no_manifest`).
///
/// Returns `Ok(true)` when nothing was refused or failed (removed, absent, or not empty: the not-empty folder is logged and
/// kept), and `Ok(false)` when an entry, the folder or the manifest was refused or failed. Every refusal is logged at error
/// once, with its outcome word; the caller decides whether that stops it (the baseline does, the routine cleanup does not).
fn remove_recorded(stage_root: &Path, name: &str, reason: &str, log: &crate::log::Log) -> Result<bool> {
    // The manifest's own leftover temporary is this stage's, named by the recorded name: removed with it.
    remove_literal(stage_root, &format!("{name}{MANIFEST_SUFFIX}{LEDGER_TMP_SUFFIX}"), log)?;
    let folder = stage_root.join(name);
    let folder_meta = fs::symlink_metadata(&folder).ok();
    let Some(entries) = read_manifest(stage_root, name) else {
        log.info("stage_cleanup", 8, json!({"name": name, "outcome": "kept", "reason": "no_manifest"}))?;
        forget_name(stage_root, name)?;
        return Ok(true);
    };
    let mut clean = true;
    // A link is not a root to walk from: `remove_owned` takes the folder's real path, which a link would redirect.
    let folder_is_real_folder = folder_meta.as_ref().is_some_and(|m| m.is_dir() && !crate::util::is_link(m));
    if folder_is_real_folder {
        for (entry, outcome) in crate::util::remove_owned(&folder, &entries) {
            if !matches!(outcome, crate::util::Outcome::Removed | crate::util::Outcome::Absent | crate::util::Outcome::NotEmpty) {
                clean = false;
                log.error("stage_cleanup", 8, json!({"name": name, "entry": entry, "outcome": outcome.word(), "reason": reason}))?;
            }
        }
    }
    let folder_outcome = crate::util::remove_entry(stage_root, name);
    let line = json!({"name": name, "outcome": folder_outcome.word(), "reason": reason});
    if matches!(folder_outcome, crate::util::Outcome::Removed | crate::util::Outcome::Absent) {
        log.info("stage_cleanup", 8, line)?;
        let manifest = format!("{name}{MANIFEST_SUFFIX}");
        let manifest_outcome = crate::util::remove_entry(stage_root, &manifest);
        if !matches!(manifest_outcome, crate::util::Outcome::Removed | crate::util::Outcome::Absent) {
            log.error("stage_cleanup", 8, json!({"name": manifest, "outcome": manifest_outcome.word(), "reason": reason}))?;
            return Ok(false);
        }
        forget_name(stage_root, name)?;
        Ok(clean)
    } else {
        if matches!(folder_outcome, crate::util::Outcome::NotEmpty) {
            log.error("stage_cleanup", 8, line)?;
            return Ok(clean);
        }
        log.error("stage_cleanup", 8, line)?;
        Ok(false)
    }
}

/// Removes the unrecorded staged folder `name` of the release being staged (an install from before the manifest). Only
/// `expected`, the entries the signed release says a stage writes, children first; then the folder, if it is then empty.
/// A folder that is a link or a file is refused. A folder that still holds a file the release does not list is kept: its
/// count is logged (the listing reports, it deletes nothing). Returns true when the folder is gone.
fn remove_expected(stage_root: &Path, name: &str, expected: &[String], log: &crate::log::Log) -> Result<bool> {
    let folder = stage_root.join(name);
    let real_folder = fs::symlink_metadata(&folder).map(|m| m.is_dir() && !crate::util::is_link(&m)).unwrap_or(false);
    if !real_folder {
        log.error("stage_cleanup", 8, json!({"name": name, "outcome": "refused", "reason": "unrecorded_same_release"}))?;
        return Ok(false);
    }
    for (entry, outcome) in crate::util::remove_owned(&folder, expected) {
        if !matches!(outcome, crate::util::Outcome::Removed | crate::util::Outcome::Absent) {
            log.error("stage_cleanup", 8, json!({"name": name, "entry": entry, "outcome": outcome.word(), "reason": "unrecorded_same_release"}))?;
        }
    }
    let outcome = crate::util::remove_entry(stage_root, name);
    if matches!(outcome, crate::util::Outcome::Removed | crate::util::Outcome::Absent) {
        log.info("stage_cleanup", 8, json!({"name": name, "outcome": outcome.word(), "reason": "unrecorded_same_release"}))?;
        return Ok(true);
    }
    let left = fs::read_dir(&folder).map(|rd| rd.flatten().count()).unwrap_or(0);
    log.error("stage_cleanup", 8, json!({"name": name, "outcome": outcome.word(), "reason": "unrecorded_same_release", "left_entries": left}))?;
    Ok(false)
}

/// The staged folder `v` still holds files the release does not list (a stranger, or a file of a damaged record): it is set
/// aside by a rename to the literal `<v>.unrecorded`, which moves its bytes and deletes nothing, and the stage goes on. One
/// aside per version: when `<v>.unrecorded` is already there, nothing is moved and `Ok(false)` tells the caller to stop.
/// `v`'s ledger name and manifest go with it (the folder they recorded is no longer there). Logged at error with the count
/// of entries it held.
fn set_aside_unrecorded(stage_root: &Path, v: &str, log: &crate::log::Log) -> Result<bool> {
    let from = stage_root.join(v);
    let to = stage_root.join(format!("{v}.unrecorded"));
    if fs::symlink_metadata(&to).is_ok() {
        log.error("stage_cleanup", 8, json!({"name": v, "outcome": "stop", "reason": "aside_exists", "to": to.display().to_string()}))?;
        return Ok(false);
    }
    let left = fs::read_dir(&from).map(|rd| rd.flatten().count()).unwrap_or(0);
    if let Err(e) = fs::rename(&from, &to) {
        log.error("stage_cleanup", 8, json!({"name": v, "outcome": "stop", "reason": format!("rename failed: {e}"), "left_entries": left}))?;
        return Ok(false);
    }
    log.error("stage_cleanup", 8, json!({"name": v, "outcome": "set_aside", "reason": "unrecorded_files_held", "left_entries": left, "to": to.display().to_string()}))?;
    forget_name(stage_root, v)?;
    let _ = crate::util::remove_entry(stage_root, &format!("{v}{MANIFEST_SUFFIX}"));
    remove_literal(stage_root, &format!("{v}{MANIFEST_SUFFIX}{LEDGER_TMP_SUFFIX}"), log)?;
    Ok(true)
}

fn members_intact(dir: &Path, wanted: &[Content]) -> bool {
    wanted.iter().all(|c| match crate::fetch::sha256_file(&dir.join(&c.path)) {
        Ok((n, h)) => n == c.size && h == c.sha256,
        Err(_) => false,
    })
}

/// The stored manifest pair a single-file artifact keeps as its proof, as bytes: manifest, then signature. It must be the pair
/// that lists exactly `art` for `version`: the stage chose the artifact from the stored manifest earlier, and another run
/// may have stored a newer pair since (the state lock is held per read, not across the stage).
fn proof_pair(state_dir: &Path, version: &str, art: &Artifact) -> Result<(Vec<u8>, Vec<u8>)> {
    let (m, manifest_bytes, sig) = manifest::stored_pair(state_dir)?;
    let listed = manifest::find(&m, version, &art.kind, art.platform.as_deref())?;
    if listed != *art {
        return Err(Error::new(8, "the stored manifest changed while the release was staged: it no longer lists the file that was fetched"));
    }
    Ok((manifest_bytes, sig))
}

fn write_proof(dir: &Path, pair: &(Vec<u8>, Vec<u8>)) -> Result<()> {
    let proof = dir.join(PROOF_DIR);
    fs::create_dir_all(&proof).map_err(|e| Error::new(9, format!("{}: {e}", proof.display())))?;
    // The literal `<file>.new` temporary of each proof file, which the manifest lists (`staged_entries`), so a kill leaves
    // nothing a cleanup cannot name.
    for (name, bytes) in PROOF_FILES.iter().zip([&pair.0, &pair.1]) {
        crate::util::write_literal_new(&proof.join(name), bytes).map_err(|e| Error::new(9, format!("{}: {e}", proof.join(name).display())))?;
    }
    Ok(())
}

fn proof_intact(dir: &Path, pair: &(Vec<u8>, Vec<u8>)) -> bool {
    PROOF_FILES.iter().zip([&pair.0, &pair.1]).all(|(name, bytes)| fs::read(dir.join(PROOF_DIR).join(name)).is_ok_and(|b| b == **bytes))
}

impl Session {
    /// Records that the runtime refused the staged release `version` whose runtime has `runtime_sha256` (its Authenticode check failed after
    /// the stage verified every byte against the manifest): the next stage answers `none: discarded` for that release and fetches nothing.
    /// The record is keyed by version and runtime hash, so a rebuilt release under the same version is staged again.
    pub fn pc_discard(&self, root: &Path, version: &str, runtime_sha256: &str, reason: &str) -> Result<(Ended, Value)> {
        if !version_name(version) || !sha256_name(runtime_sha256) || !reason_word(reason) {
            return Err(Error::new(9, "discard needs a version, a lowercase sha256 and a reason word"));
        }
        let mut all = read_discards(root);
        if !all.iter().any(|(v, h)| v == version && h == runtime_sha256) {
            all.push((version.to_string(), runtime_sha256.to_string()));
        }
        let doc = json!({"discards": all.iter().map(|(v, h)| json!({"version": v, "runtime_sha256": h})).collect::<Vec<_>>()});
        let path = discards_path(root);
        let io = |e: std::io::Error| Error::new(8, format!("{}: {e}", path.display()));
        if let Some(dir) = path.parent() {
            fs::create_dir_all(dir).map_err(io)?;
        }
        crate::util::write_atomic(&path, doc.to_string().as_bytes()).map_err(io)?;
        self.log.info("stage_discard", 8, json!({"version": version, "runtime_sha256": runtime_sha256, "reason": reason, "recorded": all.len()}))?;
        let report = json!({"action": "discarded", "version": version, "reason": reason});
        Ok((self.end("unchanged", &format!("discarded {version}: {reason}"))?, report))
    }

    /// Stages the release the channel names, unless it is the running one or a failed one. Returns the run's end and the
    /// report `{"action": "none"|"staged"|"blocked", "version", "direction", "files": [...], "reason"}`.
    pub fn pc_stage(&self, version: Option<&str>, root: &Path, cur: &Current) -> Result<(Ended, Value)> {
        let (reason, report) = self.pc_stage_body(version, root, cur)?;
        Ok((self.end("unchanged", &reason)?, report))
    }

    /// `pc_stage` without the `run_end` record, for a caller (the C ABI) that ends the run itself. Returns the end reason.
    pub fn pc_stage_body(&self, version: Option<&str>, root: &Path, cur: &Current) -> Result<(String, Value)> {
        self.stage_body(&Profile::pc(), version, root, cur)
    }

    /// The stage for any profile, without the `run_end` record (`NevrStage`). Returns the end reason and the report.
    pub fn stage_body(&self, profile: &Profile, version: Option<&str>, root: &Path, cur: &Current) -> Result<(String, Value)> {
        let (m, v) = self.release(version)?;
        let dir = direction(&cur.version, &v);
        let kind = profile.kind.as_str();
        let none = |action: &str, reason: &str| -> Result<(String, Value)> {
            self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": action, "reason": reason, "direction": dir}))?;
            let report = json!({"action": action, "version": v, "direction": dir, "reason": reason, "files": []});
            Ok((format!("{action}: {reason}"), report))
        };
        let listed = m["releases"][v.as_str()]["artifacts"].as_array().map(|a| a.iter().filter(|x| x["kind"] == kind).count()).unwrap_or(0);
        if listed == 0 {
            return none("blocked", &format!("no_{}", kind.replace('-', "_")));
        }
        let art = manifest::find(&m, &v, kind, None)?;
        let wanted: Vec<Content> = match profile.form {
            Form::Zip => art.contents.iter().filter(|c| profile.takes(&c.path)).cloned().collect(),
            Form::File => vec![Content { path: profile.runtime().to_string(), sha256: art.sha256.clone(), size: art.size }],
        };
        let Some(runtime_sha256) = wanted.iter().find(|c| c.path == profile.runtime()).map(|c| c.sha256.clone()) else { return none("blocked", "no_runtime_member") };
        if cur.quarantine.iter().any(|(qv, qs)| *qv == v && *qs == runtime_sha256) {
            return none("none", "quarantined");
        }
        if read_discards(root).iter().any(|(dv, ds)| *dv == v && *ds == runtime_sha256) {
            return none("none", "discarded");
        }
        if cur.version == v && cur.runtime_sha256 == runtime_sha256 {
            return none("none", "current");
        }
        // A single-file artifact keeps the pair that listed it (see the module comment); a zip's anchor is the signature in each file.
        let proof = match profile.form {
            Form::File => match proof_pair(&self.cfg.state_dir, &v, &art) {
                Ok(p) => Some(p),
                Err(e) => {
                    self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                    return Err(e);
                }
            },
            Form::Zip => None,
        };
        // What this stage would write, checked before anything is made: a set the manifest could not record is refused here.
        let paths: Vec<&str> = wanted.iter().map(|c| c.path.as_str()).collect();
        let entries = match check_recordable(&paths, proof.is_some()) {
            Ok(e) => e,
            Err(e) => {
                self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                return Err(e);
            }
        };
        let stage_root = root.join("update").join("staging");
        // A linked `update` or `staging` folder is refused: the stage would write through the link.
        let linked = [root.join("update"), stage_root.clone()].into_iter().find(|p| fs::symlink_metadata(p).is_ok_and(|m| crate::util::is_link(&m)));
        if let Some(p) = linked {
            let msg = format!("{} is a link: the stage is refused, nothing was staged", p.display());
            self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": msg, "direction": dir}))?;
            return Err(Error::new(8, msg));
        }
        if let Err(e) = fs::create_dir_all(&stage_root) {
            let msg = format!("the staging folder {}: {e}", stage_root.display());
            self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": msg, "direction": dir}))?;
            return Err(Error::new(8, msg));
        }
        let published = fs::read_to_string(root.join("update").join("staged.json")).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()).and_then(|j| j["version"].as_str().map(str::to_string));
        clean_staging(&stage_root, published.as_deref(), &self.log)?;
        let target = stage_root.join(&v);
        let report = |reason: &str| json!({"action": "staged", "version": v, "direction": dir, "reason": reason,
            "files": wanted.iter().map(|c| json!({"path": c.path, "size": c.size, "sha256": c.sha256})).collect::<Vec<_>>()});
        if published.as_deref() == Some(v.as_str()) && target.is_dir() && members_intact(&target, &wanted) && proof.as_ref().is_none_or(|p| proof_intact(&target, p)) {
            self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": "staged", "reason": "already_staged", "direction": dir, "files": wanted.len()}))?;
            return Ok(("staged: already_staged".to_string(), report("already_staged")));
        }
        // A folder already at the target is removed only through its record; an unrecorded one is left, and the stage stops.
        if fs::symlink_metadata(&target).is_ok() {
            // Through its record when the ledger names it and its manifest reads back. Otherwise (no record, or a record that
            // does not read back, a bad one included) the signed list of this release (`entries`) is the record: the same
            // release, so a damaged record can never wedge the version being staged. A stranger file keeps the folder and
            // stops the stage, in either case.
            let ledger_names_v = read_ledger(&stage_root)?.contains(&v);
            let gone = if ledger_names_v && read_manifest(&stage_root, &v).is_some() {
                // A refusal or failure inside the record is logged by `remove_recorded`; whether the folder is still there
                // decides, below, and a folder left in place is set aside rather than stopping the stage.
                let _ = remove_recorded(&stage_root, &v, "replaced", &self.log)?;
                fs::symlink_metadata(&target).is_err()
            } else {
                let gone = remove_expected(&stage_root, &v, &entries, &self.log)?;
                if gone {
                    forget_name(&stage_root, &v)?;
                    let _ = crate::util::remove_entry(&stage_root, &format!("{v}{MANIFEST_SUFFIX}"));
                    remove_literal(&stage_root, &format!("{v}{MANIFEST_SUFFIX}{LEDGER_TMP_SUFFIX}"), &self.log)?;
                }
                gone
            };
            if !gone && !set_aside_unrecorded(&stage_root, &v, &self.log)? {
                return Err(Error::new(8, format!("the staging folder {} holds files this installer did not stage, and {v}.unrecorded is already there: the stage stops", target.display())));
            }
        }

        let zip_path = self.cfg.work_dir.join(art.path.rsplit('/').next().unwrap_or("artifact"));
        fs::create_dir_all(&self.cfg.work_dir).map_err(|e| Error::new(8, format!("{}: {e}", self.cfg.work_dir.display())))?;
        let url = format!("{}/{}", self.cfg.base, art.path);
        let fetched = match artifact::download(&url, &art, &zip_path) {
            Ok(f) => f,
            Err(e) => {
                self.log.error("artifact_fetch", 8, json!({"kind": kind, "url": url, "size": art.size, "ok": false, "reason": e.msg}))?;
                self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                return Err(e);
            }
        };
        self.log.info("artifact_fetch", 8, json!({"kind": kind, "url": url, "size": art.size, "bytes": fetched.bytes, "sha256": fetched.sha256, "ok": true, "resumed_from": fetched.resumed_from}))?;
        let staged = (|| -> Result<()> {
            if profile.form == Form::Zip {
                artifact::verify_zip(&zip_path, &art, false)?;
            }
            let partial_name = format!("{v}.partial-{}", std::process::id());
            let partial = stage_root.join(&partial_name);
            if !remove_recorded(&stage_root, &partial_name, "replaced", &self.log)? {
                return Err(Error::new(8, format!("the leftover {partial_name} could not be removed by its record: the stage stops")));
            }
            begin_staging(&stage_root, &partial_name, &entries)?;
            fs::create_dir_all(&partial).map_err(|e| Error::new(8, format!("the staging folder {}: {e}", partial.display())))?;
            let removing = |_: &Error| {
                let _ = remove_recorded(&stage_root, &partial_name, "failed", &self.log);
            };
            let written = match profile.form {
                Form::Zip => artifact::extract(&zip_path, &Artifact { contents: wanted.clone(), ..art.clone() }, &partial),
                Form::File => artifact::place_file(&zip_path, &art, profile.runtime(), &partial).map(|p| vec![p]),
            }
            .inspect_err(removing)?;
            if let Some(pair) = &proof {
                write_proof(&partial, pair).inspect_err(removing)?;
            }
            for (c, p) in wanted.iter().zip(written.iter()) {
                self.log.info("stage_member", 8, json!({"path": c.path, "size": c.size, "sha256": c.sha256, "ok": true, "file": p.file_name().map(|n| n.to_string_lossy().to_string())}))?;
            }
            // The final name is recorded before its manifest is written, and a leftover manifest temporary of that name (from a
            // killed write) is removed by name first: a kill at any point leaves a recorded name, never an unnamed temporary.
            remove_literal(&stage_root, &format!("{v}{MANIFEST_SUFFIX}{LEDGER_TMP_SUFFIX}"), &self.log)?;
            record_name(&stage_root, &v)?;
            write_manifest(&stage_root, &v, &entries)?;
            fs::rename(&partial, &target).map_err(|e| {
                let _ = remove_recorded(&stage_root, &partial_name, "failed", &self.log);
                Error::new(8, format!("the staging folder {}: {e}", target.display()))
            })?;
            forget_name(&stage_root, &partial_name)?;
            let _ = crate::util::remove_entry(&stage_root, &format!("{partial_name}{MANIFEST_SUFFIX}"));
            Ok(())
        })();
        let _ = fs::remove_file(&zip_path);
        match staged {
            Ok(()) => {
                self.log.info("stage_end", 8, json!({"version": v, "ok": true, "action": "staged", "direction": dir, "files": wanted.len()}))?;
                Ok((format!("staged {v} ({dir})"), report("staged")))
            }
            Err(e) => {
                self.log.error("stage_member", 8, json!({"ok": false, "reason": e.msg}))?;
                self.log.error("stage_end", 8, json!({"version": v, "ok": false, "reason": e.msg, "direction": dir}))?;
                Err(e)
            }
        }
    }
}

/// Where the stage records the releases the runtime refused after staging them (`NevrPcDiscard`): in the core's own state folder, so the
/// next stage does not fetch them again. A file the core cannot read is an empty record: a stage is never blocked by damage.
pub fn discards_path(root: &Path) -> PathBuf {
    root.join("update").join("manifest").join("discards.json")
}

/// The releases recorded in `discards.json`, as (version, runtime sha256).
pub fn read_discards(root: &Path) -> Vec<(String, String)> {
    let Ok(text) = fs::read_to_string(discards_path(root)) else { return Vec::new() };
    let Ok(v) = serde_json::from_str::<Value>(&text) else { return Vec::new() };
    v["discards"]
        .as_array()
        .map(|a| a.iter().filter_map(|e| Some((e["version"].as_str()?.to_string(), e["runtime_sha256"].as_str()?.to_string()))).collect())
        .unwrap_or_default()
}

fn version_name(v: &str) -> bool {
    !v.is_empty() && v.len() <= 64 && v.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'+' | b'-')) && !v.starts_with('.') && !v.ends_with('.')
}

fn sha256_name(h: &str) -> bool {
    h.len() == 64 && h.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn reason_word(r: &str) -> bool {
    !r.is_empty() && r.len() <= 64 && r.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

/// The folders `pc stage` uses under a nevr root.
pub fn paths(root: &Path) -> (PathBuf, PathBuf, PathBuf) {
    (root.join("update").join("manifest"), root.join("logs").join("nevr-update.jsonl"), root.join("update").join("work"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::sha256_hex;

    const SHA_A: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const SHA_B: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    fn manifest_listing(sha: &str) -> Vec<u8> {
        json!({"releases": {"1.0.0": {"artifacts": [{"kind": "quest-runtime", "platform": "android-arm64", "path": "v1.0.0/nevr-runtime-v1.0.0-quest-runtime.so", "size": 5, "sha256": sha}]}}})
            .to_string()
            .into_bytes()
    }

    fn store(dir: &Path, manifest: &[u8], sig: &[u8]) {
        fs::write(dir.join("manifest.json"), manifest).unwrap();
        fs::write(dir.join("manifest.json.sig"), sig).unwrap();
        fs::write(dir.join("state.json"), json!({"last_manifest_sha256": sha256_hex(manifest)}).to_string()).unwrap();
    }

    fn artifact_listed_by(manifest: &[u8]) -> Artifact {
        manifest::find(&serde_json::from_slice(manifest).unwrap(), "1.0.0", "quest-runtime", None).unwrap()
    }

    #[test]
    fn the_proof_is_the_stored_pair_that_lists_the_file_that_was_fetched() {
        let d = tempfile::tempdir().unwrap();
        let (a, b) = (manifest_listing(SHA_A), manifest_listing(SHA_B));
        let fetched = artifact_listed_by(&a);
        store(d.path(), &a, b"signature a");
        assert_eq!(proof_pair(d.path(), "1.0.0", &fetched).unwrap(), (a.clone(), b"signature a".to_vec()));
        // Another run stored a newer pair, between the choice of the artifact and the copy: that pair lists other bytes.
        store(d.path(), &b, b"signature b");
        let e = proof_pair(d.path(), "1.0.0", &fetched).unwrap_err();
        assert!(e.msg.contains("changed while the release was staged"), "{e}");
    }

    #[test]
    fn the_proof_needs_the_stored_signature() {
        let d = tempfile::tempdir().unwrap();
        let a = manifest_listing(SHA_A);
        store(d.path(), &a, b"x");
        fs::remove_file(d.path().join("manifest.json.sig")).unwrap();
        assert!(proof_pair(d.path(), "1.0.0", &artifact_listed_by(&a)).unwrap_err().msg.contains("no stored signature"));
    }

    #[test]
    fn the_pc_profile_stages_the_runtime_and_plain_plugin_names_only() {
        for (path, want) in [
            ("nevr-runtime.dll", true),
            ("plugins/ok-1_a.dll", true),
            ("plugins/.dll", false),
            ("plugins/notes.txt", false),
            ("plugins/sub/x.dll", false),
            ("plugins/x.dll/", false),
            ("Plugins/x.dll", false),
            ("BugSplat64.dll", false),
            ("x/plugins/x.dll", false),
        ] {
            assert_eq!(staged_member(path), want, "{path}");
        }
        assert!(staged_member(&format!("plugins/{}.dll", "a".repeat(64))));
        assert!(!staged_member(&format!("plugins/{}.dll", "a".repeat(65))));
    }

    #[test]
    fn the_quest_profile_names_one_file_and_the_pc_profile_names_the_runtime_first() {
        let q = Profile::parse(QUEST_PROFILE).unwrap();
        assert_eq!((q.kind.as_str(), q.form, q.runtime()), ("quest-runtime", Form::File, "libnevr-runtime.so"));
        assert!(q.takes("libnevr-runtime.so") && !q.takes("other.so"));
        let p = Profile::pc();
        assert_eq!((p.kind.as_str(), p.form, p.runtime()), ("windows-zip", Form::Zip, "nevr-runtime.dll"));
    }

    fn log_text(dir: &Path) -> String {
        fs::read_to_string(dir.join("log.jsonl")).unwrap_or_default()
    }

    #[test]
    fn a_staged_folder_with_no_manifest_is_left_and_logged() {
        // Red-first (P4): the current clean_staging removes every unpublished folder it lists.
        let root = tempfile::tempdir().unwrap();
        let staging = root.path().join("update").join("staging");
        fs::create_dir_all(staging.join("9.9.9")).unwrap();
        fs::write(staging.join("9.9.9").join("nevr-runtime.dll"), b"not ours to remove").unwrap();
        let log = crate::log::Log::open(&root.path().join("log.jsonl"), "test").unwrap();
        clean_staging(&staging, None, &log).unwrap();
        assert!(staging.join("9.9.9").join("nevr-runtime.dll").is_file(), "a folder with no manifest must be left");
        assert!(log_text(root.path()).contains("no_manifest"), "the folder is logged with reason no_manifest");
    }

    #[test]
    fn a_link_in_staging_named_like_a_version_is_never_followed() {
        // Red-first (P4): the canary sits outside the root; the link in staging points at it.
        let root = tempfile::tempdir().unwrap();
        let canary = tempfile::tempdir().unwrap();
        fs::write(canary.path().join("keep.txt"), b"canary").unwrap();
        let staging = root.path().join("update").join("staging");
        fs::create_dir_all(&staging).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(canary.path(), staging.join("1.0.0")).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_dir(canary.path(), staging.join("1.0.0")).unwrap();
        let log = crate::log::Log::open(&root.path().join("log.jsonl"), "test").unwrap();
        clean_staging(&staging, None, &log).unwrap();
        assert_eq!(fs::read(canary.path().join("keep.txt")).unwrap(), b"canary", "the canary outside the root must be intact");
    }

    fn stage_fixture() -> (tempfile::TempDir, PathBuf, crate::log::Log) {
        let root = tempfile::tempdir().unwrap();
        let staging = root.path().join("update").join("staging");
        fs::create_dir_all(&staging).unwrap();
        let log = crate::log::Log::open(&root.path().join("log.jsonl"), "test").unwrap();
        (root, staging, log)
    }

    #[test]
    fn a_recorded_parent_that_is_a_link_refuses_the_entry_and_the_canary_stays() {
        let (root, staging, log) = stage_fixture();
        let canary = tempfile::tempdir().unwrap();
        fs::write(canary.path().join("x.dll"), b"canary").unwrap();
        let folder = staging.join("1.0.0");
        fs::create_dir_all(&folder).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(canary.path(), folder.join("plugins")).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_dir(canary.path(), folder.join("plugins")).unwrap();
        begin_staging(&staging, "1.0.0", &["plugins/x.dll".to_string(), "plugins".to_string()]).unwrap();
        clean_staging(&staging, None, &log).unwrap();
        assert_eq!(fs::read(canary.path().join("x.dll")).unwrap(), b"canary", "the link's target must be intact");
        assert!(log_text(root.path()).contains("refused_link"), "the refusal is logged");
    }

    #[test]
    fn a_recorded_folder_with_an_unlisted_file_is_not_empty_and_the_file_is_kept() {
        let (root, staging, log) = stage_fixture();
        let folder = staging.join("1.0.0");
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join("nevr-runtime.dll"), b"ours").unwrap();
        fs::write(folder.join("stranger.dll"), b"not listed").unwrap();
        begin_staging(&staging, "1.0.0", &["nevr-runtime.dll".to_string()]).unwrap();
        clean_staging(&staging, None, &log).unwrap();
        assert_eq!(fs::read(folder.join("stranger.dll")).unwrap(), b"not listed", "an unlisted file is kept");
        assert!(!folder.join("nevr-runtime.dll").exists(), "the listed member is removed");
        assert!(log_text(root.path()).contains("not_empty"), "the folder is logged not_empty");
    }

    #[test]
    fn an_interrupted_stage_is_cleaned_and_a_stranger_beside_it_is_not() {
        let (_root, staging, log) = stage_fixture();
        let partial = "1.2.3.partial-4242";
        let entries = staged_entries(&["plugins/a.dll", "nevr-runtime.dll"], true);
        begin_staging(&staging, partial, &entries).unwrap();
        let p = staging.join(partial);
        fs::create_dir_all(p.join("plugins")).unwrap();
        fs::create_dir_all(p.join("proof")).unwrap();
        fs::write(p.join("nevr-runtime.dll"), b"a").unwrap();
        fs::write(p.join("plugins").join("a.dll"), b"b").unwrap();
        fs::write(p.join("proof").join("manifest.json"), b"c").unwrap();
        fs::write(p.join("proof").join("manifest.json.sig"), b"d").unwrap();
        fs::write(staging.join("stranger.txt"), b"mine").unwrap();
        // The stage died: the partial is older than the stale age.
        clean_staging_at(&staging, None, &log, SystemTime::now() + PARTIAL_STALE + Duration::from_secs(1)).unwrap();
        assert!(!p.exists(), "everything the stage made is gone");
        assert!(!manifest_path(&staging, partial).exists(), "its manifest is gone");
        assert!(!ledger_path(&staging).exists(), "its ledger name is gone");
        assert_eq!(fs::read(staging.join("stranger.txt")).unwrap(), b"mine", "a file beside it is untouched");
    }

    #[test]
    fn the_manifest_lists_members_children_first_and_the_proof() {
        assert_eq!(
            staged_entries(&["plugins/a.dll", "nevr-runtime.dll"], true),
            [
                "plugins/a.dll.new",
                "plugins/a.dll",
                "plugins",
                "nevr-runtime.dll.new",
                "nevr-runtime.dll",
                "proof/manifest.json.new",
                "proof/manifest.json",
                "proof/manifest.json.sig.new",
                "proof/manifest.json.sig",
                "proof"
            ]
        );
        assert_eq!(staged_entries(&["libnevr-runtime.so"], false), ["libnevr-runtime.so.new", "libnevr-runtime.so"]);
        assert!(staged_entry_ok("plugins/a.dll") && staged_entry_ok("proof/manifest.json.sig") && staged_entry_ok("libnevr-runtime.so"));
        assert!(staged_entry_ok("plugins/a.dll.new") && staged_entry_ok("proof/manifest.json.new") && staged_entry_ok("libnevr-runtime.so.new"));
        for bad in ["../x.dll", "other/a.dll", "plugins/a.exe", "proof/other", "a\\b", "plugins.new", "nevr-runtime.dll.new.new", "plugins/.x.dll", "plugins/con.dll", "x.new"] {
            assert!(!staged_entry_ok(bad), "{bad}");
        }
    }

    #[test]
    fn a_release_that_cannot_be_recorded_is_refused_before_anything_is_made() {
        // The cap: one wildcard member per plugin, each with its `.new` and its folder, is more than a manifest holds.
        let many: Vec<String> = (0..40).map(|i| format!("plugins/p{i}.dll")).collect();
        let refs: Vec<&str> = many.iter().map(String::as_str).collect();
        assert!(check_recordable(&refs, false).unwrap_err().msg.contains("at most 64"));
        // The grammar: a name the core does not stage cannot be recorded (`is_member` refuses a dot-led and a device name).
        for bad in ["plugins/.x.dll", "plugins/con.dll"] {
            assert!(check_recordable(&[bad], false).unwrap_err().msg.contains("cannot be recorded"), "{bad}");
        }
        assert_eq!(check_recordable(&["plugins/ok.dll"], false).unwrap().len(), 3);
    }

    fn ledger_names(staging: &Path) -> Vec<String> {
        serde_json::from_slice::<Value>(&fs::read(ledger_path(staging)).unwrap()).unwrap()["names"].as_array().unwrap().iter().map(|n| n.as_str().unwrap().to_string()).collect()
    }

    #[test]
    fn a_killed_extraction_leaves_nothing_a_cleanup_cannot_name() {
        // Item 1: the member's literal `<member>.new` temporary is listed, so an interrupted partial is removed whole.
        let (_root, staging, log) = stage_fixture();
        let partial = "1.2.3.partial-4242";
        begin_staging(&staging, partial, &staged_entries(&["nevr-runtime.dll"], false)).unwrap();
        let p = staging.join(partial);
        fs::create_dir_all(&p).unwrap();
        fs::write(p.join("nevr-runtime.dll.new"), b"killed mid-write").unwrap();
        clean_staging_at(&staging, None, &log, SystemTime::now() + PARTIAL_STALE + Duration::from_secs(1)).unwrap();
        assert!(!p.exists(), "the partial, with its temporary, is removed whole");
        assert!(!manifest_path(&staging, partial).exists() && !ledger_path(&staging).exists());
    }

    #[test]
    fn a_ledger_name_whose_folder_is_gone_is_dropped_with_its_manifest() {
        // Item 3: nothing is left to keep, so the name goes and its `.files` with it, even the published one.
        let (root, staging, log) = stage_fixture();
        begin_staging(&staging, "1.0.0", &staged_entries(&["nevr-runtime.dll"], false)).unwrap();
        clean_staging_at(&staging, Some("1.0.0"), &log, SystemTime::now()).unwrap();
        assert!(!manifest_path(&staging, "1.0.0").exists(), "its manifest is removed by name");
        assert!(!ledger_path(&staging).exists(), "the ledger is empty, so it is removed");
        assert!(log_text(root.path()).contains("folder_gone"));
    }

    #[test]
    fn an_orphan_manifest_is_left_and_logged_because_only_a_listing_finds_it() {
        // Item 3: a `.files` with no ledger name cannot be named from a record, so it is not removed.
        let (root, staging, log) = stage_fixture();
        fs::write(staging.join("9.9.9.files"), json!({"entries": ["nevr-runtime.dll"]}).to_string()).unwrap();
        clean_staging(&staging, None, &log).unwrap();
        assert!(staging.join("9.9.9.files").exists(), "an orphan manifest is left");
        assert!(log_text(root.path()).contains("no_manifest"));
    }

    #[test]
    fn the_ledger_refuses_a_name_past_its_cap_and_writes_nothing() {
        // Item 3: the 257th name is refused, not cut: no manifest is written, the ledger keeps its 256 names.
        let (_root, staging, _log) = stage_fixture();
        let names: Vec<String> = (0..MAX_LEDGER_NAMES).map(|i| format!("1.0.{i}")).collect();
        write_ledger(&staging, &names).unwrap();
        let err = begin_staging(&staging, "9.9.9", &staged_entries(&["nevr-runtime.dll"], false)).unwrap_err();
        assert!(err.msg.contains("256"), "{}", err.msg);
        assert!(!manifest_path(&staging, "9.9.9").exists(), "no manifest for a refused name");
        assert_eq!(ledger_names(&staging).len(), MAX_LEDGER_NAMES);
    }

    #[test]
    fn a_ledger_that_is_a_link_is_moved_aside_and_its_target_is_untouched() {
        // Item 8 (round 4): a link ledger is not followed or removed: it is renamed to `.bad`, and the target is intact.
        let (_root, staging, log) = stage_fixture();
        let canary = tempfile::tempdir().unwrap();
        let target = canary.path().join("ledger.json");
        fs::write(&target, json!({"names": ["1.0.0"]}).to_string()).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(&target, ledger_path(&staging)).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_file(&target, ledger_path(&staging)).unwrap();
        assert!(read_ledger(&staging).is_err());
        // The stage's start moves the link aside by name (a rename moves the link, never its target) and starts fresh.
        clean_staging(&staging, None, &log).unwrap();
        assert!(fs::symlink_metadata(ledger_path(&staging)).is_err(), "the link is no longer the ledger");
        assert!(fs::symlink_metadata(PathBuf::from(format!("{}{LEDGER_BAD}", ledger_path(&staging).display()))).is_ok(), "moved to .bad");
        assert_eq!(fs::read(&target).unwrap(), json!({"names": ["1.0.0"]}).to_string().into_bytes(), "the link's target is intact");
        assert!(log_text(_root.path()).contains("moved_aside"));
    }

    #[test]
    fn a_ledger_over_its_cap_is_refused_not_cut() {
        // Item 8: a hostile ledger of 257 names is refused by the reader, so no name is silently dropped or removed.
        let (_root, staging, _log) = stage_fixture();
        let names: Vec<String> = (0..=MAX_LEDGER_NAMES).map(|i| format!("1.0.{i}")).collect();
        fs::write(ledger_path(&staging), json!({"names": names}).to_string()).unwrap();
        assert!(read_ledger(&staging).unwrap_err().msg.contains("cap"));
    }

    #[test]
    fn a_manifest_over_its_cap_is_kept_with_its_folder_and_logged_not_wedged() {
        // Item 8: a manifest of 65 entries does not read back; its folder is left, logged `no_manifest`, and the name is
        // forgotten, so the stage is not stuck on it.
        let (root, staging, log) = stage_fixture();
        let folder = staging.join("1.0.0");
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join("nevr-runtime.dll"), b"kept").unwrap();
        let entries: Vec<String> = (0..=MAX_ENTRIES).map(|i| format!("plugins/p{i}.dll")).collect();
        fs::write(manifest_path(&staging, "1.0.0"), json!({"entries": entries}).to_string()).unwrap();
        fs::write(ledger_path(&staging), json!({"names": ["1.0.0"]}).to_string()).unwrap();
        clean_staging(&staging, None, &log).unwrap();
        assert_eq!(fs::read(folder.join("nevr-runtime.dll")).unwrap(), b"kept", "the folder is left");
        assert!(log_text(root.path()).contains("no_manifest"));
        assert!(!ledger_path(&staging).exists(), "the name is forgotten: the ledger is empty and removed");
    }

    #[test]
    fn a_recorded_folder_that_is_itself_a_link_is_unlinked_and_its_target_is_intact() {
        // Item 8: the link is removed as a link; the folder it points at is never walked.
        let (_root, staging, log) = stage_fixture();
        let canary = tempfile::tempdir().unwrap();
        fs::write(canary.path().join("keep.txt"), b"canary").unwrap();
        fs::write(canary.path().join("nevr-runtime.dll"), b"canary").unwrap();
        begin_staging(&staging, "1.0.0", &staged_entries(&["nevr-runtime.dll"], false)).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(canary.path(), staging.join("1.0.0")).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_dir(canary.path(), staging.join("1.0.0")).unwrap();
        clean_staging(&staging, None, &log).unwrap();
        assert!(fs::symlink_metadata(staging.join("1.0.0")).is_err(), "the link is gone");
        assert_eq!(fs::read(canary.path().join("keep.txt")).unwrap(), b"canary");
        assert_eq!(fs::read(canary.path().join("nevr-runtime.dll")).unwrap(), b"canary", "a file the link pointed at is intact");
    }

    #[test]
    fn a_killed_manifest_or_ledger_write_leaves_nothing_a_cleanup_cannot_name() {
        // Round 4: a manifest temporary `<name>.files.tmp` is removed with its recorded name, and a ledger temporary at the
        // stage's start by its literal name.
        let (_root, staging, log) = stage_fixture();
        let partial = "1.2.3.partial-4242";
        begin_staging(&staging, partial, &staged_entries(&["nevr-runtime.dll"], false)).unwrap();
        fs::write(staging.join(format!("{partial}.files.tmp")), b"killed mid-write").unwrap();
        fs::write(staging.join("..").join(format!("{LEDGER}.tmp")), b"killed mid-write").unwrap();
        clean_staging_at(&staging, None, &log, SystemTime::now() + PARTIAL_STALE + Duration::from_secs(1)).unwrap();
        assert!(!staging.join(format!("{partial}.files.tmp")).exists(), "the manifest temporary is removed by name");
        assert!(!staging.join("..").join(format!("{LEDGER}.tmp")).exists(), "the ledger temporary is removed by name");
        assert!(!manifest_path(&staging, partial).exists() && !ledger_path(&staging).exists());
    }
}
