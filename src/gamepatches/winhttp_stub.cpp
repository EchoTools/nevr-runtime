#include "winhttp_stub.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "common/logging.h"

// IWinHttpRequest IID — {A1C9FEEE-0617-4F23-9D58-8961EA43567C}
static const IID IID_IWinHttpRequest = {0xA1C9FEEE, 0x0617, 0x4F23, {0x9D, 0x58, 0x89, 0x61, 0xEA, 0x43, 0x56, 0x7C}};

// ============================================================================
// Helpers
// ============================================================================

static std::string WideToUtf8(const wchar_t* ws) {
  if (!ws) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string s(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len, nullptr, nullptr);
  return s;
}

static size_t CurlWriteCb(void* p, size_t sz, size_t n, void* ud) {
  size_t total = sz * n;
  auto* v = static_cast<std::vector<char>*>(ud);
  v->insert(v->end(), (char*)p, (char*)p + total);
  return total;
}

static size_t CurlHeaderCb(char* buf, size_t sz, size_t n, void* ud) {
  size_t total = sz * n;
  auto* hdrs = static_cast<std::map<std::wstring, std::wstring>*>(ud);
  std::string line(buf, total);
  size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();
    int kl = MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, nullptr, 0);
    int vl = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, nullptr, 0);
    if (kl > 0 && vl > 0) {
      std::wstring wk(kl - 1, L'\0');
      std::wstring wv(vl - 1, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, &wk[0], kl);
      MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, &wv[0], vl);
      (*hdrs)[wk] = wv;
    }
  }
  return total;
}

// Cast the COM `this` pointer (vtable pointer) back to our object.
// COM passes the interface pointer as `this`, which points to `vtbl`.
// Since `vtbl` is the first member of WinHttpRequestStub, we can cast directly.
#define SELF(thisPtr) reinterpret_cast<WinHttpRequestStub*>(thisPtr)

// ============================================================================
// IUnknown — vtable slots [0..2]
// ============================================================================

static HRESULT STDMETHODCALLTYPE Stub_QueryInterface(void* pThis, REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch) || IsEqualIID(riid, IID_IWinHttpRequest)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] QI accepted — returning stub (riid match)");
    *ppv = pThis;
    SELF(pThis)->m_refCount++;
    return S_OK;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] QI rejected {%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
      riid.Data1, riid.Data2, riid.Data3,
      riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
      riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
  *ppv = nullptr;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Stub_AddRef(void* pThis) {
  auto* self = SELF(pThis);
  ULONG n = ++self->m_refCount;
  Log(EchoVR::LogLevel::Debug, "[NEVR.HTTP] AddRef -> %lu", n);
  return n;
}

static ULONG STDMETHODCALLTYPE Stub_Release(void* pThis) {
  auto* self = SELF(pThis);
  ULONG n = --self->m_refCount;
  Log(EchoVR::LogLevel::Debug, "[NEVR.HTTP] Release -> %lu", n);
  if (n == 0) {
    self->~WinHttpRequestStub();
    free(self);
  }
  return n;
}

// ============================================================================
// IDispatch — vtable slots [3..6]
// ============================================================================

// Forward declarations — these are defined below in the IWinHttpRequest section.
static HRESULT STDMETHODCALLTYPE Stub_Open(void* pThis, BSTR Method, BSTR Url, VARIANT);
static HRESULT STDMETHODCALLTYPE Stub_SetRequestHeader(void* pThis, BSTR Header, BSTR Value);
static HRESULT STDMETHODCALLTYPE Stub_Send(void* pThis, VARIANT);
static HRESULT STDMETHODCALLTYPE Stub_get_Status(void* pThis, long* Status);
static HRESULT STDMETHODCALLTYPE Stub_get_ResponseText(void* pThis, BSTR* Body);
static HRESULT STDMETHODCALLTYPE Stub_get_ResponseBody(void* pThis, VARIANT* Body);

static HRESULT STDMETHODCALLTYPE Stub_GetTypeInfoCount(void*, UINT* p) {
  if (p) *p = 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE Stub_GetTypeInfo(void*, UINT, LCID, ITypeInfo** p) {
  if (p) *p = nullptr;
  return S_OK;
}

// DISPID-to-name lookup tables for IWinHttpRequest.
// DISPIDs are from the WinHTTP type library (winhttp.dll).
static constexpr LONG kDispId_Open = 1;
static constexpr LONG kDispId_SetProxy = 2;
static constexpr LONG kDispId_SetCredentials = 3;
static constexpr LONG kDispId_SetRequestHeader = 4;
static constexpr LONG kDispId_Send = 5;
static constexpr LONG kDispId_WaitForResponse = 6;
static constexpr LONG kDispId_Abort = 7;
static constexpr LONG kDispId_Status = 8;
static constexpr LONG kDispId_StatusText = 9;
static constexpr LONG kDispId_ResponseText = 10;
static constexpr LONG kDispId_ResponseBody = 11;
static constexpr LONG kDispId_ResponseStream = 12;
static constexpr LONG kDispId_AllResponseHeaders = 13;
static constexpr LONG kDispId_ResponseHeader = 14;
static constexpr LONG kDispId_Option = 15;
static constexpr LONG kDispId_SetTimeouts = 17;
static constexpr LONG kDispId_SetClientCertificate = 18;
static constexpr LONG kDispId_SetAutoLogonPolicy = 19;

struct DispIdEntry {
  const wchar_t* name;
  LONG dispId;
};

static const DispIdEntry g_DispIdTable[] = {
    {L"Open", kDispId_Open},
    {L"SetProxy", kDispId_SetProxy},
    {L"SetCredentials", kDispId_SetCredentials},
    {L"SetRequestHeader", kDispId_SetRequestHeader},
    {L"Send", kDispId_Send},
    {L"WaitForResponse", kDispId_WaitForResponse},
    {L"Abort", kDispId_Abort},
    {L"Status", kDispId_Status},
    {L"StatusText", kDispId_StatusText},
    {L"ResponseText", kDispId_ResponseText},
    {L"ResponseBody", kDispId_ResponseBody},
    {L"ResponseStream", kDispId_ResponseStream},
    {L"AllResponseHeaders", kDispId_AllResponseHeaders},
    {L"ResponseHeader", kDispId_ResponseHeader},
    {L"Option", kDispId_Option},
    {L"SetTimeouts", kDispId_SetTimeouts},
    {L"SetClientCertificate", kDispId_SetClientCertificate},
    {L"SetAutoLogonPolicy", kDispId_SetAutoLogonPolicy},
};
static constexpr size_t kDispIdTableSize = sizeof(g_DispIdTable) / sizeof(g_DispIdTable[0]);

static HRESULT STDMETHODCALLTYPE Stub_GetIDsOfNames(void*, REFIID, LPOLESTR* rgszNames, UINT cNames, LCID,
                                                    DISPID* rgDispId) {
  if (!rgszNames || !rgDispId) return E_POINTER;
  for (UINT i = 0; i < cNames; i++) {
    rgDispId[i] = DISPID_UNKNOWN;
    if (rgszNames[i]) {
      for (size_t j = 0; j < kDispIdTableSize; j++) {
        if (wcscmp(rgszNames[i], g_DispIdTable[j].name) == 0) {
          rgDispId[i] = g_DispIdTable[j].dispId;
          break;
        }
      }
    }
  }
  // Return S_OK even if some names were not found — COM convention:
  // caller checks for DISPID_UNKNOWN on each entry.
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_Invoke(void* pThis, DISPID dispIdMember, REFIID, LCID, WORD wFlags,
                                              DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO*,
                                              UINT*) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] Invoke DISPID=%ld flags=0x%04x cArgs=%u",
      static_cast<long>(dispIdMember), wFlags,
      pDispParams ? pDispParams->cArgs : 0);

  auto* self = SELF(pThis);

  // Helper: extract BSTR from reversed DISPPARAMS args (index 0 = rightmost arg).
  auto ArgBstr = [&](UINT idx) -> BSTR {
    if (!pDispParams || idx >= pDispParams->cArgs) return nullptr;
    VARIANTARG& va = pDispParams->rgvarg[pDispParams->cArgs - 1 - idx];
    return (va.vt == VT_BSTR) ? va.bstrVal : nullptr;
  };

  if (wFlags & DISPATCH_PROPERTYGET) {
    switch (dispIdMember) {
      case kDispId_Status: {
        long status = 0;
        Stub_get_Status(pThis, &status);
        if (status == 0) status = 200;  // default: pretend success
        if (pVarResult) { pVarResult->vt = VT_I4; pVarResult->lVal = status; }
        return S_OK;
      }
      case kDispId_StatusText:
        if (pVarResult) { pVarResult->vt = VT_BSTR; pVarResult->bstrVal = SysAllocString(L"OK"); }
        return S_OK;
      case kDispId_ResponseText: {
        BSTR body = nullptr;
        Stub_get_ResponseText(pThis, &body);
        if (pVarResult) {
          if (body) {
            pVarResult->vt = VT_BSTR; pVarResult->bstrVal = body;
          } else {
            pVarResult->vt = VT_BSTR; pVarResult->bstrVal = SysAllocString(L"");
          }
        }
        return S_OK;
      }
      case kDispId_ResponseBody: {
        VARIANT body; VariantInit(&body);
        Stub_get_ResponseBody(pThis, &body);
        if (pVarResult) { *pVarResult = body; }
        return S_OK;
      }
      case kDispId_AllResponseHeaders:
        if (pVarResult) { pVarResult->vt = VT_BSTR; pVarResult->bstrVal = SysAllocString(L""); }
        return S_OK;
      case kDispId_ResponseHeader:
        // ResponseHeader(index) → BSTR value
        if (pVarResult) { pVarResult->vt = VT_BSTR; pVarResult->bstrVal = SysAllocString(L""); }
        return S_OK;
      case kDispId_Option:
        if (pVarResult) { pVarResult->vt = VT_EMPTY; }
        return S_OK;
      case kDispId_ResponseStream:
        if (pVarResult) { pVarResult->vt = VT_EMPTY; }
        return S_OK;
      default:
        // Unknown property get
        if (pVarResult) { pVarResult->vt = VT_EMPTY; }
        return S_OK;
    }
  }

  if (wFlags & DISPATCH_PROPERTYPUT) {
    switch (dispIdMember) {
      case kDispId_Option:
        return S_OK;
      default:
        return S_OK;
    }
  }

  // DISPATCH_METHOD
  switch (dispIdMember) {
    case kDispId_Open: {
      // Open(Method, Url [, Async])
      BSTR method = ArgBstr(0);
      BSTR url = ArgBstr(1);
      VARIANT async; VariantInit(&async);
      if (pDispParams && pDispParams->cArgs >= 3)
        async = pDispParams->rgvarg[0];  // rightmost = Async
      return Stub_Open(pThis, method, url, async);
    }
    break;
    case kDispId_SetRequestHeader: {
      // SetRequestHeader(Header, Value)
      BSTR header = ArgBstr(0);
      BSTR value = ArgBstr(1);
      return Stub_SetRequestHeader(pThis, header, value);
    }
    break;
    case kDispId_Send: {
      // Send([Body]) — no-op: succeed without real HTTP.
      // The game calls this for Oculus telemetry/health checks which no longer
      // exist. The ws_bridge handles actual service traffic independently.
      Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] Send DISPID=5 (no-op) url=%ls",
          self->m_url.empty() ? L"(none)" : self->m_url.c_str());
      self->m_sent = true;
      self->m_statusCode = 200;
      return S_OK;
    }
    break;
    case kDispId_SetProxy:
      return S_OK;
    case kDispId_SetCredentials:
      return S_OK;
    case kDispId_WaitForResponse:
      if (pVarResult) { pVarResult->vt = VT_BOOL; pVarResult->boolVal = VARIANT_TRUE; }
      return S_OK;
    case kDispId_Abort:
      return S_OK;
    case kDispId_SetTimeouts:
      return S_OK;
    case kDispId_SetClientCertificate:
      return S_OK;
    case kDispId_SetAutoLogonPolicy:
      return S_OK;
    case kDispId_ResponseHeader: {
      // ResponseHeader(index) — dispatch as getter
      if (pVarResult) { pVarResult->vt = VT_BSTR; pVarResult->bstrVal = SysAllocString(L""); }
      return S_OK;
    }
    default:
      Log(EchoVR::LogLevel::Debug, "[NEVR.HTTP] Invoke unhandled DISPID=%ld returning S_OK",
          static_cast<long>(dispIdMember));
      return S_OK;
  }
}

// ============================================================================
// IWinHttpRequest — vtable slots [7..25]
// ============================================================================

static HRESULT STDMETHODCALLTYPE Stub_SetProxy(void*, long, VARIANT, VARIANT) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] SetProxy called");
  return S_OK;
}

// SetCredentials — present in Windows IWinHttpRequest but absent from the Xbox One
// interface the game actually uses. Kept for reference; not in the vtable.
__attribute__((unused))
static HRESULT STDMETHODCALLTYPE Stub_SetCredentials(void*, BSTR, BSTR, long) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] SetCredentials called");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_Open(void* pThis, BSTR Method, BSTR Url, VARIANT) {
  auto* self = SELF(pThis);
  if (Method) self->m_method = Method;
  if (Url) self->m_url = Url;
  self->m_sent = false;
  self->m_responseBody.clear();
  self->m_responseHeaders.clear();
  self->m_statusCode = 0;
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] Open %ls %ls", Method ? Method : L"(null)", Url ? Url : L"(null)");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_SetRequestHeader(void* pThis, BSTR Header, BSTR Value) {
  if (Header && Value) SELF(pThis)->m_requestHeaders[Header] = Value;
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] SetRequestHeader %ls: %ls", Header ? Header : L"(null)", Value ? Value : L"(null)");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_GetResponseHeader(void* pThis, BSTR Header, BSTR* Value) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] GetResponseHeader called");
  if (!Value) return E_POINTER;
  *Value = nullptr;
  auto* self = SELF(pThis);
  if (!self->m_sent || !Header) return S_OK;
  auto it = self->m_responseHeaders.find(Header);
  if (it != self->m_responseHeaders.end()) *Value = SysAllocString(it->second.c_str());
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_GetAllResponseHeaders(void* pThis, BSTR* Headers) {
  if (!Headers) return E_POINTER;
  *Headers = nullptr;
  auto* self = SELF(pThis);
  if (!self->m_sent) return S_OK;
  std::wstring all;
  for (const auto& [k, v] : self->m_responseHeaders) all += k + L": " + v + L"\r\n";
  if (!all.empty()) *Headers = SysAllocString(all.c_str());
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_Send(void* pThis, VARIANT) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] Send (vtbl, url=%ls)", SELF(pThis)->m_url.empty() ? L"(none)" : SELF(pThis)->m_url.c_str());
  auto* self = SELF(pThis);
  CURL* curl = curl_easy_init();
  if (!curl) return E_FAIL;

  std::string url = WideToUtf8(self->m_url.c_str());
  std::string method = WideToUtf8(self->m_method.c_str());

  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] Send %s %s", method.c_str(), url.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (_stricmp(method.c_str(), "POST") == 0)
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  else if (_stricmp(method.c_str(), "GET") != 0)
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());

  struct curl_slist* hlist = nullptr;
  std::vector<std::string> hstrs;
  for (const auto& [k, v] : self->m_requestHeaders) {
    hstrs.push_back(WideToUtf8(k.c_str()) + ": " + WideToUtf8(v.c_str()));
    hlist = curl_slist_append(hlist, hstrs.back().c_str());
  }
  if (hlist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

  self->m_responseBody.clear();
  self->m_responseHeaders.clear();
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &self->m_responseBody);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &self->m_responseHeaders);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(hlist);

  if (res != CURLE_OK) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.HTTP] curl failed: %s", curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    return E_FAIL;
  }

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  self->m_statusCode = code;
  curl_easy_cleanup(curl);
  self->m_sent = true;

  Log(EchoVR::LogLevel::Debug, "[NEVR.HTTP] Response: %ld (%zu bytes)", self->m_statusCode, self->m_responseBody.size());
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_Status(void* pThis, long* Status) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] get_Status called (status=%ld)", SELF(pThis)->m_statusCode);
  if (Status) *Status = SELF(pThis)->m_statusCode;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_StatusText(void*, BSTR* S) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] get_StatusText called");
  if (S) *S = SysAllocString(L"OK");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_ResponseText(void* pThis, BSTR* Body) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] get_ResponseText called");
  if (!Body) return E_POINTER;
  *Body = nullptr;
  auto* self = SELF(pThis);
  if (!self->m_sent || self->m_responseBody.empty()) return S_OK;
  int wl = MultiByteToWideChar(CP_UTF8, 0, self->m_responseBody.data(), (int)self->m_responseBody.size(), nullptr, 0);
  if (wl > 0) {
    *Body = SysAllocStringLen(nullptr, wl);
    if (*Body) MultiByteToWideChar(CP_UTF8, 0, self->m_responseBody.data(), (int)self->m_responseBody.size(), *Body, wl);
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_ResponseBody(void* pThis, VARIANT* Body) {
  if (!Body) return E_POINTER;
  VariantInit(Body);
  auto* self = SELF(pThis);
  if (!self->m_sent || self->m_responseBody.empty()) return S_OK;
  SAFEARRAY* psa = SafeArrayCreateVector(VT_UI1, 0, (ULONG)self->m_responseBody.size());
  if (!psa) return E_OUTOFMEMORY;
  void* pData = nullptr;
  if (SUCCEEDED(SafeArrayAccessData(psa, &pData))) {
    memcpy(pData, self->m_responseBody.data(), self->m_responseBody.size());
    SafeArrayUnaccessData(psa);
    Body->vt = VT_ARRAY | VT_UI1;
    Body->parray = psa;
  } else {
    SafeArrayDestroy(psa);
    return E_FAIL;
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_ResponseStream(void*, VARIANT* V) {
  if (V) VariantInit(V);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE Stub_get_Option(void*, long, VARIANT* V) {
  if (V) VariantInit(V);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE Stub_put_Option(void*, long, VARIANT) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_WaitForResponse(void*, VARIANT, VARIANT_BOOL* S) {
  Log(EchoVR::LogLevel::Info, "[NEVR.HTTP] WaitForResponse called");
  if (S) *S = VARIANT_TRUE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE Stub_Abort(void*) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_SetTimeouts(void*, long, long, long, long) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_SetClientCertificate(void*, BSTR) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_SetAutoLogonPolicy(void*, long) { return S_OK; }

// ============================================================================
// Static vtable — 25 slots for Xbox One IWinHttpRequest (no SetCredentials)
// ============================================================================

static void* s_vtbl[25] = {
    (void*)Stub_QueryInterface,       // [0]  IUnknown
    (void*)Stub_AddRef,               // [1]
    (void*)Stub_Release,              // [2]
    (void*)Stub_GetTypeInfoCount,     // [3]  IDispatch
    (void*)Stub_GetTypeInfo,          // [4]
    (void*)Stub_GetIDsOfNames,        // [5]
    (void*)Stub_Invoke,               // [6]
    (void*)Stub_SetProxy,             // [7]  IWinHttpRequest (XB1: no SetCredentials)
    (void*)Stub_Open,                 // [8]
    (void*)Stub_SetRequestHeader,     // [9]
    (void*)Stub_GetResponseHeader,    // [10]
    (void*)Stub_GetAllResponseHeaders,// [11]
    (void*)Stub_Send,                 // [12]
    (void*)Stub_get_Status,           // [13]
    (void*)Stub_get_StatusText,       // [14]
    (void*)Stub_get_ResponseText,     // [15]
    (void*)Stub_get_ResponseBody,     // [16]
    (void*)Stub_get_ResponseStream,   // [17]
    (void*)Stub_get_Option,           // [18]
    (void*)Stub_put_Option,           // [19]
    (void*)Stub_WaitForResponse,      // [20]
    (void*)Stub_Abort,                // [21]
    (void*)Stub_SetTimeouts,          // [22]
    (void*)Stub_SetClientCertificate, // [23]
    (void*)Stub_SetAutoLogonPolicy,   // [24]
};

// ============================================================================
// Construction / Factory
// ============================================================================

WinHttpRequestStub::WinHttpRequestStub() : vtbl(s_vtbl), m_refCount(1), m_statusCode(0), m_sent(false) {}

HRESULT WINAPI CreateWinHttpRequestStub(REFIID riid, void** ppvObject) {
  if (!ppvObject) return E_POINTER;

  // Use malloc + placement new so Release() can free() without calling
  // a virtual destructor (there isn't one).
  void* mem = malloc(sizeof(WinHttpRequestStub));
  if (!mem) return E_OUTOFMEMORY;
  auto* stub = new (mem) WinHttpRequestStub();

  // QI for the requested interface (adds a ref), then release the construction ref.
  HRESULT hr = Stub_QueryInterface(stub, riid, ppvObject);
  Stub_Release(stub);  // balance construction refcount
  return hr;
}
