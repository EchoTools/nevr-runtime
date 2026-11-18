# Custom maps channel: what it does, how to configure it, what to look for

The custom-map channel delivers a lobby group's maps: the client or the dedicated server fetches a signed manifest for its
group, verifies it, and mounts the maps: the cached ones at launch, and a map fetched during a session in the same run. Maps only add resources; no game bytes come from the CDN. The
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
  session requests, merged with the groups the cache index holds, filtered by `FetchGroups`. Each group is fetched on
  demand (see the next section).
- Before that pass, the worker drains the request entry (`DrainRequests`): each queued group runs with the scope its
  requests asked for. A lobby session success queues its group, and the session pin asks for a map it lacks. Both go
  through `InstallClientRequests` (`src/runtime/content/map_channel.cpp`), which installs `nevr_maps::MapRequests`
  (`src/core/map_request.h`) on the two core seams and replaces the session-group seam that `ArmSessionGroupRequest`
  (`src/core/map_channel.cpp`) set. Each request is written as a `map_request` record (`docs/standards/logging.md`).
  Without a running channel the session logs `no_channel`.
- A request wakes the worker through `FetchWorker::Request`. A request made within `kExtraFetchGapMs` (10 minutes) of the last
  extra pass is refused by the gap, and the worker runs one pass when that gap ends (`FetchWorker::WaitForPass`, a timed wait
  on the worker's own thread): a refused request is never left queued with no pass to run it. Requests inside one gap share
  that one pass, and a request that arrives during a pass is run by the next one.
- `StopFetch` (`src/runtime/content/map_channel.cpp`) stops the request entry first, so later requests are refused and
  the waiting ones are answered `cancelled`. It has no caller, so the PC shutdown path does not stop the fetch.
  KNOWN-WRONG #TBD.

### On-demand fetch and the request entry (ADR 0048 decision 16)

- A client's pass is on demand. `Channel::FetchPass` (`src/core/map_channel.cpp`) and every request set
  `FetchScope::on_demand`. A map the cache holds is refreshed when its listed version is newer, and a map the cache does
  not hold is downloaded only when a request names it. A map that is neither held nor requested is recorded as
  `map_plan reason=not_needed` and not fetched.
- A request that names a map the group's signed manifest does not list is refused with `map_plan reason=not_listed`.
- A cached copy whose version no verified manifest lists is refused: the cache is not mounted in its place. The copy is
  recorded as `map_mounted refused=stale` (Warning), the listed version is fetched when a session asks for it, and the
  session pin's decision for a stale copy is a download notice (`src/core/map_session_pin.cpp`).
- `ChannelConfig::mount_on_demand` (`src/core/map_run.h`) defaults to `false`: the boot mount grants every cached map its
  groups list. A map fetched during a session mounts in the same run: the fetch worker's request pass calls
  `Channel::MountGroups` with `MountScope`, which grants only the ids the request named (`mount_on_demand` is the scope's
  switch; an unasked map is recorded as `map_skipped reason=not_asked`, Info), registers each granted map's set, and calls
  `nevr_resource_sets::MountLaterSets` (`src/runtime/patch/resource_sets.h`). The game's own thread then runs the later mount
  on its next frame (`OnFrame`), once the game has asked its first resource; nothing waits for it, and a frame with no
  later mount pending costs one atomic load. No config key sets `mount_on_demand` (ADR 0048 decision 16).
- A later mount builds ONE new shadow of every mounted set, earlier and new, on fresh chunk indices (`Registry::MountLater`,
  `src/core/resource_set_registry.h`), and the resolver is given it in place of the earlier one, so a request is still
  answered by one lookup in one shadow. A mounted set is never replaced or withdrawn. Each later mount keeps the shadow it
  replaces for the process (a request in flight may still read from it), so the process holds one retired shadow for each
  later mount; the growth is bounded by the number of maps a run fetches. A map's set registered after the first mount is
  held until the next later mount; a plugin's set registered after the first mount is still refused `too_late`, and so is
  any set once the run's registration was abandoned (the resolver hook was never installed).
- The mount observer is told of the run's first mount through `Notify` and of each later mount through `NotifyLater`, with
  that mount's own report (`MountReport::earlier_sets` says how many sets came before). The map channel's observer grants the
  maps that mount took and, on a dedicated server, releases the join hold for the groups that mount covered: the hold is
  released from the observer, never from the worker that fetched.
- The request entry's download notice follows what its mount did (`FetchReport::mounted_now`, `FetchReport::superseded`, set by
  `ServeSessionRequests`): a map the mount grants reads "join again when it is ready" and mounts during this run; a map this run
  already mounts at another version (`mounted_other_version`) reads "the update ... applies the next time you start the game", since
  the mounted version stays until the game starts again. A group request that does both says both.
- The session pin (`SetChannelView`, `SetMountedPins`) has no production caller on the PC, so a client does not yet
  decide a version skew from the mounted pins. KNOWN-WRONG #TBD (K6).

### Quest client

- `StartFromConfig` (`src/quest/cdn/map_start.cpp`) is called from the sentinel's integration step that runs after the
  `CFile` request-log hook installs (`src/quest/integration/production_steps.cpp`). It calls `Start`
  (`src/quest/cdn/map_service.cpp`).
- `Start` arms the session request (`ArmSessionGroupRequest`) and its background pass fetches the same
  `RecordedGroupSeams` groups as the PC.
- The Quest's mounter is built with `signed_only` false (`src/quest/cdn/map_service.cpp`).
- The Quest's pass is the core's on-demand `FetchPass`, so it refreshes held maps and downloads only what its groups ask
  for, as the PC does. It has no request entry: a session's group is recorded for the pass, and a map the session pin
  lacks is not requested. KNOWN-WRONG #TBD (K6: the Quest request entry).
- A map the pass stored mounts in the same run, on the pass's own thread: `Start` calls `Channel::MountGroups` for the
  groups that stored a map, then `MountLaterNow` (`src/quest/cdn/map_service.cpp`), which calls `map_sets::MountLater`
  (`src/quest/sentinel/map_sets.h`) and hands the resource request hook (`PublishMapSets`) the one new shadow of every
  mounted set. The same registry rules as the PC apply: no map is withdrawn once mounted, and each later mount keeps the
  shadow it replaced for the process. The Quest records the later mount as `map_set_mount` with `later=true`.

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
- **PC view:** a client's boot publishes the view of its boot report, and each request its worker serves publishes the listing
  of its group (`src/core/map_request_serve.h`, called from `src/runtime/content/map_channel.cpp`). A dedicated server publishes
  no view.
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
  worker for the group through `RequestServerGroup`, which `SetRequestFetch` registers. A session with no group logs `map hold
  off` and arms nothing. A group already mounted in this process is checked again at every session create (`map check`): the
  joins wait for its manifest check, so a version the channel published since is never hidden behind the old one.
- A map found at a new version while its old one is mounted cannot be mounted in the process (a mounted map is never replaced):
  a Warning `map update` names the maps, and the hold stands with no deadline until the session ends, so no join reaches the
  old version. The server hosts the new version after it is restarted. A failed check of a mounted group (`map check failed`)
  lifts the hold, and the session goes ahead on what is mounted. KNOWN-WRONG #TBD: nothing restarts the server by itself.
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
| `map script refused reason=<token>` | A map script is not served. The token is in `core/map_script_check.h` or `core/script_image_rules.h`. |
| `map_skew reason=version` / `reason=not_mounted` | The client is leaving a session whose map version it does not match. |
| `map_pin status=off reason=<token>` | The session pin did not install; the token names the check that failed. |
| `session group <outcome> group=<id>` | The lobby session's group, and whether a request was made. |
| `map hold armed` / `map hold released` / `map hold timeout` | A server's maps hold: armed, lifted by the mount, or timed out. |
| `map check group=<id>` / `map update group=<id> ids=<ids>` / `map check failed group=<id>` | A server's session create checks a mounted group: the check, a new version of mounted maps (joins wait until restart), a check that could not be made. |
| `signed_only group=<id> scripts_refused=<n>` | Scripts refused under `-require-signed-plugins`. |

Map file contents are never logged. A script's bytes are never logged. A path or an owner is a quoted field.

## Known gaps, in one list

Every `KNOWN-WRONG #TBD` on this page marks a part the code states is not wired yet:

- `StopFetch` has no caller, so the PC shutdown path does not stop the background fetch.
- The Quest has no request entry (K6).
- `RegistrationEntries` has no caller, so a server does not report its pin at registration.
- The Quest's `InstallTick` has no caller, so the Quest does not run the pin's tick.
- No local level name or local map id is supplied on either platform, so local precedence does not skip any CDN map.
