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
- The snippets compile and run with `minisign` 0.12, `jq` 1.8, GNU coreutils with `bash` as `sh`, Python 3.14, Rust 1.99
  with `minisign-verify` 0.3.0, and .NET SDK 10 with `BouncyCastle.Cryptography` 2.7.0 (netstandard2.0). The Rust and C#
  loops for the zip members (step 8), the `adb` command and `Get-AuthenticodeSignature` are described and are not
  part of that run.
- What the PC host checks before it loads the runtime is split into built and not built in "What the PC host checks".

## The order of checks

Run these in order. Each one stops the install when it fails, and a failed check never falls through to a later one.
The "defends against" column is what the check is for, so you can tell what a shortcut gives up.

| Step | Check | Defends against |
| --- | --- | --- |
| 1 | Fetch `manifest.json` (at most 1 MiB) and `manifest.json.sig` (at most 1 KiB) | A server that sends an endless body |
| 2 | The signature, over the exact bytes, before any JSON parser reads them. Algorithm `ED`, a pinned key, not revoked | Anyone who can write to the storage, the cache or a mirror in front of it |
| 3 | `schema_version` is 1 and `product` is `nevr-runtime` | A validly signed file for something else, or a format you cannot read |
| 4 | `manifest_version` is not lower than the highest you accepted | Rollback to an older manifest that was really signed |
| 5 | The current time is before `expires` | A stale manifest served forever so nobody sees the update |
| 6 | Store the new highest `manifest_version`, the union of `revoked_keys`, and the bytes | The same two attacks on the next run |
| 7 | Pick the channel's version; it must be a key of `releases` | Installing something no channel names |
| 8 | For each artifact you handle: exact `size`, `sha256`, and for the Windows zip every member | A swapped, truncated or padded file; files mixed from two releases |

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

**Store between runs** (in the installer's own data directory, never next to the files a player can edit freely):

| What | Why |
| --- | --- |
| `highest_manifest_version` | Step 4 |
| `revoked_keys` (the union of every accepted manifest's list) | A revoked key stays refused whatever later manifests say |
| `last_manifest_sha256`, and the last verified `manifest.json` and `.sig` bytes | Step 4 treats an equal version with different bytes as an attack |
| The installed release version and each installed artifact's `sha256` | Step 7 does nothing when the channel's version is lower than the installed one |

Compile in a minimum `manifest_version` too, the one that was current when your build was made, and use
`max(stored, compiled)` in step 4. A fresh install with nothing stored then cannot be rolled back further than its own
build. The snippets below keep the first three rows in a `state.json`:

```json
{ "highest_manifest_version": 1, "revoked_keys": [], "last_manifest_sha256": "<64 hex>" }
```

## Step 1: fetch

The manifest is at `https://evr.echo.taxi/nevr-runtime/manifest.json` and its signature at
`https://evr.echo.taxi/nevr-runtime/manifest.json.sig`. Every artifact `path` in the manifest is relative to
`https://evr.echo.taxi/nevr-runtime/` and is one version directory and one file name, so a manifest cannot send you to
another host.

```sh
BASE=https://evr.echo.taxi/nevr-runtime
curl --fail --silent --show-error --max-filesize 1048576 -o manifest.json     "$BASE/manifest.json"
curl --fail --silent --show-error --max-filesize 1024    -o manifest.json.sig "$BASE/manifest.json.sig"
```

`--max-filesize` only applies when the server announces a length, so the scripts below check the size again after the
download. If step 2 fails on a pair you just fetched, fetch the pair again, twice, 3 seconds apart, before you report
the failure: the two files are separate objects and a reader can catch them between two writes.

## Step 2: the signature

The signature is a minisign signature in the hashed mode (`ED`): Ed25519 over the BLAKE2b-512 hash of the file, plus a
signed "trusted comment". Line 2 of the `.sig` is base64 of 74 bytes, `alg(2) || key_id(8) || signature(64)`. Refuse
anything but `ED` (the legacy `Ed` mode signs the raw file), find the pinned key whose id matches, check it is not
revoked, then verify. Take nothing from the trusted comment: the JSON is authoritative.

Why the explicit `ED` check: the `minisign` command verifies a legacy `Ed` signature too, so it cannot be the whole check.

### Command line (minisign, jq)

This script runs steps 2 to 7. It prints `FAIL step N: ...` on stderr and exits 1 at the first failure. It creates
`state.json` as `{}` when the file is absent and rewrites it only when steps 2 to 5 pass.

```sh verify-manifest.sh
#!/bin/sh
# usage: verify-manifest.sh <dir with manifest.json and manifest.json.sig> <pinned-keys.txt> <state.json> <now> <channel>
# <now> is UTC as YYYY-MM-DDTHH:MM:SSZ, e.g. $(date -u +%Y-%m-%dT%H:%M:%SZ)
set -eu
dir=$1 pinned=$2 state=$3 now=$4 channel=$5
man=$dir/manifest.json sig=$dir/manifest.json.sig
fail() { echo "FAIL step $1: $2" >&2; exit 1; }
[ -f "$state" ] || echo '{}' > "$state"

[ "$(wc -c < "$man")" -le 1048576 ] || fail 1 "manifest.json is over 1 MiB"
[ "$(wc -c < "$sig")" -le 1024 ] || fail 1 "manifest.json.sig is over 1 KiB"

# Step 2. The blob is 74 bytes = 148 hex digits: alg (4) + key id (16) + signature.
blob=$(sed -n 2p "$sig" | base64 -d | od -An -v -tx1 | tr -d ' \n') || fail 2 "signature line is not base64"
[ "${#blob}" -eq 148 ] || fail 2 "signature blob is not 74 bytes"
[ "$(printf %s "$blob" | cut -c1-4)" = 4544 ] || fail 2 "signature algorithm is not ED (hashed)"
id=$(printf %s "$blob" | cut -c5-20)
if jq -e --arg id "$id" '(.revoked_keys // []) | index($id)' "$state" > /dev/null; then
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

# Step 4.
mv=$(jq -r .manifest_version "$man")
hi=$(jq -r '.highest_manifest_version // 0' "$state")
sha=$(sha256sum "$man" | cut -d' ' -f1)
[ "$mv" -ge "$hi" ] || fail 4 "manifest version went down: got $mv, have $hi"
if [ "$mv" -eq "$hi" ] && [ "$sha" != "$(jq -r '.last_manifest_sha256 // ""' "$state")" ]; then
  fail 4 "same manifest_version, different bytes"
fi

# Step 5. The timestamps are UTC with a Z suffix and whole seconds, so a string comparison is a time comparison.
jq -e --arg now "$now" '.expires > $now' "$man" > /dev/null \
  || fail 5 "update information is stale since $(jq -r .expires "$man"); check the clock or try later"

# Step 6.
jq -n --slurpfile m "$man" --slurpfile s "$state" --arg sha "$sha" '{
  highest_manifest_version: $m[0].manifest_version,
  revoked_keys: ((($s[0].revoked_keys // []) + ($m[0].revoked_keys // [])) | unique),
  last_manifest_sha256: $sha }' > "$state.new"
mv -f "$state.new" "$state"

# Step 7.
ver=$(jq -r --arg c "$channel" '.channels[$c] // empty' "$man")
[ -n "$ver" ] || fail 7 "channel $channel has no release"
jq -e --arg v "$ver" '.releases[$v]' "$man" > /dev/null || fail 7 "channel $channel names $ver, which is not in releases"
echo "OK signed by key id $id; manifest_version $mv; channel $channel -> $ver"
jq -r --arg v "$ver" '.releases[$v].artifacts[] | "  \(.kind)\t\(.path)\t\(.size)\t\(.sha256)"' "$man"
```

Run it:

```sh
sh verify-manifest.sh <dir> pinned-keys.txt state.json "$(date -u +%Y-%m-%dT%H:%M:%SZ)" beta
```

### Rust (`minisign-verify`)

`minisign-verify` 0.3.0 checks the key id, the signature and the trusted-comment signature in one call, and its third
argument, `allow_legacy`, is `false` to refuse `Ed`. The program below runs the same steps as the script, with the
same arguments and the same `FAIL step N` verdicts. It needs `base64`, `minisign-verify`, `serde_json` and `sha2`.

```toml Cargo.toml
[package]
name = "manifestcheck"
version = "0.1.0"
edition = "2021"

[dependencies]
base64 = "0.22"
minisign-verify = "0.3"
serde_json = "1"
sha2 = "0.10"
```

```rust src/main.rs
use base64::{engine::general_purpose::STANDARD, Engine};
use minisign_verify::{PublicKey, Signature};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::{env, fs, process};

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

fn main() {
    let a: Vec<String> = env::args().collect();
    match (a.get(1).map(String::as_str), a.len()) {
        (Some("manifest"), 7) => manifest(&a[2], &a[3], &a[4], &a[5], &a[6]),
        (Some("artifact"), 6) => artifact(&a[2], &a[3], &a[4], &a[5]),
        _ => fail(0, "usage: manifestcheck manifest <dir> <pinned-keys.txt> <state.json> <now> <channel> | manifestcheck artifact <manifest.json> <version> <kind> <file>"),
    }
}

/// Steps 2 to 7.
fn manifest(dir: &str, pinned: &str, state_path: &str, now: &str, channel: &str) {
    let read = |name: &str| fs::read(format!("{dir}/{name}")).unwrap_or_else(|e| fail(1, &format!("{name}: {e}")));
    let (bytes, sig_bytes) = (read("manifest.json"), read("manifest.json.sig"));
    if bytes.len() > 1 << 20 || sig_bytes.len() > 1024 {
        fail(1, "manifest.json is over 1 MiB or manifest.json.sig is over 1 KiB");
    }
    let sig_text = String::from_utf8(sig_bytes).unwrap_or_else(|_| fail(2, "signature file is not UTF-8"));
    let state: Value = match fs::read(state_path) {
        Ok(b) => serde_json::from_slice(&b).unwrap_or_else(|e| fail(6, &format!("{state_path}: {e}"))),
        Err(_) => json!({}),
    };

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

    // Step 4.
    let version = m["manifest_version"].as_u64().unwrap_or_else(|| fail(3, "manifest_version is not an integer"));
    let highest = state["highest_manifest_version"].as_u64().unwrap_or(0);
    let sha = hex(&Sha256::digest(&bytes));
    if version < highest {
        fail(4, &format!("manifest version went down: got {version}, have {highest}"));
    }
    if version == highest && state["last_manifest_sha256"] != json!(sha) {
        fail(4, "same manifest_version, different bytes");
    }

    // Step 5: whole-second UTC strings with a Z suffix compare as times.
    let expires = m["expires"].as_str().unwrap_or_else(|| fail(3, "expires is not a string"));
    if now >= expires {
        fail(5, &format!("update information is stale since {expires}; check the clock or try later"));
    }

    // Step 6.
    let mut all = revoked(&state);
    for k in revoked(&m) {
        if !all.contains(&k) {
            all.push(k);
        }
    }
    let stored = json!({ "highest_manifest_version": version, "revoked_keys": all, "last_manifest_sha256": sha });
    fs::write(state_path, serde_json::to_vec_pretty(&stored).expect("state serializes"))
        .unwrap_or_else(|e| fail(6, &format!("{state_path}: {e}")));

    // Step 7.
    let release = m["channels"][channel].as_str().unwrap_or_else(|| fail(7, &format!("channel {channel} has no release")));
    if m["releases"][release].is_null() {
        fail(7, &format!("channel {channel} names {release}, which is not in releases"));
    }
    println!("OK signed by key id {id}; manifest_version {version}; channel {channel} -> {release}");
}

/// Step 8 for one file.
fn artifact(manifest_path: &str, version: &str, kind: &str, file: &str) {
    let bytes = fs::read(manifest_path).unwrap_or_else(|e| fail(8, &format!("{manifest_path}: {e}")));
    let m: Value = serde_json::from_slice(&bytes).unwrap_or_else(|e| fail(8, &format!("{manifest_path}: {e}")));
    let entry = m["releases"][version]["artifacts"]
        .as_array()
        .and_then(|a| a.iter().find(|x| x["kind"] == kind))
        .unwrap_or_else(|| fail(8, &format!("release {version} has no {kind} artifact")));
    let mut f = fs::File::open(file).unwrap_or_else(|e| fail(8, &format!("{file}: {e}")));
    let (mut hasher, mut size, mut buf) = (Sha256::new(), 0u64, [0u8; 65536]);
    loop {
        let n = f.read(&mut buf).unwrap_or_else(|e| fail(8, &format!("{file}: {e}")));
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        size += n as u64;
    }
    let sha = hex(&hasher.finalize());
    if Some(size) != entry["size"].as_u64() {
        fail(8, &format!("size is {size}, the manifest says {}", entry["size"]));
    }
    if entry["sha256"] != json!(sha) {
        fail(8, "sha256 differs from the manifest");
    }
    println!("OK step 8: {file} matches {kind} of release {version} (size {size}, sha256 {sha})");
}
```

```sh
cargo build --release
./target/release/manifestcheck manifest <dir> pinned-keys.txt state.json "$(date -u +%Y-%m-%dT%H:%M:%SZ)" beta
```

### C# (netstandard2.0, `BouncyCastle.Cryptography`)

The .NET base library has no Ed25519 and no BLAKE2b, so the library below uses `Ed25519Signer` and `Blake2bDigest` from
BouncyCastle, and `System.Security.Cryptography.SHA256` for the file hashes. It targets netstandard2.0 so the same file
builds for a desktop app and for Unity. It compiles and runs under .NET SDK 10. Running it under Unity IL2CPP on a Quest
is not shown here.

```xml NevrManifest/NevrManifest.csproj
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>netstandard2.0</TargetFramework>
    <LangVersion>9.0</LangVersion>
    <Nullable>disable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="BouncyCastle.Cryptography" Version="2.*" />
    <PackageReference Include="System.Text.Json" Version="8.*" />
  </ItemGroup>
</Project>
```

```csharp NevrManifest/ManifestCheck.cs
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
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

    /// <summary>What to keep between runs (step 6).</summary>
    public sealed class State
    {
        public long Highest;
        public HashSet<string> Revoked = new HashSet<string>();
        public string LastSha256 = "";
    }

    public static class ManifestCheck
    {
        const string TrustedPrefix = "trusted comment: ";

        static string Hex(byte[] b) => string.Concat(b.Select(x => x.ToString("x2")));

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
        /// UTC as YYYY-MM-DDTHH:MM:SSZ. On success <paramref name="state"/> holds what step 6 stores (the caller
        /// writes it down) and the parsed manifest is returned; any failure throws <see cref="Rejected"/>.
        /// </summary>
        public static JsonDocument Verify(byte[] manifest, string sigText, IEnumerable<string> pinned, State state, string now)
        {
            if (manifest.Length > 1 << 20 || sigText.Length > 1024)
                throw new Rejected(1, "manifest.json is over 1 MiB or manifest.json.sig is over 1 KiB");

            // Step 2: before any JSON parser sees the manifest.
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
            JsonDocument doc;
            try { doc = JsonDocument.Parse(manifest); }
            catch (JsonException e) { throw new Rejected(3, "manifest.json: " + e.Message); }
            var root = doc.RootElement;
            if (!root.TryGetProperty("schema_version", out var schema) || !schema.TryGetInt32(out var schemaValue) || schemaValue != 1
                || !root.TryGetProperty("product", out var product) || product.GetString() != "nevr-runtime")
                throw new Rejected(3, "schema_version is not 1 or product is not nevr-runtime");

            // Step 4.
            var version = root.GetProperty("manifest_version").GetInt64();
            string sha;
            using (var sha256 = SHA256.Create()) sha = Hex(sha256.ComputeHash(manifest));
            if (version < state.Highest)
                throw new Rejected(4, "manifest version went down: got " + version + ", have " + state.Highest);
            if (version == state.Highest && sha != state.LastSha256)
                throw new Rejected(4, "same manifest_version, different bytes");

            // Step 5: whole-second UTC strings with a Z suffix compare as times.
            var expires = root.GetProperty("expires").GetString();
            if (string.CompareOrdinal(now, expires) >= 0)
                throw new Rejected(5, "update information is stale since " + expires + "; check the clock or try later");

            // Step 6.
            state.Highest = version;
            state.LastSha256 = sha;
            if (root.TryGetProperty("revoked_keys", out var revoked))
                foreach (var k in revoked.EnumerateArray()) state.Revoked.Add(k.GetString());
            return doc;
        }

        /// <summary>Step 7: the release version the channel names, which must be a key of releases.</summary>
        public static string ChannelVersion(JsonElement manifest, string channel)
        {
            if (!manifest.GetProperty("channels").TryGetProperty(channel, out var v))
                throw new Rejected(7, "channel " + channel + " has no release");
            var version = v.GetString();
            if (!manifest.GetProperty("releases").TryGetProperty(version, out _))
                throw new Rejected(7, "channel " + channel + " names " + version + ", which is not in releases");
            return version;
        }

        /// <summary>Step 8 for one file: exact size and sha256 of the artifact of that kind.</summary>
        public static void CheckArtifact(JsonElement manifest, string version, string kind, string path)
        {
            var entry = manifest.GetProperty("releases").GetProperty(version).GetProperty("artifacts")
                .EnumerateArray().FirstOrDefault(a => a.GetProperty("kind").GetString() == kind);
            if (entry.ValueKind != JsonValueKind.Object) throw new Rejected(8, "release " + version + " has no " + kind + " artifact");
            var size = new FileInfo(path).Length;
            if (size != entry.GetProperty("size").GetInt64())
                throw new Rejected(8, "size is " + size + ", the manifest says " + entry.GetProperty("size").GetInt64());
            string sha;
            using (var sha256 = SHA256.Create())
            using (var file = File.OpenRead(path)) sha = Hex(sha256.ComputeHash(file));
            if (sha != entry.GetProperty("sha256").GetString()) throw new Rejected(8, "sha256 differs from the manifest");
        }
    }
}
```

A command-line wrapper with the same arguments and the same `FAIL step N` verdicts as the script and the Rust program (a
.NET 10 console that references the library; this is the part that is not netstandard):

```xml Harness/Harness.csproj
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>
  <ItemGroup>
    <ProjectReference Include="../NevrManifest/NevrManifest.csproj" />
  </ItemGroup>
</Project>
```

```csharp Harness/Program.cs
using System.Text.Json;
using Nevr.Manifest;

try
{
    if (args.Length == 6 && args[0] == "manifest")
    {
        var dir = args[1];
        var pinned = File.ReadLines(args[2]).Where(l => l.Trim().Length > 0 && !l.StartsWith("#"))
            .Select(l => l.Split(' ', StringSplitOptions.RemoveEmptyEntries)[1]).ToList();
        var state = new State();
        if (File.Exists(args[3]))
        {
            using var s = JsonDocument.Parse(File.ReadAllText(args[3]));
            if (s.RootElement.TryGetProperty("highest_manifest_version", out var h)) state.Highest = h.GetInt64();
            if (s.RootElement.TryGetProperty("last_manifest_sha256", out var l)) state.LastSha256 = l.GetString();
            if (s.RootElement.TryGetProperty("revoked_keys", out var r))
                foreach (var k in r.EnumerateArray()) state.Revoked.Add(k.GetString());
        }
        using var doc = ManifestCheck.Verify(File.ReadAllBytes(dir + "/manifest.json"), File.ReadAllText(dir + "/manifest.json.sig"), pinned, state, args[4]);
        File.WriteAllText(args[3], JsonSerializer.Serialize(new Dictionary<string, object>
        {
            ["highest_manifest_version"] = state.Highest,
            ["revoked_keys"] = state.Revoked.OrderBy(k => k, StringComparer.Ordinal).ToArray(),
            ["last_manifest_sha256"] = state.LastSha256,
        }));
        var release = ManifestCheck.ChannelVersion(doc.RootElement, args[5]);
        Console.WriteLine($"OK manifest_version {state.Highest}; channel {args[5]} -> {release}");
    }
    else if (args.Length == 5 && args[0] == "artifact")
    {
        using var doc = JsonDocument.Parse(File.ReadAllBytes(args[1]));
        ManifestCheck.CheckArtifact(doc.RootElement, args[2], args[3], args[4]);
        Console.WriteLine($"OK step 8: {args[4]} matches {args[3]} of release {args[2]}");
    }
    else
    {
        Console.Error.WriteLine("usage: harness manifest <dir> <pinned-keys.txt> <state.json> <now> <channel> | harness artifact <manifest.json> <version> <kind> <file>");
        return 2;
    }
    return 0;
}
catch (Rejected e)
{
    Console.Error.WriteLine($"FAIL step {e.Step}: {e.Message}");
    return 1;
}
```

```sh
dotnet run --project Harness -- manifest <dir> pinned-keys.txt state.json "$(date -u +%Y-%m-%dT%H:%M:%SZ)" beta
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
  `schema_version`, and a changed meaning gets a new one.
- **Step 4.** Let `have = max(stored highest, compiled minimum)`. A lower `manifest_version` is refused with
  `manifest version went down: got N, have M`. An equal one with different bytes is refused too (the signer never produces
  it); an equal one with the same bytes means nothing is new.
- **Step 5.** Past `expires` you stop updating and say so. You may still launch what is installed.
- **Step 6.** Store only after steps 2 to 5 pass. A key in `revoked_keys` stays refused, whatever later manifests say.
- **Step 7.** `channels` maps a channel name (`stable`, `beta`) to a version. A channel that is absent has no release.
  If the version is lower than the one installed and the player did not change channel or ask to go back, do nothing.
  Install a release's artifacts as a set or not at all, and never take artifacts from two manifests.

## Step 8: artifacts

For each artifact of the chosen release that your platform handles, `kind` tells what it is: `windows-zip`,
`quest-library`, `quest-apk` (and `quest-apk-bundled` later). `plugin` and `game-data` are reserved: skip a kind you do
not handle.

1. Build the URL as the base above plus `path`. Refuse a `path` that is not one version directory and one file name
   (the check is in the script below).
2. Download it, stop at `size` bytes.
3. Require the exact `size` and the `sha256`.

```sh check-artifact.sh
#!/bin/sh
# usage: check-artifact.sh <manifest.json> <version> <kind> <file>
set -eu
man=$1 ver=$2 kind=$3 file=$4
fail() { echo "FAIL step 8: $1" >&2; exit 1; }
entry=$(jq -c --arg v "$ver" --arg k "$kind" '.releases[$v].artifacts[] | select(.kind == $k)' "$man")
[ -n "$entry" ] || fail "release $ver has no $kind artifact"
printf %s "$entry" | jq -e '.path | test("^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)/nevr-runtime-v[0-9]+\\.[0-9]+\\.[0-9]+-[a-z0-9.-]+$")' > /dev/null \
  || fail "path in the manifest is not one version directory and one file name"
want_size=$(printf %s "$entry" | jq -r .size)
want_sha=$(printf %s "$entry" | jq -r .sha256)
[ "$(wc -c < "$file")" -eq "$want_size" ] || fail "size is $(wc -c < "$file"), the manifest says $want_size"
[ "$(sha256sum "$file" | cut -d' ' -f1)" = "$want_sha" ] || fail "sha256 differs from the manifest"
echo "OK step 8: $file matches $kind of release $ver (size $want_size, sha256 $want_sha)"
```

Fetching one (the download stops when the server sends more than `size` bytes; the script then checks the exact size):

```sh
entry_path=$(jq -r --arg v "$ver" --arg k windows-zip '.releases[$v].artifacts[] | select(.kind == $k) | .path' manifest.json)
entry_size=$(jq -r --arg v "$ver" --arg k windows-zip '.releases[$v].artifacts[] | select(.kind == $k) | .size' manifest.json)
curl --fail --silent --show-error --max-filesize "$entry_size" -o windows.zip "$BASE/$entry_path"
sh check-artifact.sh manifest.json "$ver" windows-zip windows.zip
```

### The Windows zip: every member

The zip's `contents` lists every file entry with its own `path`, `sha256` and `size`. Extract nothing before this
passes. Every entry in the zip must be listed, every listed file must be present, and an entry must not be listed twice.
Because an entry must equal a listed name exactly, and a listed name is a relative path with no `..` segment, a
path-traversal entry cannot pass.

```python check-zip.py
# usage: python3 -I check-zip.py <manifest.json> <version> <zip>
import hashlib, json, sys, zipfile

manifest, version, archive = sys.argv[1:4]


def fail(message):
    sys.exit("FAIL step 8: " + message)


releases = json.load(open(manifest))["releases"]
artifact = next((a for a in releases[version]["artifacts"] if a["kind"] == "windows-zip"), None)
if artifact is None:
    fail("release " + version + " has no windows-zip artifact")
listed = {c["path"]: c for c in artifact["contents"]}
with zipfile.ZipFile(archive) as z:
    names = [i.filename for i in z.infolist() if not i.is_dir()]
    if len(names) != len(set(names)):
        fail("an entry appears twice in the zip")
    for name in names:
        want = listed.get(name)
        if want is None:
            fail("the zip holds " + name + ", which the manifest does not list")
        with z.open(name) as f:
            data = f.read(want["size"] + 1)  # one byte past the size is enough to see an oversized entry
        if len(data) != want["size"]:
            fail(name + " has a different size than the manifest says")
        if hashlib.sha256(data).hexdigest() != want["sha256"]:
            fail(name + " has a different sha256 than the manifest says")
    absent = sorted(set(listed) - set(names))
    if absent:
        fail("the manifest lists " + ", ".join(absent) + ", which the zip does not hold")
print("OK step 8: all %d entries of %s match the manifest" % (len(names), archive))
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
`signer_cert_sha256`. After the sha256 and size pass, check the APK itself, before and after install:

```sh
# the file: package name and version code
aapt2 dump badging app.apk > badging.txt && grep -E "^package: " badging.txt
# the file: the signature is valid (exit 0) and the SHA-256 of the signing certificate
apksigner verify --print-certs app.apk > certs.txt && grep "certificate SHA-256 digest" certs.txt
# the installed package on the headset: version code
adb shell dumpsys package com.readyatdawn.r15 | grep versionCode
```

Compare each with the manifest: the package name equals `android.package`; the `versionCode` equals
`android.version_code`; when `android.signer_cert_sha256` is present, the certificate digest equals it. An installer that
runs on the headset reads the installed package's version code and signing certificate through the Android package
manager instead of `adb`. `signer_cert_sha256` is optional: when the manifest has no value, compare only
the package name and the version code, and do not invent one.

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
| Step 7, channel has no release | "No release is published on this channel yet." | Offer the other channel |
| Step 8, size, sha256, a zip member, or APK package, version code or certificate | "A downloaded file did not match the release, so it was deleted and nothing was installed." | Delete the download; install nothing from that release |

## Test vectors

All are under `docs/reference/release-verification/`. `good/` is the example manifest and its signature. The others
differ in the way their names say. The files are the vectors; no other set is published.

| Directory | What it is | Expected at the script's step |
| --- | --- | --- |
| `good/` | The example manifest, hashed-mode signature by the test key | passes steps 2 to 7 with `test-pinned-keys.txt` |
| `legacy-ed/` | Same manifest, signed in the legacy `Ed` mode by the test key | step 2, algorithm is not ED. `minisign -V` alone accepts it |
| `unpinned-key/` | Same manifest, hashed signature by a different throwaway key | step 2, signing key is not pinned |
| `same-version-other-bytes/` | A different manifest with the same `manifest_version` 1, signed by the test key | step 4 when `state.json` holds the hash of `good/manifest.json` |

Cases made from `good/` with a command, so no further files are needed:

| Case | How | Expected |
| --- | --- | --- |
| One byte of the manifest changed | `cp -r good tampered && printf X \| dd of=tampered/manifest.json bs=1 seek=100 conv=notrunc` | step 2, signature does not verify |
| One byte of an artifact entry changed | `sed -i '0,/"size": 4914229/s//"size": 4914228/' tampered/manifest.json` | step 2, signature does not verify |
| One byte of the trusted comment changed | `sed -i 's/manifest_version=1/manifest_version=2/' tampered/manifest.json.sig` | step 2, signature does not verify |
| A lower `manifest_version` | a `state.json` that holds `{"highest_manifest_version": 2}` | step 4, `manifest version went down: got 1, have 2` |
| An expired manifest | `<now>` = `2026-11-08T00:00:00Z` | step 5, stale since 2026-11-07T18:00:00Z |
| A revoked key | a `state.json` that holds `{"revoked_keys": ["c7b61c00e82fdcb1"]}` | step 2, signing key c7b61c00e82fdcb1 is revoked |
| A byte of an artifact flipped after download | `printf X \| dd of=windows.zip bs=1 seek=200000 conv=notrunc` | step 8, sha256 differs |
| An extra entry in the zip | `zip -j windows.zip extra.txt`, then `python3 -I check-zip.py good/manifest.json 5.5.1 windows.zip` | step 8, the zip holds extra.txt, which the manifest does not list |

The real v5.5.1 files named in the example (the Windows zip and the Quest library) are on the project's GitHub release
page; their sizes and hashes are the ones in `good/manifest.json`, so step 8 can be run for those two on a fresh
machine. The example's APK is not published on GitHub and the example has no `signer_cert_sha256`.
