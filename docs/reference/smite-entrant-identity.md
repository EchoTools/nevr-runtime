# Smite: which identifier the game resolves to an entrant slot

Issue #119: the smite handler in `src/runtime/server/gameserver.cpp` (`Envelope::kLobbySmiteEntrant`)
resolves the server's `entrant_id` to a slot by comparing it with `entrant->userId`, and that comparison
cannot match. This records what the game itself does with the id, from ReVault (`echovr.exe`), so the
protocol question can be answered from the binary.

## What the runtime does now

`entrant_id` is parsed as a UUID into a `GUID` (`ParseUuidToGuid`), then compared byte for byte
(`memcmp`, 16 bytes) with `Lobby::EntrantData::userId`, which is an `XPlatformId` (a platform qword
whose low nibble is the provider, then the account id; `src/abi/echovr.h`). A random v4 UUID never has
that shape, so `found` stays false and the handler logs `Smite entrant not found in lobby`.

## What the game does with a smite

| Piece | Address | Behaviour |
| --- | --- | --- |
| Event | `SNSLobbySmiteEntrant`, payload 16 bytes | `[u64 slot][u8 reason][7 bytes padding]` (built by `CNSLobby::SmiteEntrant`, `0x140616630`; the runtime's `EncodeLobbySmiteEntrant` writes the same shape) |
| Handler | `0x140616870`, registered in `CNSLobby::RegisterHostCallbacks` (`0x14060f730`, registration at `0x14060fdcb`) | when an owner slot is set (`lobby+0x1F0 != -1`) it rejects a sender that is neither the local sentinel peer nor the owner's peer (`[NSLOBBY] smite entrant received from non-host non-owner peer`); otherwise logs `[NSLOBBY] smiting entrant in slot %llu` and calls `RemoveEntrant` (`0x140610700`) with the slot and the reason byte |
| Slot space | `lobby+0x360`, element stride `0xD8` (`CNSLobby::SmiteEntrant`, `IMUL RAX,RDX,0xd8` at `0x14061665d`) | the slot is an index into this entrant array |

So the runtime's output (a slot index) is what the game wants; only the lookup from `entrant_id` to the
index is wrong.

## The identifier the game itself maps to a slot

`CNSLobby::StartSessionCBHost` (`0x140616ae0`) allocates a second array on the lobby: items at
`lobby+0xC8`, count at `lobby+0xD0`, stride `0x28`:

| Offset in item | Content |
| --- | --- |
| `+0x00` | peer handle |
| `+0x08` | 16-byte GUID of the entrant |
| `+0x18` | join state: 0 not joined, 1 join pending, 2 add pending, 3 state pending, 4 accepted (the strings in `0x140603e20`) |
| `+0x24` | float timeout, initialised to `0x42700000` (60.0) |

`CNSLobby::AcceptPlayersSuccessCBHost` (`0x140603e20`) takes the 16-byte ids in the accept message,
`memcmp`s each against `item+0x08` over the array (`FUN_1400ce340(msg_id, lobby+0xC8 + 8 + i*0x28, 0x10)`)
and uses the matching index `i` as the entrant slot: it reads `entrant[i]` at `lobby+0x360 + i*0xD8`, logs
`[NSLOBBY] accepting entrant from peer %s into slot %llu` with `i`, and sends
`SNSLobbyAddEntrantAcceptedv2` for it. A GUID with no match logs `unknown player session accepted`.

The accept message is what the runtime builds in `EncodeLobbyEntrantsAccept`
(`src/runtime/server/messages.cpp`) from the same `entrant_ids` strings the server sends, so the id space
in `entrant_id` for a smite is the one the game already resolves for accepts: the `+0xC8` GUID array,
index = slot.

## Not proven

- Whether items in the `+0xC8` array are cleared or compacted when an entrant leaves (`RemoveEntrant`,
  `0x140610700`, was not read), so an index may be stale after departures.
- The runtime's `EchoVR::Lobby` hides the array inside `_unk3[0xD0]` (offsets `0x60`-`0x12F`); no
  runtime code reads `+0xC8` or `+0xD0` today.
- That nakama sends `entrant_id` as the same UUID it sends in accepts (issue #119 says no smite
  sender exists yet in nakama or nevr-server-rs).
- No run exercised a smite.
