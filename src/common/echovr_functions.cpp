#include "echovr_functions.h"

#include "echovr.h"
#include "pch.h"

namespace EchoVR {
// Game base address — set by NEVR_SetGameModule (launcher path) or DllMain (legacy dbgcore.dll path).
// Must be initialized before InitializeFunctionPointers() is called.
CHAR* g_GameBaseAddress = nullptr;

// --- UDP/protocol function pointer typedefs and assignments ---
typedef void ListenProxyFunc(void);
ListenProxyFunc* ListenProxy = nullptr;

typedef void udp_recvfrom_sockaddr_storageFunc(void);
udp_recvfrom_sockaddr_storageFunc* udp_recvfrom_sockaddr_storage = nullptr;

typedef void CleanupPingsFunc(long long* in_RCX, long long* in_R8);
CleanupPingsFunc* CleanupPings = nullptr;

typedef unsigned int udp_protocol_lookup_or_dispatchFunc(long long, unsigned long long, int);
udp_protocol_lookup_or_dispatchFunc* udp_protocol_lookup_or_dispatch = nullptr;

typedef unsigned long long* udp_protocol_get_stateFunc(long long, unsigned long long*, unsigned long long, int);
udp_protocol_get_stateFunc* udp_protocol_get_state = nullptr;

typedef unsigned int udp_protocol_get_peer_idFunc(long long, unsigned long long, int);
udp_protocol_get_peer_idFunc* udp_protocol_get_peer_id = nullptr;

typedef long long* udp_protocol_find_peerFunc(long long, long long);
udp_protocol_find_peerFunc* udp_protocol_find_peer = nullptr;

typedef long long udp_protocol_find_peer_by_addrFunc(long long, long long);
udp_protocol_find_peer_by_addrFunc* udp_protocol_find_peer_by_addr = nullptr;

typedef long long udp_protocol_get_contextFunc(long long);
udp_protocol_get_contextFunc* udp_protocol_get_context = nullptr;

typedef void udp_protocol_handshake_or_intro1Func(long long, unsigned long long, void*, unsigned long long,
                                                  unsigned int, unsigned int);
udp_protocol_handshake_or_intro1Func* udp_protocol_handshake_or_intro1 = nullptr;

typedef void udp_protocol_handshake_or_intro2Func(long long, unsigned long long, unsigned int, unsigned int);
udp_protocol_handshake_or_intro2Func* udp_protocol_handshake_or_intro2 = nullptr;

typedef void udp_protocol_handshake_or_intro3Func(long long, long long, int, int);
udp_protocol_handshake_or_intro3Func* udp_protocol_handshake_or_intro3 = nullptr;

PoolFindItemFunc* PoolFindItem = nullptr;
TcpBroadcasterListenFunc* TcpBroadcasterListen = nullptr;
BroadcasterSendFunc* BroadcasterSend = nullptr;
BroadcasterReceiveLocalEventFunc* BroadcasterReceiveLocalEvent = nullptr;
BroadcasterListenFunc* BroadcasterListen = nullptr;
BroadcasterUnlistenFunc* BroadcasterUnlisten = nullptr;
CJsonGetFloatFunc* CJsonGetFloat = nullptr;
JsonValueAsStringFunc* JsonValueAsString = nullptr;
UriContainerParseFunc* UriContainerParse = nullptr;
BuildCmdLineSyntaxDefinitionsFunc* BuildCmdLineSyntaxDefinitions = nullptr;
AddArgSyntaxFunc* AddArgSyntax = nullptr;
AddArgHelpStringFunc* AddArgHelpString = nullptr;
PreprocessCommandLineFunc* PreprocessCommandLine = nullptr;
WriteLogFunc* WriteLog = nullptr;
HttpConnectFunc* HttpConnect = nullptr;
LoadJsonFromFileFunc* LoadJsonFromFile = nullptr;
LoadLocalConfigFunc* LoadLocalConfig = nullptr;
NetGameSwitchStateFunc* NetGameSwitchState = nullptr;
NetGameScheduleReturnToLobbyFunc* NetGameScheduleReturnToLobby = nullptr;
GetProcAddressFunc* GetProcAddress = nullptr;
SetWindowTextAFunc* SetWindowTextA_ = nullptr;

void InitializeFunctionPointers() {
  ListenProxy = (ListenProxyFunc*)(g_GameBaseAddress + 0x600f90);
  udp_recvfrom_sockaddr_storage = (udp_recvfrom_sockaddr_storageFunc*)(g_GameBaseAddress + 0x1f8d90);
  CleanupPings = (CleanupPingsFunc*)(g_GameBaseAddress + 0x1071000);
  udp_protocol_lookup_or_dispatch = (udp_protocol_lookup_or_dispatchFunc*)(g_GameBaseAddress + 0x4fc170);
  udp_protocol_get_state = (udp_protocol_get_stateFunc*)(g_GameBaseAddress + 0x4fc240);
  udp_protocol_get_peer_id = (udp_protocol_get_peer_idFunc*)(g_GameBaseAddress + 0x4fc300);
  udp_protocol_find_peer = (udp_protocol_find_peerFunc*)(g_GameBaseAddress + 0x5ec330);
  udp_protocol_find_peer_by_addr = (udp_protocol_find_peer_by_addrFunc*)(g_GameBaseAddress + 0x59a4a0);
  udp_protocol_get_context = (udp_protocol_get_contextFunc*)(g_GameBaseAddress + 0x631b40);
  udp_protocol_handshake_or_intro1 = (udp_protocol_handshake_or_intro1Func*)(g_GameBaseAddress + 0x1071b90);
  udp_protocol_handshake_or_intro2 = (udp_protocol_handshake_or_intro2Func*)(g_GameBaseAddress + 0x511020);
  udp_protocol_handshake_or_intro3 = (udp_protocol_handshake_or_intro3Func*)(g_GameBaseAddress + 0x5c7730);
  PoolFindItem = (PoolFindItemFunc*)(g_GameBaseAddress + 0x2CA9E0);
  TcpBroadcasterListen = (TcpBroadcasterListenFunc*)(g_GameBaseAddress + 0xF81100);
  BroadcasterSend = (BroadcasterSendFunc*)(g_GameBaseAddress + 0xF89AF0);
  BroadcasterReceiveLocalEvent = (BroadcasterReceiveLocalEventFunc*)(g_GameBaseAddress + 0xF87AA0);
  BroadcasterListen = (BroadcasterListenFunc*)(g_GameBaseAddress + 0xF80ED0);
  BroadcasterUnlisten = (BroadcasterUnlistenFunc*)(g_GameBaseAddress + 0xF8DF20);
  CJsonGetFloat = (CJsonGetFloatFunc*)(g_GameBaseAddress + 0x5FCA60);
  JsonValueAsString = (JsonValueAsStringFunc*)(g_GameBaseAddress + 0x5FE290);
  UriContainerParse = (UriContainerParseFunc*)(g_GameBaseAddress + 0x621EC0);
  BuildCmdLineSyntaxDefinitions = (BuildCmdLineSyntaxDefinitionsFunc*)(g_GameBaseAddress + 0xFEA00);
  AddArgSyntax = (AddArgSyntaxFunc*)(g_GameBaseAddress + 0xD31B0);
  AddArgHelpString = (AddArgHelpStringFunc*)(g_GameBaseAddress + 0xD30D0);
  PreprocessCommandLine = (PreprocessCommandLineFunc*)(g_GameBaseAddress + 0x116720);
  WriteLog = (WriteLogFunc*)(g_GameBaseAddress + 0xEBE70);
  HttpConnect = (HttpConnectFunc*)(g_GameBaseAddress + 0x1F60C0);
  LoadJsonFromFile = (LoadJsonFromFileFunc*)(g_GameBaseAddress + 0x5F0990);
  LoadLocalConfig = (LoadLocalConfigFunc*)(g_GameBaseAddress + 0x179EB0);
  NetGameSwitchState = (NetGameSwitchStateFunc*)(g_GameBaseAddress + 0x1B8650);
  NetGameScheduleReturnToLobby = (NetGameScheduleReturnToLobbyFunc*)(g_GameBaseAddress + 0x1A89F0);
  GetProcAddress = (GetProcAddressFunc*)(g_GameBaseAddress + 0xEAEF0);
  SetWindowTextA_ = (SetWindowTextAFunc*)(g_GameBaseAddress + 0x5105F0);
}

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
}  // namespace EchoVR
