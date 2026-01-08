# EchoVR Cosmetic Data Structure Mapping

## Lobby Memory Layout (Complete)

### Base Structure (Lobby*)
```
Offset: 0x00     | Field: _unk0                | Type: void*
Offset: 0x08     | Field: broadcaster          | Type: Broadcaster*
Offset: 0x10     | Field: tcpBroadcaster       | Type: TcpBroadcaster*
Offset: 0x18     | Field: maxEntrants          | Type: uint32
Offset: 0x1C     | Field: hostingFlags         | Type: uint32
Offset: 0x20-2F  | Field: _unk2                | Type: CHAR[16]
Offset: 0x30     | Field: serverLibraryModule  | Type: int64
Offset: 0x38     | Field: serverLibrary        | Type: IServerLib*
Offset: 0x40-5F  | Field: acceptEntrantFunc    | Type: DelegateProxy (24 bytes)
Offset: 0x60-12F | Field: _unk3                | Type: CHAR[208]
Offset: 0x130    | Field: hosting              | Type: uint32
Offset: 0x134-137| Field: _unk4                | Type: CHAR[4]
Offset: 0x138    | Field: hostPeer             | Type: uint64 (Peer)
Offset: 0x140    | Field: internalHostPeer     | Type: uint64 (Peer)
Offset: 0x148-1CB| Field: localEntrants        | Type: Pool<LocalEntrantv2>
Offset: 0x1CC    | Field: gameSessionId        | Type: GUID (16 bytes)
Offset: 0x1DC-1EB| Field: _unk6                | Type: CHAR[16]
Offset: 0x1EC    | Field: entrantsLocked       | Type: uint32
Offset: 0x1F0    | Field: ownerSlot            | Type: uint64
Offset: 0x1F8    | Field: ownerChanged         | Type: uint32
Offset: 0x1FC-35F| Field: _unk7                | Type: CHAR[356]
Offset: 0x360    | Field: entrantData          | Type: HeapArray<EntrantData>
                 |                             | (ptr + count + allocator = 24 bytes)

Offset: 0x51420+ | LoadoutInstancesArray       | Variable per player slot
```

### EntrantData Array Layout
```
Base Address = Lobby* + 0x360 (HeapArray pointer)
Entry 0:      Base + (0 * 0x250)
Entry 1:      Base + (1 * 0x250)
Entry 2:      Base + (2 * 0x250)
...
Entry N:      Base + (N * 0x250)

TOTAL SIZE PER ENTRY: 0x250 bytes (592 bytes)
```

### EntrantData Structure (Per Player)
```
Offset: 0x00     | Field: userId               | Type: XPlatformId (16 bytes)
                 |   - platformCode: uint64 @ 0x00
                 |   - accountId: uint64 @ 0x08
Offset: 0x10     | Field: platformId           | Type: SymbolId (int64)
Offset: 0x18     | Field: uniqueName           | Type: char[36]
Offset: 0x3C     | Field: displayName          | Type: char[36]
Offset: 0x60     | Field: sfwDisplayName       | Type: char[36]
Offset: 0x84     | Field: censored             | Type: int32
Offset: 0x88     | Field: flags                | Type: uint16 (bitfield)
                 |   - owned: 1 bit
                 |   - dirty: 1 bit
                 |   - crossplayEnabled: 1 bit
                 |   - unused: 13 bits
Offset: 0x8A     | Field: ping                 | Type: uint16
Offset: 0x8C     | Field: genIndex             | Type: uint16
Offset: 0x8E     | Field: teamIndex            | Type: uint16
Offset: 0x90     | Field: json                 | Type: Json (16 bytes)
                 |   - root: void*
                 |   - cache: void*
Offset: 0xA0     | Field: slotIndex            | Type: uint16 ⭐ COSMETIC KEY
Offset: 0xA2-24F | Field: _unk_cosmetic_data   | Type: CHAR[424] (HIDDEN COSMETIC DATA)
```

### EntrantData.json Structure (Session State)

The `json` field at offset 0x90 contains the player's real-time session state. This is NOT the 
loadout/cosmetic data - it's the player's current session properties. Access via `json_dumps(entrant->json.root, 0)`.

```json
{
  "invr": false,                    // Is player in VR headset
  "partyid": 0,                     // Party group ID (0 = no party)
  "voip": {
    "muted": false,                 // Voice chat muted
    "deafened": false,              // Voice chat deafened
    "channelmask": 4294967295,      // Voice channel bitmask (0xFFFFFFFF = all)
    "modeffect": 0.0                // Voice modulation effect
  },
  "bodytype": -2076784106321327439, // SymbolId for body type (e.g., male_a)
  "mm": {                           // MATCHMAKING STATE
    "status": 5,                    // Matchmaking status (see enum below)
    "gametype": -1                  // SymbolId of game type (-1 = none)
  },
  "teamname": "",                   // Custom team name
  "purchasedcombat": true,          // Has purchased combat/arena access
  "afk": false,                     // Is player AFK
  "spawned": true                   // Is player spawned in lobby
}
```

#### Matchmaking Status Values (mm.status)
| Value | State | Description |
|-------|-------|-------------|
| 5 | Idle | In lobby, not matchmaking |
| 11 | Searching | Actively searching for match |
| ? | Queued | In matchmaking queue (TBD) |
| ? | Found | Match found, connecting (TBD) |

#### Known gametype SymbolIds (mm.gametype)
| SymbolId | Game Type |
|----------|-----------|
| -1 | None (not matchmaking) |
| -3791849610740453517 | Arena (unverified) |

### Cosmetic/Loadout Data Access Pattern

#### Step 1: Get Player Slot Index
```c
Lobby *lobby = ...;
uint16 player_id = 0-7;

// Calculate entry address
EntrantData *entry = (EntrantData*)(*(void**)(lobby + 0x360)) + (player_id * 0x250);

// Get slot index (needed for loadout array lookup)
uint16 slot_index = entry->slotIndex & 0x1F;  // Mask to 0-31 range
```

#### Step 2: Verify Player Exists
```c
// Check if player data is initialized
void *player_check = *(void**)(lobby + 0x360 + (player_id * 0x250) + 0x3C0);
if (player_check == nullptr) return;  // Player doesn't exist
```

#### Step 3: Get Loadout Instances Array
```c
// Base: lobby + 0x51420 + (slot_index * 0x40)
LoadoutInstancesArray *loadout_array = 
    (LoadoutInstancesArray*)(lobby + 0x51420 + (slot_index * 0x40));

// Access array data:
int64 first_loadout_id = loadout_array->entries[0];  // @ offset +0x00
uint64 loadout_count = loadout_array->count;         // @ offset +0x30
```

#### Step 4: Resolve Cosmetic Items
```c
// For a given loadout_id, resolve the actual cosmetic data
LoadoutData *loadout_data = FUN_1404f37a0(
    *(void**)(lobby + 0x08),  // context from lobby
    loadout_id                // the loadout to look up
);

// Access cosmetic resources:
// param_5[1] from net_apply_loadout_items_to_player contains the slot ID
uint16 cosmetic_slot = *(uint16*)(param_5 + 1);

// Primary cosmetic resource ID:
int64 primary_cosmetic = *(int64*)(
    *(int64*)(*(int64*)(loadout_data + 0x370) + 0xB8) +
    (cosmetic_slot * 8)
);

// Fallback if primary is -1:
int64 fallback_cosmetic = *(int64*)(
    *(int64*)(*(int64*)(loadout_data + 0x370) + 0x80) +
    (cosmetic_slot * 8)
);

// Get human-readable name:
const char *cosmetic_name = CResourceID_GetName(&primary_cosmetic);
```

## LoadoutSlot Structure (Cosmetic Item IDs)

This structure contains all customizable cosmetic item IDs for a single loadout slot.
Each field is a 64-bit SymbolId hash reference to a cosmetic resource.

```
Offset: 0x00     | Field: selectionmode        | Type: int64 (SymbolId hash 0x36ff74b22c06eca3)
Offset: 0x08     | Field: banner               | Type: int64 (Cosmetic banner item ID)
Offset: 0x10     | Field: booster              | Type: int64 (Booster cosmetic item ID)
Offset: 0x18     | Field: bracer               | Type: int64 (Bracer/arm cosmetic)
Offset: 0x20     | Field: chassis              | Type: int64 (Main body/chassis cosmetic)
Offset: 0x28     | Field: decal                | Type: int64 (Decal cosmetic for head)
Offset: 0x30     | Field: decal_body           | Type: int64 (Decal cosmetic for body)
Offset: 0x38     | Field: emissive             | Type: int64 (Emissive glow effect ID)
Offset: 0x40     | Field: emote                | Type: int64 (Primary emote animation ID)
Offset: 0x48     | Field: secondemote          | Type: int64 (Secondary emote animation ID)
Offset: 0x50     | Field: goal_fx              | Type: int64 (Goal effect animation/particle ID)
Offset: 0x58     | Field: medal                | Type: int64 (Medal cosmetic ID)
Offset: 0x60     | Field: pattern              | Type: int64 (Pattern cosmetic for head)
Offset: 0x68     | Field: pattern_body         | Type: int64 (Pattern cosmetic for body)
Offset: 0x70     | Field: pip                  | Type: int64 (Pip icon cosmetic ID)
Offset: 0x78     | Field: tag                  | Type: int64 (Tag/label cosmetic ID)
Offset: 0x80     | Field: tint                 | Type: int64 (Primary color tint ID)
Offset: 0x88     | Field: tint_alignment_a     | Type: int64 (Tint alignment A - team color)
Offset: 0x90     | Field: tint_alignment_b     | Type: int64 (Tint alignment B - team color)
Offset: 0x98     | Field: tint_body            | Type: int64 (Tint for body)
Offset: 0xA0     | Field: title                | Type: int64 (Title cosmetic ID)

TOTAL SIZE: 0xA8 bytes (168 bytes) per loadout slot
```

### Known SymbolId Hashes for LoadoutSlot Fields
```
selectionmode:  0x36ff74b22c06eca3
banner:         (dynamic)
booster:        (dynamic)
bracer:         (dynamic)
chassis:        (dynamic)
decal:          (dynamic)
decal_body:     (dynamic)
emissive:       0x33e09d043af69263
emote:          (dynamic)
secondemote:    0x0a24ff1ad58f2d35
goal_fx:        0x2fd0809e524b5192
medal:          (dynamic)
pattern:        (dynamic)
pattern_body:   (dynamic)
pip:            (dynamic)
tag:            (dynamic)
tint:           (dynamic)
tint_alignment_a: (dynamic)
tint_alignment_b: (dynamic)
tint_body:      (dynamic)
title:          (dynamic)
```

## LoadoutInstancesArray Structure

```
Offset: 0x00     | Field: entries[0]           | Type: int64 (loadout ID)
Offset: 0x08     | Field: entries[1]           | Type: int64
Offset: 0x10     | Field: entries[2]           | Type: int64
Offset: 0x18     | Field: entries[3]           | Type: int64
Offset: 0x20     | Field: entries[4]           | Type: int64
Offset: 0x28     | Field: entries[5]           | Type: int64
Offset: 0x30     | Field: count                | Type: uint64 (⭐ IMPORTANT)
Offset: 0x38     | Field: _unk                 | Type: uint64

TOTAL SIZE: 0x40 bytes (64 bytes) per player slot

ITERATION PATTERN:
for (uint64 i = 0; i < loadout_array->count; i++) {
    int64 current_loadout = loadout_array->entries[i];
    // Process loadout_id
}
```

## Function Entry Points for Hooking

### Primary Hook Target ✅
```
Address: 0x140154c00
Name: net_apply_loadout_items_to_player
Purpose: Apply cosmetics to a player
Parameters: (Lobby *lobby, uint16 player_id, int64 loadout_id, uint32 flags, uint8 *metadata)
Calling Conv: Standard x64 (RCX, RDX, R8, R9 + Stack)
```

### Supporting Functions
```
0x1404f37a0: LoadoutData *FUN_1404f37a0(void *context, int64 loadout_id)
  -> Resolves loadout data for cosmetic lookup

0x1401cc070: int FUN_1401cc070(uint8 *metadata)
  -> Validates metadata array

0x1401c98c0: const char *FUN_1401c98c0(Lobby *lobby, uint16 player_id)
  -> Gets player display name

0x1401544a0: void FUN_1401544a0(Lobby *lobby, uint16 player_id)
  -> Wrapper that calls with default loadout
```

## Struct Size Verification

| Structure | Expected | Actual | Status |
|-----------|----------|--------|--------|
| XPlatformId | 16 | 16 | ✅ |
| EntrantData | ~156 | 592 (0x250) | ⚠️ CORRECTED |
| Lobby | ~376+ | N/A (dynamic) | ✅ |
| LoadoutInstancesArray | 64 | 64 (0x40) | ✅ |
| Json | 16 | 16 | ✅ |

## Memory Access Example

```c
// Given a Lobby pointer and player_id (0-7):
Lobby *lobby = GetLobbyPointer();
uint16 player_id = 2;

// Direct offset calculation:
EntrantData *entry = (EntrantData*)
    (*(uint64*)(lobby + 0x360) + (player_id * 0x250));

// Get cosmetic slot index:
uint16 slot = entry->slotIndex & 0x1F;

// Get loadout instances for this player:
uint64 loadout_array_addr = (uint64)lobby + 0x51420 + (slot * 0x40);
uint64 loadout_count = *(uint64*)(loadout_array_addr + 0x30);
int64 first_loadout = *(int64*)loadout_array_addr;

// Get player name for logging:
const char *player_name = FUN_1401c98c0(lobby, player_id);

// Print result:
printf("Player %d (%s): %llu loadouts\n", player_id, player_name, loadout_count);
```

## Profile JSON Serialization System

The game serializes player profiles (including loadouts) to JSON format before sending to the game service.
This section documents the serialization path and message types.

### Profile Update Function

```
Address: 0x1406131f0
Name: CNSUser_SendProfileUpdate
Purpose: Sends user profile update to game service via TCP broadcaster
Serialization: Uses CJsonTraversal to serialize profile data to JSON
Message Type: SNSUserServerProfileUpdateRequest (symbol hash 0x6d54a19a3ff24415)
Local Files: Saves to clientprofile.json and clientprofilepending.json
Log Output: "[NSUSER] sent profile update: %s"
```

### Profile Message Types (TCP Broadcaster)

| Message Name | Address | Symbol Hash | Direction |
|-------------|---------|-------------|-----------|
| SNSUserServerProfileUpdateRequest | 0x141c3afa8 | 0x6d54a19a3ff24415 | Client → Server |
| SNSUserServerProfileUpdateSuccess | 0x141c3b010 | - | Server → Client |
| SNSUserServerProfileUpdateFailure | 0x141c3b048 | - | Server → Client |
| SNSLoggedInUserProfileRequest | 0x141c3ae80 | - | Client → Server |
| SNSOtherUserProfileRequest | 0x141c3aee8 | - | Client → Server |
| SNSOtherUserProfileSuccess | 0x141c3af48 | - | Server → Client |
| SNSOtherUserProfileFailure | 0x141c3af78 | - | Server → Client |
| SNSUpdateProfile | 0x141c3d0f6 | - | Internal |
| SNSRefreshProfileForUser | 0x1416d65c0 | - | Internal |
| SNSRefreshProfileFromServer | 0x1416d6648 | - | Internal |

### Loadout Network Messages (UDP Broadcaster)

| Message Name | Address | Direction | Description |
|-------------|---------|-----------|-------------|
| SR15NetSaveLoadoutRequest | 0x1416d60f8 | Client → Server | Player saves loadout |
| SR15NetSaveLoadoutSuccess | 0x1416d6130 | Server → Client | Save confirmed |
| SR15NetSaveLoadoutPartial | 0x1416d6168 | Server → Client | Partial save result |
| SR15NetSaveLoadoutFailure | 0x1416d61c0 | Server → Client | Save failed |
| SR15NetCurrentLoadoutRequest | 0x1416d63c0 | Client → Server | Request current loadout |
| SR15NetCurrentLoadoutResponse | 0x1416d63f8 | Server → Client | Current loadout data |
| SR15NetLoadoutNumberNotification | 0x1416d6488 | Bidirectional | Loadout slot changed |

### Loadout Handler Functions

```
Address: 0x1401a79d0
Name: OnMsgSaveLoadoutRequest
Purpose: Server-side handler for loadout save requests
Memory Layout:
  - param_1 + 0xe2: Number of active entrants (uint16)
  - param_1 + 0x138: SlotIndex lookup table
  - param_1 + 0x13a: Generation/session ID table
  - param_1 + 0x51420: Loadout instances array base
  - param_1 + 0x51820: Sensor runtime data array
  - param_1 + 0x64770: Event dispatcher
Hook Candidate: YES - Standard x64 ABI (RCX, RDX)

Address: 0x14015f530
Name: OnMsgCurrentLoadoutRequest
Purpose: Handler for current loadout requests

Address: 0x1401309f0
Name: Broadcast_R15NetSaveLoadoutSuccess
Purpose: Sends save success response to client

Address: 0x140130b00
Name: BroadcastToTargets_SaveLoadout
Purpose: Broadcasts loadout to specific targets

Address: 0x140130e00
Name: BroadcastToExcluding_SaveLoadout
Purpose: Broadcasts loadout excluding certain targets
```

### Profile JSON Key Paths (Pipe-separated notation)

The game uses pipe-separated paths for JSON key access:

```
loadout|instances              - Array of loadout instances
loadout|instances|%s|slots     - Slots for a specific instance
loadout|instances|%s|slots|%s  - Specific slot data
loadout|instances|unified|slots|%s - Unified loadout slots
loadout|number                 - Selected loadout number (0-4)
loadout|number_body            - Body loadout number
```

### Profile Files

| Filename | Purpose |
|----------|---------|
| clientprofile.json | Current client profile state |
| clientprofilepending.json | Pending profile changes |
| serverprofile.json | Server-authoritative profile |
| json\|r14\|defaultserverprofile.json | Default server profile template |

### CJson Serialization Classes

```cpp
// Core JSON class
class CJson {
    json_t* root;           // jansson json_t pointer
    void* computecb;        // Compute callback
    void* resolvecb;        // Resolve callback  
    CHashTable* cache;      // Key-value cache
};

// JSON traversal for serialization
class CJsonTraversal {
    // Walks JSON structure for serialization
};

// JSON inspector for reading
class CJsonInspectorRead {
    // Inspects/deserializes JSON to structs
};

// JSON inspector for writing  
class CJsonInspectorWrite {
    // Serializes structs to JSON
};
```

### Key Ghidra Functions for JSON

| Address | Name | Purpose |
|---------|------|---------|
| 0x1404bbd60 | CJson::~CJson | JSON destructor |
| 0x1405f6ae0 | CJson::LoadFromProjectSourceDb | Load JSON from project |
| 0x1405f7e10 | CJsonTraversal::Process | Process JSON traversal |
| 0x1405ff170 | CJson::UnixTime | Get Unix timestamp from JSON |
| 0x1404b4580 | CJsonInspectorRead::Inspect | Inspect/read CSymbol64 |
| 0x141318df0 | CPluginCS::ExportDataToJSON | Export plugin data to JSON |

## LoadoutSlot JSON Serialization Functions

The game uses a consistent pattern for serializing and deserializing LoadoutSlot structs to JSON.

### LoadoutSlot_Inspect_Deserialize (0x140136060)

```
Address: 0x140136060
Purpose: Reads LoadoutSlot fields from JSON using CJsonInspectorRead
Signature: void LoadoutSlot_Inspect_Deserialize(LoadoutSlot* slot, CJsonInspectorRead* inspector)

Field Mapping (JSON Key → Struct Offset):
  "selectionmode"    → 0x00 (int, via CJsonInspectorRead_ReadInt)
  "banner"           → 0x08 (SymbolId)
  "booster"          → 0x10 (SymbolId)
  "bracer"           → 0x18 (SymbolId)
  "chassis"          → 0x20 (SymbolId)
  "decal"            → 0x28 (SymbolId)
  "decal_body"       → 0x30 (SymbolId)
  "emissive"         → 0x38 (SymbolId)
  "emote"            → 0x40 (SymbolId)
  "secondemote"      → 0x48 (SymbolId)
  "goal_fx"          → 0x50 (SymbolId)
  "medal"            → 0x58 (SymbolId)
  "pattern"          → 0x60 (SymbolId)
  "pattern_body"     → 0x68 (SymbolId)
  "pip"              → 0x70 (SymbolId)
  "tag"              → 0x78 (SymbolId)
  "tint"             → 0x80 (SymbolId)
  "tint_alignment_a" → 0x88 (SymbolId)
  "tint_alignment_b" → 0x90 (SymbolId)
  "tint_body"        → 0x98 (SymbolId)
  "title"            → 0xA0 (SymbolId)
```

### LoadoutSlot_Inspect_Serialize (0x140136fc0)

```
Address: 0x140136fc0
Purpose: Writes LoadoutSlot fields to JSON using CJsonInspectorWrite
Signature: void LoadoutSlot_Inspect_Serialize(LoadoutSlot* slot, CJsonInspectorWrite* inspector)

Uses same field mapping as Deserialize (reverse direction)
```

### CJsonInspectorRead Helper Functions

| Address | Name | Purpose |
|---------|------|---------|
| 0x140137ef0 | CJsonInspectorRead_ReadInt | Read integer value from JSON key |
| 0x140175350 | CJsonInspectorRead_ReadSymbolId | Read SymbolId (u64) from JSON key |

### CJsonInspectorWrite Helper Functions

| Address | Name | Purpose |
|---------|------|---------|
| 0x1401758b0 | CJsonInspectorWrite_WriteInt | Write integer value to JSON key |
| 0x140175770 | CJsonInspectorWrite_WriteSymbolId | Write SymbolId (u64) to JSON key |

## LoadoutEntry JSON Serialization Functions

LoadoutEntry is a parent struct that contains a LoadoutSlot plus metadata.

### LoadoutEntry Structure

```
Offset: 0x00     | Field: bodytype             | Type: SymbolId (u64)
Offset: 0x08     | Field: teamid               | Type: uint16
Offset: 0x0A     | Field: airole               | Type: uint16
Offset: 0x10     | Field: xf                   | Type: SymbolId (u64) - unknown purpose
Offset: 0x30     | Field: loadout              | Type: LoadoutSlot (nested, 0xA8 bytes)

TOTAL SIZE: 0xD8 bytes (216 bytes)
```

### LoadoutEntry_Inspect_Deserialize (0x140133e50)

```
Address: 0x140133e50
Purpose: Reads LoadoutEntry (including nested LoadoutSlot) from JSON
Signature: int LoadoutEntry_Inspect_Deserialize(CJsonInspectorRead* inspector, LoadoutEntry* entry, char* path)

JSON Fields:
  "bodytype" → 0x00 (SymbolId)
  "teamid"   → 0x08 (uint16)
  "airole"   → 0x0A (uint16)
  "xf"       → 0x10 (SymbolId)
  "loadout"  → 0x30 (nested LoadoutSlot via LoadoutSlot_Inspect_Deserialize)
```

### LoadoutEntry_Inspect_Serialize (0x140134090)

```
Address: 0x140134090
Purpose: Writes LoadoutEntry (including nested LoadoutSlot) to JSON
Signature: int LoadoutEntry_Inspect_Serialize(CJsonInspectorWrite* inspector, LoadoutEntry* entry, char* path, ...)

JSON Fields:
  "bodytype" → from offset 0x00 (SymbolId)
  "teamid"   → from offset 0x08 (uint16)
  "airole"   → from offset 0x0A (uint16)
  "xf"       → from offset 0x10 (SymbolId)
  "loadout"  → from offset 0x30 (nested LoadoutSlot via LoadoutSlot_Inspect_Serialize)
```

## Loadout Save Broadcast Flow

When a player saves a loadout, the following function chain executes:

```
1. OnMsgSaveLoadoutRequest (0x1401a79d0)
   ↓ Validates the save request from client
2. ApplyLoadoutSave (0x1401958f0)
   ↓ Validates cosmetic items against player's unlocks
3. SerializeAndBroadcastLoadoutSave (0x1401a9d60)
   ↓ Serializes using EasyStream (magic: 0xc8c33e4833671bbd)
4. BroadcastToTargets_SaveLoadout + BroadcastToExcluding_SaveLoadout
   ↓ Sends to all connected clients
```

Key hook point for intercepting loadout broadcasts:
- **SerializeAndBroadcastLoadoutSave** (0x1401a9d60) - After validation, before network send

---
Generated: 2026-01-07 | Ghidra Analysis Complete ✅
