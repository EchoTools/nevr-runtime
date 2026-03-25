#include "echovr_functions.h"

#include "echovr.h"
#include "pch.h"

namespace EchoVR {
// Obtain a handle for the game - MUST be initialized first before any function pointers that use it
CHAR* g_GameBaseAddress = (CHAR*)GetModuleHandle(NULL);

// --- UDP/protocol function pointer typedefs and assignments ---
typedef void ListenProxyFunc(void);
ListenProxyFunc* ListenProxy = (ListenProxyFunc*)(g_GameBaseAddress + 0x600f90);

typedef void udp_recvfrom_sockaddr_storageFunc(void);
udp_recvfrom_sockaddr_storageFunc* udp_recvfrom_sockaddr_storage =
    (udp_recvfrom_sockaddr_storageFunc*)(g_GameBaseAddress + 0x1f8d90);

typedef void CleanupPingsFunc(long long* in_RCX, long long* in_R8);
CleanupPingsFunc* CleanupPings = (CleanupPingsFunc*)(g_GameBaseAddress + 0x1071000);

typedef unsigned int udp_protocol_lookup_or_dispatchFunc(long long, unsigned long long, int);
udp_protocol_lookup_or_dispatchFunc* udp_protocol_lookup_or_dispatch =
    (udp_protocol_lookup_or_dispatchFunc*)(g_GameBaseAddress + 0x4fc170);

typedef unsigned long long* udp_protocol_get_stateFunc(long long, unsigned long long*, unsigned long long, int);
udp_protocol_get_stateFunc* udp_protocol_get_state = (udp_protocol_get_stateFunc*)(g_GameBaseAddress + 0x4fc240);

typedef unsigned int udp_protocol_get_peer_idFunc(long long, unsigned long long, int);
udp_protocol_get_peer_idFunc* udp_protocol_get_peer_id = (udp_protocol_get_peer_idFunc*)(g_GameBaseAddress + 0x4fc300);

typedef long long* udp_protocol_find_peerFunc(long long, long long);
udp_protocol_find_peerFunc* udp_protocol_find_peer = (udp_protocol_find_peerFunc*)(g_GameBaseAddress + 0x5ec330);

typedef long long udp_protocol_find_peer_by_addrFunc(long long, long long);
udp_protocol_find_peer_by_addrFunc* udp_protocol_find_peer_by_addr =
    (udp_protocol_find_peer_by_addrFunc*)(g_GameBaseAddress + 0x59a4a0);

typedef long long udp_protocol_get_contextFunc(long long);
udp_protocol_get_contextFunc* udp_protocol_get_context = (udp_protocol_get_contextFunc*)(g_GameBaseAddress + 0x631b40);

typedef void udp_protocol_handshake_or_intro1Func(long long, unsigned long long, void*, unsigned long long,
                                                  unsigned int, unsigned int);
udp_protocol_handshake_or_intro1Func* udp_protocol_handshake_or_intro1 =
    (udp_protocol_handshake_or_intro1Func*)(g_GameBaseAddress + 0x1071b90);

typedef void udp_protocol_handshake_or_intro2Func(long long, unsigned long long, unsigned int, unsigned int);
udp_protocol_handshake_or_intro2Func* udp_protocol_handshake_or_intro2 =
    (udp_protocol_handshake_or_intro2Func*)(g_GameBaseAddress + 0x511020);

typedef void udp_protocol_handshake_or_intro3Func(long long, long long, int, int);
udp_protocol_handshake_or_intro3Func* udp_protocol_handshake_or_intro3 =
    (udp_protocol_handshake_or_intro3Func*)(g_GameBaseAddress + 0x5c7730);

/// Obtains a pool item/block/memory page from a given pool for the given index.
PoolFindItemFunc* PoolFindItem = (PoolFindItemFunc*)(g_GameBaseAddress + 0x2CA9E0);

/// Registers a callback for a websocket message type. ReVault: FUN_140F81100
TcpBroadcasterListenFunc* TcpBroadcasterListen = (TcpBroadcasterListenFunc*)(g_GameBaseAddress + 0xF81100);

/// Unregisters a callback for a certain type of websocket message, using the return value from its registration.
/// <returns>None</returns>

UINT64 TcpBroadcasterUnlisten(EchoVR::TcpBroadcaster* broadcaster, UINT16 cbResult) {
  // Obtain the listeners pool from the broadcaster structure.
  BYTE* listeners = (BYTE*)broadcaster->data + 352;

  // Obtain our block index and offset within it.
  UINT64 blockCapacity = *(UINT64*)(listeners + 40);
  UINT64 blockIndex = cbResult / blockCapacity;
  UINT64 indexInBlock = cbResult % blockCapacity;

  // Obtain the item from the pool
  UINT64 itemPage = NULL;
  if (blockIndex) {
    itemPage = *(UINT64*)(PoolFindItem(listeners, (blockIndex - 1) >> 1) + 8 * (blockIndex & 1));
  } else {
    itemPage = *(UINT64*)(listeners + 8);
  }

  // Free the item from the page at its given offset.
  UINT64 itemData = itemPage + 16;
  UINT64 result = itemData + (UINT32)(-((INT32)itemData) & 7);
  UINT32* flags = (UINT32*)(result + (indexInBlock * 80) + 12);
  *flags |= 1u;  // mark the page as free
  return result;
}

/// Sends a message to a game server broadcaster.
/// ReVault: FUN_140F89AF0 (CBroadcaster_EncodeAndSendPacket)
BroadcasterSendFunc* BroadcasterSend = (BroadcasterSendFunc*)(g_GameBaseAddress + 0xF89AF0);

/// Receives/relays a local event on the broadcaster, triggering a listener.
/// <returns>TODO: Unverified, probably success result.</returns>
BroadcasterReceiveLocalEventFunc* BroadcasterReceiveLocalEvent =
    (BroadcasterReceiveLocalEventFunc*)(g_GameBaseAddress + 0xF87AA0);

/// Registers a callback for a game broadcaster message type. ReVault: FUN_140F80ED0
BroadcasterListenFunc* BroadcasterListen = (BroadcasterListenFunc*)(g_GameBaseAddress + 0xF80ED0);

/// Unregisters a callback for a certain type of game broadcast message, using the return value from its registration.
/// <returns>None</returns>
BroadcasterUnlistenFunc* BroadcasterUnlisten = (BroadcasterUnlistenFunc*)(g_GameBaseAddress + 0xF8DF20);

/// Reads a float value from a parsed JSON document at a given path.
CJsonGetFloatFunc* CJsonGetFloat = (CJsonGetFloatFunc*)(g_GameBaseAddress + 0x5FCA60);

/// Obtains a JSON string value(with a default fallback value if it could not be obtained).
/// <returns>The resulting string returned from the JSON get string operation.</returns>
JsonValueAsStringFunc* JsonValueAsString = (JsonValueAsStringFunc*)(g_GameBaseAddress + 0x5FE290);

/// Parses a URI string into a URI container structure.
/// <returns>The result of the URI parsing operation.</returns>
UriContainerParseFunc* UriContainerParse = (UriContainerParseFunc*)(g_GameBaseAddress + 0x621EC0);

/// Builds the CLI argument options and help descriptions list.
/// <returns>TODO: Unverified, probably success result</returns>
BuildCmdLineSyntaxDefinitionsFunc* BuildCmdLineSyntaxDefinitions =
    (BuildCmdLineSyntaxDefinitionsFunc*)(g_GameBaseAddress + 0xFEA00);

/// Adds an argument to the CLI argument syntax object.
/// <returns>None</returns>
AddArgSyntaxFunc* AddArgSyntax = (AddArgSyntaxFunc*)(g_GameBaseAddress + 0xD31B0);

/// Adds an argument help string to the CLI argument syntax object.
/// <returns>None</returns>
AddArgHelpStringFunc* AddArgHelpString = (AddArgHelpStringFunc*)(g_GameBaseAddress + 0xD30D0);

/// Processes the provided command line options for the running process.
/// ReVault: FUN_140116720 (PreprocessCommandLine, 808+ lines)
PreprocessCommandLineFunc* PreprocessCommandLine = (PreprocessCommandLineFunc*)(g_GameBaseAddress + 0x116720);

/// Writes a log to the logger, if all conditions such as log level are met.
/// <returns>None</returns>
WriteLogFunc* WriteLog = (WriteLogFunc*)(g_GameBaseAddress + 0xEBE70);

/// HTTP/HTTPS URI connection. ReVault: FUN_1401F60C0 (HttpConnect)
/// Reconstruction: CServerConfig.h — strips protocol, extracts hostname:port
HttpConnectFunc* HttpConnect = (HttpConnectFunc*)(g_GameBaseAddress + 0x1F60C0);

/// Loads a JSON file from the given path. ReVault: FUN_1405F0990 (CJson_LoadFromPath, 85 lines)
LoadJsonFromFileFunc* LoadJsonFromFile = (LoadJsonFromFileFunc*)(g_GameBaseAddress + 0x5F0990);

/// Loads the local config (located at ./_local/config.json) for the provided game instance.
LoadLocalConfigFunc* LoadLocalConfig = (LoadLocalConfigFunc*)(g_GameBaseAddress + 0x179EB0);

/// Switches net game state. ReVault: FUN_1401B8650 (Quest: ResetLoadTimeout @ 0x135cde0)
/// Log: "[NETGAME] NetGame switching state (from %s, to %s)"
NetGameSwitchStateFunc* NetGameSwitchState = (NetGameSwitchStateFunc*)(g_GameBaseAddress + 0x1B8650);

/// Schedules return to lobby. ReVault: FUN_1401A89F0 (Quest: ScheduleReturnToLobby @ 0x134aa6c)
NetGameScheduleReturnToLobbyFunc* NetGameScheduleReturnToLobby =
    (NetGameScheduleReturnToLobbyFunc*)(g_GameBaseAddress + 0x1A89F0);

/// The game's definition for GetProcAddress.
/// Reference: https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress
GetProcAddressFunc* GetProcAddress = (GetProcAddressFunc*)(g_GameBaseAddress + 0xEAEF0);

/// The game's definition for SetWindowTitle.
/// Reference: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowtexta
SetWindowTextAFunc* SetWindowTextA_ = (SetWindowTextAFunc*)(g_GameBaseAddress + 0x5105F0);
}  // namespace EchoVR
