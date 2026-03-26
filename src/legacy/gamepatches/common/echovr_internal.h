#pragma once
#ifndef ECHOVR_INTERNAL_H
#define ECHOVR_INTERNAL_H

#include "echovr.h"
#include "pch.h"

namespace EchoVR {

// Game base address handle
extern CHAR* g_GameBaseAddress;

// Pool item accessor
typedef BYTE* PoolFindItemFunc(PVOID pool, UINT64 index);
extern PoolFindItemFunc* PoolFindItem;

// TCP broadcaster message subscription. Returns handle for unregistration.
typedef UINT16 TcpBroadcasterListenFunc(EchoVR::TcpBroadcaster* broadcaster, EchoVR::SymbolId messageId, INT64 unk1,
                                        INT64 unk2, INT64 unk3, VOID* delegateProxy, BOOL prepend);
extern TcpBroadcasterListenFunc* TcpBroadcasterListen;

extern UINT64 TcpBroadcasterUnlisten(EchoVR::TcpBroadcaster* broadcaster, UINT16 cbResult);

// Send message via UDP broadcaster
typedef INT32 BroadcasterSendFunc(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId,
                                  INT32 mbThreadPriority, VOID* item, UINT64 size, VOID* buffer, UINT64 bufferLen,
                                  EchoVR::Peer peer, UINT64 dest, FLOAT priority, EchoVR::SymbolId unk);
extern BroadcasterSendFunc* BroadcasterSend;

// Trigger local event on broadcaster
typedef UINT64 BroadcasterReceiveLocalEventFunc(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId,
                                                const CHAR* msgName, VOID* msg, UINT64 msgSize);
extern BroadcasterReceiveLocalEventFunc* BroadcasterReceiveLocalEvent;

// Subscribe to broadcaster message. Returns handle for unregistration.
typedef UINT16 BroadcasterListenFunc(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId,
                                     BOOL isReliableMsgType, VOID* px, BOOL prepend);
extern BroadcasterListenFunc* BroadcasterListen;

// Unsubscribe from broadcaster message
typedef UINT64 BroadcasterUnlistenFunc(EchoVR::Broadcaster* broadcaster, UINT16 cbResult);
extern BroadcasterUnlistenFunc* BroadcasterUnlisten;

// JSON string value getter with fallback
typedef CHAR* JsonValueAsStringFunc(EchoVR::Json* root, CHAR* keyName, CHAR* defaultValue, BOOL reportFailure);
extern JsonValueAsStringFunc* JsonValueAsString;

// JSON float value getter (CJson::Float thunk)
typedef FLOAT CJsonGetFloatFunc(PVOID root, const CHAR* path, FLOAT defaultValue, INT32 required);
extern CJsonGetFloatFunc* CJsonGetFloat;

// URI string parser
typedef HRESULT UriContainerParseFunc(EchoVR::UriContainer* uriContainer, CHAR* uri);
extern UriContainerParseFunc* UriContainerParse;

// CLI argument handling
typedef UINT64 BuildCmdLineSyntaxDefinitionsFunc(PVOID pGame, PVOID pArgSyntax);
extern BuildCmdLineSyntaxDefinitionsFunc* BuildCmdLineSyntaxDefinitions;

typedef VOID AddArgSyntaxFunc(PVOID pArgSyntax, const CHAR* sArgName, UINT64 minOptions, UINT64 maxOptions,
                              BOOL validate);
extern AddArgSyntaxFunc* AddArgSyntax;

typedef VOID AddArgHelpStringFunc(PVOID pArgSyntax, const CHAR* sArgName, const CHAR* sArgHelpDescription);
extern AddArgHelpStringFunc* AddArgHelpString;

typedef UINT64 PreprocessCommandLineFunc(PVOID pGame);
extern PreprocessCommandLineFunc* PreprocessCommandLine;

// Logging
typedef VOID WriteLogFunc(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl);
extern WriteLogFunc* WriteLog;

// HTTP connection
typedef UINT64 HttpConnectFunc(VOID* unk, CHAR* uri);
extern HttpConnectFunc* HttpConnect;

// Config loading
typedef UINT64 LoadLocalConfigFunc(PVOID pGame);
extern LoadLocalConfigFunc* LoadLocalConfig;

// Net game state transitions
typedef VOID NetGameSwitchStateFunc(PVOID pGame, NetGameState state);
extern NetGameSwitchStateFunc* NetGameSwitchState;

typedef VOID NetGameScheduleReturnToLobbyFunc(PVOID pGame);
extern NetGameScheduleReturnToLobbyFunc* NetGameScheduleReturnToLobby;

// Win32 API wrappers
typedef FARPROC GetProcAddressFunc(HMODULE hModule, LPCSTR lpProcName);
extern GetProcAddressFunc* GetProcAddress;

typedef UINT64 SetWindowTextAFunc(HWND hWnd, LPCSTR lpString);
extern SetWindowTextAFunc* SetWindowTextA_;

// UDP protocol functions
typedef void ListenProxyFunc(void);
extern ListenProxyFunc* ListenProxy;

typedef void udp_recvfrom_sockaddr_storageFunc(void);
extern udp_recvfrom_sockaddr_storageFunc* udp_recvfrom_sockaddr_storage;

typedef void CleanupPingsFunc(long long* in_RCX, long long* in_R8);
extern CleanupPingsFunc* CleanupPings;

typedef unsigned int udp_protocol_lookup_or_dispatchFunc(long long, unsigned long long, int);
extern udp_protocol_lookup_or_dispatchFunc* udp_protocol_lookup_or_dispatch;

typedef unsigned long long* udp_protocol_get_stateFunc(long long, unsigned long long*, unsigned long long, int);
extern udp_protocol_get_stateFunc* udp_protocol_get_state;

typedef unsigned int udp_protocol_get_peer_idFunc(long long, unsigned long long, int);
extern udp_protocol_get_peer_idFunc* udp_protocol_get_peer_id;

typedef long long* udp_protocol_find_peerFunc(long long, long long);
extern udp_protocol_find_peerFunc* udp_protocol_find_peer;

typedef long long udp_protocol_find_peer_by_addrFunc(long long, long long);
extern udp_protocol_find_peer_by_addrFunc* udp_protocol_find_peer_by_addr;

typedef long long udp_protocol_get_contextFunc(long long);
extern udp_protocol_get_contextFunc* udp_protocol_get_context;

typedef void udp_protocol_handshake_or_intro1Func(long long, unsigned long long, void*, unsigned long long,
                                                  unsigned int, unsigned int);
extern udp_protocol_handshake_or_intro1Func* udp_protocol_handshake_or_intro1;

typedef void udp_protocol_handshake_or_intro2Func(long long, unsigned long long, unsigned int, unsigned int);
extern udp_protocol_handshake_or_intro2Func* udp_protocol_handshake_or_intro2;

typedef void udp_protocol_handshake_or_intro3Func(long long, long long, int, int);
extern udp_protocol_handshake_or_intro3Func* udp_protocol_handshake_or_intro3;

}  // namespace EchoVR

#endif  // ECHOVR_INTERNAL_H