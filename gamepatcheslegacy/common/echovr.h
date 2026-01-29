#pragma once
#ifndef ECHOVR_H
#define ECHOVR_H

#include <cstdint>

#include "../../common/platform_stubs.h"
#include "../../extern/nevr-common/common/Windows.h"
#include "../../extern/nevr-common/common/guiddef.h"

namespace EchoVR {

// Forward declarations
class IServerLib;
struct Broadcaster;
struct TcpBroadcaster;

// TODO: Heap allocator for game plugin modules
struct Allocator {};

// TODO: Pool buffer structure
struct PoolBuffer {};

// TODO: Pool managing arbitrary objects via PoolBuffer (incomplete struct size)
template <typename T>
struct Pool {};

// Fixed-size array wrapper
template <typename T>
struct Array {
  T* items;
  UINT64 count;
};

// Heap-allocated array with allocator tracking
template <typename T>
struct HeapArray {
  T* items;
  UINT64 count;
  Allocator* allocator;
};

// Address info (sockaddr_in at start, padded)
struct AddressInfo {
  UINT64 raw[16];
};

// Parsed URI container (size: 0x120)
const struct UriContainer {
  CHAR _unk0[0x120];
};

// Parsed JSON object
struct Json {
  VOID* root;
  VOID* cache;
};

// Log level flags
enum class LogLevel : INT32 {
  Debug = 0x1,
  Info = 0x2,
  Warning = 0x4,
  Error = 0x8,
  Default = 0xE,
  Any = 0xF,
};

// Callback delegate structure
struct DelegateProxy {
  VOID* instance;    // Caller instance
  UINT64 method[2];  // Method to call via proxyFunc
  VOID* proxyFunc;   // Wrapper function that invokes method
};

// 64-bit symbol hash identifier
typedef INT64 SymbolId;

// User/account identifier
struct XPlatformId {
  UINT64 platformCode;
  UINT64 accountId;
};

// UDP peer index
typedef UINT64 Peer;
const Peer Peer_Self = 0xFFFFFFFFFFFFFFFC;
const Peer Peer_AllPeers = 0xFFFFFFFFFFFFFFFD;
const Peer Peer_SelfAndAllPeers = 0xFFFFFFFFFFFFFFFE;
const Peer Peer_InvalidPeer = 0xFFFFFFFFFFFFFFFF;

// UDP broadcast socket info
struct BroadcastSocketInfo {
  UINT64 port : 16;
  UINT64 read : 24;
  UINT64 write : 24;
  UINT64 socket;
  // TODO: struct size is larger, fields unknown
};

// UDP broadcaster data (offsets documented inline)
struct BroadcasterData {
  Allocator* allocator;                                     // 0x00
  Broadcaster* owner;                                       // 0x08
  BroadcastSocketInfo broadcastSocketInfo;                  // 0x10
  CHAR _unk0[0xE8 + (0xE8 - sizeof(broadcastSocketInfo))];  // Unknown
  DelegateProxy logFunc;                                    // 0x1e0
  UINT32 selfType;                                          // 0x200
  UINT32 dummyType;                                         // 0x204
  CHAR _unk1[0x78];                                         // Unknown (was CTimer)
  AddressInfo addr;                                         // 0x280 (sockaddr_in + padding)
  CHAR displayName[128];                                    // 0x300
  CHAR name[128];                                           // 0x380
  // TODO: more fields unknown
};

// UDP broadcaster wrapper
struct Broadcaster {
  BroadcasterData* data;
};

// TCP peer (websocket connection)
struct TcpPeer {
  UINT32 index;
  UINT32 gen;
};
const TcpPeer TcpPeer_Self = {0xFFFFFFFD, 0};
const TcpPeer TcpPeer_AllPeers = {0xFFFFFFFE, 0};
const TcpPeer TcpPeer_InvalidPeer = {0xFFFFFFFF, 0};

// TODO: TCP peer connection statistics
struct TcpPeerConnectionStats {};

// TCP broadcaster data (websocket client)
// NOTE: vtable may be slightly off but CreatePeer/SendToPeer work
class TcpBroadcasterData {
 public:
  virtual VOID __Unknown0() = 0;
  virtual ~TcpBroadcasterData() = 0;
  virtual VOID Shutdown() = 0;
  virtual UINT32 IsServer() = 0;
  virtual VOID AddPeerFromBuffer(PoolBuffer* buffer) = 0;
  virtual UINT64 GetPeerCount() = 0;
  virtual UINT32 HasPeer(TcpPeer) = 0;
  virtual UINT32 IsPeerConnecting(TcpPeer) = 0;
  virtual UINT32 IsPeerConnected(TcpPeer) = 0;
  virtual UINT32 IsPeerDisconnecting(TcpPeer) = 0;
  virtual AddressInfo* GetPeerAddress(AddressInfo* result, TcpPeer) = 0;
  virtual VOID __Unknown1() = 0;
  virtual const CHAR* GetPeerDisplayName(TcpPeer) = 0;
  virtual TcpPeer* GetPeerByAddress(TcpPeer* result, const AddressInfo*) = 0;
  virtual TcpPeer* GetPeerByIndex(TcpPeer* result, UINT32) = 0;
  virtual VOID FreePeer(TcpPeer) = 0;
  virtual VOID DisconnectPeer(TcpPeer) = 0;
  virtual VOID DisconnectAllPeers() = 0;
  virtual VOID __Unknown2() = 0;  // vtable padding for CreatePeer/SendToPeer alignment
  virtual TcpPeer* CreatePeer(TcpPeer* result, const UriContainer*) = 0;
  virtual VOID DestroyPeer(TcpPeer) = 0;
  virtual VOID SendToPeer(TcpPeer, SymbolId msgtype, const VOID* item, UINT64 itemSize, const VOID* buffer,
                          UINT64 bufferSize) = 0;
  virtual VOID Update() = 0;
  virtual UINT32 Update_2(UINT32, UINT32) = 0;
  virtual UINT32 HandlePeer(SymbolId, TcpPeer, const VOID*, UINT64) = 0;
  virtual const TcpPeerConnectionStats* GetPeerConnectionStats(TcpPeer) = 0;
  virtual TcpPeerConnectionStats* GetPeerConnectionStats_0(TcpPeer) = 0;

  TcpBroadcaster* owner;
  AddressInfo addressInfo;
  CHAR displayName[24];
  CHAR name[24];
  // TODO: more fields unknown
};

// TCP broadcaster wrapper
struct TcpBroadcaster {
  TcpBroadcasterData* data;
};

// Loadout cosmetic slot (0xA8 bytes)
// Serialization: LoadoutSlot_Inspect_Deserialize @ 0x140136060
//                LoadoutSlot_Inspect_Serialize   @ 0x140136fc0
struct LoadoutSlot {
  SymbolId selectionmode;     // 0x00 - JSON: "selectionmode" (int)
  SymbolId banner;            // 0x08
  SymbolId booster;           // 0x10
  SymbolId bracer;            // 0x18
  SymbolId chassis;           // 0x20
  SymbolId decal;             // 0x28
  SymbolId decal_body;        // 0x30
  SymbolId emissive;          // 0x38
  SymbolId emote;             // 0x40
  SymbolId secondemote;       // 0x48
  SymbolId goal_fx;           // 0x50
  SymbolId medal;             // 0x58
  SymbolId pattern;           // 0x60
  SymbolId pattern_body;      // 0x68
  SymbolId pip;               // 0x70
  SymbolId tag;               // 0x78
  SymbolId tint;              // 0x80
  SymbolId tint_alignment_a;  // 0x88
  SymbolId tint_alignment_b;  // 0x90
  SymbolId tint_body;         // 0x98
  SymbolId title;             // 0xA0
};

// Loadout entry with metadata (0xD8 bytes)
// Serialization: LoadoutEntry_Inspect_Deserialize @ 0x140133e50
//                LoadoutEntry_Inspect_Serialize   @ 0x140134090
struct LoadoutEntry {
  SymbolId bodytype;        // 0x00
  uint16_t teamid;          // 0x08
  uint16_t airole;          // 0x0A
  uint8_t _padding[4];      // 0x0C
  SymbolId xf;              // 0x10 - Unknown (possibly effects)
  uint8_t _reserved[0x18];  // 0x18
  LoadoutSlot loadout;      // 0x30
};

// Lobby privacy level
enum class LobbyType : INT8 {
  Public = 0x0,
  Private = 0x1,
  Unassigned = 0x2,
};

// Net game state machine
enum class NetGameState : INT32 {
  OSNeedsUpdate = -100,
  OBBMissing = -99,
  NoNetwork = -98,
  BroadcasterError = -97,
  CertificateError = -96,
  ServiceUnavailable = -95,
  LoginFailed = -94,
  LoginReplaced = -93,
  LobbyBooted = -92,
  LoadFailed = -91,
  LoggedOut = 0,
  LoadingRoot = 1,
  LoggingIn = 2,
  LoggedIn = 3,
  LoadingGlobal = 4,
  Lobby = 5,
  ServerLoading = 6,
  LoadingLevel = 7,
  ReadyForGame = 8,
  InGame = 9
};

// Main lobby/session structure
struct Lobby {
  // Player data (0x250 bytes each)
  struct EntrantData {
    XPlatformId userId;       // 0x00
    SymbolId platformId;      // 0x10
    CHAR uniqueName[36];      // 0x18
    CHAR displayName[36];     // 0x3C
    CHAR sfwDisplayName[36];  // 0x60
    INT32 censored;           // 0x84
    UINT16 owned : 1;         // 0x88
    UINT16 dirty : 1;
    UINT16 crossplayEnabled : 1;
    UINT16 unused : 13;
    UINT16 ping;                     // 0x8A
    UINT16 genIndex;                 // 0x8C
    UINT16 teamIndex;                // 0x8E
    Json json;                       // 0x90
    UINT16 slotIndex;                // 0xA0 (verified @ +0x178 from base)
    CHAR _unk_cosmetic_data[0x1A8];  // 0xA2
  };

  // Local player session info
  struct LocalEntrantv2 {
    GUID loginSession;
    XPlatformId userId;
    GUID playerSession;
    UINT16 teamIndex;
    BYTE padding[6];
  };

  VOID* _unk0;                                // 0x00
  Broadcaster* broadcaster;                   // 0x08
  TcpBroadcaster* tcpBroadcaster;             // 0x10
  UINT32 maxEntrants;                         // 0x18
  UINT32 hostingFlags;                        // 0x1C (bit 1 = pass host ownership)
  CHAR _unk2[0x10];                           // 0x20
  INT64 serverLibraryModule;                  // 0x30
  IServerLib* serverLibray;                   // 0x38 (typo preserved for ABI)
  DelegateProxy acceptEntrantFunc;            // 0x40
  CHAR _unk3[0xD0];                           // 0x60
  UINT32 hosting;                             // 0x130
  CHAR _unk4[0x04];                           // 0x134
  Peer hostPeer;                              // 0x138
  Peer internalHostPeer;                      // 0x140
  Pool<LocalEntrantv2> localEntrants;         // 0x148
  CHAR _unk5[0x84 - sizeof(localEntrants)];   // Unknown
  GUID gameSessionId;                         // 0x1CC
  CHAR _unk6[0x10];                           // 0x1DC
  UINT32 entrantsLocked;                      // 0x1EC
  UINT64 ownerSlot;                           // 0x1F0
  UINT32 ownerChanged;                        // 0x1F8 (TODO: verify)
  CHAR _unk7[0x360 - 0x1FC];                  // 0x1FC
  HeapArray<Lobby::EntrantData> entrantData;  // 0x360
  CHAR _unk_after_entrants[0x1C0];            // 0x378

  // NOTE: Server context fields (SNSServerContext) are accessed via callbacks,
  // not directly from Lobby. Key offsets documented:
  //   +0x448: Registration state (int32)
  //   +0x910: Entrant connections array pointer
  //
  // Loadout data via: lobby_ptr + 0x51420 + (slotIndex * 0x40)
};

// Game server library interface
class IServerLib {
 public:
  virtual INT64 UnkFunc0(VOID* unk1, INT64 a2, INT64 a3) = 0;
  virtual VOID* Initialize(Lobby* lobby, Broadcaster* broadcaster, VOID* unk2, const CHAR* logPath) = 0;
  virtual VOID Terminate() = 0;
  virtual VOID Update() = 0;
  virtual VOID UnkFunc1(UINT64 unk) = 0;
  virtual VOID RequestRegistration(INT64 serverId, CHAR* radId, SymbolId regionId, SymbolId lockedVersion,
                                   const Json* localConfig) = 0;
  virtual VOID Unregister() = 0;
  virtual VOID EndSession() = 0;
  virtual VOID LockPlayerSessions() = 0;
  virtual VOID UnlockPlayerSessions() = 0;
  virtual VOID AcceptPlayerSessions(Array<GUID>* playerUuids) = 0;
  virtual VOID RemovePlayerSession(GUID* playerUuid) = 0;
};

}  // namespace EchoVR

#endif  // ECHOVR_H
