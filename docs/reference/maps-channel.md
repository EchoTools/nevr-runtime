# Custom maps channel: what it does, how to configure it, what to look for

The custom-map channel delivers a lobby group's maps: the client or the dedicated server fetches a signed manifest for its
group, verifies it, and mounts the maps at the next launch. Maps only add resources; no game bytes come from the CDN. The
decisions are in `docs/adr/0048-custom-maps-channel.md`. This page states what the tree does today. Where the code says a
part is not wired yet, the place is marked `KNOWN-WRONG #TBD`; an issue number is filed later.

## Turning it on

The channel runs only when `maps.base_url` is set. With it unset (the default):

- one Info record `[NEVR.MAPS] map_start reason=not_configured` is written;
- no thread is started, no directory is made, no hook is installed.

Everything below assumes `maps.base_url` is set.

## Configuration keys

The keys live in the `maps:` section of `config.yaml` in the nEVR root (`%LOCALAPPDATA%\nevr\echovr`). Every key is
optional. `docs/reference/example-config.yaml` shows the section.

| Key | Type | Default | What it does |
| --- | --- | --- | --- |
| `maps.base_url` | string (URL) | empty: channel off | The base URL. A trailing `/` is added when it is missing. A group's manifest is read from `guilds/<group id>/manifest` under it (`GroupManifestUrl`, `core/map_run.h`). |
| `maps.channel` | string | empty | The channel name the base URL is bound to. Empty: the name of the first verified manifest (`core/map_channel.h`). |
| `maps.boot_budget_ms` | non-negative integer, milliseconds | 0: no limit | How long one boot mount may take. A map that would start after the budget is spent is skipped with `reason=boot_budget`. |
| `maps.cache_cap_gib` | non-negative integer, GiB | 20 | The map cache's cap. A fetch that would pass it first removes the least recently played maps (never one this run mounted); when that frees too little the map fails `over_budget` and the request says so. Old versions of an updated map are removed after the index commit. 0: no cap (the device's storage share still applies). |
| `maps.official_keys` | list of minisign public keys | empty | Keys in official trust. They are never compiled in: the build compiles in no official key (`CompiledOfficialKeys`, `core/map_channel.cpp`). |
| `maps.trusted_keys` | list of minisign public keys | empty | Author keys. They are not official. |

Rules the core applies (`core/map_channel.cpp`, `ReadSettings`):

- Official and author keys together are refused (`reason=mixed_keys`, Warning). Official keys are compiled in or set in
  `maps.official_keys`; a key from `maps.trusted_keys` never makes a manifest official.
- A key that does not parse is refused (`reason=bad_key`, Warning). The record names the key and its list index, never the
  key text.
- A refused configuration mounts nothing and fetches nothing for the run.

Platform differences, as the code reads them:

- **PC** (`src/runtime/lifecycle/service_map.cpp`) reads the keys as flat values. The key lists are read as CSV, like
  `guilds`. `maps.boot_budget_ms` goes through `strtoul`, so a value that is not a number reads as 0 with no Warning.
- **Quest** (`src/quest/sentinel/quest_config.cpp`, `ReadMaps`) rejects a value of the wrong kind with one Warning that
  names the key and not its value, and the default stays. A list that is not a YAML sequence is rejected.

The section has no cache-size key, no subscription key and no server allow-list key. No config key chooses which maps a
player or a host has (ADR 0048 decision 16; the comment at `src/core/nevr_config.cpp`).

## Cache location

Downloaded maps live in `<nEVR root>\cache\maps\` (`MapsCacheDir`, `src/core/nevr_root.h`). That directory is outside
`content\` and outside the files the asset cache enumerates. On the PC the channel's directory is set from `MapsCacheDir`
(`src/runtime/content/map_channel.cpp`). The Quest's cache directory is passed in by its own startup
(`MakeChannelConfig`, `src/quest/cdn/map_service.cpp`); this page does not verify its path on the headset.

## What runs today

### PC client

- The boot calls the channel's boot mount (`nevr_map_channel::pc::Boot`) after the plugins load, then its background
  pass starts (`StartFetch`), both from `src/runtime/lifecycle/boot.cpp`. The boot mount reads the cache index and mounts
  cached groups with no network.
- The background pass fetches the groups `RecordedGroupSeams` names (`src/runtime/content/map_channel.cpp`): this run's
  session requests, merged with the groups the cache index holds, filtered by `FetchGroups`.
- A lobby session success queues its group for that pass. `ArmSessionGroupRequest` (`src/core/map_channel.cpp`) sets
  `SetSessionGroupRequest` to `RecordRequestedGroup` once the boot returns a running channel. The session logs
  `session group requested group=<id>`; without a running channel it logs `no_channel`.
- `StopFetch` (`src/runtime/content/map_channel.cpp`) has no caller, so the PC shutdown path does not stop the fetch.
  KNOWN-WRONG #TBD.

### Quest client

- `StartFromConfig` (`src/quest/cdn/map_start.cpp`) is called from the sentinel's integration step that runs after the
  `CFile` request-log hook installs (`src/quest/integration/production_steps.cpp`). It calls `Start`
  (`src/quest/cdn/map_service.cpp`).
- `Start` arms the session request (`ArmSessionGroupRequest`) and its background pass fetches the same
  `RecordedGroupSeams` groups as the PC.
- The Quest's mounter is built with `signed_only` false (`src/quest/cdn/map_service.cpp`).

### Local copies (ADR 0048 decision 8)

The core decides local precedence (`src/core/map_local_precedence.h`, `DecideLocalPrecedence`; used in `src/core/map_run.cpp`):
a CDN map whose id or level name matches a local copy is skipped with `map_skipped reason=local_map`. The names come from
`ChannelConfig::local_level_names` and `local_map_ids`, and neither platform supplies them: `local_level_names` is never
assigned in production code, the PC boot passes an empty list (`src/runtime/content/map_channel.cpp`), and the Quest boot
passes `{}` (`src/quest/cdn/map_service.cpp`). So today the channel skips nothing as local. KNOWN-WRONG #TBD: no reader of
hand-installed copies exists on either platform. The session pin's own `local_map` record (PC only, below) does work.

### Session pin (ADR 0048 decision 9)

The pin is `id@version`. A client whose map version differs from the session's pin leaves before it loads. Two cases do not
leave: a session that states no pin (`map_unpinned`), and a client with a local copy of the session's map (`local_map`, PC).

- **PC login:** the login carries `nevr_maps` when a pin is set (`LoginEntries`, `src/runtime/compat/ws_bridge.cpp`).
- **PC tick:** `nevr_map_session_pin::Tick` runs on the game thread every frame (`src/runtime/frame/tick.cpp`).
- **PC registration:** `RegistrationEntries` has no caller. KNOWN-WRONG #TBD: a server does not report its pins at
  registration.
- **Quest login:** the login's `map_report` is set to `LoginEntries` (`src/quest/integration/production_steps.cpp`).
- **Quest tick:** `InstallTick` (`src/quest/cdn/map_pin_game.cpp`) has no caller. KNOWN-WRONG #TBD.

### Dedicated server

- The PC boot runs the channel on a dedicated server too: `src/runtime/lifecycle/boot.cpp` passes `is_server` to
  `nevr_map_channel::pc::Boot`. `StartFetch` starts a server worker (`StartServerWorker`, `src/runtime/content/map_channel.cpp`)
  instead of the background pass, and that worker fetches only the group the server is allocated for.
- The session create arms the worker. `nevr_map_hold::OnSessionCreate` (called from `src/runtime/server/gameserver_callbacks.cpp`)
  takes the allocated group, arms the maps hold (`src/core/map_server_hold.h`, `src/runtime/server/map_hold.cpp`), and asks the
  worker for the group through `RequestServerGroup`, which `SetRequestFetch` registers. A session with no group, or with a
  group that is already mounted, logs `map hold off` and arms nothing.
- The hold lifts when the group mounts (`map hold released`) or times out (`map hold timeout`; the session then starts on
  stock content). It is separate from the levels hold (`network.join_hold_ms`, `docs/reference/join-hold.md`).
- Without `maps.base_url` no request function is set, so no hold is armed.

### Signed-only runs

When the PC runs under `-require-signed-plugins`, the channel's configuration is marked signed-only
(`src/runtime/content/map_channel.cpp`). No official key is compiled in, so every map script is refused under that flag
(`signed_only`), and script-free maps still mount (ADR 0048 decisions 7 and 12).

## Timeout of a server's maps hold

The timeout comes from the set's size and the measured download rate. It is capped by these bounds
(`src/core/map_server_hold.h`), which are not measured on a live server:

| Bound | Value | Reason |
| --- | --- | --- |
| Ceiling | 120 s | The `--login-timeout` default of the repository's client login run. |
| Floor | 5 s | The least time a hold waits, so a set the cache already holds still waits for its manifest's round trip. |
| Slack | 2 s | The manifest round trip and the mount, added to the transfer time. |

No config key sets the timeout. `network.join_hold_ms` does not change it. The join-hold page's "The maps hold" section
(`docs/reference/join-hold.md`) gives the timeout's behaviour and its prologue checks.

## Log lines to look for

All records carry the tag `[NEVR.MAPS]`. The full table, with each event's fields and levels, is the `[NEVR.MAPS]` row of
`docs/standards/logging.md`. The lines most useful to an operator:

| Line | Meaning |
| --- | --- |
| `map_start reason=not_configured` | No `maps.base_url`: the channel does not run. |
| `map_start reason=ok` | The channel runs; `mounted`, `deferred` and `local` count its maps. |
| `map hold off reason=no_group` | A dedicated server's session names no group to fetch: the session starts on the stock maps. |
| `map_start reason=bad_key` / `reason=mixed_keys` | The configuration is refused; nothing is mounted. |
| `map_skipped reason=local_map` | A CDN map is skipped because a local copy wins. The local name is in `local_name`. |
| `map_mounted refused=stale` | The cache holds a version no verified manifest lists now; the map does not mount until the next fetch stores the listed one. |
| `map script refused reason=<token>` | A map script is not served. The token is in `core/map_script_check.h` or `core/script_image_rules.h`. |
| `map_skew reason=version` / `reason=not_mounted` | The client is leaving a session whose map version it does not match. |
| `map_pin status=off reason=<token>` | The session pin did not install; the token names the check that failed. |
| `session group <outcome> group=<id>` | The lobby session's group, and whether a request was made. |
| `map hold armed` / `map hold released` / `map hold timeout` | A server's maps hold: armed, lifted by the mount, or timed out. |
| `signed_only group=<id> scripts_refused=<n>` | Scripts refused under `-require-signed-plugins`. |

Map file contents are never logged. A script's bytes are never logged. A path or an owner is a quoted field.

## Known gaps, in one list

Every `KNOWN-WRONG #TBD` on this page marks a part the code states is not wired yet:

- `StopFetch` has no caller, so the PC shutdown path does not stop the background fetch.
- `RegistrationEntries` has no caller, so a server does not report its pin at registration.
- The Quest's `InstallTick` has no caller, so the Quest does not run the pin's tick.
- No local level name or local map id is supplied on either platform, so local precedence does not skip any CDN map.
