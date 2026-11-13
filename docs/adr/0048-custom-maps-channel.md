---
# atwd read contract
read: whole
read_via: cat docs/adr/0048-custom-maps-channel.md
audience: all
---

# ADR 0048: Custom maps are served from a CDN as content-addressed pieces under one signed manifest per lobby group

Status: proposed. Issue #2733. Applies to the custom-map channel's core in `src/core/` (`map_types.h`, `map_format.h`,
`minisign_verify.h`, `map_manifest.h`, `map_index.h`, `map_derive.h`, `map_run.h`, `map_fetch_queue.h`, `config_alias.h`,
`script_image_rules.h`, `resource_set_registry.h`), its build fragment `src/core/maps.cmake`, and the `maps` section of
`config.yaml` (`src/core/nevr_config.cpp`, flat keys in `src/runtime/lifecycle/service_map.cpp`). Reuses ADR 0045
(`docs/adr/0045-cdn-texture-delivery.md`): its resolver and precedence (decisions 1 and 2), its one chunk-index counter
(decision 3), never blocking the boot (decision 7) and limits from the device (decision 8), and records one exception to its
decision 10. The log tag is `[NEVR.MAPS]`.

## Context

A custom map today is a level file that the player installs by hand and EchoMapLoader mounts. The channel delivers a lobby group's
maps instead: nevr fetches them in the background, verifies them once, and mounts them at the next launch, beside every pack and
plugin set that works today. The constraints: nothing that works now may change (with no `maps.base_url` nevr behaves exactly as
before), maps only add resources, and no game bytes travel on the CDN.

A client and a dedicated server must hold the same maps for a session, or they split the players. The one id both learn from the
session they are in is the lobby group id.

## Decision

1. **One signature, everything else by hash.** A group's manifest (`nevr-maps-manifest` v1) is signed with minisign Ed25519
   (Monocypher, `third_party/monocypher/`). It pins each map's package index (`nevr-map-package` v1) by sha256 and size, the index
   pins each member, and each member is a list of pieces. There is no per-blob signature. The signature is checked before any JSON
   is parsed.
2. **Pieces are cut at frame boundaries by the publisher.** A set member is cut after a zstd frame once a piece holds 512 KiB and
   the frame's sha256 starts below 64, or before 8 MiB. The client only concatenates pieces in order, so the rule can change
   without a client release; the client refuses a piece over `kMaxPieceBytes` (`map_format.h`). The client never unzips; the
   `.nevrmap` archive is an upload container only.
3. **Freshness.** One serial floor per signed channel name, group and signing key, compared strictly; an equal serial must carry
   equal body bytes. A per-`(id, platform)` version floor. A `base_url` is bound to the channel name of its first verified manifest
   (`maps.channel` names it outright). A manifest meets the highest floors of the keys trusted now for its group: a key no longer
   trusted (test to official) holds nobody back, and a rollback under any trusted key is refused (`FloorBook`, `map_manifest.h`).
   One group's floors never hold another back. Official keys are compiled in or set in `maps.official_keys`, author keys in
   `maps.trusted_keys`; the two are never trusted for the same `base_url` (`mixed_keys`). Past `expires`, a map with scripts does
   not mount; a script-free map still does. The mount re-verifies each stored manifest's signature and checks each map's package
   index against its pin before anything mounts; it hashes no member file.
4. **Integrity at rest.** A member's bytes are hashed once, at fetch. At mount each file is opened share-read and held for the
   process (`MapFs::OpenHeld`), and its `FileStamp` (volume, 128-bit file id, size, last write), read through the held handle, must
   equal the cache index's. A changed stamp defers the map to a re-hash on the worker, never on the boot thread (ADR 0045
   decision 7).
5. **Maps only add resources.** A map's set mounts through the shared resource-set registry (`src/core/resource_set_registry.h`),
   which refuses stock keys, collisions and override tables. A map's edited rules reach the game through config aliases
   (`src/core/config_alias.h`): a map may alias at most `kMaxConfigAliases` (4) allowlisted keys (v1: `mp_arena_rules`) to its own
   `<id>_<suffix>` records. An alias is live while any level of its map is loaded, and membership is a set, never a count. When two
   loaded maps alias one key, the main level's map wins, otherwise the lowest map id; the result depends only on which maps are
   loaded. A map whose aliases cannot all be staged keeps nothing it registered.
6. **Scripts.** A map's script DLLs are registered before its set, all or nothing, and only if each passes the PE rules
   (`src/core/script_image_rules.h`: PE32+ AMD64 DLL, no imports, TLS or CLR, entry point 0, exactly one unforwarded
   `setup_bindings` export in code, allowed sections only, no writable-and-executable section; the Quest twin checks the ELF shape
   of a map's library). Stock and content-pack script hashes are refused. Which scripted maps a server mounts is decision 18; no
   allow list exists.
7. **Signed-only.** Under `-require-signed-plugins`, a map's scripts count as signed only when the manifest that pins them is
   signed by a compiled-in official key; a key set in `maps.official_keys` or `maps.trusted_keys` never satisfies it, because a
   flag that config could satisfy would not restrict anything. A map whose scripts do not count is refused `signed_only`, and
   script-free maps still mount. No official key is compiled in yet (decision 12), so under that flag every map script is refused.
8. **Precedence.** ADR 0045 decision 2 holds: cargo, then stock, then plugin sets and maps, and a map never replaces a stock name.
   A plugin set or a content pack keeps its keys (the existing mount report). A local copy of the same level wins over the CDN
   copy: the CDN map is skipped with `map_skipped reason=local_map`, so the two never mix per key.
9. **Version pin.** Players match only on the same map version: the pin is `id@version`, with `source_sha256` (the map document
   the export read) tying the PC and Quest packages of one version together. nevr reports its pins at registration and login, and
   a client whose version differs from the session's pin leaves before loading.
10. **Revocation.** `revoked_ids`, an id missing from the manifest, or a listed sha256 (index, member or piece) drops the map at
    the next mount that sees it. A DLL already loaded is not unloaded mid-session (not defended).
11. **Refused in v1:** `submode` and `engine_limits` (they write game data) and new sound banks (an unmeasured event-id
    collision). The v1 package index names three member roles, a set, scripts and recipes (`map_format.h`), so a package has no
    place for any of them.
12. **No compiled default host and no compiled official key.** `maps.base_url` must be configured until the official bucket and
    content key are chosen, and no official key is compiled in until then.
13. **A server fetches the content of the group it hosts: the one exception to ADR 0045 decision 10.** Texture cargo stays client
    only. A dedicated server fetches the maps of the lobby group it is allocated for, because hosts must not choose which assets
    they support: a host that served only some groups' maps would split the players. What would change it: nothing short of a
    service that hands each server its maps itself.
14. **One signed manifest per lobby group.** Each group has one manifest under `base_url`, `guilds/<group id>/manifest`
    (`GroupManifestUrl`, `map_run.h`), and objects are content addressed, so a map two groups list at one version, and a piece two
    maps share, are fetched and stored once. The group id is in the signed body, and a manifest whose group is not the path's is
    refused (`guild_mismatch`). Reason: a small change costs a small download, and this is the CDN's existing layout. What would
    change it: when the service names the client's manifest (#2356), the service chooses which manifest a client reads; the path
    layout stays.
15. **The group id is opaque.** It is a lowercase, path-safe string of 1 to `kGroupIdMaxBytes` characters of `[0-9a-z-]`
    (`IsPathSafeGroupId`, `map_manifest.h`; the bound in `map_format.h`), used as a path segment, a cache key and a field of the
    signed body. It is never parsed as a UUID, because the service may change the form of group ids and the maps code must not
    change when it does. What would change it: a group id form that needs other characters, which would change the bound's
    character set, not the code that uses it.
16. **Clients and servers follow their groups; no local config chooses maps.** There is no `maps.subscribed` and no
    `maps.server_allow`: no key chooses which maps a player or a host has, because a client and a server must never disagree
    because of local config. A client asks for its session's group when a lobby session succeeds (`map_fetch_queue.h`); the
    fetch runs in the background and the maps mount at the next launch (ADR 0045 decision 7). Until #2356, the groups a client
    fetches are the ones it last played in, kept in the cache index, latest first, as many as the device's storage share holds
    (ADR 0045 decision 8). What would change it: #2356, where the service names each client's manifest.
17. **A server takes its group from its session and holds joins for it.** A dedicated server learns its group from the session
    it is allocated for, as a client does. When that group's maps are not mounted, it fetches them and holds joins with the maps
    channel's own join hold, independent of `network.join_hold_ms` (which stays the levels-settle hold), because defaults live in
    code and hosting needs no configuration. The hold's timeout comes from the set's size and the measured download rate, capped by
    a named ceiling. At the timeout the session starts on stock content with a Warning, because a host must never hang a session on
    a download. A `guilds: any` server fetches only the group it is allocated for. What would change it: the registration answer
    returning the resolved group ids, which lets a server prefetch its listed guilds before allocation (#2743).
18. **A server mounts its groups' scripted maps on the manifest signature.** There is no allow list: the signature is the
    channel's one authority. Under `-require-signed-plugins` decision 7 applies to a server as to a client, so only a manifest
    signed by a compiled-in official key counts. What would change it: accepting decision 7's official key (decision 12), which
    changes which manifests count under the flag, not this rule.

## What is built

The core is built into `nevr_core` and tested by its GTests (`src/core/maps.cmake`): the formats and the minisign verifier
(decisions 1, 3, 11, 14, 15), the fetch, the resume, the recipe rebuild and the cached mount over main's `nevr_cargo::Env`
(decisions 2, 4, 10, and the cache index of decision 16), the registry's map entry points and the script image rules (decisions 5
and 6), the config aliases' pure rules (decision 5), the `signed_only` refusal of a map whose manifest is not official (decision
7), and the request queue (`map_fetch_queue.h`). Not built yet (#2733): the PC and Quest platform layers and the channel that
starts them, including which keys they give the core as official (decision 7) and the `mixed_keys` check (decision 3); the
config-alias hooks; the local-copy precedence (decision 8); the session pin (decision 9); the server's group fetch and its join
hold (decisions 13 and 17); and the server mount (decision 18).

## Consequences

- With no `maps.base_url`, nevr starts no thread, makes no directory, installs no hook and writes one Info line saying the channel
  is not configured (not built yet, #2733).
- The channel reaches files through two seams. Main's `nevr_cargo::Env` (`src/core/cargo_env.h`) is the texture cargo's own:
  `ReadSmallFile`, `MakeDirs`, `WriteFileAtomically`, `OpenAppend` (`Append`, `Truncate`, `Sync`), `QueryDevice`, `NewHasher`,
  `Get` and `Event`; the channel adds nothing to it and has no temporary file: a fetch appends each verified piece to a member file
  in a staging directory, cuts back a failed piece's bytes with `Truncate`, and renames the directory into place. `nevr_maps::MapFs`
  (`map_types.h`) adds what only a map needs: a held share-read handle and its stamp, its final path, a directory listing, a
  removal and a rename of a whole directory. The hasher is the portable SHA-256 (`src/quest/data/sha256.h`): the Quest `Env`
  (`src/quest/cargo/posix_env.cpp`) uses it today, and the PC `Env` the platform commit adds uses the same one, never BCrypt.
- The limits are ADR 0045 decision 8's: one document's bytes, a recipe's buffers and the groups the cache keeps are shares of the
  device's memory and free space (`MapDocumentBytes`, `map_manifest.h`), never a constant. What a format defines stays constant in
  `map_format.h`, which cites the format.
- The cache lives in `<root>\cache\maps\` (`src/core/nevr_root.h`), outside `content\` and outside the files `asset_cache`
  enumerates.
- An attacker who can rewrite `%LOCALAPPDATA%\nevr` and restore file stamps can already replace nevr (ADR 0010): not defended.
- Expiry blocks scripted maps only, so script-free maps keep working offline.
