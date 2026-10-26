# Verifying a release

For people who write an installer or a launcher that puts nEVR on a player's machine (a PC installer, a Quest
installer, a dedicated-server script). It says what to fetch, what to check, in what order, and what to tell the player
when a check fails. Every step has a command or a small snippet, and the fixtures in
`docs/reference/release-verification/` let you see every check pass and every tampered copy fail.

The release is described by one signed file, `manifest.json`, with its signature `manifest.json.sig` beside it. An
installer installs only what that file describes. Tracking issue for the manifest and its publication: #898.

> **The fixtures are test fixtures.** Everything under `docs/reference/release-verification/` is signed with a test key
> (id `B1DC2FE8001CB6C7`) that is **not one of the pinned keys**. The fixtures exist only to show each check pass and
> fail. They are not a release, no installer may accept them, and the pinned keys never sign anything but real
> releases.

## Status of this document

- The manifest is specified by #898 and no live manifest is published. Until one is, run every step against the
  fixtures. They hold the real v5.5.1 file list, hashes and sizes, signed with a **test key**, not with a release key.
  A live manifest is signed by one of the pinned keys below, and the fixtures are not ("Fixtures and test key").
- The snippets compile and run with `minisign` 0.12, `jq` 1.8, GNU coreutils (`dd`, `date`, `sha256sum`) with `bash` as
  `sh`, `curl`, Python 3.14, Rust 1.99 with the crates pinned in `Cargo.toml`, and .NET SDK 10 with the packages pinned
  in the two `.csproj` files. The Rust and C# loops for the zip members (step 8), the `adb` command and
  `Get-AuthenticodeSignature` are described and are not part of that run.
- What the PC host checks before it loads the runtime is split into built and not built in "What the PC host checks".

## The order of checks

Run these in order. Each one stops the install when it fails, and a failed check never falls through to a later one.
The "defends against" column is what the check is for, so you can tell what a shortcut gives up.

| Step | Check | Defends against |
| --- | --- | --- |
| 1 | Fetch `manifest.json` (at most 1 MiB) and `manifest.json.sig` (at most 1 KiB), reading no more than that whatever the server sends | A server that sends an endless body |
| 2 | The signature, over the exact bytes you hold, before any JSON parser reads them. Algorithm `ED`, a pinned key, not revoked | Anyone who can write to the storage, the cache or a mirror in front of it |
| 3 | `schema_version` is 1 and `product` is `nevr-runtime` | A validly signed file for something else, or a format you cannot read |
| 4 | `manifest_version` is not lower than the highest you accepted, nor than the minimum compiled into the installer | Rollback to an older manifest that was really signed, on a fresh install or after the stored state was lost |
| 5 | The current time is a valid UTC time and is before `expires` | A stale manifest served forever so nobody sees the update; a broken clock reading that makes every manifest look fresh |
| 6 | Store the new highest `manifest_version`, the union of `revoked_keys`, and the verified bytes, the state last | The same two attacks on the next run |
| 7 | Pick the channel's version; it must be a key of `releases` | Installing something no channel names |
| 8 | Read the manifest back from what step 6 stored, not from a path you were handed. For each artifact you handle: check its `path` before any request, read at most `size` bytes, then exact `size`, `sha256`, and for the Windows zip every entry | A swapped, truncated or padded file; files mixed from two releases; a manifest that was never verified |

## What you pin and what you store

**Pin these three public keys in the installer.** A manifest is accepted when it is signed by any of them and the key
is not revoked. K1 signs releases. K2 and KR are pinned ahead of time and are not used for an ordinary release, so
the signing key can change without a new installer. The base64 line is the second line of a minisign `.pub` file.

```text pinned-keys.txt
# Pinned manifest public keys: LABEL BASE64KEY.
# K1: key id 343D0247859CEEC0 (file order c0ee9c8547023d34)
K1 RWTA7pyFRwI9NPbeZVbRJxtiA0YsG3jolqhV8mxuCEP0AUb24092wkuy
# K2: key id 5572A7F345AF723E (file order 3e72af45f3a77255)
K2 RWQ+cq9F86dyVc7yFNVU+YAVnSl5pJFun+A68lujJ1p3GsMb2v3vrK0F
# KR: key id 7F97E500E1AC0C41 (file order 410cace100e5977f)
KR RWRBDKzhAOWXf+lhvvVo22yLwhwmABcky2IkqcvmLWtpG8Pm9/ux8dYx
```

A minisign key id has two spellings, and mixing them up makes a pin or a revocation silently never match:

- `minisign` prints the id as upper-case hex of the 8 bytes in **reverse** order (`343D0247859CEEC0`).
- The bytes sit in the key and signature blobs in the order `c0ee9c8547023d34`. `revoked_keys` in the manifest uses this
  **file order**, lower-case hex, and so do the snippets below.

**Store between runs** in one state directory, in the installer's own data directory, never next to the files a player
can edit freely. The snippets write it, and step 8 reads it back:

| File in the state directory | What it holds | Why |
| --- | --- | --- |
| `state.json` | `highest_manifest_version`, `revoked_keys` (the union of every accepted manifest's list), `last_manifest_sha256` | Step 4 (an equal version with different bytes is an attack), step 2 (a revoked key stays refused whatever later manifests say), step 8 (which manifest was verified) |
| `manifest.json`, `manifest.json.sig` | The last manifest that passed steps 2 to 5, byte for byte | Step 8 uses these bytes, and only these |

```json
{ "highest_manifest_version": 1, "revoked_keys": [], "last_manifest_sha256": "<64 hex>" }
```

Also keep the installed release version and each installed artifact's `sha256`: step 7 does nothing when the channel's
version is lower than the installed one.

The files are written one at a time, each as a temporary file renamed over the old one, the manifest and its signature
first and `state.json` last. A crash between two renames leaves a manifest whose hash is not in `state.json`, and step 8
refuses it. A state file that exists and cannot be read, or does not hold what the table says, is a failure at step 6,
never an empty state: an unreadable file must not turn into a fresh install.

**Compile in a minimum `manifest_version`**, the one that was current when your build was made. It is a constant in the
source that you raise with every release of the installer, and nothing at run time can lower it. Step 4 uses
`max(stored, compiled)`, so a fresh install, or one whose state was lost or deleted, cannot be rolled back further than
its own build. Each snippet below has the constant at the top; the examples set it to 1, the `manifest_version` of the
fixtures.

### If a signing key is compromised

K2 and KR are pinned so that the signing key can change without a new installer, and this is what a consumer does
when it happens. A manifest signed by a pinned key that is not revoked, with the compromised key's id (file order) in
its `revoked_keys` and a `manifest_version` above the highest the consumer has accepted, passes steps 2 to 5. Step 6
stores the revocation, and from then on a manifest signed by the revoked key is refused at step 2, whatever its
`manifest_version`. KR is the key kept for this case.

Two limits follow from the rules above. The recovery manifest has to carry a higher `manifest_version` than any the
compromised key signed, because a consumer that has already accepted a higher one refuses a lower one as a rollback. And
a consumer that never fetches the recovery manifest keeps trusting the revoked key until it does, or until it is
replaced by an installer that pins a different set of keys. "Test vectors" has a case that runs this.

## Step 1: fetch

The manifest is at `https://evr.echo.taxi/nevr-runtime/manifest.json` and its signature at
`https://evr.echo.taxi/nevr-runtime/manifest.json.sig`. Every artifact `path` in the manifest is relative to
`https://evr.echo.taxi/nevr-runtime/` and is `v<version>/nevr-runtime-v<version>-<name>`, so a manifest cannot send you
to another host. Do not follow redirects.

`curl --max-filesize` only applies when the server announces a length, and a server that announces none (or a wrong
one) can send an endless body. So every download goes through this script, which stops reading after `max` bytes and
fails when the server sent more:

```sh fetch-capped.sh
#!/bin/sh
# usage: fetch-capped.sh <step> <url> <out-file> <max-bytes>
# Writes at most <max-bytes> bytes to <out-file>. Fails, and removes <out-file>, when the server sends more than that,
# whether or not it announced a length, or when curl fails.
set -eu
LC_ALL=C
export LC_ALL
step=$1 url=$2 out=$3 max=$4
fail() { rm -f "$out"; echo "FAIL step $step: $1" >&2; exit 1; }
case $max in ''|*[!0-9]*) fail "the byte limit $max is not a number" ;; esac
rc=$(mktemp)
trap 'rm -f "$rc"' EXIT
{ if curl --fail --silent --show-error "$url"; then echo 0 > "$rc"; else echo 1 > "$rc"; fi; } \
  | dd of="$out" bs=65536 iflag=fullblock,count_bytes count=$((max + 1)) 2>/dev/null || true
[ -f "$out" ] || fail "could not write $out"
got=$(wc -c < "$out")
[ "$got" -le "$max" ] || fail "the server sent more than $max bytes"
[ "$(cat "$rc")" = 0 ] || fail "could not fetch $url"
```

```sh
BASE=https://evr.echo.taxi/nevr-runtime
sh fetch-capped.sh 1 "$BASE/manifest.json"     manifest.json     1048576
sh fetch-capped.sh 1 "$BASE/manifest.json.sig" manifest.json.sig 1024
```

The Rust program and the C# harness below do the same step as `fetch <base-url> <dir>`, with the same limits.

If step 2 fails on a pair you just fetched, fetch the pair again, twice, 3 seconds apart, before you report the failure:
the two files are separate objects and a reader can catch them between two writes.

## Step 2: the signature

The signature is a minisign signature in the hashed mode (`ED`): Ed25519 over the BLAKE2b-512 hash of the file, plus a
signed "trusted comment". Line 2 of the `.sig` is base64 of 74 bytes, `alg(2) || key_id(8) || signature(64)`. Refuse
anything but `ED` (the legacy `Ed` mode signs the raw file), find the pinned key whose id matches, check it is not
revoked, then verify. Take nothing from the trusted comment: the JSON is authoritative.

Why the explicit `ED` check: the `minisign` command verifies a legacy `Ed` signature too, so it cannot be the whole check.

**Verify the bytes you parse.** Read `manifest.json` and `manifest.json.sig` once, into memory or into a private copy
(a directory only you can read, made fresh for this run), and do steps 2 to 7 on that copy. Verifying a path and then
opening the path again to parse it checks one file and uses another: the path can be rewritten in between. The script
copies both files into a `mktemp -d` directory (mode 0700) and never reads `<dir>` again; the programs read each file
into a byte array once.

### Command line (minisign, jq)

This script runs steps 2 to 7. It prints `FAIL step N: ...` on stderr and exits 1 at the first failure. It writes the
state directory only when steps 2 to 5 pass.

```sh verify-manifest.sh
#!/bin/sh
# usage: verify-manifest.sh <dir with manifest.json and manifest.json.sig> <pinned-keys.txt> <state-dir> <now> <channel>
# <now> is UTC as YYYY-MM-DDTHH:MM:SSZ, e.g. $(date -u +%Y-%m-%dT%H:%M:%SZ)
# <state-dir> holds state.json and the last verified manifest.json and manifest.json.sig (step 8 reads them from there).
# MIN_MANIFEST_VERSION is the manifest_version that was current when this installer was built. The integrator sets it
# in the source and raises it with every release of the installer; nothing at run time can lower it.
MIN_MANIFEST_VERSION=1
set -eu
LC_ALL=C
export LC_ALL
dir=$1 pinned=$2 state_dir=$3 now=$4 channel=$5
fail() { echo "FAIL step $1: $2" >&2; exit 1; }

# True for a real UTC time written exactly as YYYY-MM-DDTHH:MM:SSZ: no other character, no trailing newline.
utc() {
  case $1 in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z) ;;
    *) return 1 ;;
  esac
  [ "$(date -u -d "$1" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)" = "$1" ]
}
utc "$now" || fail 5 "the current time is not UTC written as YYYY-MM-DDTHH:MM:SSZ"

# Private copies: nothing below reads <dir> or <state-dir> again until step 6 writes to it.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
man=$work/manifest.json sig=$work/manifest.json.sig st=$work/state.json
cp "$dir/manifest.json" "$man" || fail 1 "cannot read $dir/manifest.json"
cp "$dir/manifest.json.sig" "$sig" || fail 1 "cannot read $dir/manifest.json.sig"
[ "$(wc -c < "$man")" -le 1048576 ] || fail 1 "manifest.json is over 1 MiB"
[ "$(wc -c < "$sig")" -le 1024 ] || fail 1 "manifest.json.sig is over 1 KiB"

# The state: a missing state directory or state file is a first run; any other read problem is a failure.
if [ ! -e "$state_dir" ]; then
  echo '{}' > "$st"
elif [ -d "$state_dir" ] && [ -x "$state_dir" ] && [ ! -e "$state_dir/state.json" ] && [ ! -L "$state_dir/state.json" ]; then
  echo '{}' > "$st"
else
  cp "$state_dir/state.json" "$st" || fail 6 "cannot read $state_dir/state.json"
fi
jq -e 'type == "object"
  and ((.highest_manifest_version // 0) | type == "number" and . >= 0 and . == floor)
  and ((.revoked_keys // []) | type == "array" and all(type == "string"))
  and ((.last_manifest_sha256 // "") | type == "string")' "$st" > /dev/null 2>&1 \
  || fail 6 "$state_dir/state.json is not a state file"

# Step 2. The blob is 74 bytes = 148 hex digits: alg (4) + key id (16) + signature.
[ -r "$pinned" ] || fail 2 "cannot read the pinned keys $pinned"
blob=$(sed -n 2p "$sig" | base64 -d | od -An -v -tx1 | tr -d ' \n') || fail 2 "signature line is not base64"
[ "${#blob}" -eq 148 ] || fail 2 "signature blob is not 74 bytes"
[ "$(printf %s "$blob" | cut -c1-4)" = 4544 ] || fail 2 "signature algorithm is not ED (hashed)"
id=$(printf %s "$blob" | cut -c5-20)
if jq -e --arg id "$id" '(.revoked_keys // []) | index($id)' "$st" > /dev/null; then
  fail 2 "signing key $id is revoked"
fi
found=
while read -r label key; do
  case $label in ''|'#'*) continue ;; esac
  kid=$(printf %s "$key" | base64 -d | od -An -v -tx1 | tr -d ' \n' | cut -c5-20)
  if [ "$kid" = "$id" ]; then found=$key; fi
done < "$pinned"
[ -n "$found" ] || fail 2 "signing key $id is not pinned"
minisign -V -q -m "$man" -x "$sig" -P "$found" || fail 2 "signature does not verify"

# Step 3.
jq -e '.schema_version == 1 and .product == "nevr-runtime"' "$man" > /dev/null \
  || fail 3 "schema_version is not 1 or product is not nevr-runtime"
jq -e '(.manifest_version | type == "number" and . >= 0 and . == floor)' "$man" > /dev/null \
  || fail 3 "manifest_version is not a whole number"

# Step 4. The equal-version test applies only when a manifest was stored at that version.
mv=$(jq -r .manifest_version "$man")
hi=$(jq -r '.highest_manifest_version // 0' "$st")
last=$(jq -r '.last_manifest_sha256 // ""' "$st")
have=$hi
if [ "$MIN_MANIFEST_VERSION" -gt "$have" ]; then have=$MIN_MANIFEST_VERSION; fi
sha=$(sha256sum "$man" | cut -d' ' -f1)
[ "$mv" -ge "$have" ] || fail 4 "manifest version went down: got $mv, have $have"
if [ "$mv" -eq "$hi" ] && [ "$hi" -gt 0 ] && [ "$sha" != "$last" ]; then
  fail 4 "same manifest_version, different bytes"
fi

# Step 5. Both times are whole-second UTC with a Z suffix, so a string comparison is a time comparison.
jq -e '.expires | type == "string" and test("\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\\z")' "$man" > /dev/null \
  || fail 5 "expires is not UTC written as YYYY-MM-DDTHH:MM:SSZ"
expires=$(jq -r .expires "$man")
utc "$expires" || fail 5 "expires is not a real UTC time"
jq -e --arg now "$now" '.expires > $now' "$man" > /dev/null \
  || fail 5 "update information is stale since $expires; check the clock or try later"

# Step 6. A revocation list that is present has to be a list of strings: a malformed one is never skipped.
jq -e '(.revoked_keys // []) | type == "array" and all(type == "string")' "$man" > /dev/null \
  || fail 6 "revoked_keys in the manifest is not a list of strings"
# Step 6. Three renames, the state last; a crash in between leaves a manifest whose hash is not in state.json.
jq -n --slurpfile m "$man" --slurpfile s "$st" --arg sha "$sha" '{
  highest_manifest_version: $m[0].manifest_version,
  revoked_keys: ((($s[0].revoked_keys // []) + ($m[0].revoked_keys // [])) | unique),
  last_manifest_sha256: $sha }' > "$work/state.new"
(umask 077; mkdir -p "$state_dir") || fail 6 "cannot create $state_dir"
cp "$man" "$state_dir/manifest.json.new" && cp "$sig" "$state_dir/manifest.json.sig.new" \
  && cp "$work/state.new" "$state_dir/state.json.new" || fail 6 "cannot write to $state_dir"
mv -f "$state_dir/manifest.json.new" "$state_dir/manifest.json" \
  && mv -f "$state_dir/manifest.json.sig.new" "$state_dir/manifest.json.sig" \
  && mv -f "$state_dir/state.json.new" "$state_dir/state.json" || fail 6 "cannot replace the files in $state_dir"

# Step 7. A channel name can hold anything, so the version it names is checked for shape before it is used.
jq -e --arg c "$channel" '.channels | type == "object" and has($c)' "$man" > /dev/null \
  || fail 7 "channel $channel has no release"
jq -e --arg c "$channel" '.channels[$c] | type == "string" and test("\\A(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z")' "$man" > /dev/null \
  || fail 7 "channel $channel names a version that is not X.Y.Z"
ver=$(jq -r --arg c "$channel" '.channels[$c]' "$man")
jq -e --arg v "$ver" '.releases | type == "object" and has($v)' "$man" > /dev/null \
  || fail 7 "channel $channel names $ver, which is not in releases"
echo "OK signed by key id $id; manifest_version $mv; channel $channel -> $ver"
jq -r --arg v "$ver" '.releases[$v].artifacts[] | "  \(.kind)\t\(.path)\t\(.size)\t\(.sha256)"' "$man"
```

Run it:

```sh
sh verify-manifest.sh <dir> pinned-keys.txt <state-dir> "$(date -u +%Y-%m-%dT%H:%M:%SZ)" beta
```

### Rust (`minisign-verify`)

`minisign-verify` 0.3.0 checks the key id, the signature and the trusted-comment signature in one call, and its third
argument, `allow_legacy`, is `false` to refuse `Ed`. The program below runs the same steps as the scripts, with the same
arguments and the same `FAIL step N` verdicts. Every dependency is pinned to an exact version (`=`). Commit the
`Cargo.lock` that `cargo` writes next to `Cargo.toml` and build with `cargo build --locked --release`, so a build never
resolves a newer crate than the one you reviewed. `ureq` is built without gzip: the reader that enforces a size limit
then sees exactly the bytes on the wire.

```toml Cargo.toml
[package]
name = "manifestcheck"
version = "0.1.0"
edition = "2021"

[dependencies]
base64 = "=0.22.1"
minisign-verify = "=0.3.0"
serde_json = "=1.0.154"
sha2 = "=0.10.9"
ureq = { version = "=2.12.1", default-features = false, features = ["tls"] }
```

```rust src/main.rs
use base64::{engine::general_purpose::STANDARD, Engine};
use minisign_verify::{PublicKey, Signature};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::{ErrorKind, Read, Write};
use std::{env, fs, process};

/// The manifest_version that was current when this installer was built. The integrator sets it in the source and
/// raises it with every release of the installer; nothing at run time can lower it.
const MIN_MANIFEST_VERSION: u64 = 1;

fn fail(step: u32, msg: &str) -> ! {
    eprintln!("FAIL step {step}: {msg}");
    process::exit(1)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// A key blob and a signature blob both start `alg(2) || key_id(8)`. The id is returned in file order.
fn key_id(blob: &[u8]) -> String {
    hex(&blob[2..10])
}

fn revoked(v: &Value) -> Vec<Value> {
    v["revoked_keys"].as_array().cloned().unwrap_or_default()
}

/// Exactly YYYY-MM-DDTHH:MM:SSZ, and a real calendar time. Nothing else, so no trailing newline either.
fn utc_ok(s: &str) -> bool {
    let b = s.as_bytes();
    let digits = |r: std::ops::Range<usize>| b.len() == 20 && b[r].iter().all(u8::is_ascii_digit);
    if !(digits(0..4) && b[4] == b'-' && digits(5..7) && b[7] == b'-' && digits(8..10) && b[10] == b'T'
        && digits(11..13) && b[13] == b':' && digits(14..16) && b[16] == b':' && digits(17..19) && b[19] == b'Z')
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

fn semver_ok(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    parts.len() == 3
        && parts.iter().all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()) && (*p == "0" || !p.starts_with('0')))
}

/// `v<version>/nevr-runtime-v<version>-<name>`, the name made of a-z 0-9 . - only: one version directory, one file.
fn path_ok(version: &str, path: &str) -> bool {
    semver_ok(version)
        && path
            .strip_prefix(&format!("v{version}/nevr-runtime-v{version}-"))
            .map_or(false, |name| !name.is_empty() && name.bytes().all(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'.' | b'-')))
}

fn is_sha256_hex(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// state.json: a missing file (or directory) is a first run; any other read problem, or a file that does not hold what
/// it should, is a failure. An unreadable state must not turn into an empty one.
fn read_state(dir: &str) -> Value {
    let path = format!("{dir}/state.json");
    let state = match fs::read(&path) {
        Ok(b) => serde_json::from_slice::<Value>(&b).unwrap_or_else(|e| fail(6, &format!("{path}: {e}"))),
        Err(e) if e.kind() == ErrorKind::NotFound => json!({}),
        Err(e) => fail(6, &format!("{path}: {e}")),
    };
    let ok = state.is_object()
        && state.get("highest_manifest_version").map_or(true, Value::is_u64)
        && state.get("last_manifest_sha256").map_or(true, Value::is_string)
        && state.get("revoked_keys").map_or(true, |r| r.as_array().map_or(false, |a| a.iter().all(Value::is_string)));
    if !ok {
        fail(6, &format!("{path} is not a state file"));
    }
    state
}

/// A temporary file renamed over the old one, so a reader sees the old file or the new one and never half of it.
fn write_atomic(path: &str, bytes: &[u8]) {
    let tmp = format!("{path}.new");
    fs::write(&tmp, bytes)
        .and_then(|_| fs::rename(&tmp, path))
        .unwrap_or_else(|e| fail(6, &format!("{path}: {e}")));
}

/// A failure that also removes a partly written download.
fn stop(step: u32, cleanup: Option<&str>, msg: &str) -> ! {
    if let Some(path) = cleanup {
        let _ = fs::remove_file(path);
    }
    fail(step, msg)
}

/// Reads at most `max` bytes from `url` into `sink` and returns (bytes, sha256). More than `max` is a failure, whether
/// or not the server announced a length; redirects are not followed. `cleanup` is removed on any failure.
fn fetch_capped(step: u32, url: &str, max: u64, sink: &mut dyn Write, cleanup: Option<&str>) -> (u64, String) {
    let response = ureq::AgentBuilder::new()
        .redirects(0)
        .build()
        .get(url)
        .call()
        .unwrap_or_else(|e| stop(step, cleanup, &format!("could not fetch {url}: {e}")));
    if response.status() != 200 {
        stop(step, cleanup, &format!("{url}: HTTP status {}", response.status()));
    }
    let mut reader = response.into_reader().take(max + 1);
    let (mut hasher, mut size, mut buf) = (Sha256::new(), 0u64, [0u8; 65536]);
    loop {
        let n = reader.read(&mut buf).unwrap_or_else(|e| stop(step, cleanup, &format!("{url}: {e}")));
        if n == 0 {
            break;
        }
        if size + n as u64 > max {
            stop(step, cleanup, &format!("the server sent more than {max} bytes"));
        }
        sink.write_all(&buf[..n]).unwrap_or_else(|e| stop(step, cleanup, &format!("write: {e}")));
        hasher.update(&buf[..n]);
        size += n as u64;
    }
    (size, hex(&hasher.finalize()))
}

fn main() {
    let a: Vec<String> = env::args().collect();
    match (a.get(1).map(String::as_str), a.len()) {
        (Some("fetch"), 4) => fetch(&a[2], &a[3]),
        (Some("manifest"), 7) => manifest(&a[2], &a[3], &a[4], &a[5], &a[6]),
        (Some("artifact"), 7) => artifact(&a[2], &a[3], &a[4], &a[5], &a[6]),
        _ => fail(0, "usage: manifestcheck fetch <base-url> <dir> | manifestcheck manifest <dir> <pinned-keys.txt> <state-dir> <now> <channel> | manifestcheck artifact <state-dir> <version> <kind> <base-url> <out-file>"),
    }
}

/// Step 1.
fn fetch(base: &str, dir: &str) {
    for (name, max) in [("manifest.json", 1u64 << 20), ("manifest.json.sig", 1024)] {
        let mut body = Vec::new();
        fetch_capped(1, &format!("{base}/{name}"), max, &mut body, None);
        fs::write(format!("{dir}/{name}"), body).unwrap_or_else(|e| fail(1, &format!("{dir}/{name}: {e}")));
    }
}

/// Steps 2 to 7.
fn manifest(dir: &str, pinned: &str, state_dir: &str, now: &str, channel: &str) {
    if !utc_ok(now) {
        fail(5, "the current time is not UTC written as YYYY-MM-DDTHH:MM:SSZ");
    }
    // Each file is read once; steps 2 to 7 use these bytes and never open the path again.
    let read = |name: &str| fs::read(format!("{dir}/{name}")).unwrap_or_else(|e| fail(1, &format!("{name}: {e}")));
    let (bytes, sig_bytes) = (read("manifest.json"), read("manifest.json.sig"));
    if bytes.len() > 1 << 20 || sig_bytes.len() > 1024 {
        fail(1, "manifest.json is over 1 MiB or manifest.json.sig is over 1 KiB");
    }
    let sig_text = String::from_utf8(sig_bytes).unwrap_or_else(|_| fail(2, "signature file is not UTF-8"));
    let state = read_state(state_dir);

    // Step 2: before any JSON parser sees the manifest.
    let blob = sig_text.lines().nth(1).and_then(|l| STANDARD.decode(l).ok()).unwrap_or_default();
    if blob.len() != 74 {
        fail(2, "signature blob is not 74 bytes");
    }
    if &blob[..2] != b"ED" {
        fail(2, "signature algorithm is not ED (hashed)");
    }
    let id = key_id(&blob);
    if revoked(&state).contains(&json!(id)) {
        fail(2, &format!("signing key {id} is revoked"));
    }
    let keys = fs::read_to_string(pinned).unwrap_or_else(|e| fail(2, &format!("{pinned}: {e}")));
    let key_b64 = keys
        .lines()
        .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
        .filter_map(|l| l.split_whitespace().nth(1))
        .find(|k| STANDARD.decode(k).map_or(false, |b| b.len() == 42 && key_id(&b) == id))
        .unwrap_or_else(|| fail(2, &format!("signing key {id} is not pinned")));
    let key = PublicKey::from_base64(key_b64).unwrap_or_else(|e| fail(2, &format!("pinned key: {e}")));
    let sig = Signature::decode(&sig_text).unwrap_or_else(|e| fail(2, &format!("signature file: {e}")));
    key.verify(&bytes, &sig, false)
        .unwrap_or_else(|e| fail(2, &format!("signature does not verify: {e}")));

    // Step 3: the same bytes, now parsed.
    let m: Value = serde_json::from_slice(&bytes).unwrap_or_else(|e| fail(3, &format!("manifest.json: {e}")));
    if m["schema_version"] != 1 || m["product"] != "nevr-runtime" {
        fail(3, "schema_version is not 1 or product is not nevr-runtime");
    }

    // Step 4: the stored highest and the compiled minimum. The equal-version test applies only when a manifest was
    // stored at that version.
    let version = m["manifest_version"].as_u64().unwrap_or_else(|| fail(3, "manifest_version is not a whole number"));
    let stored = state["highest_manifest_version"].as_u64().unwrap_or(0);
    let have = stored.max(MIN_MANIFEST_VERSION);
    let sha = hex(&Sha256::digest(&bytes));
    if version < have {
        fail(4, &format!("manifest version went down: got {version}, have {have}"));
    }
    if version == stored && stored > 0 && state["last_manifest_sha256"] != json!(sha) {
        fail(4, "same manifest_version, different bytes");
    }

    // Step 5: whole-second UTC strings with a Z suffix compare as times.
    let expires = m["expires"].as_str().filter(|e| utc_ok(e)).unwrap_or_else(|| fail(5, "expires is not UTC written as YYYY-MM-DDTHH:MM:SSZ"));
    if now >= expires {
        fail(5, &format!("update information is stale since {expires}; check the clock or try later"));
    }

    // Step 6: a revocation list that is present has to be a list of strings, never skipped; then the manifest and its
    // signature, and the state last.
    if !m.get("revoked_keys").map_or(true, |r| r.as_array().map_or(false, |a| a.iter().all(Value::is_string))) {
        fail(6, "revoked_keys in the manifest is not a list of strings");
    }
    let mut all = revoked(&state);
    for k in revoked(&m) {
        if !all.contains(&k) {
            all.push(k);
        }
    }
    all.sort_by(|a, b| a.as_str().cmp(&b.as_str()));
    let stored_state = json!({ "highest_manifest_version": version, "revoked_keys": all, "last_manifest_sha256": sha });
    fs::create_dir_all(state_dir).unwrap_or_else(|e| fail(6, &format!("{state_dir}: {e}")));
    write_atomic(&format!("{state_dir}/manifest.json"), &bytes);
    write_atomic(&format!("{state_dir}/manifest.json.sig"), sig_text.as_bytes());
    write_atomic(&format!("{state_dir}/state.json"), &serde_json::to_vec_pretty(&stored_state).expect("state serializes"));

    // Step 7.
    let release = m["channels"][channel].as_str().unwrap_or_else(|| fail(7, &format!("channel {channel} has no release")));
    if !semver_ok(release) {
        fail(7, &format!("channel {channel} names a version that is not X.Y.Z"));
    }
    if m["releases"][release].is_null() {
        fail(7, &format!("channel {channel} names {release}, which is not in releases"));
    }
    println!("OK signed by key id {id}; manifest_version {version}; channel {channel} -> {release}");
}

/// The manifest that passed step 2, read back from the state directory. It has to be the one `state.json` names.
fn stored_manifest(state_dir: &str) -> Value {
    let state = read_state(state_dir);
    let want = state["last_manifest_sha256"].as_str().unwrap_or("");
    let bytes = fs::read(format!("{state_dir}/manifest.json"))
        .unwrap_or_else(|e| fail(8, &format!("no verified manifest in {state_dir}: {e}")));
    if want.is_empty() || hex(&Sha256::digest(&bytes)) != want {
        fail(8, "the stored manifest is not the one that passed step 2");
    }
    serde_json::from_slice(&bytes).unwrap_or_else(|e| fail(8, &format!("stored manifest: {e}")))
}

/// Step 8 for one file: the path is checked before any request, at most `size` bytes are read, then size and sha256.
fn artifact(state_dir: &str, version: &str, kind: &str, base: &str, out: &str) {
    let m = stored_manifest(state_dir);
    let entries: Vec<&Value> = m["releases"][version]["artifacts"]
        .as_array()
        .map(|a| a.iter().filter(|x| x["kind"] == kind).collect())
        .unwrap_or_default();
    if entries.len() != 1 {
        fail(8, &format!("release {version} has no {kind} artifact, or lists it twice"));
    }
    let entry = entries[0];
    let path = entry["path"].as_str().unwrap_or("");
    if !path_ok(version, path) {
        fail(8, "path in the manifest is not v<version>/nevr-runtime-v<version>-<name>");
    }
    let size = entry["size"].as_u64().unwrap_or_else(|| fail(8, "size in the manifest is not a whole number"));
    let want_sha = entry["sha256"].as_str().filter(|s| is_sha256_hex(s)).unwrap_or_else(|| fail(8, "sha256 in the manifest is not 64 hex digits"));

    let mut file = fs::File::create(out).unwrap_or_else(|e| fail(8, &format!("{out}: {e}")));
    let (got, sha) = fetch_capped(8, &format!("{base}/{path}"), size, &mut file, Some(out));
    drop(file);
    if got != size {
        let _ = fs::remove_file(out);
        fail(8, &format!("size is {got}, the manifest says {size}"));
    }
    if sha != want_sha {
        let _ = fs::remove_file(out);
        fail(8, "sha256 differs from the manifest");
    }
    println!("OK step 8: {out} matches {kind} of release {version} (size {size}, sha256 {sha})");
}
```

```sh
cargo build --locked --release
./target/release/manifestcheck manifest <dir> pinned-keys.txt <state-dir> "$(date -u +%Y-%m-%dT%H:%M:%SZ)" beta
```

### C# (netstandard2.0, `BouncyCastle.Cryptography`)

The .NET base library has no Ed25519 and no BLAKE2b, so the library below uses `Ed25519Signer` and `Blake2bDigest` from
BouncyCastle, `System.Security.Cryptography.SHA256` for the file hashes and `HttpClient` for the capped downloads. It
targets netstandard2.0 so the same file builds for a desktop app and for Unity. It compiles and runs under .NET SDK 10.
Running it under Unity IL2CPP on a Quest is not shown here.

Both packages are pinned to one exact version (the bracketed `[x.y.z]` range is NuGet's "exactly this version"), and
`RestorePackagesWithLockFile` makes `dotnet restore` write a `packages.lock.json` beside each project. Commit those
files and restore with `dotnet restore --locked-mode`, so a build never resolves a package you did not review.

```xml NevrManifest/NevrManifest.csproj
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>netstandard2.0</TargetFramework>
    <LangVersion>9.0</LangVersion>
    <Nullable>disable</Nullable>
    <RestorePackagesWithLockFile>true</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="BouncyCastle.Cryptography" Version="[2.7.0]" />
    <PackageReference Include="System.Text.Json" Version="[8.0.6]" />
  </ItemGroup>
</Project>
```

```csharp NevrManifest/ManifestCheck.cs
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Org.BouncyCastle.Crypto.Digests;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Crypto.Signers;

namespace Nevr.Manifest
{
    /// <summary>A check failed. <see cref="Step"/> is the step number of the order of checks.</summary>
    public sealed class Rejected : Exception
    {
        public int Step { get; }
        public Rejected(int step, string message) : base(message) { Step = step; }
    }

    /// <summary>What to keep between runs (step 6), as state.json.</summary>
    public sealed class State
    {
        public long Highest;
        public HashSet<string> Revoked = new HashSet<string>();
        public string LastSha256 = "";

        /// <summary>Reads state.json. Anything that is not what <see cref="ToJson"/> writes is <see cref="Rejected"/> at step 6.</summary>
        public static State Parse(string json)
        {
            try
            {
                using (var doc = JsonDocument.Parse(json))
                {
                    var root = doc.RootElement;
                    if (root.ValueKind != JsonValueKind.Object) throw new Rejected(6, "state.json is not an object");
                    var state = new State();
                    if (root.TryGetProperty("highest_manifest_version", out var h)
                        && (h.ValueKind != JsonValueKind.Number || !h.TryGetInt64(out state.Highest) || state.Highest < 0))
                        throw new Rejected(6, "highest_manifest_version is not a whole number");
                    if (root.TryGetProperty("last_manifest_sha256", out var l))
                    {
                        if (l.ValueKind != JsonValueKind.String) throw new Rejected(6, "last_manifest_sha256 is not a string");
                        state.LastSha256 = l.GetString();
                    }
                    if (root.TryGetProperty("revoked_keys", out var r))
                    {
                        if (r.ValueKind != JsonValueKind.Array) throw new Rejected(6, "revoked_keys is not an array");
                        foreach (var k in r.EnumerateArray())
                        {
                            if (k.ValueKind != JsonValueKind.String) throw new Rejected(6, "revoked_keys holds a value that is not a string");
                            state.Revoked.Add(k.GetString());
                        }
                    }
                    return state;
                }
            }
            catch (JsonException e) { throw new Rejected(6, "state.json: " + e.Message); }
        }

        public byte[] ToJson()
        {
            using (var stream = new MemoryStream())
            {
                using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
                {
                    writer.WriteStartObject();
                    writer.WriteNumber("highest_manifest_version", Highest);
                    writer.WriteStartArray("revoked_keys");
                    foreach (var k in Revoked.OrderBy(k => k, StringComparer.Ordinal)) writer.WriteStringValue(k);
                    writer.WriteEndArray();
                    writer.WriteString("last_manifest_sha256", LastSha256);
                    writer.WriteEndObject();
                }
                return stream.ToArray();
            }
        }
    }

    /// <summary>One artifact of a release, with every field checked for shape.</summary>
    public sealed class Artifact
    {
        public string Path;
        public long Size;
        public string Sha256;
    }

    public static class ManifestCheck
    {
        const string TrustedPrefix = "trusted comment: ";
        static readonly Regex Utc = new Regex(@"\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\z", RegexOptions.CultureInvariant);
        static readonly Regex SemVer = new Regex(@"\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z", RegexOptions.CultureInvariant);
        static readonly Regex FileName = new Regex(@"\A[a-z0-9.-]+\z", RegexOptions.CultureInvariant);
        static readonly Regex Sha256Hex = new Regex(@"\A[0-9a-f]{64}\z", RegexOptions.CultureInvariant);

        static string Hex(byte[] b) => string.Concat(b.Select(x => x.ToString("x2")));

        /// <summary>Exactly YYYY-MM-DDTHH:MM:SSZ and a real calendar time: no other character, no trailing newline.</summary>
        public static bool UtcOk(string s) => s != null && Utc.IsMatch(s) &&
            DateTime.TryParseExact(s, "yyyy-MM-dd'T'HH:mm:ss'Z'", System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.None, out _);

        static bool Ed25519(byte[] publicKey, byte[] message, byte[] signature)
        {
            var verifier = new Ed25519Signer();
            verifier.Init(false, new Ed25519PublicKeyParameters(publicKey, 0));
            verifier.BlockUpdate(message, 0, message.Length);
            return verifier.VerifySignature(signature);
        }

        static byte[] Base64(string text, string what)
        {
            try { return Convert.FromBase64String(text); }
            catch (FormatException) { throw new Rejected(2, what + " is not base64"); }
        }

        /// <summary>
        /// Steps 2 to 6. <paramref name="pinned"/> holds the base64 keys of pinned-keys.txt, <paramref name="now"/> is
        /// UTC as YYYY-MM-DDTHH:MM:SSZ, <paramref name="minimumVersion"/> is the manifest_version that was current when
        /// this installer was built (the integrator's constant). On success <paramref name="state"/> holds what step 6
        /// stores (the caller writes it down) and the parsed manifest is returned. Any failure, including a field of
        /// the wrong type, throws <see cref="Rejected"/>; nothing else escapes.
        /// </summary>
        public static JsonDocument Verify(byte[] manifest, string sigText, IEnumerable<string> pinned, State state, string now, long minimumVersion)
        {
            var at = new[] { 5 };
            var holder = new JsonDocument[1];
            try { return VerifyCore(manifest, sigText, pinned, state, now, minimumVersion, at, holder); }
            catch (Rejected) { holder[0]?.Dispose(); throw; }
            catch (Exception e) when (e is InvalidOperationException || e is KeyNotFoundException || e is FormatException
                || e is ArgumentException || e is OverflowException || e is InvalidCastException || e is JsonException)
            {
                holder[0]?.Dispose();
                throw new Rejected(at[0], "manifest.json does not have the expected shape (" + e.GetType().Name + ")");
            }
        }

        static JsonDocument VerifyCore(byte[] manifest, string sigText, IEnumerable<string> pinned, State state, string now,
            long minimumVersion, int[] at, JsonDocument[] holder)
        {
            if (!UtcOk(now)) throw new Rejected(5, "the current time is not UTC written as YYYY-MM-DDTHH:MM:SSZ");
            at[0] = 1;
            if (manifest.Length > 1 << 20 || sigText.Length > 1024)
                throw new Rejected(1, "manifest.json is over 1 MiB or manifest.json.sig is over 1 KiB");

            // Step 2: before any JSON parser sees the manifest.
            at[0] = 2;
            var lines = sigText.Replace("\r", "").Split('\n');
            if (lines.Length < 4 || !lines[2].StartsWith(TrustedPrefix, StringComparison.Ordinal))
                throw new Rejected(2, "signature file is not a minisign signature");
            var blob = Base64(lines[1], "signature line");
            if (blob.Length != 74) throw new Rejected(2, "signature blob is not 74 bytes");
            if (blob[0] != (byte)'E' || blob[1] != (byte)'D') throw new Rejected(2, "signature algorithm is not ED (hashed)");
            var id = Hex(blob.Skip(2).Take(8).ToArray());
            var signature = blob.Skip(10).ToArray();
            if (state.Revoked.Contains(id)) throw new Rejected(2, "signing key " + id + " is revoked");

            byte[] key = null;
            foreach (var b64 in pinned)
            {
                var k = Base64(b64, "pinned key");
                if (k.Length == 42 && Hex(k.Skip(2).Take(8).ToArray()) == id) key = k.Skip(10).ToArray();
            }
            if (key == null) throw new Rejected(2, "signing key " + id + " is not pinned");

            var blake = new Blake2bDigest(512);
            blake.BlockUpdate(manifest, 0, manifest.Length);
            var digest = new byte[64];
            blake.DoFinal(digest, 0);
            if (!Ed25519(key, digest, signature)) throw new Rejected(2, "signature does not verify");
            var global = Base64(lines[3], "trusted comment signature");
            var comment = Encoding.UTF8.GetBytes(lines[2].Substring(TrustedPrefix.Length));
            if (global.Length != 64 || !Ed25519(key, signature.Concat(comment).ToArray(), global))
                throw new Rejected(2, "signature does not verify (trusted comment)");

            // Step 3: the same bytes, now parsed.
            at[0] = 3;
            JsonDocument doc;
            try { doc = JsonDocument.Parse(manifest); }
            catch (JsonException e) { throw new Rejected(3, "manifest.json: " + e.Message); }
            holder[0] = doc;
            var root = doc.RootElement;
            if (!root.TryGetProperty("schema_version", out var schema) || !schema.TryGetInt32(out var schemaValue) || schemaValue != 1
                || !root.TryGetProperty("product", out var product) || product.ValueKind != JsonValueKind.String || product.GetString() != "nevr-runtime")
                throw new Rejected(3, "schema_version is not 1 or product is not nevr-runtime");
            if (!root.TryGetProperty("manifest_version", out var versionElement) || !versionElement.TryGetInt64(out var version) || version < 0)
                throw new Rejected(3, "manifest_version is not a whole number");

            // Step 4: the stored highest and the compiled minimum. The equal-version test applies only when a manifest
            // was stored at that version.
            at[0] = 4;
            var have = Math.Max(state.Highest, minimumVersion);
            string sha;
            using (var sha256 = SHA256.Create()) sha = Hex(sha256.ComputeHash(manifest));
            if (version < have)
                throw new Rejected(4, "manifest version went down: got " + version + ", have " + have);
            if (version == state.Highest && state.Highest > 0 && sha != state.LastSha256)
                throw new Rejected(4, "same manifest_version, different bytes");

            // Step 5: whole-second UTC strings with a Z suffix compare as times.
            at[0] = 5;
            if (!root.TryGetProperty("expires", out var expiresElement) || expiresElement.ValueKind != JsonValueKind.String
                || !UtcOk(expiresElement.GetString()))
                throw new Rejected(5, "expires is not UTC written as YYYY-MM-DDTHH:MM:SSZ");
            var expires = expiresElement.GetString();
            if (string.CompareOrdinal(now, expires) >= 0)
                throw new Rejected(5, "update information is stale since " + expires + "; check the clock or try later");

            // Step 6: the caller writes it down, the manifest and its signature first and the state last.
            at[0] = 6;
            var revokedNow = new List<string>();
            if (root.TryGetProperty("revoked_keys", out var revoked))
                foreach (var k in revoked.EnumerateArray())
                {
                    if (k.ValueKind != JsonValueKind.String) throw new Rejected(6, "revoked_keys holds a value that is not a string");
                    revokedNow.Add(k.GetString());
                }
            state.Highest = version;
            state.LastSha256 = sha;
            foreach (var k in revokedNow) state.Revoked.Add(k);
            return doc;
        }

        /// <summary>Step 7: the release version the channel names, which must be a key of releases.</summary>
        public static string ChannelVersion(JsonElement manifest, string channel)
        {
            try
            {
                if (!manifest.GetProperty("channels").TryGetProperty(channel, out var v))
                    throw new Rejected(7, "channel " + channel + " has no release");
                if (v.ValueKind != JsonValueKind.String || !SemVer.IsMatch(v.GetString()))
                    throw new Rejected(7, "channel " + channel + " names a version that is not X.Y.Z");
                var version = v.GetString();
                if (!manifest.GetProperty("releases").TryGetProperty(version, out _))
                    throw new Rejected(7, "channel " + channel + " names " + version + ", which is not in releases");
                return version;
            }
            catch (Exception e) when (e is InvalidOperationException || e is KeyNotFoundException) { throw new Rejected(7, "channels or releases has the wrong shape"); }
        }

        /// <summary>
        /// Step 8, the first half: the manifest that passed step 2, read back from the state directory, has to be the
        /// one state.json names. Returns it parsed.
        /// </summary>
        public static JsonDocument StoredManifest(byte[] stored, State state)
        {
            string sha;
            using (var sha256 = SHA256.Create()) sha = Hex(sha256.ComputeHash(stored));
            if (state.LastSha256.Length == 0 || sha != state.LastSha256)
                throw new Rejected(8, "the stored manifest is not the one that passed step 2");
            try { return JsonDocument.Parse(stored); }
            catch (JsonException e) { throw new Rejected(8, "stored manifest: " + e.Message); }
        }

        /// <summary>
        /// Step 8, the second half: the one artifact of that kind in that release, with its path checked before any
        /// request: one version directory named for the release and one file name, nothing else.
        /// </summary>
        public static Artifact FindArtifact(JsonElement manifest, string version, string kind)
        {
            try
            {
                if (!SemVer.IsMatch(version)) throw new Rejected(8, "version " + version + " is not X.Y.Z");
                if (!manifest.GetProperty("releases").TryGetProperty(version, out var release))
                    throw new Rejected(8, "release " + version + " is not in the manifest");
                var matches = release.GetProperty("artifacts").EnumerateArray()
                    .Where(a => a.GetProperty("kind").GetString() == kind).ToList();
                if (matches.Count != 1) throw new Rejected(8, "release " + version + " has no " + kind + " artifact, or lists it twice");
                var entry = matches[0];
                var path = entry.GetProperty("path").GetString();
                var prefix = "v" + version + "/nevr-runtime-v" + version + "-";
                if (path == null || !path.StartsWith(prefix, StringComparison.Ordinal) || !FileName.IsMatch(path.Substring(prefix.Length)))
                    throw new Rejected(8, "path in the manifest is not v<version>/nevr-runtime-v<version>-<name>");
                if (!entry.GetProperty("size").TryGetInt64(out var size) || size < 0)
                    throw new Rejected(8, "size in the manifest is not a whole number");
                var sha = entry.GetProperty("sha256").GetString();
                if (sha == null || !Sha256Hex.IsMatch(sha)) throw new Rejected(8, "sha256 in the manifest is not 64 hex digits");
                return new Artifact { Path = path, Size = size, Sha256 = sha };
            }
            catch (Exception e) when (e is InvalidOperationException || e is KeyNotFoundException || e is FormatException)
            {
                throw new Rejected(8, "the manifest does not have the expected shape (" + e.GetType().Name + ")");
            }
        }

        /// <summary>
        /// Reads at most <paramref name="max"/> bytes of <paramref name="url"/> into <paramref name="sink"/> and returns
        /// their count and sha256. More than that is a failure, whether or not the server announced a length. The
        /// <see cref="HttpClient"/> must not follow redirects.
        /// </summary>
        public static (long Size, string Sha256) Fetch(HttpClient http, string url, long max, Stream sink, int step)
        {
            try
            {
                using (var response = http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult())
                {
                    if ((int)response.StatusCode != 200) throw new Rejected(step, url + ": HTTP status " + (int)response.StatusCode);
                    using (var body = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
                    using (var sha256 = SHA256.Create())
                    {
                        var buffer = new byte[65536];
                        long total = 0;
                        while (true)
                        {
                            var n = body.Read(buffer, 0, (int)Math.Min(buffer.Length, max + 1 - total));
                            if (n == 0) break;
                            total += n;
                            if (total > max) throw new Rejected(step, "the server sent more than " + max + " bytes");
                            sink.Write(buffer, 0, n);
                            sha256.TransformBlock(buffer, 0, n, null, 0);
                        }
                        sha256.TransformFinalBlock(new byte[0], 0, 0);
                        return (total, Hex(sha256.Hash));
                    }
                }
            }
            catch (Exception e) when (e is HttpRequestException || e is IOException || e is TaskCanceledException || e is InvalidOperationException)
            {
                throw new Rejected(step, "could not fetch " + url + ": " + e.Message);
            }
        }
    }
}
```

A command-line wrapper with the same arguments and the same `FAIL step N` verdicts as the scripts and the Rust program
(a .NET 10 console that references the library; this is the part that is not netstandard). It holds the integrator's
minimum `manifest_version` and does the file work: reading each file once, and writing the state directory with
temporary files that are renamed into place.

```xml Harness/Harness.csproj
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <RestorePackagesWithLockFile>true</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <ProjectReference Include="../NevrManifest/NevrManifest.csproj" />
  </ItemGroup>
</Project>
```

```csharp Harness/Program.cs
using System.Net;
using System.Text;
using System.Text.Json;
using Nevr.Manifest;

// The manifest_version that was current when this installer was built. The integrator sets it in the source and
// raises it with every release of the installer; nothing at run time can lower it.
const long MinimumManifestVersion = 1;

static byte[] ReadBytes(string path, int step)
{
    try { return File.ReadAllBytes(path); }
    catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { throw new Rejected(step, path + ": " + e.Message); }
}

// A temporary file renamed over the old one, so a reader sees the old file or the new one and never half of it.
static void WriteAtomic(string path, byte[] bytes)
{
    try
    {
        File.WriteAllBytes(path + ".new", bytes);
        File.Move(path + ".new", path, true);
    }
    catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { throw new Rejected(6, path + ": " + e.Message); }
}

// A missing state directory, or a state directory with no state.json, is a first run; any other read problem is a
// failure, never an empty state.
static State ReadState(string dir)
{
    var path = Path.Combine(dir, "state.json");
    if (!Directory.Exists(dir) && !File.Exists(dir)) return new State();
    try { return State.Parse(File.ReadAllText(path)); }
    catch (FileNotFoundException) { return new State(); }
    catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { throw new Rejected(6, path + ": " + e.Message); }
}

static void MakeDirectory(string dir)
{
    try { Directory.CreateDirectory(dir); }
    catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { throw new Rejected(6, dir + ": " + e.Message); }
}

static HttpClient NewClient() => new HttpClient(new HttpClientHandler { AllowAutoRedirect = false });

try
{
    if (args.Length == 3 && args[0] == "fetch")
    {
        // Step 1.
        using var http = NewClient();
        foreach (var (name, max) in new[] { ("manifest.json", 1L << 20), ("manifest.json.sig", 1024L) })
        {
            using var body = new MemoryStream();
            ManifestCheck.Fetch(http, args[1] + "/" + name, max, body, 1);
            File.WriteAllBytes(Path.Combine(args[2], name), body.ToArray());
        }
    }
    else if (args.Length == 6 && args[0] == "manifest")
    {
        var dir = args[1];
        var pinned = File.ReadLines(args[2]).Where(l => l.Trim().Length > 0 && !l.StartsWith("#"))
            .Select(l => l.Split(' ', StringSplitOptions.RemoveEmptyEntries)).Where(p => p.Length >= 2).Select(p => p[1]).ToList();
        var stateDir = args[3];
        var state = ReadState(stateDir);
        // Each file is read once; steps 2 to 7 use these bytes and never open the path again.
        var manifestBytes = ReadBytes(dir + "/manifest.json", 1);
        var sigBytes = ReadBytes(dir + "/manifest.json.sig", 1);
        using var doc = ManifestCheck.Verify(manifestBytes, Encoding.UTF8.GetString(sigBytes), pinned, state, args[4], MinimumManifestVersion);
        // Step 6: the manifest and its signature first, the state last.
        MakeDirectory(stateDir);
        WriteAtomic(Path.Combine(stateDir, "manifest.json"), manifestBytes);
        WriteAtomic(Path.Combine(stateDir, "manifest.json.sig"), sigBytes);
        WriteAtomic(Path.Combine(stateDir, "state.json"), state.ToJson());
        var release = ManifestCheck.ChannelVersion(doc.RootElement, args[5]);
        Console.WriteLine($"OK manifest_version {state.Highest}; channel {args[5]} -> {release}");
    }
    else if (args.Length == 6 && args[0] == "artifact")
    {
        // Step 8: from the manifest that step 6 stored, not from a path the caller names.
        var stateDir = args[1];
        var state = ReadState(stateDir);
        byte[] stored;
        try { stored = File.ReadAllBytes(Path.Combine(stateDir, "manifest.json")); }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { throw new Rejected(8, "no verified manifest in " + stateDir + ": " + e.Message); }
        using var doc = ManifestCheck.StoredManifest(stored, state);
        var artifact = ManifestCheck.FindArtifact(doc.RootElement, args[2], args[3]);
        var outPath = args[5];
        try
        {
            long size;
            string sha;
            using (var http = NewClient())
            using (var file = File.Create(outPath))
                (size, sha) = ManifestCheck.Fetch(http, args[4] + "/" + artifact.Path, artifact.Size, file, 8);
            if (size != artifact.Size) throw new Rejected(8, $"size is {size}, the manifest says {artifact.Size}");
            if (sha != artifact.Sha256) throw new Rejected(8, "sha256 differs from the manifest");
            Console.WriteLine($"OK step 8: {outPath} matches {args[3]} of release {args[2]} (size {size}, sha256 {sha})");
        }
        catch
        {
            if (File.Exists(outPath)) File.Delete(outPath);
            throw;
        }
    }
    else
    {
        Console.Error.WriteLine("usage: harness fetch <base-url> <dir> | harness manifest <dir> <pinned-keys.txt> <state-dir> <now> <channel> | harness artifact <state-dir> <version> <kind> <base-url> <out-file>");
        return 2;
    }
    return 0;
}
catch (Rejected e)
{
    Console.Error.WriteLine($"FAIL step {e.Step}: {e.Message}");
    return 1;
}
catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
{
    Console.Error.WriteLine($"FAIL step 2: {e.Message}");
    return 1;
}
```

```sh
dotnet restore --locked-mode
dotnet run --project Harness -- manifest <dir> pinned-keys.txt <state-dir> "$(date -u +%Y-%m-%dT%H:%M:%SZ)" beta
```

### Fixtures and test key

The fixtures in `docs/reference/release-verification/` are signed by a test key, id `B1DC2FE8001CB6C7` (file order
`c7b61c00e82fdcb1`), which is not one of the pinned keys and signs nothing but these fixtures. The pinned keys never
sign a fixture, an example or anything else that is not a real release. `test-key.pub` is the test key's minisign public
key file. To see the good path pass, pin the test key in place of the real ones:

```text test-pinned-keys.txt
TEST RWTHthwA6C/csRKFPrZPxjAVZFImiwbgTWoK+P0qiEHQ4mnKdzhbzOqA
```

With the real `pinned-keys.txt` the same fixtures are refused at step 2 as `not pinned`, which is the behaviour a
mirror serving a test file must get. Use a fixed `<now>` before `2026-11-07T18:00:00Z` (the fixture's `expires`) when you
run them.

## Step 3 to 7: what the script above does

These are in the script and in the programs; the rules, so you can port them:

- **Step 3.** After the signature passes, parse the same bytes. Refuse a `schema_version` other than 1 and a `product`
  other than `nevr-runtime`. Ignore members you do not know: an optional member can be added without a new
  `schema_version`, and a changed meaning gets a new one. A member that is present with the wrong type is a failure, not
  a crash: the C# library turns every such case into the same `Rejected` the other checks throw.
- **Step 4.** Let `have = max(stored highest, compiled minimum)`. A lower `manifest_version` is refused with
  `manifest version went down: got N, have M`. An equal one with different bytes is refused too when a manifest was
  stored at that version (the signer never produces it); an equal one with the same bytes means nothing is new. On a
  first run nothing is stored, so only the compiled minimum applies.
- **Step 5.** `<now>` has to be a real UTC time written exactly `YYYY-MM-DDTHH:MM:SSZ`, and so does `expires`; anything
  else (an empty value, a local time, a trailing newline) fails here instead of comparing as "earlier than everything".
  Take `<now>` from the system clock in your own code. Past `expires` you stop updating and say so. You may still launch
  what is installed.
- **Step 6.** Store only after steps 2 to 5 pass. A key in `revoked_keys` stays refused, whatever later manifests say.
  A `revoked_keys` that is present and is not a list of strings is a failure here, never skipped: a revocation that
  cannot be read must not be ignored.
- **Step 7.** `channels` maps a channel name (`stable`, `beta`) to a version of the form `X.Y.Z`. A channel that is
  absent has no release. If the version is lower than the one installed and the player did not change channel or ask to
  go back, do nothing. Install a release's artifacts as a set or not at all, and never take artifacts from two
  manifests.

## Step 8: artifacts

Step 8 never takes a manifest path from its caller. It reads the manifest that step 6 stored in the state directory, and
it requires those bytes to hash to the `last_manifest_sha256` in `state.json`, the hash of the bytes that passed step 2.
A manifest that was never verified, or that was changed after it was, is refused with `the stored manifest is not the one
that passed step 2`.

For each artifact of the chosen release that your platform handles, `kind` tells what it is: `windows-zip`,
`quest-library`, `quest-apk` (and `quest-apk-bundled` later). `plugin` and `game-data` are reserved: skip a kind you do
not handle.

1. Check the `path` before you make any request. It has to be `v<version>/nevr-runtime-v<version>-<name>` for the
   release's own version, with a name of `a-z`, `0-9`, `.` and `-` only: one version directory and one file name. The
   check is anchored at both ends, so nothing (a newline included) can follow it.
2. Build the URL as the base above plus `path` and read at most `size` bytes, whatever the server announces. More than
   `size` bytes is a failure, and the partial file is deleted.
3. Require the exact `size` and the `sha256`.

```sh stored-entry.sh
#!/bin/sh
# usage: stored-entry.sh <state-dir> <version> <kind>
# Prints the one artifact of that kind in release <version>, read from the manifest that step 6 stored in <state-dir>,
# after checking that the stored manifest is the one that passed step 2 and that the entry is well formed.
set -eu
LC_ALL=C
export LC_ALL
state_dir=$1 ver=$2 kind=$3
fail() { echo "FAIL step 8: $1" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
man=$work/manifest.json st=$work/state.json
cp "$state_dir/manifest.json" "$man" 2>/dev/null && cp "$state_dir/state.json" "$st" 2>/dev/null \
  || fail "no verified manifest in $state_dir; run verify-manifest.sh first"
want=$(jq -r '.last_manifest_sha256 // ""' "$st") || fail "$state_dir/state.json is not a state file"
[ -n "$want" ] && [ "$(sha256sum "$man" | cut -d' ' -f1)" = "$want" ] \
  || fail "the stored manifest is not the one that passed step 2"
jq -e --arg v "$ver" --arg k "$kind" '
  ($v | test("\\A(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z"))
  and ([.releases[$v].artifacts[]? | select(.kind == $k)] | length == 1)' "$man" > /dev/null \
  || fail "release $ver has no $kind artifact, or lists it twice"
jq -c --arg v "$ver" --arg k "$kind" '.releases[$v].artifacts[] | select(.kind == $k)' "$man" > "$work/entry.json"
jq -e --arg v "$ver" '
  (.path | type == "string")
  and (("v" + $v + "/nevr-runtime-v" + $v + "-") as $p
       | (.path | startswith($p)) and (.path[($p | length):] | test("\\A[a-z0-9.-]+\\z")))' "$work/entry.json" > /dev/null \
  || fail "path in the manifest is not v<version>/nevr-runtime-v<version>-<name>"
jq -e '(.size | type == "number" and . >= 0 and . == floor) and (.sha256 | type == "string" and test("\\A[0-9a-f]{64}\\z"))' \
  "$work/entry.json" > /dev/null || fail "size or sha256 in the manifest is malformed"
cat "$work/entry.json"
```

```sh check-artifact.sh
#!/bin/sh
# usage: check-artifact.sh <state-dir> <version> <kind> <base-url> <out-file>
# Step 8 for one artifact: the path is checked before any request, at most `size` bytes are read, then the exact size
# and sha256. On any failure <out-file> is removed.
set -eu
LC_ALL=C
export LC_ALL
state_dir=$1 ver=$2 kind=$3 base=$4 out=$5
here=$(dirname "$0")
fail() { rm -f "$out"; echo "FAIL step 8: $1" >&2; exit 1; }
entry=$(sh "$here/stored-entry.sh" "$state_dir" "$ver" "$kind") || exit 1
path=$(printf %s "$entry" | jq -j .path)
size=$(printf %s "$entry" | jq -r .size)
sha=$(printf %s "$entry" | jq -r .sha256)
sh "$here/fetch-capped.sh" 8 "$base/$path" "$out" "$size" || exit 1
got=$(wc -c < "$out")
[ "$got" -eq "$size" ] || fail "size is $got, the manifest says $size"
[ "$(sha256sum "$out" | cut -d' ' -f1)" = "$sha" ] || fail "sha256 differs from the manifest"
echo "OK step 8: $out matches $kind of release $ver (size $size, sha256 $sha)"
```

```sh
sh check-artifact.sh <state-dir> 5.5.1 windows-zip "$BASE" windows.zip
```

### The Windows zip: every member

The zip's `contents` lists every file entry with its own `path`, `sha256` and `size`. Extract nothing before this
passes. Every entry in the zip, directories and links included, must be listed, every listed file must be present, an
entry must not appear twice, and an entry that is not a regular file is refused. Because an entry must equal a listed
name exactly, and a listed name is a relative path with no `..` segment, a path-traversal entry cannot pass. Like the
other step 8 checks, it reads the stored manifest from the state directory.

```python check-zip.py
# usage: python3 -I check-zip.py <state-dir> <version> <zip>
import hashlib, json, os, sys, zipfile

state_dir, version, archive = sys.argv[1:4]


def fail(message):
    sys.exit("FAIL step 8: " + message)


def stored_manifest():
    """The manifest from the state directory, which has to be the one state.json names."""
    try:
        with open(os.path.join(state_dir, "state.json")) as f:
            state = json.load(f)
        with open(os.path.join(state_dir, "manifest.json"), "rb") as f:
            data = f.read()
    except (OSError, ValueError) as error:
        fail("no verified manifest in %s: %s" % (state_dir, error))
    want = state.get("last_manifest_sha256", "") if isinstance(state, dict) else ""
    if not isinstance(want, str) or not want or hashlib.sha256(data).hexdigest() != want:
        fail("the stored manifest is not the one that passed step 2")
    return json.loads(data)


releases = stored_manifest()["releases"]
found = [a for a in releases.get(version, {}).get("artifacts", []) if a.get("kind") == "windows-zip"]
if len(found) != 1:
    fail("release " + version + " has no windows-zip artifact, or lists it twice")
paths = [c["path"] for c in found[0]["contents"]]
if len(paths) != len(set(paths)):
    fail("the manifest lists an entry twice")
listed = {c["path"]: c for c in found[0]["contents"]}
with zipfile.ZipFile(archive) as z:
    infos = z.infolist()
    names = [i.filename for i in infos]
    if len(names) != len(set(names)):
        fail("an entry appears twice in the zip")
    for info in infos:
        name = info.filename
        want = listed.get(name)
        if want is None:
            fail("the zip holds " + name + ", which the manifest does not list")
        kind = (info.external_attr >> 16) & 0o170000  # 0 when the zip records no type
        if info.is_dir() or kind not in (0, 0o100000):
            fail(name + " is not a regular file")
        with z.open(info) as f:
            data = f.read(want["size"] + 1)  # one byte past the size is enough to see an oversized entry
        if len(data) != want["size"]:
            fail(name + " has a different size than the manifest says")
        if hashlib.sha256(data).hexdigest() != want["sha256"]:
            fail(name + " has a different sha256 than the manifest says")
    absent = sorted(set(listed) - set(names))
    if absent:
        fail("the manifest lists " + ", ".join(absent) + ", which the zip does not hold")
print("OK step 8: all %d entries of %s match the manifest" % (len(infos), archive))
```

In Rust the same loop is the `zip` crate's `ZipArchive::by_index`, in C# `System.IO.Compression.ZipArchive.Entries`;
both read an entry through a stream you can cap at `size + 1` bytes. The Rust and C# loops are not in this page's run.

### Windows: after the check

The sha256 of the zip and of each member is what binds the files to the release. The members `BugSplat64.dll` and
`echovr_server.exe` are also Authenticode-signed once the release has been through the signer. `SIGNING.txt` in the zip
says how to check a signature (`Get-AuthenticodeSignature .\BugSplat64.dll`: status `Valid`). A `Valid` status says
the file is signed by someone Windows trusts; the manifest hash says it is this release's file. Do both.

### Quest: the APK

An APK artifact carries an `android` object: `package` (always `com.readyatdawn.r15`), `version_code`, and optionally
`signer_cert_sha256`. After the sha256 and size pass, check the APK itself, before and after install. The script reads
the `android` object from the stored manifest, runs `aapt2` and `apksigner`, and compares:

```sh check-apk.sh
#!/bin/sh
# usage: check-apk.sh <state-dir> <version> <apk>
# Compares the APK file with the quest-apk artifact of that release in the manifest that step 6 stored.
set -eu
LC_ALL=C
export LC_ALL
state_dir=$1 ver=$2 apk=$3
here=$(dirname "$0")
fail() { echo "FAIL step 8: $1" >&2; exit 1; }
entry=$(sh "$here/stored-entry.sh" "$state_dir" "$ver" quest-apk) || exit 1
printf %s "$entry" | jq -e '.android | type == "object" and (.package | type == "string") and (.version_code | type == "number")' \
  > /dev/null || fail "the quest-apk entry has no android package and version_code"
want_pkg=$(printf %s "$entry" | jq -r .android.package)
want_code=$(printf %s "$entry" | jq -r .android.version_code)
want_certs=$(printf %s "$entry" | jq -r '[.android.signer_cert_sha256 // empty] | flatten | .[]')
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
aapt2 dump badging "$apk" > "$work/badging.txt" || fail "aapt2 could not read $apk"
pkg=$(sed -n "s/^package: name='\([^']*\)'.*/\1/p" "$work/badging.txt")
code=$(sed -n "s/^package: .* versionCode='\([0-9]*\)'.*/\1/p" "$work/badging.txt")
[ "$pkg" = "$want_pkg" ] || fail "the APK package is '$pkg', the manifest says '$want_pkg'"
[ "$code" = "$want_code" ] || fail "the APK versionCode is '$code', the manifest says '$want_code'"
apksigner verify --print-certs "$apk" > "$work/certs.txt" || fail "apksigner does not accept $apk"
digests=$(sed -n 's/.*certificate SHA-256 digest: \([0-9a-f]\{64\}\)$/\1/p' "$work/certs.txt" | sort -u)
[ -n "$digests" ] || fail "apksigner printed no signing certificate"
if [ -n "$want_certs" ]; then
  for d in $digests; do
    printf '%s\n' "$want_certs" | grep -qx "$d" || fail "the APK is signed by $d, which the manifest does not list"
  done
fi
echo "OK step 8: $apk is $pkg $code signed by: $(printf %s "$digests" | tr '\n' ' ')"
```

```sh
sh check-apk.sh <state-dir> 5.5.1 app.apk
# the installed package on the headset: version code
adb shell dumpsys package com.readyatdawn.r15 | grep versionCode
```

`apksigner` prints one digest line per signer (and per signature scheme). When the manifest has `signer_cert_sha256`,
every digest it prints has to be that value: a certificate that is not listed fails, even when a listed one is also
present. `signer_cert_sha256` is optional: when the manifest has no value, the script compares only the package name and
the version code, and does not invent one. An installer that runs on the headset reads the installed package's version
code and signing certificates through the Android package manager instead of `adb`, and applies the same rule.

The `quest-library` artifact (`nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so`) is a loose ELF with no signature of
its own: the size and sha256 are the whole check.

## What the PC host checks

The host is `BugSplat64.dll`, the file the game imports. Its source is `src/runtime/host/`. What a player's
machine does before it runs the runtime, as of this tree:

**Built**

- The host (`src/runtime/host/`) is compiled into `BugSplat64.dll` together with the runtime
  (`src/runtime/lifecycle/dllmain.cpp` passes the in-process runtime entry point to `nevr_host::OnProcessAttach`). There
  is no separate runtime file to load today, so the host has nothing of that kind to verify.
- The host checks the game image before it hooks the game's main wrapper
  (`nevr_game_image_guard::IsSupportedGameModule`, called from `src/runtime/host/host_entry.cpp`) and validates the
  wrapper's prologue bytes before it writes the hook (`RuntimeRendezvous::Install` in
  `src/runtime/host/runtime_rendezvous.cpp`).
- The host's file-load path (`nevr_host::RuntimeLoader::Load`, `src/runtime/host/runtime_load.cpp`) loads a runtime
  DLL from the host's own directory, validates the runtime's ABI descriptor (`src/core/runtime_abi.h`, ABI major
  `kMajor` = 1; a major mismatch is rejected) and then calls it. It does no signature or hash check, and the shipped
  `BugSplat64.dll` does not use this path.
- The runtime verifies **plugins**, not itself, before `LoadLibrary` (ADR 0010, `docs/adr/0010-signed-plugins-only.md`;
  `src/runtime/ext/plugin_signature.cpp`, pins in `src/runtime/ext/plugin_trust.cpp`): an embedded Authenticode
  signature whose digest matches, a chain to the pinned Microsoft Identity Verification Root CA 2020, the pinned
  signing-profile EKU, a bound RFC 3161 timestamp, and a denylist, for the plugin and every DLL it imports.

**Not built (proposed in ADR 0012, `docs/adr/0012-static-install-root.md`, status proposed)**

- A runtime DLL separate from `BugSplat64.dll`, named `nevr-runtime.dll`, loaded from `%LOCALAPPDATA%\nevr\echovr\`.
- The host verifying that file with the ADR 0010 verifier before it maps it, held open with `FILE_SHARE_READ` across the
  load, then the `versions` record, and one refusal dialog followed by exit code 78 on a refusal or a missing file.
- Any host check of the runtime against `manifest.json`. No ADR in the tree decides it. Do not rely on the host to
  compare the runtime with the manifest: on PC the installer is the only party that checks the manifest today.

What this means for an installer on PC: verify steps 1 to 8 yourself, including every zip member, before you copy a
file into the player's folders. When the runtime moves to `nevr-runtime.dll`, the host's check will be an Authenticode
check, which does not replace the hash check against the manifest.

## What to tell the player when a check fails

Keep what is installed and playable in every case below. Show one sentence, offer a retry, and log the step and the
message. None of these should be reported as "update failed" without the reason.

| Failed | Say | Do |
| --- | --- | --- |
| Step 1, the server could not be reached or sent too much | "Could not get update information. Check your connection and try again." | Retry later |
| Step 2, signature, or a key that is not pinned or is revoked | "The update information is not signed by nEVR, so it was not used. Nothing was changed." | Do not retry in a loop; fetch the pair again twice 3 seconds apart first, then stop |
| Step 3, `schema_version` or `product` | "This update needs a newer installer." | Point the player at the installer's download page |
| Step 4, version went down or same version with other bytes | "The update information is older than what this installer has already seen, so it was not used." | Log `manifest version went down: got N, have M` |
| Step 5, expired | "Update information is stale since `<expires>`. Check your clock or try later." | Launching the installed version stays allowed |
| Step 5, the clock reading is not a valid UTC time | "This installer could not read the clock. Nothing was changed." | Log it; do not guess a time |
| Step 6, the state could not be read or written | "This installer could not read or save its own state. Nothing was changed." | Do not continue with an empty state |
| Step 7, channel has no release | "No release is published on this channel yet." | Offer the other channel |
| Step 8, size, sha256, a zip entry, a path that is not allowed, or APK package, version code or certificate | "A downloaded file did not match the release, so it was deleted and nothing was installed." | Delete the download; install nothing from that release |

## Test vectors

All are under `docs/reference/release-verification/`. `good/` is the example manifest and its signature. The others
differ in the way their names say. The files are the vectors; no other set is published. The cases below that need a
signature the fixtures do not carry (a revocation, a recovery, a manifest with a hostile path) are made with
`minisign -G -W` throwaway keys, which is how `tools/tests/test_verifying_a_release_doc.py` makes them.

| Directory | What it is | Expected at the script's step |
| --- | --- | --- |
| `good/` | The example manifest, hashed-mode signature by the test key | passes steps 2 to 7 with `test-pinned-keys.txt` |
| `legacy-ed/` | Same manifest, signed in the legacy `Ed` mode by the test key | step 2, algorithm is not ED. `minisign -V` alone accepts it |
| `unpinned-key/` | Same manifest, hashed signature by a different throwaway key | step 2, signing key is not pinned |
| `same-version-other-bytes/` | A different manifest with the same `manifest_version` 1, signed by the test key | step 4 when the state directory holds the hash of `good/manifest.json` |

Cases made from `good/` with a command, so no further files are needed (`<state>` is a state directory; the first
four put a `state.json` there):

| Case | How | Expected |
| --- | --- | --- |
| One byte of the manifest changed | `cp -r good tampered && printf X \| dd of=tampered/manifest.json bs=1 seek=100 conv=notrunc` | step 2, signature does not verify |
| One byte of an artifact entry changed | `sed -i '0,/"size": 4914229/s//"size": 4914228/' tampered/manifest.json` | step 2, signature does not verify |
| One byte of the trusted comment changed | `sed -i 's/manifest_version=1/manifest_version=2/' tampered/manifest.json.sig` | step 2, signature does not verify |
| A lower `manifest_version` | a `state.json` that holds `{"highest_manifest_version": 2}` | step 4, `manifest version went down: got 1, have 2` |
| A fresh install built after the manifest | `MIN_MANIFEST_VERSION=2` in the script or program, no state at all | step 4, `manifest version went down: got 1, have 2` |
| An expired manifest | `<now>` = `2026-11-08T00:00:00Z` | step 5, stale since 2026-11-07T18:00:00Z |
| A clock reading that is not a time | `<now>` = `` (empty), `yesterday`, `2026-10-26T12:00:00` or `2026-10-26T12:00:00Z` followed by a newline | step 5, the current time is not UTC written as YYYY-MM-DDTHH:MM:SSZ |
| A revoked key | a `state.json` that holds `{"revoked_keys": ["c7b61c00e82fdcb1"]}` | step 2, signing key c7b61c00e82fdcb1 is revoked |
| A state file that cannot be read | a `state.json` that is not JSON, or a directory named `state.json` | step 6, not a state file or cannot read |
| A byte of an artifact flipped after download | `printf X \| dd of=windows.zip bs=1 seek=200000 conv=notrunc` | step 8, sha256 differs |
| A server that sends more than `size` | a local server for the artifact path that ignores the length | step 8, the server sent more than N bytes, and the file is gone |
| A manifest nobody verified | the check-artifact.sh script on a state directory whose `manifest.json` was edited after step 6 | step 8, the stored manifest is not the one that passed step 2 |
| An extra entry in the zip | `zip -j windows.zip extra.txt`, then `python3 -I check-zip.py <state> 5.5.1 windows.zip` | step 8, the zip holds extra.txt, which the manifest does not list |
| A directory entry or a symbolic link in the zip | `zip -r` of a directory, or `zip -y` of a link | step 8, the zip holds it, or it is not a regular file |
| A revocation list that is not a list of strings | a signed manifest whose `revoked_keys` is `"abc"` or `[1]` | step 6, `revoked_keys in the manifest is not a list of strings` |
| A member of the wrong type | a signed manifest whose `product` is `5`, `manifest_version` is `"1"`, `channels` is `[]` or `releases` is `"x"` | a `FAIL step N` line and exit 1 in all three implementations, never a stack trace |
| Key compromise | a manifest signed by a pinned key B that lists key A in `revoked_keys` with a higher `manifest_version`, then one more signed by A | the first passes steps 2 to 5; the second fails at step 2, signing key is revoked |

The real v5.5.1 files named in the example (the Windows zip and the Quest library) are on the project's GitHub release
page; their sizes and hashes are the ones in `good/manifest.json`, so step 8 can be run for those two on a fresh
machine once a local server serves them at the path the manifest names. The example's APK is not published on GitHub
and the example has no `signer_cert_sha256`.
