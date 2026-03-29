#pragma once
#include <oaidl.h>
#include <unknwn.h>
#include <windows.h>

#include <map>
#include <string>
#include <vector>

// WinHTTP enum types (not in MinGW headers)
typedef long HTTPREQUEST_PROXY_SETTING;
typedef long HTTPREQUEST_SETCREDENTIALS_FLAGS;
typedef long WinHttpRequestOption;
typedef long WinHttpRequestAutoLogonPolicy;

/// IWinHttpRequest COM implementation backed by libcurl.
///
/// Uses a hand-rolled vtable to guarantee COM ABI layout. MinGW's C++ vtable
/// includes virtual destructors and RTTI entries that shift method offsets,
/// breaking the COM interface the game expects. The manual vtable has exactly
/// 26 function pointers in the correct order:
///   [0-2]   IUnknown:        QueryInterface, AddRef, Release
///   [3-6]   IDispatch:       GetTypeInfoCount, GetTypeInfo, GetIDsOfNames, Invoke
///   [7-25]  IWinHttpRequest: SetProxy .. SetAutoLogonPolicy
class WinHttpRequestStub {
 public:
  WinHttpRequestStub();

  // The first member MUST be the vtable pointer — this is what COM callers see
  // when they cast the object pointer to an interface pointer.
  void** vtbl;

  ULONG m_refCount;
  std::wstring m_method;
  std::wstring m_url;
  std::map<std::wstring, std::wstring> m_requestHeaders;
  std::vector<char> m_responseBody;
  std::map<std::wstring, std::wstring> m_responseHeaders;
  long m_statusCode;
  bool m_sent;
};

HRESULT WINAPI CreateWinHttpRequestStub(REFIID riid, void** ppvObject);
