# Fetching and placing a content pack (PC)

A content pack (`content-packs.md`) that changes the game's data is a `content-pack` artifact of the signed release manifest
(`verifying-a-release.md`, Step 8). It is hosted on `evr.echo.taxi` under `packs/`, never on a GitHub release page
(ADR 0028). This page is for an installer or launcher that fetches and places one, and for a server host.

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
6. The placement is the switch: an installed pack is served at the next start (`content-packs.md`). The installer does not
   edit `config.yaml`.

## Serving, and running stock

The runtime serves a pack it finds at `content\<id>\` in the nevr root, on a client and on a server; no `config.yaml` entry
is needed (`content-packs.md`). A pack changes game data, so a client and a server agree only when both carry it. Every
server runs nevr-runtime and carries the pack, so a player with the pack finds servers that match; that is why a placed
pack is served rather than shipped off. To run the game stock, remove the pack's folder `content\<id>\`.

A pack that carries scripts needs a run without `-require-signed-plugins`: a signed-only run refuses it whole.

## Check that it is served

nEVR's own log (`<root>\logs\`) says what the runtime decided, in one `[NEVR.CONTENT]` line at start:
`content pack served id=<id> ...`, `content pack refused id=<id> reason=<token> ...` (the reasons are listed in
`content-packs.md`, "When a pack is refused") or `no content pack installed`. While the game loads it writes one
`manifest served`, `package served` or `script served` line for each file it serves.
