# ADR 0047: Custom maps are served from a CDN as content-addressed pieces under one signed manifest

Status: proposed. Applies to `src/core/map_types.h`, the map-channel code under `src/core/maps/` and `src/runtime/maps/` (each
packet's build fragment), `src/runtime/cargo/win_env.*` (the PC `Env` and `MapFs`), the file-backed resource sets in
`src/runtime/patch/resource_sets.*`, and the map script provider `src/runtime/content/script_provider.*`. Reuses the blob
namespace and the platform names of `docs/adr/0045-cdn-texture-delivery.md`. The reference is `docs/reference/maps-channel.md`;
the log prefix is `[NEVR.MAPS]` (`docs/standards/logging.md`).

## Context

A custom map today is a `.kfclvl` that the player installs by hand and EchoMapLoader mounts. The channel lets a player subscribe
to maps by id: nevr fetches them in the background, verifies them once, and mounts them at the next launch, beside every pack and
plugin set that works today. The constraints are the owner's: nothing that works now may change (with no `maps.*` config nevr
behaves exactly as before), maps only add resources (R2), and no game bytes travel on the CDN (F1).

## Decision

1. **One signature, everything else by hash.** The channel manifest (`nevr-maps-manifest` v1) is signed with minisign Ed25519.
   It pins each map's package index (`nevr-map-package` v1) by sha256 and size, the index pins each member, and each member is a
   list of pieces. There is no per-blob signature. The signature is checked before any JSON is parsed.
2. **Pieces are cut at frame boundaries by the publisher.** A set member is cut after a zstd frame once a piece holds 512 KiB and
   the frame's sha256 starts below 64, or before 8 MiB. The client only concatenates pieces in order, so the rule can change
   without a client release. The client never unzips; the `.nevrmap` is an upload container only.
3. **Freshness.** One serial floor per channel (`base_url`), compared strictly; an equal serial must carry equal body bytes. A
   per-`(id, platform)` version floor. Official keys are compiled in or set in `maps.official_keys`; official and author keys are never trusted for the same `base_url` (`mixed_keys`). Past
   `expires`, a map with scripts does not mount; a script-free map still does. The stored manifest is re-verified at every boot,
   before anything mounts.
4. **Integrity at rest.** Hashing happens once, at fetch. At boot each file is opened share-read and held for the process, and
   its `FileStamp` (volume, 128-bit file id, size, last write), read through the held handle, must equal the index. A changed
   stamp defers the map to a background re-hash.
5. **R2: maps only add resources.** A map's set mounts through the existing registry, which refuses stock keys, collisions and
   override tables. A map's edited rules reach the game through config aliases: a map may alias at most 4 allowlisted keys (v1:
   `mp_arena_rules`) to its own `<id>_<suffix>` records. An alias is live while any level of its map is loaded, and membership is
   a set, never a count. When two loaded maps alias one key, the main level's map wins, otherwise the lowest map id; the result
   depends only on which maps are loaded.
6. **Scripts.** A map's script DLLs are registered before its set, all or nothing, and only if each passes the PE rules (PE32+
   AMD64 DLL, no imports, TLS or CLR, entry point 0, exactly one unforwarded `setup_bindings` export in code, allowed sections
   only, no writable-and-executable section). Stock and content-pack script hashes are refused. A server mounts a map only if
   every script it carries is in `maps.server_allow`.
7. **Signed-only (F6).** Under `-require-signed-plugins`, a map's scripts count as signed only when the manifest that pins them
   is signed by a compiled-in official key; `maps.trusted_keys` (author and dev keys) never satisfy it. Until this ADR is
   accepted, every map script is refused `signed_only` under that flag, and script-free maps mount.
8. **Precedence.** A plugin set or a content pack keeps its keys (the existing mount report). A local `.kfclvl` of the same
   level wins over the CDN copy: the CDN map is skipped with `map_skipped reason=local_map`, so the two never mix per key.
9. **Version pin (F3).** Players match only on the same map version: the pin is `id@version`, with `source_sha256` (the map
   document the export read) tying the PC and Quest packages of one version together. nevr reports its pins at registration and
   login, and a client whose version differs from the session's pin leaves before loading.
10. **Revocation.** `revoked_ids`, an id missing from the manifest, or a listed sha256 (index, member or piece) drops the map at
    the next boot that sees it. A DLL already loaded is not unloaded mid-session (not defended).
11. **Refused in v1:** `submode` and `engine_limits` (they write game data) and new sound banks (an unmeasured event-id
    collision).
12. **No compiled default host.** `maps.base_url` must be configured until the official bucket and content key are chosen.

## Consequences

- With no `maps.subscribed`, nevr starts no thread, makes no directory, installs no hook and writes one Info line,
  `[NEVR.MAPS] map_start reason=not_configured`.
- `nevr_cargo::Env` is unchanged; the extra file operations live in `nevr_maps::MapFs`.
- The cache lives in `<root>\cache\maps\`, outside `content\` and outside the files `asset_cache` enumerates.
- An attacker who can rewrite `%LOCALAPPDATA%\nevr` and restore file stamps can already replace nevr (ADR 0010): not defended.
- Expiry blocks scripted maps only, so script-free maps keep working offline.
