# The custom-map channel

How nevr fetches, verifies and mounts subscribed custom maps (`docs/adr/0047-custom-maps-channel.md`). The shared types are in
`src/core/map_types.h`.

## Config

| Key | Meaning |
| --- | --- |
| `maps.base_url` | The channel's bucket URL. Required: there is no compiled default host. |
| `maps.subscribed` | The map ids to fetch and mount. Absent or empty: the channel is off. |
| `maps.trusted_keys` | Minisign public keys (author or dev) trusted for this `base_url`; each is logged at Warning. Never mixed with the official keys on one `base_url`. |
| `maps.cap_mib` | The cache cap; a map that would pass it is skipped. |
| `maps.server_allow` | Map ids (optionally `id@index_sha256`) whose scripts a server may run. A server mounts a map only if every script is allowed. |

## Bucket

```
<base_url>/blobs/sha256/<64 hex>     pieces and package indexes (immutable)
<base_url>/v1/maps/manifest.signed   the current channel manifest: the minisig, a "--" line, then the body
<base_url>/v1/maps/manifests/<serial>.signed
```

Member paths are exact: `set/<platform>.nevrres`, then `scripts/<hash16>.dll` (PC) or `scripts/lib<hash16>.so` (Quest). Map ids
match `[a-z][a-z0-9_]{0,54}`; `level_name` is `<id>` on PC and `<id>_lowspec` on Quest.

## Cache

```
<root>\cache\maps\
  index.json                                      nevr-maps-index v1: the floors and every verified version; written last
  manifest.signed                                 the last verified channel manifest
  <id>\<platform>\<index sha12>\package.json
  <id>\<platform>\<index sha12>\set\windows-x86_64.nevrres
  <id>\<platform>\<index sha12>\scripts\<hash16>.dll
  <id>\<platform>\<index sha12>.tmp\              assembly in progress; removed by GC
```

## Events

Every line is `[NEVR.MAPS] <event> key=value ...`, with a `reason=` or `refused=` token when something is not done.

| Event | When |
| --- | --- |
| `map_start` | The channel starts, or does not (`not_configured`, `busy`). |
| `map_manifest` | A manifest fetch and verify. |
| `map_boot` | The stored manifest at boot. |
| `map_deferred` | A version whose stamp differs waits for a background re-hash. |
| `map_mounted` | A map's set and scripts registered, or the reason they were refused. |
| `map_skipped` | A map not mounted for a reason outside its own bytes (`local_map`). |
| `map_summary` | One Warning line listing the refused ids and their tokens. |
| `config_alias` | An alias refused, or a conflict between two loaded maps (`winner=`, `loser=`). |

## Reason tokens

| Token | Meaning |
| --- | --- |
| `not_configured` | No `maps.subscribed`: nothing runs. |
| `busy` | Another process on the same root is fetching; no fetch this run. |
| `mixed_keys` | Official and author keys configured for one `base_url`; refused. |
| `http_<status>`, `transport` | The fetch failed; the cache is untouched. |
| `over_cap` | A download passed its cap, or a map would pass `maps.cap_mib`. |
| `bad_signature`, `unknown_key`, `malformed` | The manifest does not verify; the stored one is kept. |
| `serial_down`, `version_down` | A replay or a downgrade; refused. |
| `expired` | Past `expires`: maps with scripts are not mounted. |
| `unsupported_game`, `stock_mismatch` | The map was built for another game build or stock manifest; skipped. |
| `revoked` | Revoked, or missing from the manifest; not mounted, then deleted by GC. |
| `piece_hash`, `member_hash` | A piece (after 3 tries) or an assembled member does not match its sha256. |
| `no_manifest` | No stored manifest at boot; nothing mounts. |
| `stamp` | The file on disk is not the file that was verified. |
| `rekey_mismatch` | A rekeyed script's stock file differs. |
| `local_map` | A local `.kfclvl` holds the same level; the CDN copy is skipped. |
| `alias_missing` | The alias key is not in the map's mounted set; the pair is dropped. |
| `stock_hash`, `pack_hash`, `duplicate` | A script whose hash is a stock script, a content-pack script, or another map's. |
| `signed_only` | `-require-signed-plugins` and the script is not official-signed (ADR 0047 rule 7). |
| `server_not_allowed` | A server and the script is not in `maps.server_allow`. |
| `pe_not_pe`, `pe_not_dll`, `pe_not_amd64`, `pe_truncated` | The script is not a whole PE32+ AMD64 DLL. |
| `pe_imports`, `pe_delay_imports`, `pe_bound_imports`, `pe_iat`, `pe_tls`, `pe_clr` | A directory that must be empty is not. |
| `pe_entry_point` | `AddressOfEntryPoint` is not 0. |
| `pe_no_exports`, `pe_export_count`, `pe_export_name`, `pe_export_forwarded`, `pe_export_not_code` | The export is not exactly one unforwarded `setup_bindings` in code. |
| `pe_section_name`, `pe_section_wx` | A section outside the allowlist, or both writable and executable. |
| `kStockKey`, `kCollides`, `kOverride`, `kUnresolved`, ... | The registry's existing mount refusals, by `RefusalName`. |
