# Mic provider: fixing one-way voice under NEVR (GH #15)

2026-09-21, Claude + Andrew. Design and investigation record, written so an
interruption only costs a `git log`/ReVault read, not a re-derivation. No
code has been written yet — this is the state to build from.

## The bug (GH #15)

Voice is one-way under the NEVR runtime: players can hear others but cannot
be heard. Reproduced by a tester and on a maintainer's own client.

## Root cause, confirmed

`echovr.exe` picks its mic-input provider **independently** of the login/
social provider. `CR15Game::CreateNetServiceProviders` @ `0x140109810`
creates OVR (`pnsovr.dll`) and/or DMO (`pnsdemo.dll`) providers per the
`-micprovider` flag, plus RAD (`pnsrad.dll`) unconditionally, then selects
one for mic input (logged as "Using OVR/DMO/RAD provider for mic input";
default RAD). NEVR forcing `pnsrad` for login/social does not force it for
mic — that was a wrong initial hypothesis, corrected during this
investigation.

On Windows, `pnsrad.dll`'s six `Mic*` exports are all the same 3-byte stub
at `0x180088d10` (`MicAvailable`/`MicCreate`/`MicDetected`/`MicRead` share
one body): `return 0`. The game sees no microphone and never starts capture.

**Encode and decode both work.** `VoipEncode` @ `0x180089500` calls a real
`opus_encode`; `VoipDecode` @ `0x180089320` calls a real `opus_decode` and
delivers straight to a per-decoder callback. This is why receive already
works (the one-way symptom) — only PCM capture is missing.

## How the game consumes the mic — the design-determining trace

Two independent consumers call `Mic*`, both via dynamic dispatch through
`NRadEngine::CPlatformService::MicRead` @ `0x14060cad0` (`GetMethodProc
(provider_handle, "MicRead")`, same pattern for the other five exports).

**`CaptureAndEncodeLocalVoice` @ `0x140d7bd90`** (renamed from a stale,
wrong `ProcessTeamBalancing` — the old name described nothing in the
function body), per-frame, per player slot with local voice active:

1. `MicAvailable()` — how many samples are ready.
2. `MicRead(buf, count)` — pulls them, **prepended** with any leftover
   samples saved from the previous tick (per-slot state at `+0x16`/`+0x18`).
3. While remaining ≥ `VoipPacketSize()` (`0x140618ff0`): slice off one
   packet-sized chunk, `VoipEncode()` (`0x140618eb0`) it, hand the result to
   `BuildSyncPacket()` (`0x140d431e0`).
4. Any remainder smaller than a packet is saved for next tick.

**This settles the load-bearing design question: the mic provider does not
need to deliver fixed-size reads.** `MicRead` may return any count from 0 up
to the requested amount; the game's own leftover-buffer logic reassembles
into `VoipPacketSize`-sized encode units. The provider only has to be
honest about samples-available and samples-actually-copied.

Gated on session/mode state (`+0x8c14` ∈ {1,2,3}) and a peer-voice-enabled
bit (`+6 & 0x400`) — capture is conditional per game mode, not unconditional.

**`CaptureVoiceIntoSlotBuffer` @ `0x140d85c70`** (renamed from a stale
`UpdateBoostRecharge`) is a second consumer: also calls `MicAvailable`/
`MicRead`, but appends raw samples to a growable per-slot array instead of
encoding immediately. Purpose not yet determined — possibly spectator,
replay, or analytics capture. Does not block the fix; flagged for later.

## The reference implementation that already exists

`libpnsrad.so` (Quest/Android build) has a **complete, working** mic
provider — OpenSL ES capture:

- `MicCreate` @ `0x3066f0`: opens an OpenSL recorder, 48kHz mono int16,
  recording preset 3, attaches a buffer-queue callback (`FUN_00306c70`).
- `MicStart` @ `0x306ecc`: enqueues 10 buffers, then starts recording.
- `SMicBuffers` @ `0x3064ac`: 16 slots × 960 samples (20ms, 0x780 bytes)
  each, two lock-free ring queues (free/ready).
- `MicAvailable` @ `0x307178`: `ready_count × 960`, in samples.
- `MicRead(buf, n)` @ `0x3071c4`: copies whole 960-sample frames from the
  ready queue; logs "Reading less than available from mic, some data will
  be lost" if asked for less than one frame's worth.

Windows `pnsrad.dll` reports different size constants (`MicBufferSize` =
24000, `MicCaptureSize` = 2400, both confirmed against `pnsovr.dll`'s
matching constants) — same shape, different numbers. The Android structure
is the model to follow; the PC constants are what to report.

**Caveat on this reference** (Andrew, 2026-09-21): the Quest/Android side of
this ReVault project may have address/symbol glitches — confirmed one
concretely: `libpnsovr.so`'s symbol table does not line up with
`libpnsrad.so`'s (`libpnsovr.so 0x3066c8` resolves to an unrelated OpenSSL
function, not a mic export). Treat the `libpnsrad.so` struct layout as a
strong hint, not as ground truth to `static_assert` against without
independent confirmation on the PC side.

## `pnsovr.dll` — read for contract only, not to be reactivated

`pnsovr.dll`'s `Mic*`/`Voip*` are thin wrappers over `LibOVRPlatform64_1.dll`
(imported `ovr_*` symbols — that DLL is not yet in ReVault, Andrew is adding
it). Read for two reasons only:

- **`MicRead`'s contract** (`ovr_Microphone_GetPCM`, passthrough).
- **Receive-side shape**: `Update()` @ `0x180097e60` polls decoders once per
  call, pulls up to 2400 **float** samples via `ovr_VoipDecoder_GetDecodedPCM`,
  converts to int16 (`×32768`), then invokes the same kind of per-connection
  callback `pnsrad.dll`'s `VoipDecode` calls inline. Confirms `pnsrad`'s
  inline-delivery receive path is equivalent in shape, just not deferred to
  a poll step.

**Decision (Andrew): `pnsovr.dll` will not be reactivated or used as a
fallback.** It requires the Oculus platform/runtime to be present and
initialized (`MicDetected` there is `ovr_IsPlatformInitialized()`), which
excludes servers, `-windowed`, headset-free clients, and Wine — the
opposite of what NEVR needs. `pnsrad_enabler` already forces `pnsrad` to
load in place of it; nothing in the game requires `pnsovr.dll` to be
present once its mic exports are implemented in `pnsrad`, so the intent is
to make the install `pnsovr.dll`-free, not merely `pnsovr.dll`-inert.
Confirmed nothing else keeps the game on the OVR mic path if a provider
default is left as-is — `-micprovider` still exists as a flag for anyone
who wants to force it, but that's a deliberate opt-in, not a fallback we
maintain.

## The fix

Implement `pnsrad.dll`'s six `Mic*` exports for real, on Windows, via
WASAPI capture:

1. **Capture module** (new, alongside `pnsrad_enabler.cpp` — same job
   category: "make pnsrad do what a platform provider would have done").
   A WASAPI capture stream opened at 48kHz mono int16, feeding a ring buffer
   structured like `SMicBuffers` (free/ready queues) but sized to the
   Windows constants (`MicCaptureSize` = 2400, `MicBufferSize` = 24000).
2. **Hook the six exports** in `pnsrad.dll`, same technique as
   `pnsrad_enabler`'s existing patches (prologue-validated, `HookGuard`-
   recorded).
3. **No changes needed downstream** — `VoipEncode`, `BuildSyncPacket`, and
   the entire receive path are already correct.

### Testing

- **Automatable now, via `tools/winvm/systest.py`** (the same rig that
  proved local-nakama login end-to-end, `docs/reference/local-nakama.md`):
  boot the runtime, confirm `MicAvailable`/`MicRead` get called and return
  nonzero against a fake/silent WASAPI input, and confirm `VoipEncode`
  actually produces output bytes. That is a scriptable pass/fail — it
  proves the pipe is not dead, not that audio sounds right.
- **Real two-way audio** — a live headset/mic and a second peer — is a
  human test, not scriptable in the VM. Andrew or a tester runs this once
  the automated check passes.

## Linux support for `echovr.exe` — where this leaves us

Andrew: Linux support for `echovr.exe` is coming "sooner than later." This
repo already has Wine-target presets (`linux-wine-debug`/`-release`,
`cmake/toolchain-msvc-wine.cmake`) used today to test the Windows PE build
under Wine — that is the same mechanism a native Linux user would run the
game through, since `echovr.exe` is a closed Windows binary with no source
available to port; "Linux support" in practice means **Wine/Proton**, not a
native recompile.

**Consequence for the mic provider:** WASAPI is a Windows API, but under
Wine it is backed by Wine's own audio driver (`winepulse`/`winealsa`),
which forwards to the host's PulseAudio/PipeWire/ALSA. Our WASAPI capture
code does not need a separate Linux implementation — Wine translates the
call. **This is not free of risk**, and I'm flagging it rather than
asserting it's fine:

- Wine's WASAPI **capture** path has historically been less mature than
  playback. This needs to be verified empirically under Wine before calling
  Linux support "done" for voice, not assumed from Windows-only testing.
- The existing `linux-wine-*` CMake presets build and test the DLL under
  Wine already, so **the test surface for this exists** — it just needs a
  mic-capture check added to it (or to `tools/winvm/systest.py`'s Linux/Wine
  equivalent, which does not exist yet — today's `systest.py` is native-
  Windows-VM only).
- If Wine's WASAPI capture turns out to be unreliable, the fallback is a
  runtime-detected branch (are we running under Wine?) that talks to
  PipeWire/PulseAudio directly instead of through WASAPI. **Not proposed as
  work now** — no evidence yet that WASAPI-via-Wine is insufficient. This is
  a documented risk, not a task.

**Net assessment: implementing via the standard (non-Oculus) path costs us
nothing on the Linux question, and may in fact be the only path.**
`pnsovr.dll`'s route depends on the Oculus platform SDK, which is Windows/
Meta-Store-specific in ways that are, if anything, harder to get working
under Wine than direct WASAPI. Choosing WASAPI over OVR was already the
right call for reasons independent of Linux (no platform SDK dependency,
works headless/windowed); Linux is a second, independent reason to prefer
it, not a complication.

## Open threads, explicitly not blocking

- `CaptureVoiceIntoSlotBuffer`'s purpose (spectator/replay/analytics?) —
  worth answering eventually, not before shipping the capture fix.
- The ReVault reconstruction carried **four different class names**
  (`CNSProvider`, `CPlatformService`, `CPlatformServiceProvider`,
  `CPlatformCS`) for the same function across different scratch files.
  Corrected in ReVault to the earliest-established, most-cited name
  (`CPlatformService`, per `NRadEngine/Game/CPlatformService.cpp` in
  `echovr-reconstruction`); the inconsistency itself was not otherwise
  resolved and may recur elsewhere in that tree.

## ReVault state (as of this doc)

Renames/comments/tags written back 2026-09-21, all in project `echovr`:

| Binary | VA | Change |
|---|---|---|
| `echovr.exe` | `0x140109810` | renamed → `CR15Game::CreateNetServiceProviders`, commented |
| `echovr.exe` | `0x14060cad0` | renamed → `NRadEngine::CPlatformService::MicRead`, commented (superseded an earlier wrong rename to `CNSProvider::MicRead` in the same session) |
| `echovr.exe` | `0x140d7bd90` | renamed `ProcessTeamBalancing` → `CaptureAndEncodeLocalVoice`, commented, tagged `nevr-15-mic-consumer` |
| `echovr.exe` | `0x140d85c70` | renamed `UpdateBoostRecharge` → `CaptureVoiceIntoSlotBuffer`, commented, tagged `nevr-15-mic-consumer` |
| `pnsrad.dll` | `0x180088d10` | commented (stub root cause), tagged `nevr-15-mic-stub` |
| `pnsrad.dll` | `0x180089500`, `0x180089320` | commented (confirmed real Opus encode/decode) |
| `pnsovr.dll` | `0x180097390`, `0x180097450`, `0x180097e60` | commented (contract reference, not to be reactivated) |
| `libpnsrad.so` | `0x3064ac` | commented (Android reference struct, with the Quest-glitch caveat inline) |
