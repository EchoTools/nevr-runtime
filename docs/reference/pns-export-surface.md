# The platform-service DLLs' export surface (pnsrad, pnsovr, pnsdemo)

The game reaches each provider DLL only by looking exports up by name (`CNSProvider`/`CPlatformService`
dispatch, e.g. `MicRead` through `GetMethodProc`), so a DLL's export table is its whole interface. This is
that table, read from the shipped files with `tools/pe_exports.py` (a read-only PE parser; it never loads
the DLL). Regenerate with:

```
python3 tools/pe_exports.py echovr/bin/win10/pnsrad.dll            # one table per DLL, with shared bodies
python3 tools/pe_exports.py --compare echovr/bin/win10/pnsrad.dll echovr/bin/win10/pnsovr.dll
python3 tools/pe_exports.py --json echovr/bin/win10/pnsovr.dll
```

Measured on `pnsrad.dll` sha256 `2410095d3b43d408…` (the build `src/runtime/patch/provider_identity.h` pins),
`pnsovr.dll` `4a5ccaf59bf7b89b…`, `pnsdemo.dll` `3a238214aaf52482…`.

| DLL | named exports | distinct bodies |
| --- | --- | --- |
| `pnsrad.dll` | 41 | 33 |
| `pnsovr.dll` | 52 | 47 |
| `pnsdemo.dll` | 19 | 19 |

## pnsrad vs pnsovr

38 names are in both. Only in `pnsrad.dll` (3): `Activities`, `Friends`, `Party`. Only in `pnsovr.dll` (14):
`CheckEntitlement`, `CrashReportUserName`, `IAP`, `ModalUIVisible`, `RichPresence`, `Social`, `VoipAnswer`,
`VoipAvailable`, `VoipCall`, `VoipHangUp`, `VoipMute`, `VoipPushToTalkKey`, `VoipRead`, `VoipUnmute`.

So an OVR-era behaviour that pnsrad does not provide at all, because it has no export of that name: the
call-based voice path (`VoipCall`, `VoipAnswer`, `VoipHangUp`, `VoipMute`, `VoipUnmute`, `VoipRead`,
`VoipAvailable`, `VoipPushToTalkKey`), rich presence, the OVR social object, entitlement and IAP checks,
crash-report user name and the modal-UI query. Whether the game ever looks those up on the RAD provider is
not shown by the table; it needs the call sites (ReVault `CPlatformService::GetMethodProc` callers).

## Bodies that are shared (identical-code folding or one stub)

`pnsrad.dll`:

| Address | Exports | Body |
| --- | --- | --- |
| `0x00085fb0` | `MicDestroy`, `MicStart`, `MicStop`, `Update` | `ret 0` (a no-op) |
| `0x00088d10` | `MicAvailable`, `MicCreate`, `MicDetected`, `MicRead` | `xor eax,eax; ret` (returns 0) |
| `0x00088d50` | `MicSampleRate`, `VoipSampleRate` | reads one global |
| `0x00088d70` | `ProviderID`, `UserProviderID` | reads one global |

`pnsovr.dll`: `AllowGuests`, `ModalUIVisible`, `VoipSetBitRate` (`0x000842b0`, returns 0); `MicAvailable`,
`MicCaptureSize` (`0x00097370`, returns 0x960); `MicSampleRate`, `VoipSampleRate` (`0x00097470`);
`ProviderID`, `UserProviderID` (`0x000974a0`).

A per-export hook therefore cannot tell `MicRead` from `MicCreate` in pnsrad by address: both names are the
same function. This is why the Mic provider (`src/runtime/patch/mic_provider.cpp`) installs through the
game's symbol lookup rather than by patching a body, and it is the constraint any call tracer has to
design around (trace at the lookup, or accept that folded names report as one).

## Other facts the table settles

- `Friends`, `Party` and `Activities` are exported by `pnsrad.dll` and by neither `pnsovr.dll` nor
  `pnsdemo.dll`; they are the social objects the runtime's facade stands in for. `pnsdemo.dll` has no
  `Mic*` or `Voip*` exports at all and exports `Social` (as `pnsovr.dll` does).
- In `pnsrad.dll` the real (non-stub) Mic exports are `MicBufferSize`, `MicCaptureSize` and `MicSampleRate`;
  `MicAvailable`, `MicCreate`, `MicDetected`, `MicRead` are the `return 0` stub and `MicDestroy`, `MicStart`,
  `MicStop` are no-ops. In `pnsovr.dll` every `Mic*` export except `MicAvailable`/`MicCaptureSize`
  (constant 0x960) and `MicSampleRate` (constant 0xBB80) has its own body.
- `AllowGuests` returns 1 in `pnsrad.dll` and `pnsdemo.dll`, and 0 in `pnsovr.dll`.
- There are no forwarders in any of the three tables.

## Not built

The runtime call tracer the issue sketches (a MinHook trampoline per export that logs typed arguments, the
result and the duration) is not implemented. It needs each export's signature (the typed list in
`echovr-reconstruction/src/pnsrad/Plugin/RadPluginAPI.h` covers the `RadPlugin*` set, not the social and voip
exports), an answer to the folded-body problem above, and a client run to prove the hooks leave behaviour
unchanged; this lane runs no client. This table and tool are the static first half: the full list of what
to trace and which names cannot be separated.
