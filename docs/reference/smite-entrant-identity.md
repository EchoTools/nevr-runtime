# Smite: which identifier the game resolves to an entrant slot

Issue #119: the smite handler in `src/runtime/server/gameserver_callbacks.cpp` (`Envelope::kLobbySmiteEntrant`)
turns the server's `entrant_id` UUID into the entrant slot index the game's smite event carries. This
records what the game itself does with the id, from ReVault (`echovr.exe`), and how the runtime resolves it.

## What the runtime does

`entrant_id` is parsed as a UUID into a `GUID` (`ParseUuidToGuid`) and resolved by
`ServerContext::FindEntrantSlotBySession` (`src/runtime/server/server_context.cpp`), which scans the
lobby's player-session array (`EchoVR::Lobby::playerSessions`, `playerSessionCount`, `PlayerSessionSlot`
in `src/abi/echovr.h`) for the item whose `guid` matches and whose `joinState` is 4 (accepted), and returns its index, provided
the index is inside the entrant array at `lobby+0x360`. Both arrays have the player limit as their length
(`CNSLobby::StartSessionCBHost`), so the entrant count is capacity, not a live count; the join-state
check is what separates a live slot from a departed one. The handler logs `Smite entrant not found in lobby` when nothing
matches. `Lobby::EntrantData::userId` is an `XPlatformId`, so comparing a UUID with it can never match;
`ServerContextSmite.EntrantUserIdBytesDoNotMatch` pins that.

## What the game does with a smite

| Piece | Address | Behaviour |
| --- | --- | --- |
| Event | `SNSLobbySmiteEntrant`, payload 16 bytes | `[u64 slot][u8 reason][7 bytes padding]` (built by `CNSLobby::SmiteEntrant`, `0x140616630`; the runtime's `EncodeLobbySmiteEntrant` writes the same shape) |
| Handler | `0x140616870`, registered in `CNSLobby::RegisterHostCallbacks` (`0x14060f730`, registration at `0x14060fdcb`) | when an owner slot is set (`lobby+0x1F0 != -1`) it rejects a sender that is neither the local sentinel peer nor the owner's peer (`[NSLOBBY] smite entrant received from non-host non-owner peer`); otherwise logs `[NSLOBBY] smiting entrant in slot %llu` and calls `RemoveEntrant` (`0x140610700`) with the slot and the reason byte |
| Slot space | `lobby+0x360`, element stride `0xD8` (`CNSLobby::SmiteEntrant`, `IMUL RAX,RDX,0xd8` at `0x14061665d`) | the slot is an index into this entrant array |

So the runtime's output is a slot index, which is what the game wants; the work is the lookup from
`entrant_id` to that index.

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

## Departures

`RemoveEntrant` (`0x140610700`) resets the slot in place: the peer UUID goes back to the sentinel, the
join state to 0 and the timeout to 60.0. The array is not compacted, so a slot index stays valid for the
life of the session and a departed slot is recognised by join state 0 (ReVault comment on `0x140610700`).

## Not proven

- That nakama sends `entrant_id` as the same UUID it sends in accepts (issue #119 says no smite
  sender exists yet in nakama or nevr-server-rs).
- The value of the sentinel GUID `RemoveEntrant` writes (`_DAT_142100b20`).
- No run exercised a smite.
