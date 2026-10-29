# Fetching and turning on a content pack (PC)

A content pack (`content-packs.md`) that changes the game's data is a `content-pack` artifact of the signed release manifest
(`verifying-a-release.md`, Step 8). It is hosted on `evr.echo.taxi` under `packs/`, never on a GitHub release page
(ADR 0028). This page is for an installer or launcher that fetches one and for a server host who turns one on.

## Fetch and place

1. Run steps 1 to 7 of `verifying-a-release.md` and take the chosen release.
2. Take its one `content-pack` artifact with the pack's `id` (zero or two is a failure). A release that lists none has no pack.
3. Check `path` is `packs/<id>-<first 12 hex of sha256>.zip` before any request, read at most `size` bytes, and require the
   exact `size` and `sha256`.
4. Read `content_pack.requires_stock_manifest_sha256`. Hash the player's own stock manifests under
   `_data/<content_pack.stock_data_version>/rad15/win10/manifests/` in the game install and require every listed hash to be one
   of them. If one is missing, the install is not the stock data the pack was built from: stop, place nothing, say which hash.
   The manifest cannot check this claim; the consumer does.
5. Extract only the paths listed in `contents`, each under `%LOCALAPPDATA%\nevr\echovr`: `content\<id>\...` and `plugins\...`.
   An entry that is not listed, or whose path leaves that folder, is a failure of the whole extract. Nothing is written into the
   game install (ADR 0012).
6. The placement turns nothing on. The installer does not edit `config.yaml`.

## Turn it on (a server host, or a player on a server that runs the pack)

A pack applies only when `config.yaml` (in the nevr root, or the file `-config-file` names) lists its serving plugin with the
pack's id, and the pack's own plugins. `NvrContentOverlay` is the serving plugin for EchoCombat; it has to load before the game
reads its data, so it is `early: true` (`content-packs.md`, "Load early"):

```yaml
plugins:
  - name: "NvrContentOverlay"
    file: "NvrContentOverlay.dll"
    early: true
    args: {"active": "echocombat"}
  - name: "DroneFix"
    file: "DroneFix.dll"
  - name: "PowerBoost"
    file: "PowerBoost.dll"
  - name: "VeilNet"
    file: "VeilNet.dll"
```

Keep the rest of the file. Install the pack on the game server and on every client that plays on it: the pack changes game
data, so a stock server and a pack client disagree, and the other way round. Remove the `NvrContentOverlay` entry (or its
`active` arg) and the game is stock again.

### A server that hosts combat loads the plugins

The plugins are not client-only. EchoCombat's own table lists DroneFix, PowerBoost and VeilNet as running on the server and
on the clients: PowerBoost decides OVERCHARGE's damage bonus and cheat death on the server, DroneFix runs the drone's
follow-update fix, and VeilNet relays UMBRAL VEIL, NEUTRON, TRANSLOCATOR and OVERCHARGE events between players through the
server. A server that hosts combat with the pack therefore lists all four entries above in its own `config.yaml`, not only
the serving plugin.

## Check that it is on

A pack lives at `content\<id>\` in the nevr root (ADR 0012); the serving plugin reads it from there.

- nEVR's log has a `Loaded:` line for each plugin (`[NEVR.PLUGIN]`).
- The serving plugin's own log says it is serving the pack. An overlay that logs through the host writes it to the run log as
  `[NEVR.PLUGIN] plugin=NvrContentOverlay ...` (`docs/standards/logging.md`, Rule 15); an older overlay writes its own file under
  `plugin_logs\NvrContentOverlay\` in the folder it was loaded from. A refusal names the cause: a stock manifest that is not
  the one the pack was built from, a pack file that changed, or a load that came too late (the entry lacks `early: true`).
