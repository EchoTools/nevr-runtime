# Server mode activation in nevr-runtime

## Files reviewed
- `/home/andrew/src/nevr-server/src/gamepatches/patches.cpp`
- `/home/andrew/src/nevr-server/src/gamepatches/patch_addresses.h`
- `/home/andrew/src/nevr-server/src/legacy/gamepatches/patches.cpp`
- `/home/andrew/src/nevr-server/src/legacy/gameserver/gameserver.cpp`

## Activation mechanism (current gamepatches)
Server mode is **explicitly gated by the `-server` CLI flag**.

Evidence (current `gamepatches/patches.cpp`):
- `BuildCmdLineSyntaxDefinitionsHook` registers `-server` as a valid argument.
- `PreprocessCommandLineHook` parses args and sets `isServer = TRUE` when `-server` is present.
- When `isServer` is true, it calls:
  - `PatchEnableServer()` (forces dedicated server mode patches)
  - `PatchDisableLoadingTips()` (avoid loading tips resources in server mode)

`-server` is mutually exclusive with `-offline`, and `-noconsole` requires `-headless`.

### What PatchEnableServer does
`PatchEnableServer()` applies several binary patches, including:
- **SERVER_FLAGS_CHECK**: ORs server mode flags (bit 2 load sessions from broadcast, bit 3 dedicated server flag) so dedicated server mode is always set.
- **ALLOW_INCOMING**: Forces `allow_incoming` in netconfig to true (accept connections).
- **SPECTATORSTREAM_CHECK**: NOPs the check for `-spectatorstream` so the server can enter load-lobby state without that arg.

These patches only run if `-server` is provided.

## Activation mechanism (legacy gamepatches)
Legacy `gamepatches` has the same behavior:
- `-server` is added to CLI syntax
- `PreprocessCommandLineHook` sets `isServer = TRUE` when `-server` is present
- `PatchEnableServer()` is called when `isServer` is true

## ServerDB initialization (gameserver)
`legacy/gameserver/gameserver.cpp` shows that server registration and ServerDB connection occur in `GameServerLib::RequestRegistration` and use `serverdb_host` from config (default `ws://localhost:777/serverdb`).

This is downstream of server-mode activation and does **not** control whether server mode is entered. It only configures where the server registers once the game is already in server mode.

## Conclusion: what is missing
Server mode is **not** activated by `-mp`/`-level`. It is activated by the **`-server` flag**, which the harness does not pass.

### Likely missing flags
- `-server` (required to set `isServer` and apply dedicated server patches)
- Optional: `-headless` (if running without graphics/audio)
- Optional: `-noconsole` (requires `-headless`)
- Optional: `-config-path <path>` (if you need a custom config for serverdb_host, etc.)

### Key implication
Without `-server`, the patches that force dedicated server mode (server flags + allow_incoming + spectatorstream bypass) never apply, so the game remains in client mode even when using `-mp` and `-level`.
