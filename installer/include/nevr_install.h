/* nevr_install.h: the C ABI of the core install method (docs/adr/0023-core-install-method.md).
 *
 * Opaque session handle, UTF-8 strings, no callbacks. Every function returns a NevrStatus; the text of the last failure
 * is NevrLastError(session). A session is used from one thread at a time. Call from a worker thread, never from a UI
 * thread: fetches and checks block. Strings returned through `char **out` are freed with NevrStringFree.
 *
 * Names follow docs/standards/naming.md: Nevr* functions and types, NEVR_* macros. Changing this file is a breaking change
 * for every installer that calls it.
 */
#ifndef NEVR_INSTALL_H
#define NEVR_INSTALL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NevrSession NevrSession;

typedef enum NevrStatus {
  NEVR_OK = 0,           /* the call did what it says */
  NEVR_FAILED = 1,       /* a check or step failed; nothing was changed; see NevrLastError */
  NEVR_NEEDS_USER = 2,   /* the player must decide something; see NevrLastError */
  NEVR_BAD_ARGUMENT = 10,/* a null or malformed argument */
  NEVR_PANIC = 11        /* an internal error; the run is over */
} NevrStatus;

/* config_json: {"platform": "pc"|"quest", "root": "<installer state directory>", "base": "<manifest base url>"?,
 *   "channel": "beta"?, "log": "<log file>"?, "work": "<download directory>"?, "pins": "<LABEL BASE64KEY lines>"?,
 *   "min_manifest_version": N?, "now": "YYYY-MM-DDTHH:MM:SSZ"?}
 * Refuses (NEVR_FAILED, *out = NULL) when the log cannot be opened: a run that cannot record itself changes nothing. */
int NevrInstallOpen(const char *config_json, NevrSession **out);

/* Steps 1 to 6: fetch manifest.json and .sig, verify, store. */
int NevrManifestFetch(NevrSession *s);

/* Steps 2 to 6 on bytes the caller already holds. */
int NevrManifestVerify(NevrSession *s, const uint8_t *manifest, size_t manifest_len, const uint8_t *sig, size_t sig_len);

/* Step 7: JSON {"version": "...", "artifacts": [{kind, platform, path, size, sha256}...]}. channel may be NULL. */
int NevrReleasePick(NevrSession *s, const char *channel, char **out_json);

/* Step 8 for one artifact: fetch to out_path, exact size and sha256; the file is deleted on failure. platform may be NULL. */
int NevrArtifactFetch(NevrSession *s, const char *version, const char *kind, const char *platform, const char *out_path);

/* Every member of a zip artifact against `contents`. allow_dirs is nonzero for game data. platform selects the entry as in
 * NevrArtifactFetch and may be NULL, except for kind "game-data": a release lists one game-data artifact per platform
 * ("android-arm64", "windows-x86_64"), so a game-data call that names none is refused (NEVR_FAILED). */
int NevrArtifactVerifyZip(NevrSession *s, const char *version, const char *kind, const char *platform, const char *zip_path, int allow_dirs);

/* The Quest plan as JSON, from what the caller observed on the headset (the caller runs the steps it owns, such as the
 * package install, and reports the result with NevrRunEnd):
 * {"installed": {"version_code": N, "signers": ["<sha256 of a signing certificate>", ...] | null} | null,
 *  "data": {"legacy": bool, "media": bool} | null, "consent_uninstall": bool} */
int NevrPlanBuild(NevrSession *s, const char *version, const char *observed_json, char **out_json);

/* Self-update (docs/adr/0024-pc-self-update.md): stages the channel's release under <root_dir>/update/staging/<version> unless it
 * is the running one or a quarantined one, after NevrManifestFetch has verified the manifest. Open the session with
 * "nevr_root" in config_json so state, log and downloads live under the nevr root. current_json (may be NULL):
 * {"version": "...", "runtime_sha256": "<hex>", "quarantine": [{"version": "...", "runtime_sha256": "<hex>"}]}.
 * Result: {"action": "none"|"staged"|"blocked", "version", "direction": "upgrade"|"downgrade"|"replace"|"install",
 *          "reason", "files": [{"path", "size", "sha256"}]}. Nothing is published unless every file passed its checks. */
int NevrPcStage(NevrSession *s, const char *root_dir, const char *current_json, char **out_json);

/* NevrPcStage for any profile (the Quest self-update stages its runtime library through this). profile_json:
 *   {"kind": "windows-zip" | "quest-runtime", "members": ["<exact path of the runtime>", ...]}
 * "windows-zip" is the channel's zip, from which the listed members are extracted, each checked against the artifact's `contents`;
 * a member may be a directory, a slash, an asterisk and an extension (<dir>/<asterisk>.<ext>), which takes the plain names with
 * that extension directly under <dir>. "quest-runtime" is one file with no zip: members is exactly its file name, it is checked
 * against the artifact's own size and sha256, and the stored manifest.json and manifest.json.sig that listed it are kept in
 * <root_dir>/update/staging/<version>/proof/, the manifest pair that listed the staged file. The first member names the runtime: the file
 * the release is identified by for current_json and the quarantine. The two profiles are the constants PC_PROFILE (what
 * NevrPcStage uses: the members "nevr-runtime.dll" and "plugins/<asterisk>.dll") and QUEST_PROFILE in installer/src/stage.rs:
 *   Quest: {"kind":"quest-runtime","members":["libnevr_runtime.so"]}
 * A kind the core does not stage, an unknown key, a member that is not a safe path: NEVR_FAILED, nothing fetched. A null
 * profile_json is NEVR_BAD_ARGUMENT. A session opened with platform "pc" must name the root the host loads from (or the
 * config's dev_nevr_root); one opened with platform "quest" takes any absolute root. Result and current_json as NevrPcStage;
 * a release that lists no artifact of the kind is {"action": "blocked", "reason": "no_<kind>"} (no_windows_zip, no_quest_runtime). */
int NevrStage(NevrSession *s, const char *root_dir, const char *profile_json, const char *current_json, char **out_json);

/* Self-update: the runtime refused the release staged by NevrPcStage (its Authenticode check failed). Records `version` with its
 * `runtime_sha256` (64 lowercase hex) and a lowercase `reason` word; the next NevrPcStage answers {"action": "none", "reason": "discarded"}
 * for that release and fetches nothing. A rebuilt release under the same version has other bytes and is staged again. */
int NevrPcDiscard(NevrSession *s, const char *root_dir, const char *version, const char *runtime_sha256, const char *reason);

/* Ends the run: state is "installed", "unchanged", "restored" or "needs-user". */
int NevrRunEnd(NevrSession *s, const char *state, const char *reason);

const char *NevrLastError(const NevrSession *s);
void NevrStringFree(char *p);
void NevrClose(NevrSession *s);
const char *NevrCoreVersion(void);

#ifdef __cplusplus
}
#endif

#endif /* NEVR_INSTALL_H */
